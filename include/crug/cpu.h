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

struct cpu {
    u64 self;
    u32 id;
    u32 apic_id;
    u64 tsc_hz;
    u64 ticks;
};

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
