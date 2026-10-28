/* Host tests for the per-process address space (phase U2).
 *
 * kernel/mm/vm_space.c deliberately never touches CR3 or the LAPIC, so the
 * host binary links it unchanged and reuses the fake physical allocator owned
 * by tests/host/test_heap.c.  The tests cover the slot-0 inheritance contract,
 * window validation, the raw leaf PTE flags, frame distinctness between two
 * spaces, partial rollback and full reclaim on destroy.
 *
 * vm_space_activate() lives in kernel/mm/vm.c because it writes CR3; it is
 * covered by the test-user2 boot gate, not here.
 */
#include <crug/vm.h>
#include <crug/mm.h>
#include <crug/string.h>
#include "test.h"

/* The fake PMM in test_heap.c reports how many arena pages are free, so a
 * leak in create/map/destroy shows up as a mismatch instead of hiding. */
u64 host_pmm_free_pages_count(void);

#define SPACE_PAGE (CRUG_PAGE_SIZE)
#define SPACE_DATA (VM_PRESENT | VM_WRITE | VM_NX)
#define SPACE_TEXT (VM_PRESENT)

/* Read-only walk to a leaf PTE: vm_space_translate() hides the permission
 * bits on purpose, and the tests need to see them. */
static u64 *leaf_pte(const vm_space *s, u64 va)
{
    u64 entry;
    u64 *pdpt;
    u64 *pd;
    u64 *pt;

    if (!s || !s->pml4)
        return 0;
    entry = s->pml4[vm_pml4_index(va)];
    if (!(entry & VM_PRESENT))
        return 0;
    pdpt = (u64 *)pmm_phys_to_virt(entry & VM_ADDR_MASK);
    entry = pdpt[vm_pdpt_index(va)];
    if (!(entry & VM_PRESENT))
        return 0;
    pd = (u64 *)pmm_phys_to_virt(entry & VM_ADDR_MASK);
    entry = pd[vm_pd_index(va)];
    if (!(entry & VM_PRESENT))
        return 0;
    pt = (u64 *)pmm_phys_to_virt(entry & VM_ADDR_MASK);
    return &pt[vm_pt_index(va)];
}

static u64 *make_kernel_root(void)
{
    u64 *root = (u64 *)pmm_phys_to_virt(pmm_alloc_page());

    if (!root)
        return 0;
    memset(root, 0, CRUG_PAGE_SIZE);
    /* A plausible slot-0 entry: the kernel identity map starts at 0. */
    root[0] = (u64)0x1000u | VM_PRESENT | VM_WRITE;
    return root;
}

