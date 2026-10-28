/* Host tests for the phase 6C Workbench shell (kernel/shell/shell.c): the
   line editor, history, tokenizer and every command against an in-memory
   shell_io.  The text grid is real (kernel/lib/textgrid.c); the framebuffer
   is the fake already linked for the other graphics tests. */
#include <crug/types.h>
#include <crug/kernel.h>
#include <crug/shell.h>
#include <crug/textgrid.h>
#include <crug/version.h>
#include <string.h>
#include "test.h"

#define SINK_MAX 64

static struct textgrid g_grid;
static struct shell g_shell;
static char g_sink[SINK_MAX][SHELL_LINE_MAX + 32];
static u32 g_sink_count;
static char g_written[128];
static u32 g_write_count;
static u32 g_sync_count;
static char g_ran[128];

static const char *g_cat_text = "first line\nsecond line\n";
static const char *g_list_text = "hello.txt\ntmp";

static void sink_line(const char *text, void *ctx)
{
    (void)ctx;
    if (g_sink_count >= SINK_MAX)
        return;
    strncpy(g_sink[g_sink_count], text, sizeof(g_sink[0]) - 1u);
    g_sink[g_sink_count][sizeof(g_sink[0]) - 1u] = 0;
    g_sink_count++;
}

static void run_hook(const char *line, void *ctx)
{
    (void)ctx;
    strncpy(g_ran, line, sizeof(g_ran) - 1u);
    g_ran[sizeof(g_ran) - 1u] = 0;
}

static int fake_list(const char *path, char *out, usize cap)
{
    usize len = strlen(g_list_text);

    if (strcmp(path, "/") != 0 && strcmp(path, "/tmp") != 0)
        return -1;
    if (len + 1u > cap)
        return -1;
    memcpy(out, g_list_text, len + 1u);
    return (int)len;
}

static int fake_read(const char *path, char *out, usize cap, u64 *out_len)
{
    usize len = strlen(g_cat_text);

    *out_len = len;
    if (strcmp(path, "/hello.txt") != 0)
        return -1;
    if (len > cap)
        len = cap;
    memcpy(out, g_cat_text, len);
    return 0;
}

static int fake_write(const char *path, const char *data, usize len)
{
    (void)path;
    if (len >= sizeof(g_written))
        return -1;
    memcpy(g_written, data, len);
    g_written[len] = 0;
    g_write_count++;
    return 0;
}

static int fake_sync(void)
{
    g_sync_count++;
    return 0;
}

static void fake_info(struct shell_info *info)
{
    info->uptime_ns = 1500000000ull;
    info->heap_used = 8976ull;
    info->heap_total = 57344ull;
    info->cur_thread = "boot";
    info->switches = 112ull;
}

static const struct shell_io g_io = {
    fake_list, fake_read, fake_write, fake_sync, fake_info
};

static void reset(void)
{
    u32 i;

    tg_init(&g_grid);
    shell_init(&g_shell, &g_grid, &g_io);
    shell_set_sink(&g_shell, sink_line, NULL);
    shell_set_run_hook(&g_shell, run_hook, NULL);
    for (i = 0; i < SINK_MAX; i++)
        g_sink[i][0] = 0;
    g_sink_count = 0;
    g_write_count = 0;
    g_sync_count = 0;
    g_ran[0] = 0;
    g_written[0] = 0;
}

static void type_line(const char *line)
{
    while (*line)
        shell_feed(&g_shell, (u8)*line++);
    shell_feed(&g_shell, (u8)'\n');
}

static int sink_has(const char *needle)
{
    u32 i;

    for (i = 0; i < g_sink_count; i++)
        if (strstr(g_sink[i], needle))
            return 1;
    return 0;
}

static void test_shell_init(void)
{
    char buf[TG_COLS];

    reset();
    CHECK(tg_line_count(&g_grid) == 1u);
    tg_line(&g_grid, 0, buf, sizeof(buf));
    CHECK_STR(buf, SHELL_PROMPT);
    CHECK(shell_commands(&g_shell) == 0u);
    CHECK(shell_commands(NULL) == 0u);
    CHECK(shell_run(NULL, "help") == -1);
    CHECK(shell_run(&g_shell, NULL) == -1);
    CHECK(shell_feed(NULL, 'a') == 0);
}

