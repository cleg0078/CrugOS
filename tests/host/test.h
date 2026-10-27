/* Tiny host-side test harness (no external dependencies). */
#ifndef CRUG_TEST_H
#define CRUG_TEST_H

#include <stdio.h>

extern int tests_run;
extern int tests_failed;

void test_fail(const char *file, int line, const char *what);

#define CHECK(cond) \
    do { \
        tests_run++; \
        if (!(cond)) { \
            test_fail(__FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_STR(actual, expected) \
    do { \
        const char *a_ = (actual); \
        const char *e_ = (expected); \
        tests_run++; \
        if (a_ == NULL || e_ == NULL || __builtin_strcmp(a_, e_) != 0) { \
            test_fail(__FILE__, __LINE__, e_); \
            fprintf(stderr, "  expected: %s\n  actual:   %s\n", \
                    e_ ? e_ : "(null)", a_ ? a_ : "(null)"); \
        } \
    } while (0)

#define RUN(fn) \
    do { \
        printf("- %s\n", #fn); \
        fn(); \
    } while (0)

#endif /* CRUG_TEST_H */