void test_vm_space_functions(void)
{
    vm_space a;
    vm_space b;
    vm_space c;
    u64 *kroot;
    u64 *pte;
    u64 before;
    u64 after;
    u64 pa;
    u64 pb;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));

    kroot = make_kernel_root();
    CHECK(kroot != 0);
    if (!kroot)
        return;

    /* Creating a space without a published kernel root must refuse cleanly. */
    vm_space_set_kernel_root(0);
    CHECK(vm_space_create(&a) == -1);
    CHECK(a.pml4 == 0);
    vm_space_destroy(0);

    vm_space_set_kernel_root(kroot);
    before = host_pmm_free_pages_count();
    CHECK(vm_space_create(&a) == 0);
    CHECK(vm_space_create(&b) == 0);
    CHECK(vm_space_create(0) == -1);
    CHECK(a.pml4 != 0);
    CHECK(b.pml4 != 0);
    CHECK(a.pml4 != b.pml4);
    /* Slot 0 is the shared supervisor identity map: copied, never owned. */
    CHECK(a.pml4[0] == kroot[0]);
    CHECK(b.pml4[0] == kroot[0]);
    CHECK((a.pml4[0] & VM_USER) == 0);
    CHECK(a.user_pages == 0);
    CHECK(b.user_pages == 0);

    /* The user window is empty until something is mapped into it. */
    CHECK(vm_space_translate(&a, USER_BASE) == 0);
    CHECK(vm_space_translate(&a, USER_TOP - SPACE_PAGE) == 0);

    /* The same virtual address lands on different frames in two spaces: the
     * isolation property the U2 boot gate proves on real hardware. */
    CHECK(vm_space_map_user_pages(&a, USER_BASE, 1, SPACE_DATA) == 0);
    CHECK(vm_space_map_user_pages(&b, USER_BASE, 1, SPACE_DATA) == 0);
    pa = vm_space_translate(&a, USER_BASE);
    pb = vm_space_translate(&b, USER_BASE);
    CHECK(pa != 0);
    CHECK(pb != 0);
    CHECK(pa != pb);
    CHECK((pa & (SPACE_PAGE - 1)) == 0);
    CHECK((pb & (SPACE_PAGE - 1)) == 0);
    CHECK(a.user_pages == 1);
    CHECK(b.user_pages == 1);

    /* Translation carries the offset inside the page. */
    CHECK(vm_space_translate(&a, USER_BASE + 7) == pa + 7);

    /* A data page is writable, non-executable and reachable at CPL3. */
    pte = leaf_pte(&a, USER_BASE);
    CHECK(pte != 0);
    if (pte) {
        CHECK((*pte & VM_PRESENT) != 0);
        CHECK((*pte & VM_USER) != 0);
        CHECK((*pte & VM_GLOBAL) == 0);
        CHECK((*pte & VM_WRITE) != 0);
        CHECK((*pte & VM_NX) != 0);
    }

    /* A text page is executable, read only and still user reachable. */
    CHECK(vm_space_map_user_pages(&a, USER_BASE + 2 * SPACE_PAGE, 1, SPACE_TEXT) == 0);
    CHECK(a.user_pages == 2);
    pte = leaf_pte(&a, USER_BASE + 2 * SPACE_PAGE);
    CHECK(pte != 0);
    if (pte) {
        CHECK((*pte & VM_USER) != 0);
        CHECK((*pte & VM_GLOBAL) == 0);
        CHECK((*pte & VM_WRITE) == 0);
        CHECK((*pte & VM_NX) == 0);
    }

    /* Mapping over a live page fails and leaves the space untouched. */
    CHECK(vm_space_map_user_pages(&a, USER_BASE, 1, SPACE_DATA) == -1);
    CHECK(a.user_pages == 2);
    CHECK(vm_space_translate(&a, USER_BASE) == pa);

    /* Window and argument validation. */
    CHECK(vm_space_map_user_pages(0, USER_BASE, 1, SPACE_DATA) == -1);
    CHECK(vm_space_map_user_pages(&a, USER_BASE + 1, 1, SPACE_DATA) == -1);
    CHECK(vm_space_map_user_pages(&a, USER_BASE - SPACE_PAGE, 1, SPACE_DATA) == -1);
    CHECK(vm_space_map_user_pages(&a, USER_TOP, 1, SPACE_DATA) == -1);
    CHECK(vm_space_map_user_pages(&a, USER_TOP - SPACE_PAGE, 2, SPACE_DATA) == -1);
    CHECK(vm_space_map_user_pages(&a, USER_BASE, 0, SPACE_DATA) == -1);
    CHECK(vm_space_map_user_pages(&a, USER_BASE,
                                  (USER_TOP - USER_BASE) / SPACE_PAGE + 1ull,
                                  SPACE_DATA) == -1);
    CHECK(a.user_pages == 2);

    /* The last page of the window is mappable; the first one past it is not. */
    CHECK(vm_space_map_user_pages(&b, USER_TOP - SPACE_PAGE, 1, SPACE_DATA) == 0);
    CHECK(vm_space_translate(&b, USER_TOP - SPACE_PAGE) != 0);
    CHECK(vm_space_translate(&b, USER_TOP) == 0);
    CHECK(b.user_pages == 2);
    vm_space_unmap_user_pages(&b, USER_TOP - SPACE_PAGE, 1);
    CHECK(vm_space_translate(&b, USER_TOP - SPACE_PAGE) == 0);
    CHECK(b.user_pages == 1);

    /* A collision part way through a range rolls back only the pages this
     * call had already mapped.  Clear USER_BASE first so the range below
     * starts from an empty window. */
    vm_space_unmap_user_pages(&b, USER_BASE, 1);
    CHECK(b.user_pages == 0);
    CHECK(vm_space_translate(&b, USER_BASE) == 0);
    CHECK(vm_space_map_user_pages(&b, USER_BASE + SPACE_PAGE, 1, SPACE_DATA) == 0);
    CHECK(b.user_pages == 1);
    CHECK(vm_space_map_user_pages(&b, USER_BASE, 3, SPACE_DATA) == -1);
    CHECK(vm_space_translate(&b, USER_BASE) == 0);
    CHECK(vm_space_translate(&b, USER_BASE + SPACE_PAGE) != 0);
    CHECK(vm_space_translate(&b, USER_BASE + 2 * SPACE_PAGE) == 0);
    CHECK(b.user_pages == 1);
    vm_space_unmap_user_pages(&b, USER_BASE + SPACE_PAGE, 1);
    CHECK(b.user_pages == 0);

    /* Unmap releases exactly the requested pages and skips absent ones. */
    vm_space_unmap_user_pages(&a, USER_BASE, 1);
    CHECK(vm_space_translate(&a, USER_BASE) == 0);
    CHECK(vm_space_translate(&a, USER_BASE + 2 * SPACE_PAGE) != 0);
    CHECK(a.user_pages == 1);
    vm_space_unmap_user_pages(&a, USER_BASE, 4);
    CHECK(a.user_pages == 0);
    CHECK(vm_space_translate(&a, USER_BASE + 2 * SPACE_PAGE) == 0);

    /* Destroy returns every leaf frame and every table page. */
    vm_space_destroy(&a);
    vm_space_destroy(&b);
    CHECK(a.pml4 == 0);
    CHECK(a.phys == 0);
    CHECK(a.user_pages == 0);
    CHECK(b.pml4 == 0);
    vm_space_destroy(&a);
    after = host_pmm_free_pages_count();
    CHECK(after == before);
}

