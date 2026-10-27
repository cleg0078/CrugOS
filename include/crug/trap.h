#ifndef CRUG_TRAP_H
#define CRUG_TRAP_H

#include <crug/types.h>

/* Must match the push order in kernel/arch/x86_64/isr.S. */
typedef struct trap_frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error;
    u64 rip, cs, rflags, rsp, ss;
} trap_frame;

void idt_init(void);
void trap_dispatch(trap_frame *frame);
int trap_selftest_int3(void);

#endif
