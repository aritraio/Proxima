#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "buf.h"
#include "conn.h"
#include "event.h"
#include "health.h"
#include "http.h"
#include "metrics.h"
#include "pipe_pool.h"
#include "pool.h"
#include "proxy.h"

/* Platform-neutral event testing and bitmasks */
#ifdef __linux__
#include <sys/epoll.h>
#define EV_IS_IN(ev)   ((ev) & EPOLLIN)
#define EV_IS_OUT(ev)  ((ev) & EPOLLOUT)
#define EV_IS_EOF(ev)  ((ev) & (EPOLLRDHUP | EPOLLHUP))
#define EV_IS_ERR(ev)  ((ev) & EPOLLERR)
#define PX_EV_IN       (EPOLLIN)
#define PX_EV_OUT      (EPOLLOUT)
#else
#include <poll.h>
#define EV_IS_IN(ev)   ((ev) & POLLIN)
#define EV_IS_OUT(ev)  ((ev) & POLLOUT)
#define EV_IS_EOF(ev)  ((ev) & POLLHUP)
#define EV_IS_ERR(ev)  ((ev) & (POLLERR | POLLNVAL))
#define PX_EV_IN       (POLLIN)
#define PX_EV_OUT      (POLLOUT)
#endif

/* Global pool and health checker links (defined in main.c or tests) */
struct server_pool *g_proxy_pool = NULL;
struct health_checker *g_health_checker = NULL;

static void conn_timer_cb(struct event_loop *loop, struct loop_timer *timer, void *arg);
static void conn_sync_interest(struct conn *c);
static void conn_advance_select(struct conn *c);
static int conn_flush_stage_client(struct conn *c);

const char *conn_state_name(enum conn_state s)
{
    switch (s) {
    case CS_ACCEPTED:       return "CS_ACCEPTED";
    case CS_READ_REQ:       return "CS_READ_REQ";
    case CS_SELECT:         return "CS_SELECT";
    case CS_CONNECT:        return "CS_CONNECT";
    case CS_SEND_REQ_HEAD:  return "CS_SEND_REQ_HEAD";
    case CS_RELAY:          return "CS_RELAY";
    case CS_CLOSING:        return "CS_CLOSING";
    case CS_DONE:           return "CS_DONE";
    default:                return "UNKNOWN";
    }
}

static void conn_set_timer(struct conn *c, uint32_t timeout_ms)
{
    if (c->loop == NULL) return;
    if (c->timer != NULL) {
        loop_timer_del(c->timer);
        c->timer = NULL;
    }
    c->deadline_ms = px_now_ms() + timeout_ms;
    c->timer = loop_timer_add(c->loop, c->deadline_ms, conn_timer_cb, c);
}

static void conn_timer_cb(struct event_loop *loop, struct loop_timer *timer, void *arg)
{
    struct conn *c = arg;
    (void)loop; (void)timer;

    c->timer = NULL;
    if (c->state == CS_CONNECT) {
        if (c->node) node_end(c->node, 0);
        conn_abort(c, 504);
    } else if (c->state == CS_READ_REQ) {
        conn_abort(c, 408);
    } else {
        conn_abort(c, 0);
    }
}

int conn_alloc_buffers(struct conn *c)
{
    if (c == NULL) return PX_ERR;
    if (!mbuf_is_allocated(&c->u2c)) {
        mbuf_init(&c->u2c, c->prm.buf_cap, 1);
        if (!mbuf_is_allocated(&c->u2c)) return PX_ERR;
    }
    if (!mbuf_is_allocated(&c->stage)) {
        mbuf_init(&c->stage, c->prm.stage_cap, 1);
        if (!mbuf_is_allocated(&c->stage)) return PX_ERR;
    }
    return PX_OK;
}

void conn_free_buffers(struct conn *c)
{
    if (c == NULL) return;
    mbuf_destroy(&c->u2c);
    mbuf_destroy(&c->stage);
}