static void test_shell_line_editing(void)
{
    reset();
    CHECK(shell_feed(&g_shell, 'a') == 0);
    CHECK(shell_feed(&g_shell, 'b') == 0);
    CHECK(shell_feed(&g_shell, 0x7F) == 0);
    CHECK_STR(g_shell.line, "a");
    CHECK((u32)strlen(g_shell.line) == g_shell.len);

    CHECK(shell_feed(&g_shell, 0x15) == 0); /* Ctrl-U */
    CHECK(g_shell.len == 0u);
    CHECK_STR(g_shell.line, "");

    /* The empty line still executes, but nothing runs. */
    CHECK(shell_feed(&g_shell, (u8)'\n') == 0);
    CHECK(shell_commands(&g_shell) == 0u);
    CHECK(g_sink_count == 0u);

    /* A cancelled line never reaches the command table. */
    type_line("help");
    CHECK(shell_commands(&g_shell) == 1u);
    reset();
    shell_feed(&g_shell, 'x');
    shell_feed(&g_shell, 'y');
    CHECK(shell_feed(&g_shell, 0x03) == 2);
    CHECK(g_shell.len == 0u);
    CHECK(shell_commands(&g_shell) == 0u);
    CHECK(sink_has("^C"));

    /* The editor refuses to overflow its fixed line buffer. */
    reset();
    while (g_shell.len < SHELL_LINE_MAX - 1u)
        shell_feed(&g_shell, 'x');
    shell_feed(&g_shell, 'y');
    CHECK(g_shell.len == SHELL_LINE_MAX - 1u);
}

static void test_shell_help_and_ver(void)
{
    reset();
    CHECK(shell_run(&g_shell, "help") == 0);
    CHECK(sink_has("commands:"));
    CHECK(sink_has("show this command list"));
    CHECK(sink_has("kernel version and phase"));
    CHECK(sink_has("clear the scrollback"));

    reset();
    CHECK(shell_run(&g_shell, "ver") == 0);
    CHECK(sink_has("CrugOS "));
    CHECK(sink_has("(phase " CRUG_PHASE_STR ")"));
    CHECK(sink_has("x86_64"));
}

static void test_shell_unknown(void)
{
    reset();
    CHECK(shell_run(&g_shell, "bogus") == 1);
    CHECK(shell_commands(&g_shell) == 1u);
    CHECK_STR(g_sink[0], "sh: bogus: unknown command");
    CHECK(shell_run(&g_shell, "   ") == -1);
}

static void test_shell_echo(void)
{
    reset();
    CHECK(shell_run(&g_shell, "echo hello world") == 0);
    CHECK_STR(g_sink[0], "hello world");

    /* More words than SHELL_ARGS_MAX keep their tail in the last token. */
    reset();
    CHECK(shell_run(&g_shell, "echo a b c d e f g h i j") == 0);
    CHECK_STR(g_sink[0], "a b c d e f g h i j");

    reset();
    CHECK(shell_run(&g_shell, "echo") == 0);
    CHECK_STR(g_sink[0], "");
}

static void test_shell_ls(void)
{
    reset();
    CHECK(shell_run(&g_shell, "ls /") == 0);
    CHECK_STR(g_sink[0], "  hello.txt");
    CHECK_STR(g_sink[1], "  tmp");

    reset();
    CHECK(shell_run(&g_shell, "ls /tmp") == 0);
    CHECK_STR(g_sink[0], "  hello.txt");

    reset();
    /* A known command always reports "ran"; the error text is the
       command's own business. */
    CHECK(shell_run(&g_shell, "ls /missing") == 0);
    CHECK_STR(g_sink[0], "ls: /missing: cannot read");
}

