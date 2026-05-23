/*
 * tests/libctest.c — smoke test for the Tier 1 libc additions.
 *
 * Covers:
 *   - FILE * round-trip via fopen / fwrite / fclose / fopen / fread
 *   - fgets / fputs / fflush / ftell / rewind
 *   - string extras: strdup, strstr, strtok_r, strerror, strcasecmp
 *   - stdlib: qsort + bsearch, strtoul/strtoll, abs/labs
 *   - setjmp / longjmp
 *   - byte-swap: htons / htonl
 *   - inet_pton / inet_ntop / inet_aton / inet_ntoa
 *   - socket() stub returns -1 + errno=ENOSYS
 *
 * Each check prints PASS/FAIL with a label. Returns 0 if all pass,
 * 1 if any fails. No external setup needed — uses /tmp via ramfs.
 */

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int pass_count;
static int fail_count;

static void check(const char *label, int cond) {
    if (cond) {
        printf("  PASS %s\n", label);
        pass_count++;
    } else {
        printf("  FAIL %s\n", label);
        fail_count++;
    }
}

static int cmp_int(const void *a, const void *b) {
    int ai = *(const int *)a;
    int bi = *(const int *)b;
    return (ai > bi) - (ai < bi);
}

static jmp_buf jb;
static void  do_longjmp(void) { longjmp(jb, 42); }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("== libc Tier 1 smoke test ==\n");

    /* ---------------- FILE * round-trip ---------------- */
    {
        const char *path = "/tmp/libctest.txt";
        mkdir("/tmp", 0755);    /* may already exist */
        FILE *f = fopen(path, "w");
        check("fopen-w", f != NULL);
        if (f) {
            size_t n = fwrite("hello\nworld\n", 1, 12, f);
            check("fwrite", n == 12);
            check("ftell-after-write", ftell(f) == 12);
            check("fclose-w", fclose(f) == 0);
        }

        f = fopen(path, "r");
        check("fopen-r", f != NULL);
        if (f) {
            char line[32];
            char *r = fgets(line, sizeof(line), f);
            check("fgets-1", r == line && strcmp(line, "hello\n") == 0);
            r = fgets(line, sizeof(line), f);
            check("fgets-2", r == line && strcmp(line, "world\n") == 0);
            r = fgets(line, sizeof(line), f);
            check("fgets-eof", r == NULL && feof(f));
            rewind(f);
            int c = fgetc(f);
            check("rewind+fgetc", c == 'h');
            ungetc(c, f);
            c = fgetc(f);
            check("ungetc", c == 'h');
            fclose(f);
        }
    }

    /* ---------------- string extras ---------------- */
    {
        char *d = strdup("foo");
        check("strdup", d && strcmp(d, "foo") == 0);
        free(d);

        char buf[] = "  hello world  ";
        char *p = strstr(buf, "world");
        check("strstr-found", p && strcmp(p, "world  ") == 0);
        check("strstr-missing", strstr(buf, "xyz") == NULL);

        char tk[] = "a,b,,c";
        char *sp = 0;
        char *t1 = strtok_r(tk, ",", &sp);
        char *t2 = strtok_r(NULL, ",", &sp);
        char *t3 = strtok_r(NULL, ",", &sp);
        char *t4 = strtok_r(NULL, ",", &sp);
        check("strtok_r-1", t1 && strcmp(t1, "a") == 0);
        check("strtok_r-2", t2 && strcmp(t2, "b") == 0);
        check("strtok_r-3", t3 && strcmp(t3, "c") == 0);
        check("strtok_r-end", t4 == NULL);

        check("strcasecmp", strcasecmp("HeLLo", "hello") == 0);
        check("strncasecmp", strncasecmp("abcdef", "ABCxxx", 3) == 0);
        check("strnlen", strnlen("hi", 100) == 2 && strnlen("hi", 1) == 1);

        const char *err = strerror(ENOENT);
        check("strerror-known", err && strstr(err, "No such") != NULL);
    }

    /* ---------------- stdlib extras ---------------- */
    {
        int arr[] = { 5, 2, 8, 1, 9, 3, 7, 4, 6, 0 };
        qsort(arr, 10, sizeof(int), cmp_int);
        int sorted_ok = 1;
        for (int i = 0; i < 10; i++) if (arr[i] != i) sorted_ok = 0;
        check("qsort", sorted_ok);

        int key = 7;
        int *hit = bsearch(&key, arr, 10, sizeof(int), cmp_int);
        check("bsearch-found", hit && *hit == 7);
        int miss = 99;
        check("bsearch-miss", bsearch(&miss, arr, 10, sizeof(int), cmp_int) == NULL);

        check("strtoul-hex", strtoul("0xff", 0, 0) == 255);
        check("strtoll-neg", strtoll("-42", 0, 10) == -42);
        check("abs", abs(-7) == 7);
        check("labs", labs(-1234L) == 1234L);
    }

    /* ---------------- setjmp / longjmp ---------------- */
    {
        int v = setjmp(jb);
        if (v == 0) {
            do_longjmp();
            check("longjmp-unreached", 0);
        } else {
            check("setjmp/longjmp", v == 42);
        }
    }

    /* ---------------- byte-swap + inet ---------------- */
    {
        check("htons", htons(0x1234) == 0x3412);
        check("htonl", htonl(0x11223344) == 0x44332211);
        check("ntohs-roundtrip", ntohs(htons(0xabcd)) == 0xabcd);
        check("htobe64",
              htobe64(0x0123456789abcdefULL) == 0xefcdab8967452301ULL);

        struct in_addr a;
        int r = inet_aton("192.168.1.1", &a);
        check("inet_aton", r == 1);
        /* Expected: 192.168.1.1 in network order = 0x0101a8c0 */
        check("inet_aton-value", a.s_addr == htonl(0xc0a80101));

        check("inet_aton-bad", inet_aton("999.0.0.0", &a) == 0);
        check("inet_aton-junk", inet_aton("1.2.3.4.5", &a) == 0);

        char buf[INET_ADDRSTRLEN];
        struct in_addr lo = { .s_addr = htonl(0x7f000001) };
        const char *s = inet_ntop(AF_INET, &lo, buf, sizeof(buf));
        check("inet_ntop-loopback",
              s && strcmp(s, "127.0.0.1") == 0);

        struct in_addr parsed;
        int p = inet_pton(AF_INET, "10.0.0.5", &parsed);
        check("inet_pton-ok", p == 1);
        check("inet_pton-value", parsed.s_addr == htonl(0x0a000005));

        int p6 = inet_pton(AF_INET6, "::1", &parsed);
        check("inet_pton-ipv6-eafnosupport",
              p6 == -1 && errno == EAFNOSUPPORT);
    }

    /* ---------------- socket stub ---------------- */
    {
        errno = 0;
        int s = socket(AF_INET, SOCK_STREAM, 0);
        check("socket-ok", s >= 0);    /* FASE 8.5 onwards: real */
        if (s >= 0) close(s);
    }

    /* ---------------- strerror ---------------- */
    {
        check("strerror-EPERM",
              strcmp(strerror(EPERM), "Operation not permitted") == 0);
        check("strerror-ENOENT",
              strcmp(strerror(ENOENT), "No such file or directory") == 0);
        check("strerror-EBADF",
              strcmp(strerror(EBADF), "Bad file descriptor") == 0);
        check("strerror-EINVAL",
              strcmp(strerror(EINVAL), "Invalid argument") == 0);
        check("strerror-ENOSYS",
              strcmp(strerror(ENOSYS), "Function not implemented") == 0);
        check("strerror-ENOTTY",
              strcmp(strerror(ENOTTY), "Not a typewriter") == 0);
        check("strerror-ERANGE",
              strcmp(strerror(ERANGE), "Numerical result out of range") == 0);
        check("strerror-ETIMEDOUT",
              strcmp(strerror(ETIMEDOUT), "Connection timed out") == 0);
        check("strerror-ECONNREFUSED",
              strcmp(strerror(ECONNREFUSED), "Connection refused") == 0);
        check("strerror-EAGAIN",
              strcmp(strerror(EAGAIN), "Resource temporarily unavailable") == 0);
        /* Unknown errno falls back to "errno=N" itoa path. */
        const char *uk = strerror(9999);
        check("strerror-unknown-itoa",
              strstr(uk, "9999") != 0);
    }

    /* ---------------- stat / access ---------------- */
    {
        const char *path = "/tmp/libctest-stat.txt";
        FILE *fp = fopen(path, "w");
        if (fp) { fwrite("hi", 1, 2, fp); fclose(fp); }

        struct stat st;
        errno = 0;
        check("stat-ok",       stat(path, &st) == 0);
        check("stat-size==2",  st.st_size == 2);
        check("stat-isreg",    S_ISREG(st.st_mode));

        errno = 0;
        check("stat-enoent",
              stat("/nope/nope/nope", &st) == -1 && errno == ENOENT);

        errno = 0;
        check("access-ok",     access(path, F_OK) == 0);
        errno = 0;
        check("access-enoent",
              access("/nope/nope/nope", F_OK) == -1 && errno == ENOENT);

        unlink(path);
    }

    /* ---------------- time / clock_gettime ---------------- */
    {
        time_t t1 = time(0);
        check("time-positive", t1 >= 0);
        time_t t2 = 0;
        time_t r = time(&t2);
        check("time-out-param-matches", r == t2);

        struct timespec ts;
        check("clock_gettime-REALTIME",
              clock_gettime(CLOCK_REALTIME, &ts) == 0);
        check("clock_gettime-MONOTONIC",
              clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
        check("clock_gettime-ts-sane",
              ts.tv_sec >= 0 && ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000);

        errno = 0;
        check("clock_gettime-bad-id",
              clock_gettime(99, &ts) == -1 && errno == EINVAL);
    }

    /* ---------------- dup / dup2 / fcntl ---------------- */
    {
        const char *path = "/tmp/libctest-dup.txt";
        FILE *fp = fopen(path, "w");
        if (fp) { fwrite("abc", 1, 3, fp); fclose(fp); }

        int fd = open(path, O_RDONLY);
        check("dup-base-open",  fd >= 3);
        int dfd = dup(fd);
        check("dup-fresh-fd",   dfd >= 3 && dfd != fd);

        /* Both fds can read; dup'd one starts from offset 0. */
        char a[4] = {0}, b[4] = {0};
        check("dup-read-base",  read(fd, a, 3) == 3);
        check("dup-read-clone", read(dfd, b, 3) == 3);
        check("dup-same-bytes", a[0]=='a' && b[0]=='a');

        int target = 15;
        check("dup2-to-target", dup2(fd, target) == target);
        check("dup2-self-noop", dup2(fd, fd)   == fd);

        errno = 0;
        check("dup-bad-fd",
              dup(9999) == -1 && errno == EBADF);

        int fl = fcntl(fd, F_GETFL, 0);
        check("fcntl-getfl",   fl >= 0);
        check("fcntl-setfl",   fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
        check("fcntl-roundtrip",
              (fcntl(fd, F_GETFL, 0) & O_NONBLOCK) != 0);
        check("fcntl-getfd-zero", fcntl(fd, F_GETFD, 0) == 0);

        errno = 0;
        check("fcntl-bogus-cmd",
              fcntl(fd, 99, 0) == -1 && errno == EINVAL);

        close(dfd);
        close(target);
        close(fd);
        unlink(path);
    }

    /* ---------------- mkstemp / tmpfile ---------------- */
    {
        char tmpl[] = "/tmp/lctXXXXXX";
        int fd = mkstemp(tmpl);
        check("mkstemp-fd",      fd >= 3);
        /* "/tmp/lct" = 8 chars; XXXXXX sits at indices 8..13. */
        check("mkstemp-rewrote",
              tmpl[8] != 'X' && tmpl[13] != 'X');
        /* Verify the file exists. */
        struct stat st;
        check("mkstemp-stat",    stat(tmpl, &st) == 0);
        close(fd);
        unlink(tmpl);

        /* Bad template */
        errno = 0;
        char bad[] = "/tmp/no-x";
        check("mkstemp-einval",
              mkstemp(bad) == -1 && errno == EINVAL);

        /* tmpfile */
        FILE *tf = tmpfile();
        check("tmpfile-non-null", tf != NULL);
        if (tf) {
            check("tmpfile-write", fwrite("xx", 1, 2, tf) == 2);
            fclose(tf);
        }
    }

    /* ---------------- ctype ---------------- */
    {
        check("ctype-isalpha-a", isalpha('a'));
        check("ctype-isalpha-9", !isalpha('9'));
        check("ctype-isdigit-5", isdigit('5'));
        check("ctype-isspace-tab", isspace('\t') && isspace(' '));
        check("ctype-isblank-tab", isblank('\t') && !isblank('\n'));
        check("ctype-ispunct-comma", ispunct(',') && !ispunct('a'));
        check("ctype-isgraph-A", isgraph('A') && !isgraph(' '));
        check("ctype-tolower", tolower('Z') == 'z');
        check("ctype-toupper", toupper('z') == 'Z');
    }

    /* ---------------- limits ---------------- */
    {
        check("limits-INT_MAX",  INT_MAX  == 2147483647);
        check("limits-CHAR_BIT", CHAR_BIT == 8);
        check("limits-PATH_MAX", PATH_MAX == 128);
        check("limits-LONG_MAX", LONG_MAX == 9223372036854775807L);
    }

    /* ---------------- signal (real after FASE wait+sigaction) ---------------- */
    {
        check("signal-sig-numbers", SIGINT == 2 && SIGTERM == 15);
        /* signal() now wraps sigaction(). Install SIG_DFL, retrieve
         * previous (also SIG_DFL since we never set anything else),
         * verify round-trip. */
        errno = 0;
        sighandler_t prev = signal(SIGUSR1, SIG_DFL);
        check("signal-default-roundtrip", prev == SIG_DFL);

        /* Install an arbitrary user pointer. The kernel doesn't
         * dereference it until a signal actually fires, so the
         * call just stashes it in t->sa_handler[]. Returns the
         * previous handler (SIG_DFL). */
        errno = 0;
        prev = signal(SIGUSR1, (sighandler_t)0x12345);
        check("signal-install-returns-prev", prev == SIG_DFL);

        /* Restore + verify we get back the 0x12345 we installed. */
        errno = 0;
        prev = signal(SIGUSR1, SIG_DFL);
        check("signal-restore-returns-installed",
              prev == (sighandler_t)0x12345);
    }

    /* ---------------- math (requires FPU init from kernel) ---------------- */
    {
        /* If FPU isn't initialised, any double op will #UD and the
         * task dies before we reach the FAIL print. So getting here
         * with sensible values already proves FPU works. */
        check("math-fabs",   fabs(-2.5) == 2.5);
        check("math-floor",  floor(3.7) == 3.0);
        check("math-ceil",   ceil(3.2)  == 4.0);
        check("math-floor-neg", floor(-1.5) == -2.0);
        check("math-fmod",   fmod(7.0, 3.0) == 1.0);

        double r = sqrt(16.0);
        check("math-sqrt-16", r > 3.99 && r < 4.01);
        r = sqrt(2.0);
        check("math-sqrt-2", r > 1.41 && r < 1.42);

        r = pow(2.0, 10.0);
        check("math-pow-2-10", r > 1023.5 && r < 1024.5);

        /* sin(0)=0, cos(0)=1, sin(pi/2)≈1. */
        check("math-sin-0", fabs(sin(0.0)) < 1e-9);
        check("math-cos-0", fabs(cos(0.0) - 1.0) < 1e-9);
        check("math-sin-pi2", fabs(sin(M_PI_2) - 1.0) < 1e-3);

        /* exp(0)=1, exp(1)≈e. */
        check("math-exp-0", fabs(exp(0.0) - 1.0) < 1e-9);
        check("math-exp-1", fabs(exp(1.0) - M_E) < 1e-3);

        /* log(e)=1, log(1)=0. */
        check("math-log-1", fabs(log(1.0)) < 1e-9);
        check("math-log-e", fabs(log(M_E) - 1.0) < 1e-3);

        check("math-isnan", isnan(NAN));
        check("math-isinf", isinf(INFINITY));
        check("math-isfinite", isfinite(1.0) && !isfinite(INFINITY));
    }

    /* ---------------- write con offset ---------------- */
    {
        const char *p = "/tmp/seek-test.txt";
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        check("seek-open",  fd >= 3);
        ssize_t w = write(fd, "AAAAAAAAAA", 10);
        check("seek-init-write", w == 10);
        check("seek-lseek-3", lseek(fd, 3, SEEK_SET) == 3);
        w = write(fd, "BB", 2);
        check("seek-mid-write", w == 2);
        close(fd);

        fd = open(p, O_RDONLY);
        char buf[16] = {0};
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        check("seek-read-len-10", r == 10);
        check("seek-content-AAABBAAAAA",
              strcmp(buf, "AAABBAAAAA") == 0);
        close(fd);

        unlink(p);
    }

    /* ---------------- big stack (64 KiB) ---------------- */
    {
        /* If the user stack was still 4 KiB, this volatile array
         * would overflow into unmapped memory and the task would
         * die before we print PASS. */
        volatile char big[16 * 1024];
        for (size_t i = 0; i < sizeof(big); i += 4096) big[i] = (char)i;
        check("stack-16k", big[0] == 0 && big[12288] == 0);
        volatile char bigger[32 * 1024];
        bigger[0] = 7;
        bigger[sizeof(bigger) - 1] = 9;
        check("stack-32k", bigger[0] == 7 && bigger[sizeof(bigger)-1] == 9);
    }

    /* ---------------- summary ---------------- */
    printf("== %d passed, %d failed ==\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
