#include <stdio.h>
#include "test.h"

int tests_run;
int tests_failed;

void test_fail(const char *file, int line, const char *what)
{
    tests_failed++;
    printf("  FAIL %s:%d: %s\n", file, line, what);
}

void test_string_functions(void);
void test_printf_functions(void);
void test_elf_loader(void);
void test_bitmap_functions(void);
void test_vm_math(void);
void test_handle_functions(void);
void test_crc32c_functions(void);
void test_blake2b_functions(void);
void test_aes_functions(void);
void test_argon2_functions(void);
void test_crypt_functions(void);
void test_lineedit_functions(void);
void test_input_functions(void);

int main(void)
{
    printf("CrugOS host unit tests\n");
    RUN(test_string_functions);
    RUN(test_printf_functions);
    RUN(test_elf_loader);
    RUN(test_bitmap_functions);
    RUN(test_vm_math);
    RUN(test_handle_functions);
    RUN(test_crc32c_functions);
    RUN(test_blake2b_functions);
    RUN(test_aes_functions);
    RUN(test_argon2_functions);
    RUN(test_crypt_functions);
    RUN(test_lineedit_functions);
    RUN(test_input_functions);
    printf("%d checks, %d failure(s)\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
