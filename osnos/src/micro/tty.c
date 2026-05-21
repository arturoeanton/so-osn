#include "tty.h"

#include "../drivers/framebuffer.h"
#include "../servers/shell_server.h"
#include "task.h"

/* ----- Module state ----- */

#define TTY_READ_BUF   256
#define TTY_LINE_BUF   256

static struct osnos_termios tty_t;

static char     read_buf[TTY_READ_BUF];
static size_t   read_head, read_tail, read_count;

static char     line_buf[TTY_LINE_BUF];
static size_t   line_len;

static uint64_t stat_chars_in;
static uint64_t stat_lines_out;
static uint64_t stat_signals;

static inline void cli(void) { __asm__ volatile ("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile ("sti" ::: "memory"); }

static void read_buf_push(char c) {
    if (read_count >= TTY_READ_BUF) {
        /* Drop oldest — the kernel never blocks on its own buffer.  */
        read_tail = (read_tail + 1) % TTY_READ_BUF;
        read_count--;
    }
    read_buf[read_head] = c;
    read_head = (read_head + 1) % TTY_READ_BUF;
    read_count++;
}

/*
 * Echo helper. Skipped while the shell itself is the foreground
 * consumer — the shell does its own echo in the IPC_KEY_EVENT path,
 * so a TTY echo on top would double everything. When a user ELF is
 * fg, the shell is blocked on IPC_PROC_EXITED and isn't drawing
 * anything, so the TTY is responsible.
 */
static void tty_echo_char(char c) {
    if (!(tty_t.c_lflag & TTY_ECHO)) return;
    if (shell_fg_pid() == 0) return;          /* shell is fg → it'll echo */
    char s[2] = { c, 0 };
    framebuffer_draw_string(s, 0xffffff);
}

static void tty_echo_erase(void) {
    if (!(tty_t.c_lflag & TTY_ECHOE)) return;
    if (shell_fg_pid() == 0) return;
    framebuffer_backspace();
}

/* Deliver SIGINT to the current foreground user task. Kernel tasks
 * (the shell included) are never targeted — they cancel their own
 * input via the IPC keystroke path. */
static void tty_signal(int sig) {
    (void)sig;        /* only SIGINT today; queue is binary kill_pending */
    uint64_t pid = shell_fg_pid();
    if (pid == 0) return;
    task_t *t = task_by_pid(pid);
    if (!t || !t->pml4) return;               /* must be a user task */
    t->kill_pending = 1;
    stat_signals++;
}

/* Deliver SIGTSTP to the foreground user task — Ctrl+Z. The task
 * transitions to TASK_STOPPED on its next dispatch (user_task_
 * trampoline checks the flag); resume via shell `fg`/`bg`. Send
 * IPC to the shell so it can drop fg_pid and print "[pid] stopped".
 *
 * Special case: if the task is currently TASK_BLOCKED (e.g. asleep
 * inside nanosleep) it will not be re-dispatched until the timer
 * wakes it, so the stop_pending flag would sit unchecked and
 * `jobs`/`fg` would see "blocked", not "stopped". Fold the stop
 * into the state directly. The saved_iret_* are populated by the
 * blocking syscall (nanosleep, etc.); when `fg` later flips state
 * to READY, user_task_trampoline replays them and the syscall
 * returns immediately — the sleep effectively gets interrupted
 * with a normal-looking 0 return, which matches user intuition
 * after a Ctrl+Z. */
static void tty_stop_signal(void) {
    uint64_t pid = shell_fg_pid();
    if (pid == 0) return;
    task_t *t = task_by_pid(pid);
    if (!t || !t->pml4) return;
    if (t->state == TASK_BLOCKED) {
        /* Apply the stop transition directly. The trampoline (which
         * normally consumes stop_pending and sets state=STOPPED) will
         * not run until something resumes us, so we have to do the
         * bookkeeping here. Leave stop_pending = 0 so the very first
         * dispatch after fg/bg resumes cleanly instead of immediately
         * re-stopping. */
        t->state = TASK_STOPPED;
    } else {
        /* Running/ready/etc. Leave the flag set; the trampoline picks
         * it up on the next dispatch and applies the transition + clear. */
        t->stop_pending = 1;
    }
    stat_signals++;

    ipc_msg_t msg;
    msg.from    = 0;
    msg.to      = SERVER_SHELL;
    msg.type    = IPC_PROC_STOPPED;
    msg.arg0    = 0;
    msg.arg1    = pid;
    msg.data[0] = 0;
    ipc_send(&msg);
}

/* ----- Public API ----- */

void tty_init(void) {
    tty_t.c_iflag = TTY_ICRNL;
    tty_t.c_oflag = 0;
    tty_t.c_cflag = 0;
    tty_t.c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE;
    tty_t.c_line  = 0;
    for (size_t i = 0; i < NCCS; i++) tty_t.c_cc[i] = 0;
    tty_t.c_cc[TTY_VINTR]  = 3;       /* ^C  */
    tty_t.c_cc[TTY_VQUIT]  = 28;      /* ^\  */
    tty_t.c_cc[TTY_VERASE] = 0x7f;    /* DEL (most terminals send 0x7f for BS) */
    tty_t.c_cc[TTY_VKILL]  = 21;      /* ^U  */
    tty_t.c_cc[TTY_VEOF]   = 4;       /* ^D  */
    tty_t.c_cc[TTY_VMIN]   = 1;
    tty_t.c_cc[TTY_VTIME]  = 0;
    tty_t.c_cc[TTY_VSTART] = 17;      /* ^Q  */
    tty_t.c_cc[TTY_VSTOP]  = 19;      /* ^S  */
    tty_t.c_cc[TTY_VSUSP]  = 26;      /* ^Z  */

    read_head = read_tail = read_count = 0;
    line_len = 0;
    stat_chars_in = stat_lines_out = stat_signals = 0;
}

void tty_input(char c) {
    stat_chars_in++;

    /* CR/NL translation per IFLAGS. */
    if ((tty_t.c_iflag & TTY_ICRNL) && c == '\r') c = '\n';
    else if ((tty_t.c_iflag & TTY_INLCR) && c == '\n') c = '\r';
    else if ((tty_t.c_iflag & TTY_IGNCR) && c == '\r') return;

    /* ISIG: turn certain control chars into signals before they ever
     * reach the line buffer. */
    if (tty_t.c_lflag & TTY_ISIG) {
        if (c != 0 && c == (char)tty_t.c_cc[TTY_VINTR]) {
            tty_signal(2 /* SIGINT */);
            return;
        }
        if (c != 0 && c == (char)tty_t.c_cc[TTY_VSUSP]) {
            tty_stop_signal();    /* Ctrl+Z → SIGTSTP */
            return;
        }
        /* VQUIT TODO when we have core-dump signals; for now drop. */
    }

    if (tty_t.c_lflag & TTY_ICANON) {
        /* Canonical mode — accumulate into line_buf and only flush at
         * '\n' or VEOF. Handles VERASE locally so apps see clean lines. */

        if (c != 0 && c == (char)tty_t.c_cc[TTY_VERASE]) {
            if (line_len > 0) {
                line_len--;
                tty_echo_erase();
            }
            return;
        }
        if (c == '\b') {
            /* '\b' (0x08) is what our PS/2 driver produces; treat as
             * VERASE regardless of the c_cc setting so the user can
             * always backspace. */
            if (line_len > 0) {
                line_len--;
                tty_echo_erase();
            }
            return;
        }
        if (c != 0 && c == (char)tty_t.c_cc[TTY_VKILL]) {
            /* Clear the whole line. ECHOK would normally print a '\n'
             * — keep it simple: just visually erase and reset. */
            while (line_len > 0) {
                line_len--;
                tty_echo_erase();
            }
            return;
        }

        /* Append, echo, and on '\n' flush to the read buffer. */
        if (line_len < TTY_LINE_BUF - 1) {
            line_buf[line_len++] = c;
        }
        tty_echo_char(c);

        if (c == '\n') {
            cli();
            for (size_t i = 0; i < line_len; i++) read_buf_push(line_buf[i]);
            sti();
            line_len = 0;
            stat_lines_out++;
        } else if (c != 0 && c == (char)tty_t.c_cc[TTY_VEOF]) {
            /* EOF: flush any pending bytes (without the EOF char) and
             * leave the read buffer ready. A read() that drains it
             * returns 0 next time, signalling end-of-file. */
            cli();
            /* The VEOF char was appended above — back it out. */
            if (line_len > 0 && line_buf[line_len - 1] == c) line_len--;
            for (size_t i = 0; i < line_len; i++) read_buf_push(line_buf[i]);
            sti();
            line_len = 0;
        }
    } else {
        /* Raw mode — push the char straight through. Echo if asked. */
        cli();
        read_buf_push(c);
        sti();
        tty_echo_char(c);
    }
}

size_t tty_read(char *buf, size_t max) {
    size_t n = 0;
    cli();
    while (n < max && read_count > 0) {
        buf[n++] = read_buf[read_tail];
        read_tail = (read_tail + 1) % TTY_READ_BUF;
        read_count--;
    }
    sti();
    return n;
}

bool tty_readable(void) { return read_count > 0; }

void tty_clear(void) {
    cli();
    read_head = read_tail = read_count = 0;
    line_len = 0;
    sti();
}

int tty_get_termios(struct osnos_termios *out) {
    if (!out) return -1;
    *out = tty_t;
    return 0;
}

int tty_set_termios(const struct osnos_termios *in) {
    if (!in) return -1;
    tty_t = *in;
    return 0;
}

uint64_t tty_chars_in   (void) { return stat_chars_in; }
uint64_t tty_lines_out  (void) { return stat_lines_out; }
uint64_t tty_signals_sent(void) { return stat_signals; }
