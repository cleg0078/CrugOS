/* CrugOS single syscall dispatcher (phase U1: callable from ring 3). */
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
#define SYS_RESOLVE 15u
#define SYS_WRITE 16u
#define SYS_OPEN 17u
#define SYS_READ 18u
#define SYS_CLOSE 19u
#define SYS_LISTDIR 20u
#define SYS_SPAWN 21u
#define SYS_WAIT 22u
#define SYS_EXEC 23u
#define SYS_FORK 24u
#define SYS_APP_COUNT 25u
#define SYS_APP_INFO 26u
#define SYS_APP_SPAWN 27u
#define SYS_CAP_QUERY 28u
#define SYS_CAP_CHECK 29u
#define SYS_PKG_COUNT 30u
#define SYS_PKG_INFO 31u
#define SYS_PKG_INSTALL 32u
#define SYS_PKG_REMOVE 33u
#define SYS_MAX 34u

/* Open flags for SYS_OPEN (phase U3). SYS_O_RDONLY is the zero flag. */
#define SYS_O_RDONLY 0u
#define SYS_O_WRONLY 1u
#define SYS_O_CREAT 2u
#define SYS_O_TRUNC 4u

#define SYS_ENOENT (-2ll)
#define SYS_ESRCH (-3ll)
#define SYS_EINVAL (-22ll)
#define SYS_ENOSYS (-38ll)
#define SYS_ENOMEM (-12ll)
#define SYS_EBADF (-9ll)
#define SYS_EAGAIN (-11ll)
#define SYS_EIO (-5ll)
#define SYS_EFAULT (-14ll)
#define SYS_EBUSY (-16ll)
#define SYS_EPERM (-1ll)

#define SYSCALL_VECTOR 0x80u
#define SYS_PING_MAGIC 0x43524731ull

void syscall_dispatch(trap_frame *frame);
int syscall_selftest(void);

/* Socket path self-test: connect, send, receive and close through int 0x80. */
int syscall_socket_selftest(u32 ip, u16 port);

/* Resolver path self-test: resolve HOST through int 0x80 and expect ADDR. */
int syscall_resolve_selftest(const char *host, u32 addr);

/* Phase 7A application registry: count, text record and spawn by id. */
#define SYS_APP_INFO_MAX 512u

/* Phase 7B capabilities and packages: the caller's mask, one check, and the
   read-only package registry plus installation and removal requests.
   SYS_CAP_QUERY returns the caller's mask, SYS_CAP_CHECK(need) returns 1 when
   every bit of NEED is held, SYS_PKG_INFO(index, buf, cap) writes the text
   record, SYS_PKG_INSTALL(path) installs a signed .crpkg and
   SYS_PKG_REMOVE(id) removes one (both need CAP_PKG_INSTALL). */
#define SYS_PKG_INFO_MAX 192u

#endif /* CRUG_SYSCALL_H */
