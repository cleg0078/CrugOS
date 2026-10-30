/* Host tests for the trap policy and the crash-report formatter (phase H3).
 *
 * trapfmt.c is deliberately free of kprintf/serial/framebuffer so the exact
 * bytes a fatal trap writes can be asserted here: scripts/run-trap.py and the
 * serial log both depend on that shape.
 */
#include <crug/string.h>
#include <crug/trap.h>
#include "test.h"

static void frame_set(trap_frame *f, u64 vector, u64 error, u64 cs)
{
    memset(f, 0, sizeof(*f));
    f->vector = vector;
    f->error = error;
    f->rip = 0x1111;
    f->cs = cs;
    f->rflags = 0x202;
    f->rsp = 0x2222;
    f->ss = 0x10;
    f->rax = 1;
    f->rbx = 2;
    f->rcx = 3;
    f->rdx = 4;
    f->rsi = 5;
    f->rdi = 6;
    f->rbp = 7;
    f->r8 = 8;
    f->r9 = 9;
    f->r10 = 0xa;
    f->r11 = 0xb;
    f->r12 = 0xc;
    f->r13 = 0xd;
    f->r14 = 0xe;
    f->r15 = 0xf;
}

static void test_trap_classify(void)
{
    static const u64 user_killable[] = {0, 1, 6, 11, 12, 13, 14, 16, 17, 19, 21};
    static const u64 fatal_only[] = {2, 5, 7, 8, 9, 10, 15, 18, 20, 22, 23,
                                     24, 25, 26, 27, 28, 29, 30, 31};
    u32 i;

    /* #BP is handled for both rings; #OF kills the user frame that raised
       it and stays fatal in the kernel, because a bad shift count in
       kernel code is a bug, not a recoverable signal. */
    CHECK(trap_classify(3, 0x08) == TRAP_HANDLED);
    CHECK(trap_classify(3, 0x3b) == TRAP_HANDLED);
    CHECK(trap_classify(4, 0x3b) == TRAP_USER);
    CHECK(trap_classify(4, 0x08) == TRAP_FATAL);
    for (i = 0; i < sizeof(user_killable) / sizeof(user_killable[0]); i++) {
        CHECK(trap_classify(user_killable[i], 0x3b) == TRAP_USER);
        CHECK(trap_classify(user_killable[i], 0x08) == TRAP_FATAL);
    }
    for (i = 0; i < sizeof(fatal_only) / sizeof(fatal_only[0]); i++) {
        CHECK(trap_classify(fatal_only[i], 0x3b) == TRAP_FATAL);
        CHECK(trap_classify(fatal_only[i], 0x08) == TRAP_FATAL);
    }
    /* Spurious vectors and anything past the CPU exception range are fatal. */
    CHECK(trap_classify(32, 0x08) == TRAP_FATAL);
    CHECK(trap_classify(32, 0x3b) == TRAP_FATAL);
    CHECK(trap_classify(128, 0x3b) == TRAP_FATAL);
    CHECK(trap_classify(255, 0x3b) == TRAP_FATAL);
}

static void test_trap_ist(void)
{
    CHECK(trap_ist_for(8) == TRAP_IST_DF);
    CHECK(trap_ist_for(2) == TRAP_IST_NMI);
    CHECK(trap_ist_for(18) == TRAP_IST_MC);
    CHECK(trap_ist_for(1) == TRAP_IST_DB);
    CHECK(TRAP_IST_DF == 1u && TRAP_IST_NMI == 2u && TRAP_IST_MC == 3u &&
          TRAP_IST_DB == 4u);
    CHECK(trap_ist_for(0) == 0u);
    CHECK(trap_ist_for(3) == 0u);
    CHECK(trap_ist_for(4) == 0u);
    CHECK(trap_ist_for(13) == 0u);
    CHECK(trap_ist_for(14) == 0u);
    CHECK(trap_ist_for(32) == 0u);
    CHECK(trap_ist_for(128) == 0u);
}

