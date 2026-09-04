/*
 * pipe_pool.h -- process-wide pool of non-blocking pipe pairs backing
 * the splice(2) zero-copy fast path (see DESIGN.md section 14).
 *
 * Why a pool at all: splice() needs a kernel pipe per direction, and
 * pipe2() costs 2 fds + an inode allocation per call.  At 10k conns a
 * naive per-conn pipe would double descriptor usage and hammer the
 * kernel.  Instead:
 *
 *   * pipe_pool_init() creates `max_pairs` pipe pairs once, up front,
 *     each O_NONBLOCK|O_CLOEXEC.
 *   * A conn BORROWS a pair when it enters CS_RELAY (the only phase in
 *     which a splice fast path applies) and RELEASES it when the relay
 *     finishes, disconnects, or the conn returns to keep-alive idle.
 *   * Borrow is a freelist pop: no syscalls in the hot path.
 *   * If the pool is EMPTY the caller MUST fall back to ordinary mbuf
 *     streaming -- never block, never drop traffic.  The fallback is
 *     silent except for the g_metrics.pipe_pool_starves counter, which
 *     tells you the pool is undersized.
 *
 * Sizing rule of thumb: pairs = min(max_conns / 4, 1024).  Splice is
 * only engaged for bodies above a size threshold (config, default 0 =
 * disabled until the M7 experiment), so not every relay needs a pair.
 */
#ifndef PX_PIPE_POOL_H
#define PX_PIPE_POOL_H

#include <stddef.h>
#include <stdint.h>

/* LIFO freelist.  `pairs` holds 2*count ints: [r0,w0,r1,w1,...].
 * count == number of pairs currently available (stack top). */
struct pipe_pool {
    int      *pairs;      /* 2 * cap ints */
    size_t    cap;        /* max pairs (0 = disabled) */
    size_t    count;      /* available now */
    uint32_t  starves;    /* borrow attempts that hit empty (mirrors
                             g_metrics.pipe_pool_starves for tests) */
};

/* Create the pool.  max_pairs == 0 disables the pool entirely (mbuf
 * path always).  Returns 0 or -1 with errno set. */
int  pipe_pool_init(struct pipe_pool *pp, size_t max_pairs);

/* Pop one pair.  out[0] = read end, out[1] = write end.
 * Returns 1 on success, 0 if the pool is empty (caller falls back to
 * mbuf streaming; the starve counter is bumped here).  Never blocks. */
int  pipe_pool_borrow(struct pipe_pool *pp, int out[2]);

/* Push a pair back.  Idempotent-free contract: each borrowed pair is
 * released exactly once; releasing a pair the pool does not own (or
 * twice) is a bug -- guard with the fd values (-1 after release). */
void pipe_pool_release(struct pipe_pool *pp, int r, int w);

void pipe_pool_destroy(struct pipe_pool *pp);

/* Single instance per process (same lifecycle as g_metrics). */
extern struct pipe_pool g_pipe_pool;

#endif /* PX_PIPE_POOL_H */
