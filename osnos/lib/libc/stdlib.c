#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "syscall.h"

/* ---------------------------------------------------------------- */
/* exit / abort                                                      */
/* ---------------------------------------------------------------- */

/* Forward decl — defined at the bottom of the file. */
static void run_atexit(void);

__attribute__((noreturn))
void exit(int code) {
    /* atexit handlers in LIFO order, then flush stdio, then syscall. */
    run_atexit();
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

__attribute__((noreturn))
void abort(void) {
    /*
     * SIGABRT convention: exit code = 128 + 6. Same value Linux
     * would produce if a signal handler weren't installed.
     */
    _exit(128 + 6);
}

/* ---------------------------------------------------------------- */
/* atoi / strtol                                                     */
/* ---------------------------------------------------------------- */

int       atoi (const char *s) { return (int)      strtol (s, 0, 10); }
long      atol (const char *s) { return            strtol (s, 0, 10); }
long long atoll(const char *s) { return            strtoll(s, 0, 10); }

/*
 * Core parser used by all four strto* variants. Reads up to 64 bits
 * into an unsigned accumulator; the signed wrappers apply the sign
 * afterwards. base==0 means "infer from prefix": 0x→16, 0→8, else 10.
 */
static unsigned long long parse_uint(const char *s, char **endptr,
                                      int base, int *neg_out) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    int neg = 0;
    if      (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }

    if ((base == 0 || base == 16) &&
        s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2; base = 16;
    } else if (base == 0 && *s == '0') {
        s++; base = 8;
    } else if (base == 0) {
        base = 10;
    }

    unsigned long long acc = 0;
    while (*s) {
        int d;
        if      (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else                             break;
        if (d >= base) break;
        acc = acc * (unsigned)base + (unsigned)d;
        s++;
    }
    if (endptr)  *endptr  = (char *)s;
    if (neg_out) *neg_out = neg;
    return acc;
}

long strtol(const char *s, char **endptr, int base) {
    int neg = 0;
    unsigned long long v = parse_uint(s, endptr, base, &neg);
    return neg ? -(long)v : (long)v;
}

long long strtoll(const char *s, char **endptr, int base) {
    int neg = 0;
    unsigned long long v = parse_uint(s, endptr, base, &neg);
    return neg ? -(long long)v : (long long)v;
}

unsigned long strtoul(const char *s, char **endptr, int base) {
    int neg = 0;
    unsigned long long v = parse_uint(s, endptr, base, &neg);
    return neg ? -(unsigned long)v : (unsigned long)v;
}

unsigned long long strtoull(const char *s, char **endptr, int base) {
    int neg = 0;
    unsigned long long v = parse_uint(s, endptr, base, &neg);
    return neg ? -v : v;
}

/* strtod — parses a decimal floating-point literal, optionally with
 * a fractional part and a [eE]±digits exponent. Not IEEE-perfect
 * (no Grisu/Ryu); good enough for TCC's constant folding and shell
 * arithmetic. Hex floats (0x1.fp10) are NOT supported. */
double strtod(const char *s, char **endptr) {
    if (!s) { if (endptr) *endptr = (char *)s; return 0.0; }
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }

    double v = 0.0;
    int any = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0 + (double)(*s - '0');
        s++; any = 1;
    }
    if (*s == '.') {
        s++;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (double)(*s - '0') * scale;
            scale *= 0.1;
            s++; any = 1;
        }
    }
    if (any && (*s == 'e' || *s == 'E')) {
        s++;
        int esign = 1;
        if (*s == '+') s++;
        else if (*s == '-') { esign = -1; s++; }
        int exp_v = 0;
        while (*s >= '0' && *s <= '9') {
            exp_v = exp_v * 10 + (*s - '0');
            s++;
        }
        /* v *= 10^(esign*exp_v) via repeated mul — fine up to ~1e308. */
        double p = 1.0;
        for (int i = 0; i < exp_v; i++) p *= 10.0;
        if (esign < 0) v /= p; else v *= p;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -v : v;
}

