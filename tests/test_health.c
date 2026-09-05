#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "event.h"
#include "health.h"
#include "pool.h"

void conn_begin_drain(struct conn *c, int64_t deadline_ms)
{
    (void)c;
    (void)deadline_ms;
}

int main(void)
{
    struct health_checker hc;
    struct server_pool pool;
    struct proxy_config cfg;
    struct server_node *node;
    char err[128];

    config_defaults(&cfg);
    cfg.health_ok_threshold = 2;
    cfg.health_fail_threshold = 3;

    assert(pool_init(&pool, 2, BAL_ROUND_ROBIN) == 0);
    assert(pool_add(&pool, "127.0.0.1", 9001, 1, err, sizeof err) == 0);
    node = &pool.nodes[0];
    assert(node->health == NH_UP);

    assert(health_init(&hc, NULL, &pool, &cfg, err, sizeof err) == 0);

    /* 1 and 2 failures: still UP */
    health_on_probe_result(&hc, node, 0);
    assert(node->health == NH_UP && node->consec_fail == 1);
    health_on_probe_result(&hc, node, 0);
    assert(node->health == NH_UP && node->consec_fail == 2);

    /* 3rd failure: transitions to NH_CHECKING */
    health_on_probe_result(&hc, node, 0);
    assert(node->health == NH_CHECKING && node->consec_fail == 3);

    /* In CHECKING: 1 success does not restore (need 2) */
    health_on_probe_result(&hc, node, 1);
    assert(node->health == NH_CHECKING && node->consec_ok == 1);

    /* 2nd success: restored to NH_UP */
    health_on_probe_result(&hc, node, 1);
    assert(node->health == NH_UP && node->consec_ok == 2);

    /* Fail again into CHECKING */
    health_on_probe_result(&hc, node, 0);
    health_on_probe_result(&hc, node, 0);
    health_on_probe_result(&hc, node, 0);
    assert(node->health == NH_CHECKING);

    /* 3 more fails (total 6) -> transitions to NH_DOWN */
    health_on_probe_result(&hc, node, 0);
    health_on_probe_result(&hc, node, 0);
    health_on_probe_result(&hc, node, 0);
    assert(node->health == NH_DOWN);

    /* In DOWN: 1 success not enough */
    health_on_probe_result(&hc, node, 1);
    assert(node->health == NH_DOWN);

    /* 2nd success: back to UP */
    health_on_probe_result(&hc, node, 1);
    assert(node->health == NH_UP);

    /* Passive failure triggers failure immediately */
    health_on_passive_failure(&hc, node);
    assert(node->consec_fail == 1);

    health_stop(&hc);
    pool_free(&pool);

    printf("test_health: ALL PASS\n");
    return 0;
}
