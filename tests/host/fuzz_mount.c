/* Host-side mount-path fuzzer for CrugOS.
 *
 * The kernel trust boundary starts at the disk image: the GPT, the ext4
 * superblock, the group descriptors, the inode table, the directory blocks and
 * the encryption header are all attacker controlled in the threat model, and
 * every byte of them reaches ring 0 during the boot path. This fuzzer loads a
 * real image produced by scripts/mkdisk.sh and, for every case, forks a child
 * that applies a small mutation plan to its private copy of the image and then
 * exercises the mount path:
 *
 *     gpt_parse -> ext4_mount (read-only, or read-write for a share of cases)
 *               -> lookup / list / read_file -> umount
 *
 * The parent only observes the child exit status, so a wild write, a NULL
 * dereference or a kernel panic is caught instead of killing the run. The same
 * machinery mutates the 256-byte encryption header and pushes it through
 * crypt_meta_validate().
 *
 * A finding is: the child died on a signal, or the child hit the kernel panic
 * hook (exit status 97). The mutated image of a finding is written out so the
 * case can be studied with gdb.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <crug/types.h>
#include <crug/block.h>
#include <crug/crypt.h>
#include <crug/ext4.h>
#include <crug/gpt.h>
#include <crug/heap.h>
#include <crug/kernel.h>

#define MUT_MAX 4u
#define REGION_MAX 8u
#define PANIC_EXIT 97

#define MUT_BYTE 0u
#define MUT_FF 1u
#define MUT_ZERO 2u
#define MUT_U16 3u
#define MUT_U32 4u
#define MUT_U64 5u
#define MUT_SPLICE 6u
#define MUT_FILL 7u

/* ------------------------------------------------------------------ */
/* Host stubs for the kernel interfaces the mount path depends on.    */
/* ------------------------------------------------------------------ */

static int g_show_kernel_log;
static u32 g_timeout_sec = 10u;

void kprintf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    if (!g_show_kernel_log)
        return;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs("  kernel: ", stderr);
    fputs(buf, stderr);
    fputc(10, stderr);
}

void panic(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs("FUZZ panic: ", stderr);
    fputs(buf, stderr);
    fputc(10, stderr);
    _exit(PANIC_EXIT);
}

/* The real PMM hands out physical pages; the host build only needs a bump
   allocator over a static arena. free() is a no-op: a case never runs long
   enough for the leak to matter and the kernel heap reuses the pages. */
#define ARENA_BYTES (32u * 1024u * 1024u)
static u8 g_arena[ARENA_BYTES] __attribute__((aligned(4096)));
static u64 g_arena_used;

u64 pmm_alloc_pages(u64 count)
{
    u64 bytes;
    u64 pa;

    if (count == 0u || count > ARENA_BYTES / 4096u)
        return 0u;
    bytes = count * 4096u;
    if (g_arena_used + bytes > ARENA_BYTES)
        return 0u;
    pa = (u64)(uintptr)&g_arena[g_arena_used];
    g_arena_used += bytes;
    return pa;
}

u64 pmm_alloc_pages_aligned(u64 count, u64 align_pages)
{
    (void)align_pages;
    return pmm_alloc_pages(count);
}

u64 pmm_alloc_page(void)
{
    return pmm_alloc_pages(1u);
}

void pmm_free_page(u64 pa)
{
    (void)pa;
}

void pmm_free_pages(u64 pa, u64 count)
{
    (void)pa;
    (void)count;
}

/* ------------------------------------------------------------------ */
/* A memory-backed block device.                                     */
/* ------------------------------------------------------------------ */

struct disk_ctx {
    u8 *image;
    u64 bytes;
    u64 offset;
    u64 sectors;
};

