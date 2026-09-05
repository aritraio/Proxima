#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "buf.h"
#include "proxy.h"

int main(void)
{
    struct mbuf m;
    const char abc[] = "abc";
    const char defgh[] = "defgh";

    mbuf_init(&m, 0, 1);
    assert(!mbuf_is_allocated(&m) && mbuf_is_empty(&m));
    assert(mbuf_reserve_append(&m, 3) == PX_OK);
    assert(m.cap >= 3 && m.data != NULL);
    assert(mbuf_append(&m, abc, 3) == PX_OK);
    assert(mbuf_len(&m) == 3 && memcmp(mbuf_head(&m), abc, 3) == 0);
    mbuf_consume(&m, 2);
    assert(m.start == 2 && mbuf_len(&m) == 1);
    assert(mbuf_reserve(&m, 4) == PX_OK);
    assert(m.start == 0 && mbuf_len(&m) == 1 && mbuf_head(&m)[0] == 'c');
    assert(mbuf_append(&m, defgh, 5) == PX_OK);
    assert(mbuf_len(&m) == 6 && memcmp(mbuf_head(&m), "cdefgh", 6) == 0);
    mbuf_reset(&m);
    assert(mbuf_is_empty(&m) && mbuf_is_allocated(&m));
    mbuf_destroy(&m);
    assert(!mbuf_is_allocated(&m) && m.cap == 0 && m.start == 0 && m.end == 0);

    mbuf_init(&m, 4, 1);
    assert(mbuf_append(&m, "abcd", 4) == PX_OK);
    mbuf_consume(&m, 2);
    assert(mbuf_reserve_append(&m, 3) == PX_OK);
    assert(m.start == 2 && memcmp(mbuf_head(&m), "cd", 2) == 0);
    mbuf_destroy(&m);

    mbuf_init(&m, 4, 0);
    assert(mbuf_append(&m, "abcd", 4) == PX_OK);
    assert(mbuf_append(&m, "e", 1) == PX_ERR);
    mbuf_consume(&m, 2);
    assert(mbuf_reserve(&m, 2) == PX_OK);
    assert(m.start == 0 && mbuf_tail_space(&m) == 2);
    mbuf_commit(&m, 3); /* Bounds violation is ignored. */
    assert(m.end == 2);
    mbuf_destroy(&m);
    printf("test_buf: ALL PASS\n");
    return 0;
}
