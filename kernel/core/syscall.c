/* CrugOS phase 2 syscall dispatcher. */
#include <crug/syscall.h>
#include <crug/harden.h>
#include <crug/thread.h>
#include <crug/process.h>
#include <crug/msgport.h>
#include <crug/heap.h>
#include <crug/kernel.h>
#include <crug/string.h>
#include <crug/net.h>
#include <crug/uaccess.h>
#include <crug/ext4.h>
#include <crug/storage.h>
#include <crug/exec.h>
#include <crug/app.h>
#include <crug/caps.h>
#include <crug/perms.h>
#include <crug/pkg.h>

static i64 syscall_port_create(void)
{
    msgport *p;
    u64 h;
    handle_table *table;

    table = process_handles(process_current());
    if (!table)
        return SYS_EINVAL;
    p = msgport_create();
    if (!p)
        return SYS_ENOMEM;
    h = handle_alloc(table, HANDLE_PORT, p, HANDLE_RIGHT_ALL);
    if (h == HANDLE_NONE) {
        msgport_destroy(p);
        return SYS_ENOMEM;
    }
    return (i64)h;
}


static i64 syscall_socket_handle(handle_table *table, struct net_tcp_socket *s)
{
    u64 h;

    if (!table || !s)
        return SYS_ENOMEM;
    h = handle_alloc(table, HANDLE_SOCKET, s, HANDLE_RIGHT_ALL);
    if (h == HANDLE_NONE) {
        tcp_socket_release(s);
        return SYS_ENOMEM;
    }
    return (i64)h;
}

/* Phase U1: the same dispatcher serves CPL0 selftests and CPL3 programs. A
   user call never dereferences an argument directly: every pointer is range
   checked and copied through a kernel bounce buffer. */
/* Phase 7B: every user-originated request carries the caller's capability
   mask.  Kernel threads hold CAP_ALL, so a CPL0 selftest never trips this. */
static int syscall_caps_ok(int user, u32 need)
{
    thread *cur;

    if (!user)
        return 1;
    cur = sched_current();
    if (!cur)
        return 0;
    return (cur->caps & need) == need;
}

/* A read-only open needs CAP_FS_READ; creating, truncating or opening for
   write additionally needs CAP_FS_WRITE. */
static u32 syscall_open_need(u32 flags)
{
    u32 need = CAP_FS_READ;

    if ((flags & (u32)(SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC)) != 0)
        need |= CAP_FS_WRITE;
    return need;
}

#define SYS_BOUNCE_MAX MSGPORT_MAX_PAYLOAD
#define SYS_USER_IO_MAX 4096u
#define SYS_WRITE_CHUNK 256u
#define SYS_USER_NAME_MAX 128u

static int from_user(const trap_frame *frame)
{
    return (frame->cs & 3u) == 3u;
}

/* SYS_WRITE(1|2, buf, len): console output for ring-3 programs. */
static i64 syscall_write(const trap_frame *frame)
{
    u64 fd = frame->rdi;
    u64 uaddr = frame->rsi;
    u64 len = frame->rdx;
    u64 done = 0;
    char chunk[SYS_WRITE_CHUNK + 1u];
    int user = from_user(frame);

    if (fd != 1u && fd != 2u)
        return SYS_EBADF;
    if (len > SYS_USER_IO_MAX)
        len = SYS_USER_IO_MAX;
    if (len == 0)
        return 0;
    if (uaddr == 0)
        return SYS_EFAULT;
    if (user && uaccess_check(uaddr, len) != 0)
        return SYS_EFAULT;
    if (!user && !harden_range_ok(uaddr, len, HARDEN_IDENTITY_LIMIT))
        return SYS_EFAULT;
    while (done < len) {
        u64 n = len - done;

        if (n > SYS_WRITE_CHUNK)
            n = SYS_WRITE_CHUNK;
        if (user) {
            if (uaccess_user_read(chunk, uaddr + done, n) != 0)
                return SYS_EFAULT;
        } else {
            memcpy(chunk, (const void *)(uintptr)(uaddr + done), (usize)n);
        }
        chunk[n] = 0;
        kprintf("%s", chunk);
        done += n;
    }
    return (i64)done;
}

/* Phase U3: the fd table lives on the thread and reaches ext4 through this
   backend, which keeps kernel/fs/ufile.c free of filesystem headers. */
static int ufile_be_lookup(void *ctx, const char *path, u32 *ino)
{
    return ext4_lookup((struct ext4_fs *)ctx, path, ino);
}

static int ufile_be_read(void *ctx, const char *path, void *buf, usize cap,
                         u64 *size)
{
    return ext4_read_file((struct ext4_fs *)ctx, path, buf, cap, size);
}

