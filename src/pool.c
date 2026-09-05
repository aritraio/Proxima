#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pool.h"

int pool_init(struct server_pool *pool, size_t cap, enum bal_algo algo)
{
    if (pool == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(pool, 0, sizeof *pool);
    if (cap > 0) {
        pool->nodes = calloc(cap, sizeof *pool->nodes);
        if (pool->nodes == NULL) return -1;
    }
    pool->cap = cap;
    pool->algo = algo;
    pool->count = 0;
    pool->cursor = 0;
    return 0;
}

void pool_free(struct server_pool *pool)
{
    if (pool == NULL) return;
    free(pool->nodes);
    memset(pool, 0, sizeof *pool);
}

int pool_add(struct server_pool *pool, const char *host, uint16_t port,
             int weight, char *err, size_t errsz)
{
    struct addrinfo hints, *res = NULL;
    char port_str[16];
    int rc;
    struct server_node *node;

    if (pool == NULL || host == NULL || weight < 1) {
        if (err && errsz) snprintf(err, errsz, "Invalid pool_add parameters");
        return -1;
    }

    if (pool->count >= pool->cap) {
        size_t new_cap = pool->cap == 0 ? 4 : pool->cap * 2;
        struct server_node *new_nodes = realloc(pool->nodes, new_cap * sizeof *new_nodes);
        if (new_nodes == NULL) {
            if (err && errsz) snprintf(err, errsz, "Out of memory expanding pool");
            return -1;
        }
        memset(new_nodes + pool->count, 0, (new_cap - pool->count) * sizeof *new_nodes);
        pool->nodes = new_nodes;
        pool->cap = new_cap;
    }

    snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || res == NULL) {
        if (err && errsz) {
            snprintf(err, errsz, "Failed to resolve %s:%u: %s",
                     host, (unsigned)port, gai_strerror(rc));
        }
        return -1;
    }

    node = &pool->nodes[pool->count];
    memset(node, 0, sizeof *node);

    if (res->ai_addrlen > sizeof node->addr) {
        if (err && errsz) snprintf(err, errsz, "Resolved address too large for storage");
        freeaddrinfo(res);
        return -1;
    }

    memcpy(&node->addr, res->ai_addr, res->ai_addrlen);
    node->addrlen = (socklen_t)res->ai_addrlen;
    snprintf(node->ident, sizeof node->ident, "%s:%u", host, (unsigned)port);
    node->weight = weight;
    node->health = NH_UP;

    freeaddrinfo(res);
    pool->count++;
    return 0;
}

int pool_has_up_node(const struct server_pool *pool)
{
    size_t i;
    if (pool == NULL || pool->count == 0) return 0;
    for (i = 0; i < pool->count; i++) {
        if (pool->nodes[i].health == NH_UP) return 1;
    }
    return 0;
}

struct server_node *pool_pick(struct server_pool *pool)
{
    size_t i, idx;
    if (pool == NULL || pool->count == 0) return NULL;

    if (pool->algo == BAL_ROUND_ROBIN) {
        for (i = 0; i < pool->count; i++) {
            idx = (pool->cursor + i) % pool->count;
            if (pool->nodes[idx].health == NH_UP) {
                pool->cursor = (idx + 1) % pool->count;
                return &pool->nodes[idx];
            }
        }
        return NULL;
    }

    if (pool->algo == BAL_WEIGHTED_LEAST_CONN) {
        size_t best_idx = (size_t)-1;
        uint64_t best_score = UINT64_MAX;

        for (i = 0; i < pool->count; i++) {
            idx = (pool->cursor + i) % pool->count;
            if (pool->nodes[idx].health == NH_UP) {
                uint64_t score = node_lc_score(&pool->nodes[idx]);
                if (best_idx == (size_t)-1 || score < best_score) {
                    best_score = score;
                    best_idx = idx;
                }
            }
        }

        if (best_idx == (size_t)-1) return NULL;

        pool->cursor = (best_idx + 1) % pool->count;
        return &pool->nodes[best_idx];
    }

    return NULL;
}