struct conn *conn_accept(struct event_loop *loop, int cfd,
                         const struct conn_params *prm)
{
    struct conn *c;
    int flags;

    if (loop == NULL || cfd < 0 || prm == NULL) return NULL;

    c = calloc(1, sizeof *c);
    if (c == NULL) return NULL;

    c->loop = loop;
    c->cfd = cfd;
    c->ufd = -1;
    c->pipe_r = -1;
    c->pipe_w = -1;
    c->prm = *prm;
    if (c->prm.stage_cap < 2 * c->prm.max_head) {
        c->prm.stage_cap = 2 * c->prm.max_head;
    }
    c->t0_ms = (uint64_t)px_now_ms();
    c->state = CS_ACCEPTED;

    flags = fcntl(cfd, F_GETFL, 0);
    if (flags >= 0) fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

    mbuf_init(&c->c2u, c->prm.buf_cap, 1);
    if (!mbuf_is_allocated(&c->c2u)) {
        free(c);
        return NULL;
    }

    http_parser_init(&c->preq, HTTP_REQUEST);
    http_parser_init(&c->presp, HTTP_RESPONSE);
    chunk_watch_init(&c->cw_resp);

    metrics_conn_open(&g_metrics);

    c->state = CS_READ_REQ;
    conn_set_timer(c, c->prm.idle_timeout_ms);

    if (loop_add(loop, cfd, PX_EV_IN, c, FD_ROLE_CLIENT) != 0) {
        conn_close(c);
        return NULL;
    }
    c->want[FD_ROLE_CLIENT] = PX_EV_IN;

    return c;
}

uint32_t conn_want(struct conn *c, enum fd_role role)
{
    uint32_t w = 0;
    if (c == NULL || c->state == CS_CLOSING || c->state == CS_DONE) return 0;

    if (role == FD_ROLE_CLIENT) {
        if (c->cfd < 0) return 0;
        switch (c->state) {
        case CS_ACCEPTED:
        case CS_READ_REQ:
            if (!c->client_eof) w |= PX_EV_IN;
            break;
        case CS_SELECT:
        case CS_CONNECT:
        case CS_SEND_REQ_HEAD:
            if (!c->client_eof && !c->req_body_done && mbuf_tail_space(&c->c2u) > 0) {
                w |= PX_EV_IN;
            }
            break;
        case CS_RELAY:
            if (!c->client_eof && !c->req_body_done && mbuf_tail_space(&c->c2u) > 0) {
                w |= PX_EV_IN;
            }
            if (!mbuf_is_empty(&c->stage) || !mbuf_is_empty(&c->u2c)) {
                w |= PX_EV_OUT;
            }
            break;
        default:
            break;
        }
    } else if (role == FD_ROLE_UPSTREAM) {
        if (c->ufd < 0) return 0;
        switch (c->state) {
        case CS_CONNECT:
            w |= PX_EV_OUT;
            break;
        case CS_SEND_REQ_HEAD:
            w |= PX_EV_OUT;
            break;
        case CS_RELAY:
            if (!mbuf_is_empty(&c->c2u)) {
                w |= PX_EV_OUT;
            }
            if (!c->upstream_eof && !c->resp_done && mbuf_tail_space(&c->u2c) > 0) {
                w |= PX_EV_IN;
            }
            break;
        default:
            break;
        }
    }
    return w;
}

static void conn_sync_interest(struct conn *c)
{
    uint32_t wc, wu;
    if (c == NULL || c->loop == NULL) return;

    if (c->cfd >= 0) {
        wc = conn_want(c, FD_ROLE_CLIENT);
        if (wc != c->want[FD_ROLE_CLIENT]) {
            loop_mod(c->loop, c->cfd, wc);
            c->want[FD_ROLE_CLIENT] = wc;
        }
    }
    if (c->ufd >= 0) {
        wu = conn_want(c, FD_ROLE_UPSTREAM);
        if (wu != c->want[FD_ROLE_UPSTREAM]) {
            loop_mod(c->loop, c->ufd, wu);
            c->want[FD_ROLE_UPSTREAM] = wu;
        }
    }
}

void conn_abort(struct conn *c, int err_status)
{
    if (c == NULL || c->state == CS_CLOSING || c->state == CS_DONE) return;

    c->err_status = (uint16_t)err_status;
    if (err_status > 0 && c->tx_bytes == 0 && c->cfd >= 0) {
        char reply[PX_ERR_REPLY_CAP];
        int n = http_build_error_reply(reply, sizeof reply, err_status, 0);
        if (n > 0) {
            (void)conn_alloc_buffers(c);
            if (mbuf_is_allocated(&c->stage)) {
                mbuf_reset(&c->stage);
                (void)mbuf_append(&c->stage, reply, (size_t)n);
                (void)conn_flush_stage_client(c);
            }
        }
        metrics_record_status(&g_metrics, err_status);
    } else if (err_status == 0) {
        metrics_record_internal_error(&g_metrics);
    }

    conn_close(c);
}

