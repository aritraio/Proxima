#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "http.h"

enum chunk_state {
    CWS_SIZE_FIRST, CWS_SIZE, CWS_EXT, CWS_SIZE_LF, CWS_DATA, CWS_DATA_CR, CWS_DATA_LF,
    CWS_TRAILER_START, CWS_TRAILER, CWS_TRAILER_CR, CWS_TRAILER_LF,
    CWS_DONE, CWS_ERROR
};

static int ascii_eq(const uint8_t *s, size_t n, const char *word)
{
    size_t i;
    for (i = 0; word[i] != '\0'; i++) {
        uint8_t c;
        if (i == n) return 0;
        c = s[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + ('a' - 'A'));
        if (c != (uint8_t)word[i]) return 0;
    }
    return i == n;
}

static int has_token(const uint8_t *s, size_t n, const char *word)
{
    size_t i = 0;
    while (i < n) {
        size_t begin, end;
        while (i < n && (s[i] == ',' || s[i] == ' ' || s[i] == '\t')) i++;
        begin = i;
        while (i < n && s[i] != ',') i++;
        end = i;
        while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
        if (ascii_eq(s + begin, end - begin, word)) return 1;
    }
    return 0;
}

static void parser_error(struct http_parser *p, uint16_t status)
{
    p->err_status = status;
    p->state = HSS_ERROR;
}

void http_parser_init(struct http_parser *p, enum http_kind kind)
{
    uint16_t i;
    memset(p, 0, sizeof *p);
    p->kind = kind;
    p->state = kind == HTTP_REQUEST ? HSS_RL_START : HSS_ST_VER;
    p->method = p->target = p->version = p->reason = PX_RANGE_NONE;
    for (i = 0; i < PX_MAX_HEADERS; i++)
        p->hname[i] = p->hvalue[i] = PX_RANGE_NONE;
    p->idx_host = p->idx_content_length = p->idx_transfer_encoding =
        p->idx_connection = p->idx_expect = p->idx_x_forwarded_for =
        p->idx_upgrade = PX_INDEX_NONE;
}

void http_parser_reset(struct http_parser *p)
{
    http_parser_init(p, p->kind);
}

static int valid_token(uint8_t c)
{
    return c > 32 && c < 127 && strchr("()<>@,;:\\\"/[]?={} \t", c) == NULL;
}

static enum http_method method_from_range(const struct http_parser *p,
                                          const uint8_t *base);

static void index_header(struct http_parser *p, const uint8_t *base, uint16_t i)
{
    struct px_range name = p->hname[i], value = p->hvalue[i];
    const uint8_t *v = base + value.off;
    uint64_t number = 0;
    size_t j;

    if (ascii_eq(base + name.off, name.len, "host")) p->idx_host = (uint8_t)i;
    else if (ascii_eq(base + name.off, name.len, "content-length")) {
        for (j = 0; j < value.len; j++) {
            if (v[j] < '0' || v[j] > '9' ||
                number > (UINT64_MAX - (uint64_t)(v[j] - '0')) / 10) {
                parser_error(p, 400); return;
            }
            number = number * 10 + (uint64_t)(v[j] - '0');
        }
        if (value.len == 0 || (p->idx_content_length != PX_INDEX_NONE &&
            p->content_length != number)) { parser_error(p, 400); return; }
        p->content_length = number;
        if (p->idx_content_length == PX_INDEX_NONE) p->idx_content_length = (uint8_t)i;
    } else if (ascii_eq(base + name.off, name.len, "transfer-encoding")) {
        p->idx_transfer_encoding = (uint8_t)i;
        if (has_token(v, value.len, "chunked")) p->chunked = 1;
    } else if (ascii_eq(base + name.off, name.len, "connection")) {
        p->idx_connection = (uint8_t)i;
        if (has_token(v, value.len, "close")) p->expect_continue |= 0x02u;
        if (has_token(v, value.len, "keep-alive")) p->expect_continue |= 0x04u;
    }
    else if (ascii_eq(base + name.off, name.len, "expect")) {
        p->idx_expect = (uint8_t)i;
        if (has_token(v, value.len, "100-continue")) p->expect_continue |= 0x01u;
    } else if (ascii_eq(base + name.off, name.len, "x-forwarded-for")) p->idx_x_forwarded_for = (uint8_t)i;
    else if (ascii_eq(base + name.off, name.len, "upgrade")) p->idx_upgrade = (uint8_t)i;
}

