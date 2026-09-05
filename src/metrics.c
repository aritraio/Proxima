#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "metrics.h"

struct px_metrics g_metrics;

void metrics_reset(struct px_metrics *m)
{
    if (m == NULL) return;
    memset(m, 0, sizeof *m);
}

size_t metrics_render(char *dst, size_t cap)
{
    int n;
    size_t total = 0;
    uint64_t mean_lat_ns = 0;

    if (dst == NULL || cap == 0) return 0;

    if (g_metrics.upstream_connects > 0) {
        mean_lat_ns = g_metrics.upstream_connect_ns_sum / g_metrics.upstream_connects;
    }

    n = snprintf(dst + total, cap - total,
        "# HELP pxlb_requests_total Total resolved HTTP requests\n"
        "# TYPE pxlb_requests_total counter\n"
        "pxlb_requests_total %" PRIu64 "\n"
        "# HELP pxlb_responses HTTP responses by status class\n"
        "# TYPE pxlb_responses counter\n"
        "pxlb_responses{class=\"1xx\"} %" PRIu64 "\n"
        "pxlb_responses{class=\"2xx\"} %" PRIu64 "\n"
        "pxlb_responses{class=\"3xx\"} %" PRIu64 "\n"
        "pxlb_responses{class=\"4xx\"} %" PRIu64 "\n"
        "pxlb_responses{class=\"5xx\"} %" PRIu64 "\n"
        "# HELP pxlb_internal_errors Requests aborted with no client-visible status\n"
        "# TYPE pxlb_internal_errors counter\n"
        "pxlb_internal_errors %" PRIu64 "\n"
        "# HELP pxlb_rx_bytes Total payload and head bytes received from clients\n"
        "# TYPE pxlb_rx_bytes counter\n"
        "pxlb_rx_bytes %" PRIu64 "\n"
        "# HELP pxlb_tx_bytes Total payload and head bytes sent to clients\n"
        "# TYPE pxlb_tx_bytes counter\n"
        "pxlb_tx_bytes %" PRIu64 "\n"
        "# HELP pxlb_active_conns Active client connections\n"
        "# TYPE pxlb_active_conns gauge\n"
        "pxlb_active_conns %" PRIu64 "\n"
        "# HELP pxlb_upstream_connects_total Total completed upstream TCP connects\n"
        "# TYPE pxlb_upstream_connects_total counter\n"
        "pxlb_upstream_connects_total %" PRIu64 "\n"
        "# HELP pxlb_upstream_connect_duration_ns_mean Mean upstream connect duration in nanoseconds\n"
        "# TYPE pxlb_upstream_connect_duration_ns_mean gauge\n"
        "pxlb_upstream_connect_duration_ns_mean %" PRIu64 "\n"
        "# HELP pxlb_upstream_connect_duration_ns_max Max upstream connect duration in nanoseconds\n"
        "# TYPE pxlb_upstream_connect_duration_ns_max gauge\n"
        "pxlb_upstream_connect_duration_ns_max %" PRIu64 "\n"
        "# HELP pxlb_accounting_anomalies Internal accounting anomalies\n"
        "# TYPE pxlb_accounting_anomalies counter\n"
        "pxlb_accounting_anomalies %" PRIu64 "\n"
        "# HELP pxlb_pipe_pool_starves Splicing pipe pool exhaustion events\n"
        "# TYPE pxlb_pipe_pool_starves counter\n"
        "pxlb_pipe_pool_starves %" PRIu64 "\n",
        g_metrics.req_total,
        g_metrics.req_1xx,
        g_metrics.req_2xx,
        g_metrics.req_3xx,
        g_metrics.req_4xx,
        g_metrics.req_5xx,
        g_metrics.internal_errors,
        g_metrics.rx_bytes,
        g_metrics.tx_bytes,
        g_metrics.active_conns,
        g_metrics.upstream_connects,
        mean_lat_ns,
        g_metrics.upstream_connect_ns_max,
        g_metrics.accounting_anomalies,
        g_metrics.pipe_pool_starves
    );

    if (n < 0) {
        dst[0] = '\0';
        return 0;
    }
    if ((size_t)n >= cap) {
        return cap - 1;
    }
    return (size_t)n;
}
