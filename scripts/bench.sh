#!/bin/bash
# scripts/bench.sh -- load & performance suite (DESIGN.md 11.3).
# Compares direct-to-origin vs via-proxy on the same box (proxy tax),
# separates request-bound (small, state-machine) from bandwidth-bound
# (large, memcpy/splice) workloads, and reports p50/p90/p99 + errors.
#
# Uses wrk when available, otherwise a python3 concurrent fallback.
# Usage: bash scripts/bench.sh [--quick] [--splice]
#   --quick   short runs (2s) for CI; default 10s per phase
#   --splice  also run proxy with splice_threshold=65536 (M7 compare)
set -u
BUILD="${BUILD:-build}"
PXLB="$BUILD/pxlb"
ORIGIN="$BUILD/origin"
O1_PORT=19001
O2_PORT=19002
PROXY_PORT=18080
CFG=/tmp/pxlb_bench.conf
DUR=10
if [ "${1:-}" = "--quick" ]; then DUR=2; shift; fi
WANT_SPLICE=0
if [ "${1:-}" = "--splice" ]; then WANT_SPLICE=1; shift; fi

cleanup() {
  for p in ${O1_PID:-} ${O2_PID:-} ${PX_PID:-}; do
    if kill -0 "$p" 2>/dev/null; then kill "$p" 2>/dev/null; fi
  done
  wait 2>/dev/null || true
  rm -f "$CFG"
}
trap cleanup EXIT INT TERM

make "$PXLB" "$ORIGIN" >/dev/null 2>&1 || { echo "build failed"; exit 1; }

echo "=== pxlb bench (methodology: DESIGN 11.3) ==="
echo "1. Always compare proxy vs direct-to-origin on the same box; the gap is your proxy tax."
echo "2. wrk keeps conns alive -- set 'ulimit -n 65535' or you measure fd exhaustion."
ulimit -n 65535 2>/dev/null || true
echo "   ulimit -n: $(ulimit -n)"
echo "3. TIME_WAIT churn: proxy closes backend conns per request; tune tcp_tw_reuse for bench;"
echo "   upstream keep-alive (DESIGN 12) is the real fix."
echo "4. Separate request-bound (small, RPS) from bandwidth-bound (large, memcpy/splice)."
echo "5. Record CPU% alongside throughput."
echo ""

"$ORIGIN" "$O1_PORT" 2>/tmp/bench_o1.log &
O1_PID=$!
"$ORIGIN" "$O2_PORT" 2>/tmp/bench_o2.log &
O2_PID=$!
sleep 0.5

start_proxy() { # threshold
  cat > "$CFG" <<EOF
listen          127.0.0.1:$PROXY_PORT
workers         1
backlog         128
max_conns       4096
max_head        65536
buf_cap         65536
connect_timeout 2000
idle_timeout    30000
balance         round-robin
health_enabled  0
splice_threshold $1
backend         127.0.0.1:$O1_PORT weight=1
backend         127.0.0.1:$O2_PORT weight=1
EOF
  "$PXLB" -c "$CFG" 2>/tmp/bench_px.log &
  PX_PID=$!
  sleep 0.8
}
stop_proxy() { kill "$PX_PID" 2>/dev/null; wait "$PX_PID" 2>/dev/null || true; PX_PID=""; }

# python fallback loader: N threads x M reqs.
# For large bodies use a fresh conn per request (Connection: close) to avoid
# exercising the known v1 sequential-large keep-alive gap; small/upload reuse
# keep-alive conns to exercise the state machine.
py_load() { # url_path method body_len concurrency total_reqs [reuse]
  python3 - "$1" "$2" "$3" "$4" "$5" "${6:-1}" <<'PY'
import concurrent.futures, http.client, os, sys, time
path, method, blen, conc, total, reuse = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
host, port = "127.0.0.1", 18080
if path.startswith("DIRECT:"):
    port = 19001
    path = path[len("DIRECT:"):]
body = os.urandom(blen) if blen > 0 and method == "POST" else None
per = total // conc
lat = []
errs = 0
def worker(_):
    global errs
    ls = []
    try:
        c = None
        for _ in range(per):
            # large bodies: fresh conn + close (bandwidth-bound, no keep-alive)
            use_reuse = reuse and ("big" not in path)
            if c is None:
                c = http.client.HTTPConnection(host, port, timeout=15)
            hdrs = {"Host": "x"}
            if not use_reuse:
                hdrs["Connection"] = "close"
            t0 = time.monotonic()
            try:
                if method == "POST":
                    hdrs["Content-Length"] = str(blen)
                    hdrs["Content-Type"] = "application/octet-stream"
                    c.request("POST", path, body=body, headers=hdrs)
                else:
                    c.request("GET", path, headers=hdrs)
                r = c.getresponse()
                r.read()
                if r.status != 200:
                    errs += 1
                ls.append((time.monotonic() - t0) * 1000)
            except Exception:
                errs += 1
                try: c.close()
                except Exception: pass
                c = http.client.HTTPConnection(host, port, timeout=15)
                continue
            if not use_reuse:
                try: c.close()
                except Exception: pass
                c = None
        if c is not None:
            try: c.close()
            except Exception: pass
    except Exception:
        pass
    return ls
t0 = time.monotonic()
with concurrent.futures.ThreadPoolExecutor(max_workers=conc) as ex:
    for ls in ex.map(worker, range(conc)):
        lat.extend(ls)
dt = time.monotonic() - t0
lat.sort()
def pct(p):
    return lat[int(len(lat) * p / 100)] if lat else 0
print(f"  reqs={len(lat)} errs={errs} time={dt:.1f}s rps={len(lat)/dt:.0f} "
      f"p50={pct(50):.1f}ms p90={pct(90):.1f}ms p99={pct(99):.1f}ms")
PY
}

