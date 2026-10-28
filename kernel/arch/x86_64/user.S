/* CrugOS ring-3 thread trampoline (phase U1).
 *
 * cpu_switch() drops us here on a fresh thread's kernel stack. The thread
 * record already carries the user rip/rsp, so build the iretq frame that
 * crosses the privilege boundary and fall into ring 3.
 */

    .text
    .code64
    .globl thread_user_start
    .type thread_user_start, @function
thread_user_start:
    /* Interrupts stay off until the iretq restores RFLAGS.IF. */
    cli
    subq $32, %rsp
    movq %rsp, %rdi
    call thread_user_frame         /* fills rip, rsp, ss, cs */
    movq 0(%rsp), %rcx             /* user rip */
    movq 8(%rsp), %rdx             /* user rsp */
    movq 16(%rsp), %rax            /* user ss */
    movq 24(%rsp), %rbx            /* user cs */
    addq $32, %rsp
    pushq %rax                     /* ss */
    pushq %rdx                     /* rsp */
    pushq $0x202                   /* rflags: IF=1, IOPL=0 */
    pushq %rbx                     /* cs */
    pushq %rcx                     /* rip */
    movw %ax, %ds
    movw %ax, %es
    iretq
    .size thread_user_start, . - thread_user_start
