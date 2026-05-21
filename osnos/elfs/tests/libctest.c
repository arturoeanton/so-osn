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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
        check("socket-enosys", s == -1 && errno == ENOSYS);
    }

    /* ---------------- summary ---------------- */
    printf("== %d passed, %d failed ==\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
