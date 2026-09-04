/*
 * http.h -- incremental, zero-allocation HTTP/1.1 head parser.
 *
 * Contract (this is what makes it "state machine without dynamic string
 * reallocations"):
 *
 *  * The parser is a pure byte scanner.  It holds a cursor `pos` and
 *    optionally a few range offsets into the buffer being scanned.
 *    It NEVER allocates, NEVER copies, and NEVER touches the bytes.
 *  * The connection code feeds it the live region of an mbuf.  While a
 *    head is being parsed the connection code does NOT consume from the
 *    buffer, so "offset relative to buffer start" is stable across
 *    calls.  Between calls the buffer may only grow at the tail (new
 *    reads); if the tail is full the caller compacts, which is safe
 *    because nothing was consumed yet.
 *  * Parsing stops exactly at the end of the head (blank line).  The
 *    caller then consumes `head_len` bytes and handles the body by
 *    framing rules (below), with whatever body bytes were over-read
 *    already sitting at the front of the buffer -- zero copies.
 *
 * Limits: if a head exceeds `head_limit` before terminating, the parser
 * errors with 431 (headers) / 414 (request line) as appropriate.
 *
 * The parser handles both requests and responses (kind field) because
 * a proxy parses BOTH ends of the same connection.
 */
#ifndef PX_HTTP_H
#define PX_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "proxy.h"

enum http_kind {
    HTTP_REQUEST = 0,
    HTTP_RESPONSE
};

/* Incremental scanner states.  Exposed (rather than hidden in a .c) so
 * unit tests can drive the machine byte-by-byte and assert exact state
 * after each feed.  Req-line and status-line states share the space but
 * only the ones matching parser->kind are ever entered. */
enum http_scan_state {
    /* --- request line: "GET /x HTTP/1.1" ------------------------ */
    HSS_RL_START = 0,        /* expecting a token (method) */
    HSS_RL_METHOD,
    HSS_RL_SP1,              /* after method */
    HSS_RL_TARGET,
    HSS_RL_SP2,              /* after target */
    HSS_RL_VER,              /* inside "HTTP/" */
    HSS_RL_VER_MAJOR,
    HSS_RL_VER_DOT,
    HSS_RL_VER_MINOR,
    HSS_RL_CR,               /* saw CR, expect LF */
    /* --- status line: "HTTP/1.1 200 OK" ------------------------- */
    HSS_ST_VER,
    HSS_ST_VER_MAJOR,
    HSS_ST_VER_DOT,
    HSS_ST_VER_MINOR,
    HSS_ST_SP1,              /* after version */
    HSS_ST_CODE,
    HSS_ST_SP2,              /* after code (reason is optional) */
    HSS_ST_REASON,
    HSS_ST_CR,
    /* --- shared header block ------------------------------------ */
    HSS_HDR_LINE_START,      /* first char of a header line (name or CR) */
    HSS_HDR_NAME,
    HSS_HDR_OWS,             /* optional whitespace before ':' (tolerated) */
    HSS_HDR_COLON,
    HSS_HDR_VALUE_OWS,       /* optional whitespace after ':' */
    HSS_HDR_VALUE,
    HSS_HDR_CR,              /* value terminated by CR */
    HSS_HDR_LF,              /* line done: fold? no (obs-fold => 400) */
    /* --- message head complete ----------------------------------- */
    HSS_DONE,                /* blank line consumed; parser returns HPS_DONE */
    HSS_ERROR
};

enum http_method {
    HTTP_M_UNKNOWN = 0,
    HTTP_M_GET, HTTP_M_HEAD, HTTP_M_POST, HTTP_M_PUT,
    HTTP_M_DELETE, HTTP_M_OPTIONS, HTTP_M_PATCH
};

/* How the message body is delimited once the head is parsed.
 * RFC 7230 section 3.3.3, resolved for the specific link we are on. */
enum body_framing {
    BF_NONE = 0,       /* no body allowed: HEAD, 1xx, 204, 304 */
    BF_LENGTH,         /* Content-Length present */
    BF_CHUNKED,        /* Transfer-Encoding: chunked */
    BF_UNTIL_EOF       /* no framing: body ends when the peer closes */
};

enum http_parse_status {
    HPS_NEED_MORE = 0,  /* scan this head fragment again with more bytes */
    HPS_DONE,           /* head (status line + headers) fully consumed */
    HPS_ERROR           /* parser->err_status holds the code to reply */
};

/* The scanner.  `state` doubles as a diagnostic for unit tests. */
struct http_parser {
    enum http_kind kind;
    uint32_t       state;        /* enum http_scan_state */
    size_t         pos;          /* bytes scanned so far from message start */
    uint16_t       nheaders;     /* completed header lines */
    uint16_t       status;       /* response status code (0 until parsed) */
    uint8_t        http_major;
    uint8_t        http_minor;
    uint8_t        chunked;          /* TE contained "chunked" (requests) */
    uint8_t        expect_continue;  /* "Expect: 100-continue" (requests) */
    uint16_t       err_status;       /* 0, or 400/414/431/505 on HPS_ERROR */

