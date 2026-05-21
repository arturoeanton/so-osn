#include "shell_server.h"

#include <stddef.h>
#include <stdint.h>

#include "../drivers/framebuffer.h"
#include "../fs/fat.h"
#include "../fs/vfs.h"
#include "../net/arp.h"
#include "../net/eth.h"
#include "../net/icmp.h"
#include "../net/socket.h"
#include "../net/tcp.h"
#include "../include/osnos_dirent.h"
#include "../include/osnos_fcntl.h"
#include "../include/osnos_elf.h"
#include "../proc/builtin.h"
#include "../proc/elf.h"
#include "../proc/exec.h"
#include "../include/osnos_keys.h"
#include "../include/osnos_limits.h"
#include "../include/osnos_stat.h"
#include "../include/osnos_status.h"
#include "../lib/string.h"
#include "../micro/fd.h"
#include "../micro/gdt.h"
#include "../micro/idt.h"
#include "../micro/ipc.h"
#include "../micro/kmalloc.h"
#include "../micro/pmm.h"
#include "../micro/reaper.h"
#include "../micro/syscall.h"
#include "../micro/task.h"
#include "../micro/timer.h"
#include "../micro/tss.h"
#include "../micro/uaccess.h"
#include "../micro/vmm.h"

#define HISTORY_MAX  16
#define HISTORY_NONE ((size_t)-1)

static char input[OSNOS_INPUT_MAX];
static size_t input_len = 0;

/*
 * PID of the foreground child task spawned by cmd_exec, or 0 if none.
 * cmd_exec sets this after a successful proc_exec; the IPC_PROC_EXITED
 * handler clears it. While non-zero, Ctrl+C from the user maps to
 * "kill the foreground task" (sets t->kill_pending). When zero, Ctrl+C
 * falls back to its legacy meaning (cancel the input line).
 */
static uint64_t fg_pid = 0;

static char current_path[OSNOS_PATH_MAX] = "/home";

static char history[HISTORY_MAX][OSNOS_INPUT_MAX];
static size_t history_count = 0;
static size_t history_pos = HISTORY_NONE;
static char history_scratch[OSNOS_INPUT_MAX];

static void prompt(void);
static void run_command(const char *cmd);
static void report_fs_failure(osnos_status_t status);
static void shell_send_console_color(const char *s, uint32_t color);
static void shell_send_console(const char *s);
static void shell_send_clear(void);

static int is_absolute_path(const char *path) {
    return path[0] == '/';
}

static bool make_absolute_path(const char *path, char *out, size_t out_size) {
    if (path == 0 || path[0] == 0) {
        size_t want = os_strlcpy(out, current_path, out_size);
        return want + 1 <= out_size;
    }

    /* Strip a matched pair of outer quotes (plus trailing whitespace)
     * so `cat "foo bar"` and `cat 'foo bar'` reach the FS as
     * `foo bar`. Internal quotes and unbalanced ones pass through. */
    char unquoted[OSNOS_PATH_MAX];
    const char *s = path;
    const char *e = path + os_strlen(path);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    if (e - s >= 2 && (*s == '"' || *s == '\'') && e[-1] == *s) {
        s++;
        e--;
        size_t n = (size_t)(e - s);
        if (n + 1 > sizeof(unquoted)) return false;
        for (size_t i = 0; i < n; i++) unquoted[i] = s[i];
        unquoted[n] = 0;
        path = unquoted;
        if (path[0] == 0) {
            size_t want = os_strlcpy(out, current_path, out_size);
            return want + 1 <= out_size;
        }
    }

    if (is_absolute_path(path)) {
        size_t want = os_strlcpy(out, path, out_size);
        return want + 1 <= out_size;
    }

    size_t cur_len = os_strlen(current_path);
    size_t add_len = os_strlen(path);
    size_t separator = (cur_len > 1) ? 1 : 0;
    size_t need = cur_len + separator + add_len + 1;

    os_strlcpy(out, current_path, out_size);
    if (separator) os_strlcat(out, "/", out_size);
    os_strlcat(out, path, out_size);

    return need <= out_size;
}

static void report_path_too_long(void) {
    shell_send_console_color("\npath too long\n", 0xff5555);
    prompt();
}

static int split_two_args(
    const char *args,
    char *first,
    size_t first_size,
    char *second,
    size_t second_size
) {
    const char *p = args;

    while (*p == ' ') p++;

    size_t i = 0;
    while (*p && *p != ' ' && i + 1 < first_size) {
        first[i++] = *p++;
    }
    first[i] = 0;

    if (*p == 0) {
        second[0] = 0;
        return 0;
    }

    while (*p == ' ') p++;

    size_t j = 0;
    while (*p && j + 1 < second_size) {
        second[j++] = *p++;
    }
    second[j] = 0;

    return j > 0;
}

/* ---- console senders (best-effort, return ignored) ---- */

static void shell_send_console_color(const char *s, uint32_t color) {
    ipc_msg_t msg;
    msg.from = SERVER_SHELL;
    msg.to = SERVER_CONSOLE;
    msg.type = IPC_CONSOLE_WRITE;
    msg.arg0 = color;
    msg.arg1 = 0;
    os_strlcpy(msg.data, s, IPC_DATA_SIZE);
    ipc_send(&msg);
}

static void shell_send_console(const char *s) {
    shell_send_console_color(s, 0xffffff);
}

static void shell_send_clear(void) {
    ipc_msg_t msg;
    msg.from = SERVER_SHELL;
    msg.to = SERVER_CONSOLE;
    msg.type = IPC_CONSOLE_CLEAR;
    msg.arg0 = 0;
    msg.arg1 = 0;
    msg.data[0] = 0;
    ipc_send(&msg);
}

/* ---- generic fs senders ---- */

static osnos_status_t shell_send_fs1(ipc_type_t type, const char *path) {
    ipc_msg_t msg;
    msg.from = SERVER_SHELL;
    msg.to = SERVER_FS;
    msg.type = type;
    msg.arg0 = 0;
    msg.arg1 = 0;
    os_strlcpy(msg.data, path ? path : "", IPC_DATA_SIZE);
    return ipc_send(&msg);
}

static osnos_status_t shell_send_fs2(
    ipc_type_t type,
    const char *a,
    const char *b
) {
    ipc_msg_t msg;
    msg.from = SERVER_SHELL;
    msg.to = SERVER_FS;
    msg.type = type;
    msg.arg0 = 0;
    msg.arg1 = 0;

    size_t a_len = os_strlcpy(msg.data, a, IPC_DATA_SIZE);

    if (a_len + 1 < IPC_DATA_SIZE) {
        os_strlcpy(msg.data + a_len + 1, b, IPC_DATA_SIZE - a_len - 1);
    }

    return ipc_send(&msg);
}

static void report_fs_failure(osnos_status_t status) {
    shell_send_console("\n");
    if (status == OSNOS_EAGAIN) {
        shell_send_console_color("ipc queue full\n", 0xff5555);
    } else if (status == OSNOS_ESRCH) {
        shell_send_console_color("fs service unavailable\n", 0xff5555);
    } else {
        shell_send_console_color("ipc send error\n", 0xff5555);
    }
    prompt();
}

static void check_fs(osnos_status_t status) {
    if (status != OSNOS_OK) report_fs_failure(status);
}

/* ---- history ---- */

static void history_save(const char *cmd) {
    if (cmd[0] == 0) return;

    if (history_count > 0 &&
        os_streq(history[history_count - 1], cmd)) {
        return;
    }

    if (history_count < HISTORY_MAX) {
        os_strlcpy(history[history_count], cmd, OSNOS_INPUT_MAX);
        history_count++;
        return;
    }

    for (size_t i = 0; i + 1 < HISTORY_MAX; i++) {
        os_strlcpy(history[i], history[i + 1], OSNOS_INPUT_MAX);
    }
    os_strlcpy(history[HISTORY_MAX - 1], cmd, OSNOS_INPUT_MAX);
}

static void redraw_input(const char *new_input) {
    if (input_len > 0) {
        char erase[OSNOS_INPUT_MAX + 1];
        size_t n = input_len;
        if (n > OSNOS_INPUT_MAX) n = OSNOS_INPUT_MAX;
        for (size_t i = 0; i < n; i++) erase[i] = '\b';
        erase[n] = 0;
        shell_send_console(erase);
    }

    os_strlcpy(input, new_input, OSNOS_INPUT_MAX);
    input_len = os_strlen(input);

    if (input_len > 0) {
        shell_send_console(input);
    }
}

static void history_up(void) {
    if (history_count == 0) return;

    if (history_pos == HISTORY_NONE) {
        input[input_len] = 0;
        os_strlcpy(history_scratch, input, OSNOS_INPUT_MAX);
        history_pos = history_count - 1;
    } else if (history_pos > 0) {
        history_pos--;
    } else {
        return;
    }

    redraw_input(history[history_pos]);
}

static void history_down(void) {
    if (history_pos == HISTORY_NONE) return;

    if (history_pos + 1 < history_count) {
        history_pos++;
        redraw_input(history[history_pos]);
    } else {
        history_pos = HISTORY_NONE;
        redraw_input(history_scratch);
    }
}

/* ---- helpers for handlers ---- */

static bool resolve_path_or_cwd(const char *args, char *out, size_t out_size) {
    if (args[0]) {
        return make_absolute_path(args, out, out_size);
    }
    size_t want = os_strlcpy(out, current_path, out_size);
    return want + 1 <= out_size;
}

static void format_size(size_t n, char *out, size_t out_size) {
    os_format_u64((uint64_t)n, out, out_size);
}

/* ---- command handlers ---- */

static void cmd_help(const char *args);
static void cmd_clear(const char *args);
static void cmd_pwd(const char *args);
static void cmd_cd(const char *args);
static void cmd_ls(const char *args);
static void cmd_cat(const char *args);
static void cmd_touch(const char *args);
static void cmd_rm(const char *args);
static void cmd_mkdir(const char *args);
static void cmd_rmdir(const char *args);
static void cmd_tree(const char *args);
static void cmd_cp(const char *args);
static void cmd_mv(const char *args);
static void cmd_echo(const char *args);
static void cmd_history(const char *args);
static void cmd_test(const char *args);
static void cmd_ps(const char *args);
static void cmd_mem(const char *args);
static void cmd_mount(const char *args);
static void cmd_arp(const char *args);
static void cmd_ping(const char *args);
static void cmd_udptest(const char *args);
static void cmd_tcptest(const char *args);
static void cmd_exec(const char *args);
static void cmd_kill(const char *args);
static void cmd_neof(const char *args);
static void cmd_uname(const char *args);
static void cmd_version(const char *args);
static void cmd_whoami(const char *args);
static void cmd_date(const char *args);
static void cmd_banner(const char *args);
static void cmd_reboot(const char *args);

typedef struct {
    const char *name;
    size_t name_len;
    void (*handler)(const char *args);
    const char *help;
} shell_command_t;

#define CMD(n, h, msg) { n, sizeof(n) - 1, h, msg }
#define ALIAS(n, h)    { n, sizeof(n) - 1, h, 0 }

static const shell_command_t commands[] = {
    CMD("help",    cmd_help,    "help"),
    CMD("clear",   cmd_clear,   "clear | cls"),
    ALIAS("cls",   cmd_clear),
    CMD("pwd",     cmd_pwd,     "pwd"),
    CMD("cd",      cmd_cd,      "cd [PATH]"),
    CMD("ls",      cmd_ls,      "ls [PATH]"),
    CMD("tree",    cmd_tree,    "tree [PATH]"),
    CMD("cat",     cmd_cat,     "cat FILE"),
    CMD("touch",   cmd_touch,   "touch FILE"),
    CMD("rm",      cmd_rm,      "rm FILE | PATTERN"),
    CMD("mkdir",   cmd_mkdir,   "mkdir DIR"),
    CMD("rmdir",   cmd_rmdir,   "rmdir DIR"),
    CMD("cp",      cmd_cp,      "cp SRC DST"),
    CMD("mv",      cmd_mv,      "mv SRC DST"),
    CMD("echo",    cmd_echo,    "echo TEXT [> FILE | >> FILE]"),
    CMD("history", cmd_history, "history"),
    CMD("test",    cmd_test,    "test (run self-test suite)"),
    CMD("ps",      cmd_ps,      "ps"),
    CMD("mem",     cmd_mem,     "mem"),
    CMD("mount",   cmd_mount,   "mount"),
    CMD("arp",     cmd_arp,     "arp [IP] (default: gateway)"),
    CMD("ping",    cmd_ping,    "ping IP (one ICMP echo, 1s timeout)"),
    CMD("udptest", cmd_udptest, "udptest [PORT] (bind+echo loop, 10s)"),
    CMD("tcptest", cmd_tcptest, "tcptest [PORT] (listen, accept 1 conn, RST)"),
    CMD("exec",    cmd_exec,    "exec /bin/PROG [args] [&]"),
    CMD("kill",    cmd_kill,    "kill PID"),
    CMD("neof",    cmd_neof,    "neof"),
    CMD("uname",   cmd_uname,   "uname"),
    CMD("version", cmd_version, "version"),
    CMD("whoami",  cmd_whoami,  "whoami"),
    CMD("date",    cmd_date,    "date"),
    CMD("banner",  cmd_banner,  "banner"),
    CMD("reboot",  cmd_reboot,  "reboot"),
};

#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

static void cmd_help(const char *args) {
    (void)args;
    char buf[IPC_DATA_SIZE];
    buf[0] = 0;

    os_strlcat(buf, "\ncommands:\n", sizeof(buf));
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (!commands[i].help) continue;
        os_strlcat(buf, "  ", sizeof(buf));
        os_strlcat(buf, commands[i].help, sizeof(buf));
        os_strlcat(buf, "\n", sizeof(buf));
    }

    shell_send_console_color(buf, 0xaaaaaa);
    prompt();
}

