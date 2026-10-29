/* CrugOS fork probe (phase U5).
 *
 * SYS_FORK duplicates the calling address space page for page. The child
 * resumes at the same instruction with 0 in rax, the parent gets the child's
 * thread id, and then both run the same code:
 *
 *   child  : overwrites g_word with 0x2222, reads it back, prints the value
 *            and exits 3.
 *   parent : reaps the child, then reads its own g_word, which must still be
 *            0x1111, and prints it.
 *
 * A shared (not copied) page would show 0x2222 in the parent, so the gate
 * proves the two address spaces are independent.
 */
#include "lib.h"

#define FORK_INIT_WORD 0x1111u
#define FORK_CHILD_WORD 0x2222u
#define FORK_CHILD_EXIT 3

static volatile u64 g_word = FORK_INIT_WORD;

int main(int argc, char **argv, char **envp)
{
    i64 tid;

    (void)argc;
    (void)argv;
    (void)envp;
    u_puts("CRUGOS: user fork start\n");
    g_word = FORK_INIT_WORD;
    tid = u_fork();
    if (tid < 0) {
        u_puts("CRUGOS: user fork failed (");
        u_putnum((u64)(-tid));
        u_puts(")\n");
        return 1;
    }
    if (tid == 0) {
        /* Child: a private copy of every page, g_word included. */
        g_word = FORK_CHILD_WORD;
        u_puts("CRUGOS: user fork child word ");
        u_putnum((u64)g_word);
        u_puts("\n");
        u_exit(FORK_CHILD_EXIT);
    }
    u_puts("CRUGOS: user fork tid ");
    u_putnum((u64)tid);
    u_puts("\n");
    {
        i64 code = u_wait((u32)tid);

        u_puts("CRUGOS: user fork reap exit ");
        u_putnum((u64)code);
        u_puts("\n");
        if (code != (i64)FORK_CHILD_EXIT) {
            u_puts("CRUGOS: user fork wrong child exit\n");
            return 2;
        }
    }
    u_puts("CRUGOS: user fork parent word ");
    u_putnum((u64)g_word);
    u_puts("\n");
    if (g_word != FORK_INIT_WORD) {
        u_puts("CRUGOS: user fork memory not isolated\n");
        return 3;
    }
    u_puts("CRUGOS: user fork ok (child exit 3, memory isolated)\n");
    return 0;
}
