/* CrugOS per-process address space (phase U2).
 *
 * A vm_space is one PML4: slot 0 is a copy of the kernel's shared supervisor
 * identity map, slot 1 carries the 512 GiB user window. The kernel keeps one
 * space per ring-3 thread and switches CR3 on every context switch, so two
 * programs see the same virtual addresses over different physical pages.
 *
 * This file only depends on the page allocator (pmm_alloc_page/pmm_free_page)
 * and the identity translations in <crug/mm.h>, never on CR3 or the LAPIC, so
 * the host unit tests link it unchanged and reuse the fake arena from
 * tests/host/test_heap.c. Writing CR3 lives in kernel/mm/vm.c.
 */
#include <crug/vm.h>
#include <crug/mm.h>
#include <crug/string.h>
#include <crug/kernel.h>

#define SPACE_SLOT 1u
#define PAGE_MASK (CRUG_PAGE_SIZE - 1ull)

static u64 *g_kernel_root;

void vm_space_set_kernel_root(u64 *root)
{
    g_kernel_root = root;
}

static void space_free_table(void *table)
{
    if (table)
        pmm_free_page(pmm_virt_to_phys(table));
}

/* Return the next level table, creating it when create is set. A fresh user
   table is reachable at CPL3, so its parent entry carries VM_USER. */
static u64 *space_next(u64 *table, u64 index, int create)
{
    u64 entry = table[index];
    u64 *next;

    if (entry & VM_PRESENT) {
        if (entry & VM_HUGE)
            return 0;
        if ((entry & VM_USER) == 0)
            table[index] = entry | VM_USER | VM_WRITE;
        return (u64 *)pmm_phys_to_virt(entry & VM_ADDR_MASK);
    }
    if (!create)
        return 0;
    next = (u64 *)pmm_phys_to_virt(pmm_alloc_page());
    if (!next)
        return 0;
    memset(next, 0, CRUG_PAGE_SIZE);
    table[index] = pmm_virt_to_phys(next) | VM_PRESENT | VM_WRITE | VM_USER;
    return next;
}

/* Walk to the leaf PTE of VA inside S, creating intermediate tables. */
static u64 *space_pte(vm_space *s, u64 va)
{
    u64 *pdpt;
    u64 *pd;
    u64 *pt;

    if (!s || !s->pml4)
        return 0;
    if (vm_pml4_index(va) != SPACE_SLOT)
        return 0;
    pdpt = space_next(s->pml4, SPACE_SLOT, 1);
    if (!pdpt)
        return 0;
    pd = space_next(pdpt, vm_pdpt_index(va), 1);
    if (!pd)
        return 0;
    if (pd[vm_pd_index(va)] & VM_HUGE)
        return 0;
    pt = space_next(pd, vm_pd_index(va), 1);
    if (!pt)
        return 0;
    return &pt[vm_pt_index(va)];
}

/* Read-only walk to the leaf PTE, or 0 when any level is missing. */
static u64 *space_find_pte(const vm_space *s, u64 va)
{
    u64 entry = 0;
    u64 *pdpt;
    u64 *pd;
    u64 *pt;

    if (!s || !s->pml4 || vm_pml4_index(va) != SPACE_SLOT)
        return 0;
    entry = s->pml4[SPACE_SLOT];
    if (!(entry & VM_PRESENT) || (entry & VM_HUGE))
        return 0;
    pdpt = (u64 *)pmm_phys_to_virt(entry & VM_ADDR_MASK);
    entry = pdpt[vm_pdpt_index(va)];
    if (!(entry & VM_PRESENT) || (entry & VM_HUGE))
        return 0;
    pd = (u64 *)pmm_phys_to_virt(entry & VM_ADDR_MASK);
    entry = pd[vm_pd_index(va)];
    if (!(entry & VM_PRESENT) || (entry & VM_HUGE))
        return 0;
    pt = (u64 *)pmm_phys_to_virt(entry & VM_ADDR_MASK);
    return &pt[vm_pt_index(va)];
}

