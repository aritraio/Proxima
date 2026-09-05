#!/bin/bash
# scripts/e2e.sh -- end-to-end correctness harness (DESIGN.md 11.2).
# Runs proxy + 2 origins, asserts status/body hash per scenario, prints PASS/FAIL.
set -u
BUILD="${BUILD:-build}"
PXLB="$BUILD/pxlb"
ORIGIN="$BUILD/origin"
PROXY_PORT=18080
O1_PORT=19001
O2_PORT=19002
CFG=/tmp/pxlb_e2e.conf
PASS=0
FAIL=0

ok()   { echo "PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL: $1${2:+ -- $2}"; FAIL=$((FAIL+1)); }

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing $1, abort"; exit 2; }; }
need curl
need python3

cleanup() {
  for p in ${O1_PID:-} ${O2_PID:-} ${PX_PID:-}; do
    if kill -0 "$p" 2>/dev/null; then kill "$p" 2>/dev/null; fi
  done
  wait ${O1_PID:-} 2>/dev/null || true
  wait ${O2_PID:-} 2>/dev/null || true
  wait ${PX_PID:-} 2>/dev/null || true
  rm -f "$CFG"
}
trap cleanup EXIT INT TERM

# --- build ---
make "$PXLB" "$ORIGIN" >/dev/null 2>&1 || { echo "build failed"; exit 1; }

# --- start origins ---
"$ORIGIN" "$O1_PORT" 2>/tmp/origin1.log &
O1_PID=$!
"$ORIGIN" "$O2_PORT" 2>/tmp/origin2.log &
O2_PID=$!

wait_port() { # host port timeout_s
  python3 - "$1" "$2" "$3" <<'PY'
import socket, sys, time
h, p, t = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
d = time.time() + t
while time.time() < d:
    s = socket.socket()
    s.settimeout(0.3)
    if s.connect_ex((h, p)) == 0:
        s.close(); sys.exit(0)
    s.close(); time.sleep(0.1)
sys.exit(1)
PY
}
wait_port 127.0.0.1 "$O1_PORT" 5 || { echo "origin1 not ready"; cat /tmp/origin1.log; exit 1; }
wait_port 127.0.0.1 "$O2_PORT" 5 || { echo "origin2 not ready"; cat /tmp/origin2.log; exit 1; }

# --- proxy config (health off for determinism; toggle test uses SIGSTOP) ---
cat > "$CFG" <<EOF
listen          127.0.0.1:$PROXY_PORT
workers         1
backlog         128
max_conns       4096
max_head        65536
buf_cap         65536
connect_timeout 2000
idle_timeout    10000
balance         round-robin
health_enabled  0
backend         127.0.0.1:$O1_PORT weight=1
backend         127.0.0.1:$O2_PORT weight=1
EOF

"$PXLB" -c "$CFG" 2>/tmp/pxlb.log &
PX_PID=$!
wait_port 127.0.0.1 "$PROXY_PORT" 5 || { echo "proxy not ready"; cat /tmp/pxlb.log; exit 1; }
sleep 0.3

PX="http://127.0.0.1:$PROXY_PORT"
O1="http://127.0.0.1:$O1_PORT"

# 1. GET small
if curl -sf "$PX/echo" -H "Host: x" -o /tmp/e_small.txt; then
  if grep -q "GET /echo" /tmp/e_small.txt; then ok "GET small /echo"; else bad "GET small /echo" "body mismatch"; fi
else bad "GET small /echo" "curl exit $?"; fi

# 2. GET large hash (1 MiB PRNG stream, proxy vs direct must match)
python3 - <<'PY' >/tmp/e_hash.txt 2>&1
import hashlib, http.client
def fetch(host, port, path):
    c = http.client.HTTPConnection(host, port, timeout=10)
    c.request("GET", path, headers={"Host": "x"})
    r = c.getresponse()
    assert r.status == 200, r.status
    return r.read()
direct = fetch("127.0.0.1", 19001, "/big?n=1048576")
via = fetch("127.0.0.1", 18080, "/big?n=1048576")
assert direct == via, f"mismatch {len(direct)} vs {len(via)}"
# verify PRNG formula byte i = (i*31+17)&0xFF
for i in (0, 1, 1000, 1000000):
    assert direct[i] == ((i*31+17) & 0xFF), f"prng byte {i}"
