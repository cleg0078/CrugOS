#include <crug/storage.h>
#include <crug/block.h>
#include <crug/virtio.h>
#include <crug/nvme.h>
#include <crug/ahci.h>
#include <crug/gpt.h>
#include <crug/ext4.h>
#include <crug/journal.h>
#include <crug/raid.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include "ext4_internal.h"

#define STORAGE_MAX_DISKS 8u

static const u8 raid_type_guid[16] = {
    0x0F, 0x88, 0x9D, 0xA1, 0xFC, 0x05, 0x3B, 0x4D,
    0xA0, 0x06, 0x74, 0x3F, 0x0F, 0x84, 0x91, 0x1E
};

/* Large enough that one journal transaction spans several descriptors:
   the per-descriptor tag capacity is 62 on a 1 KiB journal. */
#define CRASH_FILE_SIZE 102400u

static struct ext4_fs g_root_fs;
static struct block_device *g_root_part;
static u8 g_crash_buf[CRASH_FILE_SIZE];
static u32 g_journal_replayed;
static int g_present;
static char g_hello[256];
static u64 g_hello_size;

static struct gpt_table g_tables[STORAGE_MAX_DISKS];
static u8 g_parsed[STORAGE_MAX_DISKS];
static struct block_device *g_disks[STORAGE_MAX_DISKS];

static u32 storage_collect_disks(void)
{
    struct block_device *bd;
    u32 count = 0;

    for (bd = block_first(); bd && count < STORAGE_MAX_DISKS; bd = bd->next) {
        if (bd->sector_size != CRUG_SECTOR_SIZE)
            continue;
        g_disks[count++] = bd;
    }
    return count;
}

