/* CrugOS single syscall dispatcher (phase 2: int 0x80 gateway). */
#ifndef CRUG_SYSCALL_H
#define CRUG_SYSCALL_H

#include <crug/types.h>
#include <crug/trap.h>

#define SYS_PING 0u
#define SYS_GETTID 1u
#define SYS_YIELD 2u
#define SYS_SLEEP_TICKS 3u
#define SYS_PORT_CREATE 4u
#define SYS_PORT_SEND 5u
#define SYS_PORT_RECV 6u
#define SYS_HANDLE_CLOSE 7u
#define SYS_EXIT 8u
#define SYS_MAX 9u

#define SYS_EINVAL (-22ll)
#define SYS_ENOSYS (-38ll)
#define SYS_ENOMEM (-12ll)
#define SYS_EBADF (-9ll)
#define SYS_EAGAIN (-11ll)

#define SYSCALL_VECTOR 0x80u
#define SYS_PING_MAGIC 0x43524731ull

void syscall_dispatch(trap_frame *frame);
int syscall_selftest(void);

#endif /* CRUG_SYSCALL_H */
