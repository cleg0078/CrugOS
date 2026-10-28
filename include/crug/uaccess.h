/* CrugOS user-memory access helpers (phase U1).
 *
 * Every pointer that arrives from CPL3 goes through here. The range check is
 * pure arithmetic (harden_user_range_ok) so it holds even on CPUs without
 * SMAP; stac/clac only exists to let the kernel touch the page afterwards.
 */
#ifndef CRUG_UACCESS_H
#define CRUG_UACCESS_H

#include <crug/types.h>

/* Same value as SYS_EFAULT, repeated so this header does not depend on the
   syscall ABI. */
#define UACCESS_EFAULT (-14)

/* 0 when [addr, addr+len) lies inside the user window, UACCESS_EFAULT else. */
int uaccess_check(u64 addr, u64 len);

/* Copy from user memory into a kernel buffer. */
int uaccess_user_read(void *kdst, u64 uaddr, u64 len);

/* Copy from a kernel buffer into user memory. */
int uaccess_user_write(u64 uaddr, const void *ksrc, u64 len);

/* Copy a NUL-terminated user string into kdst (capacity cap including the
   terminator). Returns UACCESS_EFAULT for a bad pointer or an unterminated
   string. */
int uaccess_user_str(char *kdst, u64 uaddr, u64 cap);

#endif /* CRUG_UACCESS_H */
