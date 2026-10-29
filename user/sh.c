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

/* Phase 7A: the .app registry builtin. A record is
   'id|name|version|prog|RRGGBB|KEY=value;...' with no trailing newline. */
#define SH_APP_REC 256u

static void sh_field(const char *rec, int index, char *out, u64 cap)
{
    u64 i = 0;
    int f = 0;

    while (*rec != 0 && f < index) {
        if (*rec == (char)124)
            f++;
        rec++;
    }
    while (*rec != 0 && *rec != (char)124) {
        if (i + 1u < cap)
            out[i++] = *rec;
        rec++;
    }
    out[i] = 0;
}

static i64 sh_app_lookup(const char *id, i64 count, char *rec, u32 cap)
{
    char idbuf[40];
    i64 i;

    for (i = 0; i < count; i++) {
        if (u_app_info((u32)i, rec, cap) <= 0)
            continue;
        sh_field(rec, 0, idbuf, sizeof(idbuf));
        if (u_strcmp(idbuf, id) == 0)
            return i;
    }
    return -1;
}

static int sh_cmd_app(int argc, char **argv)
{
    char rec[SH_APP_REC];
    char idbuf[40];
    char namebuf[40];
    char verbuf[24];
    char iconbuf[16];
    i64 count;
    i64 i;

    if (argc < 2) {
        sh_line("CRUGOS: sh app usage: app list|info <id>|run <id>");
        return -1;
    }
    count = u_app_count();
    if (count < 0) {
        sh_line("CRUGOS: sh error app registry unavailable");
        return -1;
    }
    if (u_strcmp(argv[1], "list") == 0) {
        for (i = 0; i < count; i++) {
            if (u_app_info((u32)i, rec, SH_APP_REC) <= 0)
                continue;
            sh_field(rec, 0, idbuf, sizeof(idbuf));
            sh_field(rec, 2, verbuf, sizeof(verbuf));
            u_puts("CRUGOS: sh app ");
            u_puts(idbuf);
            u_puts(" ");
            sh_line(verbuf);
        }
        sh_marker("CRUGOS: sh app count", 0, count, "");
        return 0;
    }
    if (argc < 3) {
        sh_line("CRUGOS: sh app usage: app list|info <id>|run <id>");
        return -1;
    }
    i = sh_app_lookup(argv[2], count, rec, SH_APP_REC);
    if (i < 0) {
        u_puts("CRUGOS: sh error unknown app ");
        sh_line(argv[2]);
        return -1;
    }
    sh_field(rec, 0, idbuf, sizeof(idbuf));
    sh_field(rec, 1, namebuf, sizeof(namebuf));
    sh_field(rec, 2, verbuf, sizeof(verbuf));
    sh_field(rec, 4, iconbuf, sizeof(iconbuf));
    if (u_strcmp(argv[1], "info") == 0) {
        u_puts("CRUGOS: sh app ");
        u_puts(idbuf);
        u_puts(" ");
        sh_line(verbuf);
        u_puts("CRUGOS: sh app ");
        u_puts(idbuf);
        u_puts(" name ");
        sh_line(namebuf);
        u_puts("CRUGOS: sh app ");
        u_puts(idbuf);
        u_puts(" icon ");
        sh_line(iconbuf);
        return 0;
    }
    if (u_strcmp(argv[1], "run") == 0) {
        i64 tid = u_app_spawn((u32)i);
        i64 code;

        if (tid < 0) {
            u_puts("CRUGOS: sh error cannot launch ");
            sh_line(idbuf);
            return -1;
        }
        u_puts("CRUGOS: sh app ");
        u_puts(idbuf);
        u_puts(" tid ");
        u_putnum((u64)tid);
        sh_nl();
        code = u_wait((u32)tid);
        u_puts("CRUGOS: sh app ");
        u_puts(idbuf);
        u_puts(" exit ");
        u_putnum((u64)code);
        sh_nl();
        return 0;
    }
    sh_line("CRUGOS: sh app usage: app list|info <id>|run <id>");
    return -1;
}

/* Phase 7C: the package builtin. A registry record is
   'id|name|version|publisher|caps|prog' with no trailing newline. */
#define SH_PKG_REC 192u

