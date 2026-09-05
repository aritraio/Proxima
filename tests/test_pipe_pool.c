#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include "metrics.h"
#include "pipe_pool.h"

int main(void)
{
    struct pipe_pool pp;
    int p1[2], p2[2], p3[2];

    metrics_reset(&g_metrics);

    assert(pipe_pool_init(&pp, 0) == 0 && pp.cap == 0 && pp.count == 0);
    assert(pipe_pool_borrow(&pp, p1) == 0);
    assert(pp.starves == 1 && g_metrics.pipe_pool_starves == 1);
    pipe_pool_destroy(&pp);

    assert(pipe_pool_init(&pp, 2) == 0 && pp.cap == 2 && pp.count == 2);
    assert(pipe_pool_borrow(&pp, p1) == 1);
    assert(pp.count == 1);
    assert(pipe_pool_borrow(&pp, p2) == 1);
    assert(pp.count == 0);

    /* Pool exhausted -> borrow fails without blocking */
    assert(pipe_pool_borrow(&pp, p3) == 0);
    assert(pp.starves == 1);

    /* Release one pair back */
    pipe_pool_release(&pp, p1[0], p1[1]);
    assert(pp.count == 1);

    /* Borrow again succeeds */
    assert(pipe_pool_borrow(&pp, p3) == 1);
    assert(p3[0] == p1[0] && p3[1] == p1[1]);
    assert(pp.count == 0);

    pipe_pool_release(&pp, p2[0], p2[1]);
    pipe_pool_release(&pp, p3[0], p3[1]);
    assert(pp.count == 2);

    pipe_pool_destroy(&pp);
    assert(pp.pairs == NULL && pp.count == 0);

    printf("test_pipe_pool: ALL PASS\n");
    return 0;
}
