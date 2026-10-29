/* Minimal freestanding syscall library for CrugOS ring-3 programs (U1..U5). */
#ifndef CRUG_USER_LIB_H
#define CRUG_USER_LIB_H

#include <crug/types.h>

#define U_SYS_GETTID 1u
#define U_SYS_YIELD 2u
#define U_SYS_EXIT 8u
#define U_SYS_WRITE 16u
#define U_SYS_OPEN 17u
#define U_SYS_READ 18u
#define U_SYS_CLOSE 19u
#define U_SYS_LISTDIR 20u
#define U_SYS_SPAWN 21u
#define U_SYS_WAIT 22u
#define U_SYS_EXEC 23u
#define U_SYS_FORK 24u

/* SYS_OPEN flags (phase U3). U_O_RDONLY is the zero flag. */
#define U_O_RDONLY 0u
#define U_O_WRONLY 1u
#define U_O_CREAT 2u
#define U_O_TRUNC 4u

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

static inline i64 u_putnum(u64 v)
{
    char digits21[21];
    char out[21];
    u32 n = 0;
    u32 i;

    if (v == 0)
        return u_puts("0");
    while (v != 0 && (u64)n < sizeof(digits21)) {
        digits21[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (i = 0; i < n; i++)
        out[i] = digits21[n - 1u - i];
    out[n] = 0;
    return u_puts(out);
}

static inline i64 u_puthex(u64 v)
{
    char digits[16];
    char out[17];
    u32 n = 0;
    u32 i;

    if (v == 0)
        return u_puts("0");
    while (v != 0 && (u64)n < sizeof(digits)) {
        u32 d = (u32)(v & 0xfu);

        digits[n++] = (char)(d < 10u ? ('0' + d) : ('a' + d - 10u));
        v >>= 4;
    }
    for (i = 0; i < n; i++)
        out[i] = digits[n - 1u - i];
    out[n] = 0;
    return u_puts(out);
}

static inline i64 u_write(u32 fd, const void *buf, u64 len)
{
    return u_syscall3(U_SYS_WRITE, (u64)fd, (u64)(uintptr)buf, len);
}

static inline i64 u_open(const char *path, u32 flags, u32 mode)
{
    return u_syscall3(U_SYS_OPEN, (u64)(uintptr)path, (u64)flags, (u64)mode);
}

static inline i64 u_read(u32 fd, void *buf, u32 len)
{
    return u_syscall3(U_SYS_READ, (u64)fd, (u64)(uintptr)buf, (u64)len);
}

static inline i64 u_close(u32 fd)
{
    return u_syscall3(U_SYS_CLOSE, (u64)fd, 0, 0);
}

static inline i64 u_listdir(const char *path, void *buf, u32 len)
{
    return u_syscall3(U_SYS_LISTDIR, (u64)(uintptr)path, (u64)(uintptr)buf,
                      (u64)len);
}

static inline int u_strcmp(const char *a, const char *b)
{
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

static inline i64 u_atoi(const char *s)
{
    i64 v = 0;
    int neg = 0;

    if (*s == (char)45) {
        neg = 1;
        s++;
    }
    while (*s >= (char)48 && *s <= (char)57) {
        v = v * 10 + (i64)(*s - (char)48);
        s++;
    }
    return neg ? -v : v;
}

static inline i64 u_spawn(const char *path, u32 argc, const char *const *argv)
{
    return u_syscall3(U_SYS_SPAWN, (u64)(uintptr)path, (u64)argc,
                      (u64)(uintptr)argv);
}

static inline i64 u_wait(u32 tid)
{
    return u_syscall3(U_SYS_WAIT, (u64)tid, 0, 0);
}

/* Replaces the running image; only returns when the exec failed. */
static inline i64 u_exec(const char *path, u32 argc, const char *const *argv)
{
    return u_syscall3(U_SYS_EXEC, (u64)(uintptr)path, (u64)argc,
                      (u64)(uintptr)argv);
}

/* Doubles the calling program: the child continues at the same instruction in
   its own copy of the address space and sees 0 here, the parent sees the tid. */
static inline i64 u_fork(void)
{
    return u_syscall3(U_SYS_FORK, 0, 0, 0);
}

static inline i64 u_gettid(void)
{
    return u_syscall3(U_SYS_GETTID, 0, 0, 0);
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