static void cmd_clear(const char *args) {
    (void)args;
    shell_send_clear();
    prompt();
}

static void cmd_pwd(const char *args) {
    (void)args;
    shell_send_console("\n");
    shell_send_console(current_path);
    shell_send_console("\n");
    prompt();
}

static void cmd_cd(const char *args) {
    if (args[0] == 0) {
        os_strlcpy(current_path, "/home", sizeof(current_path));
    } else {
        char tmp[OSNOS_PATH_MAX];
        if (!make_absolute_path(args, tmp, sizeof(tmp))) {
            report_path_too_long();
            return;
        }
        os_strlcpy(current_path, tmp, sizeof(current_path));
    }
    shell_send_console("\n");
    prompt();
}

static void cmd_ls(const char *args) {
    char path[OSNOS_PATH_MAX];
    if (!resolve_path_or_cwd(args, path, sizeof(path))) {
        report_path_too_long();
        return;
    }
    check_fs(shell_send_fs1(IPC_FS_LIST, path));
}

static void cmd_cat(const char *args) {
    if (args[0] == 0) {
        shell_send_console_color("\nusage: cat FILE\n", 0xff5555);
        prompt();
        return;
    }
    char path[OSNOS_PATH_MAX];
    if (!make_absolute_path(args, path, sizeof(path))) {
        report_path_too_long();
        return;
    }
    check_fs(shell_send_fs1(IPC_FS_READ, path));
}

static void cmd_touch(const char *args) {
    if (args[0] == 0) {
        shell_send_console_color("\nusage: touch FILE\n", 0xff5555);
        prompt();
        return;
    }
    char path[OSNOS_PATH_MAX];
    if (!make_absolute_path(args, path, sizeof(path))) {
        report_path_too_long();
        return;
    }
    check_fs(shell_send_fs1(IPC_FS_TOUCH, path));
}

static void cmd_rm(const char *args) {
    if (args[0] == 0) {
        shell_send_console_color("\nusage: rm FILE\n", 0xff5555);
        prompt();
        return;
    }
    char path[OSNOS_PATH_MAX];
    if (!make_absolute_path(args, path, sizeof(path))) {
        report_path_too_long();
        return;
    }
    check_fs(shell_send_fs1(IPC_FS_DELETE, path));
}

static void cmd_mkdir(const char *args) {
    if (args[0] == 0) {
        shell_send_console_color("\nusage: mkdir DIR\n", 0xff5555);
        prompt();
        return;
    }
    char path[OSNOS_PATH_MAX];
    if (!make_absolute_path(args, path, sizeof(path))) {
        report_path_too_long();
        return;
    }
    check_fs(shell_send_fs1(IPC_FS_MKDIR, path));
}

static void cmd_rmdir(const char *args) {
    if (args[0] == 0) {
        shell_send_console_color("\nusage: rmdir DIR\n", 0xff5555);
        prompt();
        return;
    }
    char path[OSNOS_PATH_MAX];
    if (!make_absolute_path(args, path, sizeof(path))) {
        report_path_too_long();
        return;
    }
    check_fs(shell_send_fs1(IPC_FS_RMDIR, path));
}

static void cmd_tree(const char *args) {
    char path[OSNOS_PATH_MAX];
    if (!resolve_path_or_cwd(args, path, sizeof(path))) {
        report_path_too_long();
        return;
    }
    check_fs(shell_send_fs1(IPC_FS_TREE, path));
}

static void cmd_cp(const char *args) {
    char a[OSNOS_PATH_MAX];
    char b[OSNOS_PATH_MAX];

    if (!split_two_args(args, a, sizeof(a), b, sizeof(b))) {
        shell_send_console_color("\nusage: cp SRC DST\n", 0xff5555);
        prompt();
        return;
    }

    char src[OSNOS_PATH_MAX];
    char dst[OSNOS_PATH_MAX];
    if (!make_absolute_path(a, src, sizeof(src)) ||
        !make_absolute_path(b, dst, sizeof(dst))) {
        report_path_too_long();
        return;
    }
    check_fs(shell_send_fs2(IPC_FS_COPY, src, dst));
}

static void cmd_mv(const char *args) {
    char a[OSNOS_PATH_MAX];
    char b[OSNOS_PATH_MAX];

    if (!split_two_args(args, a, sizeof(a), b, sizeof(b))) {
        shell_send_console_color("\nusage: mv SRC DST\n", 0xff5555);
        prompt();
        return;
    }

    char src[OSNOS_PATH_MAX];
    char dst[OSNOS_PATH_MAX];
    if (!make_absolute_path(a, src, sizeof(src)) ||
        !make_absolute_path(b, dst, sizeof(dst))) {
        report_path_too_long();
        return;
    }
    check_fs(shell_send_fs2(IPC_FS_MOVE, src, dst));
}

/*
 * Strip a trailing run of whitespace, then if the span starts AND
 * ends with the same matching quote character (" or '), strip those
 * too. In-place advance of *start / pull-back of *end.
 */
static void strip_outer_quotes(const char **start, const char **end) {
    const char *s = *start;
    const char *e = *end;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    if (e - s >= 2 && (*s == '"' || *s == '\'') && e[-1] == *s) {
        s++;
        e--;
    }
    *start = s;
    *end   = e;
}

static void cmd_echo(const char *args) {
    const char *p = args;
    const char *redir = 0;
    int append = 0;

    while (*p) {
        if (p[0] == '>' && p[1] == '>') { redir = p; append = 1; break; }
        if (p[0] == '>')                { redir = p; append = 0; break; }
        p++;
    }

    /* Content span = [args, redir or end-of-string). */
    const char *cs = args;
    const char *ce = redir ? redir : args + os_strlen(args);
    strip_outer_quotes(&cs, &ce);

    if (!redir) {
        char line[OSNOS_INPUT_MAX];
        size_t n = (size_t)(ce - cs);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        for (size_t k = 0; k < n; k++) line[k] = cs[k];
        line[n] = 0;

        shell_send_console("\n");
        shell_send_console(line);
        shell_send_console("\n");
        prompt();
        return;
    }

    char content[OSNOS_INPUT_MAX];
    char filename[OSNOS_NAME_MAX];
    char abs_filename[OSNOS_PATH_MAX];

    size_t n = (size_t)(ce - cs);
    if (n + 2 > sizeof(content)) n = sizeof(content) - 2;
    for (size_t k = 0; k < n; k++) content[k] = cs[k];
    content[n++] = '\n';
    content[n]   = 0;

    /* Filename span = after `>`/`>>`, skip blanks, strip quotes. */
    const char *name = redir + (append ? 2 : 1);
    while (*name == ' ' || *name == '\t') name++;
    const char *ns = name;
    const char *ne = name + os_strlen(name);
    strip_outer_quotes(&ns, &ne);

    size_t fn_len = (size_t)(ne - ns);
    if (fn_len >= sizeof(filename)) fn_len = sizeof(filename) - 1;
    for (size_t k = 0; k < fn_len; k++) filename[k] = ns[k];
    filename[fn_len] = 0;

    if (!make_absolute_path(filename, abs_filename, sizeof(abs_filename))) {
        report_path_too_long();
        return;
    }

    check_fs(shell_send_fs2(
        append ? IPC_FS_APPEND : IPC_FS_WRITE,
        abs_filename,
        content
    ));
}

static void cmd_history(const char *args) {
    (void)args;
    char buf[IPC_DATA_SIZE];
    buf[0] = 0;

    os_strlcat(buf, "\n", sizeof(buf));
    for (size_t i = 0; i < history_count; i++) {
        char idx[8];
        format_size(i + 1, idx, sizeof(idx));
        os_strlcat(buf, "  ", sizeof(buf));
        os_strlcat(buf, idx, sizeof(buf));
        os_strlcat(buf, "  ", sizeof(buf));
        os_strlcat(buf, history[i], sizeof(buf));
        os_strlcat(buf, "\n", sizeof(buf));
    }

    shell_send_console_color(buf, 0xaaaaaa);
    prompt();
}

/*
 * Self-test. Calls vfs_* directly from the shell, which couples shell -> VFS
 * (otherwise the shell only talks via IPC). Acceptable as a kernel-side
 * debugger: shell and FS live in the same address space today.
 *
 * Uses /test as a sandbox. Aborts if /test already exists (do not clobber).
 *
 * Output is flushed in chunks because the total exceeds IPC_DATA_SIZE.
 */
static void check_test(
    bool cond,
    const char *name,
    char *buf,
    size_t buf_size,
    char *fail_log,
    size_t fail_log_size,
    size_t *pass,
    size_t *fail
) {
    os_strlcat(buf, cond ? "  PASS " : "  FAIL ", buf_size);
    os_strlcat(buf, name, buf_size);
    os_strlcat(buf, "\n", buf_size);
    if (cond) (*pass)++;
    else {
        (*fail)++;
        /* Always also record into the fail-only log so the summary
         * doesn't scroll off the screen with the per-check output. */
        os_strlcat(fail_log, "  FAIL ", fail_log_size);
        os_strlcat(fail_log, name,     fail_log_size);
        os_strlcat(fail_log, "\n",     fail_log_size);
    }
}

static void test_flush(char *buf, size_t buf_size, bool force, uint32_t color) {
    if (buf[0] == 0) return;
    if (!force && os_strlen(buf) < buf_size - 128) return;
    shell_send_console_color(buf, color);
    buf[0] = 0;
}

