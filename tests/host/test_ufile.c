/* Host unit tests for the per-thread open file table (phase U3).
 *
 * The table is filesystem agnostic: every hook arrives through
 * struct ufile_backend, so this test keeps a tiny in-memory volume instead of
 * a disk. It pins the fd numbering (3 + slot, not 3 + inode), the snapshot
 * read path, buffered writes flushed on close and the read-write bracket that
 * a flush asks the kernel for. */
#include <crug/ufile.h>
#include <crug/syscall.h>
#include <crug/string.h>
#include "test.h"

#include <stdlib.h>

#define FAKE_FILES 8u
#define FAKE_NAME 32u
#define FAKE_BYTES 256u

typedef struct fake_fs {
    char name[FAKE_FILES][FAKE_NAME];
    u8 data[FAKE_FILES][FAKE_BYTES];
    u32 len[FAKE_FILES];
    u32 files;
    int creates;
    int writes;
    int truncates;
    int fsyncs;
    int begins;
    int ends;
    int begin_fail;
} fake_fs;

void *ufile_host_alloc(usize n)
{
    return malloc((size_t)n);
}

void ufile_host_free(void *p)
{
    free(p);
}

static int fake_find(fake_fs *fs, const char *path)
{
    u32 i;

    for (i = 0; i < fs->files; i++) {
        if (strcmp(fs->name[i], path) == 0)
            return (int)i;
    }
    return -1;
}

static void fake_put(fake_fs *fs, const char *path, const char *text)
{
    int i = fake_find(fs, path);

    if (i < 0) {
        i = (int)fs->files++;
        strncpy(fs->name[i], path, FAKE_NAME - 1u);
    }
    fs->len[i] = (u32)strlen(text);
    memcpy(fs->data[i], text, fs->len[i]);
}

static const char *fake_text(fake_fs *fs, const char *path)
{
    int i = fake_find(fs, path);

    if (i < 0)
        return "<missing>";
    fs->data[i][fs->len[i]] = 0;
    return (const char *)fs->data[i];
}

static int fake_lookup(void *ctx, const char *path, u32 *out_ino)
{
    fake_fs *fs = (fake_fs *)ctx;
    int i = fake_find(fs, path);

    if (i < 0)
        return -1;
    if (out_ino)
        *out_ino = (u32)i + 1u;
    return 0;
}

static int fake_read(void *ctx, const char *path, void *buf, usize cap,
                     u64 *out_size)
{
    fake_fs *fs = (fake_fs *)ctx;
    int i = fake_find(fs, path);
    u32 n;

    if (i < 0)
        return -1;
    n = fs->len[i] < cap ? fs->len[i] : (u32)cap;
    if (n != 0)
        memcpy(buf, fs->data[i], n);
    if (out_size)
        *out_size = fs->len[i];
    return 0;
}

static int fake_create(void *ctx, const char *path, u16 mode)
{
    fake_fs *fs = (fake_fs *)ctx;

    (void)mode;
    fs->creates++;
    if (fake_find(fs, path) >= 0 || fs->files >= FAKE_FILES)
        return -1;
    strncpy(fs->name[fs->files], path, FAKE_NAME - 1u);
    fs->len[fs->files] = 0;
    fs->files++;
    return 0;
}

static int fake_write(void *ctx, const char *path, const void *buf, usize len)
{
    fake_fs *fs = (fake_fs *)ctx;
    int i = fake_find(fs, path);

    fs->writes++;
    if (i < 0 || len > FAKE_BYTES)
        return -1;
    memcpy(fs->data[i], buf, len);
    fs->len[i] = (u32)len;
    return 0;
}

static int fake_truncate(void *ctx, const char *path, u64 size)
{
    fake_fs *fs = (fake_fs *)ctx;
    int i = fake_find(fs, path);

    fs->truncates++;
    if (i < 0 || size > FAKE_BYTES)
        return -1;
    fs->len[i] = (u32)size;
    return 0;
}

static int fake_fsync(void *ctx)
{
    fake_fs *fs = (fake_fs *)ctx;

    fs->fsyncs++;
    return 0;
}

static int fake_begin(void *ctx)
{
    fake_fs *fs = (fake_fs *)ctx;

    fs->begins++;
    return fs->begin_fail ? -1 : 0;
}

static int fake_end(void *ctx)
{
    fake_fs *fs = (fake_fs *)ctx;

    fs->ends++;
    return 0;
}

static const struct ufile_backend g_backend = {
    fake_lookup,
    fake_read,
    fake_create,
    fake_write,
    fake_truncate,
    fake_fsync,
    fake_begin,
    fake_end
};

