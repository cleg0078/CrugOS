/* CrugOS Phase 0 kernel entry. */
#include <crug/types.h>
#include <crug/compiler.h>
#include <crug/io.h>
#include <crug/kernel.h>

void kernel_main(const boot_info *bi);

void kernel_main(const boot_info *bi)
{
    u64 tsc_start;

    serial_init();
    trace("K:entry");
    tsc_start = arch_read_tsc();

    bootinfo_validate(bi);

    fb_init(&bi->fb);
    console_init();
    trace("K:console");

    console_write("\n");
    console_write("CrugOS 0.1 (phase 0) -- Amiga-inspired microkernel bring-up\n");
    console_write("CRUGOS: hello\n");

    report_boot(bi, arch_read_tsc() - tsc_start);

    kprintf("CRUGOS: phase 0 boot ok\n");
    trace("K:exit-ok");
    arch_exit_qemu(0x10);
}
