/* CrugOS kernel heap: page-backed slabs plus large allocations. */
#ifndef CRUG_HEAP_H
#define CRUG_HEAP_H

#include <crug/types.h>

#define HEAP_SLAB_CLASSES 8u
#define HEAP_MAX_ALLOC (1024ull * 1024ull)

void heap_init(void);
void *kmalloc(usize size);
void *kzalloc(usize size);
void kfree(void *ptr);
u64 heap_total_bytes(void);
u64 heap_used_bytes(void);
u64 heap_slab_pages(void);
u64 heap_large_pages(void);
int heap_selftest(void);

#endif /* CRUG_HEAP_H */
