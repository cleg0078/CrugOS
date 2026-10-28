#pragma once

#include <crug/types.h>
#include <crug/bootinfo.h>
#include <crug/ext4.h>

int storage_init(const boot_info *bi);
void storage_lock(void);
int storage_selftest(void);
int storage_present(void);
struct ext4_fs *storage_root(void);
int storage_remount_rw(void);
int storage_remount_ro(void);
const char *storage_hello_file(void);
u64 storage_hello_size(void);
