/* main.c -- pxlb standalone CLI daemon.
 *
 * Boots the single-threaded reactor from a config file, owns the listener,
 * drives loop_run() until drain completes, and handles lifecycle signals.
 *
 * Signals (async-signal-safe: handlers only set flags; the reactor thread
 * polls them every 100ms between epoll waits, so no unsafe work runs in
 * handler context):
 *   SIGINT/SIGTERM -> graceful drain (loop_begin_drain, 5s grace)
 *   SIGUSR1        -> dump RED metrics to stderr (Prometheus text)
 *   SIGHUP         -> reload backends from config file
 *   SIGPIPE        -> ignored
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "buf.h"
#include "config.h"
#include "conn.h"
#include "event.h"
#include "health.h"
#include "http.h"
#include "metrics.h"
#include "pipe_pool.h"
#include "pool.h"
#include "proxy.h"

extern struct server_pool *g_proxy_pool;
extern struct health_checker *g_health_checker;

/* --- signal flags (sig_atomic_t, set in handler, read in loop) --- */
static volatile sig_atomic_t g_sig_term = 0;
static volatile sig_atomic_t g_sig_usr1 = 0;
static volatile sig_atomic_t g_sig_hup = 0;

static struct event_loop *g_loop = NULL;
static struct server_pool *g_pool_ptr = NULL;
static struct health_checker *g_hc_ptr = NULL;
static struct proxy_config *g_cfg_ptr = NULL;
static struct conn_params g_conn_prm;
static char g_config_path[512] = "pxlb.conf";

static void on_term(int sig) { (void)sig; g_sig_term = 1; }
static void on_usr1(int sig) { (void)sig; g_sig_usr1 = 1; }
static void on_hup(int sig) { (void)sig; g_sig_hup = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-c <config>] [-v] [-h]\n"
        "  -c <path>  config file (default: pxlb.conf)\n"
        "  -v         print version/build info and exit\n"
        "  -h         print this help and exit\n",
        prog);
}

static void version_info(void)
{
    printf("pxlb %s (C11, single-threaded reactor)\n", PX_VERSION);
#ifdef __linux__
    printf("platform: linux epoll-ET\n");
#else
    printf("platform: portable poll fallback\n");
#endif
    printf("build: %s %s\n", __DATE__, __TIME__);
}

/* --- reactor callbacks --- */

static void on_conn_event(struct event_loop *loop, struct conn *c,
                          enum fd_role role, uint32_t events)
{
    (void)loop;
    conn_handle_events(c, role, events);
}

static void on_accept(struct event_loop *loop, int lfd)
{
    for (;;) {
        int nfd;
#ifdef __linux__
        nfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (nfd < 0 && errno == ENOSYS) {
            /* Very old kernel: fall back to accept + fcntl. */
            nfd = accept(lfd, NULL, NULL);
            if (nfd >= 0) {
                int fl = fcntl(nfd, F_GETFL, 0);
                if (fl >= 0) fcntl(nfd, F_SETFL, fl | O_NONBLOCK);
                fl = fcntl(nfd, F_GETFD, 0);
                if (fl >= 0) fcntl(nfd, F_SETFD, fl | FD_CLOEXEC);
            }
        }
#else
        nfd = accept(lfd, NULL, NULL);
        if (nfd >= 0) {
            int fl = fcntl(nfd, F_GETFL, 0);
            if (fl >= 0) fcntl(nfd, F_SETFL, fl | O_NONBLOCK);
            fl = fcntl(nfd, F_GETFD, 0);
            if (fl >= 0) fcntl(nfd, F_SETFD, fl | FD_CLOEXEC);
        }
#endif
        if (nfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            return;
        }

        /* M6 connection cap: accept then immediately 503 + close
         * (DESIGN.md 4.6). Never drop silently: the client sees a
         * clean 503 with Connection: close. */
        if (g_cfg_ptr != NULL &&
            g_metrics.active_conns >= (uint64_t)g_cfg_ptr->max_conns) {
            char reply[PX_ERR_REPLY_CAP];
            size_t n = http_build_error_reply(reply, sizeof reply, 503, 0);
            /* Best effort; ignore EAGAIN (tiny reply, socket fresh). */
            ssize_t w = write(nfd, reply, n);
            (void)w;
            close(nfd);
            metrics_record_status(&g_metrics, 503);
            continue;
        }

        if (loop_phase(loop) != LOOP_RUNNING) {
            close(nfd);
            return;
        }

        if (conn_accept(loop, nfd, &g_conn_prm) == NULL) {
            close(nfd);
            /* OOM: reply nothing; the client will see a close. Count it
             * as an internal error so RED stays consistent. */
            metrics_record_internal_error(&g_metrics);
        }
    }
}

