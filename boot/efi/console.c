/* Loader console: serial plus the firmware ConOut device. */
#include <crug/types.h>
#include "uefi.h"
#include "loader.h"

static efi_simple_text_output *g_con_out;

void loader_console_bind(void *con_out)
{
    g_con_out = (efi_simple_text_output *)con_out;
}

void loader_print(const char *s)
{
    loader_serial_write(s);
    while (*s && g_con_out && g_con_out->output_string) {
        char16 buf[2];
        char c = *s++;
        buf[1] = 0;
        if (c == '\n') {
            buf[0] = '\r';
            g_con_out->output_string(g_con_out, buf);
            buf[0] = '\n';
            g_con_out->output_string(g_con_out, buf);
        } else {
            buf[0] = (char16)(unsigned char)c;
            g_con_out->output_string(g_con_out, buf);
        }
    }
}

void loader_println(const char *s)
{
    loader_print(s);
    loader_print("\n");
}

void loader_print_hex(u64 value)
{
    char buf[19];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        u32 nibble = (u32)((value >> (60 - 4 * i)) & 0xF);
        buf[2 + i] = (char)(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10));
    }
    buf[18] = 0;
    loader_print(buf);
}

void loader_fail(const char *msg)
{
    loader_print("\nCRUGOS loader: FAIL: ");
    loader_println(msg);
    loader_serial_write("CRUGOS loader: exiting with failure\n");
    loader_qemu_exit(0x11);
}

void loader_status_fail(const char *what, u64 status)
{
    loader_print("\nCRUGOS loader: FAIL: ");
    loader_print(what);
    loader_print(" (status ");
    loader_print_hex(status);
    loader_println(")");
    loader_qemu_exit(0x11);
}
