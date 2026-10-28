/* shell_ops: the kernel side of struct shell_io, over the mounted root
   filesystem, the heap counters and the scheduler.  Errors are reported to
   the caller (the shell prints them) instead of panicking: a shell command
   must never take the machine down. */
#include <crug/types.h>
#include <crug/shell.h>
#include <crug/storage.h>
#include <crug/heap.h>
#include <crug/thread.h>
#include <crug/timer.h>

static int ops_list(const char *path, char *out, usize cap)
{
    struct ext4_fs *fs = storage_root();

    if (!fs || !out || cap == 0)
        return -1;
    return ext4_list(fs, path, out, cap);
}

static int ops_read(const char *path, char *out, usize cap, u64 *out_len)
{
    struct ext4_fs *fs = storage_root();
    u64 size = 0;

    if (!fs || !out_len)
        return -1;
    if (ext4_read_file(fs, path, out, cap, &size) != 0)
        return -1;
    /* ext4_read_file reports the full size but truncates at cap. */
    *out_len = size > cap ? (u64)cap : size;
    return 0;
}

static int ops_write(const char *path, const char *data, usize len)
{
    struct ext4_fs *fs = storage_root();
    u32 ino;
    int ret;

    if (!fs)
        return -1;
    /* The boot mounts the root read-only: take a short read-write window and
       close it again so a shell command cannot leave the volume mounted for
       writing. */
    if (storage_remount_rw() != 0)
        return -1;
    if (ext4_lookup(fs, path, &ino) != 0) {
        if (ext4_create(fs, path, 0644u) != 0) {
            (void)storage_remount_ro();
            return -1;
        }
    }
    ret = ext4_write_file(fs, path, data, len);
    if (storage_remount_ro() != 0)
        ret = -1;
    return ret;
}

static int ops_sync(void)
{
    struct ext4_fs *fs = storage_root();

    if (!fs)
        return -1;
    /* Every write already closes its remount (which flushes), so sync is only
       meaningful while the volume happens to be writable. */
    if (!(fs->flags & EXT4_MOUNT_RW))
        return 0;
    return ext4_fsync(fs);
}

static void ops_info(struct shell_info *out)
{
    thread *cur;

    if (!out)
        return;
    cur = sched_current();
    out->uptime_ns = timer_monotonic_ns();
    out->heap_used = heap_used_bytes();
    out->heap_total = heap_total_bytes();
    out->cur_thread = cur ? thread_name(cur) : "?";
    out->switches = sched_switches();
}

static const struct shell_io g_shell_io = {
    ops_list,
    ops_read,
    ops_write,
    ops_sync,
    ops_info,
};

const struct shell_io *shell_kernel_io(void)
{
    return &g_shell_io;
}