/* --- config reload (SIGHUP) --- */

static void do_reload(void)
{
    struct proxy_config ncfg;
    char err[256];
    size_t i, j;

    if (config_load(&ncfg, g_config_path, err, sizeof err) != 0) {
        fprintf(stderr, "pxlb: reload failed: %s\n", err);
        return;
    }

    /* Update scalar tunables for future accepts. Health thresholds are
     * read live from g_cfg_ptr via hc->cfg, so copy them too. */
    g_cfg_ptr->connect_timeout_ms = ncfg.connect_timeout_ms;
    g_cfg_ptr->idle_timeout_ms = ncfg.idle_timeout_ms;
    g_cfg_ptr->health_enabled = ncfg.health_enabled;
    g_cfg_ptr->health_interval_ms = ncfg.health_interval_ms;
    g_cfg_ptr->health_timeout_ms = ncfg.health_timeout_ms;
    g_cfg_ptr->health_ok_threshold = ncfg.health_ok_threshold;
    g_cfg_ptr->health_fail_threshold = ncfg.health_fail_threshold;
    snprintf(g_cfg_ptr->health_path, sizeof g_cfg_ptr->health_path,
             "%s", ncfg.health_path);
    g_cfg_ptr->algo = ncfg.algo;
    g_cfg_ptr->max_conns = ncfg.max_conns;
    g_cfg_ptr->max_head = ncfg.max_head;
    g_cfg_ptr->buf_cap = ncfg.buf_cap;
    g_cfg_ptr->splice_threshold = ncfg.splice_threshold;
    g_conn_prm.max_head = ncfg.max_head;
    g_conn_prm.buf_cap = ncfg.buf_cap;
    g_conn_prm.stage_cap = ncfg.max_head * PX_STAGE_CAP_FACTOR;
    g_conn_prm.connect_timeout_ms = ncfg.connect_timeout_ms;
    g_conn_prm.idle_timeout_ms = ncfg.idle_timeout_ms;
    g_conn_prm.splice_threshold = ncfg.splice_threshold;
    if (g_pool_ptr != NULL) g_pool_ptr->algo = ncfg.algo;

    /* Merge backends: add missing, refresh weights, mark removed DOWN.
     * We never free nodes with possible in-flight conns; DOWN nodes
     * simply leave rotation while in-flight traffic finishes. */
    for (i = 0; i < ncfg.nbackends; i++) {
        char ident[128];
        struct server_node *found = NULL;
        snprintf(ident, sizeof ident, "%s:%u",
                 ncfg.backends[i].host, (unsigned)ncfg.backends[i].port);
        for (j = 0; j < g_pool_ptr->count; j++) {
            if (strcmp(g_pool_ptr->nodes[j].ident, ident) == 0) {
                found = &g_pool_ptr->nodes[j];
                break;
            }
        }
        if (found != NULL) {
            found->weight = ncfg.backends[i].weight;
            if (found->health == NH_DOWN) {
                /* Re-added backend rejoins as CHECKING; probes promote it. */
                found->health = NH_CHECKING;
                found->consec_ok = 0;
                found->consec_fail = 0;
            }
        } else {
            char aerr[128];
            if (pool_add(g_pool_ptr, ncfg.backends[i].host,
                         ncfg.backends[i].port, ncfg.backends[i].weight,
                         aerr, sizeof aerr) != 0) {
                fprintf(stderr, "pxlb: reload: cannot add %s: %s\n",
                        ident, aerr);
            } else {
                fprintf(stderr, "pxlb: reload: added backend %s\n", ident);
            }
        }
    }
    for (j = 0; j < g_pool_ptr->count; j++) {
        int still_present = 0;
        for (i = 0; i < ncfg.nbackends; i++) {
            char ident[128];
            snprintf(ident, sizeof ident, "%s:%u",
                     ncfg.backends[i].host, (unsigned)ncfg.backends[i].port);
            if (strcmp(g_pool_ptr->nodes[j].ident, ident) == 0) {
                still_present = 1;
                break;
            }
        }
        if (!still_present && g_pool_ptr->nodes[j].health == NH_UP) {
            g_pool_ptr->nodes[j].health = NH_DOWN;
            fprintf(stderr, "pxlb: reload: backend %s removed -> DOWN\n",
                    g_pool_ptr->nodes[j].ident);
        }
    }

    fprintf(stderr, "pxlb: reloaded %s (%zu backends)\n",
            g_config_path, ncfg.nbackends);
}

