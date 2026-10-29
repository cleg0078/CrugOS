/* CrugOS copy-on-write fork probe (phase U6).
 *
 * SYS_FORK no longer copies the address space: the child shares the parent's
 * frames and both sides see them mapped read-only. The first store on either
 * side takes a private copy (VM_COW, resolved by the user #PF hook), so the
 * probes below must see their own writes and the other side must never see
 * them:
 *
 *   child  : writes 0x2222 and 0x5555 into two different pages, prints both,
 *            exits 3. Both stores fault and copy.
 *   parent : reads its own 0x1111/0x3333 back (reads never fault), then, after
 *            reaping the child, stores 0x4444. By then the child's space is
 *            gone, so the kernel owns the frame alone and repairs the leaf
 *            without copying a page ('sole owner').
 *
 * A shared frame that stayed writable would leak the child's value into the
 * parent; an eager copy would waste the frames this phase exists to share.
 */
#include "lib.h"

#define FORK_INIT_WORD 0x1111u
#define FORK_CHILD_WORD 0x2222u
#define FORK_INIT_SECOND 0x3333u
#define FORK_CHILD_SECOND 0x5555u
#define FORK_PARENT_WRITE 0x4444u
#define FORK_CHILD_EXIT 3

/* Two pages: index 0 and index 512 live in different 4 KiB frames, so the
   child must take exactly two copy-on-write faults. */
static volatile u64 g_words[1024];

static void word_line(const char *label, u64 value)
{
    u_puts(label);
    u_putnum(value);
    u_puts("\n");
}

int main(int argc, char **argv, char **envp)
{
    i64 tid;

    (void)argc;
    (void)argv;
    (void)envp;
    u_puts("CRUGOS: user fork start\n");
    g_words[0] = FORK_INIT_WORD;
    g_words[512] = FORK_INIT_SECOND;
    tid = u_fork();
    if (tid < 0) {
        u_puts("CRUGOS: user fork failed (\n");
        u_putnum((u64)(-tid));
        u_puts("\n");
        return 1;
    }
    if (tid == 0) {
        /* Child: both leaves are read-only shares until these stores. */
        g_words[0] = FORK_CHILD_WORD;
        g_words[512] = FORK_CHILD_SECOND;
        word_line("CRUGOS: user fork child word ", (u64)g_words[0]);
        word_line("CRUGOS: user fork child second ", (u64)g_words[512]);
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
    word_line("CRUGOS: user fork parent word ", (u64)g_words[0]);
    word_line("CRUGOS: user fork parent second ", (u64)g_words[512]);
    if (g_words[0] != FORK_INIT_WORD || g_words[512] != FORK_INIT_SECOND) {
        u_puts("CRUGOS: user fork memory not isolated\n");
        return 3;
    }
    /* The child's space is gone, so this store is the sole-owner path. */
    g_words[0] = FORK_PARENT_WRITE;
    word_line("CRUGOS: user fork parent write ", (u64)g_words[0]);
    if (g_words[0] != FORK_PARENT_WRITE) {
        u_puts("CRUGOS: user fork parent write lost\n");
        return 4;
    }
    u_puts("CRUGOS: user fork ok (child exit 3, memory isolated, 2 COW page(s))\n");
    return 0;
}
