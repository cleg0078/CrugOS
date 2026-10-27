#include <crug/storage.h>
#include <crug/block.h>
#include <crug/virtio.h>
#include <crug/gpt.h>
#include <crug/ext4.h>
#include <crug/journal.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include "ext4_internal.h"

static struct ext4_fs g_root_fs;
static struct block_device *g_root_part;
static int g_present;
static char g_hello[256];
static u64 g_hello_size;

int storage_init(void)
{
    struct block_device *vda;
    struct gpt_table table;
    struct gpt_partition *part;
    u64 sectors;
    u32 i;
    int ret;

    block_init();
    ret = virtio_blk_init();
    if (ret < 0)
        return -1;
    vda = block_find("vda");
    if (!vda) {
        kprintf("storage: no virtio block device\n");
        return -1;
    }
    kprintf("CRUGOS: block ready (%s, %llu sectors)\n", vda->name,
            (unsigned long long)vda->sectors);
    if (gpt_parse(vda, &table) != 1 || table.count == 0) {
        kprintf("storage: no usable GPT partition\n");
        return -1;
    }
    part = &table.partitions[0];
    for (i = 0; i < table.count; i++) {
        if (strcmp(table.partitions[i].name, "crugroot") == 0) {
            part = &table.partitions[i];
            break;
        }
    }
    kprintf("gpt    : %u partition(s), root '%s' LBA %llu..%llu\n",
            table.count, part->name,
            (unsigned long long)part->first_lba,
            (unsigned long long)part->last_lba);
    if (part->last_lba < part->first_lba)
        return -1;
    sectors = part->last_lba - part->first_lba + 1;
    g_root_part = block_partition(vda, part->first_lba, sectors, "vda1");
    if (!g_root_part)
        return -1;
    kprintf("CRUGOS: gpt ready\n");
    if (ext4_mount(&g_root_fs, g_root_part, 0) != 0) {
        kprintf("storage: ext4 mount failed\n");
        return -1;
    }
    kprintf("CRUGOS: ext4 ready\n");
    if (g_root_fs.journal)
        kprintf("CRUGOS: journal ready\n");
    memset(g_hello, 0, sizeof(g_hello));
    if (ext4_read_file(&g_root_fs, "/hello.txt", g_hello,
                       sizeof(g_hello) - 1, &g_hello_size) != 0) {
        kprintf("storage: /hello.txt not found\n");
        return -1;
    }
    if (g_hello_size >= sizeof(g_hello))
        g_hello_size = sizeof(g_hello) - 1;
    g_hello[g_hello_size] = 0;
    kprintf("CRUGOS: ext4 /hello.txt: %s", g_hello);
    if (g_hello_size == 0 || g_hello[g_hello_size - 1] != '\n')
        kprintf("\n");
    g_present = 1;
    kprintf("CRUGOS: storage ready\n");
    return 1;
}

static int storage_cleanup(struct ext4_fs *fs)
{
    ext4_unlink(fs, "/selftest/b.txt");
    ext4_unlink(fs, "/selftest/a.txt");
    ext4_unlink(fs, "/selftest/tmp.txt");
    ext4_rmdir(fs, "/selftest");
    return 0;
}

static int storage_journal_test(struct ext4_fs *fs)
{
    static u8 original[1024];
    static u8 dirty[1024];
    static u8 verify[1024];
    u64 phys = 0;
    u32 i;

    if (fs->block_size != sizeof(original))
        return 0;
    for (i = 0; i < sizeof(original); i++) {
        original[i] = (u8)((i * 11u + 5u) & 0xFFu);
        dirty[i] = (u8)(~original[i]);
    }
    if (ext4_create(fs, "/selftest/j.bin", 0644) != 0)
        return 0;
    if (ext4_write_file(fs, "/selftest/j.bin", original, sizeof(original)) != 0)
        return 0;
    if (ext4_fsync(fs) != 0)
        return 0;
    if (ext4_file_block(fs, "/selftest/j.bin", 0, &phys) != 0)
        return 0;
    if (journal_test_dirty(fs, phys, dirty, sizeof(dirty)) != 0)
        return 0;
    if (ext4_umount(fs) != 0)
        return 0;
    if (ext4_mount(fs, g_root_part, EXT4_MOUNT_RW) != 0)
        return 0;
    if (block_read(fs->bd, phys * fs->sectors_per_block,
                   fs->sectors_per_block, verify) != 0)
        return 0;
    if (memcmp(verify, dirty, sizeof(dirty)) != 0)
        return 0;
    if (ext4_unlink(fs, "/selftest/j.bin") != 0)
        return 0;
    kprintf("CRUGOS: journal selftest ok (%llu blocks recovered)\n",
            (unsigned long long)journal_recovered_blocks(fs));
    return 1;
}

