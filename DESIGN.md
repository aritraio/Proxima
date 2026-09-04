# pxlb — L7 HTTP Reverse Proxy & Load Balancer in C

Design blueprint for a lightweight, high-concurrency HTTP/1.1 reverse
proxy + load balancer, built on Linux **non-blocking sockets + epoll
edge-triggered**, with zero-copy payload streaming and a zero-allocation
HTTP head parser.

Target audience: SRE / distributed-systems interview showcase. Every
design decision below is made deliberately and *defensible* — that is
the point of the project.

---

## 1. Scope

### 1.1 Goals (v1)

| Capability | Approach |
|---|---|
| Concurrency | Single-threaded reactor (epoll ET), `N` independent reactors later via `SO_REUSEPORT` |
| Routing | HTTP/1.1 request-target based; one upstream pool; host-based pools are a config extension |
| Balancing | Round-Robin and Weighted Least-Connections |
| Health | Active probes (`GET <path>`) with hysteresis; passive connect-failure signal |
| Buffering | One mbuf per direction of travel; payload relayed with zero inter-buffer copies |
| Parsing | Incremental char-level state machine; no malloc, no string copies during parse |

### 1.2 Deliberate non-goals (v1) — and why

| Non-goal | Reason / extension path |
|---|---|
| TLS/HTTPS | Add OpenSSL read/write BIOs in front of `conn_pump_*`; state machine unchanged (this is the classic nginx/HAProxy layering) |
| HTTP/2 | Requires stream multiplexing over one TCP conn; separate frontend layer |
| CONNECT / Upgrade / WebSocket | L4 tunneling mode — different beast (raw relay, no HTTP framing) |
| Request-body chunked decoding | We *forward* chunk framing verbatim; only *detection* of the end matters (see §5.4). Requests: v1 replies 501 to `Transfer-Encoding` (most tools use Content-Length) |
| Response caching / retries with replay | Retry-on-connect-failure later, GET/HEAD only, because we stream bodies (no replay buffer) |
| Upstream keep-alive pool | Fresh upstream conn per request in v1; reuse pool is an extension (§12) |
| Multi-threaded core | Single reactor keeps the state machine deterministic and testable; scale-out via reuseport workers |

---

## 2. Architecture

### 2.1 Process view

```
                      +-------------------------------------------+
  clients             |  pxlb (one process, one thread)           |
  ---------> listen() |                                           |
  (many conns)        |   epoll fd  (edge-triggered)              |
                      |      |                                    |
                      |      v                                    |
                      |  conn_handle_events(c, role, events)      |
                      |      |                                    |
                      |      v                                    |
                      |  +----------------------------+           |
                      |  | conn state machine (conn.c) |           |
                      |  |  - pumps: read/write loops  |           |
                      |  |  - parsers: req & resp      |           |
                      |  |  - buffers: c2u, u2c, stage |           |
                      |  +------+----------+-----------+           |
                      |         |          |                       |
                      |  pool_pick()   pool_pick()                 |
                      |  (RR / WLC)    (active health)             |
                      +-------------------------------------------+
                                    |  fresh non-blocking
                                    v  TCP conn per request
                              backends :9001 :9002 :9003
```

* Accept loop is a plain `accept4()` on the listen fd registered in the
  same epoll set.
* The health checker is not a thread: it is a timer-driven probe conn
  that flows through the same machinery.

### 2.2 One connection, three buffers

```
 client fd  <--->  c2u mbuf  <--->  upstream fd        (request path)
 upstream fd <-->  u2c mbuf  <--->  client fd          (response path)
                       ^
                       +-- stage mbuf: rewritten head, the ONLY copy
```

**Zero-copy contract (the interview answer):**

1. A payload byte is `read()` from the source socket into a buffer once,
   and `write()`n out of that same buffer. Payload is never copied
   between buffers and never reallocated per packet.
2. The *head* (request line / status line + headers) is parsed in place
   from `c2u`/`u2c` **without consuming it**, then rewritten into
   `stage` — because a proxy must strip hop-by-hop headers and may add
   `X-Forwarded-For`/`Host`. This is the only deliberate copy, it is
   bounded by `max_head` (default 64 KiB), and the body never passes
   through it. Real L7 proxies (nginx, HAProxy) all rewrite heads; they
   do not re-copy bodies.
