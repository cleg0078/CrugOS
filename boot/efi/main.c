/* CrugOS UEFI loader: load the kernel ELF, describe the machine, boot it. */
#include <crug/types.h>
#include <crug/bootinfo.h>
#include <crug/version.h>
#include <crug/string.h>
#include "uefi.h"
#include "loader.h"
#include "elf.h"
#include "platform.h"

#define PAGE_SIZE 4096ull
#define EFI_BUFFER_TOO_SMALL_LOCAL 0x8000000000000005ull
#define CMDLINE_MAX 512ull

typedef void (*kernel_entry_point)(const boot_info *info);

static u64 loader_page_round(u64 size)
{
    return (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static int loader_ranges_overlap(u64 a_base, u64 a_size, u64 b_base, u64 b_size)
{
    if (a_size == 0 || b_size == 0) {
        return 0;
    }
    return a_base < b_base + b_size && b_base < a_base + a_size;
}

static int guid_equal(const efi_guid *a, const efi_guid *b)
{
    int i;
    if (a->data1 != b->data1 || a->data2 != b->data2 || a->data3 != b->data3) {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        if (a->data4[i] != b->data4[i]) {
            return 0;
        }
    }
    return 1;
}

static efi_system_table *g_st;
static efi_handle g_image;

static int loader_reserve(void *ctx, u64 address, u64 size, int executable)
{
    efi_physical_address base = address;
    u64 pages;
    efi_status status;

    (void)ctx;
    if (size == 0) {
        return -1;
    }
    pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    status = g_st->boot_services->allocate_pages(EFI_ALLOCATE_ADDRESS,
                                                 executable ? EFI_LOADER_CODE
                                                            : EFI_LOADER_DATA,
                                                 pages, &base);
    if (EFI_ERROR(status)) {
        return -1;
    }
    if (base != address) {
        return -1;
    }
    return 0;
}

static void *loader_alloc_pages(u64 size)
{
    efi_physical_address base = 0;
    u64 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    efi_status status;

    status = g_st->boot_services->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, pages,
                                                 &base);
    if (EFI_ERROR(status)) {
        return 0;
    }
    return (void *)(uintptr)base;
}

static int loader_elf_copy(void *ctx, u64 destination, const void *source, u64 size)
{
    (void)ctx;
    memcpy((void *)(uintptr)destination, source, size);
    return 0;
}

static int loader_elf_zero(void *ctx, u64 destination, u64 size)
{
    (void)ctx;
    memset((void *)(uintptr)destination, 0, size);
    return 0;
}

static void loader_elf_error(void *ctx, const char *message)
{
    (void)ctx;
    loader_print("elf: ");
    loader_println(message);
}

static const elf_ops g_elf_ops = {
    .ctx = 0,
    .reserve = loader_reserve,
    .copy = loader_elf_copy,
    .zero = loader_elf_zero,
    .err = loader_elf_error,
};

static u64 loader_config_rsdp(void)
{
    efi_configuration_table *table = g_st->configuration_table;
    u64 index;

    for (index = 0; index < g_st->number_of_table_entries; index++) {
        const efi_guid *vendor = &table[index].vendor_guid;
        if (guid_equal(vendor, &g_guid_acpi2_table) ||
            guid_equal(vendor, &g_guid_acpi1_table)) {
            return (u64)(uintptr)table[index].vendor_table;
        }
    }
    return 0;
}

static u64 loader_read_tsc_local(void)
{
    return loader_read_tsc();
}

static u8 *g_memmap_buffer;
static u64 g_memmap_size;
static u64 g_memmap_key;
static u64 g_memmap_desc_size;
static u32 g_memmap_desc_version;

static void loader_snapshot_memmap(void)
{
    efi_status status;

    g_memmap_size = 0;
    status = g_st->boot_services->get_memory_map(&g_memmap_size, 0, &g_memmap_key,
                                                 &g_memmap_desc_size, &g_memmap_desc_version);
    if (status != EFI_BUFFER_TOO_SMALL_LOCAL) {
        loader_status_fail("get_memory_map(size)", status);
    }
    g_memmap_size += g_memmap_desc_size * 4;
    g_memmap_buffer = loader_alloc_pages(g_memmap_size);
    if (!g_memmap_buffer) {
        loader_fail("cannot allocate memory map buffer");
    }
    status = g_st->boot_services->get_memory_map(&g_memmap_size,
                                                 (efi_memory_descriptor *)g_memmap_buffer,
                                                 &g_memmap_key, &g_memmap_desc_size,
                                                 &g_memmap_desc_version);
    if (EFI_ERROR(status)) {
        loader_status_fail("get_memory_map(data)", status);
    }
}

static void loader_exit_boot_services(void)
{
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        efi_status status = g_st->boot_services->exit_boot_services(g_image, g_memmap_key);
        if (!EFI_ERROR(status)) {
            return;
        }
        loader_snapshot_memmap();
    }
    loader_fail("ExitBootServices failed");
}

