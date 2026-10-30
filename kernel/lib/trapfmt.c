/* Exception policy and crash-report formatting (phase H3).
 *
 * Kept free of kprintf, serial and framebuffer so the host tests can check
 * the exact bytes a fatal trap writes.  The report is a fixed set of lines so
 * scripts/run-trap.py and a human reading the serial log agree on the shape:
 *
 *   CRUGOS: trap report <name> vector=<dec> error=0x<hex> ist=<dec> from=<kernel|user>
 *   CRUGOS: trap rip=0x.. cs=0x.. rflags=0x.. rsp=0x.. ss=0x..
 *   CRUGOS: trap fault present=.. write=.. user=.. reserved=.. instruction=.. cr2=0x..
 *   CRUGOS: trap regs rax=0x.. rbx=0x.. rcx=0x.. rdx=0x.. rsi=0x.. rdi=0x.. rbp=0x..
 *   CRUGOS: trap regs r8=0x.. r9=0x.. r10=0x.. r11=0x.. r12=0x.. r13=0x.. r14=0x.. r15=0x..
 *
 * The fault line is printed only for a page fault (#PF, vector 14).  Hex is
 * lowercase and unpadded.  See docs/traps.md. */
#include <crug/trap.h>

/* Bounded writer: never stores past cap-1 bytes and always leaves room for
   the terminating NUL the caller appends. */
struct writer {
    char *out;
    usize cap;
    usize len;
};

static void w_char(struct writer *w, char c)
{
    if (w->len + 1u < w->cap) {
        w->out[w->len] = c;
        w->len++;
    }
}

static void w_str(struct writer *w, const char *s)
{
    while (*s)
        w_char(w, *s++);
}

static void w_hex(struct writer *w, u64 value)
{
    char tmp[16];
    u32 n = 0;

    if (value == 0) {
        w_char(w, '0');
        return;
    }
    while (value) {
        u32 digit = (u32)(value & 0xfu);

        tmp[n++] = (char)(digit < 10u ? '0' + digit : 'a' + (digit - 10u));
        value >>= 4;
    }
    while (n)
        w_char(w, tmp[--n]);
}

static void w_dec(struct writer *w, u64 value)
{
    char tmp[20];
    u32 n = 0;

    if (value == 0) {
        w_char(w, '0');
        return;
    }
    while (value) {
        tmp[n++] = (char)('0' + (u32)(value % 10u));
        value /= 10u;
    }
    while (n)
        w_char(w, tmp[--n]);
}

static void w_flag(struct writer *w, const char *name, u64 bit)
{
    w_str(w, name);
    w_str(w, bit ? "yes" : "no");
}

/* Faults a ring-3 thread could survive: it would be killed, the kernel would
   keep running.  Everything else is a kernel bug and must panic. */
static int trap_user_killable(u64 vector)
{
    switch (vector) {
    case 0:  /* divide error */
    case 1:  /* debug */
    case 6:  /* invalid opcode */
    case 11: /* segment not present */
    case 12: /* stack-segment fault */
    case 13: /* general protection */
    case 14: /* page fault */
    case 16: /* x87 FPU error */
    case 17: /* alignment check */
    case 19: /* SIMD FPU error */
    case 21: /* control protection */
        return 1;
    default:
        return 0;
    }
}

enum trap_class trap_classify(u64 vector, u64 cs)
{
    int user = (cs & 3u) == 3u;

    if (vector == 3)
        return TRAP_HANDLED;
    if (vector == 4)
        return user ? TRAP_USER : TRAP_FATAL;
    if (trap_user_killable(vector))
        return user ? TRAP_USER : TRAP_FATAL;
    return TRAP_FATAL;
}

u32 trap_ist_for(u64 vector)
{
    switch (vector) {
    case 8:
        return TRAP_IST_DF;
    case 2:
        return TRAP_IST_NMI;
    case 18:
        return TRAP_IST_MC;
    case 1:
        return TRAP_IST_DB;
    default:
        return 0;
    }
}

usize trap_format(const trap_frame *frame, const struct trap_report_info *info,
                  char *out, usize cap)
{
    struct writer w;
    const char *name;

    if (!out || cap == 0 || !frame || !info)
        return 0;
    w.out = out;
    w.cap = cap;
    w.len = 0;
    name = info->name ? info->name : "unknown";

    w_str(&w, "CRUGOS: trap report ");
    w_str(&w, name);
    w_str(&w, " vector=");
    w_dec(&w, info->vector);
    w_str(&w, " error=0x");
    w_hex(&w, info->error);
    w_str(&w, " ist=");
    w_dec(&w, (u64)info->ist);
    w_str(&w, " from=");
    w_str(&w, (frame->cs & 3u) == 3u ? "user" : "kernel");
    w_char(&w, '\n');

    w_str(&w, "CRUGOS: trap rip=0x");
    w_hex(&w, frame->rip);
    w_str(&w, " cs=0x");
    w_hex(&w, frame->cs);
    w_str(&w, " rflags=0x");
    w_hex(&w, frame->rflags);
    w_str(&w, " rsp=0x");
    w_hex(&w, frame->rsp);
    w_str(&w, " ss=0x");
    w_hex(&w, frame->ss);
    w_char(&w, '\n');

    if (info->has_cr2) {
        w_str(&w, "CRUGOS: trap fault ");
        w_flag(&w, "present=", info->error & 1u);
        w_char(&w, ' ');
        w_flag(&w, "write=", (info->error >> 1) & 1u);
        w_char(&w, ' ');
        w_flag(&w, "user=", (info->error >> 2) & 1u);
        w_char(&w, ' ');
        w_flag(&w, "reserved=", (info->error >> 3) & 1u);
        w_char(&w, ' ');
        w_flag(&w, "instruction=", (info->error >> 4) & 1u);
        w_str(&w, " cr2=0x");
        w_hex(&w, info->cr2);
        w_char(&w, '\n');
    }

    w_str(&w, "CRUGOS: trap regs rax=0x");
    w_hex(&w, frame->rax);
    w_str(&w, " rbx=0x");
    w_hex(&w, frame->rbx);
    w_str(&w, " rcx=0x");
    w_hex(&w, frame->rcx);
    w_str(&w, " rdx=0x");
    w_hex(&w, frame->rdx);
    w_str(&w, " rsi=0x");
    w_hex(&w, frame->rsi);
    w_str(&w, " rdi=0x");
    w_hex(&w, frame->rdi);
    w_str(&w, " rbp=0x");
    w_hex(&w, frame->rbp);
    w_char(&w, '\n');

    w_str(&w, "CRUGOS: trap regs r8=0x");
    w_hex(&w, frame->r8);
    w_str(&w, " r9=0x");
    w_hex(&w, frame->r9);
    w_str(&w, " r10=0x");
    w_hex(&w, frame->r10);
    w_str(&w, " r11=0x");
    w_hex(&w, frame->r11);
    w_str(&w, " r12=0x");
    w_hex(&w, frame->r12);
    w_str(&w, " r13=0x");
    w_hex(&w, frame->r13);
    w_str(&w, " r14=0x");
    w_hex(&w, frame->r14);
    w_str(&w, " r15=0x");
    w_hex(&w, frame->r15);
    w_char(&w, '\n');

    out[w.len] = '\0';
    return w.len;
}