static void dump_metrics_stderr(void)
{
    char buf[8192];
    size_t n = metrics_render(buf, sizeof buf);
    /* Single write() to stderr: safe from the reactor thread. */
    ssize_t w = write(STDERR_FILENO, buf, n);
    (void)w;
}

/* Periodic 100ms self-timer: polls signal flags set by handlers. */
static void signal_poll_cb(struct event_loop *loop, struct loop_timer *t,
                           void *arg)
{
    (void)t; (void)arg;
    if (g_sig_term) {
        g_sig_term = 0;
        fprintf(stderr, "pxlb: SIGTERM/SIGINT: beginning graceful drain\n");
        loop_begin_drain(loop, 5000);
        /* Do not re-arm drain: loop stops itself when conns empty. */
    }
    if (g_sig_usr1) {
        g_sig_usr1 = 0;
        dump_metrics_stderr();
    }
    if (g_sig_hup) {
        g_sig_hup = 0;
        do_reload();
    }
    if (loop_phase(loop) != LOOP_STOPPED) {
        (void)loop_timer_add(loop, px_now_ms() + 100, signal_poll_cb, NULL);
    }
}

static int make_listener(const struct proxy_config *cfg, char *err,
                         size_t errsz)
{
    struct addrinfo hints, *res = NULL, *rp;
    char port_str[16];
    int lfd = -1, rc, one = 1;

    snprintf(port_str, sizeof port_str, "%u", (unsigned)cfg->listen_port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(cfg->listen_host[0] ? cfg->listen_host : NULL,
                     port_str, &hints, &res);
    if (rc != 0) {
        if (err && errsz)
            snprintf(err, errsz, "resolve %s:%s: %s",
                     cfg->listen_host, port_str, gai_strerror(rc));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        lfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (lfd < 0) continue;
        (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        if (cfg->reuseport) {
            (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
        }
#endif
        {
            int fl = fcntl(lfd, F_GETFL, 0);
            if (fl >= 0) fcntl(lfd, F_SETFL, fl | O_NONBLOCK);
            fl = fcntl(lfd, F_GETFD, 0);
            if (fl >= 0) fcntl(lfd, F_SETFD, fl | FD_CLOEXEC);
        }
        if (bind(lfd, rp->ai_addr, rp->ai_addrlen) == 0 &&
            listen(lfd, cfg->backlog > 0 ? cfg->backlog : 128) == 0) {
            break;
        }
        close(lfd);
        lfd = -1;
    }
    freeaddrinfo(res);

    if (lfd < 0 && err && errsz) {
        snprintf(err, errsz, "cannot bind %s:%u: %s",
                 cfg->listen_host, (unsigned)cfg->listen_port,
                 strerror(errno));
    }
    return lfd;
}

int main(int argc, char **argv)
{
    const char *prog = argv[0];
    struct proxy_config cfg;
    struct server_pool pool;
    struct health_checker hc;
    struct event_loop *loop = NULL;
    char err[256];
    int opt, lfd = -1;
    size_t i, pipe_pairs;

    while ((opt = getopt(argc, argv, "c:vh")) != -1) {
        switch (opt) {
        case 'c':
            snprintf(g_config_path, sizeof g_config_path, "%s", optarg);
            break;
        case 'v':
            version_info();
            return 0;
        case 'h':
            usage(prog);
            return 0;
        default:
            usage(prog);
            return 2;
        }
    }

    if (config_load(&cfg, g_config_path, err, sizeof err) != 0) {
        fprintf(stderr, "pxlb: %s\n", err);
        return 1;
    }

    metrics_reset(&g_metrics);
    g_metrics.started_ms = (uint64_t)px_now_ms();

    /* Pipe pool for the M7 splice path (degrades to mbuf when empty). */
    pipe_pairs = cfg.max_conns / 4;
    if (pipe_pairs > 1024) pipe_pairs = 1024;
    if (pipe_pool_init(&g_pipe_pool, pipe_pairs) != 0) {
        fprintf(stderr, "pxlb: pipe_pool_init failed: %s\n", strerror(errno));
        return 1;
    }

    if (pool_init(&pool, cfg.nbackends > 0 ? cfg.nbackends : 4, cfg.algo) != 0) {
        fprintf(stderr, "pxlb: pool_init: %s\n", strerror(errno));
        pipe_pool_destroy(&g_pipe_pool);
        return 1;
    }
    for (i = 0; i < cfg.nbackends; i++) {
        if (pool_add(&pool, cfg.backends[i].host, cfg.backends[i].port,
                     cfg.backends[i].weight, err, sizeof err) != 0) {
            fprintf(stderr, "pxlb: backend %s:%u: %s\n",
                    cfg.backends[i].host, (unsigned)cfg.backends[i].port, err);
            pool_free(&pool);
            pipe_pool_destroy(&g_pipe_pool);
            return 1;
        }
    }
    g_proxy_pool = &pool;
    g_pool_ptr = &pool;
    g_cfg_ptr = &cfg;

    g_conn_prm.max_head = cfg.max_head;
    g_conn_prm.buf_cap = cfg.buf_cap;
    g_conn_prm.stage_cap = cfg.max_head * PX_STAGE_CAP_FACTOR;
    g_conn_prm.connect_timeout_ms = cfg.connect_timeout_ms;
    g_conn_prm.idle_timeout_ms = cfg.idle_timeout_ms;
    g_conn_prm.splice_threshold = cfg.splice_threshold;

    loop = loop_new(1024);
    if (loop == NULL) {
        fprintf(stderr, "pxlb: loop_new: %s\n", strerror(errno));
        pool_free(&pool);
        pipe_pool_destroy(&g_pipe_pool);
        return 1;
    }
    g_loop = loop;
    loop_set_conn_cb(loop, on_conn_event);

    lfd = make_listener(&cfg, err, sizeof err);
    if (lfd < 0) {
        fprintf(stderr, "pxlb: %s\n", err);
        loop_free(loop);
        pool_free(&pool);
        pipe_pool_destroy(&g_pipe_pool);
        return 1;
    }
    if (loop_add_listener(loop, lfd, on_accept) != 0) {
        fprintf(stderr, "pxlb: loop_add_listener: %s\n", strerror(errno));
        close(lfd);
        loop_free(loop);
        pool_free(&pool);
        pipe_pool_destroy(&g_pipe_pool);
        return 1;
    }

    memset(&hc, 0, sizeof hc);
    if (health_init(&hc, loop, &pool, &cfg, err, sizeof err) != 0) {
        fprintf(stderr, "pxlb: health_init: %s\n", err);
        loop_free(loop);
        pool_free(&pool);
        pipe_pool_destroy(&g_pipe_pool);
        return 1;
    }
    g_health_checker = &hc;
    g_hc_ptr = &hc;
    (void)g_hc_ptr;
    health_start(&hc);

    /* Signals: ignore SIGPIPE; flag the rest for the poll timer. */
    signal(SIGPIPE, SIG_IGN);
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_term;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sa.sa_handler = on_usr1;
        sigaction(SIGUSR1, &sa, NULL);
        sa.sa_handler = on_hup;
        sigaction(SIGHUP, &sa, NULL);
    }
    (void)loop_timer_add(loop, px_now_ms() + 100, signal_poll_cb, NULL);

    fprintf(stderr, "pxlb %s listening on %s:%u (%zu backends)\n",
            PX_VERSION, cfg.listen_host, (unsigned)cfg.listen_port,
            pool.count);

    (void)loop_run(loop);

    health_stop(&hc);
    g_health_checker = NULL;
    g_proxy_pool = NULL;
    pipe_pool_destroy(&g_pipe_pool);
    pool_free(&pool);
    loop_free(loop);
    g_loop = NULL;

    return 0;
}
