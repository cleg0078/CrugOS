#pragma once

#include <crug/types.h>
#include <crug/ext4.h>

int storage_init(void);
int storage_selftest(void);
int storage_present(void);
struct ext4_fs *storage_root(void);
const char *storage_hello_file(void);
u64 storage_hello_size(void);
