/* CrugOS user program loader (phase U1). */
#ifndef CRUG_EXEC_H
#define CRUG_EXEC_H

#include <crug/types.h>

#define ELF_MAX_BYTES (512u * 1024u)
#define USER_STACK_PAGES 4u
#define USER_STACK_TOP 0x0000008040000000ull

typedef struct elf_exec_result {
    u32 tid;
    u32 reserved;
    u64 entry;
    u64 base;
    u64 size;
} elf_exec_result;

struct thread;
struct boot_info;

/* Load PATH from the root filesystem and run it at CPL3. Returns the new
   thread (join it with thread_join_code) or 0 on failure. */
struct thread *elf_exec_run(const char *path, elf_exec_result *out);

/* Ring-3 selftest (make test-user, boot token user.test=1). */
void user_test_cmdline(const struct boot_info *bi);
int user_selftest(void);

#endif /* CRUG_EXEC_H */
