#include "tty.h"

#include "../drivers/framebuffer.h"
#include "syscall.h"
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
 * Echo helpers. With shellsrv (FASE 10.4 chunk 5) the shell itself
 * runs in raw mode so TTY_ECHO is off whenever it's fg — meaning
 * the early-return on `!ECHO` handles the "don't double-echo" case
 * naturally. Children in canonical mode keep the bit set and TTY
 * echoes for them.
 */
static void tty_echo_char(char c) {
    if (!(tty_t.c_lflag & TTY_ECHO)) return;
    char s[2] = { c, 0 };
    framebuffer_draw_string(s, 0xffffff);
}

static void tty_echo_erase(void) {
    if (!(tty_t.c_lflag & TTY_ECHOE)) return;
    framebuffer_backspace();
}

/* Deliver SIGINT to the current foreground user task.
 *
 * Routing: the ring-3 shell publishes its current fg child via
 * sys_set_fg → kernel_fg_pid (defined in syscall.c). The shell
 * itself never sets kernel_fg_pid to its own pid, so Ctrl+C never
 * kills the shell — only its running child. */
extern uint64_t kernel_fg_pid;

/* Deliver `sig` to one task. Helper used by tty_signal and
 * tty_stop_signal so the broadcast loops below stay simple. */
static void tty_deliver_one(task_t *t, int sig) {
    if (!t || !t->pml4) return;
    t->sig_pending |= 1u << (sig - 1);
    if (sig == 2 /* SIGINT */ || sig == 15 /* SIGTERM */) {
        t->kill_pending = 1;
    }
    if (t->state == TASK_BLOCKED) {
        if (t->saved_valid) {
            t->saved_rax = (uint64_t)(int64_t)-(int64_t)4 /* OSNOS_EINTR */;
        }
        t->wakeup_at_ms = 0;
        t->state        = TASK_READY;
    }
}

static void tty_signal(int sig) {
    uint64_t fg_pid = kernel_fg_pid;
    if (fg_pid == 0) return;
    task_t *fg = task_by_pid(fg_pid);
    if (!fg || !fg->pml4) return;
    if (sig < 1 || sig > 31) return;

    /* POSIX: Ctrl+C / Ctrl+\ / etc go to the ENTIRE foreground
     * process group, not just the lead pid. Walk the task table
     * and deliver to every task whose pgid matches the fg pid's
     * pgid. If no setpgid has happened, pgid == pid trivially and
     * only the one fg task is hit. With shellsrv setting up a
     * pipeline, all members of the pgid die together (true
     * job-control semantics). */
    uint64_t fg_pgid = fg->pgid;
    int delivered = 0;
    for (size_t i = 0; i < MAX_TASKS; i++) {
        task_t *u = (task_t *)task_slot(i);
        if (!u) continue;
        if (u->state == TASK_UNUSED || !u->pml4) continue;
        if (u->pgid != fg_pgid) continue;
        tty_deliver_one(u, sig);
        delivered++;
    }
    if (delivered == 0) {
        /* Fallback to single-pid delivery if pgid lookup yielded
         * nothing (defensive — shouldn't normally happen). */
        tty_deliver_one(fg, sig);
    }
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
/* Stop one task — helper for the fan-out below. */
static void tty_stop_one(task_t *t) {
    if (!t || !t->pml4) return;
    if (t->state == TASK_BLOCKED) {
        t->state       = TASK_STOPPED;
        t->wait_change = 1 /* WAIT_STOPPED */;
        notify_parent_stop_continue(t);
    } else if (t->state == TASK_READY || t->state == TASK_RUNNING) {
        t->stop_pending = 1;
    }
    /* Already STOPPED / ZOMBIE / DEAD: nothing to do. */
}

static void tty_stop_signal(void) {
    uint64_t fg_pid = kernel_fg_pid;
    if (fg_pid == 0) return;
    task_t *fg = task_by_pid(fg_pid);
    if (!fg || !fg->pml4) return;

    /* Broadcast Ctrl+Z to every task in the foreground process
     * group (same logic as tty_signal for Ctrl+C). With a single
     * fg task, pgid == pid so only it is stopped. With a pipeline
     * in the same pgid, all stages stop together — POSIX-correct
     * job control. */
    uint64_t fg_pgid = fg->pgid;
    int stopped = 0;
    for (size_t i = 0; i < MAX_TASKS; i++) {
        task_t *u = (task_t *)task_slot(i);
        if (!u) continue;
        if (u->state == TASK_UNUSED || !u->pml4) continue;
        if (u->pgid != fg_pgid) continue;
        tty_stop_one(u);
        stopped++;
    }
    if (stopped == 0) tty_stop_one(fg);   /* defensive fallback */
    stat_signals++;

    ipc_msg_t msg;
    msg.from    = 0;
    msg.to      = SERVER_SHELL;
    msg.type    = IPC_PROC_STOPPED;
    msg.arg0    = 0;
    msg.arg1    = fg_pid;
    msg.data[0] = 0;
    ipc_send(&msg);
}

/* ----- Public API ----- */

/* Reset SOLO los flags de termios a los defaults POSIX (canonical
 * mode + ECHO + ECHOE), sin tocar el ring buffer ni `line_*`.
 * Llamado por proc_execve al spawn de cada child: mimic the
 * Linux/PTY boundary donde cada exec hereda un terminal "limpio"
 * con echo on + canonical. Sin esto, ash sets raw+noecho para su
 * line editor, hace fork+exec, y el child (sqlite3 / lua REPL /
 * etc.) hereda raw+noecho — usuario tipea y no ve nada. Real
 * Linux no tiene este problema porque cada PTY tiene su propio
 * termios; en osnos tenemos UN solo TTY global. El reset al
 * execve es el workaround pragmático. Programs que necesitan raw
 * (vi, less, ash mismo) van a llamar tcsetattr explícitamente. */
void tty_reset_to_defaults(void) {
    tty_t.c_iflag = TTY_ICRNL;
    tty_t.c_oflag = 0;
    tty_t.c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE;
}

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
    /* Anti-clobber: si la tarea que está intentando deshabilitar ECHO
     * tiene children vivos, ignoramos el cambio. Caso típico: ash
     * (parent) hace `tcsetattr(ECHO=off)` PRE-emptivamente después
     * de fork para tener raw mode listo para su próximo prompt,
     * pero como osnos tiene un solo TTY global compartido, ese
     * cambio mata el echo del child (sqlite3, lua REPL, etc.).
     * Real Linux maneja esto via SIGTTOU + controlling-pgid checks
     * que no tenemos implementados; esta heurística es el approximate
     * práctico — sin children vivos, ash puede setear lo que quiera.
     * Cuando los children terminan, ash hace su tcsetattr propio
     * exitoso en la siguiente lectura. */
    if (!(in->c_lflag & TTY_ECHO)) {
        task_t *self = task_current();
        if (self && self->pml4) {
            extern const task_t *task_slot(size_t idx);
            for (size_t i = 0; i < 16; i++) {
                const task_t *child = task_slot(i);
                if (!child) continue;
                if (child->parent_pid != self->pid) continue;
                if (child->state == TASK_UNUSED ||
                    child->state == TASK_DEAD ||
                    child->state == TASK_ZOMBIE) continue;
                /* Hay un child activo. Ignoramos este intento de
                 * disable de ECHO — el child necesita echo on. */
                return 0;
            }
        }
    }
    tty_t = *in;
    return 0;
}

uint64_t tty_chars_in   (void) { return stat_chars_in; }
uint64_t tty_lines_out  (void) { return stat_lines_out; }
uint64_t tty_signals_sent(void) { return stat_signals; }
