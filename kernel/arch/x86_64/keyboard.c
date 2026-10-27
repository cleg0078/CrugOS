/* i8042 PS/2 keyboard: translated set-1 scancodes, polled. */
#include <crug/types.h>
#include <crug/io.h>
#include <crug/input.h>

#define I8042_DATA 0x60
#define I8042_STATUS 0x64
#define I8042_CMD 0x64
#define I8042_STATUS_OUTPUT 0x01
#define I8042_STATUS_INPUT 0x02
#define I8042_STATUS_AUX 0x20
#define I8042_CONFIG_TRANSLATE 0x40
#define I8042_CONFIG_CLOCK_OFF 0x10

#define I8042_SPIN 100000u

static int g_present;

static int input_clear(void)
{
    u32 i;

    for (i = 0; i < I8042_SPIN; i++) {
        if ((inb(I8042_STATUS) & I8042_STATUS_INPUT) == 0)
            return 0;
    }
    return -1;
}

static int output_full(void)
{
    u32 i;

    for (i = 0; i < I8042_SPIN; i++) {
        if (inb(I8042_STATUS) & I8042_STATUS_OUTPUT)
            return 0;
    }
    return -1;
}

static void drain_output(void)
{
    u32 i;

    for (i = 0; i < 32u; i++) {
        if ((inb(I8042_STATUS) & I8042_STATUS_OUTPUT) == 0)
            return;
        (void)inb(I8042_DATA);
    }
}

int keyboard_init(void)
{
    u8 cfg;

    g_present = 0;
    if (inb(I8042_STATUS) == 0xFFu)
        return -1; /* no controller on this machine */

    /* Known state: translation on (set 1 to the guest), interrupts off
       (we poll), keyboard clock enabled. */
    outb(I8042_CMD, 0xAD); /* disable first PS/2 port */
    if (input_clear() != 0)
        return -1;
    drain_output();

    outb(I8042_CMD, 0x20); /* read configuration byte */
    if (output_full() != 0)
        return -1;
    cfg = inb(I8042_DATA);
    cfg |= I8042_CONFIG_TRANSLATE;
    cfg &= (u8)~0x03u; /* no IRQ1/IRQ12: polled input */
    cfg &= (u8)~I8042_CONFIG_CLOCK_OFF;

    if (input_clear() != 0)
        return -1;
    outb(I8042_CMD, 0x60); /* write configuration byte */
    if (input_clear() != 0)
        return -1;
    outb(I8042_DATA, cfg);

    outb(I8042_CMD, 0xAE); /* enable first PS/2 port */
    if (input_clear() != 0)
        return -1;
    drain_output();

    /* Enable scanning (0xF4) and swallow the ACK: input_scancode ignores
       0xFA, and a missing keyboard simply leaves the queue empty. */
    outb(I8042_DATA, 0xF4);
    if (output_full() == 0)
        (void)inb(I8042_DATA);

    g_present = 1;
    return 0;
}

void keyboard_poll(void)
{
    u32 i;

    if (!g_present)
        return;
    for (i = 0; i < 64u; i++) {
        u8 status = inb(I8042_STATUS);

        if ((status & I8042_STATUS_OUTPUT) == 0)
            return;
        if (status & I8042_STATUS_AUX) {
            (void)inb(I8042_DATA); /* mouse byte: discard */
            continue;
        }
        input_scancode(inb(I8042_DATA));
    }
}