float strtof(const char *s, char **endptr) {
    return (float)strtod(s, endptr);
}

/* osnos doesn't have a distinct `long double` precision (clang lowers
 * `long double` to `double` on x86_64-elf without 80-bit FP), so
 * strtold is just strtod. */
long double strtold(const char *s, char **endptr) {
    return (long double)strtod(s, endptr);
}

double atof(const char *s) { return strtod(s, 0); }

int       abs  (int       n) { return n < 0 ? -n : n; }
long      labs (long      n) { return n < 0 ? -n : n; }
long long llabs(long long n) { return n < 0 ? -n : n; }

div_t div(int n, int d) {
    div_t r;
    r.quot = n / d;
    r.rem  = n % d;
    return r;
}

ldiv_t ldiv(long n, long d) {
    ldiv_t r;
    r.quot = n / d;
    r.rem  = n % d;
    return r;
}

lldiv_t lldiv(long long n, long long d) {
    lldiv_t r;
    r.quot = n / d;
    r.rem  = n % d;
    return r;
}

/* ---------------------------------------------------------------- */
/* malloc / free — sbrk-based, first-fit free list, no split / merge */
/* ---------------------------------------------------------------- */

typedef struct block_hdr {
    size_t              size;       /* payload bytes, header excluded */
    struct block_hdr   *next_free;  /* singly-linked LIFO when free   */
} block_hdr_t;

/* Round up to a 16-byte multiple so all returned pointers are aligned. */
#define ALLOC_ALIGN 16
#define ROUND_UP(n) (((n) + (ALLOC_ALIGN - 1)) & ~(size_t)(ALLOC_ALIGN - 1))

static block_hdr_t *free_list_head;

static void *hdr_to_payload(block_hdr_t *h) {
    return (void *)((char *)h + sizeof(block_hdr_t));
}

static block_hdr_t *payload_to_hdr(void *p) {
    return (block_hdr_t *)((char *)p - sizeof(block_hdr_t));
}

void *malloc(size_t size) {
    if (size == 0) return 0;
    size = ROUND_UP(size);

    /* First-fit walk of the free list. */
    block_hdr_t **link = &free_list_head;
    while (*link) {
        if ((*link)->size >= size) {
            block_hdr_t *h = *link;
            *link = h->next_free;
            h->next_free = 0;       /* mark as in-use (debugging aid) */
            return hdr_to_payload(h);
        }
        link = &(*link)->next_free;
    }

    /* No suitable free block — grow the heap. */
    size_t total = sizeof(block_hdr_t) + size;
    void *raw = sbrk((intptr_t)total);
    if (raw == (void *)-1) {
        errno = ENOMEM;
        return 0;
    }

    block_hdr_t *h = (block_hdr_t *)raw;
    h->size      = size;
    h->next_free = 0;
    return hdr_to_payload(h);
}

void free(void *ptr) {
    if (!ptr) return;
    block_hdr_t *h = payload_to_hdr(ptr);
    h->next_free  = free_list_head;
    free_list_head = h;
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)-1 / nmemb) {
        errno = ENOMEM;
        return 0;
    }
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return 0; }

    block_hdr_t *h = payload_to_hdr(ptr);
    if (h->size >= size) return ptr;   /* shrink in place */

    void *np = malloc(size);
    if (!np) return 0;
    memcpy(np, ptr, h->size);
    free(ptr);
    return np;
}

/* ---------------------------------------------------------------- */
/* qsort / bsearch                                                   */
/* ---------------------------------------------------------------- */

static void swap_bytes(char *a, char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char t = a[i]; a[i] = b[i]; b[i] = t;
    }
}

