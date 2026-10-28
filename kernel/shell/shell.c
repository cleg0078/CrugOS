/* The Workbench shell: line editor, history and command table.  Rendering
   goes through the textgrid so this file has no framebuffer dependency. */
#include <crug/types.h>
#include <crug/string.h>
#include <crug/kernel.h>
#include <crug/version.h>
#include <crug/shell.h>

#define SHELL_OUT_MAX 256u
#define SHELL_LIST_MAX 1024u
#define SHELL_CAT_LINES 24u

static char g_out[SHELL_OUT_MAX];
static char g_list[SHELL_LIST_MAX];
static char g_cat[SHELL_READ_MAX];

static void sh_line(struct shell *sh, const char *text)
{
    tg_puts(sh->tg, text);
    tg_putc(sh->tg, '\n');
    if (sh->sink)
        sh->sink(text, sh->sink_ctx);
}

static void sh_printf(struct shell *sh, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    kvsnprintf(g_out, sizeof(g_out), fmt, ap);
    va_end(ap);
    sh_line(sh, g_out);
}

static void sh_prompt(struct shell *sh)
{
    tg_puts(sh->tg, SHELL_PROMPT);
}

static void sh_history_push(struct shell *sh, const char *line)
{
    if (line[0] == 0)
        return;
    if (sh->history_count == SHELL_HISTORY) {
        memmove(sh->history[0], sh->history[1],
                (SHELL_HISTORY - 1u) * SHELL_LINE_MAX);
        sh->history_count--;
    }
    strncpy(sh->history[sh->history_count], line, SHELL_LINE_MAX - 1u);
    sh->history[sh->history_count][SHELL_LINE_MAX - 1u] = 0;
    sh->history_count++;
}

/* Replace the edited line, keeping the grid in sync. */
static void sh_replace_line(struct shell *sh, const char *text)
{
    u32 i;

    for (i = 0; i < sh->len; i++)
        tg_backspace(sh->tg);
    strncpy(sh->line, text, SHELL_LINE_MAX - 1u);
    sh->line[SHELL_LINE_MAX - 1u] = 0;
    sh->len = (u32)strlen(sh->line);
    tg_puts(sh->tg, sh->line);
}

/* ---- commands ---------------------------------------------------- */

struct shell_cmd {
    const char *name;
    const char *usage;
    int (*fn)(struct shell *sh, int argc, char **argv);
};

static int cmd_help(struct shell *sh, int argc, char **argv);
static int cmd_ver(struct shell *sh, int argc, char **argv);
static int cmd_mem(struct shell *sh, int argc, char **argv);
static int cmd_uptime(struct shell *sh, int argc, char **argv);
static int cmd_ps(struct shell *sh, int argc, char **argv);
static int cmd_ls(struct shell *sh, int argc, char **argv);
static int cmd_cat(struct shell *sh, int argc, char **argv);
static int cmd_echo(struct shell *sh, int argc, char **argv);
static int cmd_write(struct shell *sh, int argc, char **argv);
static int cmd_sync(struct shell *sh, int argc, char **argv);
static int cmd_cls(struct shell *sh, int argc, char **argv);

static const struct shell_cmd g_commands[] = {
    { "help", "show this command list", cmd_help },
    { "ver", "kernel version and phase", cmd_ver },
    { "mem", "heap usage", cmd_mem },
    { "uptime", "time since the monotonic clock started", cmd_uptime },
    { "ps", "current thread and switch count", cmd_ps },
    { "ls", "ls [path]  list a directory", cmd_ls },
    { "cat", "cat <path>  print a file", cmd_cat },
    { "echo", "echo <words>  print the words", cmd_echo },
    { "write", "write <path> <words>  create or overwrite a file", cmd_write },
    { "sync", "flush the filesystem", cmd_sync },
    { "cls", "clear the scrollback", cmd_cls },
};

#define SHELL_COMMAND_COUNT (sizeof(g_commands) / sizeof(g_commands[0]))

static int cmd_help(struct shell *sh, int argc, char **argv)
{
    u32 i;

    (void)argc;
    (void)argv;
    sh_line(sh, "commands:");
    for (i = 0; i < SHELL_COMMAND_COUNT; i++)
        sh_printf(sh, "  %s", g_commands[i].usage);
    return 0;
}

static int cmd_ver(struct shell *sh, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sh_printf(sh, "CrugOS %s (phase %s), x86_64", CRUG_VERSION_STR,
              CRUG_PHASE_STR);
    return 0;
}

static int cmd_mem(struct shell *sh, int argc, char **argv)
{
    struct shell_info info;

    (void)argc;
    (void)argv;
    memset(&info, 0, sizeof(info));
    if (sh->io && sh->io->info)
        sh->io->info(&info);
    sh_printf(sh, "heap: %llu of %llu bytes used", info.heap_used,
              info.heap_total);
    return 0;
}

