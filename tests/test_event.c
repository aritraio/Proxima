#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#define TEST_IN EPOLLIN
#else
#include <poll.h>
#define TEST_IN POLLIN
#endif

#include "conn.h"
#include "event.h"

static int drain_calls;

/* conn.c arrives in M3.  The reactor only needs this drain hook in M2. */
void conn_begin_drain(struct conn *c, int64_t deadline_ms)
{
    (void)c;
    (void)deadline_ms;
    drain_calls++;
}

struct event_test {
    int calls;
    uint32_t events;
    int order[3];
    int norder;
};

static struct event_test *active_test;

static void on_conn(struct event_loop *loop, struct conn *c,
                    enum fd_role role, uint32_t events)
{
    struct event_test *t = active_test;
    char buf[32];
    (void)role;
    (void)read(c->cfd, buf, sizeof buf);
    t->calls++;
    t->events = events;
    loop_stop(loop);
}

static void on_timer(struct event_loop *loop, struct loop_timer *timer, void *arg)
{
    struct event_test *t = arg;
    (void)timer;
    t->order[t->norder] = t->norder + 1;
    t->norder++;
    if (t->norder == 2) loop_stop(loop);
}

static void test_socket_event(void)
{
    struct event_loop *loop = loop_new(4);
    struct conn c = {0};
    struct event_test t = {0};
    int fd[2];
    assert(loop != NULL && socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    c.cfd = fd[0];
    active_test = &t;
    loop_set_conn_cb(loop, on_conn);
    assert(loop_add(loop, fd[0], TEST_IN, &c, FD_ROLE_CLIENT) == 0);
    assert(write(fd[1], "x", 1) == 1);
    assert(loop_run(loop) == 0);
    assert(t.calls == 1 && (t.events & TEST_IN) != 0);
    assert(loop_del(loop, fd[0]) == 0);
    close(fd[0]); close(fd[1]); loop_free(loop);
}

static void test_timers(void)
{
    struct event_loop *loop = loop_new(2);
    struct event_test t = {0};
    struct loop_timer *cancelled;
    int64_t now = px_now_ms();
    assert(loop != NULL);
    assert(loop_timer_add(loop, now + 4, on_timer, &t) != NULL);
    cancelled = loop_timer_add(loop, now + 2, on_timer, &t);
    assert(cancelled != NULL);
    loop_timer_del(cancelled);
    assert(loop_timer_add(loop, now + 1, on_timer, &t) != NULL);
    assert(loop_run(loop) == 0);
    assert(t.norder == 2 && t.order[0] == 1 && t.order[1] == 2);
    loop_timer_del(cancelled);
    loop_free(loop);
}

static void test_drain(void)
{
    struct event_loop *loop = loop_new(2);
    struct conn c = {0};
    int lfd[2], cfd[2];
    assert(loop != NULL && socketpair(AF_UNIX, SOCK_STREAM, 0, lfd) == 0);
    assert(loop_add_listener(loop, lfd[0], NULL) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, cfd) == 0);
    c.cfd = cfd[0];
    assert(loop_add(loop, cfd[0], TEST_IN, &c, FD_ROLE_CLIENT) == 0);
    drain_calls = 0;
    loop_begin_drain(loop, 20);
    assert(loop_phase(loop) == LOOP_DRAINING && drain_calls == 1);
    assert(loop_run(loop) == 0 && loop_phase(loop) == LOOP_STOPPED);
    assert(loop_del(loop, cfd[0]) == 0);
    close(cfd[0]); close(cfd[1]); close(lfd[1]); loop_free(loop);

    loop = loop_new(2);
    assert(loop != NULL && socketpair(AF_UNIX, SOCK_STREAM, 0, lfd) == 0);
    assert(loop_add_listener(loop, lfd[0], NULL) == 0);
    loop_begin_drain(loop, 20);
    assert(loop_phase(loop) == LOOP_STOPPED);
    assert(fcntl(lfd[0], F_GETFD) == -1);
    assert(loop_run(loop) == 0);
    close(lfd[1]); loop_free(loop);
}

#ifdef __linux__
static void on_rdhup(struct event_loop *loop, struct conn *c,
                     enum fd_role role, uint32_t events)
{
    struct event_test *t = active_test;
    (void)role;
    t->calls++;
    t->events = events;
    loop_stop(loop);
}

static void test_rdhup(void)
{
    struct event_loop *loop = loop_new(2);
    struct conn c = {0};
    struct event_test t = {0};
    int fd[2];
    assert(loop != NULL && socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    c.cfd = fd[0];
    active_test = &t;
    loop_set_conn_cb(loop, on_rdhup);
    assert(loop_add(loop, fd[0], EPOLLIN, &c, FD_ROLE_CLIENT) == 0);
    assert(shutdown(fd[1], SHUT_WR) == 0);
    assert(loop_run(loop) == 0);
    assert(t.calls == 1 && (t.events & EPOLLRDHUP) != 0);
    assert(loop_del(loop, fd[0]) == 0);
    close(fd[0]); close(fd[1]); loop_free(loop);
}
#endif

int main(void)
{
    test_socket_event();
    test_timers();
    test_drain();
#ifdef __linux__
    test_rdhup();
#endif
    printf("test_event: ALL PASS\n");
    return 0;
}
