#ifndef CRUG_CPU_H
#define CRUG_CPU_H

#include <crug/types.h>

typedef struct cpu_features {
    char vendor[13];
    u32 max_basic_leaf;
    u32 max_ext_leaf;
    u32 family;
    u32 model;
    u32 stepping;
    int nx;
    int apic;
    int tsc_deadline;
    int smep;
    int smap;
    int xsave;
    int osxsave;
    int avx;
    int avx2;
    u32 xsave_size;
} cpu_features;

extern cpu_features g_cpu_features;

struct scheduler;

struct cpu {
    u64 self;
    u32 id;
    u32 apic_id;
    u64 tsc_hz;
    u64 ticks;
    struct scheduler *sched;
};

/* Save RFLAGS and disable interrupts; restore only the IF state. */
#ifdef CRUG_HOST_TEST
/* Host unit tests run in user mode, where CLI/STI and PUSHFQ are privileged.
 * Report interrupts as enabled (IF set) and make the toggles no-ops so the
 * kernel modules that guard their state still link and run unchanged. */
static inline u64 cpu_irq_save(void)
{
    return (1ull << 9);
}

static inline void cpu_irq_restore(u64 flags)
{
    (void)flags;
}

static inline void cpu_irq_disable(void)
{
}
#else
static inline u64 cpu_irq_save(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void cpu_irq_restore(u64 flags)
{
    if (flags & (1ull << 9))
        __asm__ volatile("sti" : : : "memory");
}

static inline void cpu_irq_disable(void)
{
    __asm__ volatile("cli" : : : "memory");
}
#endif

/* Halt until the next interrupt, preserving the current IF state. Waiting on
 * input with a spin loop pins a whole host core under emulation, so idle
 * paths use HLT and wake on the next timer tick. */
#ifdef CRUG_HOST_TEST
/* Host unit tests cannot execute HLT, so the idle wait becomes a stub that
 * moves the fake clock forward (see tests/host/test_gfx.c). */
void host_cpu_wait_for_irq(void);
static inline void cpu_wait_for_irq(void)
{
    host_cpu_wait_for_irq();
}
#else
static inline void cpu_wait_for_irq(void)
{
    u64 flags = cpu_irq_save();

    __asm__ volatile("sti; hlt; cli" : : : "memory");
    cpu_irq_restore(flags);
}
#endif

struct cpu *cpu_self(void);
void cpuid(u32 leaf, u32 subleaf, u32 *a, u32 *b, u32 *c, u32 *d);
u64 read_msr(u32 msr);
void write_msr(u32 msr, u64 value);
u64 read_cr0(void);
void write_cr0(u64 value);
u64 read_cr2(void);
u64 read_cr3(void);
void write_cr3(u64 value);
u64 read_cr4(void);
void write_cr4(u64 value);
void cpu_early_init(void);
void cpu_late_init(void);
void cpu_halt_forever(void);

#endif