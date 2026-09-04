# pxlb -- L7 HTTP reverse proxy / load balancer in C (epoll, ET)

Learning-grade, interview-grade HTTP/1.1 reverse proxy and load
balancer: non-blocking sockets, epoll edge-triggered, zero-copy payload
relay, zero-allocation HTTP head parser, round-robin / weighted
least-connections balancing, active health checks.

**Status: M0.5 (hardening contracts).** M0 headers + smoke test, plus
the M0.5 production-hardening pass: tiered buffer allocation, the
compaction-safe parse invariant, always-armed EPOLLRDHUP, a splice pipe
pool, defensive node accounting, the graceful-drain lifecycle, and
cacheline-aligned RED metrics — all speced in [DESIGN.md](DESIGN.md)
and enforced by `tests/header_smoke.c`. Implementation starts at M1
(mbuf + HTTP parser + unit tests).

```
make check            # compiles headers + smoke test (-Werror), runs it
```

The smoke test is deliberately pure so it runs on any POSIX host
(macOS included). The epoll sources (src/event.c and friends) are
Linux-only and are built/tested in Linux CI -- see DESIGN.md §10.

Layout:

```
include/   public headers (portable, compile-checked everywhere)
src/       implementation (Linux-only code isolated in event.c)
tests/     unit + integration tests, origin server
scripts/   e2e + wrk/k6 load test scripts (from M3/M5 on)
DESIGN.md  architecture blueprint, state machines, roadmap
```
