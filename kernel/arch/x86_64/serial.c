/* 16550 UART console on COM1. */
#include <crug/types.h>
#include <crug/io.h>
#include <crug/kernel.h>

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00); /* disable interrupts */
    outb(COM1 + 3, 0x80); /* DLAB on */
    outb(COM1 + 0, 0x01); /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7); /* FIFO enable, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B); /* IRQs off, RTS/DSR set */
}

static int serial_tx_ready(void)
{
    return (inb(COM1 + 5) & 0x20) != 0;
}

void serial_putc(char c)
{
    while (!serial_tx_ready()) {
    }
    outb(COM1, (u8)c);
}

void serial_write(const char *s)
{
    while (*s) {
        serial_putc(*s++);
    }
}
