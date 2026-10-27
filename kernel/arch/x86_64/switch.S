/* CrugOS x86_64 context switch.
 * void cpu_switch(u64 *prev_rsp, u64 *next_rsp);
 * Saves callee-saved registers and RFLAGS on the current stack, stores RSP
 * through prev_rsp, loads RSP from next_rsp and returns into the new thread.
 */

    .text
    .global cpu_switch
    .type cpu_switch, @function
cpu_switch:
    pushfq
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq (%rsi), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popfq
    ret
    .size cpu_switch, . - cpu_switch
