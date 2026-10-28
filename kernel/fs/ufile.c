/* CrugOS per-thread open file table (phase U3). See include/crug/ufile.h. */
#include <crug/ufile.h>
#include <crug/string.h>
#include <crug/syscall.h>

#ifndef CRUG_HOST_TEST
#include <crug/heap.h>
#define UFILE_ALLOC(n) kmalloc(n)
#define UFILE_FREE(p) kfree(p)
#else
/* The host unit tests provide the allocator, so this file needs neither the
   physical memory manager nor the kernel heap. */
void *ufile_host_alloc(usize n);
void ufile_host_free(void *p);
#define UFILE_ALLOC(n) ufile_host_alloc(n)
#define UFILE_FREE(p) ufile_host_free(p)
#endif

static int ufile_backend_ok(const struct ufile_backend *be)
{
    return be && be->lookup && be->read_file && be->create &&
           be->write_file && be->truncate && be->fsync;
}

static ufile *ufile_slot(ufile_table *t, u32 fd)
{
    ufile *f;

    if (!t || fd < UFILE_FD_BASE)
        return 0;
    fd -= UFILE_FD_BASE;
    if (fd >= UFILE_FD_MAX)
        return 0;
    f = &t->file[fd];
    return f->open ? f : 0;
}

static int ufile_probe_size(const struct ufile_backend *be, void *ctx,
                            const char *path, u64 *out_size)
{
    u8 one = 0;
    u64 size = 0;

    if (be->read_file(ctx, path, &one, 1, &size) != 0)
        return -1;
    *out_size = size;
    return 0;
}

void ufile_table_init(ufile_table *t)
{
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
}

int ufile_open(ufile_table *t, const struct ufile_backend *be, void *ctx,
               const char *path, u32 flags, u16 mode)
{
    ufile_table *table = t;
    struct ufile_backend *backend = (struct ufile_backend *)be;
    ufile *f = 0;
    u32 i;
    u32 slot;
    u32 ino;
    u32 known = (u32)(SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC);
    int wronly;
    int exists;
    u64 size = 0;

    (void)backend;
    if (!table || !ufile_backend_ok(be))
        return (int)SYS_EINVAL;
    if (!path || path[0] == 0)
        return (int)SYS_EINVAL;
    if (strlen(path) >= UFILE_PATH_MAX)
        return (int)SYS_EINVAL;
    if ((flags & ~known) != 0)
        return (int)SYS_EINVAL;
    wronly = (flags & (u32)SYS_O_WRONLY) != 0;
    if (!wronly && (flags & (u32)(SYS_O_CREAT | SYS_O_TRUNC)) != 0)
        return (int)SYS_EINVAL;
    for (i = 0; i < UFILE_FD_MAX; i++) {
        if (!table->file[i].open) {
            f = &table->file[i];
            break;
        }
    }
    if (!f)
        return (int)SYS_EBUSY;
    /* The fd is the slot index, so keep it apart from the inode lookup below:
       reusing the loop counter made the descriptor 3 + inode number. */
    slot = (u32)(f - table->file);
    memset(f, 0, sizeof(*f));
    strncpy(f->path, path, UFILE_PATH_MAX - 1u);
    f->flags = flags;
    f->mode = mode ? mode : 0644u;
    exists = be->lookup(ctx, f->path, &ino) == 0;
    if (!wronly) {
        if (!exists)
            return (int)SYS_ENOENT;
        if (ufile_probe_size(be, ctx, f->path, &size) != 0)
            return (int)SYS_EIO;
        if (size > UFILE_MAX_BYTES)
            return (int)SYS_EIO;
        if (size != 0) {
            f->data = (u8 *)UFILE_ALLOC((usize)size);
            if (!f->data)
                return (int)SYS_ENOMEM;
            if (be->read_file(ctx, f->path, f->data, (usize)size, &size) != 0) {
                UFILE_FREE(f->data);
                f->data = 0;
                return (int)SYS_EIO;
            }
        }
        f->len = (u32)size;
    } else {
        f->created = exists ? 0u : 1u;
        if (f->created && (flags & (u32)SYS_O_CREAT) == 0)
            return (int)SYS_ENOENT;
        f->data = (u8 *)UFILE_ALLOC(UFILE_MAX_BYTES);
        if (!f->data)
            return (int)SYS_ENOMEM;
        if (exists && (flags & (u32)SYS_O_TRUNC) == 0) {
            if (ufile_probe_size(be, ctx, f->path, &size) != 0 ||
                size > UFILE_MAX_BYTES) {
                UFILE_FREE(f->data);
                f->data = 0;
                return (int)SYS_EIO;
            }
            if (size != 0 &&
                be->read_file(ctx, f->path, f->data, (usize)size, &size) != 0) {
                UFILE_FREE(f->data);
                f->data = 0;
                return (int)SYS_EIO;
            }
            f->len = (u32)size;
        }
    }
    table->be = be;
    table->be_ctx = ctx;
    f->open = 1u;
    return (int)(UFILE_FD_BASE + slot);
}

