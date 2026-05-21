#include <errno.h>
#include <stdlib.h>
#include <string.h>

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n-- > 0) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca != cb) return ca - cb;
        if (ca == 0)  return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *p = dst;
    while ((*p++ = *src++)) {}
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (     ; i < n;          i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *p = dst;
    while (*p) p++;
    while ((*p++ = *src++)) {}
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *p = dst;
    while (*p) p++;
    while (n-- > 0 && *src) *p++ = *src++;
    *p = 0;
    return dst;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    for (; *s; s++) if (*s == ch) return (char *)s;
    return ch == 0 ? (char *)s : 0;
}

char *strrchr(const char *s, int c) {
    char ch = (char)c;
    const char *last = 0;
    for (; *s; s++) if (*s == ch) last = s;
    if (ch == 0) return (char *)(s);
    return (char *)last;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char       *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char       *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    unsigned char  v = (unsigned char)c;
    while (n--) *d++ = v;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    unsigned char        v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) if (p[i] == v) return (void *)(p + i);
    return 0;
}

void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    unsigned char        v = (unsigned char)c;
    for (size_t i = n; i > 0; i--) if (p[i - 1] == v) return (void *)(p + i - 1);
    return 0;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p  = (char *)malloc(n);
    if (!p) { errno = ENOMEM; return 0; }
    for (size_t i = 0; i < n; i++) p[i] = s[i];
    return p;
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char  *p   = (char *)malloc(len + 1);
    if (!p) { errno = ENOMEM; return 0; }
    for (size_t i = 0; i < len; i++) p[i] = s[i];
    p[len] = 0;
    return p;
}

char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    size_t nlen = strlen(needle);
    for (; *hay; hay++) {
        if (strncmp(hay, needle, nlen) == 0) return (char *)hay;
    }
    return 0;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) return (char *)s;
        }
    }
    return 0;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    for (; s[n]; n++) {
        const char *a = accept;
        for (; *a; a++) if (s[n] == *a) break;
        if (!*a) return n;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    for (; s[n]; n++) {
        for (const char *r = reject; *r; r++) {
            if (s[n] == *r) return n;
        }
    }
    return n;
}

char *strtok_r(char *s, const char *delim, char **saveptr) {
    if (!s) s = *saveptr;
    if (!s) return 0;
    /* skip leading delimiters */
    s += strspn(s, delim);
    if (!*s) { *saveptr = 0; return 0; }
    char *tok = s;
    s += strcspn(s, delim);
    if (*s) { *s = 0; *saveptr = s + 1; }
    else    { *saveptr = 0; }
    return tok;
}

char *strtok(char *s, const char *delim) {
    static char *state;
    return strtok_r(s, delim, &state);
}

static int ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = ascii_lower((unsigned char)*a);
        int cb = ascii_lower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        if (ca == 0)  return 0;
    }
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n-- > 0) {
        int ca = ascii_lower((unsigned char)*a++);
        int cb = ascii_lower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
        if (ca == 0)  return 0;
    }
    return 0;
}

char *strerror(int errnum) {
    static char buf[24];
    switch (errnum) {
    case 0:                 return "Success";
    case EPERM:             return "Operation not permitted";
    case ENOENT:            return "No such file or directory";
    case ESRCH:             return "No such process";
    case EINTR:             return "Interrupted system call";
    case EIO:               return "Input/output error";
    case E2BIG:             return "Argument list too long";
    case EBADF:             return "Bad file descriptor";
    case EAGAIN:            return "Resource temporarily unavailable";
    case ENOMEM:            return "Out of memory";
    case EACCES:            return "Permission denied";
    case EFAULT:            return "Bad address";
    case EBUSY:             return "Device or resource busy";
    case EEXIST:            return "File exists";
    case ENOTDIR:           return "Not a directory";
    case EISDIR:            return "Is a directory";
    case EINVAL:            return "Invalid argument";
    case ENFILE:            return "Too many open files in system";
    case EMFILE:            return "Too many open files";
    case ENOTTY:            return "Not a typewriter";
    case ENOSPC:            return "No space left on device";
    case EROFS:             return "Read-only file system";
    case ERANGE:            return "Numerical result out of range";
    case ENAMETOOLONG:      return "File name too long";
    case ENOSYS:            return "Function not implemented";
    case ENOTEMPTY:         return "Directory not empty";
    case ENOTSOCK:          return "Socket operation on non-socket";
    case EPROTONOSUPPORT:   return "Protocol not supported";
    case EAFNOSUPPORT:      return "Address family not supported";
    case EADDRINUSE:        return "Address already in use";
    case EADDRNOTAVAIL:     return "Cannot assign requested address";
    case ENETDOWN:          return "Network is down";
    case ECONNRESET:        return "Connection reset by peer";
    case ETIMEDOUT:         return "Connection timed out";
    case ECONNREFUSED:      return "Connection refused";
    case EINPROGRESS:       return "Operation now in progress";
    default: {
        /* "errno=NNN" — small itoa */
        char num[12];
        int e = errnum, neg = 0;
        if (e < 0) { neg = 1; e = -e; }
        int i = 0;
        do { num[i++] = (char)('0' + e % 10); e /= 10; } while (e);
        size_t k = 0;
        const char *pfx = neg ? "errno=-" : "errno=";
        while (pfx[k]) { buf[k] = pfx[k]; k++; }
        while (i--)   { buf[k++] = num[i]; }
        buf[k] = 0;
        return buf;
    }
    }
}
