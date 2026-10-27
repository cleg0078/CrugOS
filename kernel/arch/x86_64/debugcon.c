/* QEMU port 0xE9 early trace channel. */
#include <crug/io.h>
#include <crug/kernel.h>

#define QEMU_DEBUG_CONSOLE_PORT 0xE9

void debugcon_putc(char c)
{
    outb(QEMU_DEBUG_CONSOLE_PORT, (u8)c);
}

void trace(const char *s)
{
    while (*s) {
        debugcon_putc(*s++);
    }
    debugcon_putc('\n');
}
