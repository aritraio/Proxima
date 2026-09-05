/*
 * conn.h -- one proxied HTTP exchange: the master state machine, its
 * buffers and parsers, and the per-connection parameters.
 *
 * MODEL (full rationale + transition tables in DESIGN.md):
 *
 *   A conn owns two sockets (client downstream, upstream backend) and
 *   up to three mbufs, allocated in TIERS (hazard 1) so idle/slowloris
 *   connections do not pin 256 KiB each:
 *
 *     c2u   client -> proxy -> upstream.  The ONLY buffer allocated at
 *           conn_accept() (request-head ingestion must not wait on an
 *           allocation).  Grows (up to prm.max_head) while the request
 *           head is parsed; request body streams through it, zero copy.
 *     u2c   upstream -> proxy -> client.  Unallocated (data == NULL)
 *           until CS_SELECT allocates it via conn_alloc_buffers().
 *     stage rewritten outbound head (the ONLY deliberate copy; heads
 *           only, never payload).  Also deferred to CS_SELECT.
 *
 *   The keep-alive loopback (row 20 in DESIGN.md) calls
 *   conn_free_buffers(), returning u2c + stage to the allocator between
 *   requests; c2u is retained (and compacted to index 0) so the next
 *   request head starts fresh at offset 0 (hazard 2 invariant).
 *
 *   One master state (enum conn_state) describes the lifecycle; the
 *   small per-request flags (req_body_done, resp_done, ...) resolve
 *   the overlap where the upstream response arrives while the request
 *   body is still being uploaded.  Every event entering the machine is
 *   fully qualified: (fd role, epoll events).  After every transition
 *   the machine recomputes wanted interest per fd and calls loop_mod()
 *   only when the mask changed -- see conn_sync_interest().
 *
 *   EOF MODEL (hazard 3): *_eof is set EITHER when read() returns 0 OR
 *   -- without waiting for a read -- when the loop reports EPOLLRDHUP
 *   (always armed).  EOF means "no more input will arrive", NOT "stop
 *   relaying": buffered outbound data (e.g. u2c after the backend's
 *   FIN) is still flushed before the conn closes.
 *
 *   TIMING: conn->deadline_ms is an absolute monotonic deadline for the
 *   current phase (connect timeout while CONNECT, idle timeout
 *   elsewhere).  Any progress resets it.  One reusable loop_timer per
 *   conn fires it.  During drain (hazard 6) the deadline is capped at
 *   the drain grace deadline for in-flight conns.
 *
 *   SPLICE FAST PATH (hazard 4): pipe_r/pipe_w are -1 normally.  In
 *   CS_RELAY a pair may be borrowed from the process-wide pipe pool; if
 *   the pool is empty the relay degrades to mbuf streaming -- traffic
 *   is never dropped over a pipe shortage.
 */
#ifndef PX_CONN_H
#define PX_CONN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "buf.h"
#include "http.h"
#include "proxy.h"

struct event_loop;
struct loop_timer;
struct server_node;

/* ------------------------------------------------------------------ */
/* Master lifecycle state.  Invariant: only one of these is active.    */
/* ------------------------------------------------------------------ */
enum conn_state {
    CS_ACCEPTED = 0,   /* just accept()ed; register client fd, read */
    CS_READ_REQ,       /* feeding request head to http_parse() */
    CS_SELECT,         /* head done: pick backend, snapshot address,
                          begin non-blocking connect() */
    CS_CONNECT,        /* connect in flight (EPOLLOUT on upstream fd);
                          request body may keep buffering in c2u */
    CS_SEND_REQ_HEAD,  /* flushing rewritten request head from stage to
                          upstream; body in c2u may drain after */
    CS_RELAY,          /* steady state: request body client->upstream and
                          response upstream->client stream concurrently */
    CS_CLOSING,        /* terminal: tear down fds, account, free conn */
    CS_DONE
};

/* Per-connection tunables copied out of proxy_config at accept() time
 * so conn.c never touches the live config. */
struct conn_params {
    size_t   max_head;
    size_t   buf_cap;
    size_t   stage_cap;      /* >= 2 * max_head (rewrite worst case) */
    uint32_t connect_timeout_ms;
    uint32_t idle_timeout_ms;
};

struct conn {
    /* --- loop linkage --- */
    struct event_loop *loop;
    struct conn       *next;          /* intrusive list (loop / freelist) */

    /* --- fds + interest --- */
    int        cfd;                   /* client  fd, -1 when closed */
    int        ufd;                   /* upstream fd, -1 when closed */
    uint32_t   want[FD_ROLE_COUNT];   /* current epoll masks */

    /* --- lifecycle --- */
    enum conn_state state;
    uint16_t        err_status;       /* 0, or HTTP status we reply with */
    int             saved_errno;      /* last errno captured on PX_ERR */
    struct conn_params prm;

    /* --- timer --- */
    struct loop_timer *timer;
    int64_t            deadline_ms;   /* absolute, px_now_ms() basis */

    /* --- backend --- */
    struct server_node   *node;       /* NULL until CS_SELECT */
    struct sockaddr_storage uaddr;    /* snapshot: survives pool churn */
    socklen_t              uaddr_len;