EFIAPI efi_status efi_main(efi_handle image, efi_system_table *system_table)
{
    efi_loaded_image_protocol *loaded_image = 0;
    static char16 kernel_path[] = {
        '\\', 'c', 'r', 'u', 'g', 'o', 's', '\\',
        'k', 'e', 'r', 'n', 'e', 'l', '.', 'e', 'l', 'f', 0,
    };
    static char16 cmdline_path[] = {
        '\\', 'c', 'r', 'u', 'g', 'o', 's', '\\',
        'c', 'm', 'd', 'l', 'i', 'n', 'e', '.', 't', 'x', 't', 0,
    };
    elf_image image_info;
    boot_info *info;
    memory_region *map;
    u64 map_count = 0;
    u64 map_capacity;
    u64 map_bytes;
    u64 info_bytes;
    u64 kernel_bytes;
    void *kernel_data = 0;
    u64 kernel_size = 0;
    framebuffer_info fb;
    efi_status status;
    kernel_entry_point entry;

    g_st = system_table;
    g_image = image;
    loader_serial_init();
    loader_console_bind(system_table->con_out);
    loader_println("CRUGOS loader: start (CrugOS " CRUG_VERSION_STR ")");

    status = system_table->boot_services->handle_protocol(image, (efi_guid *)&g_guid_loaded_image,
                                                          (void **)&loaded_image);
    if (EFI_ERROR(status) || !loaded_image) {
        loader_fail("no loaded image protocol");
    }

    if (loader_read_file(system_table->boot_services, loaded_image->device_handle, kernel_path,
                         &kernel_data, &kernel_size) != 0) {
        loader_fail("cannot read \\crugos\\kernel.elf");
    }
    loader_print("CRUGOS loader: kernel bytes ");
    loader_print_hex(kernel_size);
    loader_println("");

    if (elf_load64(kernel_data, kernel_size, &g_elf_ops, &image_info) != 0) {
        loader_fail("invalid kernel ELF");
    }
    loader_print("CRUGOS loader: entry ");
    loader_print_hex(image_info.entry);
    loader_println("");

    memset(&fb, 0, sizeof(fb));
    if (loader_gop_init(system_table, &fb) != 0) {
        loader_println("CRUGOS loader: warning: no graphics output");
        memset(&fb, 0, sizeof(fb));
    }

    loader_snapshot_memmap();
    map_capacity = g_memmap_size / (g_memmap_desc_size ? g_memmap_desc_size : 48) + 32;
    map_bytes = loader_page_round(map_capacity * sizeof(memory_region));
    map = loader_alloc_pages(map_bytes);
    if (!map) {
        loader_fail("cannot allocate normalized memory map");
    }
    if (loader_normalize_memmap((const efi_memory_descriptor *)g_memmap_buffer,
                                g_memmap_size / g_memmap_desc_size, g_memmap_desc_size, map,
                                map_capacity, &map_count) != 0) {
        loader_fail("cannot normalize memory map");
    }

    info_bytes = loader_page_round(sizeof(boot_info) + CMDLINE_MAX);
    info = loader_alloc_pages(info_bytes);
    if (!info) {
        loader_fail("cannot allocate boot info");
    }
    memset(info, 0, sizeof(*info));

    /* Optional \crugos\cmdline.txt carrier (crypt.pass=...). The copy lives in
       the boot_info allocation so it is marked CRUG_MEM_BOOTLOADER below. */
    {
        void *cmdline = 0;
        u64 cmdline_len = 0;

        if (loader_read_file(system_table->boot_services, loaded_image->device_handle,
                             cmdline_path, &cmdline, &cmdline_len) == 0 && cmdline != 0) {
            char *payload = (char *)((u8 *)info + sizeof(boot_info));
            u64 copy = cmdline_len < CMDLINE_MAX - 1 ? cmdline_len : CMDLINE_MAX - 1;

            memcpy(payload, cmdline, copy);
            payload[copy] = 0;
            info->command_line = payload;
            info->command_line_len = copy;
            loader_print("CRUGOS loader: cmdline bytes ");
            loader_print_hex(copy);
            loader_println("");
        }
    }

    kernel_bytes = loader_page_round(image_info.size);
    if (loader_memmap_mark(map, &map_count, map_capacity, image_info.base, kernel_bytes,
                           CRUG_MEM_KERNEL) != 0) {
        loader_fail("cannot mark kernel memory");
    }
    if (fb.base != 0 && fb.size != 0) {
        if (loader_memmap_mark(map, &map_count, map_capacity, fb.base,
                               loader_page_round(fb.size), CRUG_MEM_FRAMEBUFFER) != 0) {
            loader_fail("cannot mark framebuffer memory");
        }
    }
    if (loader_memmap_mark(map, &map_count, map_capacity, (u64)(uintptr)info, info_bytes,
                           CRUG_MEM_BOOTLOADER) != 0) {
        loader_fail("cannot mark boot info memory");
    }
    if (loader_memmap_mark(map, &map_count, map_capacity, (u64)(uintptr)map, map_bytes,
                           CRUG_MEM_BOOTLOADER) != 0) {
        loader_fail("cannot mark normalized memory map");
    }

    {
        u64 i;
        for (i = 0; i < map_count; i++) {
            if (map[i].kind != CRUG_MEM_USABLE) {
                continue;
            }
            if (loader_ranges_overlap(map[i].base, map[i].size, image_info.base, kernel_bytes) ||
                loader_ranges_overlap(map[i].base, map[i].size, (u64)(uintptr)map, map_bytes) ||
                loader_ranges_overlap(map[i].base, map[i].size, (u64)(uintptr)info, info_bytes) ||
                (fb.base != 0 &&
                 loader_ranges_overlap(map[i].base, map[i].size, fb.base,
                                       loader_page_round(fb.size)))) {
                loader_fail("memory map invariant violated");
            }
        }
    }

    info->magic = BOOTINFO_MAGIC;
    info->version = BOOTINFO_VERSION;
    info->struct_size = sizeof(boot_info);
    info->flags = 0;
    info->kernel_base = image_info.base;
    info->kernel_size = image_info.size;
    info->memory_map = map;
    info->memory_map_count = map_count;
    info->fb = fb;
    info->acpi_rsdp = loader_config_rsdp();
    info->fdt = 0;
    info->initrd_base = 0;
    info->initrd_size = 0;
    info->boot_cpu_id = 0;
    info->boot_tsc = loader_read_tsc_local();
    loader_entropy_fill(system_table, info->entropy_seed);

    loader_print("CRUGOS loader: regions ");
    loader_print_hex(map_count);
    loader_print(" rsdp ");
    loader_print_hex(info->acpi_rsdp);
    loader_println("");

    entry = (kernel_entry_point)(uintptr)image_info.entry;
    loader_exit_boot_services();
    loader_serial_write("CRUGOS loader: jumping to kernel\n");
    loader_debugcon_putc('J');
    __asm__ volatile("" ::: "memory");
    entry(info);

    loader_fail("kernel returned");
    return 0;
}
