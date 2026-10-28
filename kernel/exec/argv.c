/* CrugOS argv/envp block builder (phase U3). See include/crug/exec.h.
 *
 * The kernel writes the initial stack image into a kernel buffer and copies
 * it to the top page of the user stack, so this arithmetic is host tested and
 * user/start.S only has to know the three fixed offsets: argc at rsp, argv at
 * rsp+8 and envp right after the NULL that terminates argv. */
#include <crug/exec.h>
#include <crug/string.h>

/* Phase U4: the syscall layer copies argv out of user memory before building
 * the block, so it needs the same limits as a pure predicate. */
int exec_args_ok(u32 argc, u64 str_bytes)
{
    if (argc > EXEC_ARGS_MAX)
        return 0;
    if (str_bytes > EXEC_ARGS_STR_MAX)
        return 0;
    return 1;
}

static void put64(u8 *area, u64 off, u64 value)
{
    memcpy(area + off, &value, 8);
}

int exec_args_build(u8 *area, u64 area_size, u64 base_va, u32 argc,
                    const char *const *argv, const char *const *envp,
                    u64 *out_rsp_off)
{
    u64 offs[EXEC_ARGS_MAX];
    u64 eoffs[EXEC_ARGS_MAX];
    u64 sp;
    u64 str_bytes = 0;
    u32 envc = 0;
    u32 i;

    if (!area || !out_rsp_off)
        return -1;
    if (argc > EXEC_ARGS_MAX)
        return -1;
    if (argc != 0 && !argv)
        return -1;
    if (envp) {
        while (envp[envc] != 0) {
            if (envc >= EXEC_ARGS_MAX)
                return -1;
            envc++;
        }
    }
    if (area_size > EXEC_ARGS_BLOCK_MAX)
        area_size = EXEC_ARGS_BLOCK_MAX;
    if (area_size < 64u)
        return -1;
    memset(area, 0, (usize)area_size);
    for (i = 0; i < argc; i++) {
        u64 len;

        if (!argv[i])
            return -1;
        len = strlen(argv[i]) + 1u;
        if (str_bytes + len > EXEC_ARGS_STR_MAX)
            return -1;
        str_bytes += len;
    }
    for (i = 0; i < envc; i++) {
        u64 len;

        if (!envp[i])
            return -1;
        len = strlen(envp[i]) + 1u;
        if (str_bytes + len > EXEC_ARGS_STR_MAX)
            return -1;
        str_bytes += len;
    }
    sp = area_size;
    for (i = 0; i < envc; i++) {
        u64 len = strlen(envp[i]) + 1u;

        sp -= len;
        memcpy(area + sp, envp[i], (usize)len);
        eoffs[i] = sp;
    }
    for (i = 0; i < argc; i++) {
        u64 len = strlen(argv[i]) + 1u;

        sp -= len;
        memcpy(area + sp, argv[i], (usize)len);
        offs[i] = sp;
    }
    /* Lay the whole word block out first and align afterwards: aligning after
       writing the argc word would leave rsp below every pointer in the block
       and user/start.S would read a zero argc. */
    sp -= ((u64)envc + 1ull) * 8ull + ((u64)argc + 1ull) * 8ull + 8ull;
    sp &= ~15ull;
    put64(area, sp, (u64)argc);
    for (i = 0; i < argc; i++)
        put64(area, sp + 8ull + (u64)i * 8ull, base_va + offs[i]);
    for (i = 0; i < envc; i++)
        put64(area, sp + 8ull + ((u64)argc + 1ull) * 8ull + (u64)i * 8ull,
              base_va + eoffs[i]);
    *out_rsp_off = sp;
    return 0;
}