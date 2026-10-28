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

/* The argv/envp block the kernel copies onto the top page of the user stack. */
#define EXEC_ARGS_MAX 16u
#define EXEC_ARGS_STR_MAX 2048u
#define EXEC_ARGS_BLOCK_MAX 4096u

/* Layout builder: argc at rsp, then argv[] and NULL, then envp[] and NULL,
   then the strings. base_va is the user address area is copied to. Returns 0
   and the byte offset of rsp from the start of area, or -1 when the
   arguments do not fit. See user/start.S and docs/userspace.md. */
int exec_args_build(u8 *area, u64 area_size, u64 base_va, u32 argc,
                    const char *const *argv, const char *const *envp,
                    u64 *out_rsp_off);

/* Load PATH from the root filesystem and run it at CPL3. Returns the new
   thread (join it with thread_join_code) or 0 on failure. */
struct thread *elf_exec_run(const char *path, elf_exec_result *out);

/* Same, with argv[] visible to main(). */
struct thread *elf_exec_run_args(const char *path, u32 argc,
                                 const char *const *argv,
                                 elf_exec_result *out);

/* Ring-3 selftest (make test-user, boot token user.test=1). */
void user_test_cmdline(const struct boot_info *bi);
int user_selftest(void);

#endif /* CRUG_EXEC_H */
