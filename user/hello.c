/* Ring-3 hello: the first program that runs at CPL3 (phase U1). */
#include "lib.h"

int main(void)
{
    u_puts("CRUGOS: user hello from ring 3\n");
    return 0;
}
