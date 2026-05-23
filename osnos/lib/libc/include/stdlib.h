#pragma once

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void  *malloc (size_t size);
void  *calloc (size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free   (void *ptr);

int                atoi  (const char *s);
long               atol  (const char *s);
long long          atoll (const char *s);
long               strtol  (const char *s, char **endptr, int base);
long long          strtoll (const char *s, char **endptr, int base);
unsigned long      strtoul (const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);
/* Floating point parsers. Used by TCC for FP literal constant
 * folding; precision is "good enough for source-level constants",
 * not bit-perfect IEEE round-to-nearest. */
double             strtod  (const char *s, char **endptr);
/* system(): osnos has no shell-exec interface for ring-3 — returns
 * -1 for any command. Apps that want subprocess control use fork()
 * + execve() directly. */
int                system  (const char *cmd);

/* realpath: resolve `path` to an absolute pathname. osnos has no
 * symlinks so the resolution is "lexical normalize + absolutize via
 * getcwd if needed". If `resolved` is NULL we malloc the result. */
char              *realpath(const char *path, char *resolved);

/* ISO C rand/srand — linear congruential. Not cryptographic. */
#define RAND_MAX  0x7FFFFFFF
int   rand (void);
void  srand(unsigned seed);
float              strtof  (const char *s, char **endptr);
double             atof    (const char *s);

int      abs  (int  n);
long     labs (long n);
long long llabs(long long n);

typedef struct { int       quot; int       rem; } div_t;
typedef struct { long      quot; long      rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;
div_t   div  (int       n, int       d);
ldiv_t  ldiv (long      n, long      d);
lldiv_t lldiv(long long n, long long d);

/*
 * qsort / bsearch — standard interface. `compar(a, b)` returns
 * negative / zero / positive. qsort uses insertion sort up to small
 * sizes and a simple recursive quicksort otherwise.
 */
void  qsort  (void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size,
              int (*compar)(const void *, const void *));

/*
 * Environment. crt0 publishes the kernel-passed envp[] as the global
 * `environ` pointer; getenv walks that table. setenv / unsetenv /
 * putenv mutate a heap-resident copy (the initial array lives on the
 * stack so the first mutation duplicates it).
 */
extern char **environ;

char *getenv  (const char *name);
int   setenv  (const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv  (char *kv);

/*
 * mkstemp — overwrite the trailing "XXXXXX" of `template` with a
 * unique 6-char suffix and atomically create the file with
 * O_RDWR | O_CREAT | O_EXCL. Returns the new fd, or -1 + errno
 * (EINVAL if template doesn't end in XXXXXX, EEXIST if all 100
 * candidates collide). Template is modified in place.
 */
int   mkstemp(char *tmpl);

/*
 * atexit — registers a function to run on exit(). Up to 32 slots,
 * invoked in LIFO order. Returns 0 on success, non-zero on overflow.
 */
int   atexit  (void (*fn)(void));

__attribute__((noreturn))
void   exit   (int code);

__attribute__((noreturn))
void   abort  (void);

/*
 * POSIX pseudoterminal helpers. posix_openpt opens /dev/ptmx and
 * returns a master fd; grantpt is a no-op on osnos (no perm model);
 * unlockpt issues TIOCSPTLCK (also a no-op kernel-side but the
 * ABI exists for compatibility). ptsname_r writes "/dev/pts/<N>"
 * into the caller's buffer; the slave fd is then obtained with
 * open(pts_name, O_RDWR).
 */
int   posix_openpt(int flags);
int   grantpt    (int fd);
int   unlockpt   (int fd);
int   ptsname_r  (int fd, char *buf, size_t buflen);
char *ptsname    (int fd);
