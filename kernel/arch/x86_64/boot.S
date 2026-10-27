/* CrugOS x86_64 kernel entry.
 * UEFI loader enters here with the BootInfo pointer in %rdi (SysV ABI).
 */

    .section .text
    .global _start
    .type _start, @function
_start:
    cli
    cld
    lea __stack_top(%rip), %rsp
    xor %rbp, %rbp

    /* Save BootInfo across the BSS clear. */
    mov %rdi, %r12

    /* Zero .bss, including the boot stacks. No calls before this point. */
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx
    xor %eax, %eax
    rep stosb

    mov %r12, %rdi
    call kernel_main

1:
    cli
    hlt
    jmp 1b
    .size _start, . - _start
