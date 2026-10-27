/* Boot information passed from the UEFI loader to the CrugOS kernel.
 * Layout is shared verbatim by loader and kernel.
 */
#ifndef CRUG_BOOTINFO_H
#define CRUG_BOOTINFO_H

#include <crug/types.h>

#define BOOTINFO_MAGIC 0x43524731u /* "CRG1" */
#define BOOTINFO_VERSION 1u

enum crug_memory_kind {
    CRUG_MEM_USABLE = 0,
    CRUG_MEM_RESERVED = 1,
    CRUG_MEM_ACPI_RECLAIM = 2,
    CRUG_MEM_ACPI_NVS = 3,
    CRUG_MEM_UNUSABLE = 4,
    CRUG_MEM_BOOTLOADER = 5,
    CRUG_MEM_FRAMEBUFFER = 6,
    CRUG_MEM_KERNEL = 7,
};

typedef struct memory_region {
    u64 base;
    u64 size;
    u32 kind;
    u32 reserved;
} memory_region;

/* Matches EFI_GRAPHICS_OUTPUT_MODE_INFORMATION pixel formats. */
enum crug_pixel_format {
    CRUG_PIXEL_RGBX8888 = 0,
    CRUG_PIXEL_BGRX8888 = 1,
    CRUG_PIXEL_BITMASK = 2,
    CRUG_PIXEL_UNKNOWN = 3,
};

typedef struct framebuffer_info {
    u64 base;
    u64 size;
    u32 width;
    u32 height;
    u32 stride;
    u32 format;
} framebuffer_info;

typedef struct boot_info {
    u32 magic;
    u32 version;
    u32 struct_size;
    u32 flags;

    u64 kernel_base;
    u64 kernel_size;

    memory_region *memory_map;
    u64 memory_map_count;

    framebuffer_info fb;

    const char *command_line;
    u64 command_line_len;

    u64 acpi_rsdp;
    u64 fdt;
    u64 initrd_base;
    u64 initrd_size;

    u64 boot_cpu_id;
    u64 boot_tsc;
    u8 entropy_seed[64];
} boot_info;

#endif /* CRUG_BOOTINFO_H */
