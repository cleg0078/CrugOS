/* CrugOS phase 2 syscall dispatcher. */
#include <crug/syscall.h>
#include <crug/thread.h>
#include <crug/process.h>
#include <crug/msgport.h>
#include <crug/heap.h>
#include <crug/kernel.h>
#include <crug/net.h>

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

void syscall_dispatch(trap_frame *frame)
{
    i64 ret;
    u64 nr = frame->rax;

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

        if (!p) {
            ret = SYS_EBADF;
            break;
        }
        ret = msgport_send(p, frame->rsi, (const void *)(uintptr)frame->rdx,
                           (usize)frame->r10);
        break;
    }
    case SYS_PORT_RECV: {
        handle_table *table = process_handles(process_current());
        msgport *p = (msgport *)handle_get(table, frame->rdi, HANDLE_PORT);
        u64 type = 0;
        i64 n;

        if (!p) {
            ret = SYS_EBADF;
            break;
        }
        n = msgport_recv(p, &type, (void *)(uintptr)frame->rdx, (usize)frame->r10);
        if (n >= 0 && frame->rsi)
            *(u64 *)(uintptr)frame->rsi = type;
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
        handle_table *table = process_handles(process_current());
        struct net_tcp_socket *s = (struct net_tcp_socket *)handle_get(
            table, frame->rdi, HANDLE_SOCKET);
        int n;

        if (!s) {
            ret = SYS_EBADF;
            break;
        }
        n = tcp_send(s, (const void *)(uintptr)frame->rsi, (usize)frame->rdx);
        ret = n < 0 ? SYS_EINVAL : (i64)n;
        break;
    }
    case SYS_SOCKET_RECV: {
        handle_table *table = process_handles(process_current());
        struct net_tcp_socket *s = (struct net_tcp_socket *)handle_get(
            table, frame->rdi, HANDLE_SOCKET);
        int n;

        if (!s) {
            ret = SYS_EBADF;
            break;
        }
        n = tcp_recv(s, (void *)(uintptr)frame->rsi, (usize)frame->rdx, 3000u);
        ret = n < 0 ? SYS_EAGAIN : (i64)n;
        break;
    }
    case SYS_SOCKET_CLOSE: {
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
        struct net_dns_answer answer;
        const char *name = (const char *)(uintptr)frame->rdi;

        if (!name || !frame->rsi) {
            ret = SYS_EINVAL;
            break;
        }
        if (net_dns_resolve(name, &answer, 3000u) != 0 || answer.count == 0u) {
            ret = SYS_EIO;
            break;
        }
        *(u32 *)(uintptr)frame->rsi = answer.addr[0];
        ret = 0;
        break;
    }
    case SYS_EXIT:
        thread_exit();
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
    return 1;
}
