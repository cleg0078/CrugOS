/* Kernel console: mirrored serial + framebuffer output. */
#include <crug/types.h>
#include <crug/kernel.h>
#include <crug/input.h>

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

/* Console input: serial first, then the polled PS/2 keyboard. */
int console_try_getc(void)
{
    int c = serial_try_getc();

    if (c >= 0) {
        return c;
    }
    keyboard_poll();
    return input_try_getc();
}
