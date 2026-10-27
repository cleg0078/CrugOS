/* CrugOS single syscall dispatcher (phase 4C: adds the socket calls). */
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
#define SYS_SOCKET_CONNECT 9u
#define SYS_SOCKET_SEND 10u
#define SYS_SOCKET_RECV 11u
#define SYS_SOCKET_CLOSE 12u
#define SYS_SOCKET_LISTEN 13u
#define SYS_SOCKET_ACCEPT 14u
#define SYS_MAX 15u

#define SYS_EINVAL (-22ll)
#define SYS_ENOSYS (-38ll)
#define SYS_ENOMEM (-12ll)
#define SYS_EBADF (-9ll)
#define SYS_EAGAIN (-11ll)
#define SYS_EIO (-5ll)

#define SYSCALL_VECTOR 0x80u
#define SYS_PING_MAGIC 0x43524731ull

void syscall_dispatch(trap_frame *frame);
int syscall_selftest(void);

/* Socket path self-test: connect, send, receive and close through int 0x80. */
int syscall_socket_selftest(u32 ip, u16 port);

#endif /* CRUG_SYSCALL_H */
