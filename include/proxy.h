/*
 * proxy.h -- shared vocabulary for the L7 reverse proxy / load balancer.
 *
 * No Linux-specific headers in this file: everything here is plain C11 +
 * POSIX so headers can be compile-checked on any host.  Kernel specifics
 * (epoll, splice, TCP tunables) are confined to src/event.c and friends,
 * which are Linux-only.
 */
#ifndef PX_PROXY_H
#define PX_PROXY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PX_VERSION "0.1.0"

/* ------------------------------------------------------------------ */
/* Result codes returned by every I/O-ish routine in the code base.    */
/* Callers treat PX_AGAIN as "non-blocking socket said EAGAIN" and     */
/* re-arm interest through the event loop; PX_EOF is a clean read of 0 */
/* bytes; PX_ERR is fatal for the current operation (errno captured    */
/* by the caller in its context struct).                               */
/* ------------------------------------------------------------------ */
enum px_result {
    PX_OK = 0,   /* progress was made */
    PX_AGAIN,    /* would block: stop the current read/write loop */
    PX_EOF,      /* peer closed cleanly (read() == 0) */
    PX_ERR       /* hard error; see errno / conn.err */
};

/* A connection owns at most two sockets.  Every event entering the
 * state machine is tagged with one of these so dispatch is explicit. */
enum fd_role {
    FD_ROLE_CLIENT = 0,   /* downstream: browser / load generator */
    FD_ROLE_UPSTREAM,     /* upstream: backend origin */
    FD_ROLE_COUNT
};

/* Byte range {off,len} relative to some buffer base.  Used by the HTTP
 * parser to describe tokens *without copying them* (the zero-allocation
 * requirement).  Offsets are 32-bit because every buffer we parse from
 * is capped well below 4 GiB. */
struct px_range {
    uint32_t off;
    uint32_t len;
};

#define PX_RANGE_NONE ((struct px_range){UINT32_MAX, 0})
#define PX_INDEX_NONE 0xFFu   /* sentinel for "no such header recorded" */

/* ------------------------------------------------------------------ */
/* Limits / defaults (tunable later via config).                       */
/* ------------------------------------------------------------------ */
#define PX_MAX_HEADERS       64u     /* header lines we record per message */
#define PX_MAX_BACKENDS      64u     /* static backends from config file   */
#define PX_DEF_MAX_HEAD      (64u * 1024u)   /* request/response head cap  */
#define PX_DEF_BUF_CAP       (64u * 1024u)   /* per-direction relay buffer */
#define PX_DEF_MAX_CONNS     8192u
#define PX_DEF_CONNECT_TIMEOUT_MS   5000u
#define PX_DEF_IDLE_TIMEOUT_MS     30000u
#define PX_DEF_HEALTH_INTERVAL_MS   3000u
#define PX_DEF_HEALTH_TIMEOUT_MS    1000u
#define PX_DEF_HEALTH_OK_THRESHOLD     2u   /* consecutive successes -> UP  */
#define PX_DEF_HEALTH_FAIL_THRESHOLD   3u   /* consecutive failures -> DOWN */

/* The stage buffer that holds a rewritten head must fit the worst case:
 * original head bytes (<= max_head) plus anything we add (Host, XFF,
 * Connection rewrites).  Rather than compute exact deltas we size it at
 * 2x max_head; head rewrite only ever shrinks in practice. */
#define PX_STAGE_CAP_FACTOR  2u

/* HTTP status codes we may generate ourselves (all others are relayed
 * verbatim from the backend). */
enum http_status {
    HTTP_400_BAD_REQUEST      = 400,
    HTTP_408_REQUEST_TIMEOUT  = 408,
    HTTP_413_PAYLOAD_TOO_LARGE = 413,
    HTTP_414_URI_TOO_LONG     = 414,
    HTTP_431_HEADERS_TOO_LARGE = 431,
    HTTP_500_INTERNAL_ERROR   = 500,
    HTTP_502_BAD_GATEWAY      = 502,
    HTTP_503_SERVICE_UNAVAILABLE = 503,
    HTTP_504_GATEWAY_TIMEOUT  = 504,
    HTTP_505_VERSION_NOT_SUPPORTED = 505
};

/* Monotonic millisecond clock.  Implemented once (src/util.c); every
 * deadline in the code base is expressed in these units. */
int64_t px_now_ms(void);

#endif /* PX_PROXY_H */