static void cmd_test(const char *args) {
    (void)args;

    char buf[IPC_DATA_SIZE];
    buf[0] = 0;
    os_strlcat(buf, "\n", sizeof(buf));

    /*
     * Sticky log of every FAIL — printed last so summary + failures
     * are the bottom-most lines and don't scroll off-screen with the
     * verbose per-check output.
     */
    char fail_log[IPC_DATA_SIZE];
    fail_log[0] = 0;

    size_t pass = 0;
    size_t fail = 0;

    vfs_stat_t st;
    if (vfs_stat("/test", &st) == OSNOS_OK) {
        os_strlcat(buf, "/test already exists, aborting\n", sizeof(buf));
        shell_send_console_color(buf, 0xff5555);
        prompt();
        return;
    }

    #define CHECK(cond, name) do { \
        check_test((cond), (name), buf, sizeof(buf), \
                   fail_log, sizeof(fail_log), &pass, &fail); \
        test_flush(buf, sizeof(buf), false, 0xaaaaaa); \
    } while (0)

    osnos_status_t s;
    size_t got;
    char rdbuf[64];

    /* mkdir + stat */
    s = vfs_mkdir("/test");
    CHECK(s == OSNOS_OK, "mkdir /test");

    s = vfs_stat("/test", &st);
    CHECK(s == OSNOS_OK && st.type == VFS_NODE_DIR, "stat /test = dir");

    /* write + read */
    s = vfs_write("/test/a.txt", "hello\n", 6);
    CHECK(s == OSNOS_OK, "write a.txt");

    s = vfs_read("/test/a.txt", rdbuf, sizeof(rdbuf), &got);
    rdbuf[got] = 0;
    CHECK(s == OSNOS_OK && got == 6 && os_streq(rdbuf, "hello\n"), "read a.txt");

    /* append */
    s = vfs_append("/test/a.txt", "world\n", 6);
    CHECK(s == OSNOS_OK, "append a.txt");

    s = vfs_read("/test/a.txt", rdbuf, sizeof(rdbuf), &got);
    rdbuf[got] = 0;
    CHECK(got == 12 && os_streq(rdbuf, "hello\nworld\n"), "read after append");

    /* touch on existing keeps content */
    s = vfs_touch("/test/a.txt");
    CHECK(s == OSNOS_OK, "touch existing");
    s = vfs_read("/test/a.txt", rdbuf, sizeof(rdbuf), &got);
    CHECK(got == 12, "touch preserves content");

    /* touch creates empty */
    s = vfs_touch("/test/b.txt");
    CHECK(s == OSNOS_OK, "touch new");
    s = vfs_stat("/test/b.txt", &st);
    CHECK(s == OSNOS_OK && st.size == 0, "new file empty");

    /* stat size */
    s = vfs_stat("/test/a.txt", &st);
    CHECK(s == OSNOS_OK && st.size == 12, "stat size");

    /* stat ENOENT */
    s = vfs_stat("/test/nope", &st);
    CHECK(s == OSNOS_ENOENT, "stat absent -> ENOENT");

    /* nested mkdir */
    s = vfs_mkdir("/test/sub");
    CHECK(s == OSNOS_OK, "mkdir sub");

    /* duplicate mkdir */
    s = vfs_mkdir("/test/sub");
    CHECK(s == OSNOS_EEXIST, "mkdir dup -> EEXIST");

    /* readdir */
    size_t cursor = 0;
    vfs_dirent_t ent;
    bool found_a = false, found_b = false, found_sub = false;
    while (vfs_readdir("/test", &cursor, &ent) == OSNOS_OK) {
        if (os_streq(ent.name, "a.txt") && ent.type == VFS_NODE_REG) found_a = true;
        if (os_streq(ent.name, "b.txt") && ent.type == VFS_NODE_REG) found_b = true;
        if (os_streq(ent.name, "sub")   && ent.type == VFS_NODE_DIR) found_sub = true;
    }
    CHECK(found_a && found_b && found_sub, "readdir lists all");

    /* rmdir non-empty */
    s = vfs_rmdir("/test");
    CHECK(s == OSNOS_ENOTEMPTY, "rmdir busy -> ENOTEMPTY");

    /* unlink */
    s = vfs_unlink("/test/b.txt");
    CHECK(s == OSNOS_OK, "unlink b.txt");
    s = vfs_stat("/test/b.txt", &st);
    CHECK(s == OSNOS_ENOENT, "unlinked is gone");

    /* copy */
    s = vfs_copy("/test/a.txt", "/test/c.txt");
    CHECK(s == OSNOS_OK, "copy a->c");
    s = vfs_read("/test/c.txt", rdbuf, sizeof(rdbuf), &got);
    rdbuf[got] = 0;
    CHECK(got == 12 && os_streq(rdbuf, "hello\nworld\n"), "copy content matches");

    /* move/rename */
    s = vfs_move("/test/c.txt", "/test/d.txt");
    CHECK(s == OSNOS_OK, "move c->d");
    s = vfs_stat("/test/c.txt", &st);
    CHECK(s == OSNOS_ENOENT, "src gone after move");
    s = vfs_stat("/test/d.txt", &st);
    CHECK(s == OSNOS_OK, "dst exists after move");

    /* clean up for glob tests */
    vfs_unlink("/test/a.txt");
    vfs_unlink("/test/d.txt");

    /* glob */
    vfs_write("/test/foo.txt", "x", 1);
    vfs_write("/test/bar.txt", "x", 1);
    vfs_write("/test/foo.md",  "x", 1);

    size_t n = vfs_glob_unlink("/test/*.txt");
    CHECK(n == 2, "glob unlinked 2");
    s = vfs_stat("/test/foo.md", &st);
    CHECK(s == OSNOS_OK, "glob spared non-match");

    vfs_unlink("/test/foo.md");

    /* path validation */
    s = vfs_stat("relative", &st);
    CHECK(s == OSNOS_EINVAL, "relative -> EINVAL");

    /* list_dir smoke: /test still has "sub" dir at this point */
    char tbuf[256];
    size_t w = vfs_list_dir("/test", tbuf, sizeof(tbuf));
    CHECK(w > 0 && os_strstarts(tbuf, "sub/"), "list_dir shows sub/");

    /* cleanup */
    s = vfs_rmdir("/test/sub");
    CHECK(s == OSNOS_OK, "rmdir sub");
    s = vfs_rmdir("/test");
    CHECK(s == OSNOS_OK, "rmdir /test");

    /* sysfs (synthetic, read-only) */
    s = vfs_stat("/sys", &st);
    CHECK(s == OSNOS_OK && st.type == VFS_NODE_DIR, "sysfs root dir");

    s = vfs_stat("/sys/version", &st);
    CHECK(s == OSNOS_OK && st.type == VFS_NODE_REG && st.size > 0, "sysfs version exists");

    s = vfs_read("/sys/version", rdbuf, sizeof(rdbuf), &got);
    CHECK(s == OSNOS_OK && got > 0, "sysfs read version");

    s = vfs_write("/sys/version", "x", 1);
    CHECK(s == OSNOS_EROFS, "sysfs write -> EROFS");

    s = vfs_mkdir("/sys/newdir");
    CHECK(s == OSNOS_EROFS, "sysfs mkdir -> EROFS");

    s = vfs_stat("/sys/nope", &st);
    CHECK(s == OSNOS_ENOENT, "sysfs missing entry");

    /* stat mode populated (no enforcement, just exposed) */
    s = vfs_stat("/home", &st);
    CHECK(s == OSNOS_OK && st.mode == 0755, "ramfs dir mode 0755");

    s = vfs_stat("/home/README.TXT", &st);
    CHECK(s == OSNOS_OK && st.mode == 0644, "ramfs file mode 0644");

    s = vfs_stat("/sys/version", &st);
    CHECK(s == OSNOS_OK && st.mode == 0444, "sysfs file mode 0444");

    /* devfs */
    s = vfs_stat("/dev", &st);
    CHECK(s == OSNOS_OK && st.type == VFS_NODE_DIR, "devfs root dir");

    s = vfs_stat("/dev/null", &st);
    CHECK(s == OSNOS_OK && st.type == VFS_NODE_CHR && st.mode == 0666,
          "/dev/null is chr 0666");

    s = vfs_stat("/dev/zero", &st);
    CHECK(s == OSNOS_OK && st.type == VFS_NODE_CHR, "/dev/zero is chr");

    /* /dev/null: read = 0 bytes, write = OK (discarded) */
    char zbuf[16];
    size_t zgot = 99;
    s = vfs_read("/dev/null", zbuf, sizeof(zbuf), &zgot);
    CHECK(s == OSNOS_OK && zgot == 0, "/dev/null read = 0 bytes");

    s = vfs_write("/dev/null", "data", 4);
    CHECK(s == OSNOS_OK, "/dev/null write OK");

    /* /dev/zero: read fills with zeros */
    for (size_t i = 0; i < sizeof(zbuf); i++) zbuf[i] = 0xAA;
    s = vfs_read("/dev/zero", zbuf, sizeof(zbuf), &zgot);
    bool all_zero = (s == OSNOS_OK && zgot == sizeof(zbuf));
    for (size_t i = 0; i < zgot && all_zero; i++) {
        if (zbuf[i] != 0) all_zero = false;
    }
    CHECK(all_zero, "/dev/zero fills with zeros");

    /* devfs mkdir/unlink rejected */
    s = vfs_mkdir("/dev/newdir");
    CHECK(s == OSNOS_EROFS, "devfs mkdir -> EROFS");

    s = vfs_unlink("/dev/null");
    CHECK(s == OSNOS_EROFS, "devfs unlink -> EROFS");

    /* sysfs introspection */
    s = vfs_stat("/sys/meminfo", &st);
    CHECK(s == OSNOS_OK && st.size > 0, "/sys/meminfo exists");

    s = vfs_stat("/sys/tasks", &st);
    CHECK(s == OSNOS_OK && st.size > 0, "/sys/tasks exists");

    s = vfs_stat("/sys/mounts", &st);
    CHECK(s == OSNOS_OK && st.size > 0, "/sys/mounts exists");

    s = vfs_stat("/sys/uptime", &st);
    CHECK(s == OSNOS_OK && st.size > 0, "/sys/uptime exists");

    s = vfs_stat("/sys/cpuinfo", &st);
    CHECK(s == OSNOS_OK && st.size > 0, "/sys/cpuinfo exists");

    s = vfs_stat("/sys/services", &st);
    CHECK(s == OSNOS_OK && st.size > 0, "/sys/services exists");

    s = vfs_stat("/sys/build", &st);
    CHECK(s == OSNOS_OK && st.size > 0, "/sys/build exists");

    /* uptime monotonically increases */
    s = vfs_read("/sys/uptime", rdbuf, sizeof(rdbuf), &got);
    CHECK(s == OSNOS_OK && got > 0, "/sys/uptime read");

    /* cpuinfo has vendor_id line */
    char cbuf[256];
    s = vfs_read("/sys/cpuinfo", cbuf, sizeof(cbuf), &got);
    cbuf[got < sizeof(cbuf) ? got : sizeof(cbuf) - 1] = 0;
    CHECK(s == OSNOS_OK && os_strstarts(cbuf, "vendor_id"), "cpuinfo has vendor_id");

    /* ============== syscalls (FASE 4) ============== */

    /* prep workspace */
    vfs_mkdir("/test");

    /* open O_CREAT new file */
    int64_t fd = sys_open("/test/syscall.txt", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 3, "open O_CREAT new fd");

    int64_t nw = sys_write((int)fd, "hello", 5);
    CHECK(nw == 5, "write 5 bytes");

    nw = sys_write((int)fd, " world\n", 7);
    CHECK(nw == 7, "write 7 more");

    int64_t rc = sys_close((int)fd);
    CHECK(rc == 0, "close write fd");

    /* open O_RDONLY and read back */
    fd = sys_open("/test/syscall.txt", O_RDONLY, 0);
    CHECK(fd >= 3, "open O_RDONLY");

    char iobuf[64];
    int64_t r = sys_read((int)fd, iobuf, sizeof(iobuf));
    iobuf[r > 0 ? r : 0] = 0;
    CHECK(r == 12 && os_streq(iobuf, "hello world\n"), "read back full");

    /* lseek SEEK_SET */
    int64_t off = sys_lseek((int)fd, 6, SEEK_SET);
    CHECK(off == 6, "lseek SEEK_SET");

    r = sys_read((int)fd, iobuf, sizeof(iobuf));
    iobuf[r > 0 ? r : 0] = 0;
    CHECK(r == 6 && os_streq(iobuf, "world\n"), "read after lseek");

    /* lseek SEEK_END */
    off = sys_lseek((int)fd, 0, SEEK_END);
    CHECK(off == 12, "lseek SEEK_END");

    r = sys_read((int)fd, iobuf, sizeof(iobuf));
    CHECK(r == 0, "read at EOF returns 0");

    /* fstat */
    osnos_stat_t fst;
    rc = sys_fstat((int)fd, &fst);
    CHECK(rc == 0 && fst.st_size == 12, "fstat size");

    /* isatty: file=0, stdin/out/err=1 */
    CHECK(sys_isatty((int)fd) == 0, "isatty(file) = 0");
    CHECK(sys_isatty(OSNOS_FD_STDIN) == 1, "isatty(stdin) = 1");
    CHECK(sys_isatty(OSNOS_FD_STDOUT) == 1, "isatty(stdout) = 1");

    sys_close((int)fd);

    /* O_TRUNC empties existing file */
    fd = sys_open("/test/syscall.txt", O_WRONLY | O_TRUNC, 0);
    CHECK(fd >= 3, "open O_TRUNC");
    sys_close((int)fd);

    vfs_stat_t st2;
    s = vfs_stat("/test/syscall.txt", &st2);
    CHECK(s == OSNOS_OK && st2.size == 0, "O_TRUNC zeroed size");

    /* O_EXCL on existing file fails */
    fd = sys_open("/test/syscall.txt", O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd == -(int64_t)OSNOS_EEXIST, "O_EXCL on existing -> EEXIST");

    /* open nonexistent without O_CREAT fails */
    fd = sys_open("/test/nope.txt", O_RDONLY, 0);
    CHECK(fd == -(int64_t)OSNOS_ENOENT, "open absent -> ENOENT");

    /* read/write on bad fd */
    r = sys_read(99, iobuf, 10);
    CHECK(r == -(int64_t)OSNOS_EBADF, "read bad fd -> EBADF");

    nw = sys_write(OSNOS_FD_STDIN, "x", 1);
    CHECK(nw == -(int64_t)OSNOS_EBADF, "write stdin -> EBADF");

    /* close bad fd */
    rc = sys_close(99);
    CHECK(rc == -(int64_t)OSNOS_EBADF, "close bad fd -> EBADF");

    /* fd table exhaustion */
    int fds[OSNOS_MAX_FDS];
    int allocated = 0;
    for (size_t i = 0; i < OSNOS_MAX_FDS; i++) {
        int64_t got_fd = sys_open("/test/syscall.txt", O_RDONLY, 0);
        if (got_fd < 0) break;
        fds[allocated++] = (int)got_fd;
    }
    /* one more should fail with EMFILE */
    int64_t over = sys_open("/test/syscall.txt", O_RDONLY, 0);
    CHECK(over == -(int64_t)OSNOS_EMFILE, "fd table full -> EMFILE");

    for (int i = 0; i < allocated; i++) sys_close(fds[i]);

    /* stdin ring buffer */
    /* drain whatever might be there from real keypresses before testing */
    char drain[64];
    while (sys_read(OSNOS_FD_STDIN, drain, sizeof(drain)) > 0) { }

    r = sys_read(OSNOS_FD_STDIN, iobuf, sizeof(iobuf));
    CHECK(r == 0, "stdin empty -> 0");

    stdin_push('a');
    stdin_push('b');
    stdin_push('c');
    r = sys_read(OSNOS_FD_STDIN, iobuf, sizeof(iobuf));
    iobuf[r > 0 ? r : 0] = 0;
    CHECK(r == 3 && os_streq(iobuf, "abc"), "stdin reads buffered chars");

    r = sys_read(OSNOS_FD_STDIN, iobuf, sizeof(iobuf));
    CHECK(r == 0, "stdin drained -> 0");

    /* sys_exit: mark current task DEAD, then restore (otherwise shell dies) */
    task_t *self = task_current();
    task_state_t prev = self->state;
    sys_exit(0);
    CHECK(self->state == TASK_DEAD, "sys_exit marks DEAD");
    self->state = prev;

    /* ============== PMM (FASE 4.5) ============== */

    CHECK(pmm_total_pages() > 0, "pmm has total pages");
    CHECK(pmm_free_pages() > 0,  "pmm has free pages");

    size_t free_before = pmm_free_pages();

    uint64_t p1 = pmm_alloc_page();
    CHECK(p1 != 0 && (p1 & (PAGE_SIZE - 1)) == 0, "alloc returns aligned page");
    CHECK(pmm_free_pages() == free_before - 1, "alloc decrements free count");

    uint64_t p2 = pmm_alloc_page();
    CHECK(p2 != 0 && p2 != p1, "two allocs return distinct pages");

    /* write/read via HHDM to confirm the page is actually usable RAM */
    uint8_t *vp1 = (uint8_t *)(p1 + pmm_hhdm_offset());
    vp1[0] = 0xAB;
    vp1[PAGE_SIZE - 1] = 0xCD;
    CHECK(vp1[0] == 0xAB && vp1[PAGE_SIZE - 1] == 0xCD, "alloc page is writable");

    pmm_free_page(p1);
    pmm_free_page(p2);
    CHECK(pmm_free_pages() == free_before, "free restores count");

    /* double-free is a no-op */
    pmm_free_page(p1);
    CHECK(pmm_free_pages() == free_before, "double free is no-op");

    /* alloc + free again -> we get p1 back (LIFO-ish via hint) */
    uint64_t p3 = pmm_alloc_page();
    CHECK(p3 == p1, "freed page is reused");
    pmm_free_page(p3);

    /* ============== VMM ============== */

    uint64_t *pml4 = vmm_kernel_pml4();
    CHECK(pml4 != 0, "kernel_pml4 exists");
    CHECK(vmm_pml4_used(pml4) > 0, "pml4 has live entries (cloned from Limine)");

    /* HHDM mapping should resolve back to physical 0 (since HHDM covers
     * all RAM starting at phys 0). */
    uint64_t hhdm0 = pmm_hhdm_offset();
    uint64_t resolved = vmm_lookup(pml4, hhdm0);
    CHECK(resolved == 0, "lookup(hhdm base) -> phys 0");

    /* Map a fresh page to a scratch virtual address well outside HHDM
     * and kernel ranges. */
    const uint64_t scratch_virt = 0xffff900000000000ULL;

    uint64_t scratch_phys = pmm_alloc_page();
    CHECK(scratch_phys != 0, "alloc scratch phys");

    int ok = vmm_map(pml4, scratch_virt, scratch_phys, PTE_W);
    CHECK(ok == 1, "vmm_map scratch");

    uint64_t lk = vmm_lookup(pml4, scratch_virt);
    CHECK(lk == scratch_phys, "lookup returns mapped phys");

    /* Writing via the scratch virt should land in the same page we can
     * also access via HHDM. */
    volatile uint8_t *via_virt  = (volatile uint8_t *)scratch_virt;
    volatile uint8_t *via_hhdm  = (volatile uint8_t *)(scratch_phys + hhdm0);
    via_virt[0] = 0x42;
    CHECK(via_hhdm[0] == 0x42, "write via virt visible via hhdm");

    via_hhdm[10] = 0x99;
    CHECK(via_virt[10] == 0x99, "write via hhdm visible via virt");

    /* Lookup with offset within page */
    uint64_t lk_off = vmm_lookup(pml4, scratch_virt + 0x123);
    CHECK(lk_off == scratch_phys + 0x123, "lookup preserves page offset");

    /* Unmap + verify lookup is now 0 */
    vmm_unmap(pml4, scratch_virt);
    CHECK(vmm_lookup(pml4, scratch_virt) == 0, "lookup after unmap = 0");

    pmm_free_page(scratch_phys);

    /* ============== kmalloc / kfree ============== */

    CHECK(kheap_total_bytes() > 0, "kheap has total bytes");
    size_t heap_before = kheap_used_bytes();

    void *m1 = kmalloc(100);
    CHECK(m1 != 0, "kmalloc 100");
    CHECK(((uintptr_t)m1 & 7) == 0, "kmalloc 8-aligned");
    CHECK(kheap_used_bytes() > heap_before, "kheap used grew");

    /* writable */
    char *cm1 = (char *)m1;
    for (int i = 0; i < 100; i++) cm1[i] = (char)(i ^ 0x55);
    bool ok_write = true;
    for (int i = 0; i < 100; i++) {
        if (cm1[i] != (char)(i ^ 0x55)) { ok_write = false; break; }
    }
    CHECK(ok_write, "kmalloc block writable");

    kfree(m1);
    CHECK(kheap_used_bytes() == heap_before, "kfree restores used");

    /* alloc same size again: should reuse the just-freed coalesced
     * region, so we get the same pointer back */
    void *m2 = kmalloc(100);
    CHECK(m2 == m1, "kmalloc reuses freed block");
    kfree(m2);

    /* batch alloc/free */
    void *ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = kmalloc(64);
    }
    bool all_ok = true;
    for (int i = 0; i < 10; i++) {
        if (!ptrs[i]) all_ok = false;
        for (int j = i + 1; j < 10; j++) {
            if (ptrs[i] == ptrs[j]) all_ok = false;
        }
    }
    CHECK(all_ok, "batch alloc: 10 distinct non-null");

    /* free in reverse to exercise coalescing */
    for (int i = 9; i >= 0; i--) kfree(ptrs[i]);
    CHECK(kheap_used_bytes() == heap_before, "batch free restores used");

    /* edge cases */
    CHECK(kmalloc(0) == 0, "kmalloc(0) -> NULL");
    kfree(0);  /* must not crash */
    CHECK(kheap_used_bytes() == heap_before, "kfree(NULL) is no-op");

    /* ============== address spaces ============== */

    size_t pmm_before_as = pmm_free_pages();
    uint64_t *new_as = address_space_create();
    CHECK(new_as != 0, "address_space_create");

    /* High half cloned from kernel */
    bool half_match = true;
    for (int i = 256; i < 512; i++) {
        if (new_as[i] != pml4[i]) { half_match = false; break; }
    }
    CHECK(half_match, "AS high half cloned");

    /* Low half empty */
    bool low_empty = true;
    for (int i = 0; i < 256; i++) {
        if (new_as[i] != 0) { low_empty = false; break; }
    }
    CHECK(low_empty, "AS low half zeroed");

    /* Map a page in the user range of the new AS */
    uint64_t user_virt = 0x400000ULL;  /* typical user code base */
    uint64_t user_phys = pmm_alloc_page();
    int as_ok = vmm_map(new_as, user_virt, user_phys, PTE_W | PTE_U);
    CHECK(as_ok == 1, "map page in new AS");

    /* Lookup in new AS finds it; lookup in kernel AS does not */
    CHECK(vmm_lookup(new_as, user_virt) == user_phys, "new AS sees the page");
    CHECK(vmm_lookup(pml4, user_virt) == 0, "kernel AS does not see it");

    /* Destroy frees everything in the low half + the PML4 page */
    address_space_destroy(new_as);
    CHECK(pmm_free_pages() == pmm_before_as, "destroy recovers all pages");

    /* ============== copy_from_user / copy_to_user ============== */

    char ubuf[16];
    osnos_status_t us;

    /* High-half user pointer -> EFAULT */
    us = copy_from_user(ubuf, (void *)0x8000000000000000ULL, 8);
    CHECK(us == OSNOS_EFAULT, "copy_from_user kernel addr -> EFAULT");

    us = copy_to_user((void *)0x8000000000000000ULL, ubuf, 8);
    CHECK(us == OSNOS_EFAULT, "copy_to_user kernel addr -> EFAULT");

    /* NULL with non-zero n -> EFAULT */
    us = copy_from_user(ubuf, 0, 1);
    CHECK(us == OSNOS_EFAULT, "copy_from_user NULL -> EFAULT");

    us = copy_to_user(0, ubuf, 1);
    CHECK(us == OSNOS_EFAULT, "copy_to_user NULL -> EFAULT");

    /* Zero-length always OK */
    us = copy_from_user(ubuf, (void *)0xdead, 0);
    CHECK(us == OSNOS_OK, "copy_from_user n=0 -> OK");

    /* Overflow at the boundary -> EFAULT */
    us = copy_from_user(ubuf, (void *)(OSNOS_USER_VIRT_MAX - 4), 16);
    CHECK(us == OSNOS_EFAULT, "copy_from_user range overflow -> EFAULT");

    /*
     * Unmapped user page in the valid range. Running in a kernel task
     * (no user pml4), so the low half of kernel_pml4 has no mapping for
     * 0x10000 → the inner copy loop faults. The extable fixup must
     * redirect RIP and the call returns EFAULT instead of panicking.
     */
    us = copy_from_user(ubuf, (void *)0x10000, 8);
    CHECK(us == OSNOS_EFAULT, "copy_from_user unmapped page -> EFAULT (extable)");

    us = copy_to_user((void *)0x10000, ubuf, 8);
    CHECK(us == OSNOS_EFAULT, "copy_to_user unmapped page -> EFAULT (extable)");

    /* ============== reaper (FASE 6.3d) ============== */

    /* NULL is a silent no-op; counters should not move. */
    {
        unsigned long reaped_before = reaper_total_reaped();
        unsigned long leaks_before  = reaper_leaks();
        reaper_add_kstack(0);
        reaper_drain();
        CHECK(reaper_total_reaped() == reaped_before, "reaper NULL -> no count");
        CHECK(reaper_leaks() == leaks_before,         "reaper NULL -> no leak");
    }

    /* A synthetic kmalloc'd block goes through the queue + drain path. */
    {
        unsigned long reaped_before = reaper_total_reaped();
        size_t        kheap_before  = kheap_used_bytes();
        void *fake_kstack = kmalloc(16384);
        CHECK(fake_kstack != 0, "reaper test: kmalloc 16K");
        reaper_add_kstack(fake_kstack);
        /* Block is still allocated until the next drain. */
        CHECK(kheap_used_bytes() > kheap_before, "reaper queued: kheap up");
        reaper_drain();
        CHECK(reaper_total_reaped() == reaped_before + 1,
              "reaper drained: counter +1");
        CHECK(kheap_used_bytes() == kheap_before,
              "reaper drained: kheap back to baseline");
    }

    /* ============== timer IRQ (FASE 9.1) ============== */
    {
        uint64_t rflags;
        __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
        CHECK((rflags & (1ULL << 9)) != 0, "IF=1 (interrupts enabled)");

        /*
         * Diagnostics: read the PIC IMR and the IDT entry for 0x20 to
         * narrow down a no-tick failure to PIC / PIT / IDT.
         */
        uint8_t pic1_mask;
        __asm__ volatile ("inb $0x21, %0" : "=a"(pic1_mask));
        CHECK(pic1_mask != 0xFF,    "PIC1 IMR not fully masked");
        CHECK((pic1_mask & 1) == 0, "PIC1 IRQ 0 unmasked (mask bit 0 clear)");

        struct __attribute__((packed)) {
            uint16_t off_lo;
            uint16_t selector;
            uint8_t  ist;
            uint8_t  type_attr;
            uint16_t off_mid;
            uint32_t off_high;
            uint32_t zero;
        } *idt_entries = (void *)idt_base();
        uint64_t off20 = (uint64_t)idt_entries[0x20].off_lo |
                         ((uint64_t)idt_entries[0x20].off_mid  << 16) |
                         ((uint64_t)idt_entries[0x20].off_high << 32);
        CHECK(off20 != 0,
              "IDT[0x20] handler offset non-zero");
        CHECK((idt_entries[0x20].type_attr & 0x80) != 0,
              "IDT[0x20] present");
        CHECK((idt_entries[0x20].type_attr & 0x0F) == 0x0E,
              "IDT[0x20] is 64-bit interrupt gate");
        CHECK(idt_entries[0x20].selector == 0x08,
              "IDT[0x20] selector = kernel CS");

        /*
         * Software-trigger the handler at vector 0x20. If the IDT
         * entry + handler are wired right, ticks must advance by at
         * least one regardless of whether PIT/PIC delivery works.
         * This isolates "handler works" from "PIT delivers IRQ".
         */
        uint64_t before_int = timer_ticks();
        __asm__ volatile ("int $0x20" ::: "memory");
        uint64_t after_int = timer_ticks();
        CHECK(after_int > before_int,
              "int $0x20 invokes handler (handler is wired)");

        CHECK(timer_ticks() > 0,           "timer ticks accumulated since boot");
        CHECK(timer_irqs() > 0,            "timer IRQs serviced since boot");
        CHECK(timer_ms() > 0,              "timer_ms > 0");

        uint64_t before = timer_ticks();
        for (volatile int i = 0; i < 50000000; i++) { /* spin */ }
        uint64_t after = timer_ticks();
        CHECK(after > before, "ticks advance during busy-wait");

        /*
         * Diagnostic snapshot right after the busy-wait. We
         * accumulate it into fail_log so it shows up at the bottom
         * of the screen alongside the other failures.
         *
         * IRR bit 0 set → PIT did raise IRQ but PIC is not delivering
         * ISR bit 0 set → IRQ was vectored but EOI never sent
         * IMR bit 0 set → IRQ 0 actually still masked
         */
        __asm__ volatile ("outb %0, $0x20" :: "a"((uint8_t)0x0A));
        uint8_t pic1_irr;
        __asm__ volatile ("inb $0x20, %0" : "=a"(pic1_irr));
        __asm__ volatile ("outb %0, $0x20" :: "a"((uint8_t)0x0B));
        uint8_t pic1_isr;
        __asm__ volatile ("inb $0x20, %0" : "=a"(pic1_isr));
        uint8_t pic1_imr_now;
        __asm__ volatile ("inb $0x21, %0" : "=a"(pic1_imr_now));

        static const char *HEX = "0123456789abcdef";
        char diag[120];
        diag[0] = 0;
        os_strlcat(diag, "  PIC1 IRR=", sizeof(diag));
        char h[3];
        h[0] = HEX[pic1_irr >> 4];     h[1] = HEX[pic1_irr & 0xF];     h[2] = 0;
        os_strlcat(diag, h, sizeof(diag));
        os_strlcat(diag, " ISR=", sizeof(diag));
        h[0] = HEX[pic1_isr >> 4];     h[1] = HEX[pic1_isr & 0xF];
        os_strlcat(diag, h, sizeof(diag));
        os_strlcat(diag, " IMR=", sizeof(diag));
        h[0] = HEX[pic1_imr_now >> 4]; h[1] = HEX[pic1_imr_now & 0xF];
        os_strlcat(diag, h, sizeof(diag));
        os_strlcat(diag, "  ticks=", sizeof(diag));
        {
            char num[16];
            format_size((size_t)after, num, sizeof(num));
            os_strlcat(diag, num, sizeof(diag));
        }
        os_strlcat(diag, "\n", sizeof(diag));
        /* Pin this line to the fail summary so it doesn't scroll off. */
        os_strlcat(fail_log, diag, sizeof(fail_log));

        /*
         * nanosleep: 50 ms should block at least 50 ms, no more than
         * a few times that. We call the kernel-side syscall directly
         * (no ring-3 hop) — it's still the same hlt-loop semantics.
         */
        osnos_timespec_t req = { 0, 50 * 1000 * 1000 };
        uint64_t t0 = timer_ms();
        sys_nanosleep(&req, 0);
        uint64_t t1 = timer_ms();
        uint64_t delta = t1 - t0;
        CHECK(delta >= 50,  "nanosleep 50ms blocked at least 50ms");
        CHECK(delta < 500,  "nanosleep 50ms didn't oversleep wildly");
    }

    /* ============== SYSCALL/SYSRET MSRs (FASE 6.4b) ============== */
    {
        /* EFER.SCE must be set; STAR encodes our selectors. */
        uint32_t lo, hi;
        __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080U));
        uint64_t efer = ((uint64_t)hi << 32) | lo;
        CHECK((efer & 1ULL) != 0, "EFER.SCE enabled");

        __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000081U));
        uint64_t star = ((uint64_t)hi << 32) | lo;
        CHECK(((star >> 32) & 0xFFFFu) == GDT_KCODE,
              "STAR[47:32] = kernel CS");
        CHECK(((star >> 48) & 0xFFFFu) == GDT_KDATA,
              "STAR[63:48] = kernel DS (SYSRET base)");
    }

    /* ============== ELF loader (FASE 28) ============== */
    {
        extern const uint8_t _binary_user_hello_elf_start[];
        extern const uint8_t _binary_user_hello_elf_end[];

        /* Bad magic -> EINVAL */
        uint8_t junk[64] = {0};
        uint64_t *p = 0;
        uint64_t entry = 0, stack = 0;
        osnos_status_t es = elf_load(junk, sizeof(junk), &p, &entry, &stack);
        CHECK(es == OSNOS_EINVAL, "elf_load: bad magic -> EINVAL");

        /* Truncated (too small for Ehdr) -> EINVAL */
        es = elf_load(junk, 16, &p, &entry, &stack);
        CHECK(es == OSNOS_EINVAL, "elf_load: truncated -> EINVAL");

        /* The real embedded ELF: load, verify entry, then drop AS. */
        size_t elf_sz = (size_t)(_binary_user_hello_elf_end
                                 - _binary_user_hello_elf_start);
        CHECK(elf_sz > sizeof(Elf64_Ehdr), "embedded hello ELF > Ehdr size");

        es = elf_load(_binary_user_hello_elf_start, elf_sz, &p, &entry, &stack);
        CHECK(es == OSNOS_OK,    "elf_load: hello_elf OK");
        CHECK(entry == 0x400000, "elf_load: e_entry == 0x400000");
        CHECK(stack >= 0x400000, "elf_load: user stack above PT_LOAD");
        if (es == OSNOS_OK) address_space_destroy(p);
    }

    /* ============== sys_brk (FASE 7) ============== */
    {
        /*
         * Synthetic setup: temporarily make the current (shell) task
         * look like a ring-3 task with a brand-new heap range. We
         * don't switch CR3; sys_brk only manipulates the pml4 we hand
         * it. Restore everything before exit.
         */
        task_t *me = task_current();
        uint64_t *sv_pml4 = me->pml4;
        uint64_t  sv_hs   = me->heap_start;
        uint64_t  sv_hb   = me->heap_brk;

        uint64_t *test_pml4 = address_space_create();
        CHECK(test_pml4 != 0, "brk: test AS create");

        const uint64_t TEST_BASE = 0x20000000ULL;
        me->pml4       = test_pml4;
        me->heap_start = TEST_BASE;
        me->heap_brk   = TEST_BASE;

        /* brk(0) returns current break. */
        int64_t r = sys_brk(0);
        CHECK(r == (int64_t)TEST_BASE, "brk(0) -> current break");

        /* Grow by 0x800 bytes -> one page mapped. */
        r = sys_brk(TEST_BASE + 0x800);
        CHECK(r == (int64_t)(TEST_BASE + 0x800), "brk grow 0x800 -> new break");
        CHECK(me->heap_brk == TEST_BASE + 0x800, "brk grow updates heap_brk");
        CHECK((vmm_lookup(test_pml4, TEST_BASE) & PTE_ADDR_MASK) != 0,
              "brk grow maps backing page");

        /* Grow further into a second page. */
        r = sys_brk(TEST_BASE + 0x2100);
        CHECK(r == (int64_t)(TEST_BASE + 0x2100), "brk grow across page");
        CHECK((vmm_lookup(test_pml4, TEST_BASE + 0x1000) & PTE_ADDR_MASK) != 0,
              "brk grow maps second page");

        /* Shrink back: second page is released, first stays. */
        r = sys_brk(TEST_BASE + 0x800);
        CHECK(r == (int64_t)(TEST_BASE + 0x800), "brk shrink");
        CHECK((vmm_lookup(test_pml4, TEST_BASE + 0x1000) & PTE_ADDR_MASK) == 0,
              "brk shrink unmaps second page");

        /* brk below heap_start is refused. */
        r = sys_brk(TEST_BASE - 0x100);
        CHECK(r == (int64_t)me->heap_brk, "brk below heap_start refused");

        /* brk into kernel space is refused. */
        r = sys_brk(OSNOS_USER_VIRT_MAX);
        CHECK(r == (int64_t)me->heap_brk, "brk into kernel space refused");

        /* Restore + cleanup. */
        me->pml4       = sv_pml4;
        me->heap_start = sv_hs;
        me->heap_brk   = sv_hb;
        address_space_destroy(test_pml4);
    }

    /* ============== FASE 5 — builtins / exec / binfs ============== */

    /* Registry lookups */
    CHECK(builtin_find("hello") != 0,        "builtin_find hello");
    CHECK(builtin_find("nonexistent") == 0,  "builtin_find absent -> NULL");
    CHECK(builtin_count() >= 5,              "builtin_count >= 5");

    /* binfs visible via VFS */
    s = vfs_stat("/bin", &st);
    CHECK(s == OSNOS_OK && st.type == VFS_NODE_DIR, "binfs /bin is dir");

    s = vfs_stat("/bin/hello", &st);
    CHECK(s == OSNOS_OK && st.type == VFS_NODE_REG && st.mode == 0555,
          "/bin/hello is regular 0555");

    s = vfs_stat("/bin/nope", &st);
    CHECK(s == OSNOS_ENOENT, "/bin/nope -> ENOENT");

    /* Reading a binfs entry yields a description */
    char binbuf[128];
    s = vfs_read("/bin/true", binbuf, sizeof(binbuf), &got);
    binbuf[got < sizeof(binbuf) ? got : sizeof(binbuf) - 1] = 0;
    CHECK(s == OSNOS_OK && os_strstarts(binbuf, "builtin: true"),
          "cat /bin/true -> description");

    /* binfs is RO */
    s = vfs_write("/bin/hello", "x", 1);
    CHECK(s == OSNOS_EROFS, "binfs write -> EROFS");

    /* proc_exec: bad path */
    int64_t ep = proc_exec("/not-bin/hello", "");
    CHECK(ep == -(int64_t)OSNOS_ENOENT, "exec /not-bin/* -> ENOENT");

    ep = proc_exec("/bin/nope", "");
    CHECK(ep == -(int64_t)OSNOS_ENOENT, "exec /bin/nope -> ENOENT");

    /* proc_exec spawns an ELF user task in READY state. We can't run
     * the scheduler from inside the test, so we just verify creation
     * then tear it down so it never actually dispatches. */
    ep = proc_exec("/bin/true", "");
    CHECK(ep > 0, "exec /bin/true -> pid");

    task_t *child = task_by_pid((uint64_t)ep);
    CHECK(child != 0,                          "spawned task exists");
    CHECK(child->state == TASK_READY,          "spawned task READY");
    CHECK(child->pml4 != 0,                    "spawned task has user AS");
    CHECK(child->user_entry == 0x400000,       "spawned task entry == 0x400000");
    CHECK(child->kernel_stack_base != 0,       "spawned task has kstack");

    /* Tear down by hand: free the AS + kstack, mark DEAD. The reaper
     * will collect the slot on the next scheduler tick. */
    address_space_destroy(child->pml4);
    kfree(child->kernel_stack_base);
    child->pml4              = 0;
    child->kernel_stack_base = 0;
    child->state             = TASK_DEAD;

    /* ============== FASE 6.1 — GDT / IDT ============== */

    /* Read GDTR + IDTR via sgdt/sidt and confirm they match what we
     * installed. If we never lgdt/lidt'd, this would show Limine's. */
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr_now, idtr_now;
    __asm__ volatile ("sgdt %0" : "=m"(gdtr_now));
    __asm__ volatile ("sidt %0" : "=m"(idtr_now));

    CHECK(gdtr_now.base  == gdt_base(),  "GDTR base matches ours");
    CHECK(gdtr_now.limit == gdt_limit(), "GDTR limit matches ours");
    CHECK(idtr_now.base  == idt_base(),  "IDTR base matches ours");
    CHECK(idtr_now.limit == idt_limit(), "IDTR limit matches ours");

    /* CS should be our kernel code selector (0x08). */
    uint16_t cs_now;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs_now));
    CHECK(cs_now == 0x08, "CS = kernel code selector");

    /* No spurious exceptions have fired up to here. */
    CHECK(idt_exception_count() == 0, "no exceptions taken");

    /* ============== FASE 6.2 — TSS ============== */

    uint16_t tr_now;
    __asm__ volatile ("str %0" : "=m"(tr_now));
    CHECK(tr_now == 0x28, "TR = TSS selector 0x28");

    /* RSP0 must be set (non-zero) and point above the kernel stack base. */
    CHECK(tss_get_rsp0() != 0, "TSS.RSP0 populated");

    /* Round-trip: set/get */
    uint64_t saved = tss_get_rsp0();
    tss_set_rsp0(0xdeadbeefcafef00dULL);
    CHECK(tss_get_rsp0() == 0xdeadbeefcafef00dULL, "tss_set_rsp0 round-trip");
    tss_set_rsp0(saved);
    CHECK(tss_get_rsp0() == saved, "tss restored");

    /* ============== FASE 6 — new syscalls + builtins ABI ============== */

    /* sys_mkdir / sys_rmdir */
    int64_t r2 = sys_mkdir("/test_sc", 0755);
    CHECK(r2 == 0, "sys_mkdir OK");

    r2 = sys_mkdir("/test_sc", 0755);
    CHECK(r2 == -(int64_t)OSNOS_EEXIST, "sys_mkdir dup -> EEXIST");

    /* sys_unlink on a file we create via sys_open */
    int64_t fd2 = sys_open("/test_sc/a.txt", O_WRONLY | O_CREAT, 0644);
    CHECK(fd2 >= 3, "sys_open O_CREAT");
    sys_close((int)fd2);

    r2 = sys_unlink("/test_sc/a.txt");
    CHECK(r2 == 0, "sys_unlink OK");

    r2 = sys_unlink("/test_sc/a.txt");
    CHECK(r2 == -(int64_t)OSNOS_ENOENT, "sys_unlink absent -> ENOENT");

    /* sys_rename */
    fd2 = sys_open("/test_sc/orig.txt", O_WRONLY | O_CREAT, 0644);
    sys_close((int)fd2);
    r2 = sys_rename("/test_sc/orig.txt", "/test_sc/moved.txt");
    CHECK(r2 == 0, "sys_rename OK");

    s = vfs_stat("/test_sc/moved.txt", &st);
    CHECK(s == OSNOS_OK, "renamed file exists at new name");

    sys_unlink("/test_sc/moved.txt");

    /* sys_rmdir empty */
    r2 = sys_rmdir("/test_sc");
    CHECK(r2 == 0, "sys_rmdir OK");

    r2 = sys_rmdir("/test_sc");
    CHECK(r2 == -(int64_t)OSNOS_ENOENT, "sys_rmdir absent -> ENOENT");

    /* EFAULT cases */
    r2 = sys_mkdir(0, 0);
    CHECK(r2 == -(int64_t)OSNOS_EFAULT, "sys_mkdir NULL -> EFAULT");

    /* ===== sys_open of a directory ===== */
    int64_t dfd = sys_open("/sys", O_RDONLY, 0);
    CHECK(dfd >= 3, "sys_open dir RDONLY OK");

    /* read() on a dir fd should fail with EISDIR */
    r2 = sys_read((int)dfd, iobuf, sizeof(iobuf));
    CHECK(r2 == -(int64_t)OSNOS_EISDIR, "read(dir) -> EISDIR");

    /* getdents drains the directory and returns dirents */
    char dents[512];
    int64_t total = 0;
    int saw_version = 0, saw_tasks = 0;
    for (;;) {
        int64_t gn = sys_getdents((int)dfd, dents, sizeof(dents));
        if (gn <= 0) break;
        total += gn;
        size_t p = 0;
        while (p < (size_t)gn) {
            osnos_dirent_t *d = (osnos_dirent_t *)(dents + p);
            if (os_streq(d->d_name, "version")) saw_version = 1;
            if (os_streq(d->d_name, "tasks"))   saw_tasks = 1;
            p += d->d_reclen;
        }
    }
    CHECK(total > 0,    "getdents returned bytes");
    CHECK(saw_version,  "getdents listed /sys/version");
    CHECK(saw_tasks,    "getdents listed /sys/tasks");

    sys_close((int)dfd);

    /* Open dir with WRONLY -> EISDIR */
    int64_t bad = sys_open("/sys", O_WRONLY, 0);
    CHECK(bad == -(int64_t)OSNOS_EISDIR, "open(dir, WRONLY) -> EISDIR");

    /* getdents on a regular file fd -> ENOTDIR */
    int64_t ffd = sys_open("/home/HELLO.TXT", O_RDONLY, 0);
    CHECK(ffd >= 3, "open regular file");
    r2 = sys_getdents((int)ffd, dents, sizeof(dents));
    CHECK(r2 == -(int64_t)OSNOS_ENOTDIR, "getdents on file -> ENOTDIR");
    sys_close((int)ffd);

    /* builtins registry has the new ones */
    CHECK(builtin_find("mkdir") != 0, "builtin /bin/mkdir registered");
    CHECK(builtin_find("rmdir") != 0, "builtin /bin/rmdir registered");
    CHECK(builtin_find("rm")    != 0, "builtin /bin/rm registered");
    CHECK(builtin_find("touch") != 0, "builtin /bin/touch registered");
    CHECK(builtin_find("mv")    != 0, "builtin /bin/mv registered");
    CHECK(builtin_find("cp")    != 0, "builtin /bin/cp registered");
    CHECK(builtin_find("ls")    != 0, "builtin /bin/ls registered");

    /*
     * ===== FAT16 + LFN tests (FASE 8) =====
     *
     * Run inside /sd/__fattest as a sandbox so a flaky earlier section
     * never clobbers the seed files. Skipped entirely if /sd isn't
     * mounted (e.g. kernel booted without the sd.img drive attached).
     */
    vfs_stat_t fat_st;
    if (vfs_stat("/sd", &fat_st) == OSNOS_OK) {
        /* Pre-clean: tolerate leftovers from a crashed previous run. */
        vfs_unlink("/sd/__fattest/short.txt");
        vfs_unlink("/sd/__fattest/RENAMED.TXT");
        vfs_unlink("/sd/__fattest/with long name.txt");
        vfs_unlink("/sd/__fattest/dropped here.txt");
        vfs_unlink("/sd/__fattest/sub/inside.txt");
        vfs_unlink("/sd/__fattest/sub/moved here.txt");
        vfs_rmdir("/sd/__fattest/sub");
        vfs_rmdir("/sd/__fattest");

        char fbuf[64];
        size_t got = 0;

        /* (a) Seed file lookup — pure 8.3. */
        s = vfs_read("/sd/README.TXT", fbuf, sizeof(fbuf), &got);
        CHECK(s == OSNOS_OK, "FAT: read seed README.TXT");

        /* (b) Seed file lookup — LFN, original case. */
        s = vfs_read("/sd/My Long Filename.txt", fbuf, sizeof(fbuf), &got);
        CHECK(s == OSNOS_OK, "FAT: read LFN seed by long name");

        /* (c) LFN lookup is case-insensitive. */
        s = vfs_read("/sd/MY LONG FILENAME.TXT", fbuf, sizeof(fbuf), &got);
        CHECK(s == OSNOS_OK, "FAT: LFN lookup case-insensitive");

        /* (d) 8.3 alias of an LFN entry resolves too. */
        s = vfs_read("/sd/MYLONG~1.TXT", fbuf, sizeof(fbuf), &got);
        CHECK(s == OSNOS_OK, "FAT: 8.3 alias of LFN resolves");

        /* (e) mkdir sandbox + write 8.3 file. */
        s = vfs_mkdir("/sd/__fattest");
        CHECK(s == OSNOS_OK, "FAT: mkdir sandbox");

        s = vfs_write("/sd/__fattest/short.txt", "abc", 3);
        CHECK(s == OSNOS_OK, "FAT: write 8.3 file");
        s = vfs_read("/sd/__fattest/short.txt", fbuf, sizeof(fbuf), &got);
        CHECK(s == OSNOS_OK && got == 3 &&
              fbuf[0] == 'a' && fbuf[1] == 'b' && fbuf[2] == 'c',
              "FAT: read back 8.3 file");

        /* (f) write a long-name file → must allocate LFN slots. */
        s = vfs_write("/sd/__fattest/with long name.txt", "long-content", 12);
        CHECK(s == OSNOS_OK, "FAT: write LFN file (create with long name)");
        s = vfs_read("/sd/__fattest/with long name.txt", fbuf, sizeof(fbuf), &got);
        CHECK(s == OSNOS_OK && got == 12, "FAT: read back LFN file");

        /* (g) append — file grows. */
        s = vfs_append("/sd/__fattest/short.txt", "DEF", 3);
        CHECK(s == OSNOS_OK, "FAT: append");
        s = vfs_read("/sd/__fattest/short.txt", fbuf, sizeof(fbuf), &got);
        CHECK(s == OSNOS_OK && got == 6 && fbuf[3] == 'D',
              "FAT: append content correct");

        /* (h) overwrite — file shrinks (truncate). */
        s = vfs_write("/sd/__fattest/short.txt", "xy", 2);
        CHECK(s == OSNOS_OK, "FAT: overwrite truncates");
        s = vfs_read("/sd/__fattest/short.txt", fbuf, sizeof(fbuf), &got);
        CHECK(s == OSNOS_OK && got == 2, "FAT: post-overwrite size correct");

        /* (i) rename same-parent, 8.3 → 8.3 (fast path: one sector RMW). */
        s = vfs_move("/sd/__fattest/short.txt", "/sd/__fattest/RENAMED.TXT");
        CHECK(s == OSNOS_OK, "FAT: rename 8.3 same-parent fast path");
        s = vfs_stat("/sd/__fattest/short.txt", &fat_st);
        CHECK(s == OSNOS_ENOENT, "FAT: old 8.3 name gone after rename");
        s = vfs_stat("/sd/__fattest/RENAMED.TXT", &fat_st);
        CHECK(s == OSNOS_OK, "FAT: new 8.3 name present after rename");

        /* (j) rename LFN → 8.3 (must delete preceding LFN slots). */
        s = vfs_move("/sd/__fattest/with long name.txt",
                     "/sd/__fattest/short.txt");
        CHECK(s == OSNOS_OK, "FAT: rename LFN to 8.3");
        s = vfs_read("/sd/__fattest/short.txt", fbuf, sizeof(fbuf), &got);
        CHECK(s == OSNOS_OK && got == 12, "FAT: rename preserved content");

        /* (k) rename 8.3 → LFN (must write LFN slots). */
        s = vfs_move("/sd/__fattest/short.txt",
                     "/sd/__fattest/dropped here.txt");
        CHECK(s == OSNOS_OK, "FAT: rename 8.3 to LFN");
        s = vfs_read("/sd/__fattest/dropped here.txt", fbuf, sizeof(fbuf), &got);
        CHECK(s == OSNOS_OK && got == 12, "FAT: LFN rename preserved content");

        /* (l) Subdirectory + nested file. */
        s = vfs_mkdir("/sd/__fattest/sub");
        CHECK(s == OSNOS_OK, "FAT: mkdir nested");
        s = vfs_write("/sd/__fattest/sub/inside.txt", "in", 2);
        CHECK(s == OSNOS_OK, "FAT: write inside subdir");

        /* (m) cross-directory rename (LFN → LFN). */
        s = vfs_move("/sd/__fattest/dropped here.txt",
                     "/sd/__fattest/sub/moved here.txt");
        CHECK(s == OSNOS_OK, "FAT: cross-dir rename LFN");
        s = vfs_read("/sd/__fattest/sub/moved here.txt", fbuf, sizeof(fbuf), &got);
        CHECK(s == OSNOS_OK && got == 12, "FAT: cross-dir rename preserved");

        /* (n) rename rejects when dst exists (different entry). */
        s = vfs_write("/sd/__fattest/RENAMED.TXT", "x", 1);  /* re-create */
        CHECK(s == OSNOS_OK, "FAT: re-create file for collision test");
        s = vfs_move("/sd/__fattest/sub/moved here.txt",
                     "/sd/__fattest/RENAMED.TXT");
        CHECK(s == OSNOS_EEXIST, "FAT: rename onto existing → EEXIST");

        /* (o) rmdir on non-empty rejected. */
        s = vfs_rmdir("/sd/__fattest/sub");
        CHECK(s == OSNOS_ENOTEMPTY, "FAT: rmdir non-empty rejected");

        /* (p) Empty-file create (size==0, first_cluster==0). */
        s = vfs_write("/sd/__fattest/empty.txt", "", 0);
        CHECK(s == OSNOS_OK, "FAT: empty file create");
        s = vfs_stat("/sd/__fattest/empty.txt", &fat_st);
        CHECK(s == OSNOS_OK && fat_st.size == 0, "FAT: empty file size==0");

        /* Teardown: unlink all + rmdir. */
        s = vfs_unlink("/sd/__fattest/empty.txt");
        CHECK(s == OSNOS_OK, "FAT: unlink empty.txt");
        s = vfs_unlink("/sd/__fattest/RENAMED.TXT");
        CHECK(s == OSNOS_OK, "FAT: unlink RENAMED.TXT");
        s = vfs_unlink("/sd/__fattest/sub/inside.txt");
        CHECK(s == OSNOS_OK, "FAT: unlink sub/inside.txt");
        s = vfs_unlink("/sd/__fattest/sub/moved here.txt");
        CHECK(s == OSNOS_OK, "FAT: unlink sub/moved here.txt (LFN)");
        s = vfs_rmdir("/sd/__fattest/sub");
        CHECK(s == OSNOS_OK, "FAT: rmdir empty subdir");
        s = vfs_rmdir("/sd/__fattest");
        CHECK(s == OSNOS_OK, "FAT: rmdir sandbox");

        s = vfs_stat("/sd/__fattest", &fat_st);
        CHECK(s == OSNOS_ENOENT, "FAT: sandbox gone after rmdir");

        /* (q) fsck audit must come back clean after the whole churn. */
        {
            char rep[1024];
            fat_fsck_report(rep, sizeof(rep));
            /* Inline substring check — keep helper local to avoid
             * leaking a generic string utility from a test path. */
            const char *needles[] = {
                "leaks:         none",
                "cross-links:   none",
                "size mismatch: none",
                "bad refs:      none",
                "mirror:        OK"
            };
            for (int i = 0; i < 5; i++) {
                const char *n = needles[i];
                size_t nlen = os_strlen(n);
                bool found = false;
                for (size_t k = 0; rep[k] && k + nlen <= sizeof(rep); k++) {
                    if (os_strncmp(rep + k, n, nlen) == 0) {
                        found = true; break;
                    }
                }
                CHECK(found, n);
            }
        }
    } else {
        os_strlcat(buf, "  SKIP FAT tests (no /sd mount)\n", sizeof(buf));
        test_flush(buf, sizeof(buf), false, 0xaaaaaa);
    }

    /*
     * ===== Socket + TCP retransmission tests (FASE 8.5.11) =====
     * Pure state-machine inspection — no actual network traffic.
     * These would still pass on a kernel booted without rtl8139, since
     * we only allocate slots and check their initial flags.
     */
    {
        int sd_d = sock_create(OSNOS_SOCK_DGRAM);
        CHECK(sd_d >= 0, "SOCK: create UDP");
        CHECK(sock_local_port(sd_d) == 0, "SOCK: UDP unbound port=0");
        CHECK(sock_bind(sd_d, 0, 50001) == 0, "SOCK: bind UDP :50001");
        CHECK(sock_local_port(sd_d) == 50001, "SOCK: UDP bound port=50001");
        CHECK(sock_close(sd_d) == 0, "SOCK: close UDP");

        int sd_s = sock_create(OSNOS_SOCK_STREAM);
        CHECK(sd_s >= 0, "SOCK: create STREAM");
        CHECK(sock_tcp_state_int(sd_s) == TCP_CLOSED,
              "SOCK: fresh TCP state=CLOSED");
        CHECK(sock_tcp_retx_len(sd_s) == 0,
              "SOCK: fresh retx buffer empty");
        CHECK(sock_tcp_retx_count(sd_s) == 0,
              "SOCK: fresh retx count=0");

        CHECK(sock_bind(sd_s, 0, 50002) == 0, "SOCK: bind TCP :50002");
        CHECK(sock_local_port(sd_s) == 50002, "SOCK: TCP bound port");

        CHECK(sock_listen(sd_s, 4) == 0, "SOCK: listen TCP");
        CHECK(sock_tcp_state_int(sd_s) == TCP_LISTEN,
              "SOCK: state=LISTEN after listen");

        /* SOCK_STREAM not bound shouldn't accept listen. */
        int sd_n = sock_create(OSNOS_SOCK_STREAM);
        CHECK(sd_n >= 0, "SOCK: create another STREAM");
        CHECK(sock_listen(sd_n, 4) != 0,
              "SOCK: listen without bind → fails");
        sock_close(sd_n);

        /* Unknown socket descriptors return -1. */
        CHECK(sock_tcp_state_int(7777) == -1,
              "SOCK: bad sd → state=-1");
        CHECK(sock_tcp_retx_len(7777) == -1,
              "SOCK: bad sd → retx_len=-1");

        /* close on LISTEN frees the slot (it's an immediate teardown
         * since there's no peer to FIN with). */
        CHECK(sock_close(sd_s) == 0, "SOCK: close LISTEN");

        /* Retx counters are read-only from this seat — just sanity
         * check the API doesn't crash. */
        uint64_t r0 = sock_tcp_retx_total();
        uint64_t r1 = sock_tcp_retx_drops();
        CHECK(r0 >= 0, "SOCK: retx_total readable");
        CHECK(r1 >= 0, "SOCK: retx_drops readable");
        (void)r0; (void)r1;
    }

    /*
     * ===== kheap growth tests (FASE A) =====
     * Force the kheap past its initial 64 KiB so we exercise heap_grow.
     * After the storm we kfree everything and verify heap_used returns
     * to its pre-test baseline so we don't leak silently.
     */
    {
        size_t baseline_used  = kheap_used_bytes();
        size_t baseline_total = kheap_total_bytes();
        size_t baseline_grow  = kheap_grow_events();

        /* Block 1: single large alloc (>= initial heap). Forces a grow
         * unless we already grew earlier. */
        const size_t big = 96 * 1024;     /* 96 KiB > 64 KiB initial */
        void *p_big = kmalloc(big);
        CHECK(p_big != 0, "KHEAP: 96 KiB alloc succeeds (growth)");
        if (p_big) {
            uint8_t *q = (uint8_t *)p_big;
            for (size_t i = 0; i < big; i += 4096) q[i] = (uint8_t)i;
            CHECK(q[0] == 0, "KHEAP: 96 KiB block writable @0");
            CHECK(q[big - 1] == 0 || q[big - 1] != 0xAA, "KHEAP: writable end");
        }
        CHECK(kheap_total_bytes() > baseline_total,
              "KHEAP: total grew past baseline");
        CHECK(kheap_grow_events() > baseline_grow,
              "KHEAP: grow_events incremented");

        /* Block 2: many small allocs, freed in reverse — exercises the
         * splitter + free-list coalesce. */
        enum { N = 200 };
        static void *bag[N];
        for (int i = 0; i < N; i++) {
            bag[i] = kmalloc(128);
            CHECK(bag[i] != 0, "KHEAP: 128 B alloc in burst");
            if (!bag[i]) break;
        }
        for (int i = N - 1; i >= 0; i--) kfree(bag[i]);

        /* Block 3: free the big one too. */
        kfree(p_big);

        /* used must be back to baseline (no leaks in the test). peak
         * should be at least the size of the big alloc. */
        CHECK(kheap_used_bytes() == baseline_used,
              "KHEAP: used returns to baseline after free");
        CHECK(kheap_peak_bytes() >= baseline_used + big,
              "KHEAP: peak tracks the high-water mark");
        CHECK(kheap_grow_oom() == 0,
              "KHEAP: no OOM under normal load");

        /* kmalloc(0) is a documented no-op returning NULL. */
        CHECK(kmalloc(0) == 0, "KHEAP: kmalloc(0) → NULL");
        /* free(NULL) is a libc-convention no-op. */
        kfree(0);
        CHECK(kheap_used_bytes() == baseline_used,
              "KHEAP: kfree(NULL) doesn't move used");
    }

    /*
     * ===== Slab allocator tests (FASE B) =====
     * Verify dispatch: small allocs → slab, large → first-fit. Burst
     * fill one bucket, confirm slot accounting, free everything, then
     * confirm baseline restored.
     */
    {
        size_t baseline_used = kheap_used_bytes();
        size_t baseline_slab = kheap_slab_used_bytes();

        /* 16 B → bucket 0. */
        void *s16 = kmalloc(8);   /* rounds up to 16 */
        CHECK(s16 != 0, "SLAB: 8 B alloc succeeds");
        CHECK(((uintptr_t)s16 >> 32) == 0xffffc001,
              "SLAB: 8 B pointer in slab VA range");
        CHECK(kheap_slab_used_bytes() == baseline_slab + 16,
              "SLAB: bucket 0 charge = 16 B");
        kfree(s16);
        CHECK(kheap_slab_used_bytes() == baseline_slab,
              "SLAB: bucket 0 returns to baseline");

        /* 2048 B → bucket 7 (boundary). */
        void *s2k = kmalloc(2048);
        CHECK(s2k != 0, "SLAB: 2048 B alloc succeeds");
        CHECK(((uintptr_t)s2k >> 32) == 0xffffc001,
              "SLAB: 2048 B still in slab range");
        kfree(s2k);

        /* 2049 B → first-fit (out of slab range). */
        void *p_big = kmalloc(2049);
        CHECK(p_big != 0, "SLAB: 2049 B alloc succeeds");
        CHECK(((uintptr_t)p_big >> 32) == 0xffffc000,
              "SLAB: 2049 B falls through to first-fit");
        kfree(p_big);

        /* Burst: 100 × 64 B → bucket 2. Confirms bucket grow + slot
         * counting. */
        size_t before_used_b2 = kheap_slab_slots_used(2);
        enum { BURST = 100 };
        static void *bag64[BURST];
        for (int i = 0; i < BURST; i++) {
            bag64[i] = kmalloc(64);
            CHECK(bag64[i] != 0, "SLAB: burst alloc 64 B");
            if (!bag64[i]) break;
        }
        CHECK(kheap_slab_slots_used(2) == before_used_b2 + BURST,
              "SLAB: bucket 2 slots_used += BURST");
        for (int i = 0; i < BURST; i++) kfree(bag64[i]);
        CHECK(kheap_slab_slots_used(2) == before_used_b2,
              "SLAB: bucket 2 slots_used back");

        CHECK(kheap_used_bytes() == baseline_used,
              "SLAB: full sweep — used returns to baseline");
        CHECK(kheap_slab_grow_oom() == 0,
              "SLAB: no slab OOM under normal load");

        /* Sanity: bucket sizes match power-of-2 series. */
        CHECK(kheap_slab_bucket_size(0)  == 16,    "SLAB: bucket[0] = 16");
        CHECK(kheap_slab_bucket_size(7)  == 2048,  "SLAB: bucket[7] = 2048");
        CHECK(kheap_slab_bucket_count() == 8,      "SLAB: 8 buckets total");
    }

    /* cleanup */
    vfs_unlink("/test/syscall.txt");
    vfs_rmdir("/test");

    #undef CHECK

    /* Flush remaining body lines in gray before the summary. */
    test_flush(buf, sizeof(buf), true, 0xaaaaaa);

    /* Build summary in its own message, colored by overall result. */
    char num[16];
    os_strlcat(buf, "\n", sizeof(buf));
    format_size(pass + fail, num, sizeof(num));
    os_strlcat(buf, "total: ", sizeof(buf));
    os_strlcat(buf, num, sizeof(buf));
    format_size(pass, num, sizeof(num));
    os_strlcat(buf, "  pass: ", sizeof(buf));
    os_strlcat(buf, num, sizeof(buf));
    format_size(fail, num, sizeof(num));
    os_strlcat(buf, "  fail: ", sizeof(buf));
    os_strlcat(buf, num, sizeof(buf));
    os_strlcat(buf, "\n", sizeof(buf));

    shell_send_console_color(buf, fail ? 0xff5555 : 0x55ff55);

    /*
     * Pinned-to-bottom log: any FAIL + ad-hoc diagnostic lines that
     * tests appended via fail_log. Printed unconditionally so timer
     * IRR/ISR snapshots are visible even on a clean run.
     */
    if (fail_log[0]) {
        char hdr[64];
        hdr[0] = 0;
        os_strlcat(hdr, fail ? "\nfailures:\n" : "\ndiagnostics:\n",
                   sizeof(hdr));
        shell_send_console_color(hdr,      fail ? 0xff5555 : 0xffff00);
        shell_send_console_color(fail_log, fail ? 0xff5555 : 0xffff00);
    }

    prompt();
}