3. `mbuf_compact()` memmoves live bytes to the front when the tail is
   fragmented — O(cap) worst case, amortized, and it never runs while a
   parser is mid-message (§5.1).
4. Optional later fast path: `splice()` fd→pipe→fd for large bodies,
   bypassing the mbuf entirely — a flag-gated experiment (§12), not v1.
   Saying "we do one-copy with an optional splice path and here is the
   measurement that shows whether it matters" beats claiming magic.

Memory per connection (defaults): `c2u` 64 KiB + `u2c` 64 KiB +
`stage` 2×`max_head` = 128 KiB → ≈ 256 KiB/conn. At `max_conns = 1024`
that is ~260 MB; tune `buf_cap`/`max_head` down for larger fan-in. This
math belongs in the README and in the interview answer.

---

## 3. Event model — epoll edge-triggered

Single `epoll_create1(EPOLL_CLOEXEC)`, single thread, epoll_wait
timeout = time to the nearest deadline (timer min-heap). Every
registration carries `(conn *, fd_role)` so the state machine always
receives a fully-qualified `(role, events)` tuple — this removes the
classic proxy ambiguity of "which side of the exchange does this event
belong to?".

### 3.1 ET invariants (enforced in `conn_pump_*`)

1. **Read until EAGAIN.** On `EPOLLIN` we loop `read()`/`recv()` until
   `PX_AGAIN`. Returning to epoll_wait with data still in the socket is
   a stall bug under ET (the edge already fired).
2. **Never re-arm EPOLLIN while buffered data is unparsed/unrelayed.**
   If `u2c` still holds bytes we can write to the client, EPOLLIN on the
   upstream fd stays off; we only ask for more when the pipeline is
   empty. Interest is a *function of buffer state*, recomputed after
   every transition (`conn_sync_interest()`).
3. **EPOLLOUT only on demand.** Arm EPOLLOUT when (a) a non-blocking
   connect is in flight, or (b) a write returned EAGAIN with bytes still
   queued. Drop it the instant the outbound side drains — otherwise the
   loop spins forever.
4. **Bound reads by framing.** While a request body is streaming, never
   read more from the client than `req_body_left` (Content-Length) —
   surplus bytes would be indistinguishable from a pipelined next
   request (§5.5).
5. **Buffer-full means backpressure, not drop.** If `c2u` is full and
   the upstream socket is blocked (EAGAIN), we remove EPOLLIN from the
   client fd. TCP flow control then pushes back on the client. The
   chain client→proxy→backend is one bounded queue.

Level-triggered would forgive sloppy interest handling; ET is what
separates a toy proxy from a real one and is a superb interview topic —
these five rules are the whole game.

### 3.2 Interest-arbitration table (per master state)

`I` = EPOLLIN, `O` = EPOLLOUT, `-` = off. Conditions in parentheses.

| State | want[client] | want[upstream] |
|---|---|---|
| CS_ACCEPTED / CS_READ_REQ | I | – |
| CS_SELECT | I (while head done but nothing sent yet — actually off; see note) | – |
| CS_CONNECT | I if `req_body_left>0` and `c2u` not full | O (connect) |
| CS_SEND_REQ_HEAD | I if body still incoming and c2u has space | O while stage/c2u not drained |
| CS_RELAY | I iff `!req_body_done && c2u` has space | O iff c2u (or stage) non-empty; I iff `!resp_done && u2c` has space |
| CS_CLOSING | – | – |

Note: in CS_SELECT we must not read ahead arbitrarily: we already hold
whatever was over-read past the head in `c2u`; we keep reading only up
to framing limits once the backend is chosen — so CS_SELECT is entered
and exited in the same event callback (no syscalls between, connect is
issued immediately, state becomes CS_CONNECT). Reads of the *body* then
continue under CS_CONNECT rules.

---

## 4. Connection state machine

### 4.1 States

Defined in `conn.h` (`enum conn_state`): `CS_ACCEPTED, CS_READ_REQ,
CS_SELECT, CS_CONNECT, CS_SEND_REQ_HEAD, CS_RELAY, CS_CLOSING, CS_DONE`.

### 4.2 Event taxonomy

Every entry is `(role, event)`:

