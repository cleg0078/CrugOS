/* CrugOS spawn + exec probe (phase U4).
 *
 * 1. SYS_SPAWN starts /bin/child in its own address space and returns its
 *    thread id.
 * 2. SYS_WAIT blocks until that thread exits and returns its exit code (5).
 * 3. SYS_EXEC replaces this very thread's image with /bin/child and a
 *    different argv; that image prints "from-exec" and exits 5 too, so the
 *    gate sees the spawned thread finish with 5.
 */
#include "lib.h"

#define SPAWN_CHILD_EXIT 5

static const char *const g_child_argv[2] = { "/bin/child", "from-spawn" };
static const char *const g_exec_argv[2] = { "/bin/child", "from-exec" };

int main(int argc, char **argv, char **envp)
{
    i64 tid;
    i64 code;

    (void)argc;
    (void)argv;
    (void)envp;
    u_puts("CRUGOS: user spawn start\n");
    tid = u_spawn("/bin/child", 2, g_child_argv);
    if (tid < 0) {
        u_puts("CRUGOS: user spawn failed (");
        u_putnum((u64)(-tid));
        u_puts(")\n");
        return 1;
    }
    u_puts("CRUGOS: user spawn tid ");
    u_putnum((u64)tid);
    u_puts("\n");
    code = u_wait((u32)tid);
    u_puts("CRUGOS: user spawn child exit ");
    u_putnum((u64)code);
    u_puts("\n");
    if (code != (i64)SPAWN_CHILD_EXIT) {
        u_puts("CRUGOS: user spawn wrong exit code\n");
        return 2;
    }
    u_puts("CRUGOS: user spawn exec\n");
    (void)u_exec("/bin/child", 2, g_exec_argv);
    u_puts("CRUGOS: user spawn exec failed\n");
    return 3;
}