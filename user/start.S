/* CrugOS ring-3 program entry (phase U1).
 *
 * The kernel enters at _start with a 16-byte aligned stack and no argv; main
 * returns the program's exit code, which becomes SYS_EXIT's argument.
 */

    .section .text.start, "ax"
    .code64
    .globl _start
    .type _start, @function
_start:
    /* No cli/hlt here: those are privileged and would fault at CPL 3. */
    xorl %ebp, %ebp
    andq $-16, %rsp
    call main
    movq %rax, %rdi
    movq $8, %rax            /* SYS_EXIT */
    int $0x80
1:
    pause
    jmp 1b
    .size _start, . - _start
