/* Kernel console: mirrored serial + framebuffer output. */
#include <crug/types.h>
#include <crug/kernel.h>

static int g_console_ready;

void console_init(void)
{
    g_console_ready = 1;
}

void console_putc(char c)
{
    if (!g_console_ready) {
        serial_putc(c);
        return;
    }
    if (c == '\n') {
        serial_putc('\r');
    }
    serial_putc(c);
    fb_putc(c);
}

void console_write(const char *s)
{
    while (*s) {
        console_putc(*s++);
    }
}
