#include <crug/mm.h>
#include <crug/bitmap.h>
#include <crug/kernel.h>
#include <crug/string.h>

#define PMM_MAX_PAGES (CRUG_4GIB / CRUG_PAGE_SIZE)
#define PMM_BITMAP_MAX_BYTES (PMM_MAX_PAGES / 8u)

extern char __kernel_start[];
extern char __kernel_end[];

static u8 g_bitmap[PMM_BITMAP_MAX_BYTES] __attribute__((aligned(8)));
static u64 g_nbits;
static u64 g_total;
static u64 g_free;
static u64 g_bitmap_bytes;
static u64 g_max_phys;

static void bitmap_set_range(u64 first, u64 last)
{
    u64 i;

    if (last > g_nbits)
        last = g_nbits;
    for (i = first; i < last; i++)
        bitmap_set(g_bitmap, i);
}

static void bitmap_clear_range(u64 first, u64 last)
{
    u64 i;

    if (last > g_nbits)
        last = g_nbits;
    for (i = first; i < last; i++)
        bitmap_clear(g_bitmap, i);
}

static void reserve_range(u64 base, u64 size)
{
    u64 first, last;

    if (size == 0)
        return;
    first = ALIGN_DOWN(base, CRUG_PAGE_SIZE) / CRUG_PAGE_SIZE;
    last = ALIGN_UP(base + size, CRUG_PAGE_SIZE) / CRUG_PAGE_SIZE;
    bitmap_set_range(first, last);
}

static void count_free(void)
{
    u64 i;

    g_free = 0;
    for (i = 0; i < g_nbits; i++) {
        if (!bitmap_test(g_bitmap, i))
            g_free++;
    }
}

void pmm_init(const boot_info *bi)
{
    u64 i;

    g_max_phys = 0;
    for (i = 0; i < bi->memory_map_count; i++) {
        u64 end = bi->memory_map[i].base + bi->memory_map[i].size;
        if (end > CRUG_4GIB)
            end = CRUG_4GIB;
        if (end > g_max_phys)
            g_max_phys = end;
    }
    g_max_phys = ALIGN_DOWN(g_max_phys, CRUG_PAGE_SIZE);
    g_nbits = g_max_phys / CRUG_PAGE_SIZE;
    if (g_nbits == 0 || g_nbits > PMM_MAX_PAGES)
        panic("pmm: invalid physical memory size");

    g_bitmap_bytes = (g_nbits + 7u) / 8u;
    memset(g_bitmap, 0xff, g_bitmap_bytes);

    /* Only frames fully inside a usable region may be handed out. */
    g_total = 0;
    for (i = 0; i < bi->memory_map_count; i++) {
        const memory_region *r = &bi->memory_map[i];
        u64 start, end;

        if (r->kind != CRUG_MEM_USABLE)
            continue;
        start = ALIGN_UP(r->base, CRUG_PAGE_SIZE);
        end = ALIGN_DOWN(r->base + r->size, CRUG_PAGE_SIZE);
        if (end > g_max_phys)
            end = g_max_phys;
        if (start >= end)
            continue;
        bitmap_clear_range(start / CRUG_PAGE_SIZE, end / CRUG_PAGE_SIZE);
        g_total += (end - start) / CRUG_PAGE_SIZE;
    }

    /* Firmware-provided reservations. */
    for (i = 0; i < bi->memory_map_count; i++) {
        const memory_region *r = &bi->memory_map[i];
        if (r->kind != CRUG_MEM_USABLE)
            reserve_range(r->base, r->size);
    }

    /* The kernel image, its .bss (including this bitmap and the stacks). */
    reserve_range((u64)(uintptr)__kernel_start,
                  (u64)(uintptr)__kernel_end - (u64)(uintptr)__kernel_start);
    reserve_range(bi->kernel_base, bi->kernel_size);

    /* Never hand out the zero frame or the boot info itself. */
    bitmap_set(g_bitmap, 0);
    reserve_range((u64)(uintptr)bi, sizeof(*bi));

    count_free();

    kprintf("pmm    : %llu pages (%llu MiB), free %llu, bitmap %llu B\n",
            g_total, (g_total * CRUG_PAGE_SIZE) / (1024u * 1024u),
            g_free, g_bitmap_bytes);
}

