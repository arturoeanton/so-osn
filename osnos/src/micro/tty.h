#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Minimal kernel TTY line discipline (FASE TTY 1+2).
 *
 * Sits between the keyboard server (which delivers raw scancodes
 * decoded into char's) and sys_read(0) (which user tasks call to
 * consume bytes). Implements POSIX canonical + raw modes via a
 * termios struct that matches the Linux x86_64 kernel ABI.
 *
 * Flow:
 *
 *   PS/2 -> keyboard_server_tick -> tty_input(c)
 *                                     |
 *                                     v
 *                              [apply termios]
 *                              - ICANON: line buffer, flush on '\n'
 *                              - !ICANON (raw): byte through to rd buf
 *                              - ISIG + VINTR: SIGINT to fg user task
 *                              - VERASE / ECHOE: handle backspace
 *                              - ECHO: TODO (today no-op; shell does
 *                                its own echo in IPC_KEY_EVENT path)
 *                                     |
 *                                     v
 *                            +------------------+
 *                            |  rd buf (256 B)  |
 *                            +------------------+
 *                                     |
 *                                     v
 *                              sys_read(0, ...)
 *
 * Default mode at boot: ICANON | ECHO | ECHOE | ISIG, ICRNL on input.
 * That matches a stock Linux terminal so user ELFs reading getline()
 * get sane lines without configuring anything.
 */

/* ---- Linux termios ABI (asm-generic/termbits.h) ---- */

#define NCCS 19

typedef uint32_t tcflag_t;
typedef uint8_t  cc_t;
typedef uint32_t speed_t;

struct osnos_termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
};

/* c_iflag bits */
#define TTY_INLCR  0x0040
#define TTY_IGNCR  0x0080
#define TTY_ICRNL  0x0100

/* c_lflag bits */
#define TTY_ISIG    0x0001
#define TTY_ICANON  0x0002
#define TTY_ECHO    0x0008
#define TTY_ECHOE   0x0010
#define TTY_ECHOK   0x0020
#define TTY_ECHONL  0x0040
#define TTY_NOFLSH  0x0080

/* c_cc indices */
#define TTY_VINTR   0
#define TTY_VQUIT   1
#define TTY_VERASE  2
#define TTY_VKILL   3
#define TTY_VEOF    4
#define TTY_VTIME   5
#define TTY_VMIN    6
#define TTY_VSTART  8
#define TTY_VSTOP   9

/* ioctl request numbers (asm-generic/ioctls.h) */
#define TTY_TCGETS    0x5401u
#define TTY_TCSETS    0x5402u
#define TTY_TCSETSW   0x5403u
#define TTY_TCSETSF   0x5404u
#define TTY_TIOCGWINSZ 0x5413u

/* struct winsize — Linux layout. Used by TIOCGWINSZ to expose
 * terminal dimensions to TUI programs (e.g. /bin/ovi). */
struct osnos_winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;   /* unused, kept for ABI shape */
    unsigned short ws_ypixel;
};

/* ---- Public API ---- */

void   tty_init(void);

/* Called from keyboard_server with one decoded ASCII char per
 * keystroke. Applies the current termios; may consume the char
 * silently (backspace inside canonical line), enqueue it for read
 * (raw mode, or canonical on '\n'), or deliver SIGINT to the fg
 * user task (Ctrl+C under ISIG). */
void   tty_input(char c);

/* Drain up to `max` bytes from the read buffer into `buf`. Returns
 * the byte count (0 if empty). Non-blocking. */
size_t tty_read(char *buf, size_t max);

/* True when at least one byte sits in the read buffer (i.e. a
 * canonical line was completed, or a raw byte arrived, or EOF was
 * signalled). Drives sock-style select() readiness for stdin. */
bool   tty_readable(void);

/* Discard everything: line buffer + read buffer. Called by
 * proc_exec so a freshly exec'd task starts with empty stdin. */
void   tty_clear(void);

/* ioctl TCGETS / TCSETS. Returns 0 on success, -1 on bad fd. */
int    tty_get_termios(struct osnos_termios *out);
int    tty_set_termios(const struct osnos_termios *in);

/* Diagnostic counters (visible in /sys/meminfo). */
uint64_t tty_chars_in(void);
uint64_t tty_lines_out(void);
uint64_t tty_signals_sent(void);