static void finish_header(struct http_parser *p, const uint8_t *base)
{
    struct px_range *v = &p->hvalue[p->nheaders];
    while (v->len != 0 && (base[v->off + v->len - 1] == ' ' ||
                            base[v->off + v->len - 1] == '\t')) v->len--;
    index_header(p, base, p->nheaders);
    if (p->state != HSS_ERROR) p->nheaders++;
}

enum http_parse_status http_parse(struct http_parser *p, const uint8_t *base,
                                  size_t len, size_t head_limit)
{
    if (p->state == HSS_DONE) return HPS_DONE;
    if (p->state == HSS_ERROR || base == NULL || p->pos > len) return HPS_ERROR;
    while (p->pos < len) {
        uint8_t c = base[p->pos++];
        if (p->pos > head_limit) { parser_error(p, (p->kind == HTTP_REQUEST &&
            p->target.off != UINT32_MAX && p->state <= HSS_RL_CR) ? 414 : 431); return HPS_ERROR; }
        switch (p->state) {
        case HSS_RL_START:
            if (!valid_token(c)) goto bad; p->method = (struct px_range){(uint32_t)p->pos - 1, 1}; p->state = HSS_RL_METHOD; break;
        case HSS_RL_METHOD:
            if (c == ' ') { p->status = (uint16_t)method_from_range(p, base); p->state = HSS_RL_SP1; }
            else if (valid_token(c)) p->method.len++;
            else goto bad;
            break;
        case HSS_RL_SP1:
            if (c == ' ') break;
            if (c == '\r' || c == '\n') goto bad;
            p->target = (struct px_range){(uint32_t)p->pos - 1, 1}; p->state = HSS_RL_TARGET; break;
        case HSS_RL_TARGET:
            if (c == ' ') p->state = HSS_RL_SP2;
            else if (c > 31 && c != 127) p->target.len++;
            else goto bad;
            break;
        case HSS_RL_SP2:
            if (c == ' ') break;
            if (c != 'H') goto bad;
            p->version = (struct px_range){(uint32_t)p->pos - 1, 1}; p->state = HSS_RL_VER; break;
        case HSS_RL_VER:
            if (c != 'T') goto bad; p->version.len++; p->state = HSS_RL_VER_MAJOR; break;
        case HSS_RL_VER_MAJOR:
            if (c != 'T') goto bad; p->version.len++; p->state = HSS_RL_VER_DOT; break;
        case HSS_RL_VER_DOT:
            if (c != 'P') goto bad; p->version.len++; p->state = HSS_RL_VER_MINOR; break;
        case HSS_RL_VER_MINOR:
            if (c != '/') goto bad; p->version.len++; p->state = HSS_RL_CR; break;
        case HSS_RL_CR:
            if (c >= '0' && c <= '9') { p->http_major = (uint8_t)(c - '0'); p->version.len++; p->state = HSS_ST_VER; }
            else goto bad;
            break;
        case HSS_ST_VER: /* request version major after HTTP/ */
            if (p->kind == HTTP_REQUEST) {
                if (c != '.') goto bad; p->version.len++; p->state = HSS_ST_VER_MAJOR;
            } else { if (c != 'H') goto bad; p->version = (struct px_range){(uint32_t)p->pos - 1, 1}; p->state = HSS_ST_VER_MAJOR; }
            break;
        case HSS_ST_VER_MAJOR:
            if (p->kind == HTTP_REQUEST) { if (c < '0' || c > '9') goto bad; p->http_minor = (uint8_t)(c - '0'); p->version.len++; p->state = HSS_ST_VER_DOT; }
            else { if (c != 'T') goto bad; p->version.len++; p->state = HSS_ST_VER_DOT; }
            break;
        case HSS_ST_VER_DOT:
            if (p->kind == HTTP_REQUEST) { if (c == '\r') p->state = HSS_ST_CR; else if (c == '\n') p->state = HSS_HDR_LINE_START; else goto bad; }
            else { if (c != 'T') goto bad; p->version.len++; p->state = HSS_ST_VER_MINOR; }
            break;
        case HSS_ST_VER_MINOR:
            if (c != 'P') goto bad; p->version.len++; p->state = HSS_ST_SP1; break;
        case HSS_ST_SP1:
            if (c != '/') goto bad; p->version.len++; p->state = HSS_ST_CODE; break;
        case HSS_ST_CODE:
            if (c < '0' || c > '9') goto bad; p->http_major = (uint8_t)(c - '0'); p->version.len++; p->state = HSS_ST_SP2; break;
        case HSS_ST_SP2:
            if (c != '.') goto bad; p->version.len++; p->state = HSS_ST_REASON; break;
        case HSS_ST_REASON:
            if (c < '0' || c > '9') goto bad; p->http_minor = (uint8_t)(c - '0'); p->version.len++; p->state = HSS_HDR_OWS; break;
        case HSS_HDR_NAME: /* response status digits, or a header name */
            if (p->kind == HTTP_RESPONSE && p->hname[p->nheaders].off == UINT32_MAX &&
                p->status < 100) {
                if (c < '0' || c > '9') goto bad;
                p->status = (uint16_t)(p->status * 10 + c - '0');
                if (p->status >= 100) p->state = HSS_HDR_OWS;
            } else if (c == ':') p->state = HSS_HDR_VALUE_OWS;
            else if (c == ' ' || c == '\t') p->state = HSS_HDR_OWS;
            else if (valid_token(c)) p->hname[p->nheaders].len++;
            else goto bad;
            break;
        case HSS_HDR_OWS: /* response: status separator, or header OWS */
            if (p->kind == HTTP_RESPONSE && p->hname[p->nheaders].off == UINT32_MAX) {
                if (p->status == 0) { if (c != ' ') goto bad; p->state = HSS_HDR_NAME; }
                else if (c == ' ') { p->reason = (struct px_range){(uint32_t)p->pos, 0}; p->state = HSS_HDR_COLON; }
                else if (c == '\r') p->state = HSS_ST_CR;
                else if (c == '\n') p->state = HSS_HDR_LINE_START;
                else goto bad;
                break;
            }
            if (c == ' ' || c == '\t') break;
            if (c != ':') goto bad;
            p->state = HSS_HDR_VALUE_OWS;
            break;
        case HSS_HDR_COLON: /* response reason */
            if (p->kind == HTTP_RESPONSE) { if (c == '\r') p->state = HSS_ST_CR; else if (c == '\n') p->state = HSS_HDR_LINE_START; else { if (c < 32 && c != '\t') goto bad; p->reason.len++; } break; }
            goto bad;
        case HSS_ST_CR:
            if (c != '\n') goto bad; p->state = HSS_HDR_LINE_START; break;
        case HSS_HDR_LINE_START:
            if (c == '\r') { p->state = HSS_HDR_LF; break; }
            if (c == '\n') { p->state = HSS_DONE; return HPS_DONE; }
            if (c == ' ' || c == '\t' || !valid_token(c) || p->nheaders == PX_MAX_HEADERS) goto bad;
            p->hname[p->nheaders] = (struct px_range){(uint32_t)p->pos - 1, 1}; p->state = HSS_HDR_NAME; break;
        case HSS_HDR_VALUE_OWS:
            if (c == ' ' || c == '\t') break;
            p->hvalue[p->nheaders] = (struct px_range){(uint32_t)p->pos - 1, 0};
            if (c == '\r') { finish_header(p, base); if (p->state == HSS_ERROR) return HPS_ERROR; p->state = HSS_HDR_CR; break; }
            if (c == '\n') { finish_header(p, base); if (p->state == HSS_ERROR) return HPS_ERROR; p->state = HSS_HDR_LINE_START; break; }
            if (c < 32 && c != '\t') goto bad; p->hvalue[p->nheaders].len = 1; p->state = HSS_HDR_VALUE; break;
        case HSS_HDR_VALUE:
            if (c == '\r') { finish_header(p, base); if (p->state == HSS_ERROR) return HPS_ERROR; p->state = HSS_HDR_CR; }
            else if (c == '\n') { finish_header(p, base); if (p->state == HSS_ERROR) return HPS_ERROR; p->state = HSS_HDR_LINE_START; }
            else if (c < 32 && c != '\t') goto bad;
            else p->hvalue[p->nheaders].len++;
            break;
        case HSS_HDR_CR:
            if (c != '\n') goto bad; p->state = HSS_HDR_LINE_START; break;
        case HSS_HDR_LF:
            if (c != '\n') goto bad; p->state = HSS_DONE; return HPS_DONE;
        default: goto bad;
        }
    }
    return HPS_NEED_MORE;
bad:
    parser_error(p, 400);
    return HPS_ERROR;
}

