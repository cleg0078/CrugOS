/* CrugOS address-space isolation probe (phase U2).
 *
 * The kernel starts two copies of this program at the same time. Each writes
 * its own tag to the same virtual address (ISO_WORD, pinned by iso.ld) and
 * yields between the write and the read-back, so an address space shared by
 * the two programs would show up as a mismatched word.
 *
 * The exit code is the number of mismatches; the kernel sums the two and
 * wants zero.
 */
#include "lib.h"

#define ISO_WORD 0x0000008000030000ull
#define ISO_TAG 0x15500000ull
#define ISO_LOOPS 300u

/* Pinned to its own page at ISO_WORD by iso.ld: the kernel reads this page's
   translation from each program's page table to prove the frames differ. */
volatile u64 iso_anchor __attribute__((section(".iso"), used)) = ISO_WORD;

int main(void)
{
    i64 tid = u_syscall3(U_SYS_GETTID, 0, 0, 0);
    volatile u64 *word = (volatile u64 *)(uintptr)ISO_WORD;
    u64 tag = ISO_TAG ^ ((u64)tid << 8);
    u32 mismatch = 0;
    u32 i;

    for (i = 0; i < ISO_LOOPS; i++) {
        *word = tag;
        u_syscall3(U_SYS_YIELD, 0, 0, 0);
        if (*word != tag)
            mismatch++;
    }
    u_puts("CRUGOS: iso tid ");
    u_putnum((u64)tid);
    u_puts(" word 0x");
    u_puthex(tag);
    u_puts(mismatch == 0 ? " ok\n" : " mismatch\n");
    return (int)mismatch;
}