i64 ufile_read(ufile_table *t, u32 fd, void *buf, u32 len)
{
    ufile *f = ufile_slot(t, fd);
    u32 avail;

    if (!f || !buf)
        return SYS_EINVAL;
    if (f->flags & (u32)SYS_O_WRONLY)
        return SYS_EBADF;
    if (f->off >= f->len)
        return 0;
    avail = f->len - f->off;
    if (len > avail)
        len = avail;
    memcpy(buf, f->data + f->off, (usize)len);
    f->off += len;
    return (i64)len;
}

i64 ufile_write(ufile_table *t, u32 fd, const void *buf, u32 len)
{
    ufile *f = ufile_slot(t, fd);

    if (!f || !buf)
        return SYS_EINVAL;
    if ((f->flags & (u32)SYS_O_WRONLY) == 0)
        return SYS_EBADF;
    if ((u64)f->off + (u64)len > UFILE_MAX_BYTES)
        return SYS_EIO;
    memcpy(f->data + f->off, buf, (usize)len);
    f->off += len;
    if (f->off > f->len)
        f->len = f->off;
    return (i64)len;
}

static int ufile_flush(ufile_table *t, ufile *f)
{
    const struct ufile_backend *be = t->be;
    void *ctx = t->be_ctx;
    int ret = 0;

    if (!be)
        return (int)SYS_EIO;
    /* The boot mounts the root read-only, so the flush asks for a short
       read-write window around the block writes and gives it back after. */
    if (be->begin_write && be->begin_write(ctx) != 0)
        return (int)SYS_EIO;
    if (f->created && be->create(ctx, f->path, f->mode) != 0)
        ret = (int)SYS_EIO;
    if (ret == 0) {
        if (f->len != 0) {
            if (be->write_file(ctx, f->path, f->data, (usize)f->len) != 0)
                ret = (int)SYS_EIO;
        } else if (be->truncate(ctx, f->path, 0) != 0) {
            ret = (int)SYS_EIO;
        }
    }
    if (ret == 0 && be->fsync(ctx) != 0)
        ret = (int)SYS_EIO;
    if (be->end_write)
        (void)be->end_write(ctx);
    return ret;
}

int ufile_close(ufile_table *t, u32 fd)
{
    ufile *f = ufile_slot(t, fd);
    int ret = 0;

    if (!f)
        return (int)SYS_EBADF;
    if (f->flags & (u32)SYS_O_WRONLY)
        ret = ufile_flush(t, f);
    if (f->data)
        UFILE_FREE(f->data);
    memset(f, 0, sizeof(*f));
    return ret;
}

void ufile_close_all(ufile_table *t)
{
    u32 i;

    if (!t)
        return;
    for (i = 0; i < UFILE_FD_MAX; i++) {
        if (t->file[i].open)
            (void)ufile_close(t, UFILE_FD_BASE + i);
    }
    t->be = 0;
    t->be_ctx = 0;
}