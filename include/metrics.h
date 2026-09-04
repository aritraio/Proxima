/*
 * metrics.h -- process-wide SRE RED counters (single writer: the
 * reactor thread; readers: metrics_render() on the internal route or a
 * SIGUSR1 dump, both executed IN the reactor thread between epoll
 * waits, so no atomics are required).
 *
 * Multi-process note: with SO_REUSEPORT workers each process owns its
 * own `g_metrics` instance and its own metrics endpoint; there is
 * intentionally no cross-process aggregation in v1.
 *
 * Layout: the struct is cacheline-aligned.  All hot counters (written
 * once per event) sit in the first cacheline; cold/control fields are
 * pushed into the second via _Alignas(64).  This keeps the reader's
 * snapshot coherent when the metrics are later dumped by another
 * thread, and documents intent even though v1 is single-threaded.
 *
 * Definitions (Prometheus text rendered by metrics_render):
 *   pxlb_requests_total              -- requests whose head was resolved
 *   pxlb_responses{class=1xx..5xx}   -- client-visible statuses
 *   pxlb_internal_errors             -- requests that died with NO
 *                                        client-visible status (aborted
 *                                        mid-flight / accounting bugs).
 *                                        requests_total == sum(classes)
 *                                        + internal_errors, always.
 *   pxlb_rx_bytes / pxlb_tx_bytes
 *   pxlb_active_conns                -- gauge (open/close guarded)
 *   pxlb_upstream_connects{sum,count,max} -- connect latency ns (RED
 *                                        latency = sum/count)
 *   pxlb_accounting_anomalies        -- node_end() underflow etc. (hazard 5)
 *   pxlb_pipe_pool_starves           -- splice pool exhausted -> mbuf fallback
 */
#ifndef PX_METRICS_H
#define PX_METRICS_H

#include <stddef.h>
#include <stdint.h>

/* Internal inspection endpoint.  A GET for exactly this path is
 * intercepted at request resolution (CS_SELECT) and answered from the
 * proxy itself -- no backend is selected, no node accounting happens.
 * Rendered with metrics_render() into `stage`, zero-allocation. */
#define PX_METRICS_PATH     "/_proxima/metrics"
#define PX_METRICS_PATH_LEN (sizeof(PX_METRICS_PATH) - 1)

struct px_metrics {
    /* ---- hot cacheline: written once per request/byte ---- */
    _Alignas(64)
    uint64_t req_total;      /* requests whose head was fully resolved  */
    uint64_t req_1xx;        /* client-visible responses by class      */
    uint64_t req_2xx;
    uint64_t req_3xx;
    uint64_t req_4xx;
    uint64_t req_5xx;
    uint64_t internal_errors;    /* aborted with no client-visible status */
    uint64_t rx_bytes;           /* payload+head bytes from clients      */
    uint64_t tx_bytes;           /* payload+head bytes to clients        */
    uint64_t active_conns;       /* gauge: conn_open/conn_close          */

    /* ---- cold cacheline: latency / anomaly accumulators ---- */
    _Alignas(64)
    uint64_t upstream_connects;      /* count */
    uint64_t upstream_connect_ns_sum;/* sum   -> mean latency (RED) */
    uint64_t upstream_connect_ns_max;/* max   -> tail signal         */
    uint64_t accounting_anomalies;   /* double node_end(), etc.      */
    uint64_t pipe_pool_starves;      /* splice pool empty -> mbuf    */
    uint64_t started_ms;             /* monotonic start of this proc */
};

/* Single instance per process (see header comment).  Defined in
 * src/metrics.c; never referenced by header-inline helpers so headers
 * stay link-clean for the smoke test. */
extern struct px_metrics g_metrics;

void metrics_reset(struct px_metrics *m);

/* --- hot-path recorders (static inline: zero call overhead) -------- */

/* Count one request whose head was fully resolved.  Call exactly once
 * per request, at CS_SELECT time. */
static inline void metrics_record_request(struct px_metrics *m)
{
    m->req_total++;
}

/* Record the client-visible status actually sent for a request.  code
 * == 0 means the exchange ended with no status (aborted mid-flight);
 * that lands in internal_errors, never in a class bucket, so the
 * invariant requests_total == sum(classes) + internal_errors holds. */
static inline void metrics_record_status(struct px_metrics *m, int code)
{
    switch (code / 100) {
    case 1: m->req_1xx++; break;
    case 2: m->req_2xx++; break;
    case 3: m->req_3xx++; break;
    case 4: m->req_4xx++; break;
    case 5: m->req_5xx++; break;
    default:
        m->internal_errors++;
        break;
    }
}

static inline void metrics_record_internal_error(struct px_metrics *m)
{
    m->internal_errors++;
}

static inline void metrics_record_bytes(struct px_metrics *m,
                                        uint64_t rx, uint64_t tx)
{
    m->rx_bytes += rx;
    m->tx_bytes += tx;
}

static inline void metrics_conn_open(struct px_metrics *m)
{
    m->active_conns++;
}

/* Guarded gauge decrement: closing a conn that was never counted is an
 * internal bug and must surface as an anomaly, never underflow. */
static inline void metrics_conn_close(struct px_metrics *m)
{
    if (m->active_conns > 0)
        m->active_conns--;
    else
        m->accounting_anomalies++;
}

/* Upstream connect latency, nanoseconds (mean = sum/count). */
static inline void metrics_record_upstream_connect(struct px_metrics *m,
                                                   uint64_t ns)
{
    m->upstream_connects++;
    m->upstream_connect_ns_sum += ns;
    if (ns > m->upstream_connect_ns_max)
        m->upstream_connect_ns_max = ns;
}

static inline void metrics_record_accounting_anomaly(struct px_metrics *m)
{
    m->accounting_anomalies++;
}

static inline void metrics_record_pipe_starve(struct px_metrics *m)
{
    m->pipe_pool_starves++;
}

/* --- rendering (implemented src/metrics.c, M5) ---------------------- */

/* Render Prometheus text/plain for g_metrics into dst[0..cap).
 * Zero-allocation: caller supplies a stack buffer (>= 4096).
 * Returns the number of bytes written (<= cap-1; truncated on
 * overflow -- a full dump must not allocate). */
size_t metrics_render(char *dst, size_t cap);

#endif /* PX_METRICS_H */
