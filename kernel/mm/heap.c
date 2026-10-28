/* CrugOS kernel heap: eight slab classes plus page-backed large blocks.
 *
 * Phase H2 hardens the allocator for the ring-0 threat model.  Every metadata
 * page carries a per-page cookie (a fixed key XOR the page address) instead of
 * a plain magic number, so a stale or forged header is detected before use.
 * Large blocks live on a doubly linked list, the allocation bitmap and the
 * free list are cross-checked on every allocate and free, and heap_check()
 * walks the complete heap read-only and returns the first structural defect.
 * The boot selftest (heap.guard=1, heap.stress=<ops>) and the make test-heap
 * gate use that walk to prove integrity and to detect an injected corruption.
 */
#include <crug/heap.h>
#include <crug/cpu.h>
#include <crug/kernel.h>
#include <crug/mm.h>
#include <crug/string.h>

#define HEAP_COOKIE_KEY 0x435255474f5348ull /* 'CRUGOSH' */
#define SLAB_COOKIE_SALT 0x534c4142ull      /* 'SLAB' */
#define LARGE_COOKIE_SALT 0x4c415247ull     /* 'LARG' */
#define SLAB_HEADER_SIZE 64u
#define LARGE_HEADER_SIZE 64u

typedef struct slab_header {
    u64 cookie;
    u16 class_idx;
    u16 objects;
    u16 free_count;
    u16 in_use;
    u64 used[4];
    void *free_list;
    struct slab_header *next;
} slab_header;

typedef struct large_header {
    u64 cookie;
    u64 size;
    u64 pages;
    struct large_header *next;
    struct large_header *prev;
    u64 reserved[3];
} large_header;

typedef struct heap_slot {
    void *ptr;
    usize size;
    u8 tag;
} heap_slot;

static const u32 g_class_sizes[HEAP_SLAB_CLASSES] = {
    16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u
};

static slab_header *g_slabs[HEAP_SLAB_CLASSES];
static large_header *g_large_list;
static u64 g_slab_pages;
static u64 g_large_pages;
static u64 g_large_bytes;
static u32 g_stress_ops;
static int g_guard;

static inline u64 slab_cookie(const void *page)
{
    return HEAP_COOKIE_KEY ^ (u64)(uintptr)page ^ SLAB_COOKIE_SALT;
}

static inline u64 large_cookie(const void *page)
{
    return HEAP_COOKIE_KEY ^ (u64)(uintptr)page ^ LARGE_COOKIE_SALT;
}

static inline u32 class_size(u32 cls)
{
    return cls < HEAP_SLAB_CLASSES ? g_class_sizes[cls] : 0u;
}

static void bit_set(u64 *bits, u32 idx)
{
    bits[idx / 64u] |= 1ull << (idx % 64u);
}

static void bit_clear(u64 *bits, u32 idx)
{
    bits[idx / 64u] &= ~(1ull << (idx % 64u));
}

static int bit_test(const u64 *bits, u32 idx)
{
    return (bits[idx / 64u] >> (idx % 64u)) & 1ull ? 1 : 0;
}

/* Map one object pointer of a slab page back to its bitmap index.  Returns -1
 * when the pointer is outside the object area or not class aligned. */
static int slab_index_of(const slab_header *h, const void *obj)
{
    const u8 *start = (const u8 *)h + SLAB_HEADER_SIZE;
    const u8 *p = (const u8 *)obj;
    u32 size;

    if (h->class_idx >= HEAP_SLAB_CLASSES)
        return -1;
    size = class_size(h->class_idx);
    if (size == 0 || (u64)(uintptr)p < (u64)(uintptr)start ||
        (u64)(uintptr)p >= (u64)(uintptr)h + CRUG_PAGE_SIZE)
        return -1;
    if (((u64)(uintptr)p - (u64)(uintptr)start) % size != 0)
        return -1;
    return (int)(((u64)(uintptr)p - (u64)(uintptr)start) / size);
}

