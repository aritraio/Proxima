/*
 * health.h -- active health checking.
 *
 * On every interval tick the checker visits due nodes (a node is due
 * when interval has elapsed since its last probe) and issues a probe:
 * a non-blocking connect + "GET <health_path> HTTP/1.1" against the
 * node's listen address, driven through the normal conn machinery so
 * probes share the event loop, timeouts and buffers with real traffic.
 *
 * State transitions with hysteresis (see DESIGN.md section 8):
 *
 *   NH_UP     --fail_threshold consecutive failures-->   NH_CHECKING
 *   NH_CHECKING --ok_threshold consecutive OKs-->        NH_UP
 *   NH_CHECKING --fail_threshold more failures-->        NH_DOWN
 *   NH_DOWN   --probe every interval; ok_threshold OKs-->NH_UP
 *
 * Nodes not NH_UP are excluded from balancer picks but in-flight
 * requests are allowed to finish.  A node that fails connection setup
 * on real traffic is also penalized here (passive signal) so the pool
 * reacts immediately instead of waiting for the next interval.
 */
#ifndef PX_HEALTH_H
#define PX_HEALTH_H

#include <stddef.h>
#include <stdint.h>

#include "proxy.h"

struct event_loop;
struct loop_timer;
struct server_pool;
struct server_node;
struct conn;

struct health_checker {
    struct event_loop    *loop;
    struct server_pool   *pool;
    struct proxy_config  *cfg;      /* read-only snapshot */

    uint32_t interval_ms;
    struct loop_timer    *tick;     /* periodic driver */

    /* one in-flight probe per node that is being checked */
    size_t  nprobes;
    size_t  probes_cap;
    struct health_probe *probes;
};

struct health_probe {
    struct server_node *node;
    struct conn        *conn;       /* probe conn, or NULL when idle */
    int64_t             started_ms;
};

int  health_init(struct health_checker *hc, struct event_loop *loop,
                 struct server_pool *pool, const struct proxy_config *cfg,
                 char *err, size_t errsz);
void health_start(struct health_checker *hc);
void health_stop(struct health_checker *hc);   /* cancel tick + probes */

/* Callbacks conn.c / event.c invoke to report outcomes. */
void health_on_probe_result(struct health_checker *hc,
                            struct server_node *node, int ok);
void health_on_passive_failure(struct health_checker *hc,
                               struct server_node *node);

#endif /* PX_HEALTH_H */