void conn_close(struct conn *c)
{
    if (c == NULL || c->state == CS_DONE) return;

    c->state = CS_CLOSING;
    if (c->timer != NULL) {
        loop_timer_del(c->timer);
        c->timer = NULL;
    }
    if (c->cfd >= 0) {
        loop_del(c->loop, c->cfd);
        close(c->cfd);
        c->cfd = -1;
    }
    if (c->ufd >= 0) {
        loop_del(c->loop, c->ufd);
        close(c->ufd);
        c->ufd = -1;
    }
    if (c->pipe_r >= 0) {
        pipe_pool_release(&g_pipe_pool, c->pipe_r, c->pipe_w);
        c->pipe_r = -1;
        c->pipe_w = -1;
    }
    if (c->node != NULL) {
        node_end(c->node, 0);
        c->node = NULL;
    }

    mbuf_destroy(&c->c2u);
    mbuf_destroy(&c->u2c);
    mbuf_destroy(&c->stage);

    metrics_conn_close(&g_metrics);

    c->state = CS_DONE;
    free(c);
}

void conn_begin_drain(struct conn *c, int64_t drain_deadline_ms)
{
    if (c == NULL || c->state == CS_CLOSING || c->state == CS_DONE) return;

    if (c->state == CS_READ_REQ && mbuf_is_empty(&c->c2u) && c->preq.pos == 0) {
        /* Idle connection drops immediately per RFC 7230 §6.3 */
        conn_close(c);
        return;
    }

    c->keep_alive_ok = 0;
    if (c->deadline_ms == 0 || drain_deadline_ms < c->deadline_ms) {
        c->deadline_ms = drain_deadline_ms;
        if (c->timer != NULL) {
            loop_timer_del(c->timer);
            c->timer = loop_timer_add(c->loop, c->deadline_ms, conn_timer_cb, c);
        }
    }
}

int conn_pump_read(struct conn *c, enum fd_role role)
{
    int fd;
    struct mbuf *buf;
    size_t max_read;

    if (c == NULL) return PX_ERR;
    if (role == FD_ROLE_CLIENT) {
        fd = c->cfd;
        buf = &c->c2u;
        max_read = 16384;
        if (c->state == CS_RELAY && c->req.framing == BF_LENGTH) {
            if (c->req_body_left < max_read) max_read = (size_t)c->req_body_left;
        }
    } else {
        fd = c->ufd;
        buf = &c->u2c;
        max_read = 16384;
    }
    if (fd < 0 || max_read == 0) return PX_OK;

    while (1) {
        ssize_t n;
        if (mbuf_reserve_append(buf, max_read) != PX_OK) return PX_ERR;

        n = read(fd, mbuf_tail(buf), max_read);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return PX_AGAIN;
            c->saved_errno = errno;
            return PX_ERR;
        }
        if (n == 0) {
            if (role == FD_ROLE_CLIENT) c->client_eof = 1;
            else c->upstream_eof = 1;
            return PX_EOF;
        }

        mbuf_commit(buf, (size_t)n);
        if (role == FD_ROLE_CLIENT) {
            c->rx_bytes += (size_t)n;
            metrics_record_bytes(&g_metrics, (size_t)n, 0);
            if (c->state == CS_RELAY && c->req.framing == BF_LENGTH) {
                c->req_body_left -= (size_t)n;
                if (c->req_body_left == 0) {
                    c->req_body_done = 1;
                    return PX_OK;
                }
                if (c->req_body_left < max_read) max_read = (size_t)c->req_body_left;
            }
        }
    }
}

static int conn_flush_stage_client(struct conn *c)
{
    while (!mbuf_is_empty(&c->stage)) {
        ssize_t n = write(c->cfd, mbuf_head(&c->stage), mbuf_len(&c->stage));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return PX_AGAIN;
            c->saved_errno = errno;
            return PX_ERR;
        }
        mbuf_consume(&c->stage, (size_t)n);
        c->tx_bytes += (size_t)n;
        metrics_record_bytes(&g_metrics, 0, (size_t)n);
    }
    return PX_OK;
}

