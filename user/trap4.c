/* Ring-3 vector-4 probe (phase S1, security report H8).
 *
 * Vector 4 (#OF) is installed with a DPL-3 gate, so an `int $4` from CPL3
 * enters the handler with a user frame.  Before S1 the classifier called
 * that class TRAP_HANDLED while the dispatcher had no handled branch, so the
 * cheapest ring-3 instruction in the tree panicked the kernel.  The kernel
 * must now kill only this program and keep running. */
#include "lib.h"

int main(void)
{
    u_puts("CRUGOS: user trap4 probe (int $4 from ring 3)\n");
    __asm__ volatile("int $4");
    u_puts("CRUGOS: user trap4 returned (unexpected)\n");
    return 9;
}