static void pmm_zero(u64 pa, u64 count)
{
    memset(pmm_phys_to_virt(pa), 0, count * CRUG_PAGE_SIZE);
}

u64 pmm_alloc_page(void)
{
    u64 idx = bitmap_find_first_clear(g_bitmap, g_nbits);
    u64 pa;

    if (idx >= g_nbits)
        return 0;
    bitmap_set(g_bitmap, idx);
    g_free--;
    pa = idx * CRUG_PAGE_SIZE;
    pmm_zero(pa, 1);
    return pa;
}

u64 pmm_alloc_pages(u64 count)
{
    u64 i = 0;

    if (count == 0)
        return 0;
    while (i + count <= g_nbits) {
        u64 j;

        if (bitmap_test(g_bitmap, i)) {
            i++;
            continue;
        }
        for (j = 1; j < count; j++) {
            if (bitmap_test(g_bitmap, i + j))
                break;
        }
        if (j == count) {
            u64 pa = i * CRUG_PAGE_SIZE;

            bitmap_set_range(i, i + count);
            g_free -= count;
            pmm_zero(pa, count);
            return pa;
        }
        i += j + 1;
    }
    return 0;
}

u64 pmm_alloc_pages_aligned(u64 count, u64 align_pages)
{
    u64 i;

    if (count == 0 || align_pages == 0)
        return 0;
    if ((align_pages & (align_pages - 1)) != 0)
        return 0;

    for (i = 0; i + count <= g_nbits; i++) {
        u64 j;

        if ((i & (align_pages - 1)) != 0)
            continue;
        for (j = 0; j < count; j++) {
            if (bitmap_test(g_bitmap, i + j))
                break;
        }
        if (j == count) {
            u64 pa = i * CRUG_PAGE_SIZE;

            bitmap_set_range(i, i + count);
            g_free -= count;
            pmm_zero(pa, count);
            return pa;
        }
    }
    return 0;
}

void pmm_free_page(u64 pa)
{
    u64 idx = pa / CRUG_PAGE_SIZE;

    if ((pa & (CRUG_PAGE_SIZE - 1)) != 0 || idx >= g_nbits)
        panic("pmm: invalid free address");
    if (!bitmap_test(g_bitmap, idx))
        panic("pmm: double free");
    bitmap_clear(g_bitmap, idx);
    g_free++;
}

void pmm_free_pages(u64 pa, u64 count)
{
    u64 i;

    if (count == 0)
        return;
    if ((pa & (CRUG_PAGE_SIZE - 1)) != 0)
        panic("pmm: invalid free address");
    for (i = 0; i < count; i++) {
        u64 idx = pa / CRUG_PAGE_SIZE + i;

        if (idx >= g_nbits)
            panic("pmm: free beyond memory");
        if (!bitmap_test(g_bitmap, idx))
            panic("pmm: double free");
    }
    bitmap_clear_range(pa / CRUG_PAGE_SIZE, pa / CRUG_PAGE_SIZE + count);
    g_free += count;
}

u64 pmm_total_pages(void)
{
    return g_total;
}

u64 pmm_free_pages_count(void)
{
    return g_free;
}

u64 pmm_bitmap_bytes(void)
{
    return g_bitmap_bytes;
}

int pmm_selftest(void)
{
    u64 before = g_free;
    u64 a, b, c;
    u64 i;

    b = pmm_alloc_pages(4);
    if (b == 0)
        return 0;
    for (i = 0; i < 4; i++) {
        u64 *q = (u64 *)pmm_phys_to_virt(b + i * CRUG_PAGE_SIZE);

        if (q[0] != 0 || q[511] != 0)
            return 0;
        q[0] = i + 1;
    }
    pmm_free_pages(b, 4);

    a = pmm_alloc_page();
    if (a == 0)
        return 0;
    if (((u64 *)pmm_phys_to_virt(a))[0] != 0)
        return 0;
    ((u64 *)pmm_phys_to_virt(a))[0] = 0x1122334455667788ull;
    pmm_free_page(a);

    c = pmm_alloc_pages_aligned(2, 2);
    if (c == 0 || (c & (2 * CRUG_PAGE_SIZE - 1)) != 0)
        return 0;
    pmm_free_pages(c, 2);

    return pmm_free_pages_count() == before;
}