/* Insertion sort — used as the base case and for small ranges. */
static void insertion_sort(char *base, size_t nmemb, size_t size,
                            int (*cmp)(const void *, const void *)) {
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0; j--) {
            char *a = base + (j - 1) * size;
            char *b = base + j * size;
            if (cmp(a, b) <= 0) break;
            swap_bytes(a, b, size);
        }
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (!base || size == 0 || nmemb < 2) return;
    /*
     * Small-N → insertion sort. For larger we partition with a fixed
     * "last-element pivot" Lomuto scheme. Not pretty performance-wise
     * (worst case O(n²) on sorted input) but plenty for osnos's
     * dozens-of-items workloads.
     */
    if (nmemb <= 16) {
        insertion_sort((char *)base, nmemb, size, compar);
        return;
    }
    char *arr = (char *)base;
    char *pivot = arr + (nmemb - 1) * size;
    size_t i = 0;
    for (size_t j = 0; j < nmemb - 1; j++) {
        char *jp = arr + j * size;
        if (compar(jp, pivot) <= 0) {
            if (i != j) swap_bytes(arr + i * size, jp, size);
            i++;
        }
    }
    swap_bytes(arr + i * size, pivot, size);
    qsort(arr,                       i,             size, compar);
    qsort(arr + (i + 1) * size, nmemb - i - 1, size, compar);
}