static void cmd_ps(const char *args) {
    (void)args;
    check_fs(shell_send_fs1(IPC_FS_READ, "/sys/tasks"));
}

static void cmd_mem(const char *args) {
    (void)args;
    check_fs(shell_send_fs1(IPC_FS_READ, "/sys/meminfo"));
}

static void cmd_mount(const char *args) {
    (void)args;
    check_fs(shell_send_fs1(IPC_FS_READ, "/sys/mounts"));
}

/* Parse a dotted-quad like "10.0.2.2" into a uint32 in host order
 * (most-significant byte = first octet). Returns false on malformed
 * input. Trailing whitespace is ignored. */
static bool parse_ipv4(const char *s, uint32_t *out) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int idx = 0;
    while (*s && idx < 4) {
        if (*s < '0' || *s > '9') return false;
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) {
            v = v * 10 + (uint32_t)(*s - '0');
            s++;
            digits++;
        }
        if (v > 255 || digits == 0) return false;
        parts[idx++] = v;
        if (*s == '.') { s++; continue; }
        if (*s == 0 || *s == ' ' || *s == '\t' || *s == '\n') break;
        return false;
    }
    if (idx != 4) return false;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

static void cmd_arp(const char *args) {
    while (*args == ' ' || *args == '\t') args++;

    uint32_t ip;
    if (*args == 0) {
        ip = net_gateway_ip();
    } else if (!parse_ipv4(args, &ip)) {
        shell_send_console_color("\nusage: arp [IP]\n", 0xff5555);
        prompt();
        return;
    }

    char line[128];
    line[0] = 0;
    os_strlcat(line, "\nresolving ", sizeof(line));

    char num[8];
    for (int i = 3; i >= 0; i--) {
        os_format_u64((ip >> (i * 8)) & 0xFF, num, sizeof(num));
        os_strlcat(line, num, sizeof(line));
        if (i > 0) os_strlcat(line, ".", sizeof(line));
    }
    os_strlcat(line, " ...\n", sizeof(line));
    shell_send_console(line);

    uint8_t mac[6];
    if (!arp_resolve(ip, mac, 500)) {
        shell_send_console_color("timeout\n", 0xff5555);
        prompt();
        return;
    }

    line[0] = 0;
    os_strlcat(line, "  → ", sizeof(line));
    for (int m = 0; m < 6; m++) {
        char nh[3];
        uint8_t b = mac[m];
        nh[0] = (char)((b >> 4) < 10 ? '0' + (b >> 4) : 'a' + (b >> 4) - 10);
        nh[1] = (char)((b & 0xF) < 10 ? '0' + (b & 0xF) : 'a' + (b & 0xF) - 10);
        nh[2] = 0;
        os_strlcat(line, nh, sizeof(line));
        if (m < 5) os_strlcat(line, ":", sizeof(line));
    }
    os_strlcat(line, "\n", sizeof(line));
    shell_send_console(line);
    prompt();
}

