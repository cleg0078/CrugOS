/* Read a file from the loaded image's own device via SimpleFileSystem. */
#include <crug/types.h>
#include "uefi.h"
#include "platform.h"

#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ull
#define EFI_FILE_MODE_READ 0x0000000000000001ull

int loader_read_file(efi_boot_services *bs, efi_handle device, const char16 *path,
                     void **data, u64 *size)
{
    efi_simple_file_system *fs = 0;
    efi_file_protocol *root = 0;
    efi_file_protocol *file = 0;
    efi_file_info *info = 0;
    efi_status status;
    u64 info_size = 0;
    u64 read_size;
    void *buffer = 0;

    status = bs->handle_protocol(device, (efi_guid *)&g_guid_simple_file_system,
                                 (void **)&fs);
    if (EFI_ERROR(status) || !fs) {
        return -1;
    }
    status = fs->open_volume(fs, &root);
    if (EFI_ERROR(status) || !root) {
        return -1;
    }
    status = root->open(root, &file, (char16 *)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !file) {
        return -1;
    }

    status = file->get_info(file, (efi_guid *)&g_guid_file_info, &info_size, 0);
    if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
        return -1;
    }
    status = bs->allocate_pool(EFI_LOADER_DATA, info_size, (void **)&info);
    if (EFI_ERROR(status) || !info) {
        return -1;
    }
    status = file->get_info(file, (efi_guid *)&g_guid_file_info, &info_size, info);
    if (EFI_ERROR(status)) {
        return -1;
    }
    if (info->file_size == 0) {
        return -1;
    }
    status = bs->allocate_pool(EFI_LOADER_DATA, info->file_size, &buffer);
    if (EFI_ERROR(status) || !buffer) {
        return -1;
    }

    read_size = info->file_size;
    status = file->read(file, &read_size, buffer);
    if (EFI_ERROR(status) || read_size != info->file_size) {
        return -1;
    }

    file->close(file);
    root->close(root);
    *data = buffer;
    *size = read_size;
    return 0;
}
