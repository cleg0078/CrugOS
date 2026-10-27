#include <crug/vm.h>
#include <crug/mm.h>
#include <crug/cpu.h>
#include <crug/kernel.h>
#include <crug/string.h>

#define EFER_NXE (1ull << 11)
#define CR0_WP (1ull << 16)
#define CR4_PGE (1ull << 7)

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __bss_start[], __bss_end[];
extern char __stack_guard[];

static u64 *g_pml4;
static u64 *g_pdpt;
static u64 *g_pd[4];

static void *alloc_table(void)
{
    u64 pa = pmm_alloc_page();

    if (pa == 0)
        panic("vm: out of memory for page tables");
    return pmm_phys_to_virt(pa);
}

static int vm_canonical(u64 va)
{
    u64 top = va >> 48;

    if (va & (1ull << 47))
        return top == 0xffffu;
    return top == 0;
}

static u64 *next_table(u64 *table, u64 index, int create)
{
    u64 entry = table[index];
    u64 *next;

    if (entry & VM_PRESENT) {
        if (entry & VM_HUGE)
            return 0;
        return (u64 *)pmm_phys_to_virt(entry & VM_ADDR_MASK);
    }
    if (!create)
        return 0;
    next = (u64 *)alloc_table();
    table[index] = pmm_virt_to_phys(next) | VM_PRESENT | VM_WRITE;
    return next;
}

static int split_huge_pd(u64 *pd, u64 index)
{
    u64 entry = pd[index];
    u64 flags, base;
    u64 *pt;
    int i;

    if (!(entry & VM_PRESENT) || !(entry & VM_HUGE))
        return 0;

    pt = (u64 *)alloc_table();
    flags = entry & (VM_PRESENT | VM_WRITE | VM_USER | VM_PWT | VM_PCD |
                     VM_ACCESSED | VM_DIRTY | VM_GLOBAL | VM_NX);
    base = entry & VM_ADDR_MASK;
    for (i = 0; i < 512; i++)
        pt[i] = (base + (u64)i * CRUG_PAGE_SIZE) | flags;
    /* NX belongs on leaf entries; keep page-table references executable-clean. */
    pd[index] = pmm_virt_to_phys(pt) | (flags & ~(VM_HUGE | VM_NX));
    return 0;
}

static u64 *pte_for(u64 va)
{
    u64 *pdpt, *pd, *pt;

    if (!vm_canonical(va))
        return 0;
    pdpt = next_table(g_pml4, vm_pml4_index(va), 1);
    if (!pdpt)
        return 0;
    pd = next_table(pdpt, vm_pdpt_index(va), 1);
    if (!pd)
        return 0;
    if (pd[vm_pd_index(va)] & VM_HUGE) {
        if (split_huge_pd(pd, vm_pd_index(va)) != 0)
            return 0;
    }
    pt = next_table(pd, vm_pd_index(va), 1);
    if (!pt)
        return 0;
    return &pt[vm_pt_index(va)];
}

static void map_kernel_sections(void)
{
    u64 a;

    for (a = (u64)(uintptr)__text_start;
         a < ALIGN_UP((u64)(uintptr)__text_end, CRUG_PAGE_SIZE);
         a += CRUG_PAGE_SIZE) {
        u64 *p = pte_for(a);
        if (!p) panic("vm: text mapping failed");
        *p = (a & VM_ADDR_MASK) | VM_PRESENT | VM_GLOBAL;
    }

    for (a = (u64)(uintptr)__rodata_start;
         a < ALIGN_UP((u64)(uintptr)__rodata_end, CRUG_PAGE_SIZE);
         a += CRUG_PAGE_SIZE) {
        u64 *p = pte_for(a);
        if (!p) panic("vm: rodata mapping failed");
        *p = (a & VM_ADDR_MASK) | VM_PRESENT | VM_GLOBAL | VM_NX;
    }

    for (a = (u64)(uintptr)__data_start;
         a < ALIGN_UP((u64)(uintptr)__data_end, CRUG_PAGE_SIZE);
         a += CRUG_PAGE_SIZE) {
        u64 *p = pte_for(a);
        if (!p) panic("vm: data mapping failed");
        *p = (a & VM_ADDR_MASK) | VM_PRESENT | VM_WRITE | VM_GLOBAL | VM_NX;
    }

    for (a = (u64)(uintptr)__bss_start;
         a < ALIGN_UP((u64)(uintptr)__bss_end, CRUG_PAGE_SIZE);
         a += CRUG_PAGE_SIZE) {
        u64 *p = pte_for(a);
        if (!p) panic("vm: bss mapping failed");
        *p = (a & VM_ADDR_MASK) | VM_PRESENT | VM_WRITE | VM_GLOBAL | VM_NX;
    }

    {
        u64 *p = pte_for((u64)(uintptr)__stack_guard);
        if (p) *p = 0;
    }
}

