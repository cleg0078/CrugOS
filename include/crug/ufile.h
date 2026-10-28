/* CrugOS per-thread open file table (phase U3).
 *
 * A ring-3 program sees small integers: fd 0 is stdin (EOF), 1 and 2 are the
 * console, and 3..10 are slots in this table. Reads snapshot the file when it
 * is opened (the shell only handles small files); writes are buffered and
 * flushed on close, so a failed flush is reported as a real error instead of
 * being lost in the middle of a syscall.
 *
 * The table is pure data: the filesystem arrives through struct ufile_backend,
 * which keeps the structure host-testable without a disk. */
#ifndef CRUG_UFILE_H
#define CRUG_UFILE_H

#include <crug/types.h>

#define UFILE_FD_MAX 8u
#define UFILE_FD_BASE 3u
#define UFILE_MAX_BYTES (64u * 1024u)
#define UFILE_PATH_MAX 64u

/* The subset of the filesystem the table needs. Every hook returns 0 on
   success and a negative value on failure, like the ext4 API. */
struct ufile_backend {
    int (*lookup)(void *ctx, const char *path, u32 *out_ino);
    int (*read_file)(void *ctx, const char *path, void *buf, usize cap,
                     u64 *out_size);
    int (*create)(void *ctx, const char *path, u16 mode);
    int (*write_file)(void *ctx, const char *path, const void *buf, usize len);
    int (*truncate)(void *ctx, const char *path, u64 size);
    int (*fsync)(void *ctx);
    /* Optional: bracket the block writes of a flush. The boot leaves the root
       mounted read-only, so the kernel backend maps these to a short
       read-write remount instead of a second mount of the same volume. */
    int (*begin_write)(void *ctx);
    int (*end_write)(void *ctx);
};

typedef struct ufile {
    u8 *data;
    u32 len;
    u32 off;
    u32 flags;
    u16 mode;
    u8 open;
    u8 created;      /* the file did not exist at open: create it on close */
    char path[UFILE_PATH_MAX];
} ufile;

typedef struct ufile_table {
    const struct ufile_backend *be;
    void *be_ctx;
    ufile file[UFILE_FD_MAX];
} ufile_table;

void ufile_table_init(ufile_table *t);

/* Returns an fd (3..10) or SYS_E* (negative). */
int ufile_open(ufile_table *t, const struct ufile_backend *be, void *ctx,
               const char *path, u32 flags, u16 mode);

/* Returns the number of bytes handled, 0 at end of file, or SYS_E*. */
i64 ufile_read(ufile_table *t, u32 fd, void *buf, u32 len);
i64 ufile_write(ufile_table *t, u32 fd, const void *buf, u32 len);

/* Flushes a write slot and releases it. Returns 0 or SYS_E*. */
int ufile_close(ufile_table *t, u32 fd);

/* Called from thread_destroy: every slot is flushed and released. */
void ufile_close_all(ufile_table *t);

#endif /* CRUG_UFILE_H */