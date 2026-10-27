/* Boot information validation and accounting. */
#include <crug/types.h>
#include <crug/kernel.h>

void bootinfo_validate(const boot_info *bi)
{
    if (!bi) {
        panic("loader supplied no boot_info pointer");
    }
    if (bi->magic != BOOTINFO_MAGIC) {
        panic("bad boot_info magic 0x%x (expected 0x%x)", bi->magic, BOOTINFO_MAGIC);
    }
    if (bi->version != BOOTINFO_VERSION) {
        panic("unsupported boot_info version %u (kernel wants %u)", bi->version,
              BOOTINFO_VERSION);
    }
    if (bi->struct_size != (u32)sizeof(boot_info)) {
        panic("boot_info size mismatch: loader %u kernel %u", bi->struct_size,
              (u32)sizeof(boot_info));
    }
    if (bi->flags != 0) {
        panic("unsupported boot_info flags 0x%x", bi->flags);
    }
    if (!bi->memory_map || bi->memory_map_count == 0) {
        panic("loader supplied an empty memory map");
    }
    if (bi->memory_map_count > 8192) {
        panic("memory map too large (%llu entries)", bi->memory_map_count);
    }
    if (bi->kernel_size == 0) {
        panic("loader reported an empty kernel image");
    }
    if (bi->fb.base &&
        (bi->fb.width == 0 || bi->fb.height == 0 || bi->fb.stride < bi->fb.width)) {
        panic("loader supplied an invalid framebuffer description");
    }
}

u64 bootinfo_usable_bytes(const boot_info *bi)
{
    u64 total = 0;
    u64 i;

    for (i = 0; i < bi->memory_map_count; i++) {
        if (bi->memory_map[i].kind == CRUG_MEM_USABLE) {
            total += bi->memory_map[i].size;
        }
    }
    return total;
}