static slab_header *slab_create(u32 cls)
{
    slab_header *h;
    u8 *start;
    u32 size_class = class_size(cls);
    u32 objects;
    u32 i;

    h = (slab_header *)pmm_phys_to_virt(pmm_alloc_page());
    if (!h)
        return 0;
    memset(h, 0, CRUG_PAGE_SIZE);
    objects = (CRUG_PAGE_SIZE - SLAB_HEADER_SIZE) / size_class;
    h->cookie = slab_cookie(h);
    h->class_idx = (u16)cls;
    h->objects = (u16)objects;
    h->free_count = (u16)objects;
    start = (u8 *)h + SLAB_HEADER_SIZE;
    for (i = 0; i < objects; i++) {
        u8 *obj = start + (usize)i * size_class;

        *(void **)obj = (i + 1 < objects)
            ? (void *)(start + (usize)(i + 1) * size_class)
            : 0;
    }
    h->free_list = objects ? (void *)start : 0;
    h->next = g_slabs[cls];
    g_slabs[cls] = h;
    g_slab_pages++;
    return h;
}

static void *kmalloc_large(usize size)
{
    u64 pages = (size + LARGE_HEADER_SIZE + CRUG_PAGE_SIZE - 1) / CRUG_PAGE_SIZE;
    u64 flags = cpu_irq_save();
    large_header *h;
    u64 pa = pmm_alloc_pages(pages);

    if (pa == 0) {
        cpu_irq_restore(flags);
        return 0;
    }
    h = (large_header *)pmm_phys_to_virt(pa);
    memset(h, 0, LARGE_HEADER_SIZE);
    h->cookie = large_cookie(h);
    h->size = size;
    h->pages = pages;
    h->prev = 0;
    h->next = g_large_list;
    if (g_large_list)
        g_large_list->prev = h;
    g_large_list = h;
    g_large_pages += pages;
    g_large_bytes += size;
    cpu_irq_restore(flags);
    return (u8 *)h + LARGE_HEADER_SIZE;
}

void heap_init(void)
{
    u32 cls;

    for (cls = 0; cls < HEAP_SLAB_CLASSES; cls++)
        g_slabs[cls] = 0;
    g_large_list = 0;
    g_slab_pages = 0;
    g_large_pages = 0;
    g_large_bytes = 0;
    g_stress_ops = 0;
    g_guard = 0;
}

