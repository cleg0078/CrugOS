/* CrugOS ring-3 shell (phase U3).
 *
 * The first program that uses the file syscalls: it reads a script from the
 * root filesystem, runs one command per line, writes a file, and exits with
 * the code the script asks for. Everything it touches goes through SYS_OPEN,
 * SYS_READ, SYS_WRITE, SYS_CLOSE and SYS_LISTDIR, so a passing run proves the
 * user/kernel file path end to end (see docs/userspace.md).
 */
#include "lib.h"

#define SH_LINE_MAX 256u
#define SH_ARG_MAX 8u
#define SH_SCRIPT_MAX 4096u
#define SH_IO_CHUNK 512u

static char g_script[SH_SCRIPT_MAX];
static char g_line[SH_LINE_MAX];
static char g_io[SH_IO_CHUNK];

static void sh_nl(void)
{
    char c = (char)10;

    (void)u_write(1u, &c, 1u);
}

static void sh_line(const char *s)
{
    u_puts(s);
    sh_nl();
}

static void sh_marker(const char *lead, const char *word, i64 number,
                      const char *tail)
{
    u_puts(lead);
    if (word)
        u_puts(word);
    if (number >= 0) {
        u_puts(" ");
        u_putnum((u64)number);
    }
    u_puts(tail);
    sh_nl();
}

static int sh_space(char c)
{
    return c == (char)32 || c == (char)9 || c == (char)10 || c == (char)13;
}

/* Splits a line in place. Returns the argument count (0 for a blank line). */
static int sh_split(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;

    for (;;) {
        while (*p != 0 && sh_space(*p))
            p++;
        if (*p == 0)
            break;
        if (argc < max)
            argv[argc] = p;
        argc++;
        while (*p != 0 && !sh_space(*p))
            p++;
        if (*p == 0)
            break;
        *p = 0;
        p++;
    }
    return argc;
}

/* Reads the whole script file into g_script; returns its length or -1. */
static i64 sh_load_script(const char *path)
{
    i64 fd;
    i64 total = 0;

    fd = u_open(path, U_O_RDONLY, 0u);
    if (fd < 0)
        return -1;
    for (;;) {
        i64 n = u_read((u32)fd, g_script + total,
                       (u32)(SH_SCRIPT_MAX - 1u - (u64)total));

        if (n < 0) {
            (void)u_close((u32)fd);
            return -1;
        }
        if (n == 0)
            break;
        total += n;
        if ((u64)total >= SH_SCRIPT_MAX - 1u)
            break;
    }
    (void)u_close((u32)fd);
    g_script[total] = 0;
    return total;
}

static int sh_cmd_cat(int argc, char **argv)
{
    i64 fd;
    i64 total = 0;

    if (argc < 2) {
        sh_line("CRUGOS: sh error cat needs a path");
        return -1;
    }
    fd = u_open(argv[1], U_O_RDONLY, 0u);
    if (fd < 0) {
        u_puts("CRUGOS: sh error cannot open ");
        sh_line(argv[1]);
        return -1;
    }
    for (;;) {
        i64 n = u_read((u32)fd, g_io, SH_IO_CHUNK);

        if (n <= 0)
            break;
        (void)u_write(1u, g_io, (u64)n);
        total += n;
    }
    (void)u_close((u32)fd);
    sh_marker("CRUGOS: sh cat ", argv[1], total, " byte(s)");
    return 0;
}

static int sh_cmd_ls(int argc, char **argv)
{
    const char *path = argc >= 2 ? argv[1] : (const char *)"/";
    i64 n;

    n = u_listdir(path, g_io, SH_IO_CHUNK);
    if (n < 0) {
        u_puts("CRUGOS: sh error cannot list ");
        sh_line(path);
        return -1;
    }
    (void)u_write(1u, g_io, (u64)n);
    sh_nl();
    return 0;
}

static int sh_cmd_write(int argc, char **argv)
{
    char content[SH_LINE_MAX];
    u64 len = 0;
    i64 fd;
    i64 put;
    i64 closed;
    int i;

    if (argc < 3) {
        sh_line("CRUGOS: sh error write needs a path and text");
        return -1;
    }
    for (i = 2; i < argc && len + 2u < SH_LINE_MAX; i++) {
        const char *w = argv[i];

        if (i > 2)
            content[len++] = (char)32;
        while (*w != 0 && len + 2u < SH_LINE_MAX)
            content[len++] = *w++;
    }
    content[len++] = (char)10;
    fd = u_open(argv[1], U_O_WRONLY | U_O_CREAT | U_O_TRUNC, 0644u);
    if (fd < 0) {
        u_puts("CRUGOS: sh error cannot create ");
        sh_line(argv[1]);
        return -1;
    }
    put = u_write((u32)fd, content, len);
    closed = u_close((u32)fd);
    if (put != (i64)len || closed < 0) {
        u_puts("CRUGOS: sh error cannot write ");
        sh_line(argv[1]);
        return -1;
    }
    sh_marker("CRUGOS: sh write ", argv[1], (i64)len, " byte(s)");
    return 0;
}

static int sh_cmd_echo(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (i > 1)
            u_puts(" ");
        u_puts(argv[i]);
    }
    sh_nl();
    return 0;
}

static void sh_run(int argc, char **argv)
{
    const char *cmd;

    if (argc < 1)
        return;
    cmd = argv[0];
    sh_marker("CRUGOS: sh cmd ", cmd, -1, "");
    if (u_strcmp(cmd, "ver") == 0) {
        sh_line("CrugOS ring-3 shell (phase u6)");
    } else if (u_strcmp(cmd, "echo") == 0) {
        (void)sh_cmd_echo(argc, argv);
    } else if (u_strcmp(cmd, "cat") == 0) {
        (void)sh_cmd_cat(argc, argv);
    } else if (u_strcmp(cmd, "ls") == 0) {
        (void)sh_cmd_ls(argc, argv);
    } else if (u_strcmp(cmd, "write") == 0) {
        (void)sh_cmd_write(argc, argv);
    } else if (u_strcmp(cmd, "exit") == 0) {
        int code = argc >= 2 ? (int)u_atoi(argv[1]) : 0;

        sh_marker("CRUGOS: sh exit", 0, (i64)code, "");
        u_exit(code);
    } else {
        u_puts("CRUGOS: sh error unknown command ");
        sh_line(cmd);
    }
}

int main(int argc, char **argv)
{
    const char *path = argc >= 2 ? argv[1] : (const char *)"/etc/sh.rc";
    i64 size;
    char *line;
    char *end;
    char *args[SH_ARG_MAX];

    sh_marker("CRUGOS: sh start (argc", 0, (i64)argc, ")");
    size = sh_load_script(path);
    if (size < 0) {
        u_puts("CRUGOS: sh error cannot read ");
        sh_line(path);
        return 1;
    }
    line = g_script;
    end = g_script + size;
    while (line < end) {
        char *nl = line;

        while (nl < end && *nl != (char)10)
            nl++;
        {
            u64 n = (u64)(nl - line);
            u64 i;

            if (n >= SH_LINE_MAX)
                n = SH_LINE_MAX - 1u;
            for (i = 0; i < n; i++)
                g_line[i] = line[i];
            g_line[n] = 0;
        }
        line = nl < end ? nl + 1 : end;
        {
            char *p = g_line;

            while (*p != 0 && sh_space(*p))
                p++;
            if (*p != 0 && *p != (char)35)
                sh_run(sh_split(g_line, args, (int)SH_ARG_MAX), args);
        }
    }
    u_puts("CRUGOS: sh error script ended without exit ");
    sh_line(path);
    return 2;
}
