/* Kernel-wide interfaces. */
#ifndef CRUG_KERNEL_H
#define CRUG_KERNEL_H

#include <crug/types.h>
#include <crug/compiler.h>
#include <crug/bootinfo.h>

void kernel_main(const boot_info *bi) CRUG_NORETURN;

void arch_early_init(void);
u64 arch_read_tsc(void);
u32 arch_cpu_id(void);
void arch_halt(void) CRUG_NORETURN;
void arch_exit_qemu(u8 code) CRUG_NORETURN;

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

void console_init(void);
void console_putc(char c);
void console_write(const char *s);

void fb_init(const framebuffer_info *fb);
int fb_ready(void);
void fb_putc(char c);

void debugcon_putc(char c);
void trace(const char *s);

int kvsnprintf(char *buf, usize size, const char *fmt, va_list ap);
int ksnprintf(char *buf, usize size, const char *fmt, ...) CRUG_PRINTF(3, 4);
void kprintf(const char *fmt, ...) CRUG_PRINTF(1, 2);

void panic(const char *fmt, ...) CRUG_NORETURN CRUG_PRINTF(1, 2);

void bootinfo_validate(const boot_info *bi);
u64 bootinfo_usable_bytes(const boot_info *bi);
void report_boot(const boot_info *bi, u64 tsc_delta);

#endif /* CRUG_KERNEL_H */
