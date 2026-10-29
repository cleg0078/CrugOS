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

/* Extended keys: decoded by the PS/2 E0 prefix and the USB HID usages, and
   pushed through the same byte queue as the ASCII characters (phase 6D). */
#define INPUT_KEY_UP 0x90u
#define INPUT_KEY_DOWN 0x91u
#define INPUT_KEY_LEFT 0x92u
#define INPUT_KEY_RIGHT 0x93u

/* Function keys (phase 6J).  They sit above the arrows so the existing
   "special key" tests (ch >= INPUT_KEY_UP) also cover them. */
#define INPUT_KEY_F1 0x94u
#define INPUT_KEY_F2 0x95u
#define INPUT_KEY_F3 0x96u
#define INPUT_KEY_F4 0x97u
#define INPUT_KEY_F5 0x98u
#define INPUT_KEY_F6 0x99u
#define INPUT_KEY_F7 0x9Au
#define INPUT_KEY_F8 0x9Bu
#define INPUT_KEY_F9 0x9Cu
#define INPUT_KEY_F10 0x9Du
#define INPUT_KEY_F11 0x9Eu
#define INPUT_KEY_F12 0x9Fu

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