int conn_pump_write(struct conn *c, enum fd_role role)
{
    if (c == NULL) return PX_ERR;

    if (role == FD_ROLE_CLIENT) {
        if (c->cfd < 0) return PX_OK;
        if (!mbuf_is_empty(&c->stage)) {
            int rc = conn_flush_stage_client(c);
            if (rc != PX_OK) return rc;
        }
        while (!mbuf_is_empty(&c->u2c)) {
            ssize_t n = write(c->cfd, mbuf_head(&c->u2c), mbuf_len(&c->u2c));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return PX_AGAIN;
                c->saved_errno = errno;
                return PX_ERR;
            }
            mbuf_consume(&c->u2c, (size_t)n);
            c->tx_bytes += (size_t)n;
            metrics_record_bytes(&g_metrics, 0, (size_t)n);
        }
        return PX_OK;
    } else if (role == FD_ROLE_UPSTREAM) {
        if (c->ufd < 0) return PX_OK;
        if (!mbuf_is_empty(&c->stage)) {
            while (!mbuf_is_empty(&c->stage)) {
                ssize_t n = write(c->ufd, mbuf_head(&c->stage), mbuf_len(&c->stage));
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) return PX_AGAIN;
                    c->saved_errno = errno;
                    return PX_ERR;
                }
                mbuf_consume(&c->stage, (size_t)n);
            }
            c->req_head_sent = 1;
        }
        while (!mbuf_is_empty(&c->c2u)) {
            ssize_t n = write(c->ufd, mbuf_head(&c->c2u), mbuf_len(&c->c2u));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return PX_AGAIN;
                c->saved_errno = errno;
                return PX_ERR;
            }
            mbuf_consume(&c->c2u, (size_t)n);
        }
        return PX_OK;
    }
    return PX_OK;
}

static void rewrite_request_head(struct conn *c)
{
    const uint8_t *base = mbuf_head(&c->c2u);
    char line[512];
    size_t i;

    mbuf_reset(&c->stage);

    /* Method target version */
    snprintf(line, sizeof line, "%.*s %.*s HTTP/1.1\r\n",
             (int)c->preq.method.len, (const char *)base + c->preq.method.off,
             (int)c->preq.target.len, (const char *)base + c->preq.target.off);
    mbuf_append(&c->stage, line, strlen(line));

    /* Headers */
    for (i = 0; i < c->preq.nheaders; i++) {
        const char *name = (const char *)base + c->preq.hname[i].off;
        size_t nlen = c->preq.hname[i].len;
        const char *val = (const char *)base + c->preq.hvalue[i].off;
        size_t vlen = c->preq.hvalue[i].len;

        if (strncasecmp(name, "Connection", nlen) == 0 ||
            strncasecmp(name, "Keep-Alive", nlen) == 0 ||
            strncasecmp(name, "Proxy-Connection", nlen) == 0 ||
            strncasecmp(name, "Upgrade", nlen) == 0 ||
            strncasecmp(name, "Expect", nlen) == 0) {
            continue;
        }
        mbuf_append(&c->stage, name, nlen);
        mbuf_append(&c->stage, ": ", 2);
        mbuf_append(&c->stage, val, vlen);
        mbuf_append(&c->stage, "\r\n", 2);
    }

    /* Enforce upstream Connection: close */
    mbuf_append(&c->stage, "Connection: close\r\n", 19);
    mbuf_append(&c->stage, "X-Forwarded-For: 127.0.0.1\r\n\r\n", 30);
}

static void rewrite_response_head(struct conn *c)
{
    const uint8_t *base = mbuf_head(&c->u2c);
    char line[512];
    size_t i;

    mbuf_reset(&c->stage);

    snprintf(line, sizeof line, "HTTP/1.1 %d %.*s\r\n",
             c->resp.status,
             (int)c->presp.reason.len, (const char *)base + c->presp.reason.off);
    mbuf_append(&c->stage, line, strlen(line));

    for (i = 0; i < c->presp.nheaders; i++) {
        const char *name = (const char *)base + c->presp.hname[i].off;
        size_t nlen = c->presp.hname[i].len;
        const char *val = (const char *)base + c->presp.hvalue[i].off;
        size_t vlen = c->presp.hvalue[i].len;

        if (strncasecmp(name, "Connection", nlen) == 0 ||
            strncasecmp(name, "Keep-Alive", nlen) == 0 ||
            strncasecmp(name, "Proxy-Connection", nlen) == 0) {
            continue;
        }
        mbuf_append(&c->stage, name, nlen);
        mbuf_append(&c->stage, ": ", 2);
        mbuf_append(&c->stage, val, vlen);
        mbuf_append(&c->stage, "\r\n", 2);
    }

    if (c->keep_alive_ok) {
        mbuf_append(&c->stage, "Connection: keep-alive\r\n\r\n", 26);
    } else {
        mbuf_append(&c->stage, "Connection: close\r\n\r\n", 21);
    }
}