/* The test above proves the collision rollback. This one forces the other
 * failure path, an exhausted page allocator, by asking for the whole 512 GiB
 * window: the arena runs out, the call reports failure and rolls back the
 * frames it already took. Intermediate tables stay until destroy, which is the
 * documented U2 limit. */
void test_vm_space_oom(void)
{
    vm_space c;
    u64 before;
    u64 after;
    u64 pages;

    memset(&c, 0, sizeof(c));
    vm_space_set_kernel_root((u64 *)0);

    /* make_kernel_root from the other test is not shared, so publish a fresh
     * one; this also keeps the two tests independent when run in any order. */
    {
        u64 *root = make_kernel_root();

        CHECK(root != 0);
        if (!root)
            return;
        vm_space_set_kernel_root(root);
    }

    before = host_pmm_free_pages_count();
    CHECK(vm_space_create(&c) == 0);
    CHECK(before > 16);

    pages = (USER_TOP - USER_BASE) / SPACE_PAGE;
    CHECK(vm_space_map_user_pages(&c, USER_BASE, pages, SPACE_DATA) == -1);
    CHECK(c.user_pages == 0);
    CHECK(vm_space_translate(&c, USER_BASE) == 0);
    CHECK(vm_space_translate(&c, USER_BASE + pages * SPACE_PAGE - SPACE_PAGE) == 0);

    vm_space_destroy(&c);
    after = host_pmm_free_pages_count();
    CHECK(after == before);
}