static int storage_rw_test(void)
{
    static struct ext4_fs fs;
    static u8 pattern[5000];
    static u8 check[1024];
    u64 size;
    u32 ino;
    u32 i;

    if (ext4_mount(&fs, g_root_part, EXT4_MOUNT_RW) != 0) {
        kprintf("storage: writable mount failed\n");
        return 0;
    }
    storage_cleanup(&fs);
    if (ext4_mkdir(&fs, "/selftest", 0755) != 0)
        goto fail;
    if (ext4_create(&fs, "/selftest/a.txt", 0644) != 0)
        goto fail;
    for (i = 0; i < sizeof(pattern); i++)
        pattern[i] = (u8)((i * 7u + 3u) & 0xFFu);
    if (ext4_write_file(&fs, "/selftest/a.txt", pattern, sizeof(pattern)) != 0)
        goto fail;
    if (ext4_fsync(&fs) != 0)
        goto fail;
    memset(check, 0, sizeof(check));
    if (ext4_read_file(&fs, "/selftest/a.txt", check, sizeof(check), &size) != 0)
        goto fail;
    if (size != sizeof(pattern) || memcmp(check, pattern, sizeof(check)) != 0)
        goto fail;
    if (ext4_rename(&fs, "/selftest/a.txt", "/selftest/b.txt") != 0)
        goto fail;
    if (ext4_lookup(&fs, "/selftest/a.txt", &ino) == 0)
        goto fail;
    if (ext4_truncate(&fs, "/selftest/b.txt", 1000) != 0)
        goto fail;
    if (ext4_create(&fs, "/selftest/tmp.txt", 0644) != 0)
        goto fail;
    if (ext4_unlink(&fs, "/selftest/tmp.txt") != 0)
        goto fail;
    if (ext4_lookup(&fs, "/selftest/tmp.txt", &ino) == 0)
        goto fail;
    if (!storage_journal_test(&fs))
        goto fail;
    if (ext4_umount(&fs) != 0) {
        kprintf("storage: writable unmount failed\n");
        return 0;
    }
    if (ext4_mount(&fs, g_root_part, 0) != 0) {
        kprintf("storage: read-only remount failed\n");
        return 0;
    }
    memset(check, 0, sizeof(check));
    if (ext4_read_file(&fs, "/selftest/b.txt", check, sizeof(check), &size) != 0)
        goto fail_ro;
    if (size != 1000)
        goto fail_ro;
    for (i = 0; i < 1000; i++) {
        if (check[i] != (u8)((i * 7u + 3u) & 0xFFu))
            goto fail_ro;
    }
    if (ext4_lookup(&fs, "/selftest", &ino) != 0)
        goto fail_ro;
    if (ext4_umount(&fs) != 0)
        goto fail_ro;
    kprintf("CRUGOS: writable storage ok\n");
    return 1;
fail_ro:
    kprintf("storage: read-only verification failed\n");
    ext4_umount(&fs);
    return 0;
fail:
    kprintf("storage: writable test failed\n");
    ext4_umount(&fs);
    return 0;
}

int storage_selftest(void)
{
    char list[256];

    if (!g_present || !g_root_part)
        return 0;
    if (!ext4_selftest())
        return 0;
    if (g_hello_size != strlen("hello from ext4\n"))
        return 0;
    if (memcmp(g_hello, "hello from ext4\n", g_hello_size) != 0)
        return 0;
    if (ext4_list(&g_root_fs, "/", list, sizeof(list)) < 0)
        return 0;
    if (strstr(list, "hello.txt") == 0)
        return 0;
    return storage_rw_test();
}

int storage_present(void)
{
    return g_present;
}

struct ext4_fs *storage_root(void)
{
    return g_present ? &g_root_fs : 0;
}

const char *storage_hello_file(void)
{
    return g_present ? g_hello : 0;
}

u64 storage_hello_size(void)
{
    return g_hello_size;
}