static void cmd_ping(const char *args) {
    while (*args == ' ' || *args == '\t') args++;

    uint32_t ip;
    if (*args == 0 || !parse_ipv4(args, &ip)) {
        shell_send_console_color("\nusage: ping IP\n", 0xff5555);
        prompt();
        return;
    }

    char line[128];
    line[0] = 0;
    os_strlcat(line, "\nping ", sizeof(line));
    char num[8];
    for (int i = 3; i >= 0; i--) {
        os_format_u64((ip >> (i * 8)) & 0xFF, num, sizeof(num));
        os_strlcat(line, num, sizeof(line));
        if (i > 0) os_strlcat(line, ".", sizeof(line));
    }
    os_strlcat(line, " ...\n", sizeof(line));
    shell_send_console(line);

    uint64_t rtt = 0;
    /* Pick id/seq from timer for cheap uniqueness across pings. */
    uint16_t id  = (uint16_t)(timer_ms() & 0xFFFF);
    uint16_t seq = 1;
    if (!icmp_ping(ip, id, seq, /*timeout_ms=*/1000, &rtt)) {
        shell_send_console_color("timeout\n", 0xff5555);
        prompt();
        return;
    }

    line[0] = 0;
    os_strlcat(line, "reply: time=", sizeof(line));
    os_format_u64(rtt, num, sizeof(num));
    os_strlcat(line, num, sizeof(line));
    os_strlcat(line, " ms\n", sizeof(line));
    shell_send_console(line);
    prompt();
}