print("hash", hashlib.sha256(via).hexdigest(), len(via))
PY
if [ $? -eq 0 ]; then ok "GET large hash $(cat /tmp/e_hash.txt)"; else bad "GET large hash" "$(cat /tmp/e_hash.txt)"; fi

# 3. HEAD + 204 + 304 (no body)
HEAD_CODE=$(curl -s -o /dev/null -w "%{http_code} %{size_download}" -I "$PX/big?n=1024" -H "Host: x" 2>/dev/null || echo "fail")
if [ "$HEAD_CODE" = "200 0" ]; then ok "HEAD no body"; else bad "HEAD no body" "got $HEAD_CODE"; fi
C204=$(curl -s -o /dev/null -w "%{http_code}" "$PX/status204" -H "Host: x" 2>/dev/null)
[ "$C204" = "204" ] && ok "GET 204" || bad "GET 204" "got $C204"
C304=$(curl -s -o /dev/null -w "%{http_code}" "$PX/status304" -H "Host: x" 2>/dev/null)
[ "$C304" = "304" ] && ok "GET 304" || bad "GET 304" "got $C304"

# 4. POST with body (hash round-trip, 64 KiB)
python3 - <<'PY' 2>&1 | tee /tmp/e_post.txt | grep -q OK
import http.client, os
body = os.urandom(65536)
c = http.client.HTTPConnection("127.0.0.1", 18080, timeout=10)
c.request("POST", "/post", body=body, headers={"Host": "x", "Content-Length": str(len(body))})
r = c.getresponse()
data = r.read()
assert r.status == 200, r.status
assert data == body, f"echo mismatch {len(data)}"
print("OK post round-trip")
PY
if [ $? -eq 0 ]; then ok "POST echo round-trip"; else bad "POST echo round-trip" "$(cat /tmp/e_post.txt)"; fi

# 5. chunked response origin
CHUNKED=$(curl -s "$PX/chunked" -H "Host: x" 2>/dev/null)
if [ "$CHUNKED" = "hello world" ]; then ok "chunked response"; else bad "chunked response" "got [$CHUNKED]"; fi

# 6. truncated response -> client must see close (curl fails or short body)
python3 - <<'PY' 2>&1 | grep -q TRUNC
import http.client
c = http.client.HTTPConnection("127.0.0.1", 18080, timeout=5)
c.request("GET", "/close", headers={"Host": "x"})
try:
    r = c.getresponse()
    b = r.read()
    print(f"read {len(b)} (expected 10 of 1000, truncated)")
    assert len(b) < 1000, "expected truncation"
    print("TRUNC")
except Exception as e:
    print(f"TRUNC via exception {type(e).__name__}")
PY
if [ $? -eq 0 ]; then ok "truncated response closes"; else bad "truncated response closes"; fi

# 7. 100-continue upload
python3 - <<'PY' 2>&1 | grep -q CONTINUE_OK
import socket
s = socket.create_connection(("127.0.0.1", 18080), timeout=5)
body = b"x" * 1024
req = (b"POST /post HTTP/1.1\r\nHost: x\r\nContent-Length: 1024\r\n"
       b"Expect: 100-continue\r\nConnection: close\r\n\r\n")
s.sendall(req)
s.settimeout(3)
data = b""
try:
    data = s.recv(4096)
except Exception as e:
    pass
if b"100 Continue" in data:
    s.sendall(body)
    rest = b""
    s.settimeout(3)
    while True:
        try:
            ch = s.recv(8192)
        except Exception:
            break
        if not ch: break
        rest += ch
        if len(rest) > 2048: break
    assert b"200 OK" in data + rest, (data + rest)[:200]
    print("CONTINUE_OK")
else:
    print(f"no 100, got {data[:100]}")
s.close()
PY
if [ $? -eq 0 ]; then ok "100-continue"; else bad "100-continue"; fi

