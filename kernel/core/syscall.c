/* CrugOS phase 2 syscall dispatcher. */
#include <crug/syscall.h>
#include <crug/thread.h>
#include <crug/process.h>
#include <crug/msgport.h>
#include <crug/heap.h>
#include <crug/kernel.h>

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

        ret = handle_close(table, frame->rdi) == 0 ? 0 : SYS_EBADF;
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