void test_ufile_functions(void)
{
    fake_fs fs;
    ufile_table t;
    char buf[64];
    int fd;
    int i;
    static struct ufile_backend no_begin;
    static struct ufile_backend bad;

    memset(&fs, 0, sizeof(fs));
    ufile_table_init(&t);
    CHECK(t.be == 0);
    CHECK(t.file[0].open == 0);

    fake_put(&fs, "/hello.txt", "hello from ext4");
    fd = ufile_open(&t, &g_backend, &fs, "/hello.txt", SYS_O_RDONLY, 0);
    CHECK(fd == (int)UFILE_FD_BASE);
    memset(buf, 0, sizeof(buf));
    CHECK(ufile_read(&t, (u32)fd, buf, sizeof(buf)) == 15);
    CHECK_STR(buf, "hello from ext4");
    CHECK(ufile_read(&t, (u32)fd, buf, sizeof(buf)) == 0);
    CHECK(ufile_write(&t, (u32)fd, "x", 1) == SYS_EBADF);
    CHECK(ufile_close(&t, (u32)fd) == 0);
    CHECK(t.file[0].open == 0);
    CHECK(ufile_close(&t, (u32)fd) == SYS_EBADF);

    /* A short read stops at the end of the snapshot. */
    fd = ufile_open(&t, &g_backend, &fs, "/hello.txt", SYS_O_RDONLY, 0);
    memset(buf, 0, sizeof(buf));
    CHECK(ufile_read(&t, (u32)fd, buf, 5) == 5);
    CHECK_STR(buf, "hello");
    CHECK(ufile_read(&t, (u32)fd, buf, 5) == 5);
    CHECK_STR(buf, " from");
    CHECK(ufile_read(&t, (u32)fd, buf, 5) == 5);
    CHECK_STR(buf, " ext4");
    CHECK(ufile_read(&t, (u32)fd, buf, 5) == 0);
    CHECK(ufile_close(&t, (u32)fd) == 0);

    /* Missing file, missing create flag, unknown flags. */
    CHECK(ufile_open(&t, &g_backend, &fs, "/nope", SYS_O_RDONLY, 0) == SYS_ENOENT);
    CHECK(ufile_open(&t, &g_backend, &fs, "/nope", SYS_O_WRONLY, 0) == SYS_ENOENT);
    CHECK(ufile_open(&t, &g_backend, &fs, "/nope",
                     SYS_O_WRONLY | 0x100u, 0) == SYS_EINVAL);
    CHECK(ufile_open(&t, &g_backend, &fs, "/nope",
                     SYS_O_CREAT, 0) == SYS_EINVAL);
    CHECK(ufile_open(&t, &g_backend, &fs, "", SYS_O_RDONLY, 0) == SYS_EINVAL);
    CHECK(ufile_open(&t, &g_backend, &fs, 0, SYS_O_RDONLY, 0) == SYS_EINVAL);
    {
        char longpath[UFILE_PATH_MAX + 2u];

        memset(longpath, 'a', sizeof(longpath) - 1u);
        longpath[sizeof(longpath) - 1u] = 0;
        CHECK(ufile_open(&t, &g_backend, &fs, longpath,
                         SYS_O_RDONLY, 0) == SYS_EINVAL);
    }

    /* Create on close: nothing touches the filesystem until the fd closes. */
    fd = ufile_open(&t, &g_backend, &fs, "/new.txt",
                    SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, 0644);
    CHECK(fd == (int)UFILE_FD_BASE);
    CHECK(fs.creates == 0);
    CHECK(ufile_write(&t, (u32)fd, "abc", 3) == 3);
    CHECK(ufile_read(&t, (u32)fd, buf, sizeof(buf)) == SYS_EBADF);
    CHECK(ufile_close(&t, (u32)fd) == 0);
    CHECK(fs.creates == 1);
    CHECK(fs.writes == 1);
    CHECK(fs.begins == 1);
    CHECK(fs.ends == 1);
    CHECK(fs.fsyncs == 1);
    CHECK_STR(fake_text(&fs, "/new.txt"), "abc");

    /* Overwrite in place keeps the old tail, O_TRUNC drops it. */
    fd = ufile_open(&t, &g_backend, &fs, "/new.txt", SYS_O_WRONLY, 0);
    CHECK(fd == (int)UFILE_FD_BASE);
    CHECK(ufile_write(&t, (u32)fd, "xy", 2) == 2);
    CHECK(ufile_close(&t, (u32)fd) == 0);
    CHECK_STR(fake_text(&fs, "/new.txt"), "xyc");

    fake_put(&fs, "/trunc.txt", "0123456789");
    fd = ufile_open(&t, &g_backend, &fs, "/trunc.txt",
                    SYS_O_WRONLY | SYS_O_TRUNC, 0);
    CHECK(fd == (int)UFILE_FD_BASE);
    CHECK(ufile_write(&t, (u32)fd, "z", 1) == 1);
    CHECK(ufile_close(&t, (u32)fd) == 0);
    CHECK_STR(fake_text(&fs, "/trunc.txt"), "z");

    /* An empty flush truncates instead of writing. */
    fake_put(&fs, "/empty.txt", "0123456789");
    fd = ufile_open(&t, &g_backend, &fs, "/empty.txt",
                    SYS_O_WRONLY | SYS_O_TRUNC, 0);
    CHECK(fd == (int)UFILE_FD_BASE);
    CHECK(ufile_close(&t, (u32)fd) == 0);
    CHECK(fake_text(&fs, "/empty.txt")[0] == 0);
    CHECK(fs.truncates == 1);

    /* A failed read-write remount fails the close and writes nothing. */
    fs.begin_fail = 1;
    fd = ufile_open(&t, &g_backend, &fs, "/blocked.txt",
                    SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, 0);
    CHECK(fd == (int)UFILE_FD_BASE);
    CHECK(ufile_write(&t, (u32)fd, "no", 2) == 2);
    CHECK(ufile_close(&t, (u32)fd) == SYS_EIO);
    CHECK(fake_find(&fs, "/blocked.txt") < 0);
    fs.begin_fail = 0;

    /* Slots: eight of them, then EBUSY, and a freed slot is reused. */
    ufile_table_init(&t);
    for (i = 0; i < (int)UFILE_FD_MAX; i++)
        CHECK(ufile_open(&t, &g_backend, &fs, "/hello.txt", SYS_O_RDONLY, 0) ==
              (int)UFILE_FD_BASE + i);
    CHECK(ufile_open(&t, &g_backend, &fs, "/hello.txt", SYS_O_RDONLY, 0) ==
          SYS_EBUSY);
    CHECK(ufile_read(&t, UFILE_FD_BASE + UFILE_FD_MAX, buf, 1) == SYS_EINVAL);
    CHECK(ufile_close(&t, UFILE_FD_BASE + 2u) == 0);
    CHECK(ufile_open(&t, &g_backend, &fs, "/hello.txt", SYS_O_RDONLY, 0) ==
          (int)(UFILE_FD_BASE + 2u));
    /* fd 0, 1, 2 and anything past the table belong to the console. */
    CHECK(ufile_read(&t, 0, buf, 1) == SYS_EINVAL);
    CHECK(ufile_read(&t, 2, buf, 1) == SYS_EINVAL);
    CHECK(ufile_close(&t, 2) == SYS_EBADF);
    CHECK(ufile_read(&t, (u32)UFILE_FD_BASE, 0, 1) == SYS_EINVAL);
    ufile_close_all(&t);
    CHECK(t.be == 0);
    CHECK(ufile_read(&t, (u32)UFILE_FD_BASE, buf, 1) == SYS_EINVAL);

    /* A file past the snapshot limit and a too-large write are refused. */
    fake_put(&fs, "/big.txt", "x");
    {
        int idx = fake_find(&fs, "/big.txt");

        fs.len[idx] = (u32)UFILE_MAX_BYTES + 1u;
    }
    CHECK(ufile_open(&t, &g_backend, &fs, "/big.txt", SYS_O_RDONLY, 0) == SYS_EIO);
    fd = ufile_open(&t, &g_backend, &fs, "/new.txt",
                    SYS_O_WRONLY | SYS_O_TRUNC, 0);
    CHECK(fd == (int)UFILE_FD_BASE);
    {
        static u8 huge[UFILE_MAX_BYTES + 1u];

        memset(huge, 'q', sizeof(huge));
        CHECK(ufile_write(&t, (u32)fd, huge, sizeof(huge)) == SYS_EIO);
    }
    CHECK(ufile_close(&t, (u32)fd) == 0);

    /* A backend missing a required hook is refused outright. */
    bad = g_backend;
    bad.lookup = 0;
    CHECK(ufile_open(&t, &bad, &fs, "/hello.txt", SYS_O_RDONLY, 0) == SYS_EINVAL);
    CHECK(ufile_open(&t, 0, &fs, "/hello.txt", SYS_O_RDONLY, 0) == SYS_EINVAL);
    /* The remount hooks are optional: a read-only flush path without them
       still writes and closes cleanly. */
    no_begin = g_backend;
    no_begin.begin_write = 0;
    no_begin.end_write = 0;
    fd = ufile_open(&t, &no_begin, &fs, "/plain.txt",
                    SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, 0);
    CHECK(fd == (int)UFILE_FD_BASE);
    CHECK(ufile_write(&t, (u32)fd, "ok", 2) == 2);
    CHECK(ufile_close(&t, (u32)fd) == 0);
    CHECK_STR(fake_text(&fs, "/plain.txt"), "ok");
}
