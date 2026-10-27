/* Formatted console output. */
#include <crug/types.h>
#include <crug/kernel.h>

void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_write(buf);
}
