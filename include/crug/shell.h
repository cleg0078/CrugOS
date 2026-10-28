/* shell: the Workbench shell core (phase 6C).

   The shell owns a line editor, a small history ring and a command table,
   and renders through a caller-supplied textgrid.  Filesystem access and the
   clock sit behind struct shell_io, so the kernel plugs ext4 in through
   kernel/shell/shell_ops.c and the host tests plug in a fake in-memory
   filesystem. */
#ifndef CRUG_SHELL_H
#define CRUG_SHELL_H

#include <crug/types.h>
#include <crug/textgrid.h>

#define SHELL_LINE_MAX 80u
#define SHELL_HISTORY 8u
#define SHELL_ARGS_MAX 8u
#define SHELL_READ_MAX 4096u
#define SHELL_PROMPT "crugos> "

struct shell_info {
    u64 uptime_ns;
    u64 heap_used;
    u64 heap_total;
    const char *cur_thread;
    u64 switches;
};

/* Everything the shell needs from the machine.  A NULL function pointer is
   reported as a missing subsystem instead of crashing. */
struct shell_io {
    int (*list)(const char *path, char *out, usize cap);
    int (*read)(const char *path, char *out, usize cap, u64 *out_len);
    int (*write)(const char *path, const char *data, usize len);
    int (*sync)(void);
    void (*info)(struct shell_info *out);
};

struct shell {
    struct textgrid *tg;
    const struct shell_io *io;
    char line[SHELL_LINE_MAX];
    u32 len;
    char draft[SHELL_LINE_MAX];
    char history[SHELL_HISTORY][SHELL_LINE_MAX];
    u32 history_count;
    u32 browse; /* 0 = fresh line, n = nth most recent recall */
    void (*sink)(const char *text, void *ctx);
    void *sink_ctx;
    void (*on_run)(const char *line, void *ctx);
    void *run_ctx;
    u32 commands;
};

void shell_init(struct shell *sh, struct textgrid *tg,
                const struct shell_io *io);
/* sink() receives every command output line; on_run() fires just before a
   typed line is executed. */
void shell_set_sink(struct shell *sh, void (*sink)(const char *text, void *ctx),
                    void *ctx);
void shell_set_run_hook(struct shell *sh, void (*on_run)(const char *line, void *ctx),
                        void *ctx);
/* Feed one input byte.  Returns 0 while editing, 1 when a line executed and
   2 when the line was cancelled with Ctrl-C. */
int shell_feed(struct shell *sh, u8 c);
/* Run one line directly (used by the shell self-test); returns 0 when a
   command ran, 1 for an unknown command and -1 for an empty line. */
int shell_run(struct shell *sh, const char *line);
u32 shell_commands(const struct shell *sh);

#ifndef CRUG_HOST_TEST
/* The kernel's shell_io over the mounted root filesystem.
   Implementation: kernel/shell/shell_ops.c */
const struct shell_io *shell_kernel_io(void);
#endif

#endif /* CRUG_SHELL_H */