#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "buf.h"
#include "conn.h"
#include "event.h"
#include "metrics.h"
#include "pool.h"

extern struct server_pool *g_proxy_pool;

/* Deterministic loopback helpers: never block indefinitely on accept/read.
 * The proxy uses non-blocking sockets; the mock backend must do the same
 * so `make check` cannot hang on TCP handshake timing. */
static int set_nonblocking_tpl(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int wait_readable_tpl(int fd, int timeout_ms)
{
    struct pollfd pfd;
    int r;
    if (fd < 0) return 0;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    r = poll(&pfd, 1, timeout_ms);
    return (r > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0);
}

static int accept_nonblocking_tpl(int lfd)
{
    /* Poll for a pending connection (loopback connect is async for a
     * non-blocking proxy ufd) then accept without blocking. Retries
     * for up to ~2s; returns -1 on timeout. */
    int i;
    for (i = 0; i < 400; i++) {
        int bfd;
        if (wait_readable_tpl(lfd, 5)) {
            bfd = accept(lfd, NULL, NULL);
            if (bfd >= 0) return bfd;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        } else {
            /* No edge yet: still try a non-blocking accept in case
             * poll missed the edge, then sleep briefly. */
            bfd = accept(lfd, NULL, NULL);
            if (bfd >= 0) return bfd;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != 0) {
                /* poll timed out (0) -> accept raises EAGAIN; keep waiting */
                if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
            }
        }
        usleep(5000);
    }
    return -1;
}

static ssize_t read_with_timeout_tpl(int fd, char *buf, size_t cap)
{
    /* Wait up to ~2s for data, then do a single read. Never blocks
     * indefinitely: returns -1 on timeout. */
    int i;
    for (i = 0; i < 400; i++) {
        ssize_t n;
        if (!wait_readable_tpl(fd, 5)) {
            usleep(5000);
            continue;
        }
        n = read(fd, buf, cap);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(5000);
            continue;
        }
        return n;
    }
    errno = ETIMEDOUT;
    return -1;
}

static void on_conn_event(struct event_loop *loop, struct conn *c,
                          enum fd_role role, uint32_t events)
{
    (void)loop;
    conn_handle_events(c, role, events);
}

static void test_tiered_allocation_and_metrics_route(void)
{
    struct event_loop *loop = loop_new(16);
    struct conn_params prm = {
        .max_head = 4096,
        .buf_cap = 4096,
        .stage_cap = 8192,
        .connect_timeout_ms = 1000,
        .idle_timeout_ms = 5000,
    };
    int cfd[2];
    struct conn *c;
    char read_buf[2048];
    ssize_t n;

    metrics_reset(&g_metrics);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, cfd) == 0);

    c = conn_accept(loop, cfd[0], &prm);
    assert(c != NULL);
    assert(c->state == CS_READ_REQ);
    assert(mbuf_is_allocated(&c->c2u));
    /* Tiered allocation contract (Hazard 1): u2c and stage NOT allocated on accept */
    assert(!mbuf_is_allocated(&c->u2c));
    assert(!mbuf_is_allocated(&c->stage));
    assert(g_metrics.active_conns == 1);

    /* Send internal metrics request */
    assert(write(cfd[1], "GET /_proxima/metrics HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", 67) == 67);

    /* Drive event loop */
    loop_set_conn_cb(loop, on_conn_event);
    conn_handle_events(c, FD_ROLE_CLIENT, 0x1 /* IN */);

    /* Client receives 200 text/plain metrics response */
    n = read(cfd[1], read_buf, sizeof read_buf - 1);
    assert(n > 0);
    read_buf[n] = '\0';
    assert(strstr(read_buf, "200 OK") != NULL);
    assert(strstr(read_buf, "pxlb_requests_total") != NULL);

    close(cfd[1]);
    loop_free(loop);
}

static void test_errors_400_and_503(void)
{
    struct event_loop *loop = loop_new(16);
    struct conn_params prm = {
        .max_head = 64,
        .buf_cap = 1024,
        .stage_cap = 2048,
        .connect_timeout_ms = 1000,
        .idle_timeout_ms = 5000,
    };
    int cfd[2];
    struct conn *c;
    char read_buf[1024];
    ssize_t n;

    /* 1. Test 400 Bad Request on garbage input */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, cfd) == 0);
    c = conn_accept(loop, cfd[0], &prm);
    assert(c != NULL);
    assert(write(cfd[1], "BAD REQUEST LINE\r\n\r\n", 20) == 20);
    conn_handle_events(c, FD_ROLE_CLIENT, 0x1);
    n = read(cfd[1], read_buf, sizeof read_buf - 1);
    assert(n > 0);
    read_buf[n] = '\0';
    assert(strstr(read_buf, "400 Bad Request") != NULL);
    close(cfd[1]);

    /* 2. Test 431 on oversized header */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, cfd) == 0);
    c = conn_accept(loop, cfd[0], &prm);
    assert(c != NULL);
    assert(write(cfd[1], "GET / HTTP/1.1\r\nX-Long-Header: this_header_is_longer_than_max_head_limit_allowed\r\n\r\n", 84) == 84);
    conn_handle_events(c, FD_ROLE_CLIENT, 0x1);
    n = read(cfd[1], read_buf, sizeof read_buf - 1);
    assert(n > 0);
    read_buf[n] = '\0';
    assert(strstr(read_buf, "431 Request Header Fields Too Large") != NULL);
    close(cfd[1]);

    /* 3. Test 503 when no backend is UP */
    struct server_pool pool;
    pool_init(&pool, 0, BAL_ROUND_ROBIN);
    g_proxy_pool = &pool;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, cfd) == 0);
    c = conn_accept(loop, cfd[0], &prm);
    assert(c != NULL);
    assert(write(cfd[1], "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n", 37) == 37);
    conn_handle_events(c, FD_ROLE_CLIENT, 0x1);
    n = read(cfd[1], read_buf, sizeof read_buf - 1);
    assert(n > 0);
    read_buf[n] = '\0';
    assert(strstr(read_buf, "503 Service Unavailable") != NULL);
    close(cfd[1]);
    pool_free(&pool);

    loop_free(loop);
}