static int storage_mount_root(struct block_device *part)
{
    if (!part)
        return -1;
    g_root_part = part;
    kprintf("CRUGOS: root device %s\n", g_root_part->name);
    if (ext4_mount(&g_root_fs, g_root_part, 0) != 0) {
        /* The journal may hold a committed transaction from a crash. A
           read-write mount replays it; the boot then continues read-only. */
        u32 replayed;

        if (ext4_mount(&g_root_fs, g_root_part, EXT4_MOUNT_RW) != 0) {
            kprintf("storage: ext4 mount failed\n");
            return -1;
        }
        replayed = journal_recovered_blocks(&g_root_fs);
        if (ext4_umount(&g_root_fs) != 0 ||
            ext4_mount(&g_root_fs, g_root_part, 0) != 0) {
            kprintf("storage: ext4 mount failed\n");
            return -1;
        }
        g_journal_replayed = replayed;
        if (replayed)
            kprintf("CRUGOS: journal replayed %u block(s) at mount\n",
                    replayed);
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

static int storage_direct_root(u32 disks)
{
    u32 i, j;
    char pname[BLOCK_NAME_MAX];

    for (i = 0; i < disks; i++) {
        if (!g_parsed[i])
            continue;
        for (j = 0; j < g_tables[i].count; j++) {
            struct gpt_partition *part = &g_tables[i].partitions[j];
            u64 sectors;

            if (strcmp(part->name, "crugroot") != 0)
                continue;
            if (part->last_lba < part->first_lba)
                continue;
            kprintf("gpt    : %u partition(s), root '%s' LBA %llu..%llu\n",
                    g_tables[i].count, part->name,
                    (unsigned long long)part->first_lba,
                    (unsigned long long)part->last_lba);
            sectors = part->last_lba - part->first_lba + 1;
            ksnprintf(pname, sizeof(pname), "%s%u", g_disks[i]->name, j + 1);
            g_root_part = block_partition(g_disks[i], part->first_lba, sectors,
                                          pname);
            if (!g_root_part)
                return -1;
            kprintf("CRUGOS: gpt ready\n");
            return storage_mount_root(g_root_part);
        }
    }
    return 0;
}

static int storage_raid_root(u32 disks)
{
    struct block_device *members[RAID_MAX_MEMBERS];
    u32 count = 0;
    u32 present = 0;
    int degraded = 0;
    u32 i, j;
    char pname[BLOCK_NAME_MAX];
    struct block_device *array;

    for (i = 0; i < disks && count < RAID_MAX_MEMBERS; i++) {
        if (!g_parsed[i])
            continue;
        for (j = 0; j < g_tables[i].count && count < RAID_MAX_MEMBERS; j++) {
            struct gpt_partition *part = &g_tables[i].partitions[j];
            u64 sectors;

            if (memcmp(part->type_guid, raid_type_guid, 16) != 0)
                continue;
            if (part->last_lba < part->first_lba)
                continue;
            sectors = part->last_lba - part->first_lba + 1;
            ksnprintf(pname, sizeof(pname), "%s%u", g_disks[i]->name, j + 1);
            members[count] = block_partition(g_disks[i], part->first_lba,
                                            sectors, pname);
            if (!members[count])
                return -1;
            count++;
        }
    }
    if (count == 0) {
        kprintf("storage: no usable GPT partition\n");
        return -1;
    }
    kprintf("CRUGOS: gpt ready\n");
    if (raid_assemble(members, count, "md0", &present, &degraded) != 0) {
        kprintf("storage: raid assembly failed\n");
        return -1;
    }
    array = block_find("md0");
    if (!array)
        return -1;
    kprintf("CRUGOS: raid ready\n");
    if (raid_rebuilt_members(array) > 0)
        kprintf("CRUGOS: raid rebuilt (%u member(s))\n",
                raid_rebuilt_members(array));
    kprintf("raid   : level %u, %u member(s), %u pair(s), chunk %u sectors\n",
            raid_level(array), raid_member_count(array), raid_pairs(array),
            raid_chunk_sectors(array));
    if (degraded || raid_is_degraded(array)) {
        kprintf("CRUGOS: raid degraded (%u/%u members)\n", present,
                raid_member_count(array));
    } else {
        kprintf("CRUGOS: raid healthy (%u/%u members)\n", present, present);
    }
    return storage_mount_root(array);
}

int storage_init(void)
{
    int virtio_devices;
    int nvme_devices;
    int ahci_devices;
    u32 disks;
    u32 i;
    int ret;

    block_init();
    virtio_devices = virtio_blk_init();
    kprintf("CRUGOS: virtio ready (%d device(s))\n", virtio_devices);
    nvme_devices = nvme_init();
    kprintf("CRUGOS: nvme ready (%d device(s))\n", nvme_devices);
    ahci_devices = ahci_init();
    kprintf("CRUGOS: ahci ready (%d device(s))\n", ahci_devices);
    if (virtio_devices + nvme_devices + ahci_devices <= 0) {
        kprintf("storage: no block device driver\n");
        return -1;
    }
    disks = storage_collect_disks();
    if (disks == 0) {
        kprintf("storage: no block device\n");
        return -1;
    }
    kprintf("CRUGOS: block ready (%u device(s))\n", disks);
    for (i = 0; i < disks; i++) {
        if (gpt_parse(g_disks[i], &g_tables[i]) == 1)
            g_parsed[i] = 1;
        else
            kprintf("storage: no GPT on %s\n", g_disks[i]->name);
    }
    ret = storage_direct_root(disks);
    if (ret < 0)
        return -1;
    if (ret > 0)
        return 1;
    return storage_raid_root(disks);
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

static u8 storage_crash_byte(u32 i)
{
    return (u8)((u8)(i * 13u + 7u) ^ 0xA5u);
}

/* Phase 2: the previous boot crashed after committing a multi-descriptor
   transaction. The mount replayed it; /selftest/j.bin must now hold the
   committed (dirty) contents. */
static int storage_replay_check(void)
{
    u64 size;
    u32 i;

    if (ext4_read_file(&g_root_fs, "/selftest/j.bin", g_crash_buf,
                       sizeof(g_crash_buf), &size) != 0)
        return 0;
    if (size != CRASH_FILE_SIZE)
        return 0;
    for (i = 0; i < CRASH_FILE_SIZE; i++) {
        if (g_crash_buf[i] != storage_crash_byte(i))
            return 0;
    }
    kprintf("CRUGOS: journal replay verified (%u block(s) recovered)\n",
            g_journal_replayed);
    arch_exit_qemu(0x10);
    return 1;
}

/* Crash gate phase 1: journal 100 file blocks in one transaction with JBD2
   checksums, publish an unclean superblock and exit without checkpointing.
   Selected by the /simulate-crash marker that scripts/mkdisk.sh puts in the
   journal gate image; ordinary images never take this path. */
static int storage_crash_test(void)
{
    static struct ext4_fs fs;
    static u8 original[CRASH_FILE_SIZE];
    static u8 dirty[1024];
    u32 i;

    if (ext4_mount(&fs, g_root_part, EXT4_MOUNT_RW) != 0) {
        kprintf("storage: crash mount failed\n");
        return 0;
    }
    storage_cleanup(&fs);
    if (ext4_mkdir(&fs, "/selftest", 0755) != 0)
        return 0;
    if (ext4_create(&fs, "/selftest/j.bin", 0644) != 0)
        return 0;
    for (i = 0; i < sizeof(original); i++)
        original[i] = (u8)((i * 13u + 7u) & 0xFFu);
    if (ext4_write_file(&fs, "/selftest/j.bin", original,
                        sizeof(original)) != 0)
        return 0;
    if (ext4_fsync(&fs) != 0)
        return 0;
    /* Journal every file block in one transaction so the commit spans more
       than one descriptor (62 tags each on a 1 KiB journal). */
    if (journal_begin(&fs) != 0)
        return 0;
    for (i = 0; i < CRASH_FILE_SIZE / 1024u; i++) {
        u64 phys;
        u32 off = i * 1024u;
        u32 n;

        if (ext4_file_block(&fs, "/selftest/j.bin", i, &phys) != 0)
            return 0;
        for (n = 0; n < 1024u; n++)
            dirty[n] = storage_crash_byte(off + n);
        if (journal_queue(&fs, phys, dirty) != 0)
            return 0;
    }
    if (journal_test_skip_checkpoint(&fs, 1) != 0)
        return 0;
    if (ext4_test_mark_dirty(&fs) != 0)
        return 0;
    if (journal_commit(&fs) != 0)
        return 0;
    kprintf("CRUGOS: dirty journal committed (no checkpoint)\n");
    arch_exit_qemu(0x10);
    return 1;
}

int storage_selftest(void)
{
    char list[256];
    u32 ino;

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
    if (ext4_lookup(&g_root_fs, "/simulate-crash", &ino) == 0) {
        /* First boot on a clean gate image commits a multi-descriptor
           transaction and dies; the next boot replays it at mount. */
        if (g_journal_replayed)
            return storage_replay_check();
        return storage_crash_test();
    }
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
