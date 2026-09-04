/*
 * pool.h -- backend nodes, the pool, and the balancer.
 *
 * The pool is populated once at startup from the config file (blocking
 * getaddrinfo is fine there) and is otherwise read-mostly.  The reactor
 * is single-threaded, so node counters need no locking; the day we add
 * SO_REUSEPORT workers, per-worker cursor/health state must be split
 * out (see DESIGN.md section 7).  Health state flips (UP<->DOWN) only
 * from the health-check machinery in health.c.
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

    int                     weight;       /* >= 1 */
    enum node_health        health;

    /* health-check bookkeeping (written by health.c) */
    uint32_t consec_ok;
    uint32_t consec_fail;

    /* in-flight / lifetime counters (written by conn.c) */
    uint64_t active;      /* requests currently assigned to this node */
    uint64_t total;       /* completed requests */
    uint64_t failed;      /* requests that ended in error/refused */
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

/* Accounting around a request's lifetime (called by conn.c):
 * node_begin() when a node is picked, node_end() when the conn closes. */
void node_begin(struct server_node *node);
void node_end(struct server_node *node, int ok);

int pool_has_up_node(const struct server_pool *pool);

#endif /* PX_POOL_H */
