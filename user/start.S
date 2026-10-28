/* CrugOS ring-3 program entry (phase U1; argv since U3).
 *
 * The kernel loads the initial stack image: argc at rsp, argv at rsp+8 and
 * envp right after the argv NULL terminator. Save those three in the SysV
 * argument registers, align the stack for a normal call, and run main.
 */

    .section .text.start, "ax"
    .code64
    .globl _start
    .type _start, @function
_start:
    /* No cli/hlt here: those are privileged and would fault at CPL 3. */
    xorl %ebp, %ebp
    movq (%rsp), %rdi          /* argc */
    leaq 8(%rsp), %rsi         /* argv */
    leaq 8(%rsi,%rdi,8), %rdx  /* envp */
    andq $-16, %rsp
    call main
    movq %rax, %rdi
    movq $8, %rax            /* SYS_EXIT */
    int $0x80
1:
    pause
    jmp 1b
    .size _start, . - _start
