/* gfx.device: a pointer-driven cursor on the framebuffer text console.
   Phase 5D: the phase 5B USB mouse pointer state drives a software sprite that
   the framebuffer console composites over its text. */
#ifndef CRUG_GFX_H
#define CRUG_GFX_H

#include <crug/types.h>
#include <crug/bootinfo.h>

/* Pixels of cursor motion per input unit (the KVM relative delta). */
#define GFX_CURSOR_STEP 1
#define GFX_WAIT_MAX 120u
#define GFX_MOVES_MAX 16u

struct gfx_cursor {
    i32 x;
    i32 y;
    u32 visible;
    u64 moves;  /* pointer reports that moved the cursor */
    u64 buttons;
};

int gfx_init(void);
void gfx_cmdline(const boot_info *bi);
/* Drain pending pointer events into cursor motion; called from the console
   and from the boot self-test. */
void gfx_pointer_poll(void);
void gfx_show(int visible);
int gfx_selftest(void);
u64 gfx_move_count(void);
const struct gfx_cursor *gfx_cursor_state(void);

#endif /* CRUG_GFX_H */
