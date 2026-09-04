/*
 * header_smoke.c -- M0/M0.5 gate: every public header must compile
 * cleanly (-Wall -Wextra -Wpedantic -Werror) and the enums, structs,
 * invariants and inline defensive helpers the design depends on must be
 * sane.  Nothing here calls project *functions* (only static inlines),
 * so it links with no sources -- it exists to catch header drift before
 * implementation starts (and on hosts without epoll).
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

/* --- compile-time layout invariants -------------------------------- */
_Static_assert(sizeof(struct px_range) == 8, "px_range must be 2x u32");
_Static_assert(PX_STAGE_CAP_FACTOR == 2, "stage sizing contract");
_Static_assert(PX_MAX_HEADERS <= 255, "hdr idx sentinel is 0xFF");
_Static_assert(FD_ROLE_COUNT == 2, "one conn = client + upstream");
_Static_assert(CS_ACCEPTED == 0 && CS_DONE == 7, "conn_state ordering");
_Static_assert(HTTP_M_UNKNOWN == 0, "method enum base");

/* Hazard 7: metrics struct is cacheline-aligned and the hot counters
 * live in the first cacheline (cold section starts at a 64 B boundary). */
_Static_assert(_Alignof(struct px_metrics) == 64,
               "px_metrics must sit on its own cacheline");
_Static_assert(sizeof(struct px_metrics) % 64 == 0,
               "px_metrics must not share cachelines with neighbors");
_Static_assert(offsetof(struct px_metrics, upstream_connects) % 64 == 0,
               "cold metrics section must start on a cacheline");

/* Hazard 2: mbuf keeps a distinguishable unallocated state so conn.c can
 * defer u2c/stage allocation until CS_SELECT. */
_Static_assert(offsetof(struct mbuf, data) == 0, "data first in mbuf");
_Static_assert(offsetof(struct conn, c2u) < offsetof(struct conn, u2c),
               "c2u precedes u2c in struct conn");
_Static_assert(offsetof(struct conn, u2c) < offsetof(struct conn, stage),
               "u2c precedes stage in struct conn");

/* Hazard 1: a conn that has not reached CS_SELECT must be able to hold
 * an unallocated u2c/stage -- enforced by mbuf_init()/destroy() state. */

/* --- helpers ------------------------------------------------------- */
static int check_unique_enum(const char *name, const int *vals, size_t n)
{
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (vals[i] == vals[j]) {
                fprintf(stderr, "FAIL %s: %d duplicated\n", name, vals[i]);
                return 0;
            }
    printf("ok   %s (%zu values unique)\n", name, n);
    return 1;
}

static int check_mbuf_states(void)
{
    struct mbuf m;
    memset(&m, 0, sizeof m);              /* what conn_accept leaves for
                                              u2c/stage: unallocated */
    if (mbuf_is_allocated(&m) || !mbuf_is_empty(&m)) {
        fprintf(stderr, "FAIL mbuf zero state not unallocated/empty\n");
        return 0;
    }
    m.data = (uint8_t *)1; m.cap = 64; m.start = 0; m.end = 5;
    if (!mbuf_is_allocated(&m) || mbuf_is_empty(&m)) {
        fprintf(stderr, "FAIL mbuf populated state misread\n");
        return 0;
    }
    printf("ok   mbuf allocated/empty state predicates\n");
    return 1;
}

/* Hazard 2: the bounds backstop must accept a coherent parser state and
 * reject offset corruption (what compaction-under-the-parser produces). */
static int check_parser_bounds(void)
{
    struct http_parser p;
    memset(&p, 0, sizeof p);
    p.pos = 40;                       /* 40 bytes scanned so far */

    p.method = (struct px_range){0, 3};   /* "GET" at 0..3   */
    p.target = (struct px_range){4, 10};  /* target ends at 14 */
    p.version = (struct px_range){0, 0};  /* not yet (len 0) */
    p.nheaders = 2;
    p.hname[0] = (struct px_range){15, 6};    /* ends at 21 */
    p.hvalue[0] = (struct px_range){22, 12};  /* ends at 34 */
    p.hname[1] = (struct px_range){35, 4};    /* ends at 39 */
    p.hvalue[1] = (struct px_range){0, 0};    /* value not scanned yet */

    if (!http_parser_ranges_in_bounds(&p, 40))
        goto bad;
    if (!http_parser_ranges_in_bounds(&p, 41))   /* slack live len ok */
        goto bad;

    /* Corrupt one range so it ends past the scan cursor: this is what a
     * mid-parse mbuf_compact() would do to recorded offsets. */
    p.hvalue[1].off = 38;
    p.hvalue[1].len = 8;                    /* 38+8 = 46 > pos = 40 */
    if (http_parser_ranges_in_bounds(&p, 40))
        goto bad;

    /* PX_RANGE_NONE sentinel on an unset token must be accepted. */
    p = (struct http_parser){0};
    p.pos = 10;
    p.method = PX_RANGE_NONE;
    if (!http_parser_ranges_in_bounds(&p, 10))
        goto bad;

    printf("ok   http_parser_ranges_in_bounds catches corruption\n");
    return 1;
bad:
    fprintf(stderr, "FAIL http_parser_ranges_in_bounds\n");
    return 0;
}

