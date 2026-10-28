/* Fault injection for the trap-policy gate (phase H3).
 *
 * `trap.test=<recipe>` makes the kernel take one specific exception just
 * before it would power off, so scripts/run-trap.py can check the crash
 * report and the exit status without a debugger.  Recipes:
 *
 *   de   divide error          (#DE, vector 0)
 *   ud   invalid opcode        (#UD, vector 6, ud2)
 *   gp   general protection    (#GP, vector 13, bad %ds selector)
 *   pf   page fault            (#PF, vector 14, unmapped high address)
 *   df   double fault          (#DF, vector 8; a push on a non-canonical
 *                               %rsp faults again while delivering #GP)
 *   nmi  non-maskable interrupt(vector 2, int $2, IST2)
 *   mc   machine check         (vector 18, int $18, IST3)
 *   db   debug                 (vector 1, int $1, IST4)
 *
 * Every recipe ends in the panic path by design; a recipe that returns is a
 * bug and prints a marker so the gate fails on it.  See docs/traps.md. */
#include <crug/trap.h>
#include <crug/kernel.h>
#include <crug/string.h>

#define TRAP_TEST_MAX 16u

static char g_recipe[TRAP_TEST_MAX];

static int trap_recipe_known(const char *recipe)
{
    static const char *const names[] = {
        "de", "ud", "gp", "pf", "df", "nmi", "mc", "db"
    };
    u32 i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(recipe, names[i]) == 0)
            return 1;
    }
    return 0;
}

static int trap_space(char c)
{
    /* The loader copies cmdline.txt verbatim, so the trailing newline of the
     * token file is part of the last token. */
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int trap_token_value(const char *tok, u32 len, const char *key,
                            const char **val, u32 *vlen)
{
    u32 klen = (u32)strlen(key);

    if (len <= klen || tok[klen] != '=')
        return 0;
    if (strncmp(tok, key, klen) != 0)
        return 0;
    *val = tok + klen + 1;
    *vlen = len - klen - 1;
    return 1;
}

void trap_cmdline(const boot_info *bi)
{
    const char *cmd;
    u32 i = 0;

    if (!bi || !bi->command_line)
        return;
    cmd = bi->command_line;
    while (cmd[i]) {
        u32 start;
        u32 len;
        const char *val = 0;
        u32 vlen = 0;

        while (cmd[i] && trap_space(cmd[i]))
            i++;
        start = i;
        while (cmd[i] && !trap_space(cmd[i]))
            i++;
        len = i - start;
        if (!trap_token_value(cmd + start, len, "trap.test", &val, &vlen))
            continue;
        if (vlen == 0 || vlen >= TRAP_TEST_MAX) {
            kprintf("CRUGOS: trap test unknown\n");
            return;
        }
        memset(g_recipe, 0, sizeof(g_recipe));
        memcpy(g_recipe, val, vlen);
        if (!trap_recipe_known(g_recipe)) {
            kprintf("CRUGOS: trap test unknown %s\n", g_recipe);
            g_recipe[0] = '\0';
        }
        return;
    }
}

void trap_test_run(void)
{
    if (!g_recipe[0])
        return;
    kprintf("CRUGOS: trap test %s\n", g_recipe);

    if (strcmp(g_recipe, "de") == 0) {
        volatile u64 zero = 0;
        volatile u64 sunk;

        sunk = 100u / zero;
        (void)sunk;
    } else if (strcmp(g_recipe, "ud") == 0) {
        __asm__ volatile("ud2");
    } else if (strcmp(g_recipe, "gp") == 0) {
        /* Index 7 (0x3b) lies past the seven-entry GDT, so the %ds load
           faults. Phase U1 turned index 6 / 0x33 into the ring-3 data
           descriptor, which is why this must stay one slot further out. */
        __asm__ volatile("movw $0x3b, %%ax\n\tmovw %%ax, %%ds"
                         : : : "rax", "memory");
    } else if (strcmp(g_recipe, "pf") == 0) {
        *(volatile u64 *)(uintptr)0xffff800000000000ull = 1u;
    } else if (strcmp(g_recipe, "df") == 0) {
        /* Non-canonical %rsp: the push faults, and delivering that #GP onto
           the same broken stack escalates to #DF, which runs on IST1. */
        __asm__ volatile("movabsq $0x0000800000000000, %%rsp\n\tpushq $0"
                         : : : "memory");
    } else if (strcmp(g_recipe, "nmi") == 0) {
        __asm__ volatile("int $2");
    } else if (strcmp(g_recipe, "mc") == 0) {
        __asm__ volatile("int $18");
    } else if (strcmp(g_recipe, "db") == 0) {
        __asm__ volatile("int $1");
    }

    kprintf("CRUGOS: trap test survived %s (unexpected)\n", g_recipe);
}
