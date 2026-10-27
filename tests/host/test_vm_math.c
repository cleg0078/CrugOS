#include <crug/vm.h>
#include <crug/mm.h>
#include "test.h"

void test_vm_math(void)
{
    CHECK(CRUG_PAGE_SIZE == 4096);
    CHECK((0x12345000ull & CRUG_PAGE_MASK) == 0x12345000ull);
    CHECK(ALIGN_UP(0x1234ull, 4096) == 0x2000ull);
    CHECK(ALIGN_DOWN(0x1fffell, 4096) == 0x1f000ull);

    CHECK(vm_pml4_index(0) == 0);
    CHECK(vm_pdpt_index(0) == 0);
    CHECK(vm_pd_index(0) == 0);
    CHECK(vm_pt_index(0) == 0);

    CHECK(vm_pdpt_index(0x40000000ull) == 1);
    CHECK(vm_pd_index(0x40000000ull) == 0);
    CHECK(vm_pdpt_index(0x80000000ull) == 2);
    CHECK(vm_pdpt_index(0xffffffffull) == 3);

    CHECK(vm_pd_index(0x200000ull) == 1);
    CHECK(vm_pd_index(0x3fe00000ull) == 511);
    CHECK(vm_pt_index(0x12345000ull) == 0x145);
    CHECK(vm_pt_index(0x00000fffull) == 0x000);

    CHECK(vm_pml4_index(0x00007fffffffffffull) == 0xff);
    CHECK(vm_pml4_index(0xffff800000000000ull) == 0x100);
}

