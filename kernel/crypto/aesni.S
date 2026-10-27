/* AES-NI single-block cores. The kernel never uses xmm elsewhere; these
   routines keep no state between calls. */
    .text

    .globl aesni_encrypt_block
    .type aesni_encrypt_block, @function
aesni_encrypt_block:
    movdqu (%rsi), %xmm0
    movdqu (%rdi), %xmm1
    pxor %xmm1, %xmm0
    movdqu 16(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 32(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 48(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 64(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 80(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 96(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 112(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 128(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 144(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 160(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 176(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 192(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 208(%rdi), %xmm1
    aesenc %xmm1, %xmm0
    movdqu 224(%rdi), %xmm1
    aesenclast %xmm1, %xmm0
    movdqu %xmm0, (%rdx)
    ret
    .size aesni_encrypt_block, .-aesni_encrypt_block

    .globl aesni_decrypt_block
    .type aesni_decrypt_block, @function
aesni_decrypt_block:
    movdqu (%rsi), %xmm0
    movdqu 224(%rdi), %xmm1
    pxor %xmm1, %xmm0
    movdqu 208(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 192(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 176(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 160(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 144(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 128(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 112(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 96(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 80(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 64(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 48(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 32(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu 16(%rdi), %xmm1
    aesimc %xmm1, %xmm1
    aesdec %xmm1, %xmm0
    movdqu (%rdi), %xmm1
    aesdeclast %xmm1, %xmm0
    movdqu %xmm0, (%rdx)
    ret
    .size aesni_decrypt_block, .-aesni_decrypt_block

    .section .note.GNU-stack,"",@progbits
