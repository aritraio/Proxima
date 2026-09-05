#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#endif

#include "conn.h"
#include "event.h"

struct loop_timer {
    struct event_loop *loop;
    struct loop_timer *next_all;
    int64_t deadline_ms;
    timer_cb cb;
    void *arg;
    size_t heap_index;
    uint8_t active;
};

struct fd_reg {
    struct fd_reg *next;
    int fd;
    struct conn *owner;
    enum fd_role role;
    uint32_t events;
    accept_cb accept;
    uint8_t listener;
};

struct event_loop {
#ifdef __linux__
    int epfd;
    struct epoll_event *ready;
#endif
    int max_events;
    enum loop_phase phase;
    conn_event_cb conn_cb;
    struct fd_reg *fds;
    struct loop_timer **heap;
    size_t heap_len;
    size_t heap_cap;
    struct loop_timer *timers;
    struct loop_timer *drain_timer;
};

int64_t px_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void heap_swap(struct event_loop *loop, size_t a, size_t b)
{
    struct loop_timer *t = loop->heap[a];
    loop->heap[a] = loop->heap[b];
    loop->heap[b] = t;
    loop->heap[a]->heap_index = a;
    loop->heap[b]->heap_index = b;
}

static void heap_up(struct event_loop *loop, size_t i)
{
    while (i != 0) {
        size_t parent = (i - 1) / 2;
        if (loop->heap[parent]->deadline_ms <= loop->heap[i]->deadline_ms)
            break;
        heap_swap(loop, parent, i);
        i = parent;
    }
}

static void heap_down(struct event_loop *loop, size_t i)
{
    for (;;) {
        size_t left = i * 2 + 1, right = left + 1, best = i;
        if (left < loop->heap_len && loop->heap[left]->deadline_ms < loop->heap[best]->deadline_ms)
            best = left;
        if (right < loop->heap_len && loop->heap[right]->deadline_ms < loop->heap[best]->deadline_ms)
            best = right;
        if (best == i) break;
        heap_swap(loop, i, best);
        i = best;
    }
}

static void heap_remove(struct loop_timer *t)
{
    struct event_loop *loop = t->loop;
    size_t i;
    if (!t->active) return;
    i = t->heap_index;
    t->active = 0;
    --loop->heap_len;
    if (i != loop->heap_len) {
        loop->heap[i] = loop->heap[loop->heap_len];
        loop->heap[i]->heap_index = i;
        if (i != 0 && loop->heap[i]->deadline_ms < loop->heap[(i - 1) / 2]->deadline_ms)
            heap_up(loop, i);
        else
            heap_down(loop, i);
    }
}

static int next_timeout(const struct event_loop *loop)
{
    int64_t delta;
    if (loop->heap_len == 0) return -1;
    delta = loop->heap[0]->deadline_ms - px_now_ms();
    if (delta <= 0) return 0;
    return delta > INT_MAX ? INT_MAX : (int)delta;
}

static void fire_timers(struct event_loop *loop)
{
    while (loop->heap_len != 0 && loop->heap[0]->deadline_ms <= px_now_ms()) {
        struct loop_timer *t = loop->heap[0];
        heap_remove(t);
        if (t->cb != NULL) t->cb(loop, t, t->arg);
    }
}

static struct fd_reg *find_fd(struct event_loop *loop, int fd)
{
    struct fd_reg *r;
    for (r = loop->fds; r != NULL; r = r->next)
        if (r->fd == fd) return r;
    return NULL;
}

#ifdef __linux__
static uint32_t conn_mask(uint32_t events)
{
    return events | EPOLLET | EPOLLRDHUP;
}

static int epoll_update(struct event_loop *loop, int op, struct fd_reg *r)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = r->listener ? (r->events | EPOLLET) : conn_mask(r->events);
    ev.data.ptr = r;
    return epoll_ctl(loop->epfd, op, r->fd, &ev);
}
#endif

