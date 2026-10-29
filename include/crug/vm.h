#ifndef CRUG_VM_H
#define CRUG_VM_H

#include <crug/types.h>
#include <crug/mm.h>

/* PTE/PDE/PDPTE/PML4E flag bits. */
#define VM_PRESENT  (1ull << 0)
#define VM_WRITE    (1ull << 1)
#define VM_USER     (1ull << 2)
#define VM_PWT      (1ull << 3)
#define VM_PCD      (1ull << 4)
#define VM_ACCESSED (1ull << 5)
#define VM_DIRTY    (1ull << 6)
#define VM_HUGE     (1ull << 7)
/* Software bit (the CPU ignores it): the leaf is shared copy-on-write with a
   forked address space and is mapped read-only in every space that holds it
   (phase U6). */
#define VM_COW      (1ull << 9)
#define VM_GLOBAL   (1ull << 8)
#define VM_NX       (1ull << 63)
#define VM_ADDR_MASK 0x000ffffffffff000ull
#define VM_PTE_FLAG_MASK (VM_PRESENT | VM_WRITE | VM_USER | VM_PWT | VM_PCD | VM_GLOBAL | VM_COW | VM_NX)

/* User window (phase U1): PML4 slot 1, i.e. 512 GiB..1 TiB of canonical low
   addresses. The kernel identity map occupies PML4 slot 0 and stays
   supervisor-only, so U/S=1 leaves can only ever exist above USER_BASE. */
#define USER_BASE 0x0000008000000000ull
#define USER_TOP  0x0000010000000000ull
#define USER_PAGE CRUG_PAGE_SIZE

/* vm_space_clone() refuses more than this many user pages (phase U5, raised
   by U6): the pages are shared, not copied, so the bound now only keeps one
   fork from filling the frame reference table on its own. */
#define VM_SPACE_FORK_MAX_PAGES 4096u

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

/* Per-process address space (phase U2). Every ring-3 thread owns one of these:
   slot 0 of the root is the shared supervisor identity map and slot 1 carries
   the whole user window, so two programs map the same virtual addresses onto
   different physical pages. The walk lives in kernel/mm/vm_space.c and only
   needs the page allocator; it never touches CR3, so it is host-testable.
   vm_space_activate() (kernel/mm/vm.c) is the one place that writes CR3. */
typedef struct vm_space {
    u64 *pml4;
    u64 phys;
    u32 user_pages;
    u32 pad;
} vm_space;

/* The kernel root new spaces copy slot 0 from (set once by vm_init). */
void vm_space_set_kernel_root(u64 *root);
int vm_space_create(vm_space *s);
void vm_space_destroy(vm_space *s);
int vm_space_map_user_pages(vm_space *s, u64 va, u64 pages, u64 flags);

/* Fork: share every user leaf of SRC with DST instead of copying the frames
   (phase U6). Writable leaves become read-only plus VM_COW in both spaces and
   the first store on either side takes a private copy through
   vm_space_cow_fault(). SRC is not const because its leaves are write-
   protected. DST is created here; on failure it is already destroyed and the
   caller only frees the vm_space struct. */
int vm_space_clone(vm_space *dst, vm_space *src);

/* Resolve a copy-on-write store on VA in S: 1 when a private copy was made,
   2 when the caller turned out to be the only owner and the leaf just became
   writable again, 0 when the leaf is not copy-on-write and -1 when there is
   no free frame. The caller must flush the VA after a positive result. */
int vm_space_cow_fault(vm_space *s, u64 va);
void vm_space_unmap_user_pages(vm_space *s, u64 va, u64 pages);
u64 vm_space_translate(const vm_space *s, u64 va);
void vm_space_activate(vm_space *s);
u64 vm_kernel_root_phys(void);

/* The space whose root is in CR3, or 0 while the kernel root is active
   (phase U6: the uaccess write path gives shared leaves a private copy). */
vm_space *vm_active_space(void);

#endif