static int ufile_be_create(void *ctx, const char *path, u16 mode)
{
    return ext4_create((struct ext4_fs *)ctx, path, mode);
}

static int ufile_be_write(void *ctx, const char *path, const void *buf,
                          usize len)
{
    return ext4_write_file((struct ext4_fs *)ctx, path, buf, len);
}

static int ufile_be_truncate(void *ctx, const char *path, u64 size)
{
    return ext4_truncate((struct ext4_fs *)ctx, path, size);
}

static int ufile_be_fsync(void *ctx)
{
    return ext4_fsync((struct ext4_fs *)ctx);
}

/* The boot leaves the root mounted read-only, so a flush asks for a short
   read-write window and hands the volume back afterwards. */
static int ufile_be_begin_write(void *ctx)
{
    (void)ctx;
    return storage_remount_rw();
}

static int ufile_be_end_write(void *ctx)
{
    (void)ctx;
    return storage_remount_ro();
}

static const struct ufile_backend g_ufile_backend = {
    ufile_be_lookup,
    ufile_be_read,
    ufile_be_create,
    ufile_be_write,
    ufile_be_truncate,
    ufile_be_fsync,
    ufile_be_begin_write,
    ufile_be_end_write
};

/* Shared bounce buffers: one syscall is in flight at a time per CPU. */
static u8 g_io_buf[SYS_USER_IO_MAX];
static char g_path_buf[SYS_USER_NAME_MAX];
/* Phase U4: the string area for a spawn or exec argv vector. */
static char g_argv_buf[EXEC_ARGS_STR_MAX];

/* Copies a user path into the kernel buffer, or hands back the CPL0 pointer. */
static const char *syscall_path(const trap_frame *frame, int user, u64 uaddr)
{
    if (user) {
        if (uaccess_user_str(g_path_buf, uaddr, sizeof(g_path_buf)) != 0)
            return 0;
        return g_path_buf;
    }
    if (uaddr == 0 || !harden_range_ok(uaddr, 1, HARDEN_IDENTITY_LIMIT))
        return 0;
    return (const char *)(uintptr)uaddr;
}

/* SYS_WRITE(3..10, buf, len): append to an open file through ufile. */
static i64 syscall_file_write(const trap_frame *frame, int user)
{
    thread *cur = sched_current();
    u64 len = frame->rdx;
    u64 done = 0;

    if (!cur)
        return SYS_EBADF;
    if (len > SYS_USER_IO_MAX)
        len = SYS_USER_IO_MAX;
    if (len == 0)
        return 0;
    if (frame->rsi == 0)
        return SYS_EFAULT;
    if (user) {
        while (done < len) {
            u64 n = len - done;
            i64 put;

            if (n > SYS_WRITE_CHUNK)
                n = SYS_WRITE_CHUNK;
            if (uaccess_user_read(g_io_buf, frame->rsi + done, n) != 0)
                return SYS_EFAULT;
            put = ufile_write(&cur->files, (u32)frame->rdi, g_io_buf, (u32)n);
            if (put < 0)
                return put;
            done += n;
        }
        return (i64)done;
    }
    if (!harden_range_ok(frame->rsi, len, HARDEN_IDENTITY_LIMIT))
        return SYS_EFAULT;
    return ufile_write(&cur->files, (u32)frame->rdi,
                       (const void *)(uintptr)frame->rsi, (u32)len);
}

/* Phase U4: copies an argv vector (a user or kernel array of string
   pointers) into the kernel string buffer and fills out[] with pointers
   inside it. The caller keeps strbuf alive for the following call. */
