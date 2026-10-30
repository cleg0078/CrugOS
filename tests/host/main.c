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
void test_net_functions(void);
void test_dns_functions(void);
void test_usb_functions(void);
void test_audio_functions(void);
void test_gfx_functions(void);
void test_intuition_functions(void);
void test_commodity_functions(void);
void test_textgrid_functions(void);
void test_shell_functions(void);
void test_harden_functions(void);
void test_heap_functions(void);
void test_trap_functions(void);
void test_argv_functions(void);
void test_ufile_functions(void);
void test_uaccess_functions(void);
void test_exec_functions(void);
void test_vm_space_functions(void);
void test_vm_space_oom(void);
void test_vm_space_clone(void);
void test_vm_space_cow(void);
void test_app_functions(void);
void test_pkg_functions(void);
void test_gpu_functions(void);
void test_color_functions(void);
void test_paint_functions(void);
void test_theme_functions(void);
void test_wallpaper_functions(void);

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
    RUN(test_net_functions);
    RUN(test_dns_functions);
    RUN(test_usb_functions);
    RUN(test_audio_functions);
    RUN(test_gfx_functions);
    RUN(test_intuition_functions);
    RUN(test_commodity_functions);
    RUN(test_textgrid_functions);
    RUN(test_shell_functions);
    RUN(test_harden_functions);
    RUN(test_heap_functions);
    RUN(test_trap_functions);
    RUN(test_argv_functions);
    RUN(test_ufile_functions);
    RUN(test_uaccess_functions);
    RUN(test_exec_functions);
    RUN(test_vm_space_functions);
    RUN(test_vm_space_oom);
    RUN(test_vm_space_clone);
    RUN(test_vm_space_cow);
    RUN(test_app_functions);
    RUN(test_pkg_functions);
    RUN(test_gpu_functions);
    RUN(test_color_functions);
    RUN(test_paint_functions);
    RUN(test_theme_functions);
    RUN(test_wallpaper_functions);
    printf("%d checks, %d failure(s)\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
