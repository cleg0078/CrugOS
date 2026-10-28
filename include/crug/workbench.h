/* Workbench desktop bring-up (phase 6A): opens the first windows on the
   framebuffer, installs the intuition pointer hook and waits for a drag in
   gate mode.  Disabled unless the loader command line carries wb.enable=1, so
   a normal boot keeps the plain text console. */
#ifndef CRUG_WORKBENCH_H
#define CRUG_WORKBENCH_H

#include <crug/types.h>
#include <crug/bootinfo.h>

#define WB_WAIT_MAX 120u
#define WB_DRAG_MAX 4u

int wb_init(void);
void wb_cmdline(const boot_info *bi);
void wb_poll(void);
int wb_selftest(void);
int wb_ready(void);

#endif /* CRUG_WORKBENCH_H */