static int syscall_argv(int user, u64 uargv, u32 argc, char *strbuf,
                        usize strcap, const char **out)
{
    u64 ptrs[EXEC_ARGS_MAX];
    usize used = 0;
    u32 i;

    if (argc == 0)
        return 0;
    if (argc > EXEC_ARGS_MAX || uargv == 0)
        return -1;
    if (user) {
        if (uaccess_check(uargv, (u64)argc * sizeof(u64)) != 0)
            return -1;
        if (uaccess_user_read(ptrs, uargv, (usize)argc * sizeof(u64)) != 0)
            return -1;
    } else {
        if (!harden_range_ok(uargv, (u64)argc * sizeof(u64),
                             HARDEN_IDENTITY_LIMIT))
            return -1;
        memcpy(ptrs, (const void *)(uintptr)uargv, (usize)argc * sizeof(u64));
    }
    for (i = 0; i < argc; i++) {
        usize room = used < strcap ? strcap - used : 0;
        usize n;

        if (room < 2)
            return -1;
        if (user) {
            if (uaccess_user_str(&strbuf[used], ptrs[i], (u64)room) != 0)
                return -1;
        } else {
            const char *src;

            if (!harden_range_ok(ptrs[i], 1, HARDEN_IDENTITY_LIMIT))
                return -1;
            src = (const char *)(uintptr)ptrs[i];
            n = strlen(src);
            if (n + 1u > room)
                return -1;
            memcpy(&strbuf[used], src, n + 1u);
        }
        out[i] = &strbuf[used];
        used += strlen(&strbuf[used]) + 1u;
    }
    if (!exec_args_ok(argc, (u64)used))
        return -1;
    return 0;
}
void syscall_dispatch(trap_frame *frame)
{
    i64 ret;
    u64 nr = frame->rax;
    int user = from_user(frame);

    if (nr >= SYS_MAX) {
        frame->rax = (u64)SYS_ENOSYS;
        return;
    }

    switch (nr) {
    case SYS_PING:
        ret = (i64)SYS_PING_MAGIC;
        break;
    case SYS_GETTID: {
        thread *t = sched_current();

        ret = t ? (i64)t->id : -1;
        break;
    }
    case SYS_YIELD:
        sched_yield();
        ret = 0;
        break;
    case SYS_SLEEP_TICKS:
        sched_sleep_ticks(frame->rdi);
        ret = 0;
        break;
    case SYS_PORT_CREATE:
        ret = syscall_port_create();
        break;
    case SYS_PORT_SEND: {
        handle_table *table = process_handles(process_current());
        msgport *p = (msgport *)handle_get(table, frame->rdi, HANDLE_PORT);
        u64 len = frame->r10;
        char bounce[SYS_BOUNCE_MAX];

        if (!p) {
            ret = SYS_EBADF;
            break;
        }
        if (len > SYS_BOUNCE_MAX) {
            ret = SYS_EINVAL;
            break;
        }
        if (!user) {
            if (!harden_range_ok(frame->rdx, len, HARDEN_IDENTITY_LIMIT)) {
                ret = SYS_EFAULT;
                break;
            }
            ret = msgport_send(p, frame->rsi, (const void *)(uintptr)frame->rdx,
                               (usize)len);
            break;
        }
        if (uaccess_user_read(bounce, frame->rdx, len) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        ret = msgport_send(p, frame->rsi, bounce, (usize)len);
        break;
    }
    case SYS_PORT_RECV: {
        handle_table *table = process_handles(process_current());
        msgport *p = (msgport *)handle_get(table, frame->rdi, HANDLE_PORT);
        u64 len = frame->r10;
        u64 type = 0;
        char bounce[SYS_BOUNCE_MAX];
        i64 n;

        if (!p) {
            ret = SYS_EBADF;
            break;
        }
        if (!user) {
            if (!harden_range_ok(frame->rdx, len, HARDEN_IDENTITY_LIMIT) ||
                (frame->rsi != 0 &&
                 !harden_range_ok(frame->rsi, sizeof(u64), HARDEN_IDENTITY_LIMIT))) {
                ret = SYS_EFAULT;
                break;
            }
            n = msgport_recv(p, &type, (void *)(uintptr)frame->rdx, (usize)len);
            if (n >= 0 && frame->rsi)
                *(u64 *)(uintptr)frame->rsi = type;
            ret = n;
            break;
        }
        if (len > SYS_BOUNCE_MAX) {
            ret = SYS_EINVAL;
            break;
        }
        if (uaccess_check(frame->rdx, len) != 0 ||
            (frame->rsi != 0 && uaccess_check(frame->rsi, sizeof(u64)) != 0)) {
            ret = SYS_EFAULT;
            break;
        }
        n = msgport_recv(p, &type, bounce, (usize)len);
        if (n > 0 && uaccess_user_write(frame->rdx, bounce, (u64)n) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (n >= 0 && frame->rsi != 0 &&
            uaccess_user_write(frame->rsi, &type, sizeof(u64)) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        ret = n;
        break;
    }
    case SYS_HANDLE_CLOSE: {
        handle_table *table = process_handles(process_current());
        struct net_tcp_socket *s = (struct net_tcp_socket *)handle_get(
            table, frame->rdi, HANDLE_SOCKET);

        if (s) {
            tcp_close(s, 500u);
            tcp_socket_release(s);
        }
        ret = handle_close(table, frame->rdi) == 0 ? 0 : SYS_EBADF;
        break;
    }
    case SYS_SOCKET_CONNECT: {
        if (!syscall_caps_ok(user, CAP_NET)) {
            ret = SYS_EPERM;
            break;
        }
        handle_table *table = process_handles(process_current());
        struct net_tcp_socket *s = tcp_socket_new();

        if (!table || !s) {
            ret = SYS_ENOMEM;
            break;
        }
        if (tcp_connect(s, (u32)frame->rdi, (u16)frame->rsi, 3000u) != 0) {
            tcp_socket_release(s);
            ret = SYS_EIO;
            break;
        }
        ret = syscall_socket_handle(table, s);
        break;
    }
    case SYS_SOCKET_LISTEN: {
        if (!syscall_caps_ok(user, CAP_NET)) {
            ret = SYS_EPERM;
            break;
        }
        handle_table *table = process_handles(process_current());
        struct net_tcp_socket *s = tcp_socket_new();

        if (!table || !s) {
            ret = SYS_ENOMEM;
            break;
        }
        if (tcp_listen(s, (u16)frame->rdi) != 0) {
            tcp_socket_release(s);
            ret = SYS_EINVAL;
            break;
        }
        ret = syscall_socket_handle(table, s);
        break;
    }
    case SYS_SOCKET_SEND: {
        if (!syscall_caps_ok(user, CAP_NET)) {
            ret = SYS_EPERM;
            break;
        }
        handle_table *table = process_handles(process_current());
        struct net_tcp_socket *s = (struct net_tcp_socket *)handle_get(
            table, frame->rdi, HANDLE_SOCKET);
        char bounce[SYS_USER_IO_MAX];
        u64 len = frame->rdx;
        int n;

        if (!s) {
            ret = SYS_EBADF;
            break;
        }
        if (!user) {
            n = tcp_send(s, (const void *)(uintptr)frame->rsi, (usize)len);
            ret = n < 0 ? SYS_EINVAL : (i64)n;
            break;
        }
        if (len > SYS_USER_IO_MAX) {
            ret = SYS_EINVAL;
            break;
        }
        if (uaccess_user_read(bounce, frame->rsi, len) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        n = tcp_send(s, bounce, (usize)len);
        ret = n < 0 ? SYS_EINVAL : (i64)n;
        break;
    }
    case SYS_SOCKET_RECV: {
        if (!syscall_caps_ok(user, CAP_NET)) {
            ret = SYS_EPERM;
            break;
        }
        handle_table *table = process_handles(process_current());
        struct net_tcp_socket *s = (struct net_tcp_socket *)handle_get(
            table, frame->rdi, HANDLE_SOCKET);
        char bounce[SYS_USER_IO_MAX];
        u64 len = frame->rdx;
        int n;

        if (!s) {
            ret = SYS_EBADF;
            break;
        }
        if (!user) {
            n = tcp_recv(s, (void *)(uintptr)frame->rsi, (usize)len, 3000u);
            ret = n < 0 ? SYS_EAGAIN : (i64)n;
            break;
        }
        if (len > SYS_USER_IO_MAX) {
            ret = SYS_EINVAL;
            break;
        }
        if (uaccess_check(frame->rsi, len) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        n = tcp_recv(s, bounce, (usize)len, 3000u);
        if (n < 0) {
            ret = SYS_EAGAIN;
            break;
        }
        if (n > 0 && uaccess_user_write(frame->rsi, bounce, (u64)n) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        ret = (i64)n;
        break;
    }
    case SYS_SOCKET_CLOSE: {
        if (!syscall_caps_ok(user, CAP_NET)) {
            ret = SYS_EPERM;
            break;
        }
        handle_table *table = process_handles(process_current());
        struct net_tcp_socket *s = (struct net_tcp_socket *)handle_get(
            table, frame->rdi, HANDLE_SOCKET);

        if (!s) {
            ret = SYS_EBADF;
            break;
        }
        tcp_close(s, 500u);
        tcp_socket_release(s);
        ret = handle_close(table, frame->rdi) == 0 ? 0 : SYS_EBADF;
        break;
    }
    case SYS_SOCKET_ACCEPT: {
        if (!syscall_caps_ok(user, CAP_NET)) {
            ret = SYS_EPERM;
            break;
        }
        handle_table *table = process_handles(process_current());
        struct net_tcp_socket *ls = (struct net_tcp_socket *)handle_get(
            table, frame->rdi, HANDLE_SOCKET);
        struct net_tcp_socket *s;

        if (!ls) {
            ret = SYS_EBADF;
            break;
        }
        s = tcp_accept(ls, 5000u);
        if (!s) {
            ret = SYS_EAGAIN;
            break;
        }
        ret = syscall_socket_handle(table, s);
        break;
    }
    case SYS_RESOLVE: {
        if (!syscall_caps_ok(user, CAP_NET)) {
            ret = SYS_EPERM;
            break;
        }
        struct net_dns_answer answer;
        char namebuf[SYS_USER_NAME_MAX];
        const char *name;

        if (user) {
            if (uaccess_user_str(namebuf, frame->rdi, sizeof(namebuf)) != 0 ||
                frame->rsi == 0 || uaccess_check(frame->rsi, sizeof(u32)) != 0) {
                ret = SYS_EFAULT;
                break;
            }
            name = namebuf;
        } else {
            name = (const char *)(uintptr)frame->rdi;
            if (!name || !frame->rsi) {
                ret = SYS_EINVAL;
                break;
            }
        }
        if (net_dns_resolve(name, &answer, 3000u) != 0 || answer.count == 0u) {
            ret = SYS_EIO;
            break;
        }
        if (user) {
            if (uaccess_user_write(frame->rsi, &answer.addr[0], sizeof(u32)) != 0) {
                ret = SYS_EFAULT;
                break;
            }
        } else {
            *(u32 *)(uintptr)frame->rsi = answer.addr[0];
        }
        ret = 0;
        break;
    }
    case SYS_OPEN: {
        thread *cur = sched_current();
        struct ext4_fs *fs = storage_root();
        const char *path;

        if (!cur || !fs) {
            ret = SYS_EIO;
            break;
        }
        path = syscall_path(frame, user, frame->rdi);
        if (!path || path[0] == 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (!syscall_caps_ok(user, syscall_open_need((u32)frame->rsi))) {
            ret = SYS_EPERM;
            break;
        }
        ret = (i64)ufile_open(&cur->files, &g_ufile_backend, fs, path,
                              (u32)frame->rsi, (u16)frame->rdx);
        break;
    }
    case SYS_READ: {
        thread *cur = sched_current();
        u64 fd = frame->rdi;
        u64 uaddr = frame->rsi;
        u64 len = frame->rdx;
        i64 got;

        if (len > SYS_USER_IO_MAX)
            len = SYS_USER_IO_MAX;
        if (len == 0 || fd == 0u) {
            ret = 0;
            break;
        }
        if (!cur) {
            ret = SYS_EBADF;
            break;
        }
        if (uaddr == 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (user && uaccess_check(uaddr, len) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (!user && !harden_range_ok(uaddr, len, HARDEN_IDENTITY_LIMIT)) {
            ret = SYS_EFAULT;
            break;
        }
        got = ufile_read(&cur->files, (u32)fd, g_io_buf, (u32)len);
        if (got < 0) {
            ret = got;
            break;
        }
        if (user) {
            if (uaccess_user_write(uaddr, g_io_buf, (usize)got) != 0) {
                ret = SYS_EFAULT;
                break;
            }
        } else {
            memcpy((void *)(uintptr)uaddr, g_io_buf, (usize)got);
        }
        ret = got;
        break;
    }
    case SYS_CLOSE: {
        thread *cur = sched_current();
        u64 fd = frame->rdi;

        if (fd <= 2u) {
            ret = 0;
            break;
        }
        if (!cur) {
            ret = SYS_EBADF;
            break;
        }
        ret = (i64)ufile_close(&cur->files, (u32)fd);
        break;
    }
    case SYS_LISTDIR: {
        struct ext4_fs *fs = storage_root();
        const char *path;
        u64 cap = frame->rdx;
        usize used;
        int n;

        if (!fs) {
            ret = SYS_EIO;
            break;
        }
        if (!syscall_caps_ok(user, CAP_FS_READ)) {
            ret = SYS_EPERM;
            break;
        }
        path = syscall_path(frame, user, frame->rdi);
        if (!path) {
            ret = SYS_EFAULT;
            break;
        }
        if (frame->rsi == 0 || cap == 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (cap > SYS_USER_IO_MAX)
            cap = SYS_USER_IO_MAX;
        if (user && uaccess_check(frame->rsi, cap) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (!user && !harden_range_ok(frame->rsi, cap, HARDEN_IDENTITY_LIMIT)) {
            ret = SYS_EFAULT;
            break;
        }
        n = ext4_list(fs, path, (char *)g_io_buf, (usize)cap);
        if (n < 0) {
            ret = SYS_EIO;
            break;
        }
        used = (usize)n + 1u;
        if (used > (usize)cap)
            used = (usize)cap;
        if (user) {
            if (uaccess_user_write(frame->rsi, g_io_buf, used) != 0) {
                ret = SYS_EFAULT;
                break;
            }
        } else {
            memcpy((void *)(uintptr)frame->rsi, g_io_buf, used);
        }
        ret = (i64)n;
        break;
    }
    case SYS_SPAWN: {
        thread *cur = sched_current();
        const char *path;
        const char *av[EXEC_ARGS_MAX];
        char pathbuf[SYS_USER_NAME_MAX];
        u32 argc = (u32)frame->rsi;
        elf_exec_result res;
        usize plen;

        if (!cur) {
            ret = SYS_EINVAL;
            break;
        }
        path = syscall_path(frame, user, frame->rdi);
        if (!path || path[0] == 0) {
            ret = SYS_EFAULT;
            break;
        }
        plen = strlen(path);
        if (plen + 1u > sizeof(pathbuf)) {
            ret = SYS_EINVAL;
            break;
        }
        memcpy(pathbuf, path, plen + 1u);
        if (argc > EXEC_ARGS_MAX) {
            ret = SYS_EINVAL;
            break;
        }
        memset(av, 0, sizeof(av));
        if (syscall_argv(user, frame->rdx, argc, g_argv_buf,
                         sizeof(g_argv_buf), av) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        memset(&res, 0, sizeof(res));
        if (!elf_spawn_user(pathbuf, argc, av, cur->id, &res)) {
            ret = SYS_EIO;
            break;
        }
        ret = (i64)res.tid;
        break;
    }
    case SYS_WAIT: {
        thread *cur = sched_current();
        thread *child;

        if (!cur) {
            ret = SYS_ESRCH;
            break;
        }
        child = thread_find((u32)frame->rdi);
        /* Only the thread that spawned a child may wait for it. */
        if (!child || child->parent_id != cur->id || child == cur) {
            ret = SYS_ESRCH;
            break;
        }
        ret = thread_join_code(child);
        break;
    }
    case SYS_EXEC: {
        thread *cur = sched_current();
        const char *path;
        const char *av[EXEC_ARGS_MAX];
        char pathbuf[SYS_USER_NAME_MAX];
        u32 argc = (u32)frame->rsi;
        u64 entry = 0;
        u64 rsp = 0;
        usize plen;

        /* Exec is a user-image operation: it rewrites the trap frame that
           leads back to CPL3, so a CPL0 caller has nothing to rewrite. */
        if (!user || !cur) {
            ret = SYS_EINVAL;
            break;
        }
        path = syscall_path(frame, user, frame->rdi);
        if (!path || path[0] == 0) {
            ret = SYS_EFAULT;
            break;
        }
        plen = strlen(path);
        if (plen + 1u > sizeof(pathbuf)) {
            ret = SYS_EINVAL;
            break;
        }
        memcpy(pathbuf, path, plen + 1u);
        if (argc > EXEC_ARGS_MAX) {
            ret = SYS_EINVAL;
            break;
        }
        memset(av, 0, sizeof(av));
        if (syscall_argv(user, frame->rdx, argc, g_argv_buf,
                         sizeof(g_argv_buf), av) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (elf_exec_in_place(pathbuf, argc, av, &entry, &rsp) != 0) {
            ret = SYS_EIO;
            break;
        }
        /* Return straight into the new image: the interrupt epilogue pops
           rip/rsp from the frame we just rewrote. */
        frame->rip = entry;
        frame->rsp = rsp;
        frame->rax = 0;
        kprintf("CRUGOS: user exec %s (tid %u)\n", pathbuf, cur->id);
        return;
    }
    case SYS_FORK: {
        thread *cur = sched_current();
        vm_space *space;
        trap_frame child_frame;
        thread *child;

        /* Fork is a user-image operation: the child resumes at CPL3 at the
           instruction after this int 0x80, so a CPL0 caller has no frame to
           copy and no address space to duplicate. */
        if (!user || !cur || (cur->flags & THREAD_FLAG_USER) == 0 ||
            !cur->space) {
            ret = SYS_EINVAL;
            break;
        }
        space = (vm_space *)kmalloc(sizeof(vm_space));
        if (!space) {
            ret = SYS_ENOMEM;
            break;
        }
        if (vm_space_clone(space, cur->space) != 0) {
            kfree(space);
            ret = SYS_ENOMEM;
            break;
        }
        child_frame = *frame;
        child = thread_create_forked(cur->name, space, &child_frame, cur->id);
        if (!child) {
            vm_space_destroy(space);
            kfree(space);
            ret = SYS_ENOMEM;
            break;
        }
        child->caps = cur->caps;
        ret = (i64)child->id;
        /* The parent's own TLB may still hold the writable translation of a
           leaf this clone just write-protected (phase U6): reload CR3 before
           returning to ring 3, or the parent's next store would bypass the
           copy. */
        vm_flush_all();
        kprintf("CRUGOS: user fork tid %u -> %u (%u page(s) shared, COW)\n",
                cur->id, child->id, space->user_pages);
        break;
    }
    case SYS_APP_COUNT:
        ret = (i64)app_count();
        break;
    case SYS_APP_INFO: {
        const struct app *a = app_at((u32)frame->rdi);
        u64 cap = frame->rdx;
        int n;

        if (!a) {
            ret = SYS_ENOENT;
            break;
        }
        if (frame->rsi == 0 || cap == 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (cap > SYS_APP_INFO_MAX)
            cap = SYS_APP_INFO_MAX;
        if (user && uaccess_check(frame->rsi, cap) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (!user && !harden_range_ok(frame->rsi, cap, HARDEN_IDENTITY_LIMIT)) {
            ret = SYS_EFAULT;
            break;
        }
        n = app_info(a, (char *)g_io_buf, (usize)cap);
        if (n < 0) {
            ret = SYS_EINVAL;
            break;
        }
        if (user) {
            if (uaccess_user_write(frame->rsi, g_io_buf, (usize)n) != 0) {
                ret = SYS_EFAULT;
                break;
            }
        } else {
            memcpy((void *)(uintptr)frame->rsi, g_io_buf, (usize)n);
        }
        ret = (i64)n;
        break;
    }
    case SYS_APP_SPAWN: {
        thread *cur = sched_current();
        const struct app *a;
        u32 tid = 0u;

        if (!cur) {
            ret = SYS_EINVAL;
            break;
        }
        a = app_at((u32)frame->rdi);
        if (!a) {
            ret = SYS_ENOENT;
            break;
        }
        if (!syscall_caps_ok(user, CAP_APP_LAUNCH)) {
            ret = SYS_EPERM;
            break;
        }
        if (app_launch(a, cur->id, &tid) != 0) {
            ret = SYS_EIO;
            break;
        }
        ret = (i64)tid;
        break;
    }
    case SYS_CAP_QUERY: {
        thread *cur = sched_current();

        ret = cur ? (i64)cur->caps : 0;
        break;
    }
    case SYS_CAP_CHECK: {
        thread *cur = sched_current();
        u32 need = (u32)frame->rdi;

        if (!cur) {
            ret = SYS_EPERM;
            break;
        }
        ret = (cur->caps & need) == need ? 1 : 0;
        break;
    }
    case SYS_PKG_COUNT:
        ret = (i64)pkg_count();
        break;
    case SYS_PKG_INFO: {
        const struct pkg *pkg = pkg_at((u32)frame->rdi);
        u64 cap = frame->rdx;
        int n;

        if (!pkg) {
            ret = SYS_ENOENT;
            break;
        }
        if (frame->rsi == 0 || cap == 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (cap > SYS_PKG_INFO_MAX)
            cap = SYS_PKG_INFO_MAX;
        if (user && uaccess_check(frame->rsi, cap) != 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (!user && !harden_range_ok(frame->rsi, cap, HARDEN_IDENTITY_LIMIT)) {
            ret = SYS_EFAULT;
            break;
        }
        n = pkg_format_info(pkg, (char *)g_io_buf, (usize)cap);
        if (n < 0) {
            ret = SYS_EINVAL;
            break;
        }
        if (user) {
            if (uaccess_user_write(frame->rsi, g_io_buf, (usize)n) != 0) {
                ret = SYS_EFAULT;
                break;
            }
        } else {
            memcpy((void *)(uintptr)frame->rsi, g_io_buf, (usize)n);
        }
        ret = (i64)n;
        break;
    }
    case SYS_PKG_INSTALL: {
        const char *path;
        const char *reason = 0;

        if (!syscall_caps_ok(user, CAP_PKG_INSTALL)) {
            ret = SYS_EPERM;
            break;
        }
        path = syscall_path(frame, user, frame->rdi);
        if (!path || path[0] == 0) {
            ret = SYS_EFAULT;
            break;
        }
        if (pkg_install(path, &reason) != 0) {
            ret = SYS_EIO;
            break;
        }
        ret = 0;
        break;
    }
    case SYS_WRITE:
        if (frame->rdi >= UFILE_FD_BASE)
            ret = syscall_file_write(frame, user);
        else
            ret = syscall_write(frame);
        break;
    case SYS_EXIT:
        thread_kill_current(user ? (i64)frame->rdi : 0);
        ret = 0;
        break;
    default:
        ret = SYS_ENOSYS;
        break;
    }
    frame->rax = (u64)ret;
}

static i64 syscall0(u64 nr)
{
    i64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 syscall1(u64 nr, u64 a0)
{
    i64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 syscall2(u64 nr, u64 a0, u64 a1)
{
    i64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 syscall3(u64 nr, u64 a0, u64 a1, u64 a2)
{
    i64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

/* The port frame carries a fourth argument in %r10 (see the interrupt frame in
   kernel/arch/x86_64/isr.S), so the pointer-gate selftest needs its own
   helper: GCC has no "r10" constraint other than a bound register variable. */
static i64 syscall4(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3)
{
    i64 ret;
    register u64 r10 __asm__("r10") = a3;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

int syscall_socket_selftest(u32 ip, u16 port)
{
    /* Drive a full HTTP exchange through the socket syscalls only: connect,
       send a request, receive the reply and close. */
    static const char probe[] = "GET / HTTP/1.0\r\nHost: crugos\r\n\r\n";
    char buf[128];
    i64 h;
    i64 n;
    i64 i;
    int ok = 0;

    h = syscall2(SYS_SOCKET_CONNECT, (u64)ip, (u64)port);
    if (h <= 0)
        return -1;
    n = syscall3(SYS_SOCKET_SEND, (u64)h, (u64)(uintptr)probe,
                 (u64)(sizeof(probe) - 1));
    if (n != (i64)(sizeof(probe) - 1)) {
        syscall1(SYS_SOCKET_CLOSE, (u64)h);
        return -1;
    }
    n = syscall3(SYS_SOCKET_RECV, (u64)h, (u64)(uintptr)buf, (u64)sizeof(buf));
    for (i = 0; n > 0 && i + 6 <= n; i++) {
        if (buf[i] == '2' && buf[i + 1] == '0' && buf[i + 2] == '0' &&
            buf[i + 3] == ' ' && buf[i + 4] == 'O' && buf[i + 5] == 'K') {
            ok = 1;
            break;
        }
    }
    if (syscall1(SYS_SOCKET_CLOSE, (u64)h) != 0)
        return -1;
    return ok ? 0 : -1;
}

int syscall_resolve_selftest(const char *host, u32 addr)
{
    u32 out = 0;

    if (!host)
        return -1;
    if (syscall2(SYS_RESOLVE, (u64)(uintptr)host, (u64)(uintptr)&out) != 0)
        return -1;
    return out == addr ? 0 : -1;
}

static int syscall_pointer_gate(void)
{
    /* Report item 3: a caller-supplied buffer pointer and length must be
       validated before the kernel copies a payload or writes a u64 behind a
       NULL/out-of-range pointer. These are the real syscall entry points, not
       the helpers, so they exercise the %r10 frame slot as well. */
    static u8 payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    u8 recv_buf[8];
    u64 type = 0;
    i64 h;

    h = syscall0(SYS_PORT_CREATE);
    if (h <= 0)
        return 0;
    if (syscall4(SYS_PORT_SEND, (u64)h, 0, 0, 8) != SYS_EFAULT)
        goto fail;
    if (syscall4(SYS_PORT_SEND, (u64)h, 0, 0x100000000ull, 8) != SYS_EFAULT)
        goto fail;
    if (syscall4(SYS_PORT_SEND, (u64)h, 0, (u64)(uintptr)payload, 8) != 0)
        goto fail;
    if (syscall4(SYS_PORT_RECV, (u64)h, 0, 0, 8) != SYS_EFAULT)
        goto fail;
    if (syscall4(SYS_PORT_RECV, (u64)h, (u64)(uintptr)&type,
                 (u64)(uintptr)recv_buf, 8) != 8)
        goto fail;
    if (memcmp(recv_buf, payload, sizeof(payload)) != 0)
        goto fail;
    if (syscall1(SYS_HANDLE_CLOSE, (u64)h) != 0)
        return 0;
    return 1;
fail:
    (void)syscall1(SYS_HANDLE_CLOSE, (u64)h);
    kprintf("CRUGOS: syscall selftest failed (pointer gate)\n");
    return 0;
}

int syscall_selftest(void)
{
    i64 r;
    i64 h;

    r = syscall0(SYS_PING);
    if (r != (i64)SYS_PING_MAGIC)
        return 0;
    r = syscall0(SYS_GETTID);
    if (r < 0)
        return 0;
    r = syscall0(999);
    if (r != SYS_ENOSYS)
        return 0;
    h = syscall0(SYS_PORT_CREATE);
    if (h <= 0)
        return 0;
    if (syscall1(SYS_HANDLE_CLOSE, (u64)h) != 0)
        return 0;
    if (syscall1(SYS_HANDLE_CLOSE, (u64)h) != SYS_EBADF)
        return 0;
    if (!syscall_pointer_gate())
        return 0;
    return 1;
}
