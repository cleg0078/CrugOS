#include <crug/trap.h>
#include <crug/kernel.h>
#include <crug/cpu.h>
#include <crug/timer.h>
#include <crug/syscall.h>
#include <crug/thread.h>

typedef struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 type;
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
} __attribute__((packed)) idt_entry;

typedef struct idt_pointer {
    u16 limit;
    u64 base;
} __attribute__((packed)) idt_pointer;

extern u64 isr_stub_table[33];
extern void isr128(void);

static idt_entry g_idt[256] __attribute__((aligned(16)));
static idt_pointer g_idtr;
static volatile int g_int3_seen;

static const char *const exception_names[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow",
    "bound range", "invalid opcode", "device not available",
    "double fault", "coprocessor segment overrun", "invalid TSS",
    "segment not present", "stack-segment fault", "general protection",
    "page fault", "reserved", "x87 FPU error", "alignment check",
    "machine check", "SIMD FPU error", "virtualization",
    "control protection", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "hypervisor injection",
    "VMM communication", "security", "reserved"
};

static void idt_set(int vector, u64 handler, u8 ist, u8 dpl)
{
    idt_entry *e = &g_idt[vector];

    e->offset_low = (u16)(handler & 0xffff);
    e->selector = 0x08;
    e->ist = ist & 0x7;
    e->type = (u8)(0x8e | (dpl << 5));
    e->offset_mid = (u16)((handler >> 16) & 0xffff);
    e->offset_high = (u32)(handler >> 32);
    e->zero = 0;
}

void idt_init(void)
{
    int i;

    for (i = 0; i < 256; i++) {
        g_idt[i].offset_low = 0;
        g_idt[i].selector = 0;
        g_idt[i].ist = 0;
        g_idt[i].type = 0;
        g_idt[i].offset_mid = 0;
        g_idt[i].offset_high = 0;
        g_idt[i].zero = 0;
    }

    for (i = 0; i <= 32; i++) {
        u8 dpl = (i == 3 || i == 4) ? 3 : 0;
        u8 ist = (u8)trap_ist_for((u64)i);

        idt_set(i, isr_stub_table[i], ist, dpl);
    }
    idt_set(SYSCALL_VECTOR, (u64)(uintptr)isr128, 0, 3);

    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base = (u64)(uintptr)&g_idt[0];
    __asm__ volatile("lidt %0" : : "m"(g_idtr) : "memory");
}

static const char *trap_vector_name(u64 vector)
{
    return vector < 32 ? exception_names[vector] : "interrupt";
}

/* Phase H3: every fault writes the crash report before the policy decides
   whether the kernel survives it.  A ring-3 fault is charged to the offending
   program (phase U1); every ring-0 fault still panics. */
static void trap_report(trap_frame *frame)
{
    struct trap_report_info info;
    char buf[1024];

    info.vector = frame->vector;
    info.error = frame->error;
    info.ist = trap_ist_for(frame->vector);
    info.has_cr2 = frame->vector == 14;
    info.cr2 = info.has_cr2 ? read_cr2() : 0;
    info.name = trap_vector_name(frame->vector);
    if (trap_format(frame, &info, buf, sizeof(buf)) > 0)
        console_write(buf);
}

void trap_dispatch(trap_frame *frame)
{
    u64 v = frame->vector;
    enum trap_class cls;

    if (v == 32) {
        timer_on_irq(frame);
        return;
    }
    if (v == SYSCALL_VECTOR) {
        syscall_dispatch(frame);
        return;
    }
    if (v == 3) {
        g_int3_seen = 1;
        kprintf("trap   : breakpoint handled at rip=%p\n", (void *)(uintptr)frame->rip);
        return;
    }

    trap_report(frame);
    cls = trap_classify(v, frame->cs);
    if (cls == TRAP_USER && v != 2 && v != 8 && v != 18) {
        thread *t = sched_current();
        i64 code = -(i64)v;

        kprintf("CRUGOS: user fault %s at rip=%p err=%p tid %u\n",
                trap_vector_name(v), (void *)(uintptr)frame->rip,
                (void *)(uintptr)frame->error, (unsigned)(t ? t->id : 0));
        kprintf("CRUGOS: user fault killed tid %u code %lld\n",
                (unsigned)(t ? t->id : 0), (long long)code);
        /* Only the offending program dies: the kernel keeps running. */
        thread_kill_current(code);
    }
    panic("%s", trap_vector_name(v));
}

int trap_selftest_int3(void)
{
    g_int3_seen = 0;
    __asm__ volatile("int $3");
    return g_int3_seen;
}