static void test_full_proxy_exchange(void)
{
    struct event_loop *loop = loop_new(16);
    struct conn_params prm = {
        .max_head = 4096,
        .buf_cap = 4096,
        .stage_cap = 8192,
        .connect_timeout_ms = 2000,
        .idle_timeout_ms = 5000,
    };
    struct server_pool pool;
    char err[128];
    int lfd, cfd[2], bfd;
    struct sockaddr_in saddr;
    socklen_t slen = sizeof saddr;
    uint16_t port;
    struct conn *c;
    char buf[1024];
    ssize_t n;

    /* 1. Setup mock backend TCP listener on ephemeral port.
     * Non-blocking so accept() below can never hang the test. */
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(lfd >= 0);
    memset(&saddr, 0, sizeof saddr);
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    saddr.sin_port = 0;
    assert(bind(lfd, (struct sockaddr *)&saddr, sizeof saddr) == 0);
    assert(listen(lfd, 4) == 0);
    assert(getsockname(lfd, (struct sockaddr *)&saddr, &slen) == 0);
    port = ntohs(saddr.sin_port);
    assert(set_nonblocking_tpl(lfd) == 0);

    /* 2. Setup server pool pointing to this mock backend */
    assert(pool_init(&pool, 1, BAL_ROUND_ROBIN) == 0);
    assert(pool_add(&pool, "127.0.0.1", port, 1, err, sizeof err) == 0);
    g_proxy_pool = &pool;

    /* 3. Accept client connection */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, cfd) == 0);
    c = conn_accept(loop, cfd[0], &prm);
    assert(c != NULL);

    /* 4. Client sends request */
    assert(write(cfd[1], "GET /hello HTTP/1.1\r\nHost: example.com\r\n\r\n", 42) == 42);

    /* 5. Dispatch client read event (drives non-blocking connect). */
    conn_handle_events(c, FD_ROLE_CLIENT, 0x1);

    /* 6. Accept upstream connection on mock backend. The proxy's connect()
     * is non-blocking, so the SYN may not have arrived yet: poll + retry
     * instead of a blocking accept() that would hang `make check`. */
    bfd = accept_nonblocking_tpl(lfd);
    assert(bfd >= 0);

    /* 7. Proxy connects & flushes request head upstream */
    conn_handle_events(c, FD_ROLE_UPSTREAM, 0x4 /* OUT */);
    n = read_with_timeout_tpl(bfd, buf, sizeof buf - 1);
    assert(n > 0);
    buf[n] = '\0';
    assert(strstr(buf, "GET /hello HTTP/1.1") != NULL);
    assert(strstr(buf, "Connection: close") != NULL);
    assert(strstr(buf, "X-Forwarded-For: 127.0.0.1") != NULL);

    /* 8. Backend responds with 200 OK + payload */
    assert(write(bfd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nworld", 43) == 43);

    /* 9. Upstream readable on proxy. Wait until the proxy's upstream fd
     * actually has the response bytes (loopback delivery is async) so the
     * pump sees data instead of EAGAIN and the test stays deterministic. */
    assert(c->ufd >= 0);
    assert(wait_readable_tpl(c->ufd, 2000));
    conn_handle_events(c, FD_ROLE_UPSTREAM, 0x1 /* IN */);

    /* 10. Client reads response (poll first: never block forever). */
    n = read_with_timeout_tpl(cfd[1], buf, sizeof buf - 1);
    assert(n > 0);
    buf[n] = '\0';
    assert(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(buf, "world") != NULL);
    assert(strstr(buf, "Connection: keep-alive") != NULL);

    /* 11. Verify keep-alive loopback succeeded: connection is back in CS_READ_REQ */
    assert(c->state == CS_READ_REQ);
    assert(!mbuf_is_allocated(&c->u2c)); /* Tiered buffers freed */
    assert(!mbuf_is_allocated(&c->stage));

    close(bfd);
    close(cfd[1]);
    close(lfd);
    pool_free(&pool);
    loop_free(loop);
}

int main(void)
{
    test_tiered_allocation_and_metrics_route();
    test_errors_400_and_503();
    test_full_proxy_exchange();
    printf("test_conn: ALL PASS\n");
    return 0;
}
