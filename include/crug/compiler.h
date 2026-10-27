/* Compiler abstraction macros used across CrugOS. */
#ifndef CRUG_COMPILER_H
#define CRUG_COMPILER_H

#define CRUG_NORETURN __attribute__((noreturn))
#define CRUG_PACKED __attribute__((packed))
#define CRUG_ALIGNED(n) __attribute__((aligned(n)))
#define CRUG_SECTION(s) __attribute__((section(s)))
#define CRUG_USED __attribute__((used))
#define CRUG_WEAK __attribute__((weak))
#define CRUG_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#define CRUG_MSABI __attribute__((ms_abi))
#define CRUG_INLINE static inline

#define CRUG_BARRIER() __asm__ volatile("" ::: "memory")
#define CRUG_LIKELY(x) __builtin_expect(!!(x), 1)
#define CRUG_UNLIKELY(x) __builtin_expect(!!(x), 0)

#endif /* CRUG_COMPILER_H */
