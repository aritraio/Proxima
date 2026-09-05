#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "metrics.h"
#include "pipe_pool.h"

struct pipe_pool g_pipe_pool;

static int make_nonblocking_pipe(int fds[2])
{
#if defined(__linux__) && defined(O_CLOEXEC)
    return pipe2(fds, O_NONBLOCK | O_CLOEXEC);
#else
    int flags;
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        flags = fcntl(fds[i], F_GETFL, 0);
        if (flags < 0 || fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fds[0]); close(fds[1]);
            return -1;
        }
        flags = fcntl(fds[i], F_GETFD, 0);
        if (flags < 0 || fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC) < 0) {
            close(fds[0]); close(fds[1]);
            return -1;
        }
    }
    return 0;
#endif
}

int pipe_pool_init(struct pipe_pool *pp, size_t max_pairs)
{
    size_t i;
    int fds[2];

    if (pp == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(pp, 0, sizeof *pp);
    if (max_pairs == 0) {
        return 0;
    }

    pp->pairs = calloc(max_pairs * 2, sizeof(int));
    if (pp->pairs == NULL) return -1;
    pp->cap = max_pairs;
    pp->count = 0;
    pp->starves = 0;

    for (i = 0; i < max_pairs; i++) {
        if (make_nonblocking_pipe(fds) != 0) {
            pipe_pool_destroy(pp);
            return -1;
        }
        pp->pairs[i * 2] = fds[0];
        pp->pairs[i * 2 + 1] = fds[1];
        pp->count++;
    }

    return 0;
}

int pipe_pool_borrow(struct pipe_pool *pp, int out[2])
{
    if (pp == NULL || pp->count == 0) {
        if (pp != NULL) pp->starves++;
        metrics_record_pipe_starve(&g_metrics);
        return 0;
    }

    pp->count--;
    out[0] = pp->pairs[pp->count * 2];
    out[1] = pp->pairs[pp->count * 2 + 1];
    return 1;
}

void pipe_pool_release(struct pipe_pool *pp, int r, int w)
{
    if (pp == NULL || r < 0 || w < 0) return;

    if (pp->count < pp->cap) {
        pp->pairs[pp->count * 2] = r;
        pp->pairs[pp->count * 2 + 1] = w;
        pp->count++;
    } else {
        close(r);
        close(w);
    }
}

void pipe_pool_destroy(struct pipe_pool *pp)
{
    size_t i;
    if (pp == NULL || pp->pairs == NULL) return;

    for (i = 0; i < pp->count; i++) {
        close(pp->pairs[i * 2]);
        close(pp->pairs[i * 2 + 1]);
    }
    free(pp->pairs);
    memset(pp, 0, sizeof *pp);
}
