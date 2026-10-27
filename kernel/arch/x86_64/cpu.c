#include <crug/cpu.h>
#include <crug/kernel.h>
#include <crug/string.h>
#include <crug/apic.h>
#include <crug/trap.h>

#ifndef CRUG_REQUIRE_AVX2
#define CRUG_REQUIRE_AVX2 1
#endif

#define MSR_GS_BASE 0xc0000101u
#define CR0_MP (1ull << 1)
#define CR0_EM (1ull << 2)
#define CR4_OSFXSR (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)
#define CR4_PGE (1ull << 7)
#define CR4_OSXSAVE (1ull << 18)
#define CR4_SMEP (1ull << 20)
#define CR4_SMAP (1ull << 21)

typedef struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8 base_mid;
    u8 access;
    u8 granularity;
    u8 base_high;
} __attribute__((packed)) gdt_entry;

typedef struct gdt_pointer {
    u16 limit;
    u64 base;
} __attribute__((packed)) gdt_pointer;

typedef struct tss64 {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} __attribute__((packed)) tss64;

extern char __stack_top[];
extern char __df_stack_top[];

cpu_features g_cpu_features;

static gdt_entry g_gdt[5];
static tss64 g_tss;
static gdt_pointer g_gdtr;
static struct cpu g_boot_cpu;

