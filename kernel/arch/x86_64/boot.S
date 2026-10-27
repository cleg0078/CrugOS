/* CrugOS x86_64 kernel entry point.
 * The loader passes boot_info in %rdi (SysV ABI). Interrupts are disabled.
 */
    .section .text
    .global _start
    .type _start, @function
_start:
    cli
    cld

    lea     stack_top(%rip), %rsp
    xor     %rbp, %rbp

    mov     %rdi, %r12

    lea     __bss_start(%rip), %rdi
    lea     __bss_end(%rip), %rcx
    sub     %rdi, %rcx
    xor     %eax, %eax
    rep stosb

    mov     %r12, %rdi
    call    kernel_main

1:
    cli
    hlt
    jmp     1b
    .size _start, . - _start

    .section .bss
    .align 16
stack_bottom:
    .skip 32768
stack_top:
