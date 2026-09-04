/*
 * pool.h -- backend nodes, the pool, and the balancer.
 *
 * The pool is populated once at startup from the config file (blocking
 * getaddrinfo is fine there) and is otherwise read-mostly.  The reactor
 * is single-threaded, so node counters need no locking; the day we add
 * SO_REUSEPORT workers, per-worker cursor/health state must be split
 * out (see DESIGN.md section 7).  Health state flips (UP<->DOWN) only
 * from the health-check machinery in health.c.
 *
 * DEFENSIVE ACCOUNTING (hazard 5): node->active feeds the WLC score,
 * so an underflow would poison the balancer forever.  node_begin /
 * node_end are static inline and underflow-guarded: a decrement on a
 * zero counter is recorded as an accounting anomaly (node->anomalies
 * and g_metrics) instead of wrapping to UINT64_MAX.  The anomaly is
 * the bug surface -- find the double node_end(), never let it corrupt
 * the balancer.
 */
#ifndef PX_POOL_H
#define PX_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "proxy.h"

enum bal_algo {
    BAL_ROUND_ROBIN = 0,
    BAL_WEIGHTED_LEAST_CONN
};

enum node_health {
    NH_DOWN = 0,
    NH_UP,
    NH_CHECKING          /* transitional: probing after a failure */
};

struct server_node {
    struct sockaddr_storage addr;     /* resolved at startup */
    socklen_t               addrlen;
    char                    ident[128];   /* "127.0.0.1:9001" for logs */

    int                     weight;       /* >= 1 (pool_add validates) */
    enum node_health        health;

    /* health-check bookkeeping (written by health.c) */
    uint32_t consec_ok;
    uint32_t consec_fail;

    /* in-flight / lifetime counters (written by conn.c) */
    uint64_t active;      /* requests currently assigned to this node */
    uint64_t total;       /* cleanly completed requests */
    uint64_t failed;      /* requests that ended in error/refused */
    uint64_t anomalies;   /* accounting bugs observed on this node
                             (double node_end, decrement on zero) */
};

struct server_pool {
    struct server_node *nodes;
    size_t              count;
    size_t              cap;
    size_t              cursor;       /* round-robin cursor */
    enum bal_algo       algo;
};

int pool_init(struct server_pool *pool, size_t cap, enum bal_algo algo);
void pool_free(struct server_pool *pool);

/* Resolve host:port (blocking; startup only) and append a node.
 * weight must be >= 1.  Returns 0 or -1 with a message in err[]. */
int pool_add(struct server_pool *pool, const char *host, uint16_t port,
             int weight, char *err, size_t errsz);

/* Pick the next backend per the pool's algorithm.  Returns NULL when no
 * node is UP (the caller replies 503).  UP nodes only; a node that is
 * NH_CHECKING stays out of rotation until it recovers. */
struct server_node *pool_pick(struct server_pool *pool);

int pool_has_up_node(const struct server_pool *pool);

/* ------------------------------------------------------------------ */
/* Node accounting (inline: hot path, single thread, no locks).        */
/* ------------------------------------------------------------------ */

/* Call when a node is picked for a request (CS_SELECT). */
static inline void node_begin(struct server_node *node)
{
    node->active++;
}

/*
 * Call exactly once per request, when its conn reaches CS_DONE.
 *   ok != 0 : request completed cleanly            -> total++
 *   ok == 0 : request ended in error/refused       -> failed++
 * Underflow guard: decrementing a zero counter is an internal bug;
 * it is counted, not wrapped.
 */
static inline void node_end(struct server_node *node, int ok)
{
    if (node->active > 0) {
        node->active--;
        if (ok)
            node->total++;
        else
            node->failed++;
    } else {
        /* Double node_end() / lost node_begin: the request was never
         * counted here (or already was), so do NOT touch total/failed
         * again -- surface the bug, keep the counters truthful. */
        node->anomalies++;
    }
}

/*
 * WLC score: ceil(active * 1000 / weight) in integer arithmetic.
 * Defensive: weight < 1 (should be impossible after pool_add
 * validation) is clamped to 1 so the divisor is never zero and a
 * misconfigured node can never produce a garbage score.
 */
static inline uint64_t node_lc_score(const struct server_node *node)
{
    uint32_t w = (node->weight >= 1) ? (uint32_t)node->weight : 1u;
    return (node->active * 1000u) / w;
}

#endif /* PX_POOL_H */
