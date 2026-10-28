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
#include <crug/input.h>
#include <crug/net.h>
#include <crug/usb.h>
#include <crug/audio.h>
#include <crug/gfx.h>
#include <crug/workbench.h>

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
    console_write("CrugOS " CRUG_VERSION_STR " (phase 6a) -- Amiga-inspired microkernel bring-up\n");
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

    input_init();
    kprintf("CRUGOS: input ready (%s)\n",
            keyboard_init() == 0 ? "serial + keyboard" : "serial only");

    usb_init();
    usb_cmdline(bi);

    audio_cmdline(bi);
    audio_init();

    gfx_init();
    gfx_cmdline(bi);

    wb_cmdline(bi);
    wb_init();

    net_init(bi);
    kprintf("CRUGOS: net ready (%u device(s))\n", (unsigned)net_device_count());

    if (storage_init(bi) < 0) {
        /* No root filesystem: a missing/unlockable volume is a boot failure,
           not a kernel invariant violation, so exit without a panic dump. */
        kprintf("CRUGOS: no root filesystem\n");
        arch_exit_qemu(0x11);
    }
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

    storage_lock();
    kprintf("CRUGOS: phase 6a boot ok\n");
    trace("K:exit-ok");
    arch_exit_qemu(0x10);
}