static void handle_request_done(struct conn *c)
{
    if (c->node != NULL) {
        node_end(c->node, 1);
        c->node = NULL;
    }
    if (c->ufd >= 0) {
        loop_del(c->loop, c->ufd);
        close(c->ufd);
        c->ufd = -1;
    }
    if (c->pipe_r >= 0) {
        pipe_pool_release(&g_pipe_pool, c->pipe_r, c->pipe_w);
        c->pipe_r = -1;
        c->pipe_w = -1;
    }

    if (c->keep_alive_ok && loop_phase(c->loop) == LOOP_RUNNING) {
        /* Keep-alive loopback */
        conn_free_buffers(c);
        http_parser_init(&c->preq, HTTP_REQUEST);
        http_parser_init(&c->presp, HTTP_RESPONSE);
        chunk_watch_init(&c->cw_resp);
        memset(&c->req, 0, sizeof c->req);
        memset(&c->resp, 0, sizeof c->resp);
        c->req_body_left = 0;
        c->req_head_sent = 0;
        c->req_body_done = 0;
        c->resp_body_left = 0;
        c->resp_done = 0;
        c->sent_100 = 0;
        c->upstream_eof = 0;
        c->upstream_writable_ever = 0;
        c->err_status = 0;

        mbuf_compact(&c->c2u);
        c->state = CS_READ_REQ;
        conn_set_timer(c, c->prm.idle_timeout_ms);
        conn_sync_interest(c);

        if (mbuf_len(&c->c2u) > 0) {
            /* Pipelined data already in buffer */
            enum http_parse_status st = http_parse(&c->preq, mbuf_head(&c->c2u),
                                                   mbuf_len(&c->c2u), c->prm.max_head);
            if (st == HPS_DONE) conn_advance_select(c);
        }
    } else {
        conn_close(c);
    }
}