static int dev_read(struct block_device *bd, u64 lba, u32 count, void *buf)
{
    struct disk_ctx *ctx = (struct disk_ctx *)bd->priv;
    u64 off;

    if (!ctx || !buf)
        return -1;
    if (lba > ctx->sectors || count > ctx->sectors - lba)
        return -1;
    off = ctx->offset + lba * 512u;
    if (off > ctx->bytes)
        return -1;
    if ((u64)count * 512u > ctx->bytes - off)
        return -1;
    memcpy(buf, ctx->image + off, (usize)count * 512u);
    return 0;
}

static int dev_write(struct block_device *bd, u64 lba, u32 count, const void *buf)
{
    struct disk_ctx *ctx = (struct disk_ctx *)bd->priv;
    u64 off;

    if (!ctx || !buf)
        return -1;
    if (lba > ctx->sectors || count > ctx->sectors - lba)
        return -1;
    off = ctx->offset + lba * 512u;
    if (off > ctx->bytes)
        return -1;
    if ((u64)count * 512u > ctx->bytes - off)
        return -1;
    memcpy(ctx->image + off, buf, (usize)count * 512u);
    return 0;
}

static int dev_flush(struct block_device *bd)
{
    (void)bd;
    return 0;
}

static void dev_setup(struct block_device *bd, struct disk_ctx *ctx, u8 *image,
                      u64 bytes, u64 offset, u64 sectors)
{
    memset(bd, 0, sizeof *bd);
    memset(ctx, 0, sizeof *ctx);
    ctx->image = image;
    ctx->bytes = bytes;
    ctx->offset = offset;
    ctx->sectors = sectors;
    bd->sectors = sectors;
    bd->sector_size = 512u;
    bd->read = dev_read;
    bd->write = dev_write;
    bd->flush = dev_flush;
    bd->priv = ctx;
}

/* ------------------------------------------------------------------ */
/* Image loading and little-endian helpers.                          */
/* ------------------------------------------------------------------ */

static u32 rd16(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void put_le(u8 *dst, u64 value, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++)
        dst[i] = (u8)((value >> (8u * i)) & 0xFFu);
}

