#include <crug/trap.h>
#include <crug/kernel.h>
#include <crug/cpu.h>
#include <crug/timer.h>

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
        u8 ist = (i == 8) ? 1 : 0;

        idt_set(i, isr_stub_table[i], ist, dpl);
    }

    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base = (u64)(uintptr)&g_idt[0];
    __asm__ volatile("lidt %0" : : "m"(g_idtr) : "memory");
}

static void page_fault_panic(trap_frame *f)
{
    kprintf("trap   : page fault rip=%p cr2=%p error=%p\n",
            (void *)(uintptr)f->rip, (void *)(uintptr)read_cr2(),
            (void *)(uintptr)f->error);
    panic("page fault");
}

void trap_dispatch(trap_frame *frame)
{
    u64 v = frame->vector;

    if (v == 32) {
        timer_on_irq(frame);
        return;
    }
    if (v == 3) {
        g_int3_seen = 1;
        kprintf("trap   : breakpoint handled at rip=%p\n", (void *)(uintptr)frame->rip);
        return;
    }
    if (v == 14)
        page_fault_panic(frame);

    kprintf("trap   : vector %llu (%s) error=%p rip=%p rsp=%p\n",
            v, v < 32 ? exception_names[v] : "interrupt",
            (void *)(uintptr)frame->error, (void *)(uintptr)frame->rip,
            (void *)(uintptr)frame->rsp);
    if (v == 8)
        panic("double fault");
    panic("unhandled CPU exception");
}

int trap_selftest_int3(void)
{
    g_int3_seen = 0;
    __asm__ volatile("int $3");
    return g_int3_seen;
}