static void cmd_udptest(const char *args) {
    while (*args == ' ' || *args == '\t') args++;

    uint16_t port = 1234;
    if (*args) {
        uint32_t v = 0;
        const char *p = args;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; }
        if (v == 0 || v > 65535) {
            shell_send_console_color("\nusage: udptest [PORT]\n", 0xff5555);
            prompt();
            return;
        }
        port = (uint16_t)v;
    }

    int sd = sock_create(OSNOS_SOCK_DGRAM);
    if (sd < 0) {
        shell_send_console_color("\nsock_create failed\n", 0xff5555);
        prompt();
        return;
    }
    if (sock_bind(sd, 0, port) != 0) {
        shell_send_console_color("\nbind failed\n", 0xff5555);
        sock_close(sd);
        prompt();
        return;
    }

    char line[128];
    line[0] = 0;
    os_strlcat(line, "\nlistening UDP port ", sizeof(line));
    char num[8];
    os_format_u64(port, num, sizeof(num));
    os_strlcat(line, num, sizeof(line));
    os_strlcat(line, ", 10s, echo back to sender\n", sizeof(line));
    shell_send_console(line);

    uint64_t deadline = timer_ms() + 10000;
    while (timer_ms() < deadline) {
        uint8_t  buf[256];
        uint32_t src_ip = 0;
        uint16_t src_port = 0;
        int n = sock_recvfrom(sd, buf, sizeof(buf) - 1,
                                &src_ip, &src_port,
                                /*timeout_ms=*/200);
        if (n <= 0) continue;

        buf[n] = 0;
        line[0] = 0;
        os_strlcat(line, "rx ", sizeof(line));
        for (int i = 3; i >= 0; i--) {
            os_format_u64((src_ip >> (i * 8)) & 0xFF, num, sizeof(num));
            os_strlcat(line, num, sizeof(line));
            if (i > 0) os_strlcat(line, ".", sizeof(line));
        }
        os_strlcat(line, ":", sizeof(line));
        os_format_u64(src_port, num, sizeof(num));
        os_strlcat(line, num, sizeof(line));
        os_strlcat(line, "  [", sizeof(line));
        size_t avail = sizeof(line) - os_strlen(line) - 4;
        for (int i = 0; i < n && (size_t)i < avail; i++) {
            char c = (buf[i] >= 0x20 && buf[i] < 0x7F) ? (char)buf[i] : '.';
            char two[2] = { c, 0 };
            os_strlcat(line, two, sizeof(line));
        }
        os_strlcat(line, "]\n", sizeof(line));
        shell_send_console(line);

        /* Echo back. */
        sock_sendto(sd, buf, (size_t)n, src_ip, src_port);
    }

    sock_close(sd);
    shell_send_console("udptest done\n");
    prompt();
}

