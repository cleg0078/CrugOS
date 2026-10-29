/* /Applications/Orbit.app payload (phase 7A).
 *
 * Exits 5 so a launcher can prove it read the real child exit code, and
 * prints its bundle directory (argv[0]) so the gate can tell which of the
 * two bundles actually ran. */
#include "lib.h"

#define ORBIT_EXIT 5

int main(int argc, char **argv, char **envp)
{
    i64 tid;

    (void)envp;
    tid = u_gettid();
    u_puts("CRUGOS: user app orbit running (tid ");
    u_putnum((u64)tid);
    u_puts(")\n");
    if (argc > 0 && argv && argv[0]) {
        u_puts("CRUGOS: user app orbit dir ");
        u_puts(argv[0]);
        u_puts("\n");
    }
    u_puts("CRUGOS: user app orbit done\n");
    return ORBIT_EXIT;
}