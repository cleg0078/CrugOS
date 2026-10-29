/* CrugOS large copy-on-write fork probe (phase U6).
 *
 * The U5 fork refused more than VM_SPACE_FORK_MAX_PAGES (256) user pages.
 * Sharing frames makes a big fork cheap and the cap moved to 4096, so this
 * program maps 300 pages of .bss on purpose: an eager 256-page fork would
 * have failed here with ENOMEM.
 *
 *   parent : sets the first byte to 0xA5, forks, reaps the child, then checks
 *            that its sentinel survived and that the child's edits to the
 *            first and last page are invisible.
 *   child  : writes 0x5A to the first and last byte of the region and exits 7;
 *            both stores are copy-on-write faults on private pages.
 */
#include "lib.h"

#define FORKBIG_PAGES 300u
#define FORKBIG_EXIT 7
#define FORKBIG_MARK 0xA5u
#define FORKBIG_CHILD_MARK 0x5Au

/* 1.2 MiB of .bss: the ELF loader maps and zeroes it before main runs. */
static volatile u8 g_big[FORKBIG_PAGES * 4096u];

int main(int argc, char **argv, char **envp)
{
    i64 tid;

    (void)argc;
    (void)argv;
    (void)envp;
    u_puts("CRUGOS: user forkbig start\n");
    g_big[0] = FORKBIG_MARK;
    tid = u_fork();
    if (tid < 0) {
        u_puts("CRUGOS: user forkbig failed (\n");
        u_putnum((u64)(-tid));
        u_puts(")\n");
        return 1;
    }
    if (tid == 0) {
        u_puts("CRUGOS: user forkbig child tid ");
        u_putnum(u_gettid());
        u_puts("\n");
        g_big[0] = FORKBIG_CHILD_MARK;
        g_big[sizeof(g_big) - 1] = FORKBIG_CHILD_MARK;
        u_exit(FORKBIG_EXIT);
    }
    u_puts("CRUGOS: user forkbig tid ");
    u_putnum((u64)tid);
    u_puts("\n");
    {
        i64 code = u_wait((u32)tid);

        u_puts("CRUGOS: user forkbig reap exit ");
        u_putnum((u64)code);
        u_puts("\n");
        if (code != (i64)FORKBIG_EXIT) {
            u_puts("CRUGOS: user forkbig failed (\n");
            u_putnum((u64)code);
            u_puts(")\n");
            return 2;
        }
    }
    if (g_big[0] != FORKBIG_MARK || g_big[sizeof(g_big) - 1] != 0u) {
        u_puts("CRUGOS: user forkbig memory not isolated\n");
        return 3;
    }
    u_puts("CRUGOS: user forkbig ok (child exit 7, 300 page(s) isolated)\n");
    return 0;
}
