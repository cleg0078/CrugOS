/* Ring-3 selftest for the userspace gate (phase U1).
 *
 * `user.test=1` makes the kernel spawn three ELF programs from /bin and check
 * the two walls the milestone is about:
 *
 *   /bin/hello   prints from CPL3 and exits 0
 *   /bin/badptr  passes a kernel pointer to a syscall and must get -EFAULT
 *   /bin/fault   stores above the user window, so #PF must kill only that
 *                program; the kernel keeps running and sees exit -14
 *
 * Every default boot leaves the token out, so nothing here runs and no marker
 * is printed.  See docs/userspace.md. */
#include <crug/exec.h>
#include <crug/kernel.h>
#include <crug/thread.h>
#include <crug/string.h>

static int g_user_test;

static int user_space(char c)
{
    /* cmdline.txt is copied verbatim, so the last token carries a newline. */
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int user_token_is(const char *tok, u32 len, const char *key)
{
    u32 klen = (u32)strlen(key);

    if (len != klen + 2u)
        return 0;
    if (strncmp(tok, key, klen) != 0)
        return 0;
    return tok[klen] == '=' && tok[klen + 1u] == '1';
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
        if (user_token_is(cmd + start, len, "user.test"))
            g_user_test = 1;
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
    kprintf("CRUGOS: user selftest ok (3 program(s), 1 fault(s))\n");
    return 0;
}
