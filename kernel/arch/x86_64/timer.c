#include <crug/timer.h>
#include <crug/apic.h>
#include <crug/cpu.h>
#include <crug/io.h>
#include <crug/kernel.h>
#include <crug/thread.h>

#define PIT_HZ 1193182ull
#define MSR_TSC_DEADLINE 0x6e0u
#define TIMER_VECTOR 32u

static u64 g_tsc_hz;
static u64 g_boot_tsc;
static volatile u64 g_ticks;
static u64 g_tsc_per_tick;
static const char *g_source;
static int g_deadline_mode;

static u64 tsc_from_cpuid(void)
{
    u32 a, b, c, d;

    cpuid(0, 0, &a, &b, &c, &d);
    if (a < 0x15u)
        return 0;

    cpuid(0x15u, 0, &a, &b, &c, &d);
    if (a != 0 && b != 0 && c != 0) {
        g_source = "cpuid.15";
        return ((u64)c * b) / a;
    }

    cpuid(0x16u, 0, &a, &b, &c, &d);
    if (a != 0) {
        g_source = "cpuid.16";
        return (u64)a * 1000000ull;
    }
    return 0;
}

static u64 tsc_from_pit(void)
{
    u64 t0, t1;
    u32 spins = 0;
    u8 v;

    v = inb(0x61);
    outb(0x61, (u8)(v & ~0x03u));

    outb(0x43, 0xb0);          /* channel 2, mode 0, lo/hi */
    outb(0x42, 0xff);
    outb(0x42, 0xff);          /* 65535 PIT ticks */

    outb(0x61, (u8)((v & ~0x02u) | 0x01u));
    t0 = arch_read_tsc();
    while ((inb(0x61) & 0x20u) == 0) {
        if (++spins > 200000000u)
            return 0;
    }
    t1 = arch_read_tsc();
    outb(0x61, (u8)(v & ~0x03u));

    g_source = "pit";
    return ((t1 - t0) * PIT_HZ) / 65535ull;
}

static u64 detect_tsc_hz(void)
{
    u64 hz;

    hz = tsc_from_cpuid();
    if (hz == 0)
        hz = tsc_from_pit();
    return hz;
}

static u64 lapic_hz_from_tsc(void)
{
    u64 cur, t0, t1, elapsed, wait_us;

    lapic_write(LAPIC_REG_TIMER_DIV, 0x3u);
    lapic_write(LAPIC_REG_LVT_TIMER, TIMER_VECTOR | (1u << 16));
    lapic_write(LAPIC_REG_TIMER_INIT, 0xffffffffu);
    t0 = arch_read_tsc();
    timer_busy_wait_us(10000);
    t1 = arch_read_tsc();
    cur = lapic_read(LAPIC_REG_TIMER_CUR);
    elapsed = 0xffffffffull - cur;
    wait_us = timer_tsc_to_us(t1 - t0);
    if (wait_us == 0 || elapsed == 0)
        return 0;
    return (elapsed * 1000000ull) / wait_us;
}

int timer_init(void)
{
    u64 hz;

    g_boot_tsc = arch_read_tsc();
    hz = detect_tsc_hz();
    if (hz < 100000000ull || hz > 10000000000ull) {
        kprintf("timer  : implausible TSC frequency %llu Hz\n", hz);
        panic("timer: cannot determine TSC frequency");
    }
    g_tsc_hz = hz;
    g_tsc_per_tick = hz / 100ull;
    if (g_tsc_per_tick == 0)
        g_tsc_per_tick = 1;

    if (g_cpu_features.tsc_deadline) {
        g_deadline_mode = 1;
        lapic_write(LAPIC_REG_LVT_TIMER, TIMER_VECTOR | (2u << 17));
        write_msr(MSR_TSC_DEADLINE, arch_read_tsc() + g_tsc_per_tick);
    } else {
        u64 lapic_hz = lapic_hz_from_tsc();
        u64 count;

        if (lapic_hz == 0)
            panic("timer: cannot calibrate LAPIC timer");
        count = lapic_hz / 100ull;
        if (count == 0)
            count = 1;
        if (count > 0xffffffffull)
            count = 0xffffffffull;
        lapic_write(LAPIC_REG_TIMER_DIV, 0x3u);
        lapic_write(LAPIC_REG_LVT_TIMER, TIMER_VECTOR | (1u << 17));
        lapic_write(LAPIC_REG_TIMER_INIT, (u32)count);
    }

    kprintf("timer  : %llu Hz (%s), mode=%s, 100 Hz tick\n",
            g_tsc_hz, g_source ? g_source : "unknown",
            g_deadline_mode ? "tsc-deadline" : "lapic-periodic");
    return 1;
}

u64 timer_tsc_hz(void) { return g_tsc_hz; }
const char *timer_source(void) { return g_source ? g_source : "unknown"; }
u64 timer_ticks(void) { return g_ticks; }

u64 timer_tsc_to_us(u64 delta)
{
    if (g_tsc_hz == 0)
        return 0;
    return (delta / g_tsc_hz) * 1000000ull +
           ((delta % g_tsc_hz) * 1000000ull) / g_tsc_hz;
}

u64 timer_monotonic_ns(void)
{
    u64 delta;

    if (g_tsc_hz == 0)
        return 0;
    delta = arch_read_tsc() - g_boot_tsc;
    return (delta / g_tsc_hz) * 1000000000ull +
           ((delta % g_tsc_hz) * 1000000000ull) / g_tsc_hz;
}

void timer_busy_wait_us(u64 us)
{
    u64 target;

    if (g_tsc_hz == 0) {
        volatile u64 i;

        for (i = 0; i < us * 1000ull; i++)
            ;
        return;
    }
    target = arch_read_tsc() + (g_tsc_hz / 1000000ull) * us;
    while (arch_read_tsc() < target)
        __asm__ volatile("pause");
}

void timer_on_irq(trap_frame *frame)
{
    (void)frame;
    g_ticks++;
    if (g_deadline_mode)
        write_msr(MSR_TSC_DEADLINE, arch_read_tsc() + g_tsc_per_tick);
    lapic_eoi();
    sched_tick();
}

int timer_selftest(void)
{
    u64 target = g_ticks + 2;
    u64 timeout = arch_read_tsc() + g_tsc_hz / 5ull;

    while (g_ticks < target) {
        if (arch_read_tsc() > timeout)
            return 0;
        __asm__ volatile("sti; hlt");
    }
    return 1;
}
