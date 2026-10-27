/* Phase 0 boot report. */
#include <crug/types.h>
#include <crug/kernel.h>

static const char *pixel_format_name(u32 format)
{
    switch (format) {
    case CRUG_PIXEL_RGBX8888:
        return "RGBX8888";
    case CRUG_PIXEL_BGRX8888:
        return "BGRX8888";
    case CRUG_PIXEL_BITMASK:
        return "BITMASK";
    default:
        return "UNKNOWN";
    }
}

void report_boot(const boot_info *bi, u64 tsc_delta)
{
    u64 usable = bootinfo_usable_bytes(bi);

    kprintf("kernel : %p size=%llu KiB\n", (void *)(uintptr)bi->kernel_base,
            bi->kernel_size / 1024);
    kprintf("boot   : cpu=%llu acpi_rsdp=0x%llx fdt=0x%llx initrd=0x%llx\n",
            bi->boot_cpu_id, bi->acpi_rsdp, bi->fdt, bi->initrd_base);
    kprintf("memory : %llu regions, usable %llu KiB (%llu MiB)\n",
            bi->memory_map_count, usable / 1024, usable / (1024 * 1024));
    if (bi->fb.base) {
        kprintf("fb     : %ux%u stride=%u format=%s base=0x%llx\n",
                bi->fb.width, bi->fb.height, bi->fb.stride,
                pixel_format_name(bi->fb.format), bi->fb.base);
    } else {
        kprintf("fb     : none (serial console only)\n");
    }
    kprintf("cmdline: %s\n",
            (bi->command_line && bi->command_line_len) ? bi->command_line : "(none)");
    kprintf("entropy: %02x%02x%02x%02x%02x%02x%02x%02x\n", bi->entropy_seed[0],
            bi->entropy_seed[1], bi->entropy_seed[2], bi->entropy_seed[3],
            bi->entropy_seed[4], bi->entropy_seed[5], bi->entropy_seed[6],
            bi->entropy_seed[7]);
    kprintf("boot   : stage delta %llu TSC ticks (frequency calibration pending)\n",
            tsc_delta);
}