void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size,
              int (*compar)(const void *, const void *)) {
    if (!base || size == 0 || nmemb == 0) return 0;
    const char *arr = (const char *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *mp = arr + mid * size;
        int r = compar(key, mp);
        if (r == 0) return (void *)mp;
        if (r <  0) hi = mid;
        else        lo = mid + 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Environment stubs (no envp wiring yet)                            */
/* ---------------------------------------------------------------- */

/* `environ` is published by crt0.S — it initially points at the
 * envp array on the user stack. The first mutation (setenv/unsetenv/
 * putenv) replaces it with a malloc'd table that we own. `env_owned`
 * tracks whether we've moved off the kernel-supplied array yet. */
static int     env_owned = 0;
static size_t  env_capacity = 0;

static size_t env_count(void) {
    if (!environ) return 0;
    size_t n = 0;
    while (environ[n]) n++;
    return n;
}

static int env_takeover(void) {
    if (env_owned) return 0;
    size_t n = env_count();
    size_t cap = n + 8;
    char **fresh = (char **)malloc(sizeof(char *) * (cap + 1));
    if (!fresh) return -1;
    for (size_t i = 0; i < n; i++) {
        size_t len = 0;
        while (environ[i][len]) len++;
        char *dup = (char *)malloc(len + 1);
        if (!dup) { /* leaked partial table — best-effort. */ return -1; }
        for (size_t j = 0; j <= len; j++) dup[j] = environ[i][j];
        fresh[i] = dup;
    }
    fresh[n] = 0;
    environ = fresh;
    env_capacity = cap;
    env_owned = 1;
    return 0;
}

static int env_find(const char *name) {
    if (!environ) return -1;
    size_t klen = 0;
    while (name[klen] && name[klen] != '=') klen++;
    for (size_t i = 0; environ[i]; i++) {
        const char *e = environ[i];
        size_t j = 0;
        while (j < klen && e[j] && e[j] == name[j]) j++;
        if (j == klen && e[j] == '=') return (int)i;
    }
    return -1;
}

char *getenv(const char *name) {
    int idx = env_find(name);
    if (idx < 0) return 0;
    size_t klen = 0;
    while (name[klen] && name[klen] != '=') klen++;
    return environ[idx] + klen + 1;     /* skip "KEY=" */
}

int putenv(char *kv) {
    /* POSIX semantics: the caller's buffer becomes part of environ. */
    if (!kv) return -1;
    int has_eq = 0;
    for (size_t i = 0; kv[i]; i++) if (kv[i] == '=') { has_eq = 1; break; }
    if (!has_eq) return -1;

    if (env_takeover() != 0) return -1;

    int idx = env_find(kv);
    if (idx >= 0) {
        environ[idx] = kv;
        return 0;
    }
    size_t n = env_count();
    if (n + 1 >= env_capacity) {
        size_t newcap = env_capacity * 2 + 4;
        char **bigger = (char **)malloc(sizeof(char *) * (newcap + 1));
        if (!bigger) return -1;
        for (size_t i = 0; i < n; i++) bigger[i] = environ[i];
        bigger[n] = 0;
        environ = bigger;
        env_capacity = newcap;
    }
    environ[n] = kv;
    environ[n + 1] = 0;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value || !*name) return -1;
    int existing = env_find(name);
    if (existing >= 0 && !overwrite) return 0;

    /* Compose "KEY=VAL" into a malloc'd buffer that putenv adopts. */
    size_t nlen = 0; while (name[nlen]) nlen++;
    size_t vlen = 0; while (value[vlen]) vlen++;
    char *kv = (char *)malloc(nlen + 1 + vlen + 1);
    if (!kv) return -1;
    for (size_t i = 0; i < nlen; i++) kv[i] = name[i];
    kv[nlen] = '=';
    for (size_t i = 0; i < vlen; i++) kv[nlen + 1 + i] = value[i];
    kv[nlen + 1 + vlen] = 0;
    return putenv(kv);
}

int unsetenv(const char *name) {
    if (env_takeover() != 0) return -1;
    int idx = env_find(name);
    if (idx < 0) return 0;
    /* Shift the tail down — we don't free the slot because we don't
     * know whether it was malloc'd by setenv or a static putenv buf. */
    size_t i = (size_t)idx;
    while (environ[i + 1]) { environ[i] = environ[i + 1]; i++; }
    environ[i] = 0;
    return 0;
}

/* ---------------------------------------------------------------- */
/* mkstemp                                                            */
/* ---------------------------------------------------------------- */

/*
 * Produce a 6-char alphanumeric suffix from a 32-bit seed by base-36
 * encoding. No PRNG is required — we just spread the seed over the
 * 6 positions, and the EEXIST retry-loop in mkstemp handles
 * collisions. */
static void encode_suffix(unsigned int seed, char *out6) {
    for (int i = 0; i < 6; i++) {
        unsigned int v = seed % 36;
        out6[i] = (v < 10) ? (char)('0' + v) : (char)('a' + (v - 10));
        seed = (seed / 36) ^ ((seed * 1103515245u) + 12345u);
    }
}

int mkstemp(char *tmpl) {
    size_t n = 0;
    while (tmpl[n]) n++;
    if (n < 6) { errno = EINVAL; return -1; }
    for (int i = 0; i < 6; i++) {
        if (tmpl[n - 6 + i] != 'X') { errno = EINVAL; return -1; }
    }

    /* Mix time + pid + counter so siblings don't collide if they
     * call mkstemp in the same millisecond. */
    static unsigned int counter = 0;
    for (int attempt = 0; attempt < 100; attempt++) {
        unsigned int seed = (unsigned int)time(0) * 65537u
                          + (unsigned int)getpid() * 7919u
                          + counter++;
        encode_suffix(seed, tmpl + n - 6);

        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

/* ---------------------------------------------------------------- */
/* atexit                                                             */
/* ---------------------------------------------------------------- */

#define ATEXIT_MAX 32
static void (*atexit_fns[ATEXIT_MAX])(void);
static int   atexit_count;

int atexit(void (*fn)(void)) {
    if (!fn || atexit_count >= ATEXIT_MAX) return -1;
    atexit_fns[atexit_count++] = fn;
    return 0;
}

/* Called from exit(). Walks atexit handlers in LIFO order. Declared
 * here for exit() to find without leaking a public header. */
static void run_atexit(void) {
    while (atexit_count > 0) {
        void (*fn)(void) = atexit_fns[--atexit_count];
        if (fn) fn();
    }
}