static int cmd_uptime(struct shell *sh, int argc, char **argv)
{
    struct shell_info info;
    u64 secs;
    u64 frac;

    (void)argc;
    (void)argv;
    memset(&info, 0, sizeof(info));
    if (sh->io && sh->io->info)
        sh->io->info(&info);
    secs = info.uptime_ns / 1000000000ull;
    frac = (info.uptime_ns / 1000000ull) % 1000ull;
    sh_printf(sh, "uptime %llu.%03llu s", secs, frac);
    return 0;
}

static int cmd_ps(struct shell *sh, int argc, char **argv)
{
    struct shell_info info;

    (void)argc;
    (void)argv;
    memset(&info, 0, sizeof(info));
    if (sh->io && sh->io->info)
        sh->io->info(&info);
    sh_printf(sh, "thread %s, %llu context switch(es)",
              info.cur_thread ? info.cur_thread : "?", info.switches);
    return 0;
}

static int cmd_ls(struct shell *sh, int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    int n;
    usize i;

    if (!sh->io || !sh->io->list) {
        sh_line(sh, "ls: no filesystem");
        return -1;
    }
    n = sh->io->list(path, g_list, sizeof(g_list));
    if (n < 0) {
        sh_printf(sh, "ls: %s: cannot read", path);
        return -1;
    }
    if (n == 0) {
        sh_line(sh, "(empty)");
        return 0;
    }
    i = 0;
    while (i < (usize)n) {
        usize start = i;

        while (i < (usize)n && g_list[i] != '\n')
            i++;
        g_list[i] = 0;
        if (i > start) {
            ksnprintf(g_out, sizeof(g_out), "  %s", g_list + start);
            sh_line(sh, g_out);
        }
        i++;
    }
    return 0;
}

static int cmd_cat(struct shell *sh, int argc, char **argv)
{
    u64 len = 0;
    u64 i = 0;
    u32 lines = 0;

    if (argc < 2) {
        sh_line(sh, "usage: cat <path>");
        return -1;
    }
    if (!sh->io || !sh->io->read) {
        sh_line(sh, "cat: no filesystem");
        return -1;
    }
    if (sh->io->read(argv[1], g_cat, sizeof(g_cat), &len) != 0) {
        sh_printf(sh, "cat: %s: cannot read", argv[1]);
        return -1;
    }
    if (len == 0) {
        sh_line(sh, "(empty)");
        return 0;
    }
    while (i < len) {
        usize start = (usize)i;
        usize stop;

        while (i < len && g_cat[i] != '\n')
            i++;
        stop = (usize)i;
        if (stop > start && g_cat[stop - 1u] == '\r')
            stop--;
        if (stop - start >= sizeof(g_out))
            stop = start + sizeof(g_out) - 1u;
        memcpy(g_out, g_cat + start, stop - start);
        g_out[stop - start] = 0;
        sh_line(sh, g_out);
        lines++;
        i++;
        if (lines >= SHELL_CAT_LINES && i < len) {
            sh_line(sh, "... (truncated)");
            break;
        }
    }
    return 0;
}

static int cmd_echo(struct shell *sh, int argc, char **argv)
{
    usize used = 0;
    int i;

    for (i = 1; i < argc; i++) {
        usize len = (usize)strlen(argv[i]);

        if (used + len + 2u >= sizeof(g_out))
            break;
        if (used > 0)
            g_out[used++] = ' ';
        memcpy(g_out + used, argv[i], len);
        used += len;
    }
    g_out[used] = 0;
    sh_line(sh, g_out);
    return 0;
}

static int cmd_write(struct shell *sh, int argc, char **argv)
{
    usize used = 0;
    int i;

    if (argc < 2) {
        sh_line(sh, "usage: write <path> <words>");
        return -1;
    }
    if (!sh->io || !sh->io->write) {
        sh_line(sh, "write: no filesystem");
        return -1;
    }
    for (i = 2; i < argc; i++) {
        usize len = (usize)strlen(argv[i]);

        if (used + len + 2u >= sizeof(g_out))
            break;
        if (used > 0)
            g_out[used++] = ' ';
        memcpy(g_out + used, argv[i], len);
        used += len;
    }
    g_out[used] = 0;
    if (sh->io->write(argv[1], g_out, used) != 0) {
        sh_printf(sh, "write: %s: cannot write", argv[1]);
        return -1;
    }
    sh_printf(sh, "wrote %u byte(s) to %s", (u32)used, argv[1]);
    return 0;
}

static int cmd_sync(struct shell *sh, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!sh->io || !sh->io->sync || sh->io->sync() != 0) {
        sh_line(sh, "sync failed");
        return -1;
    }
    sh_line(sh, "synced");
    return 0;
}

static int cmd_cls(struct shell *sh, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    tg_clear(sh->tg);
    return 0;
}