| Event | Meaning |
|---|---|
| `(C,IN)` / `(U,IN)` | fd readable (loop pumps until EAGAIN) |
| `(C,OUT)` / `(U,OUT)` | fd writable — drain queue, or connect() finished (U only) |
| `(C,EOF)` / `(U,EOF)` | read()==0 or EPOLLRDHUP |
| `(C,ERR)` / `(U,ERR)` | EPOLLERR/EPOLLHUP or write error |
| `EV_TIMEOUT` | conn timer fired: connect deadline or idle deadline |

### 4.3 Transition table

`head` below = "request/response head parsed in place". Every row ends
with `conn_sync_interest()`.

| # | From | Event | Condition → Action | To |
|---|---|---|---|---|
| 1 | CS_ACCEPTED | – | entry: register client fd (I), arm idle timer | CS_READ_REQ |
| 2 | CS_READ_REQ | (C,IN) | pump reads into c2u; feed `http_parse`; NEED_MORE→keep reading until EAGAIN; if head>limit → abort 431 | CS_READ_REQ |
| 3 | CS_READ_REQ | head DONE | `http_resolve`; if chunked request → 501; build rewritten request head in `stage` (§5.3); consume `head_len` from c2u; leftover bytes are request body (or pipelined prefix — framed by req_body_left); decide client keep-alive | CS_SELECT |
| 4 | CS_READ_REQ | (C,EOF) before head | 0 bytes → silent close; partial → 400 | CS_CLOSING |
| 5 | CS_READ_REQ | EV_TIMEOUT | 408 | CS_CLOSING |
| 6 | CS_SELECT | – | entry: `pool_pick()`; none UP → 503; else `node_begin`, snapshot addr, `connect()` nonblocking; EINPROGRESS → arm (U,OUT)+connect timer | CS_CONNECT |
| 7 | CS_SELECT | connect() returns 0 | | CS_SEND_REQ_HEAD |
| 8 | CS_CONNECT | (U,OUT) or (U,ERR) | `getsockopt(SO_ERROR)`; 0 → clear connect timer | CS_SEND_REQ_HEAD |
| 9 | CS_CONNECT | connect error | 502 (or 504 if timer expired); `node_end(ok=0)`; passive health signal | CS_CLOSING |
| 10 | CS_CONNECT | EV_TIMEOUT | 504 | CS_CLOSING |
| 11 | CS_SEND_REQ_HEAD | (U,OUT) | drain `stage`; when empty: `req_head_sent=1`; continue draining body from c2u | CS_RELAY (once stage empty; body may still stream) |
| 12 | CS_RELAY | (U,OUT) | drain c2u→upstream until EAGAIN/empty; drop EPOLLOUT when drained | CS_RELAY |
| 13 | CS_RELAY | (C,IN) | read ≤ min(c2u space, req_body_left); on EAGAIN stop; when req_body_left hits 0 → `req_body_done=1`, drop (C,IN) (any pipelined bytes stay unread in socket or as c2u tail) | CS_RELAY |
| 14 | CS_RELAY | (U,IN) | pump into u2c; feed response parser; NEED_MORE → keep reading; on 1xx interim → drop it and re-init parser at offset (we never solicit 100) | CS_RELAY |
| 15 | CS_RELAY | response head DONE | resolve framing (§5.4); rewrite head into stage; consume head_len from u2c; arm (C,OUT) | CS_RELAY |
| 16 | CS_RELAY | (C,OUT) | drain stage→client, then u2c→client; when u2c empty and resp_done → finish request | see 20 |
| 17 | CS_RELAY | (U,EOF) | framing UNTIL_EOF → resp_done when u2c drains; framing LENGTH with resp_body_left>0 → **truncated**: if partial body already sent, abort silently (can't fix length); else 502 | CS_RELAY / CLOSING |
| 18 | CS_RELAY | (U,ERR) or EV_TIMEOUT | nothing sent yet → 502/504; else abort silently | CS_CLOSING |
| 19 | CS_RELAY | chunk watcher fires CHF_END | (chunked responses) resp_done=1 | CS_RELAY |
| 20 | all | resp_done && u2c empty && stage empty | `keep_alive_ok`? reset request-scoped state (§4.4), arm (C,IN) → CS_READ_REQ : close | CS_READ_REQ / CS_CLOSING |
| 21 | any | (C,ERR/EOF) mid-exchange | abort; `node_end` | CS_CLOSING |
| 22 | CS_CLOSING | – | unregister fds, `node_end`, free | CS_DONE |

### 4.4 Request/response overlap — why the table stays small

In HTTP/1.1 the backend may answer *before* the request body finishes
streaming (e.g. 413, or an auth failure). Handling that with one enum
would need state pairs (`sending_body | reading_resp`). Instead the
master state stays `CS_RELAY` and the four flags do the work:

* `req_head_sent`, `req_body_done` — request fully delivered upstream
* `resp_done` — response fully relayed to client
* `upstream_eof` — backend closed its side

The machine is then just "keep both pumps alive until each side's
terminal flag AND its buffer is drained". Response head parsing is a
*pump-local* sub-task inside CS_RELAY (rows 14–15). This is the nginx
two-handler model in miniature and keeps every transition testable.

### 4.5 Keep-alive loop & pipelining

* Client keep-alive is default for HTTP/1.1 (and HTTP/1.0 with
  `Connection: keep-alive`). On row 20 the conn re-enters CS_READ_REQ:
  parsers are reset, buffers are *not* freed, and any bytes left in c2u
  (a pipelined next request we already over-read) are fed straight to
  the fresh parser — bounded pipelining for free.
* v1 reads are framed by Content-Length (§3.1 rule 4) so pipelined
  surplus never mixes into the current body.
* Backend conns are always closed after the exchange (`Connection:
  close` added in the rewritten request head), so there is no upstream
  keep-alive state to corrupt — deliberate v1 simplification.

### 4.6 Error policy

| Situation | Reply |
|---|---|
| Malformed request line / headers, obs-fold, bad version | 400 (505 for HTTP/2 preface bytes "PRI * …") |
| Request head > max_head | 431 (414 if only the request line overflows) |
| Request body with Transfer-Encoding (v1) | 501 |
| No backend UP | 503 |
| connect() refused / reset | 502 |
| connect() timeout | 504 |
| Client idle while reading request | 408 |
| Mid-body backend failure, nothing sent yet | 502 |
| Mid-body backend failure, partial body sent | silent close (cannot fix Content-Length) |
| `max_conns` reached | accept() then immediately 503 + close |

Errors are emitted via `http_build_error_reply()` into `stage` and
flushed on (C,OUT); connection then closes (no keep-alive after an
error reply, `Connection: close`).

### 4.7 Timers

| Phase | Deadline | On fire |
|---|---|---|
| CS_CONNECT | now + connect_timeout | 504 |
| CS_READ_REQ (no progress) | now + idle_timeout | 408 |
| CS_RELAY (no progress either direction) | now + idle_timeout | silent abort (client gone or backend hung) |

One reusable `loop_timer` per conn; any progress in `conn_pump_*`
reschedules it. Health probes reuse the same machinery with their own
deadline.

---

## 5. HTTP parsing & framing

### 5.1 Zero-allocation contract

`http_parse()` (http.h) is a pure byte scanner over `[base, base+len)`:
state, cursor `pos`, and `px_range` offsets — nothing else. Because the
connection code does **not consume** the buffer while a head is being
parsed, `off` values are stable across feeds; between feeds the buffer
only grows at the tail. Limits: >max_head → 431. Recorded header ranges
(`hname[]/hvalue[]`) plus pre-resolved indexes for the headers we act
on (`idx_host`, `idx_connection`, …) mean the rewriter and framing logic
never rescan. Unit tests drive it byte-by-byte and assert the exact
`HSS_*` state after every byte.

### 5.2 What we record (not allocate)

Method, target (verbatim, incl. query), version, status, reason phrase,
nheader ranges, Content-Length value, TE chunked flag, Expect flag,
Connection tokens, Host, XFF presence, Upgrade presence.

### 5.3 Head rewriting (both directions)

The proxy rewrites heads because hop-by-hop headers are per-link
(RFC 7230 §6.1):

* **Request → backend:** strip `Connection`-listed headers + keep
  `Connection: close`; add/append `X-Forwarded-For`; drop `Expect` (we
  answer 100 locally, below); forward Host unchanged unless absent
  (then synthesize from config listener? no — 400 if HTTP/1.1 without
  Host).
* **Response → client:** strip `Connection` + its listed headers,
  `Keep-Alive`, `Proxy-Connection`, `Transfer-Encoding` **only if we
  re-framed** (we don't — we relay chunked bytes verbatim, so TE is
  passed through untouched); emit our own `Connection: keep-alive` /
  `close` matching our client-link decision. Body bytes between the
  head boundary and the next message are relayed verbatim — chunk
  framing, trailers, everything.

Rewriting is a rebuild of `stage` from the recorded ranges + added
lines; worst-case size ≤ 2×max_head → `stage_cap` covers it. No parse
tree, no strings, no realloc.

### 5.4 Response framing matrix (`http_resolve`)

| Case | Framing | End detection |
|---|---|---|
| HEAD request, or status 1xx/204/304 | BF_NONE | none — resp_done immediately after head |
| Status ≥200 with Content-Length | BF_LENGTH | count `resp_body_left` down to 0 |
| `Transfer-Encoding: chunked` | BF_CHUNKED | `chunk_watch` scans relayed bytes; CHF_END at final chunk + trailers |
| Neither (e.g. 200 without CL) | BF_UNTIL_EOF | upstream EOF |

Interim 1xx: parse sees a complete 1xx head → we discard it and
re-init the parser at the same offset to parse the real status line
(we never forward `Expect`, so upstream 100s should not occur; a stray
interim is dropped rather than confusing our framing). 100-continue
from clients: we answer `HTTP/1.1 100 Continue` ourselves as soon as
the request head is resolved and the backend is chosen, then stream
the body — the client never waits on the backend.

### 5.5 Body streaming and request boundaries

Request body handling is the subtle part and is framed exactly:

* head parsed (nothing consumed) → consume `head_len` → `req_body_left
  = Content-Length` → reads bounded by `req_body_left` → when it hits 0,
  any *unread* surplus stays in the socket, any *over-read* surplus sits
  at the c2u tail and becomes the next request's parse input (§4.5).
* This is why pipelining stays correct without an output queue: we never
  admit bytes past the body boundary while the body is being read.

---

## 6. Core structures (map to headers)

| Header | Contents |
|---|---|
| `proxy.h` | `px_result`, `fd_role`, `px_range`, defaults, generated-status enum |
| `buf.h` | `struct mbuf` — linear compacting buffer (§2.2) |
| `http.h` | `http_parser` (HSS_* scanner), `http_msg` (resolved), `chunk_watch`, framing enums |
| `conn.h` | `conn_state`, `struct conn` (context), `conn_params`, pumps + handlers |
| `event.h` | opaque `event_loop`, fd registration, `loop_timer` |
| `pool.h` | `server_node` (addr, weight, health, active/total/failed), `server_pool`, `pool_pick` |
| `config.h` | `proxy_config` snapshot + file parser |
| `health.h` | `health_checker` + probe bookkeeping |

`struct conn` layout notes: hot scalars (fds, `want[]`, state flags,
body counters) are grouped first; parsers (~700 B each) and 256 KiB of
buffers live at the tail. Conn objects are malloc'd per accept in v1; a
slab + free-list is an M7 polish item.

---

## 7. Pool & balancer

* `pool_pick()` returns an UP node or NULL (→ 503).
* **Round-Robin:** `pool->cursor++ % count`, skipping non-UP nodes,
  weight-blind.
* **Weighted Least-Connections:** score a node as
  `ceil(active * 1000 / weight)` using integer arithmetic (scale avoids
  float and preserves weight ordering); pick the minimum, tie-break by
  walking from the RR cursor so one idle node can't monopolize. A
  connect *in progress* counts toward `active` (it will occupy the
  node), so slow-to-connect backends are naturally penalized.
* Accounting: `node_begin()` at pick, `node_end(ok)` on conn teardown —
  single-threaded, no locks. Health state is owned by health.c.
* Weights and addresses come from the config file (§9) at startup.

---

## 8. Active health checking

* Timer every `health_interval_ms`; a probe is a normal conn:
  non-blocking connect to the node + `GET <health_path> HTTP/1.1` with
  `Host:`, relayed through the standard machines with `health_timeout`
  deadline. Probe conns are tagged so accounting and access logs skip
  them.
* Outcome: 2xx/3xx within timeout = OK; anything else = failure.
* Hysteresis (constants in proxy.h):
  * UP → (fail_threshold consecutive fails) → CHECKING
  * CHECKING → (ok_threshold OKs) → UP; (fail_threshold more) → DOWN
  * DOWN → probed every interval; (ok_threshold OKs) → UP
  * Non-UP nodes are excluded from `pool_pick`; in-flight traffic runs
    to completion.
* Passive signal: a real-request connect failure counts as a probe
  failure immediately (`health_on_passive_failure`) so the pool reacts
  in milliseconds, not one interval later — with the same hysteresis so
  one blip doesn't flap a node.

Demo story for interviews: kill a backend while `wrk` runs; watch error
count stay zero and throughput dip; restart it; watch it rejoin after
ok_threshold probes. That is the load-test script §11.

---

## 9. Config

```
# pxlb.conf
listen          127.0.0.1:8080
workers         1            # N = N processes behind SO_REUSEPORT
max_conns       4096
max_head        65536        # bytes
buf_cap         65536        # per-direction relay buffer, bytes
connect_timeout 2000         # ms
idle_timeout    30000        # ms
balance         weighted-least-connections   # | round-robin
health_enabled  1
health_interval 3000
health_path     /healthz
backend         127.0.0.1:9001 weight=1
backend         127.0.0.1:9002 weight=2
backend         127.0.0.1:9003 weight=1
```

`config_load()` is pure text parsing into a `proxy_config` snapshot;
`main()` seeds the pool from `cfg->backends`. Access logging: one line
per request to stderr (`ts client backend status bytes dur`), cheap and
always on in v1.

---

## 10. Roadmap

Each milestone ends with a **definition of done**. Test harness: plain
C, one `main` per test file, no framework (interviews love this), run
under ASan/UBSan from day one. `make check` = build + run all unit
tests; `make check-integration` adds end-to-end tests against local
origins. Linux CI (GitHub Actions ubuntu) is where epoll code gets
compiled + tested — macOS can compile headers and pure-logic tests
only.

| # | Milestone | Deliverables | Done when |
|---|---|---|---|
| M0 | Skeleton | Headers (done), Makefile, `tests/header_smoke.c`, `.gitignore`, CI | `make check` green on macOS + Linux; headers compile `-Wall -Wextra -Werror` |
| M1 | mbuf + parsers | `src/buf.c`, `src/http.c` | Unit tests: buf compaction vs. a model; parser corpus (valid, LF-only, obs-fold→400, >limit→431, split feeds byte-by-byte incl. every CR/LF boundary, chunked flag, Expect flag); `chunk_watch` incl. trailers + split final chunk; resolve() framing matrix incl. HEAD/204/304 |
| M2 | Reactor | `src/event.c`, `src/main.c` listener | epoll add/mod/del, ET read-until-EAGAIN proven with a test peer; timer min-heap; graceful SIGINT/SIGTERM; `strace -e epoll_ctl,epoll_wait` shows interest toggling |
| M3 | Skeleton proxy (no body) | conn.c path: REQ head → pick → connect → send head → resp head → relay → keep-alive | curl -v through proxy to `tests/origin.c`; errors 400/431/408/501/502/503/504 exercised; HEAD/204/304 framing; keep-alive verified (curl two requests one conn); access log correct |
| M4 | Streaming relay | body pumps + backpressure, chunked responses, 100-continue | 100 MB download + 100 MB upload hash-verified end-to-end; slow-backend test (pause origin) proves client reads stop (backpressure) then resume; wrk small run passes with zero errors |
| M5 | Pool, balancer, health | config.c, pool.c, health.c, CLI | Unit tests: RR rotation, WLC weight ratios (weight 2 backend gets ~2× traffic under load), node up/down exclusion; config parser tests; live toggle test: kill/restart origin during `wrk` → zero client errors |
| M6 | Hardening | pipelining leftovers, retry-on-connect-failure (GET/HEAD only, max 1), max_conns cap, timeouts audit | Test: pipeline 10 requests on one conn, all 10 answered in order; origin restart mid-bench → retried requests succeed; fd/conn caps hold |
| M7 | Perf & polish | writev batching, buffer tuning, `splice()` experiment flag, README metrics, k6 suite, slab allocator | Bench suite §11 reproducible; write-up: numbers + 3 things tried + why; full `-fsanitize=address,undefined` clean under load; TSan clean |

Keep M0–M5 as the "must have" for interviews; M6–M7 are stretch that
impress if reached.

---

## 11. Load & correctness testing

### 11.1 Local origins

* `tests/origin.c` — tiny single/multi-conn origin: echoes headers,
  serves N bytes of a PRNG stream (hash-verifiable), supports
  `/healthz`, `/slow?ms=`, `/big?n=`, `/close` (drops conn mid-body),
  configurable per-path `Content-Length` vs chunked.
* `python3 -m http.server` works as a second, independent origin for
  the demo (serves Content-Length).

### 11.2 Correctness harness

* socketpair-driven unit tests of the state machine (deterministic
  byte injection, no network) — M3.
* End-to-end script `scripts/e2e.sh`: for each scenario below, run
  proxy + 2 origins, assert status/body hash, print PASS/FAIL.
  1. GET small/large (hash), 2. HEAD + 204 + 304 (no body), 3. POST
     with body (hash round-trip), 4. chunked response origin, 5.
     truncated response → client sees conn close, 6. 100-continue
     upload, 7. pipelining, 8. every error code (§4.6), 9. keep-alive
     reuse, 10. backend toggle under load.

### 11.3 Load testing (wrk + k6)

`scripts/bench.sh` — phases below, each prints p50/p90/p99 + errors.

```bash
# baseline: origin directly (headroom reference)
wrk -t4 -c200 -d30s --latency http://127.0.0.1:9001/big?n=1048576
# through proxy, same load
wrk -t4 -c200 -d30s --latency http://127.0.0.1:8080/big?n=1048576
# request-bound (small bodies, keep-alive) — exercises the state machine
wrk -t4 -c400 -d30s --latency http://127.0.0.1:8080/echo
# upload-heavy
wrk -t4 -c200 -d30s --latency -s scripts/post.lua http://127.0.0.1:8080/post
# ramp: c 1..1000 to show the curve + where it bends
```

`scripts/post.lua` (wrk): generates a fixed 64 KiB buffer and sends it
with Content-Length on each request (wrk keeps the conn alive).

k6 (`scripts/bench_k6.js`) is the *demo* tool, not the throughput tool —
it makes graceful behavior visible:

```js
// scenarios: ramping-vus to 500, 2 origins behind proxy,
// checks: status 200, http_req_duration p(95) < 200ms, errors == 0
```

Run it while toggling a backend (`kill -STOP`/`-CONT` or restart the
origin) to show zero failed requests and the health checker pulling the
node in/out (§8 demo).

Methodology notes to *state out loud* (and in the README):
1. Always compare proxy vs direct-to-origin on the same box; the gap is
   your proxy tax.
2. `wrk` keeps connections alive — set `ulimit -n` high (`ulimit -n
   65535`) or the benchmark is measuring fd exhaustion.
3. Watch `TIME_WAIT` (netstat) — a proxy that closes backend conns per
   request churns tuples; tune with `net.ipv4.tcp_tw_reuse` for the
   bench, and note upstream keep-alive (§12) as the real fix.
4. Separate request-bound (small body, high RPS, state-machine cost)
   from bandwidth-bound (large body, memcpy/splice cost) workloads —
   they measure different things.
5. Record CPU% and `perf stat` so "throughput" is tied to actual work.

---

## 12. Pitfalls checklist & extension menu

Pitfalls (each is a classic proxy bug):
* ET + leftover data → stall (§3.1 rules 1–2)
* Reading past the request body into a pipelined request (§3.1 rule 4)
* Forwarding hop-by-hop headers across the proxy (§5.3)
* Believing a Content-Length body until the backend EOFs early (§4.3
  row 17 — truncation must close the client, not send a short body)
* 100-continue deadlock (client waits for 100, we wait for body)
* Blocking getaddrinfo in the hot path (done once, at startup)
* Writing `send()` results without handling partial writes
* Timers that never fire because epoll_wait has no deadline

Extensions (pick for follow-up interviews):
* Upstream keep-alive pool with staleness validation — biggest real
  win, shows you know HTTP/1.1 reuse hazards
* `splice()` zero-copy fast path with a benchmark proving it
* SO_REUSEPORT workers + per-worker stats aggregation
* TLS frontend (OpenSSL BIOs over the existing pumps)
* Dynamic config reload (SIGHUP) with connection draining
* gRPC-style trailers / full chunked-request support (removes the 501)
* Prometheus `/metrics` endpoint instead of stderr logs