wrk_phase() { # name url wrk_args
  echo "--- $1 ---"
  echo "wrk $3 $2"
  if command -v wrk >/dev/null 2>&1; then
    wrk -t4 "$3" --latency "$2" 2>&1 | tail -n 12
  else
    echo "(wrk not found, python fallback, DUR=${DUR}s)"
    # Scale load by DUR so --quick finishes fast.
    if [ "$DUR" -le 3 ]; then
      case "$1" in
        *large*) py_load "/big?n=262144" GET 0 4 16 ;;
        *small*|*echo*) py_load "/echo" GET 0 8 200 ;;
        *upload*) py_load "/post" POST 16384 4 32 ;;
        *) py_load "/echo" GET 0 4 40 ;;
      esac
    else
      case "$1" in
        *large*) py_load "/big?n=1048576" GET 0 16 64 ;;
        *small*|*echo*) py_load "/echo" GET 0 32 3200 ;;
        *upload*) py_load "/post" POST 65536 16 320 ;;
        *) py_load "/echo" GET 0 16 800 ;;
      esac
    fi
  fi
  echo ""
}

if command -v wrk >/dev/null 2>&1; then
  echo "wrk: $(wrk --version 2>&1 | head -n1)"
else
  echo "wrk not found -- using python fallback loader"
fi
echo ""

# Baseline: direct-to-origin headroom (no proxy)
echo "### baseline: origin directly (headroom reference) ###"
if command -v wrk >/dev/null 2>&1; then
  wrk -t4 -c200 -d${DUR}s --latency "http://127.0.0.1:$O1_PORT/big?n=1048576" 2>&1 | tail -n 8
else
  if [ "$DUR" -le 3 ]; then BIGN=262144; BTOT=16; else BIGN=1048576; BTOT=64; fi
  python3 - "DIRECT:/big?n=$BIGN" GET 0 4 "$BTOT" <<'PY'
import concurrent.futures, http.client, sys, time
# args: path is DIRECT:... but we parse manually for quick
import sys
path_arg = sys.argv[1]
if path_arg.startswith("DIRECT:"): path_arg = path_arg[len("DIRECT:"):]
blen, conc, total = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
lat = []
def w(_):
    ls = []
    c = http.client.HTTPConnection("127.0.0.1", 19001, timeout=20)
    for _ in range(total // conc):
        t0 = time.monotonic()
        c.request("GET", path_arg, headers={"Host": "x"})
        r = c.getresponse(); r.read()
        ls.append((time.monotonic()-t0)*1000)
    c.close(); return ls
t0=time.monotonic()
with concurrent.futures.ThreadPoolExecutor(max_workers=conc) as ex:
    for ls in ex.map(w, range(conc)): lat.extend(ls)
dt=time.monotonic()-t0; lat.sort()
print(f"  direct large: reqs={len(lat)} rps={len(lat)/dt:.0f} p50={lat[int(len(lat)*0.5)]:.1f}ms p99={lat[int(len(lat)*0.99)]:.1f}ms")
PY
fi
echo ""

for THR in 0 65536; do
  if [ "$THR" = "65536" ] && [ "$WANT_SPLICE" = "0" ]; then continue; fi
  if [ "$THR" = "0" ]; then echo "### through proxy, mbuf path (splice_threshold=0) ###"
  else echo "### through proxy, splice path (splice_threshold=65536, Linux only; macOS falls back to mbuf) ###"; fi
  start_proxy "$THR"
  # bandwidth-bound large
  wrk_phase "bandwidth-bound large (1 MiB, exercises memcpy/splice)" "http://127.0.0.1:$PROXY_PORT/big?n=1048576" "-c200 -d${DUR}s"
  # request-bound small keep-alive
  wrk_phase "request-bound small keep-alive (exercises state machine)" "http://127.0.0.1:$PROXY_PORT/echo" "-c400 -d${DUR}s"
  # upload-heavy
  if command -v wrk >/dev/null 2>&1; then
    echo "--- upload-heavy (64 KiB POST) ---"
    wrk -t4 -c200 -d${DUR}s --latency -s scripts/post.lua "http://127.0.0.1:$PROXY_PORT/post" 2>&1 | tail -n 8
    echo ""
  else
    wrk_phase "upload-heavy (64 KiB POST)" "http://127.0.0.1:$PROXY_PORT/post" "-c200 -d${DUR}s"
  fi
  # CPU snapshot
  if command -v ps >/dev/null 2>&1; then
    echo "proxy CPU%: $(ps -o %cpu= -p $PX_PID 2>/dev/null | tr -d ' ')%"
  fi
  # pipe starves (splice pool pressure)
  curl -s "http://127.0.0.1:$PROXY_PORT/_proxima/metrics" -H "Host: x" | grep -E "pipe_pool_starves|requests_total" || true
  echo ""
  stop_proxy
done

echo "bench done. Write-up (M7 artifact): numbers + 3 things tried + why."
echo "See DESIGN 11.3 methodology notes above."