/* Split a line in place; returns the argument count. */
static int sh_tokenize(char *line, char **argv)
{
    int argc = 0;

    while (*line) {
        while (*line == ' ' || *line == '\t')
            *line++ = 0;
        if (!*line)
            break;
        if (argc >= (int)SHELL_ARGS_MAX - 1) {
            /* Fold the remainder into the last argument so echo and write
               keep long text instead of dropping it. */
            argv[SHELL_ARGS_MAX - 1] = line;
            return (int)SHELL_ARGS_MAX;
        }
        argv[argc++] = line;
        while (*line && *line != ' ' && *line != '\t')
            line++;
    }
    return argc;
}

int shell_run(struct shell *sh, const char *line)
{
    char work[SHELL_LINE_MAX];
    char *argv[SHELL_ARGS_MAX];
    int argc;
    u32 i;

    if (!sh || !line)
        return -1;
    strncpy(work, line, sizeof(work) - 1u);
    work[sizeof(work) - 1u] = 0;
    argc = sh_tokenize(work, argv);
    if (argc == 0)
        return -1;
    sh->commands++;
    for (i = 0; i < SHELL_COMMAND_COUNT; i++) {
        if (strcmp(argv[0], g_commands[i].name) == 0) {
            g_commands[i].fn(sh, argc, argv);
            return 0;
        }
    }
    sh_printf(sh, "sh: %s: unknown command", argv[0]);
    return 1;
}

void shell_init(struct shell *sh, struct textgrid *tg,
                const struct shell_io *io)
{
    if (!sh)
        return;
    memset(sh, 0, sizeof(*sh));
    sh->tg = tg;
    sh->io = io;
    sh_prompt(sh);
}

void shell_set_sink(struct shell *sh, void (*sink)(const char *text, void *ctx),
                    void *ctx)
{
    if (!sh)
        return;
    sh->sink = sink;
    sh->sink_ctx = ctx;
}

void shell_set_run_hook(struct shell *sh,
                        void (*on_run)(const char *line, void *ctx),
                        void *ctx)
{
    if (!sh)
        return;
    sh->on_run = on_run;
    sh->run_ctx = ctx;
}

u32 shell_commands(const struct shell *sh)
{
    return sh ? sh->commands : 0;
}

int shell_feed(struct shell *sh, u8 c)
{
    u32 i;

    if (!sh || !sh->tg)
        return 0;
    if (c == '\n' || c == '\r') {
        char executed[SHELL_LINE_MAX];

        tg_putc(sh->tg, '\n');
        strncpy(executed, sh->line, sizeof(executed) - 1u);
        executed[sizeof(executed) - 1u] = 0;
        sh->len = 0;
        sh->line[0] = 0;
        sh->browse = 0;
        sh_history_push(sh, executed);
        if (executed[0] && sh->on_run)
            sh->on_run(executed, sh->run_ctx);
        if (executed[0])
            shell_run(sh, executed);
        sh_prompt(sh);
        return executed[0] ? 1 : 0;
    }
    if (c == 0x03) { /* Ctrl-C cancels the line being edited. */
        sh_line(sh, "^C");
        sh->len = 0;
        sh->line[0] = 0;
        sh->browse = 0;
        sh_prompt(sh);
        return 2;
    }
    if (c == 0x15) { /* Ctrl-U erases the line being edited. */
        for (i = 0; i < sh->len; i++)
            tg_backspace(sh->tg);
        sh->len = 0;
        sh->line[0] = 0;
        sh->browse = 0;
        return 0;
    }
    if (c == 0x10) { /* Ctrl-P: walk back through history. */
        if (sh->browse == 0 && sh->len > 0) {
            strncpy(sh->draft, sh->line, sizeof(sh->draft) - 1u);
            sh->draft[sizeof(sh->draft) - 1u] = 0;
        }
        if (sh->browse < sh->history_count) {
            sh->browse++;
            sh_replace_line(sh, sh->history[sh->history_count - sh->browse]);
        }
        return 0;
    }
    if (c == 0x0E) { /* Ctrl-N: walk forward, back to the draft. */
        if (sh->browse > 0) {
            sh->browse--;
            sh_replace_line(sh, sh->browse == 0
                                    ? sh->draft
                                    : sh->history[sh->history_count - sh->browse]);
        }
        return 0;
    }
    if (c == 0x08 || c == 0x7F) {
        if (sh->len > 0) {
            sh->len--;
            sh->line[sh->len] = 0;
            tg_backspace(sh->tg);
        }
        return 0;
    }
    if (c < 0x20 || c > 0x7E)
        return 0;
    if (sh->len + 1u >= SHELL_LINE_MAX)
        return 0;
    sh->line[sh->len++] = (char)c;
    sh->line[sh->len] = 0;
    tg_putc(sh->tg, c);
    return 0;
}