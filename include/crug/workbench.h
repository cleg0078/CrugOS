/* Workbench desktop bring-up (phase 6A/6B/6C): opens the first windows on the
   framebuffer, installs the intuition pointer hook, and waits either for a
   pointer drag or for a sequence of menu, gadget and close actions in gate
   mode.  Disabled unless the loader command line carries wb.enable=1, so a
   normal boot keeps the plain text console. */
#ifndef CRUG_WORKBENCH_H
#define CRUG_WORKBENCH_H

#include <crug/types.h>
#include <crug/bootinfo.h>

#define WB_WAIT_MAX 120u
#define WB_DRAG_MAX 4u
#define WB_ACTS_MAX 8u
#define WB_HOLD_MAX 3600u
#define WB_SHELL_CMDS_MAX 8u
#define WB_DESK_ACTS_MAX 8u
/* Two presses on the same desktop object within this window open it: the gate
   driver clicks twice in quick succession on the volume icon. */
#define WB_DOUBLE_CLICK_NS 600000000ull
/* Phase 6G: the drawer parses its listing into this table so it can scroll,
   refresh and reflow; wb.fill= creates files so the table has rows to scroll. */
#define WB_DRAWER_MAX 40u
#define WB_DRAWER_NAME_MAX 32u
#define WB_FILL_MAX 24u
#define WB_DRAWER_ROW_H 20u
/* Phase 6H: the drawer body selects rows and drags its scrollbar; the track
   is 4 pixels wide on the right edge of the body. */
#define WB_SELECT_ACTS_MAX 8u
#define WB_TRACK_W 4u

int wb_init(void);
void wb_cmdline(const boot_info *bi);
void wb_poll(void);
int wb_selftest(void);
int wb_ready(void);
void wb_hold(void);

#endif /* CRUG_WORKBENCH_H */