static void test_trap_format_page_fault(void)
{
    trap_frame f;
    struct trap_report_info info;
    char buf[1024];
    usize len;
    const char *expected =
        "CRUGOS: trap report page fault vector=14 error=0x2 ist=0 from=kernel\n"
        "CRUGOS: trap rip=0x1111 cs=0x8 rflags=0x202 rsp=0x2222 ss=0x10\n"
        "CRUGOS: trap fault present=no write=yes user=no reserved=no instruction=no"
        " cr2=0xffff800000000000\n"
        "CRUGOS: trap regs rax=0x1 rbx=0x2 rcx=0x3 rdx=0x4 rsi=0x5 rdi=0x6 rbp=0x7\n"
        "CRUGOS: trap regs r8=0x8 r9=0x9 r10=0xa r11=0xb r12=0xc r13=0xd r14=0xe r15=0xf\n";

    frame_set(&f, 14, 0x2, 0x08);
    info.vector = 14;
    info.error = 0x2;
    info.ist = 0;
    info.cr2 = 0xffff800000000000ull;
    info.has_cr2 = 1;
    info.name = "page fault";
    memset(buf, 0x5a, sizeof(buf));
    len = trap_format(&f, &info, buf, sizeof(buf));
    CHECK(len == strlen(expected));
    CHECK_STR(buf, expected);
    CHECK(buf[len] == '\0');
    CHECK((u8)buf[len + 1u] == 0x5au);

    /* A page fault with a fully set error word and a zero CR2. */
    frame_set(&f, 14, 0x1f, 0x08);
    info.error = 0x1f;
    info.cr2 = 0;
    len = trap_format(&f, &info, buf, sizeof(buf));
    CHECK(strstr(buf, "trap fault present=yes write=yes user=yes reserved=yes") != 0);
    CHECK(strstr(buf, "instruction=yes cr2=0x0\n") != 0);
    (void)len;
}

static void test_trap_format_no_fault_line(void)
{
    trap_frame f;
    struct trap_report_info info;
    char buf[1024];
    usize len;

    /* #GP carries no CR2, so the fault line must be absent. */
    frame_set(&f, 13, 0x0, 0x08);
    info.vector = 13;
    info.error = 0;
    info.ist = 0;
    info.cr2 = 0;
    info.has_cr2 = 0;
    info.name = "general protection";
    len = trap_format(&f, &info, buf, sizeof(buf));
    CHECK_STR(strstr(buf, "CRUGOS: trap report general protection vector=13"),
              "CRUGOS: trap report general protection vector=13 error=0x0 ist=0 from=kernel\nCRUGOS: trap rip=0x1111 cs=0x8 rflags=0x202 rsp=0x2222 ss=0x10\nCRUGOS: trap regs rax=0x1 rbx=0x2 rcx=0x3 rdx=0x4 rsi=0x5 rdi=0x6 rbp=0x7\nCRUGOS: trap regs r8=0x8 r9=0x9 r10=0xa r11=0xb r12=0xc r13=0xd r14=0xe r15=0xf\n");
    CHECK(strstr(buf, "trap fault") == 0);
    CHECK(len == strlen(buf));
}

static void test_trap_format_user_and_unnamed(void)
{
    trap_frame f;
    struct trap_report_info info;
    char buf[512];

    /* A ring-3 frame reports from=user and the IST it was delivered on. */
    frame_set(&f, 14, 0x6, 0x3b);
    info.vector = 14;
    info.error = 0x6;
    info.ist = 0;
    info.cr2 = 0x4000;
    info.has_cr2 = 1;
    info.name = "page fault";
    trap_format(&f, &info, buf, sizeof(buf));
    CHECK(strstr(buf, "from=user") != 0);
    CHECK(strstr(buf, "cr2=0x4000") != 0);

    /* A missing name renders as "unknown" instead of a null dereference. */
    info.name = 0;
    trap_format(&f, &info, buf, sizeof(buf));
    CHECK(strstr(buf, "CRUGOS: trap report unknown vector=14") != 0);
}

static void test_trap_format_bounds(void)
{
    trap_frame f;
    struct trap_report_info info;
    char buf[96];
    usize len;
    u32 i;

    frame_set(&f, 14, 0x2, 0x08);
    info.vector = 14;
    info.error = 0x2;
    info.ist = 0;
    info.cr2 = 0xdeadbeef;
    info.has_cr2 = 1;
    info.name = "page fault";

    /* A short buffer truncates on a byte boundary and stays NUL terminated. */
    memset(buf, 0x7e, sizeof(buf));
    len = trap_format(&f, &info, buf, 64);
    CHECK(len == 63u);
    CHECK(buf[62] == '\n' || buf[62] == ' ' || buf[62] != '\0');
    CHECK(buf[63] == '\0');
    CHECK((u8)buf[64] == 0x7eu);
    for (i = 0; i < 63u; i++)
        CHECK(buf[i] != '\0' && buf[i] != (char)0x7e);

    /* Degenerate arguments write nothing. */
    CHECK(trap_format(&f, &info, buf, 0) == 0u);
    CHECK(trap_format(&f, &info, 0, sizeof(buf)) == 0u);
    CHECK(trap_format(0, &info, buf, sizeof(buf)) == 0u);
    CHECK(trap_format(&f, 0, buf, sizeof(buf)) == 0u);

    /* A one-byte buffer can only hold the terminator. */
    buf[0] = 'x';
    CHECK(trap_format(&f, &info, buf, 1) == 0u);
    CHECK(buf[0] == '\0');
}

void test_trap_functions(void)
{
    test_trap_classify();
    test_trap_ist();
    test_trap_format_page_fault();
    test_trap_format_no_fault_line();
    test_trap_format_user_and_unnamed();
    test_trap_format_bounds();
}