/* Hazard 5: node accounting must never underflow. */
static int check_node_accounting(void)
{
    struct server_node n;
    memset(&n, 0, sizeof n);
    n.weight = 2;

    node_begin(&n);
    if (n.active != 1) { fprintf(stderr, "FAIL node_begin\n"); return 0; }
    node_end(&n, 1);                 /* clean completion */
    if (n.active != 0 || n.total != 1 || n.anomalies != 0) {
        fprintf(stderr, "FAIL clean node_end accounting\n");
        return 0;
    }
    node_end(&n, 1);                 /* buggy double end */
    if (n.active != 0 || n.total != 1 || n.anomalies != 1) {
        fprintf(stderr, "FAIL double node_end not guarded\n");
        return 0;
    }
    node_begin(&n);
    node_end(&n, 0);                 /* failed request */
    if (n.active != 0 || n.failed != 1) {
        fprintf(stderr, "FAIL failed node_end accounting\n");
        return 0;
    }
    if (node_lc_score(&n) != 0) { fprintf(stderr, "FAIL lc score\n"); return 0; }
    n.active = 4;                    /* score(weight 2) = 4*1000/2 = 2000 */
    if (node_lc_score(&n) != 2000) { fprintf(stderr, "FAIL lc weighted\n"); return 0; }
    n.weight = 0;                    /* defensive clamp, must not div0 */
    if (node_lc_score(&n) != 4000) { fprintf(stderr, "FAIL lc weight clamp\n"); return 0; }

    printf("ok   node accounting underflow-guarded, WLC clamped\n");
    return 1;
}

/* Hazard 7: RED classification buckets + guarded gauge. */
static int check_metrics(void)
{
    struct px_metrics m;
    memset(&m, 0, sizeof m);

    metrics_record_request(&m);
    metrics_record_status(&m, 200);
    metrics_record_status(&m, 100);
    metrics_record_status(&m, 302);
    metrics_record_status(&m, 404);
    metrics_record_status(&m, 503);
    metrics_record_status(&m, 0);     /* aborted: no status sent */
    if (m.req_total != 1 || m.req_2xx != 1 || m.req_1xx != 1 ||
        m.req_3xx != 1 || m.req_4xx != 1 || m.req_5xx != 1 ||
        m.internal_errors != 1) {
        fprintf(stderr, "FAIL metrics classification\n");
        return 0;
    }
    metrics_conn_open(&m);
    metrics_conn_open(&m);
    metrics_conn_close(&m);
    metrics_conn_close(&m);
    metrics_conn_close(&m);           /* buggy extra close */
    if (m.active_conns != 0 || m.accounting_anomalies != 1) {
        fprintf(stderr, "FAIL metrics gauge underflow guard\n");
        return 0;
    }
    if (_Alignof(struct px_metrics) != 64) {
        fprintf(stderr, "FAIL metrics alignment\n");
        return 0;
    }
    printf("ok   metrics RED buckets + guarded gauge (size %zu)\n",
           sizeof m);
    return 1;
}

int main(void)
{
    int ok = 1;

    /* PX_RANGE_NONE sentinel (compound literal: can't live in a static
     * assert, so verified here). */
    if (PX_RANGE_NONE.off != UINT32_MAX || PX_RANGE_NONE.len != 0) {
        fprintf(stderr, "FAIL px_range sentinel\n");
        ok = 0;
    }

    {
        static const int v[] = { CS_ACCEPTED, CS_READ_REQ, CS_SELECT,
                                 CS_CONNECT, CS_SEND_REQ_HEAD, CS_RELAY,
                                 CS_CLOSING, CS_DONE };
        ok &= check_unique_enum("conn_state", v, 8);
    }
    {
        static const int v[] = { HSS_RL_START, HSS_RL_METHOD, HSS_RL_SP1,
                                 HSS_RL_TARGET, HSS_RL_SP2, HSS_RL_VER,
                                 HSS_RL_VER_MAJOR, HSS_RL_VER_DOT,
                                 HSS_RL_VER_MINOR, HSS_RL_CR,
                                 HSS_ST_VER, HSS_ST_VER_MAJOR,
                                 HSS_ST_VER_DOT, HSS_ST_VER_MINOR,
                                 HSS_ST_SP1, HSS_ST_CODE, HSS_ST_SP2,
                                 HSS_ST_REASON, HSS_ST_CR,
                                 HSS_HDR_LINE_START, HSS_HDR_NAME,
                                 HSS_HDR_OWS, HSS_HDR_COLON,
                                 HSS_HDR_VALUE_OWS, HSS_HDR_VALUE,
                                 HSS_HDR_CR, HSS_HDR_LF, HSS_DONE,
                                 HSS_ERROR };
        ok &= check_unique_enum("http_scan_state", v, 29);
    }
    {
        static const int v[] = { BF_NONE, BF_LENGTH, BF_CHUNKED,
                                 BF_UNTIL_EOF };
        ok &= check_unique_enum("body_framing", v, 4);
    }
    {
        static const int v[] = { PX_OK, PX_AGAIN, PX_EOF, PX_ERR };
        ok &= check_unique_enum("px_result", v, 4);
    }
    {
        static const int v[] = { LOOP_RUNNING, LOOP_DRAINING, LOOP_STOPPED };
        ok &= check_unique_enum("loop_phase", v, 3);
    }
    {
        static const int v[] = { BAL_ROUND_ROBIN, BAL_WEIGHTED_LEAST_CONN };
        ok &= check_unique_enum("bal_algo", v, 2);
    }
    {
        static const int v[] = { NH_DOWN, NH_UP, NH_CHECKING };
        ok &= check_unique_enum("node_health", v, 3);
    }

    ok &= check_mbuf_states();
    ok &= check_parser_bounds();
    ok &= check_node_accounting();
    ok &= check_metrics();

    /* Conn layout sanity: the three buffers exist in tier order. */
    {
        struct conn c;
        memset(&c, 0, sizeof c);
        c.cfd = c.ufd = c.pipe_r = c.pipe_w = -1;
        (void)c;
        printf("ok   struct conn (%zu bytes) layout referenced\n", sizeof c);
    }

    printf(ok ? "header smoke: ALL PASS\n" : "header smoke: FAIL\n");
    return ok ? 0 : 1;
}
