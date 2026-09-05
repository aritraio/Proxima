#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "metrics.h"

int main(void)
{
    char buf[4096];
    size_t len;

    metrics_reset(&g_metrics);
    assert(g_metrics.req_total == 0);
    assert(g_metrics.active_conns == 0);

    /* Connection open/close accounting & underflow guard */
    metrics_conn_open(&g_metrics);
    assert(g_metrics.active_conns == 1);
    metrics_conn_close(&g_metrics);
    assert(g_metrics.active_conns == 0);
    /* Extra close must bump anomaly, never wrap */
    metrics_conn_close(&g_metrics);
    assert(g_metrics.active_conns == 0);
    assert(g_metrics.accounting_anomalies == 1);

    /* Request and status recording */
    metrics_record_request(&g_metrics);
    metrics_record_status(&g_metrics, 200);

    metrics_record_request(&g_metrics);
    metrics_record_status(&g_metrics, 502);

    metrics_record_request(&g_metrics);
    metrics_record_status(&g_metrics, 404);

    metrics_record_request(&g_metrics);
    metrics_record_status(&g_metrics, 301);

    metrics_record_request(&g_metrics);
    metrics_record_status(&g_metrics, 101);

    metrics_record_request(&g_metrics);
    metrics_record_status(&g_metrics, 0); /* aborted mid-flight -> internal error */

    metrics_record_request(&g_metrics);
    metrics_record_internal_error(&g_metrics);

    /* Invariant: req_total == 1xx + 2xx + 3xx + 4xx + 5xx + internal_errors */
    assert(g_metrics.req_total == 7);
    assert(g_metrics.req_1xx == 1);
    assert(g_metrics.req_2xx == 1);
    assert(g_metrics.req_3xx == 1);
    assert(g_metrics.req_4xx == 1);
    assert(g_metrics.req_5xx == 1);
    assert(g_metrics.internal_errors == 2);
    assert(g_metrics.req_total == (g_metrics.req_1xx + g_metrics.req_2xx +
                                  g_metrics.req_3xx + g_metrics.req_4xx +
                                  g_metrics.req_5xx + g_metrics.internal_errors));

    /* Latency accumulators */
    metrics_record_upstream_connect(&g_metrics, 1000);
    metrics_record_upstream_connect(&g_metrics, 3000);
    assert(g_metrics.upstream_connects == 2);
    assert(g_metrics.upstream_connect_ns_sum == 4000);
    assert(g_metrics.upstream_connect_ns_max == 3000);

    /* Bytes & starves */
    metrics_record_bytes(&g_metrics, 500, 1500);
    assert(g_metrics.rx_bytes == 500 && g_metrics.tx_bytes == 1500);
    metrics_record_pipe_starve(&g_metrics);
    assert(g_metrics.pipe_pool_starves == 1);

    /* Render test */
    len = metrics_render(buf, sizeof buf);
    assert(len > 0 && len < sizeof buf);
    assert(strstr(buf, "pxlb_requests_total 7") != NULL);
    assert(strstr(buf, "pxlb_responses{class=\"2xx\"} 1") != NULL);
    assert(strstr(buf, "pxlb_responses{class=\"5xx\"} 1") != NULL);
    assert(strstr(buf, "pxlb_internal_errors 2") != NULL);
    assert(strstr(buf, "pxlb_upstream_connect_duration_ns_mean 2000") != NULL);
    assert(strstr(buf, "pxlb_upstream_connect_duration_ns_max 3000") != NULL);
    assert(strstr(buf, "pxlb_accounting_anomalies 1") != NULL);
    assert(strstr(buf, "pxlb_pipe_pool_starves 1") != NULL);

    printf("test_metrics: ALL PASS\n");
    return 0;
}
