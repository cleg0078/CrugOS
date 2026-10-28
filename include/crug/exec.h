/* CrugOS user program loader (phase U1). */
#ifndef CRUG_EXEC_H
#define CRUG_EXEC_H

#include <crug/types.h>
#include <crug/vm.h>

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

/* Phase U4. A loaded image: a private address space with the ELF mapped, the
   stack reserved and the argv block copied in. The caller owns space. */
typedef struct elf_image_load {
    vm_space *space;
    u64 entry;
    u64 rsp;
    u64 base;
    u64 size;
} elf_image_load;

/* Load PATH into a fresh address space without creating a thread. Returns 0
   and fills OUT, or -1 (nothing is leaked). */
int elf_load_user(const char *path, u32 argc, const char *const *argv,
                  elf_image_load *out);

/* Spawn: load PATH and start it as a new user thread that records PARENT_ID
   as its parent (0 for the kernel). Returns the thread or 0. */
struct thread *elf_spawn_user(const char *path, u32 argc,
                              const char *const *argv, u32 parent_id,
                              elf_exec_result *out);

/* Exec: replace the calling user thread's image in place. Returns 0 and the
   new entry and stack pointer; on success it never returns to user space
   through the old image. The caller rewrites the trap frame. */
int elf_exec_in_place(const char *path, u32 argc, const char *const *argv,
                      u64 *out_entry, u64 *out_rsp);

/* Pure bounds check shared by SYS_SPAWN and SYS_EXEC: ARGC pointers and
   STR_BYTES of strings must fit the block limit. Returns 1 when they do. */
int exec_args_ok(u32 argc, u64 str_bytes);

/* Ring-3 selftest (make test-user, boot token user.test=1). */
void user_test_cmdline(const struct boot_info *bi);
int user_selftest(void);

#endif /* CRUG_EXEC_H */