    /* Ranges into the scanned buffer: {off,len} from message start.
     * method/version/reason are only meaningful per parser kind. */
    struct px_range method;
    struct px_range target;     /* request: request-target (incl. query) */
    struct px_range version;    /* e.g. "HTTP/1.1" */
    struct px_range reason;     /* response: reason phrase */
    struct px_range hname[PX_MAX_HEADERS];   /* each header line's name  */
    struct px_range hvalue[PX_MAX_HEADERS];  /* each header line's value */

    /* Index (into hname/hvalue) of headers the proxy must act on, or
     * PX_INDEX_NONE.  Looked up at parse time so the rewriter and the
     * framing logic don't rescan. */
    uint8_t idx_host;
    uint8_t idx_content_length;
    uint8_t idx_transfer_encoding;
    uint8_t idx_connection;
    uint8_t idx_expect;
    uint8_t idx_x_forwarded_for;
    uint8_t idx_upgrade;

    uint64_t content_length;    /* raw Content-Length value if seen */
};

void http_parser_init(struct http_parser *p, enum http_kind kind);
void http_parser_reset(struct http_parser *p);   /* reuse for next message */

/*
 * Scan bytes [base .. base+len).  The scanner only looks at bytes from
 * p->pos onward, so callers may append between calls.  Returns:
 *   HPS_NEED_MORE  - head incomplete; buffer may grow and we re-feed.
 *   HPS_DONE       - blank line seen; p->pos == total head length.
 *   HPS_ERROR      - malformed or over limit; reply p->err_status.
 *
 * head_limit is the caller's configured max head size; the parser
 * errors 431 (or 414 for an oversized request line) when exceeded.
 */
enum http_parse_status http_parse(struct http_parser *p,
                                  const uint8_t *base, size_t len,
                                  size_t head_limit);

/* ------------------------------------------------------------------ */
/* Resolved view of a completed head, filled by http_resolve().        */
/* Everything the connection state machine needs to route and frame.   */
/* ------------------------------------------------------------------ */
struct http_msg {
    enum http_method  method;       /* requests */
    uint16_t          status;       /* responses */
    uint8_t           http_major;
    uint8_t           http_minor;
    uint8_t           keep_alive;   /* keep-alive semantics ON THE LINK the
                                       message was received on (version +
                                       Connection tokens resolved) */
    uint8_t           expect_continue;
    enum body_framing framing;      /* how the body on THIS link ends */
    uint8_t           has_body;
    uint64_t          body_len;     /* BF_LENGTH: Content-Length value */
    size_t            head_len;     /* == parser->pos; consume this many
                                       bytes before handling the body */
};

/*
 * Resolve framing + keep-alive once the head is done.
 *  - kind request / response
 *  - `req_method` is the request method (responses only, so HEAD knows
 *    it must not expect a body).
 * The parser may need a second pass for response 1xx interim messages:
 * http_resolve returns 0 when the parsed message is an interim 1xx that
 * the caller should skip (it forwards or drops it, then re-inits the
 * parser at the same buffer offset and continues).  1 otherwise.
 */
int http_resolve(struct http_msg *out, struct http_parser *p,
                 enum http_method req_method);

/* ------------------------------------------------------------------ */
/* Body-end watchers.                                                  */
/*                                                                     */
/* The proxy relays bodies VERBATIM (chunk framing and all) so it does */
/* not decode chunked streams -- but to know when a chunked body has    */
/* ended (to resume the keep-alive machine, or to detect truncation)    */
/* it must *scan* the relayed stream.  chunk_watch is that incremental  */
/* scanner: feed it exactly the bytes relayed for the body; it returns  */
/* CHF_END the moment the final chunk + optional trailers + CRLF have   */
/* been consumed, and the caller may then treat any further bytes as    */
/* belonging to the *next* message (pipelining).                        */
/* ------------------------------------------------------------------ */
struct chunk_watch {
    uint32_t state;     /* internal */
    uint64_t rem;       /* payload bytes remaining in the current chunk */
    uint8_t  saw_end;
};

enum chunk_feed_result {
    CHF_MORE = 0,   /* keep feeding */
    CHF_END,        /* body terminally consumed; stop feeding */
    CHF_ERROR       /* malformed chunk stream */
};

void chunk_watch_init(struct chunk_watch *w);
enum chunk_feed_result chunk_watch_feed(struct chunk_watch *w,
                                        const uint8_t *p, size_t n);

/* Utility for error pages / 100-continue: build a small fixed reply.
 * dst must hold PX_ERR_REPLY_CAP bytes.  Returns bytes written. */
#define PX_ERR_REPLY_CAP 512
size_t http_build_error_reply(char *dst, size_t cap, int status,
                              uint8_t keep_alive);

#endif /* PX_HTTP_H */
