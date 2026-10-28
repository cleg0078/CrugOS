/* Host tests for the hardened kernel heap.
 *
 * The kernel page allocator is not part of the host binary, so this file owns
 * the fake PMM: an eight megabyte, page aligned arena carved out of .bss and a
 * bitmap that allocates contiguous page runs from it.  Identity mapping means
 * pmm_phys_to_virt() (a header inline) needs no stub.
 *
 * Panic paths (double free, interior large pointer, misaligned free) cannot be
 * asserted from the host because the shared panic stub traps the process, so
 * the structural guarantees are asserted through the non-panicking
 * heap_check() walk instead.
 */
#include <crug/cpu.h>
#include <crug/heap.h>
#include <crug/mm.h>
#include <crug/string.h>
#include "test.h"

#define FAKE_PAGES 2048u

static u8 g_arena[FAKE_PAGES * 4096] __attribute__((aligned(4096)));
static u8 g_used[FAKE_PAGES / 8u];

static int fake_bit(u32 idx)
{
    return (g_used[idx / 8u] >> (idx % 8u)) & 1u;
}

static void fake_mark(u32 idx, int on)
{
    if (on)
        g_used[idx / 8u] |= (u8)(1u << (idx % 8u));
    else
        g_used[idx / 8u] &= (u8)~(1u << (idx % 8u));
}

static u64 fake_alloc(u64 pages)
{
    u64 run = 0;
    u64 i;

    if (pages == 0 || pages > FAKE_PAGES)
        return 0;
    for (i = 0; i < FAKE_PAGES; i++) {
        if (fake_bit((u32)i)) {
            run = 0;
            continue;
        }
        run++;
        if (run == pages) {
            u64 start = i + 1u - pages;
            u64 k;

            for (k = 0; k < pages; k++)
                fake_mark((u32)(start + k), 1);
            return (u64)(uintptr)(g_arena + start * 4096u);
        }
    }
    return 0;
}

static void fake_free(u64 pa, u64 pages)
{
    u64 base = (u64)(uintptr)g_arena;
    u64 k;

    if (pa < base || (pa - base) % 4096u != 0)
        return;
    if ((pa - base) / 4096u + pages > FAKE_PAGES)
        return;
    for (k = 0; k < pages; k++)
        fake_mark((u32)((pa - base) / 4096u + k), 0);
}

u64 pmm_alloc_page(void)
{
    return fake_alloc(1);
}

u64 pmm_alloc_pages(u64 count)
{
    return fake_alloc(count);
}

void pmm_free_page(u64 pa)
{
    fake_free(pa, 1);
}

void pmm_free_pages(u64 pa, u64 count)
{
    fake_free(pa, count);
}

/* Exposed for tests/host/test_vm_space.c: the number of arena pages still
 * free lets an address-space test assert that create/map/destroy do not leak
 * frames or table pages. */
u64 host_pmm_free_pages_count(void)
{
    u64 free_pages = 0;
    u64 i;

    for (i = 0; i < FAKE_PAGES; i++) {
        if (!fake_bit((u32)i))
            free_pages++;
    }
    return free_pages;
}

static void *page_of(const void *p)
{
    return (void *)(uintptr)((u64)(uintptr)p & CRUG_PAGE_MASK);
}

/* Layout of the two headers, mirrored here so the tests can tamper with the
 * exact fields the kernel validates. */
struct host_slab {
    u64 cookie;
    u16 class_idx;
    u16 objects;
    u16 free_count;
    u16 in_use;
    u64 used[4];
    void *free_list;
    struct host_slab *next;
};

struct host_large {
    u64 cookie;
    u64 size;
    u64 pages;
    void *next;
    void *prev;
    u64 reserved[3];
};