static int load_file(const char *path, u8 **out, usize *out_bytes)
{
    FILE *f = fopen(path, "rb");
    long size;
    u8 *data;

    if (!f) {
        fprintf(stderr, "fuzz: cannot open %s", path);
        fputc(10, stderr);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = ftell(f);
    rewind(f);
    if (size <= 0 || (size % 512) != 0) {
        fprintf(stderr, "fuzz: %s is not a sector image", path);
        fputc(10, stderr);
        fclose(f);
        return -1;
    }
    data = (u8 *)malloc((size_t)size);
    if (!data) {
        fclose(f);
        return -1;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = data;
    *out_bytes = (usize)size;
    return 0;
}

/* Find the first GPT partition that spans at least one sector. */
static int discover_partition(u8 *image, u64 bytes, u64 *off_out, u64 *sectors_out)
{
    struct block_device dev;
    struct disk_ctx ctx;
    struct gpt_table table;
    u32 i;

    dev_setup(&dev, &ctx, image, bytes, 0, bytes / 512u);
    if (gpt_parse(&dev, &table) < 0)
        return -1;
    for (i = 0; i < table.count; i++) {
        const struct gpt_partition *p = &table.partitions[i];

        if (p->first_lba == 0u || p->last_lba < p->first_lba)
            continue;
        *off_out = p->first_lba * 512u;
        *sectors_out = p->last_lba - p->first_lba + 1u;
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Mutation plans.                                                   */
/* ------------------------------------------------------------------ */

struct region {
    const char *name;
    u64 off;
    u64 len;
    u32 weight;
};

struct mutation {
    u64 off;
    u32 kind;
    u32 len;
    u64 value;
    u8 bytes[16];
};

struct plan {
    u32 region;
    u32 rw;
    u32 count;
    struct mutation muts[MUT_MAX];
};

static u8 *g_image;
static usize g_image_bytes;
static u64 g_part_offset;
static u64 g_part_sectors;
static struct block_device g_disk;
static struct disk_ctx g_disk_ctx;
static struct block_device g_part;
static struct disk_ctx g_part_ctx;
static struct region g_regions[REGION_MAX];
static u32 g_region_count;
static u8 g_crypt_header[256];
static u64 g_rng = 0x9E3779B97F4A7C15ull;

static u64 rnd64(void)
{
    u64 x = g_rng;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

static u64 edge_value(void)
{
    switch (rnd64() % 10u) {
    case 0: return 0u;
    case 1: return 1u;
    case 2: return 2u;
    case 3: return 0xFFFFu;
    case 4: return 0x7FFFFFFFu;
    case 5: return 0x80000000u;
    case 6: return 0xFFFFFFFFu;
    case 7: return 0xFFFFFFFFFFFFFFFFull;
    case 8: return 0x1000u;
    default: return rnd64();
    }
}

static void add_region(const char *name, u64 off, u64 len, u32 weight)
{
    if (g_region_count >= REGION_MAX || off >= g_image_bytes)
        return;
    if (len > g_image_bytes - off)
        len = g_image_bytes - off;
    if (len < 16u)
        return;
    g_regions[g_region_count].name = name;
    g_regions[g_region_count].off = off;
    g_regions[g_region_count].len = len;
    g_regions[g_region_count].weight = weight;
    g_region_count++;
}

/* The interesting regions of a real ext4 volume: the superblock, the group
   descriptors, the inode table, the root directory first data block, the first
   64 KiB of metadata, and the whole partition. */
static void build_regions(void)
{
    u8 *sb = g_image + g_part_offset + 1024u;
    u32 log_block = rd32(sb + 0x18);
    u32 block_size = 1024u << (log_block & 7u);
    u32 first_data = rd32(sb + 0x14);
    u32 inode_size = rd32(sb + 0x58);
    u32 desc_size = rd16(sb + 0xFE);
    u64 gdt_off;
    u64 itable_block;
    const u8 *desc;
    const u8 *inode;

    if (inode_size < 128u || inode_size > 4096u)
        inode_size = 256u;
    if (block_size < 1024u || block_size > 65536u)
        block_size = 1024u;
    if (desc_size < 32u || desc_size > 1024u)
        desc_size = 32u;

    add_region("superblock", g_part_offset + 1024u, 1024u, 22u);

    gdt_off = (u64)(first_data + 1u) * block_size;
    if (g_part_offset + gdt_off + (u64)desc_size * 4u > g_image_bytes)
        gdt_off = 0u;
    add_region("group-desc", g_part_offset + gdt_off, (u64)desc_size * 4u, 14u);

    desc = g_image + g_part_offset + gdt_off;
    itable_block = (u64)rd32(desc + 8u) | ((u64)rd16(desc + 0x28) << 32);
    if (itable_block > 0u &&
        g_part_offset + itable_block * block_size + 32u * inode_size <= g_image_bytes) {
        u64 itable = g_part_offset + itable_block * block_size;

        add_region("inode-table", itable, (u64)inode_size * 32u, 16u);
        inode = g_image + itable + inode_size; /* inode 2 */
        if (rd16(inode + 40u) == 0xF30Au && rd16(inode + 46u) == 0u && rd16(inode + 42u) > 0u) {
            u64 root_block = ((u64)rd16(inode + 58u) << 32) | (u64)rd32(inode + 60u);

            if (root_block > 0u && g_part_offset + (root_block + 1u) * block_size <= g_image_bytes)
                add_region("root-dir", g_part_offset + root_block * block_size, block_size, 16u);
        }
    }

    add_region("metadata", g_part_offset, 65536u, 17u);
    add_region("partition", g_part_offset, g_part_sectors * 512u, 15u);
}

static u32 pick_region(void)
{
    u32 i;
    u32 total = 0u;
    u32 r;

    for (i = 0; i < g_region_count; i++)
        total += g_regions[i].weight;
    r = (u32)(rnd64() % total);
    for (i = 0; i < g_region_count; i++) {
        if (r < g_regions[i].weight)
            return i;
        r -= g_regions[i].weight;
    }
    return 0u;
}

static void build_plan(struct plan *p, int crypt)
{
    u32 i;

    memset(p, 0, sizeof *p);
    if (crypt) {
        p->region = 0u;
        p->rw = 0u;
        p->count = 1u + (u32)(rnd64() % 3u);
    } else {
        p->region = pick_region();
        p->rw = (rnd64() % 4u) == 0u ? 1u : 0u;
        p->count = 1u + (u32)(rnd64() % MUT_MAX);
    }
    for (i = 0; i < p->count; i++) {
        struct mutation *m = &p->muts[i];
        u64 span = crypt ? 256u : g_regions[p->region].len;

        m->kind = (u32)(rnd64() % 8u);
        if (span > 16u)
            m->off = rnd64() % (span - 16u);
        else
            m->off = 0u;
        m->len = 16u;
        m->value = edge_value();
        if (m->kind == MUT_SPLICE)
            m->value = rnd64() % span;
        if (m->kind == MUT_FILL) {
            u32 b;

            for (b = 0; b < 16u; b++)
                m->bytes[b] = (u8)(rnd64() & 0xFFu);
        }
    }
}

static void apply_plan(u8 *base, u64 base_len, const struct plan *p)
{
    u32 i;

    for (i = 0; i < p->count; i++) {
        const struct mutation *m = &p->muts[i];
        u64 avail;
        u32 len;
        u8 *dst;

        if (m->off >= base_len)
            continue;
        avail = base_len - m->off;
        len = m->len < avail ? m->len : (u32)avail;
        dst = base + m->off;
        switch (m->kind) {
        case MUT_BYTE:
            dst[0] ^= (u8)(m->value & 0xFFu);
            break;
        case MUT_FF:
            memset(dst, 0xFF, len);
            break;
        case MUT_ZERO:
            memset(dst, 0, len);
            break;
        case MUT_U16:
            if (avail >= 2u)
                put_le(dst, m->value, 2u);
            break;
        case MUT_U32:
            if (avail >= 4u)
                put_le(dst, m->value, 4u);
            break;
        case MUT_U64:
            if (avail >= 8u)
                put_le(dst, m->value, 8u);
            break;
        case MUT_SPLICE: {
            const u8 *src = base + (m->value % base_len);

            if (len > 0u && (u64)len <= base_len)
                memmove(dst, src, len);
            break;
        }
        case MUT_FILL:
            memcpy(dst, m->bytes, len);
            break;
        default:
            break;
        }
    }
}

static void print_plan(const struct plan *p, int crypt)
{
    u32 i;

    fprintf(stderr, "  region=%s rw=%u mutation(s)=%u",
            crypt ? "crypt-header" : g_regions[p->region].name, p->rw, p->count);
    fputc(10, stderr);
    for (i = 0; i < p->count; i++) {
        const struct mutation *m = &p->muts[i];

        fprintf(stderr, "  mutation %u: off=0x%llx kind=%u value=0x%llx",
                i, (unsigned long long)m->off, m->kind, (unsigned long long)m->value);
        fputc(10, stderr);
    }
}

/* ------------------------------------------------------------------ */
/* Cases.                                                            */
/* ------------------------------------------------------------------ */

/* Coverage counters: a child exits with 0 (rejected), 1 (mounted) or
   2 (mounted and walked a path), so the parent can report how deep the
   cases actually reached. */
static u64 g_case_mounted;
static u64 g_case_walked;
static u64 g_case_wrote;

static void child_case(const struct plan *p, int crypt)
{
    if (crypt) {
        struct crug_crypt_header header;

        memcpy(&header, g_crypt_header, sizeof g_crypt_header);
        apply_plan((u8 *)&header, sizeof header, p);
        alarm(g_timeout_sec);
        (void)crypt_meta_validate(&header);
        (void)crypt_meta_scratch_bytes(&header);
        _exit(0);
    }

    apply_plan(g_image + g_regions[p->region].off, g_regions[p->region].len, p);
    alarm(g_timeout_sec);
    {
        struct gpt_table table;
        struct ext4_fs fs;
        char buf[512];
        usize size = 0;
        u32 ino = 0;
        int depth = 0;

        (void)gpt_parse(&g_disk, &table);
        memset(&fs, 0, sizeof fs);
        if (ext4_mount(&fs, &g_part, p->rw ? EXT4_MOUNT_RW : 0u) == 0) {
            depth = 1;
            if (ext4_lookup(&fs, "/hello.txt", &ino) == 0 &&
                ext4_read_file(&fs, "/hello.txt", buf, sizeof buf, &size) == 0 &&
                size > 0u)
                depth = 2;
            (void)ext4_lookup(&fs, "/", &ino);
            (void)ext4_list(&fs, "/", buf, sizeof buf);
            size = 0;
            (void)ext4_read_file(&fs, "/selftest/b.txt", buf, sizeof buf, &size);
            if (p->rw) {
                u8 data[512];
                u32 k;
                int wrote = 0;

                for (k = 0; k < sizeof data; k++)
                    data[k] = (u8)((k * 7u + 3u) & 0xFFu);
                if (ext4_create(&fs, "/fz-a", 0644u) == 0) {
                    (void)ext4_write_file(&fs, "/fz-a", data, sizeof data);
                    (void)ext4_truncate(&fs, "/fz-a", 64u);
                    (void)ext4_fsync(&fs);
                    wrote = 1;
                }
                if (ext4_mkdir(&fs, "/fz-d", 0755u) == 0) {
                    (void)ext4_create(&fs, "/fz-d/x", 0644u);
                    (void)ext4_write_file(&fs, "/fz-d/x", data, 200u);
                    (void)ext4_rename(&fs, "/fz-d/x", "/fz-d/y");
                    (void)ext4_unlink(&fs, "/fz-d/y");
                    (void)ext4_rmdir(&fs, "/fz-d");
                    wrote = 1;
                }
                (void)ext4_rename(&fs, "/fz-a", "/fz-b");
                (void)ext4_unlink(&fs, "/fz-b");
                (void)ext4_write_file(&fs, "/hello.txt", data, 32u);
                if (wrote)
                    depth = 3;
            }
            (void)ext4_umount(&fs);
        }
        _exit(depth);
    }
}

static void dump_case(const char *dir, u32 index, const struct plan *p, int crypt)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof path, "%s/fuzz-%s-%u.img", dir, crypt ? "crypt" : "ext4", index);
    f = fopen(path, "wb");
    if (!f)
        return;
    if (crypt) {
        struct crug_crypt_header header;

        memcpy(&header, g_crypt_header, sizeof g_crypt_header);
        apply_plan((u8 *)&header, sizeof header, p);
        (void)fwrite(&header, 1, sizeof header, f);
    } else {
        u8 *copy = (u8 *)malloc((size_t)g_image_bytes);

        if (!copy) {
            fclose(f);
            return;
        }
        memcpy(copy, g_image, (size_t)g_image_bytes);
        apply_plan(copy + g_regions[p->region].off, g_regions[p->region].len, p);
        (void)fwrite(copy, 1, (size_t)g_image_bytes, f);
        free(copy);
    }
    fclose(f);
    fprintf(stderr, "  image written to %s", path);
    fputc(10, stderr);
}

static int report_case(u32 index, int crypt, const struct plan *p, int status,
                       const char *crash_dir)
{
    if (WIFSIGNALED(status))
        fprintf(stderr, "FAIL: %s case %u died on signal %d",
                crypt ? "crypt" : "ext4", index, WTERMSIG(status));
    else if (WEXITSTATUS(status) == PANIC_EXIT)
        fprintf(stderr, "FAIL: %s case %u hit the kernel panic hook",
                crypt ? "crypt" : "ext4", index);
    else
        fprintf(stderr, "FAIL: %s case %u exited with status %d",
                crypt ? "crypt" : "ext4", index, WEXITSTATUS(status));
    fputc(10, stderr);
    print_plan(p, crypt);
    if (crash_dir)
        dump_case(crash_dir, index, p, crypt);
    return 1;
}

static int run_case(u32 index, int crypt, const char *crash_dir)
{
    struct plan plan;
    pid_t pid;
    int status = 0;
    int r;

    build_plan(&plan, crypt);
    pid = fork();
    if (pid < 0) {
        fputs("fuzz: fork failed", stderr);
        fputc(10, stderr);
        return 1;
    }
    if (pid == 0)
        child_case(&plan, crypt);
    do {
        r = waitpid(pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        fputs("fuzz: waitpid failed", stderr);
        fputc(10, stderr);
        return 1;
    }
    if (WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) > 3))
        return report_case(index, crypt, &plan, status, crash_dir);
    if (WIFEXITED(status)) {
        int depth = WEXITSTATUS(status);

        if (depth >= 1)
            g_case_mounted++;
        if (depth >= 2)
            g_case_walked++;
        if (depth >= 3)
            g_case_wrote++;
    }
    return 0;
}

/* Mount the pristine image once so a harness wiring bug is not reported as a
   kernel finding. */
static int control_mount(void)
{
    struct ext4_fs fs;
    u32 ino = 0;
    char buf[512];
    usize size = 0;

    memset(&fs, 0, sizeof fs);
    if (ext4_mount(&fs, &g_part, 0u) != 0) {
        fputs("fuzz: the pristine image does not mount", stderr);
        fputc(10, stderr);
        return -1;
    }
    if (ext4_lookup(&fs, "/hello.txt", &ino) != 0 ||
        ext4_read_file(&fs, "/hello.txt", buf, sizeof buf, &size) != 0) {
        fputs("fuzz: the pristine image has no readable /hello.txt", stderr);
        fputc(10, stderr);
        (void)ext4_umount(&fs);
        return -1;
    }
    (void)ext4_umount(&fs);
    return 0;
}

static int load_crypt_header(const char *path)
{
    u8 *image = 0;
    usize bytes = 0;
    u64 off = 0;
    u64 sectors = 0;

    if (load_file(path, &image, &bytes) != 0)
        return -1;
    if (discover_partition(image, (u64)bytes, &off, &sectors) != 0 ||
        off + sizeof g_crypt_header > bytes) {
        fprintf(stderr, "fuzz: %s has no readable volume header", path);
        fputc(10, stderr);
        free(image);
        return -1;
    }
    memcpy(g_crypt_header, image + off, sizeof g_crypt_header);
    free(image);
    return 0;
}

static void usage(void)
{
    fputs("usage: fuzz-mount --image PATH [--crypt-image PATH] [--cases N]", stderr);
    fputc(10, stderr);
    fputs("                 [--crypt-cases N] [--seed N] [--timeout SEC]", stderr);
    fputc(10, stderr);
    fputs("                 [--crash-dir DIR] [--verbose-kernel] [--selftest]", stderr);
    fputc(10, stderr);
}

int main(int argc, char **argv)
{
    const char *image = 0;
    const char *crypt_image = 0;
    const char *crash_dir = 0;
    u32 cases = 1000u;
    u32 crypt_cases = 0u;
    u32 seed = 1337u;
    u32 i;
    int a;
    int selftest = 0;

    for (a = 1; a < argc; a++) {
        const char *arg = argv[a];

        if (strcmp(arg, "--image") == 0 && a + 1 < argc)
            image = argv[++a];
        else if (strcmp(arg, "--crypt-image") == 0 && a + 1 < argc)
            crypt_image = argv[++a];
        else if (strcmp(arg, "--cases") == 0 && a + 1 < argc)
            cases = (u32)strtoul(argv[++a], 0, 0);
        else if (strcmp(arg, "--crypt-cases") == 0 && a + 1 < argc)
            crypt_cases = (u32)strtoul(argv[++a], 0, 0);
        else if (strcmp(arg, "--seed") == 0 && a + 1 < argc)
            seed = (u32)strtoul(argv[++a], 0, 0);
        else if (strcmp(arg, "--timeout") == 0 && a + 1 < argc)
            g_timeout_sec = (u32)strtoul(argv[++a], 0, 0);
        else if (strcmp(arg, "--crash-dir") == 0 && a + 1 < argc)
            crash_dir = argv[++a];
        else if (strcmp(arg, "--verbose-kernel") == 0)
            g_show_kernel_log = 1;
        else if (strcmp(arg, "--selftest") == 0)
            selftest = 1;
        else {
            usage();
            return 2;
        }
    }
    if (!image || cases == 0u || g_timeout_sec == 0u) {
        usage();
        return 2;
    }
    if (load_file(image, &g_image, &g_image_bytes) != 0)
        return 1;
    heap_init();
    if (discover_partition(g_image, (u64)g_image_bytes, &g_part_offset, &g_part_sectors) != 0) {
        fputs("fuzz: the image has no GPT partition", stderr);
        fputc(10, stderr);
        return 1;
    }
    dev_setup(&g_disk, &g_disk_ctx, g_image, (u64)g_image_bytes, 0, (u64)g_image_bytes / 512u);
    dev_setup(&g_part, &g_part_ctx, g_image, (u64)g_image_bytes, g_part_offset, g_part_sectors);
    build_regions();
    if (control_mount() != 0)
        return 1;
    if (crypt_image && crypt_cases > 0u && load_crypt_header(crypt_image) != 0)
        return 1;
    if (selftest) {
        pid_t pid;
        int status = 0;

        pid = fork();
        if (pid < 0) {
            fputs("fuzz: fork failed", stderr);
            fputc(10, stderr);
            return 1;
        }
        if (pid == 0) {
            volatile int *bad = (volatile int *)0;

            *bad = 1;
            _exit(0);
        }
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        if (WIFSIGNALED(status)) {
            fprintf(stdout, "ok: fuzz selftest detected the injected crash (signal %d)",
                    WTERMSIG(status));
            fputc(10, stdout);
            return 0;
        }
        fputs("FAIL: the harness did not report an injected crash", stderr);
        fputc(10, stderr);
        return 1;
    }
    g_rng = 0x9E3779B97F4A7C15ull ^ ((u64)seed * 0x9E3779B1u);
    if (g_rng == 0u)
        g_rng = 0x9E3779B97F4A7C15ull;
    if (crash_dir)
        (void)mkdir(crash_dir, 0755);

    fprintf(stdout, "fuzz: image %s (%llu bytes), partition at byte %llu (%llu sectors)",
            image, (unsigned long long)g_image_bytes,
            (unsigned long long)g_part_offset, (unsigned long long)g_part_sectors);
    fputc(10, stdout);
    fprintf(stdout, "fuzz: %u ext4 case(s), %u header case(s), seed %u, timeout %u s",
            cases, crypt_cases, seed, g_timeout_sec);
    fputc(10, stdout);
    fflush(stdout);

    for (i = 0; i < cases; i++) {
        if (run_case(i, 0, crash_dir) != 0)
            return 1;
    }
    for (i = 0; i < crypt_cases; i++) {
        if (run_case(i, 1, crash_dir) != 0)
            return 1;
    }

    fprintf(stdout, "fuzz: %llu case(s) mounted, %llu walked a path, %llu wrote",
            (unsigned long long)g_case_mounted, (unsigned long long)g_case_walked,
            (unsigned long long)g_case_wrote);
    fputc(10, stdout);
    fprintf(stdout, "ok: fuzz %u ext4 case(s)", cases);
    if (crypt_cases > 0u)
        fprintf(stdout, " and %u crypt header case(s)", crypt_cases);
    fputs(", 0 crash(es)", stdout);
    fputc(10, stdout);
    return 0;
}
