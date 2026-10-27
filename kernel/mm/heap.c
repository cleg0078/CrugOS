/* CrugOS kernel heap: 8 slab classes plus page-backed large allocations. */
#include <crug/heap.h>
#include <crug/mm.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include <crug/cpu.h>

#define SLAB_MAGIC  0x534c4142u /* 'SLAB' */
#define LARGE_MAGIC 0x4c415247u /* 'LARG' */
#define SLAB_HEADER_SIZE 64u
#define LARGE_HEADER_SIZE 64u

typedef struct slab_header {
    u32 magic;
    u16 class_idx;
    u16 free_count;
    u16 objects;
    u16 in_use;
    u32 reserved;
    u64 used[4];
    void *free_list;
    struct slab_header *next;
} slab_header;

typedef struct large_header {
    u32 magic;
    u32 pages;
    u64 size;
    u64 reserved[5];
} large_header;

static const u32 g_class_sizes[HEAP_SLAB_CLASSES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

static slab_header *g_slabs[HEAP_SLAB_CLASSES];
static u64 g_slab_pages;
static u64 g_large_pages;
static u64 g_large_bytes;

static inline u32 class_size(u32 idx)
{
    return g_class_sizes[idx];
}

static inline void bit_set(u64 *bits, u32 idx)
{
    bits[idx / 64u] |= 1ull << (idx % 64u);
}

static inline void bit_clear(u64 *bits, u32 idx)
{
    bits[idx / 64u] &= ~(1ull << (idx % 64u));
}

static inline int bit_test(const u64 *bits, u32 idx)
{
    return (int)((bits[idx / 64u] >> (idx % 64u)) & 1ull);
}

static slab_header *slab_create(u32 cls)
{
    u64 pa = pmm_alloc_page();
    slab_header *h;
    u8 *base;
    u32 i;
    u32 size;

    if (pa == 0)
        return 0;
    h = (slab_header *)pmm_phys_to_virt(pa);
    memset(h, 0, CRUG_PAGE_SIZE);

    size = class_size(cls);
    h->magic = SLAB_MAGIC;
    h->class_idx = (u16)cls;
    h->objects = (u16)((CRUG_PAGE_SIZE - SLAB_HEADER_SIZE) / size);
    h->free_count = h->objects;
    h->in_use = 0;
    h->free_list = 0;
    h->next = g_slabs[cls];
    g_slabs[cls] = h;
    g_slab_pages++;

    base = (u8 *)h + SLAB_HEADER_SIZE;
    for (i = 0; i < h->objects; i++) {
        void **p = (void **)(base + (u64)i * size);

        *p = h->free_list;
        h->free_list = p;
    }
    return h;
}

static void *kmalloc_large(usize size)
{
    u64 pages = (size + LARGE_HEADER_SIZE + CRUG_PAGE_SIZE - 1) / CRUG_PAGE_SIZE;
    u64 flags = cpu_irq_save();
    u64 pa = pmm_alloc_pages(pages);
    large_header *h;

    if (pa == 0) {
        cpu_irq_restore(flags);
        return 0;
    }
    h = (large_header *)pmm_phys_to_virt(pa);
    h->magic = LARGE_MAGIC;
    h->pages = (u32)pages;
    h->size = size;
    g_large_pages += pages;
    g_large_bytes += size;
    cpu_irq_restore(flags);
    return (u8 *)h + LARGE_HEADER_SIZE;
}

void heap_init(void)
{
    u32 i;

    for (i = 0; i < HEAP_SLAB_CLASSES; i++)
        g_slabs[i] = 0;
    g_slab_pages = 0;
    g_large_pages = 0;
    g_large_bytes = 0;
}

void *kmalloc(usize size)
{
    u64 flags;
    u32 cls;
    slab_header *h;
    void *obj;
    u32 size_class;
    u32 idx;

    if (size == 0 || size > HEAP_MAX_ALLOC)
        return 0;
    if (size > 2048)
        return kmalloc_large(size);

    for (cls = 0; cls < HEAP_SLAB_CLASSES; cls++) {
        if (class_size(cls) >= size)
            break;
    }
    if (cls == HEAP_SLAB_CLASSES)
        return 0;

    flags = cpu_irq_save();
    for (h = g_slabs[cls]; h; h = h->next) {
        if (h->free_count != 0)
            break;
    }
    if (!h) {
        h = slab_create(cls);
        if (!h) {
            cpu_irq_restore(flags);
            return 0;
        }
    }

    obj = h->free_list;
    if (!obj) {
        cpu_irq_restore(flags);
        panic("heap: slab free list underflow");
    }
    h->free_list = *(void **)obj;
    size_class = class_size(cls);
    idx = (u32)(((u8 *)obj - ((u8 *)h + SLAB_HEADER_SIZE)) / size_class);
    if (idx >= h->objects || bit_test(h->used, idx)) {
        cpu_irq_restore(flags);
        panic("heap: corrupt slab object");
    }
    bit_set(h->used, idx);
    h->free_count--;
    h->in_use++;
    cpu_irq_restore(flags);
    return obj;
}

void *kzalloc(usize size)
{
    void *p = kmalloc(size);

    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    u64 flags;
    u64 page;
    slab_header *h;
    u32 size_class;
    u32 idx;

    if (!ptr)
        return;
    flags = cpu_irq_save();
    page = (u64)(uintptr)ptr & CRUG_PAGE_MASK;
    h = (slab_header *)pmm_phys_to_virt(page);
    if (h->magic == SLAB_MAGIC) {
        u8 *start = (u8 *)h + SLAB_HEADER_SIZE;
        u8 *p = (u8 *)ptr;

        if (p < start || p >= (u8 *)h + CRUG_PAGE_SIZE) {
            cpu_irq_restore(flags);
            panic("heap: free of non-object pointer");
        }
        size_class = class_size(h->class_idx);
        if (((u64)(p - start) % size_class) != 0) {
            cpu_irq_restore(flags);
            panic("heap: free of misaligned object");
        }
        idx = (u32)((p - start) / size_class);
        if (idx >= h->objects || !bit_test(h->used, idx)) {
            cpu_irq_restore(flags);
            panic("heap: double free");
        }
        bit_clear(h->used, idx);
        *(void **)ptr = h->free_list;
        h->free_list = ptr;
        h->free_count++;
        h->in_use--;
        cpu_irq_restore(flags);
        return;
    }

    if (h->magic == LARGE_MAGIC) {
        large_header *lh = (large_header *)h;

        if (ptr != (u8 *)h + LARGE_HEADER_SIZE) {
            cpu_irq_restore(flags);
            panic("heap: free of interior large pointer");
        }
        g_large_pages -= lh->pages;
        g_large_bytes -= lh->size;
        pmm_free_pages(page, lh->pages);
        cpu_irq_restore(flags);
        return;
    }

    cpu_irq_restore(flags);
    panic("heap: free of unknown pointer");
}

u64 heap_total_bytes(void)
{
    return (g_slab_pages + g_large_pages) * CRUG_PAGE_SIZE;
}

u64 heap_used_bytes(void)
{
    u64 used = g_large_bytes;
    u32 cls;
    slab_header *h;
    u64 flags = cpu_irq_save();

    for (cls = 0; cls < HEAP_SLAB_CLASSES; cls++) {
        for (h = g_slabs[cls]; h; h = h->next)
            used += (u64)h->in_use * class_size(cls);
    }
    cpu_irq_restore(flags);
    return used;
}

u64 heap_slab_pages(void)
{
    return g_slab_pages;
}

u64 heap_large_pages(void)
{
    return g_large_pages;
}

int heap_selftest(void)
{
    u8 *a;
    u8 *b;
    u8 *c;
    u8 *d;
    usize i;
    u64 before;

    before = heap_used_bytes();
    a = (u8 *)kmalloc(16);
    b = (u8 *)kmalloc(64);
    c = (u8 *)kzalloc(4096);
    d = (u8 *)kmalloc(70000);
    if (!a || !b || !c || !d)
        return 0;
    if (((u64)(uintptr)a & 15ull) != 0 || ((u64)(uintptr)b & 15ull) != 0)
        return 0;
    for (i = 0; i < 4096; i++) {
        if (c[i] != 0)
            return 0;
    }
    memset(a, 0xa5, 16);
    memset(b, 0x5a, 64);
    memset(c, 0xff, 4096);
    d[0] = 1;
    d[69999] = 2;
    if (a[0] != 0xa5 || b[63] != 0x5a || c[4095] != 0xff ||
        d[0] != 1 || d[69999] != 2)
        return 0;
    kfree(a);
    kfree(b);
    kfree(c);
    kfree(d);
    if (heap_used_bytes() != before)
        return 0;

    a = (u8 *)kmalloc(16);
    b = (u8 *)kmalloc(16);
    if (!a || !b || a == b)
        return 0;
    kfree(a);
    c = (u8 *)kmalloc(16);
    if (!c)
        return 0;
    kfree(b);
    kfree(c);
    return 1;
}
