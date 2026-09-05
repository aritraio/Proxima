#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "http.h"

static enum http_parse_status feed_bytes(struct http_parser *p, const char *s,
                                         size_t limit)
{
    size_t i, n = strlen(s);
    enum http_parse_status result = HPS_NEED_MORE;
    for (i = 1; i <= n; i++) {
        result = http_parse(p, (const uint8_t *)s, i, limit);
        assert(http_parser_ranges_in_bounds(p, i));
        if (result != HPS_NEED_MORE) break;
    }
    return result;
}

static void test_request(void)
{
    static const char request[] = "POST /a?q=1 HTTP/1.1\r\nHost: example\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\nbody";
    struct http_parser p;
    struct http_msg msg;
    http_parser_init(&p, HTTP_REQUEST);
    assert(feed_bytes(&p, request, 1024) == HPS_DONE);
    assert(p.nheaders == 3 && p.content_length == 4 && p.expect_continue);
    assert(http_resolve(&msg, &p, HTTP_M_UNKNOWN) == 1);
    assert(msg.framing == BF_LENGTH && msg.body_len == 4 && msg.expect_continue);
    assert(msg.head_len < strlen(request));
}

static void test_edges(void)
{
    struct http_parser p;
    struct http_msg msg;
    http_parser_init(&p, HTTP_REQUEST);
    assert(feed_bytes(&p, "GET / HTTP/1.1\nHost: x\n\n", 128) == HPS_DONE);
    assert(http_resolve(&msg, &p, HTTP_M_UNKNOWN) == 1 && msg.keep_alive);
    http_parser_init(&p, HTTP_REQUEST);
    assert(feed_bytes(&p, "GET / HTTP/1.1\r\nHost: x\r\n bad: fold\r\n\r\n", 128) == HPS_ERROR);
    assert(p.err_status == 400);
    http_parser_init(&p, HTTP_REQUEST);
    assert(feed_bytes(&p, "GET / HTTP/1.1\r\n\r\n", 128) == HPS_DONE);
    assert(http_resolve(&msg, &p, HTTP_M_UNKNOWN) == 0 && p.err_status == 400);
    http_parser_init(&p, HTTP_REQUEST);
    assert(feed_bytes(&p, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", 128) == HPS_DONE);
    assert(http_resolve(&msg, &p, HTTP_M_UNKNOWN) == 1 && msg.keep_alive);
    http_parser_init(&p, HTTP_REQUEST);
    assert(feed_bytes(&p, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 128) == HPS_ERROR);
    assert(p.err_status == 400);
    http_parser_init(&p, HTTP_REQUEST);
    assert(feed_bytes(&p, "GET /very-long HTTP/1.1\r\n", 8) == HPS_ERROR);
    assert(p.err_status == 414);
    http_parser_init(&p, HTTP_REQUEST);
    assert(feed_bytes(&p, "GET / HTTP/1.1\r\nHost: x\r\nX: long\r\n", 24) == HPS_ERROR);
    assert(p.err_status == 431);
}

static void test_framing(void)
{
    struct http_parser p;
    struct http_msg msg;
    http_parser_init(&p, HTTP_RESPONSE);
    assert(feed_bytes(&p, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n", 256) == HPS_DONE);
    assert(http_resolve(&msg, &p, HTTP_M_GET) == 1 && msg.framing == BF_LENGTH);
    assert(http_resolve(&msg, &p, HTTP_M_HEAD) == 1 && msg.framing == BF_NONE);
    http_parser_init(&p, HTTP_RESPONSE);
    assert(feed_bytes(&p, "HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\r\n\r\n", 256) == HPS_DONE);
    assert(http_resolve(&msg, &p, HTTP_M_GET) == 1 && msg.framing == BF_NONE);
    http_parser_init(&p, HTTP_RESPONSE);
    assert(feed_bytes(&p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 99\r\n\r\n", 256) == HPS_DONE);
    assert(http_resolve(&msg, &p, HTTP_M_GET) == 1 && msg.framing == BF_CHUNKED);
    http_parser_init(&p, HTTP_RESPONSE);
    assert(feed_bytes(&p, "HTTP/1.0 200 OK\r\n\r\n", 256) == HPS_DONE);
    assert(http_resolve(&msg, &p, HTTP_M_GET) == 1 && msg.framing == BF_UNTIL_EOF);
}

static void test_chunks(void)
{
    struct chunk_watch w;
    static const char body[] = "4\r\nWiki\r\n5;foo=bar\r\npedia\r\n0\r\nX-T: yes\r\n\r\n";
    size_t i;
    chunk_watch_init(&w);
    for (i = 0; i < sizeof body - 1; i++) {
        enum chunk_feed_result r = chunk_watch_feed(&w, (const uint8_t *)body + i, 1);
        assert(r == (i + 1 == sizeof body - 1 ? CHF_END : CHF_MORE));
    }
    chunk_watch_init(&w);
    assert(chunk_watch_feed(&w, (const uint8_t *)"Z\r\n", 3) == CHF_ERROR);
    chunk_watch_init(&w);
    assert(chunk_watch_feed(&w, (const uint8_t *)"\r\n", 2) == CHF_ERROR);
}

int main(void)
{
    char reply[PX_ERR_REPLY_CAP];
    struct http_parser p;
    test_request();
    test_edges();
    test_framing();
    test_chunks();
    assert(http_build_error_reply(reply, sizeof reply, 400, 0) > 0);
    assert(strstr(reply, "400 Bad Request") != NULL);

    /* Verify chunked flag is pure 0 when Connection: close is parsed */
    http_parser_init(&p, HTTP_REQUEST);
    assert(feed_bytes(&p, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 128) == HPS_DONE);
    assert(p.chunked == 0);

    printf("test_http: ALL PASS\n");
    return 0;
}
