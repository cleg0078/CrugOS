/* Loader platform helpers that talk to firmware protocols. */
#ifndef CRUG_LOADER_PLATFORM_H
#define CRUG_LOADER_PLATFORM_H

#include "uefi.h"
#include <crug/bootinfo.h>

int loader_read_file(efi_boot_services *bs, efi_handle device, const char16 *path,
                     void **data, u64 *size);

int loader_gop_init(efi_system_table *st, framebuffer_info *out);

void loader_entropy_fill(efi_system_table *st, u8 out[64]);

int loader_normalize_memmap(const efi_memory_descriptor *desc, u64 count, u64 desc_size,
                            memory_region *out, u64 capacity, u64 *out_count);

int loader_memmap_mark(memory_region *map, u64 *count, u64 capacity, u64 base, u64 size,
                       u32 kind);

#endif /* CRUG_LOADER_PLATFORM_H */