static enum http_method method_from_range(const struct http_parser *p,
                                          const uint8_t *base)
{
    const struct px_range r = p->method;
    const uint8_t *s = base + r.off;
    if (ascii_eq(s, r.len, "get")) return HTTP_M_GET;
    if (ascii_eq(s, r.len, "head")) return HTTP_M_HEAD;
    if (ascii_eq(s, r.len, "post")) return HTTP_M_POST;
    if (ascii_eq(s, r.len, "put")) return HTTP_M_PUT;
    if (ascii_eq(s, r.len, "delete")) return HTTP_M_DELETE;
    if (ascii_eq(s, r.len, "options")) return HTTP_M_OPTIONS;
    if (ascii_eq(s, r.len, "patch")) return HTTP_M_PATCH;
    return HTTP_M_UNKNOWN;
}

int http_resolve(struct http_msg *out, struct http_parser *p,
                 enum http_method req_method)
{
    uint8_t close = (p->expect_continue & 0x02u) != 0;
    uint8_t keep = (p->expect_continue & 0x04u) != 0;

    if (p->state != HSS_DONE) return 0;
    memset(out, 0, sizeof *out);
    out->method = p->kind == HTTP_REQUEST ? (enum http_method)p->status : HTTP_M_UNKNOWN;
    out->status = p->kind == HTTP_RESPONSE ? p->status : 0;
    out->http_major = p->http_major;
    out->http_minor = p->http_minor;
    out->expect_continue = p->expect_continue & 0x01u;
    out->head_len = p->pos;
    /* Token values were resolved while parsing only where they affect body
     * framing.  Connection values need no rescan for the common defaults. */
    if (p->http_major > 1 || (p->http_major == 1 && p->http_minor >= 1))
        out->keep_alive = close == 0;
    else
        out->keep_alive = keep != 0;
    if (p->kind == HTTP_REQUEST && p->http_major == 1 && p->http_minor >= 1 &&
        p->idx_host == PX_INDEX_NONE) { p->err_status = 400; return 0; }
    if (p->kind == HTTP_RESPONSE && p->status >= 100 && p->status < 200)
        return 0;
    if ((p->kind == HTTP_RESPONSE && (req_method == HTTP_M_HEAD ||
        p->status == 204 || p->status == 304)) ||
        (p->kind == HTTP_RESPONSE && p->status >= 100 && p->status < 200))
        out->framing = BF_NONE;
    else if (p->chunked != 0)
        out->framing = BF_CHUNKED;
    else if (p->idx_content_length != PX_INDEX_NONE) {
        out->framing = BF_LENGTH;
        out->body_len = p->content_length;
    } else if (p->kind == HTTP_RESPONSE)
        out->framing = BF_UNTIL_EOF;
    else
        out->framing = BF_NONE;
    out->has_body = out->framing == BF_CHUNKED || out->framing == BF_UNTIL_EOF ||
        (out->framing == BF_LENGTH && out->body_len != 0);
    return 1;
}