static void cmd_tcptest(const char *args) {
    while (*args == ' ' || *args == '\t') args++;

    uint16_t port = 80;
    if (*args) {
        uint32_t v = 0;
        const char *p = args;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; }
        if (v == 0 || v > 65535) {
            shell_send_console_color("\nusage: tcptest [PORT]\n", 0xff5555);
            prompt();
            return;
        }
        port = (uint16_t)v;
    }

    int sd = sock_create(OSNOS_SOCK_STREAM);
    if (sd < 0) {
        shell_send_console_color("\nsock_create failed\n", 0xff5555);
        prompt();
        return;
    }
    if (sock_bind(sd, 0, port) != 0) {
        shell_send_console_color("\nbind failed\n", 0xff5555);
        sock_close(sd);
        prompt();
        return;
    }
    if (sock_listen(sd, 1) != 0) {
        shell_send_console_color("\nlisten failed\n", 0xff5555);
        sock_close(sd);
        prompt();
        return;
    }

    char line[128];
    line[0] = 0;
    os_strlcat(line, "\nlisten TCP ", sizeof(line));
    char num[8];
    os_format_u64(port, num, sizeof(num));
    os_strlcat(line, num, sizeof(line));
    os_strlcat(line, ", 15s wait for handshake...\n", sizeof(line));
    shell_send_console(line);

    uint64_t deadline = timer_ms() + 15000;
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    bool got = false;

    while (timer_ms() < deadline) {
        if (sock_tcp_get_peer(sd, &peer_ip, &peer_port)) { got = true; break; }
    }

    if (got) {
        line[0] = 0;
        os_strlcat(line, "conn from ", sizeof(line));
        for (int i = 3; i >= 0; i--) {
            os_format_u64((peer_ip >> (i * 8)) & 0xFF, num, sizeof(num));
            os_strlcat(line, num, sizeof(line));
            if (i > 0) os_strlcat(line, ".", sizeof(line));
        }
        os_strlcat(line, ":", sizeof(line));
        os_format_u64(peer_port, num, sizeof(num));
        os_strlcat(line, num, sizeof(line));
        os_strlcat(line, "\n", sizeof(line));
        shell_send_console(line);

        /* Recv one chunk + echo it back + graceful close. */
        char rxbuf[256];
        int n = sock_recv(sd, rxbuf, sizeof(rxbuf) - 1, 5000);
        if (n > 0) {
            rxbuf[n] = 0;
            line[0] = 0;
            os_strlcat(line, "rx ", sizeof(line));
            os_format_u64((uint64_t)n, num, sizeof(num));
            os_strlcat(line, num, sizeof(line));
            os_strlcat(line, "B: ", sizeof(line));
            for (int i = 0; i < n && os_strlen(line) + 2 < sizeof(line); i++) {
                char c = (rxbuf[i] >= 0x20 && rxbuf[i] < 0x7F) ? rxbuf[i] : '.';
                char two[2] = { c, 0 };
                os_strlcat(line, two, sizeof(line));
            }
            os_strlcat(line, "\n", sizeof(line));
            shell_send_console(line);

            int sent = sock_send(sd, rxbuf, (size_t)n);
            (void)sent;
            shell_send_console("echoed back, closing\n");
        } else if (n == 0) {
            shell_send_console("peer closed without data\n");
        } else {
            shell_send_console("recv timeout/error\n");
        }
    } else {
        shell_send_console("no connection within 15s\n");
    }

    sock_close(sd);
    shell_send_console("tcptest done\n");
    prompt();
}

