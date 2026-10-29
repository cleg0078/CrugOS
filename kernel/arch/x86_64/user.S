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

/* Phase U5: a forked child enters here from cpu_switch() on its own kernel
 * stack. thread_fork_resume() copies the parent's saved frame into place and
 * returns its address; the shared isr_return epilogue pops the general
 * registers, drops the vector/error pair and iretqs to ring 3 with rax == 0.
 */
    .globl thread_fork_enter
    .type thread_fork_enter, @function
thread_fork_enter:
    cli
    call thread_fork_resume
    movq %rax, %rsp
    jmp isr_return
    .size thread_fork_enter, . - thread_fork_enter