void chunk_watch_init(struct chunk_watch *w)
{
    *w = (struct chunk_watch){ CWS_SIZE_FIRST, 0, 0 };
}

enum chunk_feed_result chunk_watch_feed(struct chunk_watch *w,
                                        const uint8_t *p, size_t n)
{
    size_t i;
    if (w->state == CWS_DONE) return CHF_END;
    if (w->state == CWS_ERROR) return CHF_ERROR;
    for (i = 0; i < n; i++) {
        uint8_t c = p[i];
        switch (w->state) {
        case CWS_SIZE_FIRST:
            if (c >= '0' && c <= '9') c = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') c = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') c = (uint8_t)(c - 'A' + 10);
            else goto chunk_bad;
            w->rem = c;
            w->state = CWS_SIZE;
            break;
        case CWS_SIZE:
            if (c >= '0' && c <= '9') c = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') c = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') c = (uint8_t)(c - 'A' + 10);
            else if (c == ';') { w->state = CWS_EXT; break; }
            else if (c == '\r') { w->state = CWS_SIZE_LF; break; }
            else goto chunk_bad;
            if (w->rem > (UINT64_MAX - c) / 16) goto chunk_bad;
            w->rem = w->rem * 16 + c; break;
        case CWS_EXT:
            if (c == '\r') w->state = CWS_SIZE_LF;
            else if (c == '\n') goto chunk_bad;
            break;
        case CWS_SIZE_LF:
            if (c != '\n') goto chunk_bad;
            w->state = w->rem == 0 ? CWS_TRAILER_START : CWS_DATA; break;
        case CWS_DATA:
            if (--w->rem == 0) w->state = CWS_DATA_CR;
            break;
        case CWS_DATA_CR:
            if (c != '\r') goto chunk_bad; w->state = CWS_DATA_LF; break;
        case CWS_DATA_LF:
            if (c != '\n') goto chunk_bad; w->state = CWS_SIZE_FIRST; w->rem = 0; break;
        case CWS_TRAILER_START:
            if (c == '\r') w->state = CWS_TRAILER_CR;
            else if (c == '\n' || c < 32) goto chunk_bad;
            else w->state = CWS_TRAILER;
            break;
        case CWS_TRAILER:
            if (c == '\r') w->state = CWS_TRAILER_LF;
            else if (c == '\n' || c < 32) goto chunk_bad;
            break;
        case CWS_TRAILER_LF:
            if (c != '\n') goto chunk_bad; w->state = CWS_TRAILER_START; break;
        case CWS_TRAILER_CR:
            if (c != '\n') goto chunk_bad; w->state = CWS_DONE; w->saw_end = 1; return CHF_END;
        default: goto chunk_bad;
        }
    }
    return CHF_MORE;
chunk_bad:
    w->state = CWS_ERROR;
    return CHF_ERROR;
}

size_t http_build_error_reply(char *dst, size_t cap, int status, uint8_t keep_alive)
{
    const char *reason = "Internal Server Error";
    int n;
    if (status == 400) reason = "Bad Request";
    else if (status == 408) reason = "Request Timeout";
    else if (status == 414) reason = "URI Too Long";
    else if (status == 431) reason = "Request Header Fields Too Large";
    else if (status == 502) reason = "Bad Gateway";
    n = snprintf(dst, cap, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: %s\r\n\r\n",
                 status, reason, keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}
