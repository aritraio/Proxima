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
 *   * EPOLLRDHUP (hazard 3) is ALWAYS armed: loop_add()/loop_mod()
 *     OR it into every conn-fd mask automatically, so callers must not
 *     (and need not) pass it.  A bare FIN with no payload can otherwise
 *     be missed in edge-triggered mode once EPOLLIN has fired -- and
 *     half-close detection must not depend on a read() that never
 *     arrives.  When RDHUP is reported, conn.c sets the matching *_eof
 *     flag and transitions immediately (DESIGN.md section 3.1 rule 6).
 *   * Timers are absolute-deadline monotonic (px_now_ms) entries on an
 *     intrusive min-heap; the loop sleeps in epoll_wait until the
 *     nearest deadline.
 *
 * GRACEFUL DRAIN (hazard 6): on SIGTERM/SIGQUIT main() wakes the loop
 * (eventfd) and calls loop_begin_drain().  The loop closes its
 * listener(s), flips to LOOP_DRAINING, walks every registered conn
 * calling conn_begin_drain() (idle keep-alive conns close now; active
 * relays get the grace deadline and keep_alive_ok=0), and stops the
 * loop when the conn list empties or the grace deadline fires -- never
 * an abrupt RST storm.  See DESIGN.md section 4.8.
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
 * (EPOLLIN/EPOLLOUT/EPOLLERR/EPOLLHUP/EPOLLRDHUP) normalized for the
 * fd's role; conn_handle_events() in conn.c runs the state machine.
 * RDHUP is always armed for conn fds (see header comment). */
typedef void (*conn_event_cb)(struct event_loop *loop, struct conn *c,
                              enum fd_role role, uint32_t events);

/* Accept callback for listener fds.  `lfd` is the listener; the
 * callback accepts in a loop until EAGAIN (ET) and hands each new fd
 * to conn_accept().  The callback may test loop_phase() == LOOP_DRAINING
 * and stop accepting. */
typedef void (*accept_cb)(struct event_loop *loop, int lfd);

typedef void (*timer_cb)(struct event_loop *loop, struct loop_timer *t,
                         void *arg);

/* Reactor phase -- the graceful-shutdown state machine (hazard 6). */
enum loop_phase {
    LOOP_RUNNING = 0,   /* accepting + serving */
    LOOP_DRAINING,      /* listener closed; finishing in-flight work */
    LOOP_STOPPED        /* epoll_wait has returned; loop_run() unwinding */
};

struct event_loop *loop_new(int max_events);
void loop_free(struct event_loop *loop);

int loop_run(struct event_loop *loop);    /* returns when stopped */
void loop_stop(struct event_loop *loop);

void loop_set_conn_cb(struct event_loop *loop, conn_event_cb cb);

/* --- listener ------------------------------------------------------ */
/* Register the listen socket.  The loop owns it from here on: it is
 * closed by loop_begin_drain() (and loop_free()).  Returns 0 / -1. */
int loop_add_listener(struct event_loop *loop, int lfd, accept_cb cb);

/* --- graceful drain (hazard 6) ------------------------------------- */
/* Stop accepting, grant in-flight conns `grace_ms` (from now) to reach
 * CS_DONE, then terminate.  Idempotent.  See header comment + DESIGN
 * section 4.8 for the per-conn behavior. */
void loop_begin_drain(struct event_loop *loop, uint32_t grace_ms);

enum loop_phase loop_phase(const struct event_loop *loop);

/* --- conn fds ------------------------------------------------------ */
/* Register / update / remove an fd owned by a conn.  The loop stores
 * (conn, role) alongside the fd and hands it back on every event.
 * EPOLLRDHUP is OR'ed in automatically and must not be passed. */
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