static void conn_advance_select(struct conn *c)
{
    struct server_node *node;
    int flags;

    if (!http_resolve(&c->req, &c->preq, HTTP_M_UNKNOWN)) {
        conn_abort(c, c->preq.err_status ? c->preq.err_status : 400);
        return;
    }

    if (c->req.framing == BF_CHUNKED) {
        conn_abort(c, 501);
        return;
    }

    c->keep_alive_ok = c->req.keep_alive;
    if (loop_phase(c->loop) != LOOP_RUNNING) c->keep_alive_ok = 0;
    metrics_record_request(&g_metrics);

    /* Check internal metrics route */
    if (c->req.method == HTTP_M_GET &&
        c->preq.target.len == PX_METRICS_PATH_LEN &&
        memcmp(mbuf_head(&c->c2u) + c->preq.target.off, PX_METRICS_PATH, PX_METRICS_PATH_LEN) == 0) {
        char body[4096];
        char head[256];
        size_t blen = metrics_render(body, sizeof body);
        int hlen = snprintf(head, sizeof head,
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/plain\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: %s\r\n\r\n",
                            blen, c->keep_alive_ok ? "keep-alive" : "close");
        (void)conn_alloc_buffers(c);
        mbuf_reset(&c->stage);
        mbuf_append(&c->stage, head, (size_t)hlen);
        mbuf_append(&c->stage, body, blen);
        mbuf_consume(&c->c2u, c->req.head_len);
        metrics_record_status(&g_metrics, 200);
        c->resp_done = 1;
        c->state = CS_RELAY;
        (void)conn_flush_stage_client(c);
        if (mbuf_is_empty(&c->stage)) handle_request_done(c);
        else conn_sync_interest(c);
        return;
    }

    if (conn_alloc_buffers(c) != PX_OK) {
        conn_abort(c, 500);
        return;
    }

    rewrite_request_head(c);
    mbuf_consume(&c->c2u, c->req.head_len);

    if (c->req.expect_continue && !c->sent_100) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        (void)write(c->cfd, cont, sizeof cont - 1);
        c->sent_100 = 1;
    }

    if (c->req.framing == BF_LENGTH) {
        c->req_body_left = c->req.body_len;
        if (mbuf_len(&c->c2u) >= c->req_body_left) {
            c->req_body_done = 1;
        } else {
            c->req_body_left -= mbuf_len(&c->c2u);
        }
    } else {
        c->req_body_left = 0;
        c->req_body_done = 1;
    }

    c->state = CS_SELECT;
    node = pool_pick(g_proxy_pool);
    if (node == NULL) {
        conn_abort(c, 503);
        return;
    }

    c->node = node;
    node_begin(c->node);
    memcpy(&c->uaddr, &c->node->addr, c->node->addrlen);
    c->uaddr_len = c->node->addrlen;

    c->ufd = socket(c->uaddr.ss_family, SOCK_STREAM, 0);
    if (c->ufd < 0) {
        node_end(c->node, 0);
        conn_abort(c, 502);
        return;
    }

    flags = fcntl(c->ufd, F_GETFL, 0);
    if (flags >= 0) fcntl(c->ufd, F_SETFL, flags | O_NONBLOCK);

    if (connect(c->ufd, (struct sockaddr *)&c->uaddr, c->uaddr_len) == 0) {
        c->upstream_writable_ever = 1;
        c->state = CS_SEND_REQ_HEAD;
        if (loop_add(c->loop, c->ufd, PX_EV_OUT, c, FD_ROLE_UPSTREAM) != 0) {
            conn_abort(c, 502);
            return;
        }
        c->want[FD_ROLE_UPSTREAM] = PX_EV_OUT;
        (void)conn_pump_write(c, FD_ROLE_UPSTREAM);
        if (mbuf_is_empty(&c->stage)) c->state = CS_RELAY;
    } else if (errno == EINPROGRESS) {
        c->state = CS_CONNECT;
        conn_set_timer(c, c->prm.connect_timeout_ms);
        if (loop_add(c->loop, c->ufd, PX_EV_OUT, c, FD_ROLE_UPSTREAM) != 0) {
            conn_abort(c, 502);
            return;
        }
        c->want[FD_ROLE_UPSTREAM] = PX_EV_OUT;
    } else {
        if (g_health_checker) health_on_passive_failure(g_health_checker, c->node);
        node_end(c->node, 0);
        conn_abort(c, 502);
        return;
    }

    conn_sync_interest(c);
}

