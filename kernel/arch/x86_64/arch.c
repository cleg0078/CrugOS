/* x86_64 architecture glue for Phase 0. */
#include <crug/types.h>
#include <crug/io.h>
#include <crug/kernel.h>

#define QEMU_ISA_DEBUG_EXIT_PORT 0xF4

void arch_early_init(void)
{
    serial_init();
}

u64 arch_read_tsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

u32 arch_cpu_id(void)
{
    u32 a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return b >> 24;
}

void arch_halt(void)
{
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* QEMU isa-debug-exit: guest write N makes QEMU exit with (N << 1) | 1. */
void arch_exit_qemu(u8 code)
{
    outb(QEMU_ISA_DEBUG_EXIT_PORT, code);
    arch_halt();
}
