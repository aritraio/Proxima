/*
 * event.h -- the reactor.
 *
 * The loop is deliberately opaque: all Linux/epoll specifics live in
 * src/event.c.  Headers stay portable so they can be compile-checked
 * anywhere; only the .c files that actually call epoll_* are Linux-only
 * (see the Makefile / CI notes in DESIGN.md).
 *
 * Event model (edge-triggered):
 *   * One epoll fd, one thread.  Scaling to N cores later = N reactors
 *     behind SO_REUSEPORT listeners (see DESIGN.md section 3); the
 *     loop API does not change.
 *   * Every registered fd points back at its owner (a struct conn) and
 *     the role that fd plays (client or upstream side), so the conn
 *     state machine receives fully-qualified (role, events) tuples.
 *   * ET rules enforced by convention, documented in DESIGN.md:
 *       - reads loop until EAGAIN; the loop never re-arms EPOLLIN
 *         speculatively while unread bytes sit in an mbuf,
 *       - EPOLLOUT is armed only when a write actually blocked or a
 *         non-blocking connect() is in flight; it is dropped the moment
 *         the outbound queue drains.
 *   * Timers are absolute-deadline monotonic (px_now_ms) entries on an
 *     intrusive min-heap; the loop sleeps in epoll_wait until the
 *     nearest deadline.
 */
#ifndef PX_EVENT_H
#define PX_EVENT_H

#include <stddef.h>
#include <stdint.h>

#include "proxy.h"

struct conn;
struct event_loop;
struct loop_timer;

/* Per-connection event delivery.  `events` is an epoll bitmask
 * (EPOLLIN/EPOLLOUT/EPOLLERR/EPOLLHUP) already normalized for the fd's
 * role; conn_handle_events() in conn.c runs the state machine. */
typedef void (*conn_event_cb)(struct event_loop *loop, struct conn *c,
                              enum fd_role role, uint32_t events);

typedef void (*timer_cb)(struct event_loop *loop, struct loop_timer *t,
                         void *arg);

struct event_loop *loop_new(int max_events);
void loop_free(struct event_loop *loop);

int loop_run(struct event_loop *loop);    /* returns when loop_stop()ed */
void loop_stop(struct event_loop *loop);

void loop_set_conn_cb(struct event_loop *loop, conn_event_cb cb);

/* Register / update / remove an fd owned by a conn.  The loop stores
 * (conn, role) alongside the fd and hands it back on every event. */
int loop_add(struct event_loop *loop, int fd, uint32_t events,
             struct conn *owner, enum fd_role role);
int loop_mod(struct event_loop *loop, int fd, uint32_t events);
int loop_del(struct event_loop *loop, int fd);

/* conn.c computes the wanted mask for each of its fds after every state
 * transition and calls loop_mod() only when the mask changed. */
uint32_t conn_want(struct conn *c, enum fd_role role);

/* --- timers ------------------------------------------------------- */
struct loop_timer *loop_timer_add(struct event_loop *loop,
                                  int64_t deadline_ms,
                                  timer_cb cb, void *arg);
void loop_timer_del(struct loop_timer *t);   /* safe to call after firing */

#endif /* PX_EVENT_H */
