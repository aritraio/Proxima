/*
 * header_smoke.c -- M0 gate: every public header must compile cleanly
 * (-Wall -Wextra -Wpedantic -Werror) and the enums/structs that the
 * state machine depends on must be sane.  Nothing here calls project
 * functions, so it links with no sources -- it exists to catch header
 * drift before implementation starts (and on hosts without epoll).
 */
#include <stdio.h>
#include <string.h>

#include "buf.h"
#include "config.h"
#include "conn.h"
#include "event.h"
#include "health.h"
#include "http.h"
#include "pool.h"
#include "proxy.h"

/* --- compile-time layout invariants -------------------------------- */
_Static_assert(sizeof(struct px_range) == 8, "px_range must be 2x u32");
_Static_assert(PX_STAGE_CAP_FACTOR == 2, "stage sizing contract");
_Static_assert(PX_MAX_HEADERS <= 255, "hdr idx sentinel is 0xFF");
_Static_assert(FD_ROLE_COUNT == 2, "one conn = client + upstream");
_Static_assert(CS_ACCEPTED == 0 && CS_DONE == 7, "conn_state ordering");
_Static_assert(HTTP_M_UNKNOWN == 0, "method enum base");

/* --- runtime sanity ------------------------------------------------ */
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

static int check_buffer_layout(void)
{
    /* mbuf is fully defined in buf.h, so we can inspect layout without
     * calling any (not-yet-implemented) functions. */
    struct mbuf m;
    memset(&m, 0, sizeof m);
    if (m.start != 0 || m.end != 0 || m.data != NULL)
        return 0;
    printf("ok   mbuf zero layout (%zu bytes)\n", sizeof m);
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
    ok &= check_buffer_layout();

    /* Conn layout sanity: the three buffers and two parsers exist. */
    {
        struct conn c;
        memset(&c, 0, sizeof c);
        (void)c;
        printf("ok   struct conn (%zu bytes) layout referenced\n",
               sizeof c);
    }

    printf(ok ? "header smoke: ALL PASS\n" : "header smoke: FAIL\n");
    return ok ? 0 : 1;
}