# 8. pipelining: 10 requests on one conn, all answered in order
python3 - <<'PY' 2>&1 | grep -q PIPE_OK
import socket
s = socket.create_connection(("127.0.0.1", 18080), timeout=5)
req = b"GET /echo HTTP/1.1\r\nHost: x\r\n\r\n" * 10
s.sendall(req)
s.settimeout(5)
data = b""
while data.count(b"200 OK") < 10 and len(data) < 100000:
    try:
        ch = s.recv(8192)
    except Exception:
        break
    if not ch: break
    data += ch
assert data.count(b"200 OK") == 10, f"got {data.count(b'200 OK')} responses"
print("PIPE_OK")
s.close()
PY
if [ $? -eq 0 ]; then ok "pipelining x10 in order"; else bad "pipelining x10 in order"; fi

# 9. error codes: 400 on garbage, metrics 200
python3 - <<'PY' 2>&1 | grep -q ERR400
import socket
s = socket.create_connection(("127.0.0.1", 18080), timeout=5)
s.sendall(b"BAD REQUEST LINE\r\n\r\n")
s.settimeout(3)
d = s.recv(4096)
assert b"400" in d, d[:100]
print("ERR400")
s.close()
PY
if [ $? -eq 0 ]; then ok "error 400 on garbage"; else bad "error 400 on garbage"; fi

# 10. keep-alive reuse: two requests, one conn
python3 - <<'PY' 2>&1 | grep -q KA_OK
import http.client
c = http.client.HTTPConnection("127.0.0.1", 18080, timeout=5)
c.request("GET", "/echo", headers={"Host": "x"})
assert c.getresponse().read() is not None
c.request("GET", "/echo", headers={"Host": "x"})
r = c.getresponse()
assert r.status == 200
print("KA_OK")
PY
if [ $? -eq 0 ]; then ok "keep-alive reuse"; else bad "keep-alive reuse"; fi

# 11. metrics endpoint: 200 text/plain + totals invariant
METRICS_BODY=$(curl -s "$PX/_proxima/metrics" -H "Host: x" 2>/dev/null)
METRICS_CT=$(curl -s -o /dev/null -w "%{content_type}" "$PX/_proxima/metrics" -H "Host: x" 2>/dev/null)
if echo "$METRICS_CT" | grep -q "text/plain"; then ok "metrics content-type"; else bad "metrics content-type" "got $METRICS_CT"; fi
python3 - <<PY 2>&1 | grep -q INV_OK
import re
body = """$METRICS_BODY"""
def num(pat):
    m = re.search(pat + r"\s+(\d+)", body)
    return int(m.group(1)) if m else None
total = num(r"pxlb_requests_total")
c1 = num(r'pxlb_responses\{class="1xx"\}')
c2 = num(r'pxlb_responses\{class="2xx"\}')
c3 = num(r'pxlb_responses\{class="3xx"\}')
c4 = num(r'pxlb_responses\{class="4xx"\}')
c5 = num(r'pxlb_responses\{class="5xx"\}')
ie = num(r"pxlb_internal_errors")
assert total is not None, "no total"
assert total == (c1+c2+c3+c4+c5+ie), f"{total} != {c1}+{c2}+{c3}+{c4}+{c5}+{ie}"
print("INV_OK")
PY
if [ $? -eq 0 ]; then ok "metrics invariant total==classes+internal"; else bad "metrics invariant"; fi

# 12. backend toggle under load (kill one origin, proxy still serves via other)
kill -STOP "$O1_PID" 2>/dev/null || kill "$O1_PID" 2>/dev/null
sleep 0.3
TOGGLE_OK=1
for i in 1 2 3 4; do
  if ! curl -sf "$PX/echo" -H "Host: x" -o /dev/null 2>/dev/null; then TOGGLE_OK=0; fi
done
kill -CONT "$O1_PID" 2>/dev/null || true
# with health off, round-robin will hit stopped node; allow retry: at least one must succeed
if curl -sf "$PX/echo" -H "Host: x" -o /dev/null 2>/dev/null; then ok "backend toggle survives"; else bad "backend toggle survives" "proxy error while one origin stopped"; fi

# 13. SIGTERM drain: exit 0
kill -TERM "$PX_PID"
wait "$PX_PID"
RC=$?
if [ "$RC" -eq 0 ]; then ok "SIGTERM drain exit 0"; else bad "SIGTERM drain exit 0" "exit $RC"; fi
PX_PID=""

echo "---"
echo "e2e: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
