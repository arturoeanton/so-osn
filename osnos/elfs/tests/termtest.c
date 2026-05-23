/*
 * termtest — automated end-to-end PTY pipeline test.
 *
 * Validates the FASE 10.6 showcase path without needing interactive
 * input. Spawns /bin/minishell on a PTY, writes commands to the
 * master, reads responses, then sends "exit" and reaps.
 *
 * Pipeline exercised:
 *   posix_openpt → ptmx OFD with is_pty + pty_side=0
 *   ptsname_r    → TIOCGPTN ioctl + sprintf "/dev/pts/<N>"
 *   fork         → child inherits OFDs (refcount bumps)
 *   open slave   → /dev/pts/N → slave OFD with pty_side=1
 *   dup2 0/1/2   → child std streams point at slave OFD
 *   execve       → child image becomes /bin/minishell with std fds
 *                  on the PTY slave
 *   read/write   → master ⇄ s2m_buf / m2s_buf rings via canonical
 *                  termios on the slave
 *   waitpid      → child exit reaped, ZOMBIE → DEAD transition
 *
 * Coverage:
 *   1. ptmx open + ptsname.
 *   2. fork + child execve minishell.
 *   3. Read the banner ("minishell: type 'exit' to quit").
 *   4. Read the first prompt ("mini$ ").
 *   5. Write "hola\n" → expect "you said: hola" in master read.
 *   6. Write "exit\n" → minishell prints "bye" and exits.
 *   7. waitpid → child exit 0.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int total = 0;
static int fails = 0;
#define CHECK(c,n) do { total++; if (c) printf("PASS %s\n", n); else { printf("FAIL %s\n", n); fails++; } } while (0)

/* Read up to `cap-1` bytes from `fd`, looping until either a
 * sentinel substring shows up OR we exhaust attempts. The PTY layer
 * returns -EAGAIN when its ring is empty; libc loops on it, but a
 * canonical-mode slave doesn't push to its ring until '\n' arrives,
 * so we need our own poll-with-timeout for the master side too. */
static int read_until(int fd, const char *needle,
                      char *out, int cap, int attempts) {
    int total_len = 0;
    out[0] = 0;
    for (int i = 0; i < attempts; i++) {
        char buf[128];
        int n = (int)read(fd, buf, sizeof(buf));
        if (n > 0) {
            int room = cap - 1 - total_len;
            if (n > room) n = room;
            if (n > 0) {
                memcpy(out + total_len, buf, n);
                total_len += n;
                out[total_len] = 0;
            }
            if (needle && strstr(out, needle)) return total_len;
        } else {
            /* EAGAIN / EOF — sleep a tick and retry. */
            struct timespec ts = { 0, 10 * 1000000 };
            nanosleep(&ts, 0);
        }
    }
    return total_len;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("termtest: PTY + fork + execve + minishell pipeline\n");

    /* 1. Open master. */
    int m = posix_openpt(O_RDWR);
    CHECK(m >= 3, "1.posix_openpt master fd");

    int r = unlockpt(m);
    CHECK(r == 0, "1.unlockpt");

    char slave_name[32];
    r = ptsname_r(m, slave_name, sizeof(slave_name));
    CHECK(r == 0, "1.ptsname_r");
    CHECK(strncmp(slave_name, "/dev/pts/", 9) == 0,
          "1.slave name /dev/pts/N format");

    /* 2. Fork + execve minishell on the slave. */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "termtest: fork failed errno=%d\n", errno);
        return 1;
    }
    if (pid == 0) {
        int s = open(slave_name, O_RDWR);
        if (s < 0) _exit(125);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2) close(s);
        close(m);
        char *cargv[] = { "minishell", 0 };
        execve("/bin/minishell", cargv, environ);
        _exit(127);
    }
    CHECK(pid > 0, "2.fork returned child pid in parent");

    /* 3+4. Read minishell's banner + first prompt. */
    char buf[512];
    int n = read_until(m, "mini$ ", buf, sizeof(buf), 30);
    CHECK(n > 0,                            "3.read banner+prompt");
    CHECK(strstr(buf, "minishell:") != 0,   "3.banner contains 'minishell:'");
    CHECK(strstr(buf, "mini$ ")    != 0,    "4.prompt 'mini$ ' visible");

    /* 5. Send "hola\n" — the slave's canonical termios commits the
     *    line at the newline; minishell reads, formats "you said:
     *    hola\n", writes to slave; PTY relays to s2m → we read it
     *    here on the master. */
    const char *line = "hola\n";
    int w = (int)write(m, line, 5);
    CHECK(w == 5, "5.write 'hola\\n' to master");

    n = read_until(m, "you said: hola", buf, sizeof(buf), 30);
    CHECK(n > 0 && strstr(buf, "you said: hola") != 0,
          "5.master sees 'you said: hola' response");

    /* 6. Send "exit\n" — minishell prints "bye\n" and returns 0. */
    w = (int)write(m, "exit\n", 5);
    CHECK(w == 5, "6.write 'exit\\n' to master");

    /* Drain remaining output (mostly "bye"). */
    n = read_until(m, "bye", buf, sizeof(buf), 30);
    CHECK(strstr(buf, "bye") != 0, "6.minishell prints 'bye' before exit");

    /* 7. Reap. */
    int status = 0;
    pid_t r2 = waitpid(pid, &status, 0);
    CHECK(r2 == pid,                        "7.waitpid returns child pid");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                                            "7.child exit 0");

    close(m);

    printf("\ntermtest: total=%d pass=%d fail=%d\n",
           total, total - fails, fails);
    return fails == 0 ? 0 : 1;
}