static i64 sh_pkg_lookup(const char *id, i64 count, char *rec, u32 cap)
{
    char idbuf[40];
    i64 i;

    for (i = 0; i < count; i++) {
        if (u_pkg_info((u32)i, rec, cap) <= 0)
            continue;
        sh_field(rec, 0, idbuf, sizeof(idbuf));
        if (u_strcmp(idbuf, id) == 0)
            return i;
    }
    return -1;
}

static int sh_cmd_pkg(int argc, char **argv)
{
    char rec[SH_PKG_REC];
    char idbuf[40];
    char namebuf[40];
    char verbuf[24];
    char capbuf[64];
    i64 count;
    i64 i;

    if (argc < 2) {
        sh_line("CRUGOS: sh pkg usage: pkg list|info <id>|install <path>|remove <id>");
        return -1;
    }
    count = u_pkg_count();
    if (count < 0) {
        sh_line("CRUGOS: sh error package registry unavailable");
        return -1;
    }
    if (u_strcmp(argv[1], "list") == 0) {
        for (i = 0; i < count; i++) {
            if (u_pkg_info((u32)i, rec, SH_PKG_REC) <= 0)
                continue;
            sh_field(rec, 0, idbuf, sizeof(idbuf));
            sh_field(rec, 2, verbuf, sizeof(verbuf));
            u_puts("CRUGOS: sh pkg ");
            u_puts(idbuf);
            u_puts(" ");
            sh_line(verbuf);
        }
        sh_marker("CRUGOS: sh pkg count", 0, count, "");
        return 0;
    }
    if (argc < 3) {
        sh_line("CRUGOS: sh pkg usage: pkg list|info <id>|install <path>|remove <id>");
        return -1;
    }
    if (u_strcmp(argv[1], "install") == 0) {
        if (u_pkg_install(argv[2]) != 0) {
            u_puts("CRUGOS: sh error pkg install ");
            sh_line(argv[2]);
            return -1;
        }
        u_puts("CRUGOS: sh pkg installed ");
        sh_line(argv[2]);
        return 0;
    }
    if (u_strcmp(argv[1], "remove") == 0) {
        if (u_pkg_remove(argv[2]) != 0) {
            u_puts("CRUGOS: sh error pkg remove ");
            sh_line(argv[2]);
            return -1;
        }
        u_puts("CRUGOS: sh pkg removed ");
        sh_line(argv[2]);
        return 0;
    }
    i = sh_pkg_lookup(argv[2], count, rec, SH_PKG_REC);
    if (i < 0) {
        u_puts("CRUGOS: sh error unknown package ");
        sh_line(argv[2]);
        return -1;
    }
    sh_field(rec, 0, idbuf, sizeof(idbuf));
    sh_field(rec, 1, namebuf, sizeof(namebuf));
    sh_field(rec, 2, verbuf, sizeof(verbuf));
    sh_field(rec, 4, capbuf, sizeof(capbuf));
    if (u_strcmp(argv[1], "info") == 0) {
        u_puts("CRUGOS: sh pkg ");
        u_puts(idbuf);
        u_puts(" ");
        sh_line(verbuf);
        u_puts("CRUGOS: sh pkg ");
        u_puts(idbuf);
        u_puts(" name ");
        sh_line(namebuf);
        u_puts("CRUGOS: sh pkg ");
        u_puts(idbuf);
        u_puts(" caps ");
        sh_line(capbuf);
        return 0;
    }
    sh_line("CRUGOS: sh pkg usage: pkg list|info <id>|install <path>|remove <id>");
    return -1;
}

static void sh_run(int argc, char **argv)
{
    const char *cmd;

    if (argc < 1)
        return;
    cmd = argv[0];
    sh_marker("CRUGOS: sh cmd ", cmd, -1, "");
    if (u_strcmp(cmd, "ver") == 0) {
        sh_line("CrugOS ring-3 shell (phase 7c)");
    } else if (u_strcmp(cmd, "echo") == 0) {
        (void)sh_cmd_echo(argc, argv);
    } else if (u_strcmp(cmd, "cat") == 0) {
        (void)sh_cmd_cat(argc, argv);
    } else if (u_strcmp(cmd, "ls") == 0) {
        (void)sh_cmd_ls(argc, argv);
    } else if (u_strcmp(cmd, "write") == 0) {
        (void)sh_cmd_write(argc, argv);
    } else if (u_strcmp(cmd, "app") == 0) {
        (void)sh_cmd_app(argc, argv);
    } else if (u_strcmp(cmd, "pkg") == 0) {
        (void)sh_cmd_pkg(argc, argv);
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
