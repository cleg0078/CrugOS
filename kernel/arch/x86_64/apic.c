#include <crug/apic.h>
#include <crug/cpu.h>
#include <crug/io.h>
#include <crug/kernel.h>

#define MSR_APIC_BASE 0x1bu
#define APIC_BASE_ENABLE (1ull << 11)
#define APIC_BASE_BSP (1ull << 8)

static volatile u32 *g_lapic;

u32 lapic_read(u32 offset)
{
    return g_lapic[offset / 4u];
}

void lapic_write(u32 offset, u32 value)
{
    g_lapic[offset / 4u] = value;
    (void)g_lapic[0x20u / 4u];
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_REG_EOI, 0);
}

u32 lapic_id(void)
{
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void pic_mask_all(void)
{
    outb(0x21, 0xff);
    outb(0xa1, 0xff);
    io_wait();
}

void apic_init(void)
{
    u64 base;

    if (!g_cpu_features.apic)
        panic("apic: local APIC not reported by CPUID");

    base = read_msr(MSR_APIC_BASE);
    base |= APIC_BASE_ENABLE;
    if (!(base & APIC_BASE_BSP))
        base |= APIC_BASE_BSP;
    write_msr(MSR_APIC_BASE, base);
    g_lapic = (volatile u32 *)(uintptr)(base & 0xfffff000ull);

    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_SVR, 0x1ffu);

    if (lapic_id() != 0) {
        /* Boot CPU id is informative only in phase 1. */
    }
}
