/* Minimal freestanding syscall library for CrugOS ring-3 programs (U1). */
#ifndef CRUG_USER_LIB_H
#define CRUG_USER_LIB_H

#include <crug/types.h>

#define U_SYS_EXIT 8u
#define U_SYS_WRITE 16u

/* Kernel ABI: nr in rax, arguments in rdi, rsi, rdx, then r10. */
static inline i64 u_syscall3(u64 nr, u64 a, u64 b, u64 c)
{
    i64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline i64 u_syscall4(u64 nr, u64 a, u64 b, u64 c, u64 d)
{
    i64 ret;
    register u64 r10 __asm__("r10") = d;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline u64 u_strlen(const char *s)
{
    u64 n = 0;

    while (s[n] != 0)
        n++;
    return n;
}

static inline i64 u_puts(const char *s)
{
    return u_syscall3(U_SYS_WRITE, 1u, (u64)(uintptr)s, u_strlen(s));
}

static inline i64 u_write_bad_ptr(void)
{
    /* A kernel address: the syscall layer must refuse it with -EFAULT. */
    return u_syscall3(U_SYS_WRITE, 1u, 0x00100000ull, 16u);
}

static inline void u_exit(int code)
{
    u_syscall3(U_SYS_EXIT, (u64)(i64)code, 0, 0);
    /* SYS_EXIT never returns; hlt is privileged, so idle with pause. */
    for (;;)
        __asm__ volatile("pause");
}

#endif /* CRUG_USER_LIB_H */
