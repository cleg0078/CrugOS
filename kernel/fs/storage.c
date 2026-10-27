#include <crug/storage.h>
#include <crug/block.h>
#include <crug/virtio.h>
#include <crug/gpt.h>
#include <crug/string.h>
#include <crug/kernel.h>

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
    if (ext4_mount(&g_root_fs, g_root_part) != 0) {
        kprintf("storage: ext4 mount failed\n");
        return -1;
    }
    kprintf("CRUGOS: ext4 ready\n");
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
    return 1;
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
