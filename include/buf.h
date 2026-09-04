/*
 * buf.h -- mbuf: the single linear byte buffer every data path uses.
 *
 * Design goal: *one* buffer per direction of travel per connection
 * (client->upstream and upstream->client).  Bytes are read() into the
 * buffer once and written out of it directly; payload is never copied
 * between buffers.  The only memory traffic is:
 *
 *   1. a memmove() when free space is fragmented (see mbuf_compact),
 *      bounded by buffer capacity and amortized over reads, and
 *   2. the head-region copy performed by the proxy when it rewrites
 *      request/response heads (headers only, never the body).
 *
 * The buffer is LINEAR (not a ring): data lives contiguously in
 * [start, end).  That is what lets the HTTP parser run over it with
 * plain offsets and lets callers hand a single contiguous span to
 * read()/write().  nginx chains page buffers; HAProxy uses rings; we
 * deliberately take the simpler linear-compacting design so the parse
 * state machine never has to deal with wraparound.  If profiling later
 * shows the memmove matters, the swap-in is a ring with a 2-iov
 * writev() drain; the parser API does not change because parsing only
 * ever happens while the buffer is not being consumed (see http.h).
 *
 * Capacity policy: mbuf_init(cap, grow).  With grow=0 the buffer is
 * hard-capped (relay buffers, stage buffer) -- callers react to
 * "no space" by applying backpressure (stop reading the peer) or by
 * failing the request (431).  With grow=1 it may double geometrically.
 * Allocation is amortized O(1) and never happens *during* header
 * parsing of an individual token.
 *
 * COMPACTION-SAFETY INVARIANT (DESIGN.md section 5.1, hazard 2):
 * mbuf_compact() relocates live bytes to index 0, which invalidates
 * every byte offset (struct px_range) an http_parser has recorded into
 * this buffer.  Therefore compact()/consume() MUST NOT run while a head
 * is being parsed (http_parse() may still return HPS_NEED_MORE):
 *
 *   - while a head is being parsed, reads append through the GROW-ONLY
 *     path (mbuf_reserve_append(), a realloc -- safe, because every
 *     http_parse() call re-derives its base pointer from the current
 *     mbuf_head(), so ranges recorded by the *final* call are the only
 *     ones that matter);
 *   - compact() runs only at safe points: between messages (the
 *     keep-alive loopback in conn.c, which also re-establishes the
 *     "fresh request starts at index 0" invariant) and while streaming
 *     a body (no parser ranges are live).
 *
 * TIERED ALLOCATION (hazard 1): a buffer with data == NULL / cap == 0
 * is "unallocated".  conn.c uses this state to defer u2c + stage until
 * CS_SELECT (see conn.h / DESIGN.md section 2.2); mbuf_destroy()
 * returns the buffer to this state so it can be re-initialized later.
 */
#ifndef PX_BUF_H
#define PX_BUF_H

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>   /* struct iovec, in case a 2-segment drain is added */

struct mbuf {
    uint8_t *data;
    size_t   cap;      /* allocated size */
    size_t   start;    /* first live byte */
    size_t   end;      /* one past last live byte */
    uint8_t  grow;     /* != 0: allow geometric realloc */
};

/* Allocation state predicates. */
static inline int mbuf_is_allocated(const struct mbuf *m)
{
    return m->data != NULL;
}
static inline int mbuf_is_empty(const struct mbuf *m)
{
    return m->start == m->end;
}

/* cap is a *minimum*; allocation happens here, once, up front. */
void     mbuf_init(struct mbuf *m, size_t cap, int grow);

/* Free storage and return the buffer to the unallocated state
 * (data == NULL, cap == 0) so it can be mbuf_init()'d again. */
void     mbuf_destroy(struct mbuf *m);

/* Live bytes in [start, end). */
size_t   mbuf_len(const struct mbuf *m);

/* Free writable space currently at the tail: cap - end.  Callers that
 * find 0 should either mbuf_compact() (if start > 0) or stop reading
 * (backpressure). */
size_t   mbuf_tail_space(const struct mbuf *m);

/* Move live bytes to the front so that tail space becomes contiguous.
 * No-op if start == 0.  O(len) memmove, amortized.
 * SAFE-POINT RULE: may only run between messages and while streaming a
 * body -- NEVER while http_parse() may still return HPS_NEED_MORE on
 * this buffer (see the compaction-safety invariant above). */
void     mbuf_compact(struct mbuf *m);

/* Ensure at least `need` contiguous free bytes at the tail.  Compacts
 * first (same safe-point rule as mbuf_compact); if grow is set, doubles
 * capacity until it fits.  Returns PX_OK or PX_ERR (no space, grow==0).
 * BODY-PHASE ONLY: never call while a parser holds ranges into the
 * live region -- use mbuf_reserve_append() there instead. */
int      mbuf_reserve(struct mbuf *m, size_t need);

/* Ensure `need` free bytes at the tail WITHOUT compacting: grows the
 * allocation (realloc, preserves the logical byte sequence) or fails.
 * THE ONLY growth path permitted while a parser is mid-message
 * (HPS_NEED_MORE), because compaction would invalidate px_range
 * offsets while a realloc cannot -- parsers re-derive their base from
 * mbuf_head() on every feed.  Returns PX_OK or PX_ERR (grow == 0). */
int      mbuf_reserve_append(struct mbuf *m, size_t need);

/* Write into the tail after a successful mbuf_reserve(). */
uint8_t *mbuf_tail(struct mbuf *m);

/* Declare that the producer appended n bytes at the tail. */
void     mbuf_commit(struct mbuf *m, size_t n);

/* Read-only view of the live head. */
const uint8_t *mbuf_head(const struct mbuf *m);

/* Drop the first n live bytes (they were written out / consumed). */
void     mbuf_consume(struct mbuf *m, size_t n);

/* Convenience: append a span, returning PX_OK / PX_ERR.  Used by the
 * head rewriter (the only place bytes are copied deliberately). */
int      mbuf_append(struct mbuf *m, const void *src, size_t n);

void     mbuf_reset(struct mbuf *m);   /* drop everything, keep storage */

#endif /* PX_BUF_H */