void cpuid(u32 leaf, u32 subleaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

u64 read_msr(u32 msr)
{
    u32 lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

void write_msr(u32 msr, u64 value)
{
    u32 lo = (u32)value;
    u32 hi = (u32)(value >> 32);

    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

u64 read_cr0(void) { u64 v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
void write_cr0(u64 v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
u64 read_cr2(void) { u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
u64 read_cr3(void) { u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
void write_cr3(u64 v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
u64 read_cr4(void) { u64 v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
void write_cr4(u64 v) { __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }

static u64 xgetbv(u32 index)
{
    u32 lo, hi;

    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(index));
    return ((u64)hi << 32) | lo;
}

static void xsetbv(u32 index, u64 value)
{
    __asm__ volatile("xsetbv"
                     :
                     : "c"(index), "a"((u32)value), "d"((u32)(value >> 32)));
}

static void cpu_detect(void)
{
    u32 a, b, c, d;
    u32 max_basic, max_ext;
    u32 family, model;

    memset(&g_cpu_features, 0, sizeof(g_cpu_features));
    cpuid(0, 0, &max_basic, &b, &c, &d);
    g_cpu_features.max_basic_leaf = max_basic;

    g_cpu_features.vendor[0] = (char)(b & 0xff);
    g_cpu_features.vendor[1] = (char)((b >> 8) & 0xff);
    g_cpu_features.vendor[2] = (char)((b >> 16) & 0xff);
    g_cpu_features.vendor[3] = (char)((b >> 24) & 0xff);
    g_cpu_features.vendor[4] = (char)(d & 0xff);
    g_cpu_features.vendor[5] = (char)((d >> 8) & 0xff);
    g_cpu_features.vendor[6] = (char)((d >> 16) & 0xff);
    g_cpu_features.vendor[7] = (char)((d >> 24) & 0xff);
    g_cpu_features.vendor[8] = (char)(c & 0xff);
    g_cpu_features.vendor[9] = (char)((c >> 8) & 0xff);
    g_cpu_features.vendor[10] = (char)((c >> 16) & 0xff);
    g_cpu_features.vendor[11] = (char)((c >> 24) & 0xff);
    g_cpu_features.vendor[12] = 0;

    cpuid(1, 0, &a, &b, &c, &d);
    family = (a >> 8) & 0xf;
    model = (a >> 4) & 0xf;
    if (family == 0xf)
        family += (a >> 20) & 0xff;
    if (family == 0x6 || family == 0xf)
        model += ((a >> 16) & 0xf) << 4;
    g_cpu_features.family = family;
    g_cpu_features.model = model;
    g_cpu_features.stepping = a & 0xf;
    g_cpu_features.apic = (d >> 9) & 1;
    g_cpu_features.xsave = (c >> 26) & 1;
    g_cpu_features.osxsave = (c >> 27) & 1;
    g_cpu_features.avx = (c >> 28) & 1;
    g_cpu_features.tsc_deadline = (c >> 24) & 1;

    if (max_basic >= 7) {
        u32 a7, b7, c7, d7;

        cpuid(7, 0, &a7, &b7, &c7, &d7);
        g_cpu_features.avx2 = (b7 >> 5) & 1;
        g_cpu_features.smep = (b7 >> 7) & 1;
        g_cpu_features.smap = (b7 >> 20) & 1;
    }

    cpuid(0x80000000u, 0, &max_ext, &b, &c, &d);
    g_cpu_features.max_ext_leaf = max_ext;
    if (max_ext >= 0x80000001u) {
        cpuid(0x80000001u, 0, &a, &b, &c, &d);
        g_cpu_features.nx = (d >> 20) & 1;
    }

    /* SSE state must be live before any AES-NI/SSE-encoded crypto code runs.
     * UEFI firmware normally leaves these set, but ExitBootServices hands over an
     * unspecified control-register state, and AESENC faults with #UD when
     * CR4.OSFXSR is clear. */
    {
        u32 mxcsr = 0x1f80u;

        write_cr0((read_cr0() | CR0_MP) & ~CR0_EM);
        write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
        __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    }

    if (g_cpu_features.xsave && g_cpu_features.avx) {
        u64 xcr0;

        write_cr4(read_cr4() | CR4_OSXSAVE);
        cpuid(0xd, 0, &a, &b, &c, &d);
        g_cpu_features.xsave_size = b;
        xcr0 = xgetbv(0);
        if ((a & 0x7) == 0x7 && (xcr0 & 0x7) != 0x7) {
            xsetbv(0, 0x7);
            g_cpu_features.osxsave = 1;
        }
    }
}

static void gdt_set(int i, u32 base, u32 limit, u8 access, u8 gran)
{
    g_gdt[i].limit_low = (u16)(limit & 0xffff);
    g_gdt[i].base_low = (u16)(base & 0xffff);
    g_gdt[i].base_mid = (u8)((base >> 16) & 0xff);
    g_gdt[i].access = access;
    g_gdt[i].granularity = (u8)(((limit >> 16) & 0x0f) | (gran & 0xf0));
    g_gdt[i].base_high = (u8)((base >> 24) & 0xff);
}

static void gdt_init(void)
{
    u64 tss_base = (u64)(uintptr)&g_tss;
    u32 tss_limit = sizeof(g_tss) - 1;
    u16 tss_sel = 0x18;

    memset(g_gdt, 0, sizeof(g_gdt));
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.rsp0 = (u64)(uintptr)__stack_top;
    g_tss.ist1 = (u64)(uintptr)__df_stack_top;
    g_tss.iomap_base = sizeof(g_tss);

    gdt_set(1, 0, 0xfffff, 0x9a, 0xa0);
    gdt_set(2, 0, 0xfffff, 0x92, 0xc0);
    gdt_set(3, (u32)tss_base, tss_limit, 0x89, 0x00);
    gdt_set(4, (u32)(tss_base >> 32), 0, 0x00, 0x00);

    g_gdtr.limit = sizeof(g_gdt) - 1;
    g_gdtr.base = (u64)(uintptr)&g_gdt[0];
    __asm__ volatile("lgdt %0" : : "m"(g_gdtr) : "memory");
    __asm__ volatile(
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        : : : "rax", "memory");
    __asm__ volatile("ltr %0" : : "r"(tss_sel));
}

struct cpu *cpu_self(void)
{
    u64 p;

    __asm__ volatile("movq %%gs:0, %0" : "=r"(p));
    return (struct cpu *)(uintptr)p;
}

void cpu_early_init(void)
{
    cpu_detect();

    if (!g_cpu_features.nx)
        panic("cpu: NX (no-execute) is required");

    if (CRUG_REQUIRE_AVX2 && !g_cpu_features.avx2) {
        kprintf("cpu    : avx2=no required=yes\n");
        panic("AVX2 is required but unavailable");
    }

    memset(&g_boot_cpu, 0, sizeof(g_boot_cpu));
    g_boot_cpu.self = (u64)(uintptr)&g_boot_cpu;
    g_boot_cpu.id = 0;
    g_boot_cpu.apic_id = 0;

    gdt_init();
    write_msr(MSR_GS_BASE, (u64)(uintptr)&g_boot_cpu);

    kprintf("cpu    : %s family %u model %u stepping %u nx=%s apic=%s avx2=%s required=%s\n",
            g_cpu_features.vendor, g_cpu_features.family, g_cpu_features.model,
            g_cpu_features.stepping,
            g_cpu_features.nx ? "yes" : "no",
            g_cpu_features.apic ? "yes" : "no",
            g_cpu_features.avx2 ? "yes" : "no",
            CRUG_REQUIRE_AVX2 ? "yes" : "no");
}

void cpu_late_init(void)
{
    idt_init();
    apic_init();
    pic_mask_all();

    if (g_cpu_features.smep)
        write_cr4(read_cr4() | CR4_SMEP);
    if (g_cpu_features.smap)
        write_cr4(read_cr4() | CR4_SMAP);

    kprintf("cpu    : smep=%s smap=%s tsc_deadline=%s\n",
            g_cpu_features.smep ? "yes" : "no",
            g_cpu_features.smap ? "yes" : "no",
            g_cpu_features.tsc_deadline ? "yes" : "no");
}

void cpu_halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}
