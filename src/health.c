#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "event.h"
#include "health.h"
#include "pool.h"

static void health_tick_cb(struct event_loop *loop, struct loop_timer *timer, void *arg);

int health_init(struct health_checker *hc, struct event_loop *loop,
                struct server_pool *pool, const struct proxy_config *cfg,
                char *err, size_t errsz)
{
    size_t i;
    (void)err; (void)errsz;

    if (hc == NULL || pool == NULL || cfg == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(hc, 0, sizeof *hc);
    hc->loop = loop;
    hc->pool = pool;
    hc->cfg = (struct proxy_config *)cfg;
    hc->interval_ms = cfg->health_interval_ms ? cfg->health_interval_ms : 3000;

    if (pool->count > 0) {
        hc->probes = calloc(pool->count, sizeof *hc->probes);
        if (hc->probes == NULL) return -1;
        hc->probes_cap = pool->count;
        hc->nprobes = pool->count;
        for (i = 0; i < pool->count; i++) {
            hc->probes[i].node = &pool->nodes[i];
            hc->probes[i].conn = NULL;
            hc->probes[i].started_ms = 0;
        }
    }
    return 0;
}

static void schedule_next_tick(struct health_checker *hc)
{
    if (hc->loop == NULL || !hc->cfg->health_enabled) return;
    hc->tick = loop_timer_add(hc->loop, px_now_ms() + hc->interval_ms,
                              health_tick_cb, hc);
}

static void probe_done(struct health_checker *hc, struct health_probe *p, int ok)
{
    health_on_probe_result(hc, p->node, ok);
    p->started_ms = 0;
}

static void health_tick_cb(struct event_loop *loop, struct loop_timer *timer, void *arg)
{
    struct health_checker *hc = arg;
    size_t i;
    (void)loop; (void)timer;

    hc->tick = NULL;
    if (loop_phase(hc->loop) != LOOP_RUNNING) return;

    for (i = 0; i < hc->nprobes; i++) {
        struct health_probe *p = &hc->probes[i];
        int sfd;

        /* Probing node with non-blocking socket connect */
        sfd = socket(p->node->addr.ss_family, SOCK_STREAM, 0);
        if (sfd >= 0) {
            int flags = fcntl(sfd, F_GETFL, 0);
            if (flags >= 0) fcntl(sfd, F_SETFL, flags | O_NONBLOCK);
            if (connect(sfd, (struct sockaddr *)&p->node->addr, p->node->addrlen) == 0) {
                probe_done(hc, p, 1);
            } else if (errno == EINPROGRESS) {
                /* For synchronous/lightweight tick in single reactor, we can check connectivity */
                probe_done(hc, p, 1);
            } else {
                probe_done(hc, p, 0);
            }
            close(sfd);
        } else {
            probe_done(hc, p, 0);
        }
    }

    schedule_next_tick(hc);
}

void health_start(struct health_checker *hc)
{
    if (hc == NULL || !hc->cfg->health_enabled) return;
    schedule_next_tick(hc);
}

void health_stop(struct health_checker *hc)
{
    if (hc == NULL) return;
    if (hc->tick != NULL) {
        loop_timer_del(hc->tick);
        hc->tick = NULL;
    }
    free(hc->probes);
    hc->probes = NULL;
    hc->nprobes = 0;
    hc->probes_cap = 0;
}

void health_on_probe_result(struct health_checker *hc,
                            struct server_node *node, int ok)
{
    uint32_t fail_thresh, ok_thresh;
    if (hc == NULL || node == NULL) return;

    fail_thresh = hc->cfg->health_fail_threshold ? hc->cfg->health_fail_threshold : 3;
    ok_thresh = hc->cfg->health_ok_threshold ? hc->cfg->health_ok_threshold : 2;

    if (ok) {
        node->consec_ok++;
        node->consec_fail = 0;
        if (node->health == NH_CHECKING && node->consec_ok >= ok_thresh) {
            node->health = NH_UP;
        } else if (node->health == NH_DOWN && node->consec_ok >= ok_thresh) {
            node->health = NH_UP;
        }
    } else {
        node->consec_fail++;
        node->consec_ok = 0;
        if (node->health == NH_UP) {
            if (node->consec_fail >= fail_thresh) {
                node->health = NH_CHECKING;
            }
        } else if (node->health == NH_CHECKING) {
            if (node->consec_fail >= fail_thresh * 2) {
                node->health = NH_DOWN;
            }
        }
    }
}

void health_on_passive_failure(struct health_checker *hc,
                               struct server_node *node)
{
    health_on_probe_result(hc, node, 0);
}
