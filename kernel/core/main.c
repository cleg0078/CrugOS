/* CrugOS Phase 3 kernel entry. */
#include <crug/types.h>
#include <crug/compiler.h>
#include <crug/io.h>
#include <crug/kernel.h>
#include <crug/version.h>
#include <crug/mm.h>
#include <crug/cpu.h>
#include <crug/vm.h>
#include <crug/timer.h>
#include <crug/heap.h>
#include <crug/thread.h>
#include <crug/process.h>
#include <crug/storage.h>

void kernel_main(const boot_info *bi);

void kernel_main(const boot_info *bi)
{
    boot_timings t;

    serial_init();
    trace("K:entry");
    t.start = arch_read_tsc();

    bootinfo_validate(bi);

    fb_init(&bi->fb);
    console_init();
    trace("K:console");

    console_write("\n");
    console_write("CrugOS " CRUG_VERSION_STR " (phase 3b3) -- Amiga-inspired microkernel bring-up\n");
    console_write("CRUGOS: hello\n");
    kprintf("CRUGOS: version %s\n", CRUG_VERSION_STR);

    pmm_init(bi);
    t.pmm = arch_read_tsc();
    kprintf("CRUGOS: pmm ready\n");

    cpu_early_init();
    t.cpu = arch_read_tsc();

    vm_init();
    t.vm = arch_read_tsc();
    kprintf("CRUGOS: vm ready\n");

    cpu_late_init();
    t.traps = arch_read_tsc();
    kprintf("CRUGOS: cpu ready\n");

    timer_init();
    t.timer = arch_read_tsc();
    kprintf("CRUGOS: timer ready\n");

    heap_init();
    t.heap = arch_read_tsc();
    kprintf("CRUGOS: heap ready\n");

    if (storage_init() < 0)
        panic("storage init failed");
    t.storage = arch_read_tsc();

    sched_init();
    t.sched = arch_read_tsc();
    kprintf("CRUGOS: sched ready\n");

    process_init();
    t.syscall = arch_read_tsc();
    kprintf("CRUGOS: syscall ready\n");

    __asm__ volatile("sti");

    selftest_run();
    kprintf("CRUGOS: selftest ok\n");

    report_boot(bi, &t);

    kprintf("CRUGOS: phase 3b3 boot ok\n");
    trace("K:exit-ok");
    arch_exit_qemu(0x10);
}
