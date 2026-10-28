/* Ring-3 fault: store to the first address above the user window (U1).
 * The kernel must kill this program and keep running. */
#include "lib.h"

#define USER_TOP_ADDR 0x0000010000000000ull

int main(void)
{
    volatile u64 *bad = (volatile u64 *)(uintptr)USER_TOP_ADDR;

    u_puts("CRUGOS: user fault writing above the window\n");
    *bad = 0x43524731ull;
    u_puts("CRUGOS: user fault did not happen\n");
    return 2;
}
