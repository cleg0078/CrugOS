#include <crug/kernel.h>
#include <crug/mm.h>
#include <crug/vm.h>
#include <crug/trap.h>
#include <crug/timer.h>

void selftest_run(void)
{
    if (!pmm_selftest())
        panic("selftest: physical allocator");
    if (!vm_selftest())
        panic("selftest: virtual memory");
    if (!vm_audit_wx())
        panic("selftest: W^X audit");
    if (!trap_selftest_int3())
        panic("selftest: breakpoint exception");
    if (!timer_selftest())
        panic("selftest: timer interrupt");
}