int vm_init(void)
{
    int i, j;

    g_pml4 = (u64 *)alloc_table();
    g_pdpt = (u64 *)alloc_table();
    for (i = 0; i < 4; i++)
        g_pd[i] = (u64 *)alloc_table();

    g_pml4[0] = pmm_virt_to_phys(g_pdpt) | VM_PRESENT | VM_WRITE;
    for (i = 0; i < 4; i++) {
        u64 phys_base = (u64)i * 1024ull * 1024ull * 1024ull;

        for (j = 0; j < 512; j++) {
            u64 addr = phys_base + (u64)j * 2ull * 1024ull * 1024ull;
            u64 flags = VM_PRESENT | VM_WRITE | VM_HUGE | VM_GLOBAL | VM_NX;

            if (addr == 0xfee00000ull || addr == 0xfec00000ull)
                flags |= VM_PCD | VM_PWT;
            g_pd[i][j] = addr | flags;
        }
        g_pdpt[i] = pmm_virt_to_phys(g_pd[i]) | VM_PRESENT | VM_WRITE;
    }

    map_kernel_sections();

    write_msr(0xc0000080u, read_msr(0xc0000080u) | EFER_NXE);
    write_cr3(pmm_virt_to_phys(g_pml4));
    write_cr0(read_cr0() | CR0_WP);
    write_cr4(read_cr4() | CR4_PGE);

    kprintf("vm     : 4 GiB identity map, W^X kernel sections, guard=%p\n",
            (void *)(uintptr)__stack_guard);
    return 1;
}

int vm_map_page(u64 va, u64 pa, u64 flags)
{
    u64 *p;

    if ((va & (CRUG_PAGE_SIZE - 1)) != 0 || (pa & (CRUG_PAGE_SIZE - 1)) != 0)
        return -1;
    if (!vm_canonical(va) || (pa & ~VM_ADDR_MASK) != 0)
        return -1;
    p = pte_for(va);
    if (!p)
        return -1;
    if (*p & VM_PRESENT)
        return -1;
    *p = (pa & VM_ADDR_MASK) | (flags & VM_PTE_FLAG_MASK);
    vm_flush_page(va);
    return 0;
}

int vm_map_range(u64 va, u64 pa, u64 size, u64 flags)
{
    u64 off;

    if ((va & (CRUG_PAGE_SIZE - 1)) != 0 || (pa & (CRUG_PAGE_SIZE - 1)) != 0)
        return -1;
    for (off = 0; off < size; off += CRUG_PAGE_SIZE) {
        if (vm_map_page(va + off, pa + off, flags) != 0)
            return -1;
    }
    return 0;
}

int vm_unmap_page(u64 va)
{
    u64 *p = pte_for(va);

    if (!p || !(*p & VM_PRESENT))
        return -1;
    *p = 0;
    vm_flush_page(va);
    return 0;
}

int vm_protect_page(u64 va, u64 flags)
{
    u64 *p = pte_for(va);

    if (!p || !(*p & VM_PRESENT))
        return -1;
    *p = (*p & VM_ADDR_MASK) | (flags & VM_PTE_FLAG_MASK);
    vm_flush_page(va);
    return 0;
}

u64 vm_translate(u64 va)
{
    u64 *pdpt, *pd, *pt, e;

    if (!vm_canonical(va))
        return 0;
    e = g_pml4[vm_pml4_index(va)];
    if (!(e & VM_PRESENT) || (e & VM_HUGE))
        return 0;
    pdpt = (u64 *)pmm_phys_to_virt(e & VM_ADDR_MASK);
    e = pdpt[vm_pdpt_index(va)];
    if (!(e & VM_PRESENT) || (e & VM_HUGE))
        return 0;
    pd = (u64 *)pmm_phys_to_virt(e & VM_ADDR_MASK);
    e = pd[vm_pd_index(va)];
    if (!(e & VM_PRESENT))
        return 0;
    if (e & VM_HUGE)
        return (e & VM_ADDR_MASK) + (va & 0x1fffffull);
    pt = (u64 *)pmm_phys_to_virt(e & VM_ADDR_MASK);
    e = pt[vm_pt_index(va)];
    if (!(e & VM_PRESENT))
        return 0;
    return (e & VM_ADDR_MASK) + (va & 0xfffull);
}

void vm_flush_page(u64 va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

void vm_flush_all(void)
{
    write_cr3(read_cr3());
}

int vm_audit_wx(void)
{
    int i, j, k;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 512; j++) {
            u64 e = g_pd[i][j];

            if (!(e & VM_PRESENT))
                continue;
            if (e & VM_HUGE) {
                if ((e & VM_WRITE) && !(e & VM_NX))
                    return 0;
                continue;
            }
            {
                u64 *pt = (u64 *)pmm_phys_to_virt(e & VM_ADDR_MASK);

                for (k = 0; k < 512; k++) {
                    u64 p = pt[k];

                    if (!(p & VM_PRESENT))
                        continue;
                    if ((p & VM_WRITE) && !(p & VM_NX))
                        return 0;
                }
            }
        }
    }
    if (vm_translate((u64)(uintptr)__stack_guard) != 0)
        return 0;
    return 1;
}

int vm_selftest(void)
{
    const u64 va = 0x40000000ull;
    const u64 pattern = 0xdeadbeefcafef00dull;
    u64 pa = pmm_alloc_page();
    volatile u64 *p;

    if (pa == 0)
        return 0;
    (void)vm_unmap_page(va);
    if (vm_map_page(va, pa, VM_PRESENT | VM_WRITE | VM_GLOBAL | VM_NX) != 0)
        return 0;
    if (vm_translate(va) != pa)
        return 0;
    p = (volatile u64 *)(uintptr)va;
    *p = pattern;
    if (*p != pattern)
        return 0;
    if (vm_protect_page(va, VM_PRESENT | VM_GLOBAL | VM_NX) != 0)
        return 0;
    if (vm_unmap_page(va) != 0)
        return 0;
    if (vm_translate(va) != 0)
        return 0;
    pmm_free_page(pa);
    return 1;
}
