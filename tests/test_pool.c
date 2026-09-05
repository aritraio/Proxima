#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pool.h"

static void test_round_robin(void)
{
    struct server_pool pool;
    struct server_node *n;
    char err[128];

    assert(pool_init(&pool, 4, BAL_ROUND_ROBIN) == 0);
    assert(pool_pick(&pool) == NULL);

    assert(pool_add(&pool, "127.0.0.1", 9001, 1, err, sizeof err) == 0);
    assert(pool_add(&pool, "127.0.0.1", 9002, 1, err, sizeof err) == 0);
    assert(pool_add(&pool, "127.0.0.1", 9003, 1, err, sizeof err) == 0);
    assert(pool.count == 3);
    assert(pool_has_up_node(&pool));

    /* Round robin rotation */
    n = pool_pick(&pool);
    assert(n != NULL && strcmp(n->ident, "127.0.0.1:9001") == 0);
    n = pool_pick(&pool);
    assert(n != NULL && strcmp(n->ident, "127.0.0.1:9002") == 0);
    n = pool_pick(&pool);
    assert(n != NULL && strcmp(n->ident, "127.0.0.1:9003") == 0);
    n = pool_pick(&pool);
    assert(n != NULL && strcmp(n->ident, "127.0.0.1:9001") == 0);

    /* Health filtering */
    pool.nodes[1].health = NH_DOWN; /* 9002 is DOWN */
    n = pool_pick(&pool);
    assert(n != NULL && strcmp(n->ident, "127.0.0.1:9003") == 0);
    n = pool_pick(&pool);
    assert(n != NULL && strcmp(n->ident, "127.0.0.1:9001") == 0);

    pool.nodes[0].health = NH_DOWN;
    pool.nodes[2].health = NH_CHECKING; /* CHECKING is also excluded */
    assert(!pool_has_up_node(&pool));
    assert(pool_pick(&pool) == NULL);

    pool_free(&pool);
}

static void test_weighted_least_conn(void)
{
    struct server_pool pool;
    struct server_node *n;
    char err[128];

    assert(pool_init(&pool, 2, BAL_WEIGHTED_LEAST_CONN) == 0);
    assert(pool_add(&pool, "127.0.0.1", 9001, 1, err, sizeof err) == 0); /* weight 1 */
    assert(pool_add(&pool, "127.0.0.1", 9002, 2, err, sizeof err) == 0); /* weight 2 */

    /* Both have active == 0, tie-break from cursor */
    n = pool_pick(&pool);
    assert(n == &pool.nodes[0]);
    node_begin(n); /* node 0 has active = 1 -> score = 1000/1 = 1000 */

    /* Next pick: node 0 score=1000, node 1 score=0 -> pick node 1 */
    n = pool_pick(&pool);
    assert(n == &pool.nodes[1]);
    node_begin(n); /* node 1 has active = 1 -> score = 1000/2 = 500 */

    /* Next pick: node 0 score=1000, node 1 score=500 -> pick node 1 again! */
    n = pool_pick(&pool);
    assert(n == &pool.nodes[1]);
    node_begin(n); /* node 1 has active = 2 -> score = 2000/2 = 1000 */

    /* Complete requests */
    node_end(&pool.nodes[0], 1);
    assert(pool.nodes[0].active == 0 && pool.nodes[0].total == 1);
    node_end(&pool.nodes[1], 1);
    assert(pool.nodes[1].active == 1 && pool.nodes[1].total == 1);
    node_end(&pool.nodes[1], 0);
    assert(pool.nodes[1].active == 0 && pool.nodes[1].failed == 1);

    /* Double node_end underflow guard test */
    node_end(&pool.nodes[1], 1);
    assert(pool.nodes[1].active == 0);
    assert(pool.nodes[1].anomalies == 1);

    pool_free(&pool);
}

int main(void)
{
    test_round_robin();
    test_weighted_least_conn();
    printf("test_pool: ALL PASS\n");
    return 0;
}