struct event_loop *loop_new(int max_events)
{
    struct event_loop *loop;
    if (max_events <= 0) { errno = EINVAL; return NULL; }
    loop = calloc(1, sizeof *loop);
    if (loop == NULL) return NULL;
    loop->max_events = max_events;
    loop->phase = LOOP_RUNNING;
#ifdef __linux__
    loop->epfd = -1;
    loop->epfd = epoll_create1(EPOLL_CLOEXEC);
    loop->ready = calloc((size_t)max_events, sizeof *loop->ready);
    if (loop->epfd < 0 || loop->ready == NULL) { loop_free(loop); return NULL; }
#endif
    return loop;
}

void loop_free(struct event_loop *loop)
{
    struct fd_reg *r, *next;
    struct loop_timer *t, *tnext;
    if (loop == NULL) return;
    for (r = loop->fds; r != NULL; r = next) {
        next = r->next;
        if (r->listener) close(r->fd);
        free(r);
    }
    for (t = loop->timers; t != NULL; t = tnext) { tnext = t->next_all; free(t); }
#ifdef __linux__
    if (loop->epfd >= 0) close(loop->epfd);
    free(loop->ready);
#endif
    free(loop->heap);
    free(loop);
}

void loop_set_conn_cb(struct event_loop *loop, conn_event_cb cb) { loop->conn_cb = cb; }
enum loop_phase loop_phase(const struct event_loop *loop) { return loop->phase; }
void loop_stop(struct event_loop *loop) { loop->phase = LOOP_STOPPED; }

static int add_reg(struct event_loop *loop, int fd, uint32_t events, struct conn *owner,
                   enum fd_role role, accept_cb accept, uint8_t listener)
{
    struct fd_reg *r;
    if (fd < 0 || find_fd(loop, fd) != NULL) { errno = EINVAL; return -1; }
    r = calloc(1, sizeof *r);
    if (r == NULL) return -1;
    r->fd = fd; r->events = events; r->owner = owner; r->role = role;
    r->accept = accept; r->listener = listener;
#ifdef __linux__
    if (epoll_update(loop, EPOLL_CTL_ADD, r) != 0) { free(r); return -1; }
#endif
    r->next = loop->fds;
    loop->fds = r;
    return 0;
}

int loop_add_listener(struct event_loop *loop, int lfd, accept_cb cb)
{
#ifdef __linux__
    return add_reg(loop, lfd, EPOLLIN, NULL, FD_ROLE_CLIENT, cb, 1);
#else
    return add_reg(loop, lfd, POLLIN, NULL, FD_ROLE_CLIENT, cb, 1);
#endif
}

int loop_add(struct event_loop *loop, int fd, uint32_t events, struct conn *owner,
             enum fd_role role)
{
    if (owner == NULL || role >= FD_ROLE_COUNT) { errno = EINVAL; return -1; }
    return add_reg(loop, fd, events, owner, role, NULL, 0);
}

int loop_mod(struct event_loop *loop, int fd, uint32_t events)
{
    struct fd_reg *r = find_fd(loop, fd);
    if (r == NULL) { errno = ENOENT; return -1; }
    r->events = events;
#ifdef __linux__
    return epoll_update(loop, EPOLL_CTL_MOD, r);
#else
    return 0;
#endif
}

int loop_del(struct event_loop *loop, int fd)
{
    struct fd_reg **at = &loop->fds;
    while (*at != NULL && (*at)->fd != fd) at = &(*at)->next;
    if (*at == NULL) { errno = ENOENT; return -1; }
#ifdef __linux__
    (void)epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL);
#endif
    { struct fd_reg *r = *at; *at = r->next; free(r); }
    return 0;
}

struct loop_timer *loop_timer_add(struct event_loop *loop, int64_t deadline_ms,
                                  timer_cb cb, void *arg)
{
    struct loop_timer *t;
    if (loop->heap_len == loop->heap_cap) {
        size_t cap = loop->heap_cap == 0 ? 16 : loop->heap_cap * 2;
        struct loop_timer **heap = realloc(loop->heap, cap * sizeof *heap);
        if (heap == NULL) return NULL;
        loop->heap = heap; loop->heap_cap = cap;
    }
    t = calloc(1, sizeof *t);
    if (t == NULL) return NULL;
    t->loop = loop; t->deadline_ms = deadline_ms; t->cb = cb; t->arg = arg;
    t->active = 1; t->heap_index = loop->heap_len;
    loop->heap[loop->heap_len++] = t;
    t->next_all = loop->timers; loop->timers = t;
    heap_up(loop, t->heap_index);
    return t;
}