    /* --- request side --- */
    struct http_parser preq;          /* request head scanner */
    struct http_msg    req;           /* resolved request */
    uint64_t req_body_left;           /* BF_LENGTH: body bytes still to
                                         read from client */
    uint64_t req_body_sent;           /* BF_LENGTH: body bytes already
                                         forwarded upstream (bounds the
                                         c2u drain so pipelined tail is
                                         never sent as body, M6) */
    uint8_t  req_head_sent;           /* stage fully flushed upstream */
    uint8_t  req_body_done;           /* whole request body relayed */
    uint8_t  sent_100;                /* we already sent "100 Continue" */
    uint8_t  retries;                 /* M6 idempotent retries used (max 1) */

    /* --- response side --- */
    struct http_parser presp;         /* response head scanner */
    struct http_msg    resp;          /* resolved response */
    uint64_t resp_body_left;          /* BF_LENGTH: bytes still to relay */
    struct chunk_watch cw_resp;       /* BF_CHUNKED end detection */
    uint8_t  resp_done;               /* response fully relayed */

    /* --- stream health (per side) --- */
    /* Set by read()==0 OR by an EPOLLRDHUP event without a read
     * (hazard 3).  One-shot sticky per request/conn. */
    uint8_t  client_eof;
    uint8_t  upstream_eof;
    uint8_t  upstream_writable_ever;  /* connect succeeded */

    /* --- splice pipe pair (hazard 4): -1 when not borrowed --------- */
    int pipe_r;                       /* pipe read end,  or -1 */
    int pipe_w;                       /* pipe write end, or -1 */

    /* --- keep-alive decision for the client link --- */
    uint8_t  keep_alive_ok;           /* true: loop back to CS_READ_REQ */

    /* --- buffers (see model comment above; TIERED, hazard 1) ------ */
    struct mbuf c2u;      /* allocated in conn_accept()              */
    struct mbuf u2c;      /* data==NULL until conn_alloc_buffers()   */
    struct mbuf stage;    /* data==NULL until conn_alloc_buffers()   */

    /* --- per-conn accounting / logging --- */
    uint64_t t0_ms;                   /* accept() time */
    uint64_t rx_bytes;                /* bytes received from client */
    uint64_t tx_bytes;                /* bytes sent to client */
};

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Take ownership of an accepted client fd and enter CS_ACCEPTED.
 * Registers cfd with the loop (EPOLLIN) and allocates c2u (the only
 * buffer an idle conn holds).  Returns NULL on OOM. */
struct conn *conn_accept(struct event_loop *loop, int cfd,
                         const struct conn_params *prm);

/* TIERED BUFFERS (hazard 1).  conn_alloc_buffers() must be called on
 * CS_SELECT entry, after the request head is parsed and the backend is
 * chosen: it allocates u2c + stage (c2u already exists).  Idempotent:
 * buffers that are already allocated are left alone.
 * Returns PX_OK, or PX_ERR on OOM (caller replies 500/503 and closes).
 *
 * conn_free_buffers() releases u2c + stage only -- called on the
 * keep-alive loopback so an idle keep-alive conn holds just c2u.
 * conn_close() destroys ALL buffers including c2u. */
int  conn_alloc_buffers(struct conn *c);
void conn_free_buffers(struct conn *c);

/* GRACEFUL DRAIN (hazard 6), called by the loop for every live conn
 * when loop_begin_drain() runs:
 *   - completely idle conns (CS_READ_REQ, no buffered or partial
 *     request bytes, fresh parser) are closed immediately;
 *   - conns with a request in flight (CS_SELECT..CS_RELAY) keep
 *     working but keep_alive_ok is forced to 0 (the next response head
 *     advertises Connection: close, so the client sees a clean FIN)
 *     and deadline_ms is capped at drain_deadline_ms;
 *   - conns that already hold a partial request (started before the
 *     drain) are allowed to finish that one exchange, then close. */
void conn_begin_drain(struct conn *c, int64_t drain_deadline_ms);

/* Event entry point: loop.c calls this for every (role, events) tuple.
 * Runs the state machine to quiescence (pumps until EAGAIN) and
 * re-syncs interest.  Returns the last px_result for logging. */
int conn_handle_events(struct conn *c, enum fd_role role, uint32_t events);

/* Data-path pumps, exposed for deterministic unit tests over
 * socketpairs.  Each reads/writes until EAGAIN and returns
 * PX_OK/PX_AGAIN/PX_EOF/PX_ERR.  handle_events() is a thin driver over
 * these. */
int conn_pump_read(struct conn *c, enum fd_role role);
int conn_pump_write(struct conn *c, enum fd_role role);

/* Current wanted epoll mask for one of the conn's fds. */
uint32_t conn_want(struct conn *c, enum fd_role role);

/* Schedule teardown, optionally emitting err_status to the client if
 * nothing has been sent yet.  Idempotent. */
void conn_abort(struct conn *c, int err_status);

/* Close fds (releasing any borrowed pipe pair to the pool first),
 * account to node (node_end), unregister timers, destroy all buffers
 * (incl. c2u), free. */
void conn_close(struct conn *c);

const char *conn_state_name(enum conn_state s);   /* logging / tests */

#endif /* PX_CONN_H */
