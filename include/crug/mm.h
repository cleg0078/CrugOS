#ifndef CRUG_MM_H
#define CRUG_MM_H

#include <crug/types.h>
#include <crug/bootinfo.h>

#define CRUG_PAGE_SHIFT 12u
#define CRUG_PAGE_SIZE (1ull << CRUG_PAGE_SHIFT)
#define CRUG_PAGE_MASK (~(CRUG_PAGE_SIZE - 1ull))
#define CRUG_4GIB 0x100000000ull

/* Physical memory manager: one bit per 4 KiB frame, no struct page. */
void pmm_init(const boot_info *bi);
u64 pmm_alloc_page(void);
u64 pmm_alloc_pages(u64 count);
u64 pmm_alloc_pages_aligned(u64 count, u64 align_pages);
void pmm_free_page(u64 pa);
void pmm_free_pages(u64 pa, u64 count);
u64 pmm_total_pages(void);
u64 pmm_free_pages_count(void);
u64 pmm_bitmap_bytes(void);
int pmm_selftest(void);

static inline void *pmm_phys_to_virt(u64 pa) { return (void *)(uintptr)pa; }
static inline u64 pmm_virt_to_phys(const void *va) { return (u64)(uintptr)va; }

#endif
