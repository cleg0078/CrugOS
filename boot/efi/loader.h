/* Common loader runtime services (console, serial, panic exit). */
#ifndef CRUG_LOADER_H
#define CRUG_LOADER_H

#include <crug/types.h>
#include <crug/compiler.h>

void loader_serial_init(void);
void loader_serial_putc(char c);
void loader_serial_write(const char *s);
void loader_debugcon_putc(char c);

void loader_console_bind(void *con_out);
void loader_print(const char *s);
void loader_println(const char *s);
void loader_print_hex(u64 value);
void loader_fail(const char *msg) CRUG_NORETURN;
void loader_status_fail(const char *what, u64 status) CRUG_NORETURN;

u64 loader_read_tsc(void);
void loader_qemu_exit(u8 code) CRUG_NORETURN;

#endif /* CRUG_LOADER_H */
