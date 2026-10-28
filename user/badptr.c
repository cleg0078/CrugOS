/* Ring-3 pointer gate: a kernel pointer must come back as -EFAULT (U1). */
#include "lib.h"

int main(void)
{
    i64 ret = u_write_bad_ptr();

    if (ret == -14) {
        u_puts("CRUGOS: user badptr blocked (EFAULT)\n");
        return 0;
    }
    u_puts("CRUGOS: user badptr was not blocked\n");
    return 1;
}
