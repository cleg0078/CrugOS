/* Fatal error reporting and QEMU test exit. */
#include <crug/types.h>
#include <crug/kernel.h>

extern char __text_start[];
extern char __text_end[];

static void panic_stack_scan(void)
{
    u64 *sp;
    int i;

    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    kprintf("stack:\n");
    for (i = 0; i < 32; i++) {
        u64 value = sp[i];
        if (value >= (u64)(uintptr)__text_start && value < (u64)(uintptr)__text_end) {
            kprintf("  [%d] 0x%p\n", i, (void *)(uintptr)value);
        }
    }
}

void panic(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    console_write("\n*** CRUGOS PANIC: ");
    console_write(buf);
    console_write(" ***\n");
    panic_stack_scan();
    console_write("CRUGOS: panic\n");
    arch_exit_qemu(0x11);
}