void *kmalloc(usize size)
{
    u64 flags;
    slab_header *h;
    void *obj;
    u32 cls;
    int idx;

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
    if (h->cookie != slab_cookie(h) || h->class_idx != (u16)cls ||
        (u32)h->free_count + (u32)h->in_use != (u32)h->objects) {
        cpu_irq_restore(flags);
        panic("heap: corrupt slab header");
    }
    obj = h->free_list;
    if (!obj) {
        cpu_irq_restore(flags);
        panic("heap: slab free list underflow");
    }
    idx = slab_index_of(h, obj);
    if (idx < 0 || bit_test(h->used, (u32)idx)) {
        cpu_irq_restore(flags);
        panic("heap: corrupt slab object");
    }
    h->free_list = *(void **)obj;
    if (h->free_list) {
        int next = slab_index_of(h, h->free_list);

        if (next < 0 || bit_test(h->used, (u32)next)) {
            h->free_list = 0;
            cpu_irq_restore(flags);
            panic("heap: corrupt slab free list");
        }
    }
    bit_set(h->used, (u32)idx);
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

/* Does the large-block list own this pointer?  Sets *interior when the pointer
 * is inside a block but not at its first payload byte. */
static int large_contains(const void *ptr, int *interior)
{
    const large_header *lh;
    u64 start = (u64)(uintptr)ptr;

    for (lh = g_large_list; lh; lh = lh->next) {
        u64 base;
        u64 bytes;

        if (lh->cookie != large_cookie(lh))
            continue;
        if (lh->size == 0 || lh->size > HEAP_MAX_ALLOC || lh->pages == 0)
            continue;
        base = (u64)(uintptr)lh;
        bytes = lh->pages * CRUG_PAGE_SIZE;
        if (start >= base && start < base + bytes) {
            *interior = (start != base + LARGE_HEADER_SIZE);
            return 1;
        }
    }
    return 0;
}

void kfree(void *ptr)
{
    u64 flags;
    void *page;
    slab_header *h;
    u32 size_class;
    int idx;
    int interior = 0;

    if (!ptr)
        return;
    flags = cpu_irq_save();
    page = pmm_phys_to_virt((u64)(uintptr)ptr & CRUG_PAGE_MASK);
    h = (slab_header *)page;
    if (h->cookie == slab_cookie(page)) {
        u8 *start = (u8 *)h + SLAB_HEADER_SIZE;
        u64 p = (u64)(uintptr)ptr;

        if (p < (u64)(uintptr)start ||
            p >= (u64)(uintptr)h + CRUG_PAGE_SIZE) {
            cpu_irq_restore(flags);
            panic("heap: free of non-object pointer");
        }
        if (h->class_idx >= HEAP_SLAB_CLASSES) {
            cpu_irq_restore(flags);
            panic("heap: corrupt slab header");
        }
        size_class = class_size(h->class_idx);
        if ((p - (u64)(uintptr)start) % size_class != 0) {
            cpu_irq_restore(flags);
            panic("heap: free of misaligned object");
        }
        idx = (int)((p - (u64)(uintptr)start) / size_class);
        if (idx >= (int)h->objects || !bit_test(h->used, (u32)idx)) {
            cpu_irq_restore(flags);
            panic("heap: double free");
        }
        bit_clear(h->used, (u32)idx);
        *(void **)ptr = h->free_list;
        h->free_list = ptr;
        h->free_count++;
        h->in_use--;
        cpu_irq_restore(flags);
        return;
    }
    if (large_contains(ptr, &interior)) {
        large_header *lh;

        if (interior) {
            cpu_irq_restore(flags);
            panic("heap: free of interior large pointer");
        }
        lh = (large_header *)page;
        if (lh->next)
            lh->next->prev = lh->prev;
        if (lh->prev)
            lh->prev->next = lh->next;
        else
            g_large_list = lh->next;
        g_large_pages -= lh->pages;
        g_large_bytes -= lh->size;
        pmm_free_pages((u64)(uintptr)page, lh->pages);
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
    u64 flags = cpu_irq_save();
    u64 used = g_large_bytes;
    u32 cls;

    for (cls = 0; cls < HEAP_SLAB_CLASSES; cls++) {
        slab_header *h;

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

static u32 slab_validate(const slab_header *h, u32 cls, u32 *objects)
{
    const void *cur;
    u32 size;
    u32 bits = 0;
    u32 listed = 0;
    u32 i;

    if (h->cookie != slab_cookie(h))
        return HEAP_CHECK_SLAB_COOKIE;
    if (h->class_idx != (u16)cls)
        return HEAP_CHECK_SLAB_CLASS;
    size = class_size(cls);
    if (size == 0 || h->objects == 0 ||
        (u32)h->objects > (CRUG_PAGE_SIZE - SLAB_HEADER_SIZE) / size)
        return HEAP_CHECK_SLAB_COUNTS;
    if ((u32)h->free_count + (u32)h->in_use != (u32)h->objects)
        return HEAP_CHECK_SLAB_COUNTS;
    for (i = 0; i < (u32)h->objects; i++)
        bits += (u32)bit_test(h->used, i);
    for (i = (u32)h->objects; i < 256u; i++) {
        if (bit_test(h->used, i))
            return HEAP_CHECK_SLAB_BITMAP;
    }
    if (bits != (u32)h->in_use)
        return HEAP_CHECK_SLAB_COUNTS;
    cur = h->free_list;
    while (cur) {
        int idx = slab_index_of(h, cur);

        if (idx < 0 || bit_test(h->used, (u32)idx))
            return HEAP_CHECK_SLAB_FREE_LIST;
        listed++;
        if (listed > (u32)h->objects)
            return HEAP_CHECK_SLAB_FREE_LIST;
        cur = *(const void *const *)cur;
    }
    if (listed != (u32)h->free_count)
        return HEAP_CHECK_SLAB_FREE_LIST;
    *objects += (u32)h->in_use;
    return HEAP_CHECK_OK;
}

static u32 large_validate(const large_header *lh, u64 *pages, u64 *bytes,
                          const large_header **next)
{
    u64 want;

    *next = 0;
    if (lh->cookie != large_cookie(lh))
        return HEAP_CHECK_LARGE_COOKIE;
    if (lh->size == 0 || lh->size > HEAP_MAX_ALLOC || lh->pages == 0)
        return HEAP_CHECK_LARGE_FIELDS;
    want = (lh->size + LARGE_HEADER_SIZE + CRUG_PAGE_SIZE - 1) / CRUG_PAGE_SIZE;
    if (lh->pages != want)
        return HEAP_CHECK_LARGE_FIELDS;
    *pages += lh->pages;
    *bytes += lh->size;
    *next = lh->next;
    return HEAP_CHECK_OK;
}

u32 heap_check(u32 *out_slabs, u32 *out_objects, u32 *out_large)
{
    u64 flags = cpu_irq_save();
    const large_header *prev = 0;
    const large_header *lh;
    u64 pages = 0;
    u64 bytes = 0;
    u32 slabs = 0;
    u32 objects = 0;
    u32 large = 0;
    u32 code = HEAP_CHECK_OK;
    u32 cls;

    for (cls = 0; cls < HEAP_SLAB_CLASSES && code == HEAP_CHECK_OK; cls++) {
        const slab_header *h = g_slabs[cls];

        while (h) {
            slabs++;
            code = slab_validate(h, cls, &objects);
            if (code != HEAP_CHECK_OK)
                break;
            h = h->next;
        }
    }
    for (lh = g_large_list; code == HEAP_CHECK_OK && lh;) {
        const large_header *next = 0;

        large++;
        if (large > 65536u) {
            code = HEAP_CHECK_LARGE_FIELDS;
            break;
        }
        if (lh->prev != prev) {
            code = HEAP_CHECK_LARGE_FIELDS;
            break;
        }
        code = large_validate(lh, &pages, &bytes, &next);
        if (code != HEAP_CHECK_OK)
            break;
        prev = lh;
        lh = next;
    }
    if (code == HEAP_CHECK_OK &&
        (g_slab_pages != (u64)slabs || g_large_pages != pages ||
         g_large_bytes != bytes))
        code = HEAP_CHECK_PAGE_TOTALS;
    cpu_irq_restore(flags);
    if (out_slabs)
        *out_slabs = slabs;
    if (out_objects)
        *out_objects = objects;
    if (out_large)
        *out_large = large;
    return code;
}

const char *heap_check_name(u32 code)
{
    switch (code) {
    case HEAP_CHECK_OK:
        return "ok";
    case HEAP_CHECK_SLAB_COOKIE:
        return "slab cookie";
    case HEAP_CHECK_SLAB_CLASS:
        return "slab class";
    case HEAP_CHECK_SLAB_COUNTS:
        return "slab counts";
    case HEAP_CHECK_SLAB_BITMAP:
        return "slab bitmap";
    case HEAP_CHECK_SLAB_FREE_LIST:
        return "slab free list";
    case HEAP_CHECK_LARGE_COOKIE:
        return "large cookie";
    case HEAP_CHECK_LARGE_FIELDS:
        return "large fields";
    case HEAP_CHECK_PAGE_TOTALS:
        return "page totals";
    default:
        return "unknown";
    }
}

static int heap_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int heap_token_value(const char *tok, u32 len, const char *key,
                            const char **val, u32 *vlen)
{
    u32 klen = (u32)strlen(key);

    if (len <= klen || tok[klen] != '=')
        return 0;
    if (strncmp(tok, key, (usize)klen) != 0)
        return 0;
    *val = tok + klen + 1u;
    *vlen = len - klen - 1u;
    return 1;
}

static int heap_parse_u32(const char *s, u32 len, u32 *out)
{
    u32 value = 0;
    u32 i;

    if (len == 0)
        return 0;
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        value = value * 10u + (u32)(s[i] - '0');
        if (value > 100000000u)
            return 0;
    }
    *out = value;
    return 1;
}

void heap_cmdline(const boot_info *bi)
{
    const char *cmd;
    u32 i = 0;

    g_stress_ops = 0;
    g_guard = 0;
    if (!bi || !bi->command_line)
        return;
    cmd = bi->command_line;
    while (cmd[i]) {
        const char *val;
        u32 vlen;
        u32 num;
        u32 start;

        while (cmd[i] && heap_space(cmd[i]))
            i++;
        start = i;
        while (cmd[i] && !heap_space(cmd[i]))
            i++;
        if (i == start)
            break;
        if (heap_token_value(cmd + start, i - start, "heap.stress", &val, &vlen)) {
            if (heap_parse_u32(val, vlen, &num) && num >= 1u &&
                num <= HEAP_STRESS_MAX)
                g_stress_ops = num;
        } else if (heap_token_value(cmd + start, i - start, "heap.guard",
                                    &val, &vlen)) {
            if (vlen == 1 && val[0] == '1')
                g_guard = 1;
        }
    }
    if (g_stress_ops)
        kprintf("CRUGOS: heap stress mode (%u op(s))\n", g_stress_ops);
    if (g_guard)
        kprintf("CRUGOS: heap guard mode\n");
}

static u64 heap_rand(u64 *state)
{
    u64 x = *state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

int heap_stress(u32 ops)
{
    heap_slot live[HEAP_STRESS_LIVE];
    u32 live_count = 0;
    u32 allocs = 0;
    u32 frees = 0;
    u32 i;
    u64 state = 0x243f6a8885a308d3ull;
    u64 before = heap_used_bytes();

    if (ops == 0 || ops > HEAP_STRESS_MAX)
        return 0;
    for (i = 0; i < ops; i++) {
        u32 pick = (u32)(heap_rand(&state) % 100ull);

        if (pick < 55u || live_count == 0) {
            usize size = (usize)(heap_rand(&state) % 8000ull) + 1u;
            u8 tag = (u8)(heap_rand(&state) % 250ull) + 1u;
            void *p;

            if (live_count == HEAP_STRESS_LIVE)
                continue;
            p = kmalloc(size);
            if (!p) {
                kprintf("CRUGOS: heap stress failed (allocation %u)\n", i);
                return 0;
            }
            memset(p, tag, size);
            live[live_count].ptr = p;
            live[live_count].size = size;
            live[live_count].tag = tag;
            live_count++;
            allocs++;
        } else {
            u32 k = (u32)(heap_rand(&state) % (u64)live_count);
            u8 *p = (u8 *)live[k].ptr;
            usize size = live[k].size;
            u8 tag = live[k].tag;
            usize j;

            for (j = 0; j < size; j++) {
                if (p[j] != tag) {
                    kprintf("CRUGOS: heap stress failed (object %u overwritten)\n", k);
                    return 0;
                }
            }
            kfree(p);
            live[k] = live[live_count - 1u];
            live_count--;
            frees++;
        }
        if ((i & 63u) == 0u && heap_check(0, 0, 0) != HEAP_CHECK_OK) {
            kprintf("CRUGOS: heap stress failed (check at op %u)\n", i);
            return 0;
        }
    }
    while (live_count) {
        kfree(live[live_count - 1u].ptr);
        live_count--;
        frees++;
    }
    if (heap_check(0, 0, 0) != HEAP_CHECK_OK) {
        kprintf("CRUGOS: heap stress failed (final check)\n");
        return 0;
    }
    if (heap_used_bytes() != before) {
        kprintf("CRUGOS: heap stress failed (accounting)\n");
        return 0;
    }
    kprintf("CRUGOS: heap stress ok (%u op(s), %u alloc(s), %u free(s))\n",
            ops, allocs, frees);
    return 1;
}

int heap_guard(void)
{
    slab_header *sh;
    large_header *lh;
    u64 saved_cookie;
    void *saved_next;
    u32 detected = 0;
    u32 code;
    u8 *a = (u8 *)kmalloc(16);
    u8 *b = (u8 *)kmalloc(16);
    u8 *big = (u8 *)kmalloc(5000);

    if (!a || !b || !big)
        return 0;

    /* 1. A corrupted free-list link must be caught by the free-list walk. */
    kfree(b);
    saved_next = *(void **)b;
    *(void **)b = (void *)0x40ull;
    code = heap_check(0, 0, 0);
    *(void **)b = saved_next;
    if (code == HEAP_CHECK_SLAB_FREE_LIST) {
        kprintf("CRUGOS: heap guard detected corrupt free list (code %u)\n", code);
        detected++;
    } else {
        kprintf("CRUGOS: heap guard missed corrupt free list (code %u)\n", code);
    }

    /* 2. A corrupted slab cookie must be caught by the page check. */
    sh = (slab_header *)pmm_phys_to_virt((u64)(uintptr)a & CRUG_PAGE_MASK);
    saved_cookie = sh->cookie;
    sh->cookie = 0;
    code = heap_check(0, 0, 0);
    sh->cookie = saved_cookie;
    if (code == HEAP_CHECK_SLAB_COOKIE) {
        kprintf("CRUGOS: heap guard detected corrupt slab cookie (code %u)\n", code);
        detected++;
    } else {
        kprintf("CRUGOS: heap guard missed corrupt slab cookie (code %u)\n", code);
    }

    /* 3. A corrupted large-block cookie must be caught by the list walk. */
    lh = (large_header *)(big - LARGE_HEADER_SIZE);
    saved_cookie = lh->cookie;
    lh->cookie = 0;
    code = heap_check(0, 0, 0);
    lh->cookie = saved_cookie;
    if (code == HEAP_CHECK_LARGE_COOKIE) {
        kprintf("CRUGOS: heap guard detected corrupt large cookie (code %u)\n", code);
        detected++;
    } else {
        kprintf("CRUGOS: heap guard missed corrupt large cookie (code %u)\n", code);
    }

    kfree(a);
    kfree(big);
    if (heap_check(0, 0, 0) != HEAP_CHECK_OK)
        return 0;
    if (detected != 3u) {
        kprintf("CRUGOS: heap guard failed (%u of 3 corruption(s) detected)\n",
                detected);
        return 0;
    }
    kprintf("CRUGOS: heap guard ok (3 corruption(s) detected)\n");
    return 1;
}

int heap_selftest(void)
{
    u64 before = heap_used_bytes();
    u8 *a = (u8 *)kmalloc(16);
    u8 *b = (u8 *)kmalloc(64);
    u8 *c;
    u8 *d;
    u32 i;
    u32 slabs = 0;
    u32 objects = 0;
    u32 large = 0;
    u32 code;

    if (!a || !b)
        return 0;
    c = (u8 *)kzalloc(4096);
    d = (u8 *)kmalloc(70000);
    if (!c || !d)
        return 0;
    if (((u64)(uintptr)a & 0xFu) != 0 || ((u64)(uintptr)b & 0xFu) != 0)
        return 0;
    for (i = 0; i < 4096u; i++) {
        if (c[i] != 0)
            return 0;
    }
    memset(a, 0xA5, 16);
    memset(b, 0x5A, 64);
    memset(c, 0xFF, 4096);
    d[0] = 1;
    d[69999] = 2;
    if (a[0] != 0xA5 || a[15] != 0xA5 || b[0] != 0x5A || b[63] != 0x5A ||
        c[0] != 0xFF || c[4095] != 0xFF || d[0] != 1 || d[69999] != 2)
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
    if (c != a)
        return 0;
    kfree(b);
    kfree(c);

    code = heap_check(&slabs, &objects, &large);
    if (code != HEAP_CHECK_OK) {
        kprintf("CRUGOS: heap check failed (%s, code %u)\n",
                heap_check_name(code), code);
        return 0;
    }
    kprintf("CRUGOS: heap check ok (%u slab(s), %u object(s), %u large block(s))\n",
            slabs, objects, large);
    if (g_stress_ops && !heap_stress(g_stress_ops))
        return 0;
    if (g_guard && !heap_guard())
        return 0;
    return 1;
}
