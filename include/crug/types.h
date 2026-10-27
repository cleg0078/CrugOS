/* CrugOS basic integer types and helpers. */
#ifndef CRUG_TYPES_H
#define CRUG_TYPES_H

#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>

typedef unsigned char u8;
typedef signed char i8;
typedef unsigned short u16;
typedef signed short i16;
typedef unsigned int u32;
typedef signed int i32;
typedef unsigned long long u64;
typedef signed long long i64;

typedef u64 usize;
typedef i64 isize;
typedef u64 uintptr;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN_UP(x, a) (((x) + ((u64)(a) - 1)) & ~((u64)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define CRUG_UNUSED(x) ((void)(x))

#endif /* CRUG_TYPES_H */
