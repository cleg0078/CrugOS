/* Locate, select and describe the UEFI graphics output device. */
#include <crug/types.h>
#include <crug/bootinfo.h>
#include "uefi.h"
#include "platform.h"

#define TARGET_WIDTH 1024u
#define TARGET_HEIGHT 768u

int loader_gop_init(efi_system_table *st, framebuffer_info *out)
{
    efi_graphics_output_protocol *gop = 0;
    efi_status status;

    status = st->boot_services->locate_protocol((efi_guid *)&g_guid_graphics_output, 0,
                                                (void **)&gop);
    if (EFI_ERROR(status) || !gop || !gop->mode) {
        return -1;
    }

    if (!gop->mode->info || gop->mode->info->horizontal_resolution != TARGET_WIDTH ||
        gop->mode->info->vertical_resolution != TARGET_HEIGHT) {
        u32 mode;
        for (mode = 0; mode < gop->mode->max_mode; mode++) {
            u64 info_size = 0;
            efi_gop_mode_info *info = 0;
            status = gop->query_mode(gop, mode, &info_size, &info);
            if (EFI_ERROR(status) || !info) {
                continue;
            }
            if (info->horizontal_resolution != TARGET_WIDTH ||
                info->vertical_resolution != TARGET_HEIGHT ||
                info->pixel_format > 1) {
                continue;
            }
            status = gop->set_mode(gop, mode);
            if (!EFI_ERROR(status)) {
                break;
            }
        }
    }

    if (!gop->mode->info) {
        return -1;
    }

    out->base = gop->mode->framebuffer_base;
    out->size = gop->mode->framebuffer_size;
    out->width = gop->mode->info->horizontal_resolution;
    out->height = gop->mode->info->vertical_resolution;
    out->stride = gop->mode->info->pixels_per_scan_line;
    out->format = gop->mode->info->pixel_format < 2 ? gop->mode->info->pixel_format
                                                    : CRUG_PIXEL_UNKNOWN;
    return 0;
}
