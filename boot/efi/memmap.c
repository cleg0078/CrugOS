/* EFI memory map normalization: kinds, sorting, merging, region marking. */
#include <crug/types.h>
#include <crug/bootinfo.h>
#include <crug/string.h>
#include "uefi.h"
#include "platform.h"

#define PAGE_SIZE 4096ull

static u32 map_kind(u32 efi_type)
{
    switch (efi_type) {
    case EFI_CONVENTIONAL_MEMORY:
        return CRUG_MEM_USABLE;
    case EFI_LOADER_CODE:
    case EFI_LOADER_DATA:
    case EFI_BOOT_SERVICES_CODE:
    case EFI_BOOT_SERVICES_DATA:
        return CRUG_MEM_BOOTLOADER;
    case EFI_ACPI_RECLAIM_MEMORY:
        return CRUG_MEM_ACPI_RECLAIM;
    case EFI_ACPI_MEMORY_NVS:
        return CRUG_MEM_ACPI_NVS;
    case EFI_UNUSABLE_MEMORY:
        return CRUG_MEM_UNUSABLE;
    default:
        return CRUG_MEM_RESERVED;
    }
}

static void insertion_sort(memory_region *map, u64 count)
{
    u64 i;
    for (i = 1; i < count; i++) {
        memory_region key = map[i];
        u64 j = i;
        while (j > 0 && map[j - 1].base > key.base) {
            map[j] = map[j - 1];
            j--;
        }
        map[j] = key;
    }
}

int loader_normalize_memmap(const efi_memory_descriptor *desc, u64 count, u64 desc_size,
                            memory_region *out, u64 capacity, u64 *out_count)
{
    u64 i;
    u64 n = 0;
    u64 write;

    if (!desc || !out || !out_count || desc_size < sizeof(efi_memory_descriptor)) {
        return -1;
    }
    if (count > capacity) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        const efi_memory_descriptor *d =
            (const efi_memory_descriptor *)((const u8 *)desc + i * desc_size);
        u64 bytes;
        if (d->number_of_pages == 0) {
            continue;
        }
        if (d->number_of_pages > (~0ull) / PAGE_SIZE) {
            return -1;
        }
        bytes = d->number_of_pages * PAGE_SIZE;
        if (d->physical_start > ~0ull - bytes) {
            return -1;
        }
        out[n].base = d->physical_start;
        out[n].size = bytes;
        out[n].kind = map_kind(d->type);
        out[n].reserved = 0;
        n++;
    }

    insertion_sort(out, n);

    write = 0;
    for (i = 0; i < n; i++) {
        if (write > 0 && out[write - 1].kind == out[i].kind &&
            out[write - 1].base + out[write - 1].size == out[i].base) {
            out[write - 1].size += out[i].size;
        } else {
            out[write++] = out[i];
        }
    }
    *out_count = write;
    return 0;
}

int loader_memmap_mark(memory_region *map, u64 *count, u64 capacity, u64 base, u64 size,
                       u32 kind)
{
    u64 end;
    u64 i;

    if (!map || !count || size == 0 || base > ~0ull - size) {
        return -1;
    }
    end = base + size;

    for (i = *count; i-- > 0;) {
        memory_region region = map[i];
        u64 region_end = region.base + region.size;
        u64 lo = region.base > base ? region.base : base;
        u64 hi = region_end < end ? region_end : end;
        memory_region segs[3];
        u64 n = 0;
        u64 tail;
        u64 k;

        if (lo >= hi) {
            continue;
        }
        if (region.base < lo) {
            segs[n].base = region.base;
            segs[n].size = lo - region.base;
            segs[n].kind = region.kind;
            segs[n].reserved = 0;
            n++;
        }
        segs[n].base = lo;
        segs[n].size = hi - lo;
        segs[n].kind = kind;
        segs[n].reserved = 0;
        n++;
        if (hi < region_end) {
            segs[n].base = hi;
            segs[n].size = region_end - hi;
            segs[n].kind = region.kind;
            segs[n].reserved = 0;
            n++;
        }
        if (*count - 1 + n > capacity) {
            return -1;
        }
        tail = (i + 1 < *count) ? (*count - i - 1) : 0;
        if (tail) {
            memmove(&map[i + n], &map[i + 1], tail * sizeof(memory_region));
        }
        for (k = 0; k < n; k++) {
            map[i + k] = segs[k];
        }
        *count += n - 1;
    }
    return 0;
}
