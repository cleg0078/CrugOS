/* CrugOS kernel heap: eight slab classes plus page-backed large blocks. */
#ifndef CRUG_HEAP_H
#define CRUG_HEAP_H

#include <crug/bootinfo.h>
#include <crug/types.h>

#define HEAP_SLAB_CLASSES 8u
#define HEAP_MAX_ALLOC (1024ull * 1024ull)
#define HEAP_STRESS_MAX 200000u
#define HEAP_STRESS_LIVE 64u

/* heap_check() result codes; 0 means every structure validated. */
#define HEAP_CHECK_OK 0u
#define HEAP_CHECK_SLAB_COOKIE 1u
#define HEAP_CHECK_SLAB_CLASS 2u
#define HEAP_CHECK_SLAB_COUNTS 3u
#define HEAP_CHECK_SLAB_BITMAP 4u
#define HEAP_CHECK_SLAB_FREE_LIST 5u
#define HEAP_CHECK_LARGE_COOKIE 6u
#define HEAP_CHECK_LARGE_FIELDS 7u
#define HEAP_CHECK_PAGE_TOTALS 8u

void heap_init(void);
/* Boot tokens heap.stress=<1..200000> and heap.guard=1 (phase H2). */
void heap_cmdline(const boot_info *bi);
void *kmalloc(usize size);
void *kzalloc(usize size);
void kfree(void *ptr);
u64 heap_total_bytes(void);
u64 heap_used_bytes(void);
u64 heap_slab_pages(void);
u64 heap_large_pages(void);
int heap_selftest(void);

/* Read-only structural walk over the whole heap: every slab cookie, class,
 * count, bitmap and free-list link must validate, and every large block's
 * chain, size and page count must agree with the accounting.  The optional
 * out parameters receive the slab, live-object and large-block counts. */
u32 heap_check(u32 *out_slabs, u32 *out_objects, u32 *out_large);
const char *heap_check_name(u32 code);
/* Chaotic allocate/write/free loop (heap.stress=<ops>); 1 on success. */
int heap_stress(u32 ops);
/* Injects a corrupt free-list link, slab cookie and large cookie, requires
 * heap_check() to catch each one, repairs them (heap.guard=1). */
int heap_guard(void);

#endif /* CRUG_HEAP_H */
