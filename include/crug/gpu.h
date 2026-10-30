#pragma once

#include <crug/types.h>
#include <crug/bootinfo.h>

/* Phase 8A: legacy virtio-gpu 2D scanout.  The driver owns one B8G8R8X8
   resource which is attached as the linear framebuffer, so the Intuition
   compositor, the Workbench and the text console all draw into guest memory
   and one damage rectangle is transferred to the host per present. */

#define GPU_RECT_MAX_W 4096u
#define GPU_RECT_MAX_H 2160u
#define GPU_BACKING_MAX_PAGES 8192u

struct gpu_rect {
    u32 x;
    u32 y;
    u32 w;
    u32 h;
};

struct gpu_damage {
    u32 x0;
    u32 y0;
    u32 x1;   /* exclusive */
    u32 y1;   /* exclusive */
    int valid;
};

/* Pure helpers (kernel/gpu/gpu_util.c), also linked by the host tests. */
void gpu_put32(u8 *buf, u32 off, u32 value);
void gpu_put64(u8 *buf, u32 off, u64 value);
u32 gpu_get32(const u8 *buf, u32 off);
u32 gpu_cmd_init(u8 *buf, u32 cap, u32 type);
int gpu_resp_class(u32 type);
const char *gpu_resp_name(u32 type);
int gpu_rect_clip(struct gpu_rect *r, u32 width, u32 height);
int gpu_display_pick(const u8 *resp, u32 len, u32 want_w, u32 want_h,
                     u32 *width, u32 *height);
u32 gpu_backing_pages(u32 width, u32 height);
void gpu_damage_reset(struct gpu_damage *dmg);
void gpu_damage_add(struct gpu_damage *dmg, u32 x, u32 y, u32 w, u32 h);
int gpu_damage_rect(const struct gpu_damage *dmg, struct gpu_rect *out);

/* Phase 8B: EDID mode selection and the hardware cursor plane.  The pure
   parser in kernel/gpu/edid.c decodes the base block the device answers to
   GET_EDID so the scanout can adopt the monitor's preferred mode instead of
   guessing from a fixed list, and the cursor plane lets the device composite
   the pointer so the framebuffer no longer carries a software sprite. */
#define GPU_EDID_MAX_BYTES 1024u
#define GPU_EDID_MAX_MODES 4u
#define GPU_CURSOR_PX 64u

struct gpu_edid_mode {
    u32 width;
    u32 height;
    u32 clock_khz;
    u32 refresh_hz;
};

struct gpu_edid {
    int valid;
    u32 block_count;
    u16 vendor;                 /* three packed five-bit letters */
    u16 product;
    u32 serial;
    u32 week;
    u32 year;
    u32 width_cm;
    u32 height_cm;
    u32 mode_count;
    struct gpu_edid_mode modes[GPU_EDID_MAX_MODES];
    int has_preferred;
    struct gpu_edid_mode preferred;
};

/* Decode a raw EDID byte stream (base block plus extensions).  Returns 0 when
   the header and base-block checksum are good and at least one detailed
   timing descriptor was found, -1 otherwise. */
int gpu_edid_parse(const u8 *data, u32 len, struct gpu_edid *out);

/* Write the three-letter manufacturer id ("DEL", "AUO", ...) as a NUL
   terminated string.  Unknown or zero ids produce "???". */
void gpu_edid_vendor(const struct gpu_edid *edid, char name[4]);

/* Driver control (drivers/gpu/virtio_gpu.c). */
void gpu_cmdline(const boot_info *bi);
int gpu_init(void);
int gpu_present(u32 x, u32 y, u32 w, u32 h);
int gpu_selftest(void);
int gpu_ready(void);
u32 gpu_width(void);
u32 gpu_height(void);
u64 gpu_transfers(void);
u64 gpu_flushes(void);
u32 gpu_errors(void);

/* Phase 8B status: whether the device offered EDID and whether the cursor
   plane accepted its cursor resource. */
int gpu_edid_available(void);
int gpu_cursor_ready(void);
u64 gpu_cursor_updates(void);
u64 gpu_cursor_moves(void);
