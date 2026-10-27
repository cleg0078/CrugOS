/* Phase 2 boot report. */
#include <crug/types.h>
#include <crug/kernel.h>
#include <crug/mm.h>
#include <crug/vm.h>
#include <crug/timer.h>
#include <crug/heap.h>
#include <crug/thread.h>

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

static u64 stage_us(u64 from, u64 to)
{
    if (to <= from)
        return 0;
    return timer_tsc_to_us(to - from);
}

void report_boot(const boot_info *bi, const boot_timings *t)
{
    u64 usable = bootinfo_usable_bytes(bi);

    kprintf("kernel : %p size=%llu KiB\n", (void *)(uintptr)bi->kernel_base,
            bi->kernel_size / 1024);
    kprintf("boot   : cpu=%llu acpi_rsdp=0x%llx fdt=0x%llx initrd=0x%llx\n",
            bi->boot_cpu_id, bi->acpi_rsdp, bi->fdt, bi->initrd_base);
    kprintf("memory : %llu regions, usable %llu KiB (%llu MiB)\n",
            bi->memory_map_count, usable / 1024, usable / (1024 * 1024));
    kprintf("pmm    : total=%llu free=%llu bitmap=%llu B\n",
            pmm_total_pages(), pmm_free_pages_count(), pmm_bitmap_bytes());
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
    kprintf("wx     : %s, identity map 0..4 GiB, guard unmapped\n",
            vm_audit_wx() ? "audit ok" : "AUDIT FAILED");
    kprintf("heap   : total=%llu B used=%llu B slab_pages=%llu large_pages=%llu\n",
            heap_total_bytes(), heap_used_bytes(), heap_slab_pages(),
            heap_large_pages());
    kprintf("sched  : switches=%llu\n", sched_switches());
    kprintf("boot   : pmm=%llu us cpu=%llu us vm=%llu us traps=%llu us timer=%llu us heap=%llu us storage=%llu us sched=%llu us syscall=%llu us total=%llu us\n",
            stage_us(t->start, t->pmm), stage_us(t->pmm, t->cpu),
            stage_us(t->cpu, t->vm), stage_us(t->vm, t->traps),
            stage_us(t->traps, t->timer), stage_us(t->timer, t->heap),
            stage_us(t->heap, t->storage), stage_us(t->storage, t->sched),
            stage_us(t->sched, t->syscall),
            stage_us(t->start, t->syscall));
    kprintf("timer  : %llu Hz (%s), ticks=%llu, mono=%llu ns\n",
            timer_tsc_hz(), timer_source(), timer_ticks(), timer_monotonic_ns());
}