void loop_timer_del(struct loop_timer *t) { if (t != NULL) heap_remove(t); }

static int has_conn(const struct event_loop *loop)
{
    const struct fd_reg *r;
    for (r = loop->fds; r != NULL; r = r->next)
        if (!r->listener) return 1;
    return 0;
}

static void drain_expired(struct event_loop *loop, struct loop_timer *t, void *arg)
{
    (void)t; (void)arg; loop_stop(loop);
}

void loop_begin_drain(struct event_loop *loop, uint32_t grace_ms)
{
    struct fd_reg **at;
    struct fd_reg *r;
    struct conn **owners = NULL;
    size_t nowners = 0, cap = 0, i;
    int64_t deadline;
    if (loop->phase != LOOP_RUNNING) return;
    loop->phase = LOOP_DRAINING;
    deadline = px_now_ms() + grace_ms;
    for (at = &loop->fds; *at != NULL;) {
        r = *at;
        if (r->listener) {
#ifdef __linux__
            (void)epoll_ctl(loop->epfd, EPOLL_CTL_DEL, r->fd, NULL);
#endif
            close(r->fd); *at = r->next; free(r); continue;
        }
        at = &r->next;
    }
    for (r = loop->fds; r != NULL; r = r->next) {
        for (i = 0; i < nowners; i++)
            if (owners[i] == r->owner) break;
        if (i != nowners) continue;
        if (nowners == cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            struct conn **new_owners = realloc(owners, new_cap * sizeof *owners);
            if (new_owners == NULL) { free(owners); loop_stop(loop); return; }
            owners = new_owners;
            cap = new_cap;
        }
        owners[nowners++] = r->owner;
    }
    for (i = 0; i < nowners; i++) conn_begin_drain(owners[i], deadline);
    free(owners);
    loop->drain_timer = loop_timer_add(loop, deadline, drain_expired, NULL);
    if (loop->drain_timer == NULL || !has_conn(loop)) loop_stop(loop);
}

int loop_run(struct event_loop *loop)
{
    while (loop->phase != LOOP_STOPPED) {
        int timeout;
        fire_timers(loop);
        if (loop->phase == LOOP_STOPPED) break;
        if (loop->phase == LOOP_DRAINING && !has_conn(loop)) { loop_stop(loop); break; }
        timeout = next_timeout(loop);
#ifdef __linux__
        {
            int n = epoll_wait(loop->epfd, loop->ready, loop->max_events, timeout), i;
            if (n < 0) { if (errno == EINTR) continue; return -1; }
            for (i = 0; i < n; i++) {
                struct fd_reg *r = loop->ready[i].data.ptr;
                if (r->listener) { if (r->accept != NULL) r->accept(loop, r->fd); }
                else if (loop->conn_cb != NULL) loop->conn_cb(loop, r->owner, r->role, loop->ready[i].events);
            }
        }
#else
        {
            struct fd_reg *r;
            struct pollfd *pfds;
            struct fd_reg **regs;
            size_t n = 0, i = 0;
            for (r = loop->fds; r != NULL; r = r->next) n++;
            pfds = calloc(n == 0 ? 1 : n, sizeof *pfds);
            regs = calloc(n == 0 ? 1 : n, sizeof *regs);
            if (pfds == NULL || regs == NULL) { free(pfds); free(regs); return -1; }
            for (r = loop->fds; r != NULL; r = r->next) { pfds[i].fd = r->fd; pfds[i].events = (short)r->events; regs[i++] = r; }
            { int ready = poll(pfds, n, timeout); if (ready < 0) { free(pfds); free(regs); if (errno == EINTR) continue; return -1; }
              for (i = 0; i < n; i++) if (pfds[i].revents != 0) {
                  if (regs[i]->listener) { if (regs[i]->accept != NULL) regs[i]->accept(loop, regs[i]->fd); }
                  else if (loop->conn_cb != NULL) loop->conn_cb(loop, regs[i]->owner, regs[i]->role, (uint32_t)pfds[i].revents);
              } }
            free(pfds); free(regs);
        }
#endif
    }
    return 0;
}