int vm_space_create(vm_space *s)
{
    u64 *root;

    if (!s || !g_kernel_root)
        return -1;
    root = (u64 *)pmm_phys_to_virt(pmm_alloc_page());
    if (!root)
        return -1;
    memset(root, 0, CRUG_PAGE_SIZE);
    /* Slot 0 is shared with the kernel on purpose: every space must see the
       identity map, the kernel image and the kernel stacks. */
    root[0] = g_kernel_root[0];
    s->pml4 = root;
    s->phys = pmm_virt_to_phys(root);
    s->user_pages = 0;
    s->pad = 0;
    return 0;
}

void vm_space_destroy(vm_space *s)
{
    u64 entry;
    u64 *pdpt;
    u64 *pd;
    u64 *pt;
    u64 i;
    u64 j;
    u64 k;

    if (!s || !s->pml4)
        return;
    entry = s->pml4[SPACE_SLOT];
    if ((entry & VM_PRESENT) && !(entry & VM_HUGE)) {
        pdpt = (u64 *)pmm_phys_to_virt(entry & VM_ADDR_MASK);
        for (i = 0; i < 512; i++) {
            u64 e1 = pdpt[i];

            if (!(e1 & VM_PRESENT) || (e1 & VM_HUGE))
                continue;
            pd = (u64 *)pmm_phys_to_virt(e1 & VM_ADDR_MASK);
            for (j = 0; j < 512; j++) {
                u64 e2 = pd[j];

                if (!(e2 & VM_PRESENT) || (e2 & VM_HUGE))
                    continue;
                pt = (u64 *)pmm_phys_to_virt(e2 & VM_ADDR_MASK);
                for (k = 0; k < 512; k++) {
                    u64 e3 = pt[k];

                    if (e3 & VM_PRESENT)
                        pmm_free_page(e3 & VM_ADDR_MASK);
                }
                space_free_table(pt);
            }
            space_free_table(pd);
        }
        space_free_table(pdpt);
    }
    s->pml4[SPACE_SLOT] = 0;
    space_free_table(s->pml4);
    s->pml4 = 0;
    s->phys = 0;
    s->user_pages = 0;
}

int vm_space_map_user_pages(vm_space *s, u64 va, u64 pages, u64 flags)
{
    u64 i;

    if (!s || !s->pml4 || pages == 0)
        return -1;
    if ((va & PAGE_MASK) != 0)
        return -1;
    if (va < USER_BASE || va >= USER_TOP)
        return -1;
    if (pages > (USER_TOP - va) / CRUG_PAGE_SIZE)
        return -1;
    for (i = 0; i < pages; i++) {
        u64 page_va = va + i * CRUG_PAGE_SIZE;
        u64 *pte = space_pte(s, page_va);
        u64 pa;

        if (!pte || (*pte & VM_PRESENT)) {
            vm_space_unmap_user_pages(s, va, i);
            return -1;
        }
        pa = pmm_alloc_page();
        if (pa == 0) {
            vm_space_unmap_user_pages(s, va, i);
            return -1;
        }
        memset(pmm_phys_to_virt(pa), 0, CRUG_PAGE_SIZE);
        *pte = (pa & VM_ADDR_MASK) |
               ((flags | VM_USER) & ~VM_GLOBAL & VM_PTE_FLAG_MASK);
        s->user_pages++;
    }
    return 0;
}

void vm_space_unmap_user_pages(vm_space *s, u64 va, u64 pages)
{
    u64 i;

    if (!s || !s->pml4)
        return;
    for (i = 0; i < pages; i++) {
        u64 page_va = va + i * CRUG_PAGE_SIZE;
        u64 *pte = space_find_pte(s, page_va);
        u64 pa;

        if (!pte || !(*pte & VM_PRESENT))
            continue;
        pa = *pte & VM_ADDR_MASK;
        *pte = 0;
        pmm_free_page(pa);
        if (s->user_pages != 0)
            s->user_pages--;
    }
}

u64 vm_space_translate(const vm_space *s, u64 va)
{
    u64 *pte = space_find_pte(s, va);

    if (!pte || !(*pte & VM_PRESENT))
        return 0;
    return (*pte & VM_ADDR_MASK) + (va & PAGE_MASK);
}