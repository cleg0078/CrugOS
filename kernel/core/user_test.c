/* Ring-3 selftest for the userspace gate (phase U1) and the address-space
 * isolation check (phase U2).
 *
 * `user.test=1` makes the kernel spawn three ELF programs from /bin and check
 * the two walls the milestone is about:
 *
 *   /bin/hello   prints from CPL3 and exits 0
 *   /bin/badptr  passes a kernel pointer to a syscall and must get -EFAULT
 *   /bin/fault   stores above the user window, so #PF must kill only that
 *                program; the kernel keeps running and sees exit -14
 *
 * `user.test=3` adds /bin/sh, the ring-3 shell: it opens /etc/sh.rc, runs
 * the script through the file syscalls, writes /shout.txt and exits 7.
 *
 * `user.test=4` adds /bin/spawn: it starts /bin/child with SYS_SPAWN, blocks in
 * SYS_WAIT for the child's exit code, then replaces itself with /bin/child
 * through SYS_EXEC.
 *
 * `user.test=5` adds /bin/fork: the program calls SYS_FORK, so it continues
 * in two address spaces, the child overwrites a private word and exits 3 and
 * the parent must still read its own copy of that word.
 *
 * `user.test=2` runs those three and then two concurrent instances of
 * /bin/iso, which write the SAME virtual address (0x8000030000, pinned by
 * user/iso.ld) and yield between write and check. The kernel asserts the two
 * spaces map that address onto different physical frames; each program asserts
 * it always reads back its own tag.
 *
 * Every default boot leaves the token out, so nothing here runs and no marker
 * is printed.  See docs/userspace.md. */
#include <crug/exec.h>
#include <crug/kernel.h>
#include <crug/thread.h>
#include <crug/vm.h>
#include <crug/string.h>

/* The address /bin/iso links its private word at (see user/iso.ld). */
#define USER_ISO_WORD 0x0000008000030000ull

/* 0 = off, 1 = U1 programs, 2 = U1 programs plus the isolation pair,
   3 = plus the ring-3 shell, 4 = plus spawn/wait/exec, 5 = plus fork. */
static int g_user_test;

