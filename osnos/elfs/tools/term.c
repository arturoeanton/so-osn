/*
 * term — mini terminal emulator running a child on a PTY.
 *
 * Showcase program for FASE 10.6 sesión 5. Combines every piece of
 * the POSIX subsystem osnos has accumulated:
 *
 *   - posix_openpt + ptsname (sesión 3 PTY layer)
 *   - fork + execve (ABI POSIX core)
 *   - dup2 with OFD shared offset (sesión 2)
 *   - tcgetattr + tcsetattr (raw mode on the controlling terminal)
 *   - select() multiplex stdin + master fd
 *   - waitpid + WIFEXITED (sesión 1+4 wait/job-control)
 *
 * Architecture:
 *
 *      user keyboard                  /bin/minishell (child)
 *           │                                ▲
 *           ▼                                │ reads fd 0 (slave)
 *      kernel TTY ring                       │
 *           │                                ▼
 *           ▼                          pty_pair_t
 *      term's fd 0 (raw mode)         m2s ring ⇄ s2m ring
 *           │                                │
 *           │ select(stdin + master)         │
 *           ▼                                ▼
 *        term (this program) ───── master fd
 *           │
 *           │ writes to fd 1
 *           ▼
 *      kernel TTY → consrv → /dev/fb0 (real terminal)
 *
 * Termination:
 *   - User types Ctrl+D (0x04) on term's stdin → term exits.
 *   - Child exits (e.g. minishell sees "exit") → master read 0 → exit.
 *   - Either path: tcsetattr restore + kill child + waitpid.
 *
 * Limitations:
 *   - Single-window only (no compositor / panes).
 *   - No SIGWINCH on resize (PTY layer doesn't track winsize).
 *   - Default child is /bin/minishell; pass argv[1..] to override.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
    /* Pick the child program: argv[1] override, else /bin/minishell. */
    const char *child_path = "/bin/minishell";
    char *default_argv[] = { "minishell", 0 };
    char **child_argv = default_argv;
    if (argc > 1) {
        child_path = argv[1];
        child_argv = &argv[1];
    }

    /* 1. Open the PTY master. */
    int m = posix_openpt(O_RDWR);
    if (m < 0) {
        fprintf(stderr, "term: posix_openpt failed errno=%d\n", errno);
        return 1;
    }
    if (unlockpt(m) < 0) {
        fprintf(stderr, "term: unlockpt failed errno=%d\n", errno);
        close(m);
        return 1;
    }

    char slave_name[32];
    if (ptsname_r(m, slave_name, sizeof(slave_name)) < 0) {
        fprintf(stderr, "term: ptsname_r failed errno=%d\n", errno);
        close(m);
        return 1;
    }

    /* 2. Fork the child. */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "term: fork failed errno=%d\n", errno);
        close(m);
        return 1;
    }
    if (pid == 0) {
        /* Child: redirect stdin/stdout/stderr to the slave. */
        int s = open(slave_name, O_RDWR);
        if (s < 0) {
            fprintf(stderr, "term-child: open %s failed errno=%d\n",
                    slave_name, errno);
            _exit(127);
        }
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2) close(s);
        close(m);
        /* Become a new session leader so the slave becomes the
         * child's controlling terminal (POSIX). On osnos this
         * mostly just resets pgid/sid; the PTY layer doesn't
         * implement TIOCSCTTY yet so the effect is cosmetic, but
         * keeps the API forward-compatible. */
        setsid();
        execve(child_path, child_argv, environ);
        fprintf(stderr, "term-child: execve %s failed errno=%d\n",
                child_path, errno);
        _exit(127);
    }

    /* 3. Parent: switch own stdin to RAW mode so keystrokes pass
     *    through as single bytes (no canon line-buffer, no echo,
     *    no Ctrl+C → kernel TTY signal). We feed the bytes to the
     *    master fd; the PTY's own termios applies whatever line
     *    discipline the child wants. */
    struct termios orig_t, raw_t;
    int had_tty = (tcgetattr(0, &orig_t) == 0);
    if (had_tty) {
        raw_t = orig_t;
        raw_t.c_lflag &= ~(ICANON | ECHO | ISIG);
        tcsetattr(0, TCSANOW, &raw_t);
    }

    printf("term: started %s (pid %d) on %s — Ctrl+D to exit\n",
           child_path, (int)pid, slave_name);

    /* 4. Relay loop. Stop on Ctrl+D from stdin OR EOF on master
     *    (child exited and drained s2m). */
    int running = 1;
    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(0, &rfds);
        FD_SET(m, &rfds);
        int maxfd = (m > 0) ? m : 0;

        int nready = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (nready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(0, &rfds)) {
            char buf[128];
            int n = (int)read(0, buf, sizeof(buf));
            if (n <= 0) break;
            /* Detect Ctrl+D (0x04) — terminate cleanly. */
            int has_eot = 0;
            for (int i = 0; i < n; i++) {
                if (buf[i] == 0x04) { has_eot = 1; n = i; break; }
            }
            if (n > 0) {
                if (write(m, buf, n) < 0 && errno == EPIPE) {
                    running = 0;
                }
            }
            if (has_eot) running = 0;
        }
        if (running && FD_ISSET(m, &rfds)) {
            char buf[256];
            int n = (int)read(m, buf, sizeof(buf));
            if (n <= 0) {        /* child closed slave or master EOF */
                running = 0;
                break;
            }
            write(1, buf, n);
        }
    }

    /* 5. Cleanup: restore terminal, kill+reap child, close master. */
    if (had_tty) tcsetattr(0, TCSANOW, &orig_t);

    kill(pid, 15 /* SIGTERM */);
    int status = 0;
    waitpid(pid, &status, 0);
    close(m);

    if (WIFEXITED(status)) {
        printf("\nterm: child exited with code %d\n",
               WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("\nterm: child killed by signal %d\n",
               WTERMSIG(status));
    } else {
        printf("\nterm: child gone (raw status=0x%x)\n", status);
    }
    return 0;
}
