/* COM1 serial output for the loader (direct port I/O, no firmware). */
#include <crug/types.h>
#include <crug/io.h>
#include "loader.h"

#define COM1 0x3F8

void loader_serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void loader_serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) {
    }
    outb(COM1, (u8)c);
}

void loader_serial_write(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            loader_serial_putc('\r');
        }
        loader_serial_putc(*s++);
    }
}

void loader_debugcon_putc(char c)
{
    outb(0xE9, (u8)c);
}

u64 loader_read_tsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

void loader_qemu_exit(u8 code)
{
    outb(0xF4, code);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
