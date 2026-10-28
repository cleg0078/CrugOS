/* input.device: key scancode decoding and a byte queue shared by consoles. */
#ifndef CRUG_INPUT_H
#define CRUG_INPUT_H

#include <crug/types.h>

/* Platform driver (kernel/arch/<arch>/keyboard.c). */
int keyboard_init(void);
void keyboard_poll(void);

/* Generic layer: reset state, feed one PS/2 set-1 scancode (translation
   enabled), read one translated byte. */
void input_init(void);
void input_scancode(u8 scan);
/* Queue a character decoded elsewhere, e.g. by the USB HID keyboard. */
void input_char(u8 c);

/* Pointer device: relative motion and buttons from a HID mouse. Motion is
   accumulated between reads; a read consumes the accumulated deltas and the
   pending button-change mask. */
struct input_pointer {
    i32 dx;
    i32 dy;
    i32 wheel;
    u32 buttons;  /* bit 0 left, bit 1 right, bit 2 middle */
    u32 changes;  /* button bits that changed since the last read */
};
void input_pointer_event(u32 buttons, i32 dx, i32 dy, i32 wheel);
int input_pointer_read(struct input_pointer *out);
void input_pointer_reset(void);
u64 input_pointer_count(void);
/* Optional poll hook run before the queue is read (the USB keyboard uses it). */
void input_set_poll(void (*fn)(void));
/* Run the poll hook once without consuming a queued byte; the graphics cursor
   pumps USB events this way while it waits for pointer motion. */
void input_poll(void);
int input_try_getc(void);

#endif /* CRUG_INPUT_H */