void test_heap_functions(void)
{
    u32 slabs = 0;
    u32 objects = 0;
    u32 large = 0;
    u8 *a;
    u8 *b;
    u8 *big;
    u8 *big2;
    struct host_slab *sh;
    struct host_large *lh;
    u64 before;

    printf("- kernel heap\n");
    heap_init();
    CHECK(heap_check(&slabs, &objects, &large) == HEAP_CHECK_OK);
    CHECK(slabs == 0u && objects == 0u && large == 0u);

    CHECK(kmalloc(0) == 0);
    CHECK(kmalloc(HEAP_MAX_ALLOC + 1u) == 0);

    before = heap_used_bytes();
    a = (u8 *)kmalloc(24);
    b = (u8 *)kmalloc(24);
    big = (u8 *)kmalloc(9000);
    big2 = (u8 *)kmalloc(12000);
    CHECK(a != 0 && b != 0 && big != 0 && big2 != 0);
    CHECK(heap_used_bytes() == before + 32u + 32u + 9000u + 12000u);
    CHECK(heap_check(&slabs, &objects, &large) == HEAP_CHECK_OK);
    CHECK(slabs == 1u && objects == 2u && large == 2u);

    /* A damaged slab cookie is rejected before any object is handed out. */
    sh = (struct host_slab *)page_of(a);
    CHECK(sh->class_idx == 1u);
    {
        u64 saved = sh->cookie;

        sh->cookie = 0;
        CHECK(heap_check(0, 0, 0) == HEAP_CHECK_SLAB_COOKIE);
        sh->cookie = saved;
        CHECK(heap_check(0, 0, 0) == HEAP_CHECK_OK);
    }

    /* A damaged large-block cookie is found by the list walk. */
    lh = (struct host_large *)(big - 64);
    CHECK(lh->size == 9000u);
    {
        u64 saved = lh->cookie;

        lh->cookie = 0;
        CHECK(heap_check(0, 0, 0) == HEAP_CHECK_LARGE_COOKIE);
        lh->cookie = saved;
        CHECK(heap_check(0, 0, 0) == HEAP_CHECK_OK);
    }

    /* The counts must always add up to the object total. */
    {
        u16 saved = sh->in_use;

        sh->in_use = (u16)(saved + 1u);
        CHECK(heap_check(0, 0, 0) == HEAP_CHECK_SLAB_COUNTS);
        sh->in_use = saved;
        CHECK(heap_check(0, 0, 0) == HEAP_CHECK_OK);
    }

    /* The bitmap must agree with in_use, and no bit past objects may be set. */
    {
        u64 saved0 = sh->used[0];
        u64 saved3 = sh->used[3];

        sh->used[0] = 0;
        CHECK(heap_check(0, 0, 0) == HEAP_CHECK_SLAB_COUNTS);
        sh->used[0] = saved0;
        sh->used[3] = saved3 | (1ull << 40);
        CHECK(heap_check(0, 0, 0) == HEAP_CHECK_SLAB_BITMAP);
        sh->used[3] = saved3;
        CHECK(heap_check(0, 0, 0) == HEAP_CHECK_OK);
    }

    /* A free-list link outside the object area is rejected. */
    {
        void *saved;

        kfree(b);
        saved = *(void **)b;
        *(void **)b = (void *)0x40ull;
        CHECK(heap_check(0, 0, 0) == HEAP_CHECK_SLAB_FREE_LIST);
        *(void **)b = saved;
        CHECK(heap_check(0, 0, 0) == HEAP_CHECK_OK);
    }

    /* Freeing a large block in the middle of the list keeps it consistent. */
    kfree(big);
    big = 0;
    CHECK(heap_check(0, 0, 0) == HEAP_CHECK_OK);
    kfree(big2);
    kfree(a);
    CHECK(heap_used_bytes() == before);
    CHECK(heap_check(&slabs, &objects, &large) == HEAP_CHECK_OK);
    CHECK(objects == 0u && large == 0u);

    /* The randomised stress loop re-checks the heap every 64 operations. */
    CHECK(heap_stress(2000) == 1);
    CHECK(heap_used_bytes() == before);
    CHECK(heap_check(0, 0, 0) == HEAP_CHECK_OK);

    /* And the guard records a detection for each injected corruption. */
    CHECK(heap_guard() == 1);
    CHECK(heap_check(0, 0, 0) == HEAP_CHECK_OK);
    CHECK(heap_used_bytes() == before);

    CHECK_STR(heap_check_name(HEAP_CHECK_OK), "ok");
    CHECK_STR(heap_check_name(HEAP_CHECK_SLAB_FREE_LIST), "slab free list");
    CHECK_STR(heap_check_name(HEAP_CHECK_LARGE_COOKIE), "large cookie");
    CHECK_STR(heap_check_name(99u), "unknown");
}
