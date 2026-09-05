#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "proxy.h"

void
mbuf_init(struct mbuf *m, size_t cap, int grow)
{
    *m = (struct mbuf){0};
    m->grow = grow != 0;
    if (cap != 0) {
        m->data = malloc(cap);
        if (m->data != NULL)
            m->cap = cap;
    }
}

void
mbuf_destroy(struct mbuf *m)
{
    free(m->data);
    *m = (struct mbuf){0};
}

size_t
mbuf_len(const struct mbuf *m)
{
    return m->end - m->start;
}

size_t
mbuf_tail_space(const struct mbuf *m)
{
    return m->cap - m->end;
}

void
mbuf_compact(struct mbuf *m)
{
    size_t len = mbuf_len(m);

    if (m->start != 0 && len != 0)
        memmove(m->data, m->data + m->start, len);
    m->start = 0;
    m->end = len;
}

static int
mbuf_grow(struct mbuf *m, size_t need)
{
    size_t cap = m->cap == 0 ? 1 : m->cap;
    uint8_t *data;

    while (cap - m->end < need) {
        if (cap > SIZE_MAX / 2) {
            cap = SIZE_MAX;
            if (cap - m->end < need)
                return PX_ERR;
            break;
        }
        cap *= 2;
    }
    data = realloc(m->data, cap);
    if (data == NULL)
        return PX_ERR;
    m->data = data;
    m->cap = cap;
    return PX_OK;
}

int
mbuf_reserve(struct mbuf *m, size_t need)
{
    mbuf_compact(m);
    if (need <= mbuf_tail_space(m))
        return PX_OK;
    if (!m->grow)
        return PX_ERR;
    return mbuf_grow(m, need);
}

int
mbuf_reserve_append(struct mbuf *m, size_t need)
{
    if (need <= mbuf_tail_space(m))
        return PX_OK;
    if (!m->grow)
        return PX_ERR;
    return mbuf_grow(m, need);
}

uint8_t *
mbuf_tail(struct mbuf *m)
{
    return m->data == NULL ? NULL : m->data + m->end;
}

void
mbuf_commit(struct mbuf *m, size_t n)
{
    if (n <= mbuf_tail_space(m))
        m->end += n;
}

const uint8_t *
mbuf_head(const struct mbuf *m)
{
    return m->data == NULL ? NULL : m->data + m->start;
}

void
mbuf_consume(struct mbuf *m, size_t n)
{
    size_t len = mbuf_len(m);

    if (n > len)
        return;
    m->start += n;
    if (m->start == m->end)
        m->start = m->end = 0;
}

int
mbuf_append(struct mbuf *m, const void *src, size_t n)
{
    if (n != 0 && src == NULL)
        return PX_ERR;
    if (mbuf_reserve(m, n) != PX_OK)
        return PX_ERR;
    if (n != 0) {
        memcpy(mbuf_tail(m), src, n);
        m->end += n;
    }
    return PX_OK;
}

void
mbuf_reset(struct mbuf *m)
{
    m->start = 0;
    m->end = 0;
}
