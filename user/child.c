/* CrugOS spawn target (phase U4).
 *
 * Runs twice per gate: once as the program /bin/spawn starts with SYS_SPAWN
 * (argv[1] == "from-spawn") and once as the image SYS_EXEC replaces the
 * caller with (argv[1] == "from-exec"). Both runs exit with SPAWN_CHILD_EXIT
 * so the parent can prove it observed the child's real exit code.
 */
#include "lib.h"

#define SPAWN_CHILD_EXIT 5

int main(int argc, char **argv, char **envp)
{
    i64 tid;

    (void)envp;
    tid = u_gettid();
    u_puts("CRUGOS: user child running (tid ");
    u_putnum((u64)tid);
    u_puts(")\n");
    u_puts("CRUGOS: user child argc ");
    u_putnum((u64)(i64)argc);
    u_puts("\n");
    if (argc > 1 && argv && argv[1]) {
        u_puts("CRUGOS: user child arg ");
        u_puts(argv[1]);
        u_puts("\n");
    }
    return SPAWN_CHILD_EXIT;
}