#ifndef CRUG_VM_H
#define CRUG_VM_H

#include <crug/types.h>

/* PTE/PDE/PDPTE/PML4E flag bits. */
#define VM_PRESENT  (1ull << 0)
#define VM_WRITE    (1ull << 1)
#define VM_USER     (1ull << 2)
#define VM_PWT      (1ull << 3)
#define VM_PCD      (1ull << 4)
#define VM_ACCESSED (1ull << 5)
#define VM_DIRTY    (1ull << 6)
#define VM_HUGE     (1ull << 7)
#define VM_GLOBAL   (1ull << 8)
#define VM_NX       (1ull << 63)
#define VM_ADDR_MASK 0x000ffffffffff000ull
#define VM_PTE_FLAG_MASK (VM_PRESENT | VM_WRITE | VM_USER | VM_PWT | VM_PCD | VM_GLOBAL | VM_NX)

static inline u64 vm_pml4_index(u64 va) { return (va >> 39) & 0x1ffu; }
static inline u64 vm_pdpt_index(u64 va) { return (va >> 30) & 0x1ffu; }
static inline u64 vm_pd_index(u64 va)   { return (va >> 21) & 0x1ffu; }
static inline u64 vm_pt_index(u64 va)   { return (va >> 12) & 0x1ffu; }

int vm_init(void);
int vm_map_page(u64 va, u64 pa, u64 flags);
int vm_map_range(u64 va, u64 pa, u64 size, u64 flags);
int vm_unmap_page(u64 va);
int vm_protect_page(u64 va, u64 flags);
u64 vm_translate(u64 va);
void vm_flush_page(u64 va);
void vm_flush_all(void);
int vm_audit_wx(void);
int vm_selftest(void);

#endif
