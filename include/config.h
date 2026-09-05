/*
 * config.h -- runtime configuration.
 *
 * Loaded once at startup from a small text file (see DESIGN.md for the
 * format).  config_load() fills a proxy_config; main() then seeds the
 * server_pool from cfg->backends and builds the listener.  Keeping
 * config separate from pool means the health checker and the conn
 * machinery read a stable snapshot instead of reaching into parse
 * state.
 */
#ifndef PX_CONFIG_H
#define PX_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "pool.h"
#include "proxy.h"

struct backend_cfg {
    char     host[128];
    uint16_t port;
    int      weight;
};

struct proxy_config {
    /* listener */
    char     listen_host[64];
    uint16_t listen_port;
    int      backlog;
    int      reuseport;      /* SO_REUSEPORT: allow N worker processes */
    int      workers;        /* process count when reuseport=1 */

    /* capacity */
    size_t   max_conns;      /* hard cap -> 503 when exceeded */
    size_t   max_head;       /* max request/response head size (431/414) */
    size_t   buf_cap;        /* per-direction relay buffer */

    /* timeouts (ms) */
    uint32_t connect_timeout_ms;   /* backend connect */
    uint32_t idle_timeout_ms;      /* no progress on either side */

    /* balancing */
    enum bal_algo algo;

    /* active health checks */
    int      health_enabled;
    uint32_t health_interval_ms;
    uint32_t health_timeout_ms;
    uint32_t health_ok_threshold;    /* consecutive OKs to mark UP */
    uint32_t health_fail_threshold;  /* consecutive fails to mark DOWN */
    char     health_path[128];       /* request target, e.g. /healthz */

    /* zero-copy splice fast path (M7, DESIGN 14): bodies larger than
     * this bypass the mbuf via splice(fd->pipe->fd). 0 disables the
     * path entirely (default until the M7 experiment). */
    size_t   splice_threshold;  /* bytes, 0 = mbuf only */

    /* backends (static list from the config file) */
    struct backend_cfg backends[PX_MAX_BACKENDS];
    size_t             nbackends;
};

void config_defaults(struct proxy_config *cfg);
int  config_load(struct proxy_config *cfg, const char *path,
                 char *err, size_t errsz);

#endif /* PX_CONFIG_H */