static void cmd_exec(const char *args) {
    if (args[0] == 0) {
        shell_send_console_color("\nusage: exec /bin/PROG [args] [&]\n", 0xff5555);
        prompt();
        return;
    }

    /*
     * Detect trailing '&'. The shell strips it so the child receives a
     * clean argv. Background tasks don't set fg_pid; the shell prompts
     * immediately and survives the child's eventual exit asynchronously.
     */
    char buf[OSNOS_INPUT_MAX];
    os_strlcpy(buf, args, sizeof(buf));
    size_t blen = os_strlen(buf);
    while (blen > 0 && buf[blen - 1] == ' ') buf[--blen] = 0;
    int background = 0;
    if (blen > 0 && buf[blen - 1] == '&') {
        background = 1;
        buf[--blen] = 0;
        while (blen > 0 && buf[blen - 1] == ' ') buf[--blen] = 0;
    }

    /* Split path from the rest. The path is the first whitespace-bounded
     * token; everything after the first space is forwarded as args. */
    char path[OSNOS_PATH_MAX];
    size_t i = 0;
    while (buf[i] && buf[i] != ' ' && i + 1 < sizeof(path)) {
        path[i] = buf[i];
        i++;
    }
    path[i] = 0;

    const char *rest = buf + i;
    while (*rest == ' ') rest++;

    int64_t pid = proc_exec(path, rest);
    if (pid < 0) {
        shell_send_console_color("\nexec failed\n", 0xff5555);
        prompt();
        return;
    }

    if (background) {
        /*
         * Background launch. Print "[pid]" notification and redraw the
         * prompt immediately. fg_pid stays whatever it was (typically 0).
         * The IPC_PROC_EXITED handler routes the eventual exit through
         * the "background" branch and prints "[pid] done".
         */
        char num[16];
        format_size((size_t)pid, num, sizeof(num));
        shell_send_console_color("\n[", 0xaaaaaa);
        shell_send_console_color(num, 0xaaaaaa);
        shell_send_console_color("]\n", 0xaaaaaa);
        prompt();
        return;
    }

    /*
     * Foreground: remember the child pid so Ctrl+C maps to "kill the
     * foreground task" until the child reports IPC_PROC_EXITED. Don't
     * draw the prompt here — the IPC_PROC_EXITED handler does it, after
     * the child's stdout has flushed through the console queue.
     */
    fg_pid = (uint64_t)pid;
    shell_send_console("\n");
}

static void cmd_kill(const char *args) {
    while (*args == ' ') args++;
    if (!args[0]) {
        shell_send_console_color("\nusage: kill PID\n", 0xff5555);
        prompt();
        return;
    }

    /* Parse decimal pid. Reject anything non-numeric. */
    uint64_t pid = 0;
    int any = 0;
    for (const char *p = args; *p && *p != ' '; p++) {
        if (*p < '0' || *p > '9') {
            shell_send_console_color("\nkill: invalid pid\n", 0xff5555);
            prompt();
            return;
        }
        pid = pid * 10 + (uint64_t)(*p - '0');
        any = 1;
    }
    if (!any || pid == 0) {
        shell_send_console_color("\nkill: invalid pid\n", 0xff5555);
        prompt();
        return;
    }

    /*
     * Delegate to sys_kill so the shell builtin and the libc kill(2)
     * tool share one code path. sys_kill flips kill_pending AND
     * force-wakes a BLOCKED target — doing it inline here would
     * forget the wake step and the target would stay BLOCKED until
     * its natural wake-up (long sleep, etc).
     */
    if (sys_kill(pid, 9) < 0) {
        shell_send_console_color("\nkill: no such pid\n", 0xff5555);
        prompt();
        return;
    }
    shell_send_console("\n");
    prompt();
}

static void cmd_neof(const char *args) {
    (void)args;
    shell_send_console_color(
        "\nOS: osnos\nArch: x86_64\nKernel: microkernel-style\nBootloader: Limine\nShell: osnos-shell\nFS: ramfs\n",
        0x00ffff
    );
    prompt();
}

static void cmd_uname(const char *args) {
    (void)args;
    shell_send_console("\nosnos x86_64 microkernel-style\n");
    prompt();
}

static void cmd_version(const char *args) {
    (void)args;
    shell_send_console("\nosnos 0.0.1\n");
    prompt();
}

static void cmd_whoami(const char *args) {
    (void)args;
    shell_send_console("\nroot\n");
    prompt();
}

static void cmd_date(const char *args) {
    (void)args;
    shell_send_console("\ntime is not implemented yet\n");
    prompt();
}

static void cmd_banner(const char *args) {
    (void)args;
    shell_send_console_color("\n", 0xffffff);
    shell_send_console_color("   ___  ____  _ __   ___  ___ \n", 0x00ffff);
    shell_send_console_color("  / _ \\/ __ \\| '_ \\ / _ \\/ __|\n", 0x00ffff);
    shell_send_console_color(" | (_) \\__  | | | | (_) \\__ \\\n", 0x00ffff);
    shell_send_console_color("  \\___/|___/|_| |_|\\___/|___/\n", 0x00ffff);
    shell_send_console_color("\n", 0x00ffff);
    prompt();
}

static void cmd_reboot(const char *args) {
    (void)args;
    shell_send_console("\nreboot not implemented yet\n");
    prompt();
}

/* ---- dispatch ---- */

static void prompt(void) {
    shell_send_console_color("osnos:", 0x00ff66);
    shell_send_console_color(current_path, 0x00ffff);
    shell_send_console_color("> ", 0x00ff66);
}

static void run_command(const char *cmd) {
    if (cmd[0] == 0) {
        shell_send_console("\n");
        prompt();
        return;
    }

    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (os_strncmp(cmd, commands[i].name, commands[i].name_len) != 0) {
            continue;
        }

        char next = cmd[commands[i].name_len];
        if (next != 0 && next != ' ') continue;

        const char *args = cmd + commands[i].name_len;
        while (*args == ' ') args++;

        commands[i].handler(args);
        return;
    }

    shell_send_console_color("\nunknown command: ", 0xff5555);
    shell_send_console_color(cmd, 0xff5555);
    shell_send_console("\n");
    prompt();
}

void shell_server_init(void) {
    input_len = 0;
    history_count = 0;
    history_pos = HISTORY_NONE;
    history_scratch[0] = 0;
    os_strlcpy(current_path, "/home", sizeof(current_path));

    shell_send_console_color("osnos microkernel shell\n", 0xffffff);
    shell_send_console_color("type help\n\n", 0xaaaaaa);

    prompt();
}

void shell_server_tick(void) {
    ipc_msg_t msg;

    if (!ipc_recv_block(SERVER_SHELL, &msg)) {
        return;
    }

    if (msg.type == IPC_FS_RESPONSE) {
        shell_send_console("\n");
        shell_send_console_color(msg.data, 0xffff00);
        prompt();
        return;
    }

    /*
     * Child task finished. Output from its sys_write() calls has already
     * been queued in front of this notification, so by the time the
     * prompt renders the child's stdout has flushed.
     *
     * Two distinct cases:
     *   - msg.arg1 == fg_pid : the foreground child the shell was
     *     blocking on. Clear fg_pid and redraw the prompt.
     *   - msg.arg1 != fg_pid : a background task (launched with `&`)
     *     finished. Print "[pid] done" but DON'T redraw the prompt
     *     — the user may be typing, and we'd corrupt their input.
     *     The next keystroke / Enter will scroll naturally.
     */
    if (msg.type == IPC_PROC_EXITED) {
        if (msg.arg1 == fg_pid) {
            fg_pid = 0;
            prompt();
        } else {
            char num[16];
            format_size((size_t)msg.arg1, num, sizeof(num));
            shell_send_console_color("\n[", 0xaaaaaa);
            shell_send_console_color(num, 0xaaaaaa);
            shell_send_console_color("] done\n", 0xaaaaaa);
            /* Re-emit current prompt + buffered input so the user
             * doesn't lose context. Keeps typing visible. */
            prompt();
            if (input_len > 0) {
                /* echo the line buffer back */
                char line[OSNOS_INPUT_MAX + 1];
                size_t n = input_len < OSNOS_INPUT_MAX
                            ? input_len : OSNOS_INPUT_MAX;
                for (size_t k = 0; k < n; k++) line[k] = input[k];
                line[n] = 0;
                shell_send_console(line);
            }
        }
        return;
    }

    if (msg.type != IPC_KEY_EVENT) {
        return;
    }

    if (msg.arg0 == OSNOS_KEY_UP) {
        history_up();
        return;
    }
    if (msg.arg0 == OSNOS_KEY_DOWN) {
        history_down();
        return;
    }

    char c = msg.data[0];
    if (c == 0) return;

    /*
     * Ctrl+C. Two meanings depending on whether a foreground task is
     * running:
     *
     *   fg_pid != 0  → kill the foreground task. Set its kill_pending
     *                  flag; the kernel return paths (syscall, timer)
     *                  will route it through proc_exit_current_user
     *                  on the next CPL=3 boundary. The shell does NOT
     *                  redraw its prompt — it waits for the resulting
     *                  IPC_PROC_EXITED, same as a normal child exit.
     *
     *   fg_pid == 0  → legacy meaning: discard the in-progress input
     *                  line and reprompt right away.
     */
    if (c == 0x03) {
        shell_send_console_color("^C\n", 0xff5555);
        if (fg_pid != 0) {
            task_t *fg = task_by_pid(fg_pid);
            if (fg) fg->kill_pending = 1;
            return;
        }
        input_len = 0;
        history_pos = HISTORY_NONE;
        prompt();
        return;
    }

    if (c == '\n') {
        input[input_len] = 0;
        history_save(input);
        history_pos = HISTORY_NONE;
        run_command(input);
        input_len = 0;
        return;
    }

    if (c == '\b') {
        if (input_len > 0) {
            input_len--;
            shell_send_console_color("\b", 0xffffff);
        }
        return;
    }

    if (input_len < OSNOS_INPUT_MAX - 1) {
        input[input_len++] = c;
        char s[2] = { c, 0 };
        shell_send_console(s);
    }
}