static void test_shell_cat(void)
{
    reset();
    CHECK(shell_run(&g_shell, "cat /hello.txt") == 0);
    CHECK_STR(g_sink[0], "first line");
    CHECK_STR(g_sink[1], "second line");

    reset();
    CHECK(shell_run(&g_shell, "cat") == 0);
    CHECK_STR(g_sink[0], "usage: cat <path>");

    reset();
    CHECK(shell_run(&g_shell, "cat /missing") == 0);
    CHECK_STR(g_sink[0], "cat: /missing: cannot read");
}

static void test_shell_write_and_sync(void)
{
    reset();
    CHECK(shell_run(&g_shell, "write /new.txt abc def") == 0);
    CHECK(g_write_count == 1u);
    CHECK_STR(g_written, "abc def");
    CHECK(sink_has("wrote 7 byte(s) to /new.txt"));

    reset();
    CHECK(shell_run(&g_shell, "write") == 0);
    CHECK_STR(g_sink[0], "usage: write <path> <words>");

    reset();
    CHECK(shell_run(&g_shell, "sync") == 0);
    CHECK(g_sync_count == 1u);
    CHECK_STR(g_sink[0], "synced");
}

static void test_shell_info_commands(void)
{
    reset();
    CHECK(shell_run(&g_shell, "mem") == 0);
    CHECK_STR(g_sink[0], "heap: 8976 of 57344 bytes used");

    reset();
    CHECK(shell_run(&g_shell, "uptime") == 0);
    CHECK_STR(g_sink[0], "uptime 1.500 s");

    reset();
    CHECK(shell_run(&g_shell, "ps") == 0);
    CHECK_STR(g_sink[0], "thread boot, 112 context switch(es)");
}

static void test_shell_history(void)
{
    reset();
    type_line("echo one");
    type_line("echo two");
    CHECK(shell_commands(&g_shell) == 2u);
    CHECK(g_shell.history_count == 2u);

    /* Ctrl-P walks back: newest first. */
    shell_feed(&g_shell, 0x10);
    CHECK_STR(g_shell.line, "echo two");
    shell_feed(&g_shell, 0x10);
    CHECK_STR(g_shell.line, "echo one");
    shell_feed(&g_shell, 0x10); /* at the oldest entry: stays put */
    CHECK_STR(g_shell.line, "echo one");

    /* Ctrl-N walks forward again and ends on the empty draft. */
    shell_feed(&g_shell, 0x0E);
    CHECK_STR(g_shell.line, "echo two");
    shell_feed(&g_shell, 0x0E);
    CHECK_STR(g_shell.line, "");
    CHECK(g_shell.len == 0u);

    /* A recalled line executes like a typed one. */
    shell_feed(&g_shell, 0x10);
    CHECK(shell_feed(&g_shell, (u8)'\n') == 1);
    CHECK(shell_commands(&g_shell) == 3u);
    CHECK_STR(g_ran, "echo two");
}

static void test_shell_history_limit(void)
{
    u32 i;
    char line[32];

    reset();
    for (i = 0; i < SHELL_HISTORY + 3u; i++) {
        ksnprintf(line, sizeof(line), "echo %u", i);
        type_line(line);
    }
    CHECK(g_shell.history_count == SHELL_HISTORY);
    /* The oldest three entries were dropped. */
    CHECK_STR(g_shell.history[0], "echo 3");
    CHECK_STR(g_shell.history[SHELL_HISTORY - 1u], "echo 10");
}

static void test_shell_run_hook_and_cls(void)
{
    reset();
    type_line("ver");
    CHECK_STR(g_ran, "ver");
    CHECK(tg_line_count(&g_grid) > 1u);
    CHECK(shell_run(&g_shell, "cls") == 0);
    CHECK(tg_line_count(&g_grid) == 1u);
}

static void test_shell_feed_returns(void)
{
    reset();
    type_line("help");
    CHECK(shell_feed(&g_shell, (u8)'\n') == 0);
}

void test_shell_functions(void)
{
    test_shell_init();
    test_shell_line_editing();
    test_shell_help_and_ver();
    test_shell_unknown();
    test_shell_echo();
    test_shell_ls();
    test_shell_cat();
    test_shell_write_and_sync();
    test_shell_info_commands();
    test_shell_history();
    test_shell_history_limit();
    test_shell_run_hook_and_cls();
    test_shell_feed_returns();
}