static int user_space(char c)
{
    /* cmdline.txt is copied verbatim, so the last token carries a newline. */
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Returns the level of a `key=<digit>` token, or 0 when it does not match. */
static int user_token_level(const char *tok, u32 len, const char *key)
{
    u32 klen = (u32)strlen(key);
    char c;

    if (len != klen + 2u)
        return 0;
    if (strncmp(tok, key, klen) != 0)
        return 0;
    if (tok[klen] != '=')
        return 0;
    c = tok[klen + 1u];
    if (c < '1' || c > '9')
        return 0;
    return (int)(c - '0');
}

void user_test_cmdline(const struct boot_info *bi)
{
    const char *cmd;
    u32 i = 0;

    if (!bi || !bi->command_line)
        return;
    cmd = bi->command_line;
    while (cmd[i]) {
        u32 start;
        u32 len;

        while (cmd[i] && user_space(cmd[i]))
            i++;
        start = i;
        while (cmd[i] && !user_space(cmd[i]))
            i++;
        len = i - start;
        {
            int level = user_token_level(cmd + start, len, "user.test");

            if (level > g_user_test)
                g_user_test = level;
        }
    }
}

static int user_run_one(const char *path, i64 expect)
{
    elf_exec_result res;
    thread *t;
    i64 code;

    memset(&res, 0, sizeof(res));
    t = elf_exec_run(path, &res);
    if (!t) {
        kprintf("CRUGOS: user selftest failed (%s did not start)\n", path);
        return -1;
    }
    code = thread_join_code(t);
    if (code != expect) {
        kprintf("CRUGOS: user selftest failed (%s exited %lld, expected %lld)\n",
                path, code, expect);
        return -1;
    }
    kprintf("CRUGOS: user program %s finished (exit %lld)\n", path, code);
    return 0;
}

/* Phase U2: two programs, two address spaces, one virtual address. Both are
   created before the first schedule point, so they run strictly interleaved.
   The physical frames are read from the page tables while both threads are
   still alive; the mismatch counts come back as exit codes. */
static int user_isolation(void)
{
    elf_exec_result ra;
    elf_exec_result rb;
    thread *ta;
    thread *tb;
    u64 pa_a;
    u64 pa_b;
    i64 code_a;
    i64 code_b;
    i64 mismatch;

    kprintf("CRUGOS: user isolation start\n");
    memset(&ra, 0, sizeof(ra));
    memset(&rb, 0, sizeof(rb));
    ta = elf_exec_run("/bin/iso", &ra);
    if (!ta) {
        kprintf("CRUGOS: user isolation failed (first program did not start)\n");
        return -1;
    }
    tb = elf_exec_run("/bin/iso", &rb);
    if (!tb) {
        (void)thread_join_code(ta);
        kprintf("CRUGOS: user isolation failed (second program did not start)\n");
        return -1;
    }
    pa_a = vm_space_translate(ta->space, USER_ISO_WORD);
    pa_b = vm_space_translate(tb->space, USER_ISO_WORD);
    kprintf("CRUGOS: user isolation frames 0x%llx vs 0x%llx at 0x%llx\n",
            pa_a & VM_ADDR_MASK, pa_b & VM_ADDR_MASK, USER_ISO_WORD);
    code_a = thread_join_code(ta);
    code_b = thread_join_code(tb);
    if (pa_a == 0 || pa_b == 0 || (pa_a & VM_ADDR_MASK) == (pa_b & VM_ADDR_MASK)) {
        kprintf("CRUGOS: user isolation failed (the two spaces share a frame)\n");
        return -1;
    }
    mismatch = code_a + code_b;
    if (mismatch != 0) {
        kprintf("CRUGOS: user isolation failed (%lld mismatch(es))\n", mismatch);
        return -1;
    }
    kprintf("CRUGOS: user isolation ok (2 program(s), 0 mismatch(es))\n");
    return 0;
}

/* Phase U3: the ring-3 shell. argv carries the script it has to run. */
static int user_shell(void)
{
    static const char *const argv[2] = { "/bin/sh", "/etc/sh.rc" };
    elf_exec_result res;
    thread *t;
    i64 code;

    kprintf("CRUGOS: user shell start\n");
    memset(&res, 0, sizeof(res));
    t = elf_exec_run_args("/bin/sh", 2, argv, &res);
    if (!t) {
        kprintf("CRUGOS: user selftest failed (/bin/sh did not start)\n");
        return -1;
    }
    code = thread_join_code(t);
    if (code != 7) {
        kprintf("CRUGOS: user selftest failed (/bin/sh exited %lld, expected 7)\n",
                code);
        return -1;
    }
    kprintf("CRUGOS: user shell finished (exit 7)\n");
    return 0;
}

/* Phase U4: /bin/spawn starts /bin/child, waits for its exit code and then
   execs into /bin/child itself. Both images exit 5, so a wrong wait result or
   a failed exec shows up as a different code here. */
static int user_spawn(void)
{
    elf_exec_result res;
    thread *t;
    i64 code;

    kprintf("CRUGOS: user spawn test start\n");
    memset(&res, 0, sizeof(res));
    t = elf_exec_run("/bin/spawn", &res);
    if (!t) {
        kprintf("CRUGOS: user selftest failed (/bin/spawn did not start)\n");
        return -1;
    }
    code = thread_join_code(t);
    if (code != 5) {
        kprintf("CRUGOS: user selftest failed (/bin/spawn exited %lld, expected 5)\n",
                code);
        return -1;
    }
    kprintf("CRUGOS: user spawn test ok (child exit 5, exec exit 5)\n");
    return 0;
}

/* Phase U5: /bin/fork duplicates itself, the child writes to a private word
   and exits 3, and the parent has to observe both the exit code and its own
   unchanged word, which only holds if the address space was copied. */
static int user_fork(void)
{
    elf_exec_result res;
    thread *t;
    i64 code;

    kprintf("CRUGOS: user fork test start\n");
    memset(&res, 0, sizeof(res));
    t = elf_exec_run("/bin/fork", &res);
    if (!t) {
        kprintf("CRUGOS: user selftest failed (/bin/fork did not start)\n");
        return -1;
    }
    code = thread_join_code(t);
    if (code != 0) {
        kprintf("CRUGOS: user selftest failed (/bin/fork exited %lld, expected 0)\n",
                code);
        return -1;
    }
    kprintf("CRUGOS: user fork test ok (child exit 3, memory isolated)\n");
    return 0;
}

int user_selftest(void)
{
    if (!g_user_test)
        return 0;

    kprintf("CRUGOS: user selftest start\n");
    if (user_run_one("/bin/hello", 0) != 0)
        return -1;
    if (user_run_one("/bin/badptr", 0) != 0)
        return -1;
    if (user_run_one("/bin/fault", -14) != 0)
        return -1;
    if (g_user_test >= 2 && user_isolation() != 0)
        return -1;
    if (g_user_test >= 3 && user_shell() != 0)
        return -1;
    if (g_user_test >= 4 && user_spawn() != 0)
        return -1;
    if (g_user_test >= 5 && user_fork() != 0)
        return -1;
    kprintf("CRUGOS: user selftest ok (3 program(s), 1 fault(s))\n");
    return 0;
}
