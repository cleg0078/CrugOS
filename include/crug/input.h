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
int input_try_getc(void);

#endif /* CRUG_INPUT_H */