int conn_handle_events(struct conn *c, enum fd_role role, uint32_t events)
{
    if (c == NULL) return PX_ERR;

    /* Handle client events */
    if (role == FD_ROLE_CLIENT) {
        if (EV_IS_ERR(events)) {
            if (c->node) node_end(c->node, 0);
            conn_close(c);
            return PX_ERR;
        }
        if (EV_IS_EOF(events)) {
            c->client_eof = 1;
            if (c->state == CS_READ_REQ && mbuf_is_empty(&c->c2u)) {
                conn_close(c);
                return PX_EOF;
            }
        }
        if (EV_IS_IN(events)) {
            if (c->state == CS_READ_REQ) {
                int r = conn_pump_read(c, FD_ROLE_CLIENT);
                if (r == PX_ERR) {
                    conn_abort(c, 400);
                    return PX_ERR;
                }
                if (mbuf_len(&c->c2u) > c->prm.max_head) {
                    conn_abort(c, 431);
                    return PX_ERR;
                }
                {
                    enum http_parse_status st = http_parse(&c->preq, mbuf_head(&c->c2u),
                                                           mbuf_len(&c->c2u), c->prm.max_head);
                    if (st == HPS_DONE) {
                        conn_advance_select(c);
                        return PX_OK;
                    } else if (st == HPS_ERROR) {
                        conn_abort(c, c->preq.err_status ? c->preq.err_status : 400);
                        return PX_ERR;
                    }
                }
            } else if (c->state == CS_RELAY) {
                (void)conn_pump_read(c, FD_ROLE_CLIENT);
                (void)conn_pump_write(c, FD_ROLE_UPSTREAM);
            }
        }
        if (EV_IS_OUT(events)) {
            (void)conn_pump_write(c, FD_ROLE_CLIENT);
            if (c->resp_done && mbuf_is_empty(&c->stage) && mbuf_is_empty(&c->u2c)) {
                handle_request_done(c);
                return PX_OK;
            }
        }
    } else if (role == FD_ROLE_UPSTREAM) {
        /* Upstream events */
        if (c->state == CS_CONNECT) {
            int err = 0;
            socklen_t elen = sizeof err;
            if (getsockopt(c->ufd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
                if (g_health_checker && c->node) health_on_passive_failure(g_health_checker, c->node);
                if (c->node) node_end(c->node, 0);
                conn_abort(c, 502);
                return PX_ERR;
            }
            metrics_record_upstream_connect(&g_metrics, (uint64_t)(px_now_ms() - (int64_t)c->t0_ms));
            c->upstream_writable_ever = 1;
            c->state = CS_SEND_REQ_HEAD;
            conn_set_timer(c, c->prm.idle_timeout_ms);
            (void)conn_pump_write(c, FD_ROLE_UPSTREAM);
            if (mbuf_is_empty(&c->stage)) c->state = CS_RELAY;
            conn_sync_interest(c);
            return PX_OK;
        }

        if (EV_IS_ERR(events)) {
            if (g_health_checker && c->node) health_on_passive_failure(g_health_checker, c->node);
            if (c->node) node_end(c->node, 0);
            conn_abort(c, c->tx_bytes == 0 ? 502 : 0);
            return PX_ERR;
        }

        if (EV_IS_IN(events)) {
            (void)conn_pump_read(c, FD_ROLE_UPSTREAM);
            if (!c->resp.head_len) {
                enum http_parse_status st = http_parse(&c->presp, mbuf_head(&c->u2c),
                                                       mbuf_len(&c->u2c), c->prm.max_head);
                if (st == HPS_DONE) {
                    if (c->presp.status >= 100 && c->presp.status < 200) {
                        /* Interim 1xx response discarded */
                        mbuf_consume(&c->u2c, c->presp.pos);
                        http_parser_init(&c->presp, HTTP_RESPONSE);
                    } else {
                        http_resolve(&c->resp, &c->presp, c->req.method);
                        metrics_record_status(&g_metrics, c->resp.status);
                        rewrite_response_head(c);
                        mbuf_consume(&c->u2c, c->resp.head_len);

                        if (c->resp.framing == BF_NONE) {
                            c->resp_done = 1;
                        } else if (c->resp.framing == BF_LENGTH) {
                            c->resp_body_left = c->resp.body_len;
                            if (mbuf_len(&c->u2c) >= c->resp_body_left) {
                                c->resp_done = 1;
                            } else {
                                c->resp_body_left -= mbuf_len(&c->u2c);
                            }
                        } else if (c->resp.framing == BF_CHUNKED) {
                            chunk_watch_init(&c->cw_resp);
                            if (mbuf_len(&c->u2c) > 0) {
                                enum chunk_feed_result cr = chunk_watch_feed(&c->cw_resp,
                                                                             mbuf_head(&c->u2c),
                                                                             mbuf_len(&c->u2c));
                                if (cr == CHF_END) c->resp_done = 1;
                            }
                        }
                    }
                } else if (st == HPS_ERROR) {
                    conn_abort(c, 502);
                    return PX_ERR;
                }
            } else {
                if (c->resp.framing == BF_LENGTH) {
                    if (mbuf_len(&c->u2c) >= c->resp_body_left) c->resp_done = 1;
                } else if (c->resp.framing == BF_CHUNKED) {
                    enum chunk_feed_result cr = chunk_watch_feed(&c->cw_resp,
                                                                 mbuf_head(&c->u2c),
                                                                 mbuf_len(&c->u2c));
                    if (cr == CHF_END) c->resp_done = 1;
                }
            }
            (void)conn_pump_write(c, FD_ROLE_CLIENT);
        }

        if (EV_IS_OUT(events)) {
            (void)conn_pump_write(c, FD_ROLE_UPSTREAM);
        }

        if (EV_IS_EOF(events)) {
            c->upstream_eof = 1;
            if (c->resp.framing == BF_UNTIL_EOF) {
                c->resp_done = 1;
            } else if (c->resp.framing == BF_LENGTH && c->resp_body_left > 0 && !c->resp_done) {
                /* Truncated response from upstream */
                conn_abort(c, c->tx_bytes == 0 ? 502 : 0);
                return PX_ERR;
            }
            (void)conn_pump_write(c, FD_ROLE_CLIENT);
        }

        if (c->resp_done && mbuf_is_empty(&c->stage) && mbuf_is_empty(&c->u2c)) {
            handle_request_done(c);
            return PX_OK;
        }
    }

    conn_sync_interest(c);
    return PX_OK;
}
