/* /Applications/Hello.app payload (phase 7A).
 *
 * Launched from the Workbench drawer or the ring-3 shell ('app run hello'):
 * argv[0] is the bundle directory, so the program can prove which bundle
 * started it. Exits 0 so the launcher's reap path reports a clean exit. */
#include "lib.h"

int main(int argc, char **argv, char **envp)
{
    i64 tid;

    (void)envp;
    tid = u_gettid();
    u_puts("CRUGOS: user app hello running (tid ");
    u_putnum((u64)tid);
    u_puts(")\n");
    if (argc > 0 && argv && argv[0]) {
        u_puts("CRUGOS: user app hello dir ");
        u_puts(argv[0]);
        u_puts("\n");
    }
    u_puts("CRUGOS: user app hello done\n");
    return 0;
}