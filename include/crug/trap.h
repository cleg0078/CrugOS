#ifndef CRUG_TRAP_H
#define CRUG_TRAP_H

#include <crug/bootinfo.h>
#include <crug/types.h>

/* Must match the push order in kernel/arch/x86_64/isr.S. */
typedef struct trap_frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error;
    u64 rip, cs, rflags, rsp, ss;
} trap_frame;

/* IST slots wired into the TSS by kernel/arch/x86_64/cpu.c.  Vector 0 means
   the exception runs on the interrupted stack (the normal kernel stack). */
#define TRAP_IST_DF 1u
#define TRAP_IST_NMI 2u
#define TRAP_IST_MC 3u
#define TRAP_IST_DB 4u

/* What the H3 policy does with a fault.  HANDLED returns from the handler,
   USER is a ring-3 fault that only wins against the offending process (it
   still panics today because no ring-3 thread exists yet) and FATAL always
   panics the kernel.  See docs/traps.md. */
enum trap_class {
    TRAP_HANDLED = 0,
    TRAP_USER = 1,
    TRAP_FATAL = 2
};

/* Everything trap_format() needs beyond the preserved frame itself. */
struct trap_report_info {
    u64 vector;
    u64 error;
    u32 ist;
    u64 cr2;
    int has_cr2;
    const char *name;
};

void idt_init(void);
void trap_dispatch(trap_frame *frame);
int trap_selftest_int3(void);

/* Pure policy and formatting helpers (kernel/lib/trapfmt.c, host-tested). */
enum trap_class trap_classify(u64 vector, u64 cs);
u32 trap_ist_for(u64 vector);
/* Render the multi-line crash report into out, always NUL-terminating when
   cap > 0.  Returns the number of characters stored (0 for cap == 0 or a
   NULL out) and truncates instead of overflowing. */
usize trap_format(const trap_frame *frame, const struct trap_report_info *info,
                  char *out, usize cap);

/* Boot token trap.test=<recipe> (phase H3): de, ud, gp, pf, df, nmi, mc, db. */
void trap_cmdline(const boot_info *bi);
void trap_test_run(void);

#endif
