/* Phase 7B: the capability model.

   Every thread carries a 32-bit capability mask.  A kernel thread starts with
   CAP_ALL; a ring-3 thread inherits its parent's mask and a bundle launch
   narrows it further to the intersection of what the manifest requires and
   what /System/perms.conf grants.  Nothing outside these bits is implied: a
   missing bit makes the matching syscall answer SYS_EPERM instead of doing
   the work.  The parser and the formatter are pure and host tested. */
#ifndef CRUG_CAPS_H
#define CRUG_CAPS_H

#include <crug/types.h>

#define CAP_FS_READ (1u << 0)
#define CAP_FS_WRITE (1u << 1)
#define CAP_APP_LAUNCH (1u << 2)
#define CAP_NET (1u << 3)
#define CAP_AUDIO (1u << 4)
#define CAP_PKG_INSTALL (1u << 5)
#define CAP_COUNT 6u
#define CAP_NONE 0u
#define CAP_ALL ((1u << CAP_COUNT) - 1u)

/* The longest name is 'pkg.install' (11 characters); 16 is comfortable. */
#define CAP_NAME_MAX 16u

/* The stable name of one bit, or 0 for a bit that is not defined. */
const char *cap_name(u32 bit);
/* The bit for a name, or 0 when the name is unknown. */
u32 cap_from_name(const char *name);

/* Parse a comma (or space) separated list such as "fs.read,net".
   Returns 0 and sets *out, or -1 and copies the offending token into
   bad[0..bad_cap) when it is not a capability.  An empty list is legal and
   yields CAP_NONE. */
int cap_parse_list(const char *list, usize len, u32 *out, char *bad,
                   usize bad_cap);

/* Write "fs.read,net" into out ("none" for CAP_NONE).  Returns the byte
   count, or -1 when it does not fit. */
int cap_format(u32 mask, char *out, usize cap);

u32 cap_intersect(u32 a, u32 b);

#endif /* CRUG_CAPS_H */
