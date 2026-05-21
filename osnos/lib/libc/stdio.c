#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * osnos libc — stdio with a real FILE * layer.
 *
 *   Read path:  pushback ↘
 *                          fread / fgetc — drain pushback, then drain
 *                          buf, then refill via read() syscall.
 *
 *   Write path: fwrite / fputc — append to buf; flush on full, on
 *                          '\n' if line-buffered, or on direction
 *                          switch / fflush / fclose.
 *
 * Single buffer, role flips on direction switch. When we switch from
 * read to write, the unconsumed read data is rewound via lseek so the
 * kernel offset matches what the caller thinks it is.
 *
 * printf / fprintf go through fwrite, so a line-buffered stdout gets
 * the per-newline flushing for free.
 */

struct __osnos_file {
    int     fd;
    int     err;           /* sticky error flag */
    int     eof;           /* sticky eof flag */
    int     mode;          /* _IOFBF / _IOLBF / _IONBF */
    int     dir;           /*  0 = none, 1 = writing, -1 = reading */
    int     pushback;      /* -1 = none, else next fgetc returns this */
    int     owns_buf;      /* free(buf) on close */
    char   *buf;
    size_t  bufcap;
    size_t  bufpos;        /* write: bytes pending; read: next byte */
    size_t  buflen;        /* read only: bytes loaded into buf */
    char    defbuf[BUFSIZ];
};

/* stdin/out/err: statically allocated. defbuf is part of each struct,
 * so no malloc at startup. */
static FILE __osnos_stdin = {
    .fd = 0, .mode = _IOLBF, .pushback = -1,
};
static FILE __osnos_stdout = {
    .fd = 1, .mode = _IOLBF, .pushback = -1,
};
static FILE __osnos_stderr = {
    .fd = 2, .mode = _IONBF, .pushback = -1,
};

FILE *const stdin  = &__osnos_stdin;
FILE *const stdout = &__osnos_stdout;
FILE *const stderr = &__osnos_stderr;

/* Lazily wire the default buffer the first time the file is used. */
static void ensure_buf(FILE *f) {
    if (f->buf) return;
    f->buf      = f->defbuf;
    f->bufcap   = sizeof(f->defbuf);
    f->owns_buf = 0;
}

/* Drain pending write buffer to fd. Returns 0 on success, -1 on error
 * with f->err set. */
static int drain_write(FILE *f) {
    if (f->dir != 1 || f->bufpos == 0) return 0;
    size_t off = 0;
    while (off < f->bufpos) {
        ssize_t w = write(f->fd, f->buf + off, f->bufpos - off);
        if (w <= 0) {
            f->err = 1;
            /* preserve unwritten bytes at the start of the buffer */
            size_t left = f->bufpos - off;
            for (size_t i = 0; i < left; i++) f->buf[i] = f->buf[off + i];
            f->bufpos = left;
            return -1;
        }
        off += (size_t)w;
    }
    f->bufpos = 0;
    return 0;
}

/* Drop unconsumed read buffer, rewinding the kernel offset so the
 * caller's view of the file position is consistent. */
static void drop_read(FILE *f) {
    if (f->dir != -1) return;
    size_t unread = f->buflen - f->bufpos;
    if (unread > 0) {
        /* Pull the offset back by `unread` bytes. Ignore failure;
         * non-seekable streams will just be out of sync, which is
         * the caller's problem if they read/write mix on a pipe. */
        lseek(f->fd, -(off_t)unread, SEEK_CUR);
    }
    f->bufpos = f->buflen = 0;
}

/* Prepare the file for a write. Drops any read-side state. */
static int prep_write(FILE *f) {
    ensure_buf(f);
    if (f->dir == -1) drop_read(f);
    f->dir = 1;
    return 0;
}

/* Prepare the file for a read. Flushes any write-side state. */
static int prep_read(FILE *f) {
    ensure_buf(f);
    if (f->dir == 1 && drain_write(f) < 0) return -1;
    f->dir = -1;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Public buffer-control API                                         */
/* ---------------------------------------------------------------- */

int fflush(FILE *f) {
    if (!f) return 0;   /* fflush(NULL) = no-op (libc doesn't track all) */
    if (f->dir == 1) return drain_write(f);
    if (f->dir == -1) drop_read(f);
    return 0;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    if (!f) return -1;
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) return -1;
    /* Flush before swapping buffers. */
    fflush(f);
    if (f->owns_buf) free(f->buf);
    if (buf && size > 0) {
        f->buf      = buf;
        f->bufcap   = size;
        f->owns_buf = 0;
    } else {
        f->buf      = f->defbuf;
        f->bufcap   = sizeof(f->defbuf);
        f->owns_buf = 0;
    }
    f->mode = mode;
    f->bufpos = f->buflen = 0;
    return 0;
}

void setbuf(FILE *f, char *buf) {
    if (buf) setvbuf(f, buf, _IOFBF, BUFSIZ);
    else     setvbuf(f, NULL, _IONBF, 0);
}

/* ---------------------------------------------------------------- */
/* fopen family                                                       */
/* ---------------------------------------------------------------- */

/* Parse "r" / "w" / "a" / "r+" / "w+" / "a+" (with optional 'b'). */
static int parse_mode(const char *mode) {
    int flags = 0;
    int plus  = 0;
    if (!mode || !mode[0]) return -1;
    /* second character may be '+' or 'b' (or 'b' then '+') */
    for (const char *p = mode + 1; *p; p++) {
        if (*p == '+') plus = 1;
        /* 'b' is ignored — no text/binary distinction in osnos */
    }
    switch (mode[0]) {
    case 'r':
        flags = plus ? O_RDWR : O_RDONLY;
        break;
    case 'w':
        flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
        break;
    case 'a':
        flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
        break;
    default:
        return -1;
    }
    return flags;
}

/*
 * Wrap a raw fd in a FILE*. Shared init between fopen and tmpfile.
 * Returns NULL on malloc failure; caller owns the fd in that case.
 */
static FILE *wrap_fd(int fd) {
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { errno = ENOMEM; return NULL; }
    f->fd       = fd;
    f->err      = 0;
    f->eof      = 0;
    f->mode     = _IOFBF;
    f->dir      = 0;
    f->pushback = -1;
    f->owns_buf = 0;
    f->buf      = f->defbuf;
    f->bufcap   = sizeof(f->defbuf);
    f->bufpos   = 0;
    f->buflen   = 0;
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int flags = parse_mode(mode);
    if (flags < 0) { errno = EINVAL; return NULL; }
    int fd = open(path, flags, 0644);
    if (fd < 0) return NULL;
    FILE *f = wrap_fd(fd);
    if (!f) { close(fd); return NULL; }
    return f;
}

FILE *tmpfile(void) {
    /* mkstemp creates /tmp/tmpf-XXXXXX with O_RDWR | O_CREAT |
     * O_EXCL. The file is NOT auto-deleted on fclose (see header
     * comment) — that needs an "open file description" decoupled
     * from the path, which osnos VFS doesn't have. */
    mkdir("/tmp", 0755);   /* idempotent: ignore EEXIST */
    char tmpl[32];
    const char *t = "/tmp/tmpf-XXXXXX";
    int i = 0; while (t[i]) { tmpl[i] = t[i]; i++; } tmpl[i] = 0;
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    FILE *f = wrap_fd(fd);
    if (!f) { close(fd); return NULL; }
    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *f) {
    if (!f) return NULL;
    fflush(f);
    if (f->fd >= 0 && f->fd > 2) close(f->fd);
    int flags = parse_mode(mode);
    if (flags < 0) { errno = EINVAL; return NULL; }
    int fd = open(path, flags, 0644);
    if (fd < 0) return NULL;
    f->fd       = fd;
    f->err      = 0;
    f->eof      = 0;
    f->dir      = 0;
    f->pushback = -1;
    f->bufpos   = 0;
    f->buflen   = 0;
    return f;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    fflush(f);
    int rc = 0;
    if (f->fd >= 0 && f->fd > 2) {
        if (close(f->fd) < 0) rc = EOF;
    }
    if (f->owns_buf) free(f->buf);
    /* Don't free the static stdin/stdout/stderr structs. */
    if (f != stdin && f != stdout && f != stderr) free(f);
    return rc;
}

/* ---------------------------------------------------------------- */
/* fread / fwrite                                                     */
/* ---------------------------------------------------------------- */

/* Internal: write `n` raw bytes through the file's write buffer.
 * Returns the number of bytes accepted (== n on success). */
static size_t do_write(FILE *f, const char *p, size_t n) {
    if (prep_write(f) < 0) return 0;
    /* Unbuffered: bypass buffer entirely. */
    if (f->mode == _IONBF || f->bufcap == 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(f->fd, p + off, n - off);
            if (w <= 0) { f->err = 1; return off; }
            off += (size_t)w;
        }
        return off;
    }
    size_t off = 0;
    while (off < n) {
        size_t room = f->bufcap - f->bufpos;
        size_t take = n - off < room ? n - off : room;
        for (size_t i = 0; i < take; i++) f->buf[f->bufpos + i] = p[off + i];
        f->bufpos += take;
        off += take;
        /* Flush if buffer full. */
        if (f->bufpos == f->bufcap) {
            if (drain_write(f) < 0) return off;
        } else if (f->mode == _IOLBF) {
            /* Flush if we just buffered a newline. Walk backwards from
             * the last write to find one — cheaper than per-byte check
             * in the inner loop. */
            for (size_t i = 0; i < take; i++) {
                if (f->buf[f->bufpos - take + i] == '\n') {
                    if (drain_write(f) < 0) return off;
                    break;
                }
            }
        }
    }
    return n;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || !ptr || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    size_t did   = do_write(f, (const char *)ptr, total);
    return did / size;
}

/* Refill the read buffer from fd. Returns bytes loaded, 0 on EOF,
 * -1 on error. */
static ssize_t refill(FILE *f) {
    if (f->dir != -1) return 0;
    if (f->bufpos < f->buflen) return (ssize_t)(f->buflen - f->bufpos);
    f->bufpos = 0;
    ssize_t r = read(f->fd, f->buf, f->bufcap);
    if (r < 0) { f->err = 1; f->buflen = 0; return -1; }
    if (r == 0) { f->eof = 1; f->buflen = 0; return 0; }
    f->buflen = (size_t)r;
    return r;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || !ptr || size == 0 || nmemb == 0) return 0;
    if (prep_read(f) < 0) return 0;
    char *out = (char *)ptr;
    size_t want = size * nmemb;
    size_t got  = 0;

    /* Drain pushback first. */
    if (f->pushback >= 0 && want > 0) {
        out[got++] = (char)f->pushback;
        f->pushback = -1;
    }

    while (got < want) {
        if (f->bufpos >= f->buflen) {
            ssize_t r = refill(f);
            if (r <= 0) break;
        }
        size_t avail = f->buflen - f->bufpos;
        size_t take  = want - got < avail ? want - got : avail;
        for (size_t i = 0; i < take; i++) out[got + i] = f->buf[f->bufpos + i];
        f->bufpos += take;
        got       += take;
    }
    return got / size;
}

/* ---------------------------------------------------------------- */
/* Character / line I/O                                               */
/* ---------------------------------------------------------------- */

int fgetc(FILE *f) {
    if (!f) return EOF;
    if (prep_read(f) < 0) return EOF;
    if (f->pushback >= 0) {
        int c = f->pushback;
        f->pushback = -1;
        return c;
    }
    if (f->bufpos >= f->buflen) {
        ssize_t r = refill(f);
        if (r <= 0) return EOF;
    }
    return (unsigned char)f->buf[f->bufpos++];
}

int getc(FILE *f)     { return fgetc(f); }
int getchar(void)     { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    if (!f || c == EOF) return EOF;
    /* Only one pushback slot — POSIX guarantees at least 1. */
    if (f->pushback >= 0) return EOF;
    f->pushback = (unsigned char)c;
    f->eof = 0;
    return c;
}

char *fgets(char *buf, int size, FILE *f) {
    if (!buf || size <= 0 || !f) return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = 0;
    return buf;
}

int fputc(int c, FILE *f) {
    char ch = (char)c;
    if (do_write(f, &ch, 1) != 1) return EOF;
    return (unsigned char)c;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }

int fputs(const char *s, FILE *f) {
    if (!s || !f) return EOF;
    size_t n = strlen(s);
    if (do_write(f, s, n) != n) return EOF;
    return 0;
}

int puts(const char *s) {
    if (fputs(s, stdout) < 0) return EOF;
    if (fputc('\n', stdout) == EOF) return EOF;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Positioning                                                        */
/* ---------------------------------------------------------------- */

int fseek(FILE *f, long off, int whence) {
    if (!f) return -1;
    fflush(f);
    f->pushback = -1;
    f->eof      = 0;
    off_t r = lseek(f->fd, (off_t)off, whence);
    return r < 0 ? -1 : 0;
}

long ftell(FILE *f) {
    if (!f) return -1;
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    /* Adjust for buffered state. */
    if (f->dir == 1) pos += (off_t)f->bufpos;
    else if (f->dir == -1) pos -= (off_t)(f->buflen - f->bufpos);
    if (f->pushback >= 0) pos -= 1;
    return (long)pos;
}

void rewind(FILE *f) {
    if (!f) return;
    fseek(f, 0, SEEK_SET);
    f->err = 0;
}

/* ---------------------------------------------------------------- */
/* Status                                                             */
/* ---------------------------------------------------------------- */

int feof(FILE *f)   { return f ? f->eof : 0; }
int ferror(FILE *f) { return f ? f->err : 0; }
void clearerr(FILE *f) { if (f) { f->err = 0; f->eof = 0; } }
int fileno(FILE *f) { return f ? f->fd : -1; }

void perror(const char *prefix) {
    /* No strerror table yet; print errno number. */
    if (prefix && prefix[0]) {
        fputs(prefix, stderr);
        fputs(": ", stderr);
    }
    /* small itoa */
    char buf[16];
    int e = errno;
    int neg = e < 0;
    if (neg) { e = -e; fputc('-', stderr); }
    int i = 0;
    do { buf[i++] = (char)('0' + e % 10); e /= 10; } while (e);
    while (i--) fputc(buf[i], stderr);
    fputc('\n', stderr);
}

int remove(const char *path) {
    return unlink(path);
}

/* ---------------------------------------------------------------- */
/* printf engine — routes output through a sink. The sink either     */
/* drives a FILE * (do_write above) or fills a fixed char buffer for */
/* snprintf.                                                          */
/* ---------------------------------------------------------------- */

typedef struct {
    FILE   *f;          /* non-NULL: write via FILE layer */
    char   *buf;        /* non-NULL: fill in-memory       */
    size_t  cap;
    size_t  pos;
} sink_t;

static void sink_flush(sink_t *s, const char *p, size_t n) {
    if (n == 0) return;
    if (s->f) {
        do_write(s->f, p, n);
        s->pos += n;
    } else {
        size_t room = (s->cap > s->pos + 1) ? (s->cap - 1 - s->pos) : 0;
        size_t take = n < room ? n : room;
        for (size_t i = 0; i < take; i++) s->buf[s->pos + i] = p[i];
        s->pos += n;
    }
}

static void sink_putc(sink_t *s, char c) {
    sink_flush(s, &c, 1);
}

static char *utoa_rev(unsigned long long value, int base, int upper,
                       char *buf_end) {
    static const char digits_lo[] = "0123456789abcdef";
    static const char digits_up[] = "0123456789ABCDEF";
    const char *d = upper ? digits_up : digits_lo;
    char *p = buf_end;
    *--p = 0;
    if (value == 0) { *--p = '0'; return p; }
    while (value) { *--p = d[value % (unsigned)base]; value /= (unsigned)base; }
    return p;
}

static void emit_padded(sink_t *s, const char *src, size_t len,
                        int width, int left, char pad)
{
    if (left || width <= (int)len) {
        sink_flush(s, src, len);
        for (int i = (int)len; i < width; i++) sink_putc(s, ' ');
    } else {
        for (int i = (int)len; i < width; i++) sink_putc(s, pad);
        sink_flush(s, src, len);
    }
}

static int do_format(sink_t *s, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { sink_putc(s, *p); continue; }
        p++;
        if (!*p) break;

        int left = 0, zero = 0, plus = 0, space = 0;
        for (;; p++) {
            if      (*p == '-') left  = 1;
            else if (*p == '0') zero  = 1;
            else if (*p == '+') plus  = 1;
            else if (*p == ' ') space = 1;
            else break;
        }

        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0'); p++;
            }
        }

        int is_long = 0, is_longlong = 0, is_size = 0;
        if (*p == 'l') {
            is_long = 1; p++;
            if (*p == 'l') { is_longlong = 1; p++; }
        } else if (*p == 'z') {
            is_size = 1; p++;
        }

        char numbuf[66];
        char *first;
        size_t flen;
        int negative = 0;
        char prefix = 0;

        switch (*p) {
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            flen = strlen(str);
            emit_padded(s, str, flen, width, left, ' ');
            continue;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            emit_padded(s, &c, 1, width, left, ' ');
            continue;
        }
        case 'd': case 'i': {
            long long v;
            if      (is_longlong) v = va_arg(ap, long long);
            else if (is_long)     v = va_arg(ap, long);
            else if (is_size)     v = (long long)va_arg(ap, size_t);
            else                  v = va_arg(ap, int);
            unsigned long long uv;
            if (v < 0) { negative = 1; uv = (unsigned long long)(-v); }
            else                       uv = (unsigned long long)v;
            first = utoa_rev(uv, 10, 0, numbuf + sizeof(numbuf));
            flen  = strlen(first);
            if      (negative) prefix = '-';
            else if (plus)     prefix = '+';
            else if (space)    prefix = ' ';
            break;
        }
        case 'u': {
            unsigned long long v;
            if      (is_longlong) v = va_arg(ap, unsigned long long);
            else if (is_long)     v = va_arg(ap, unsigned long);
            else if (is_size)     v = (unsigned long long)va_arg(ap, size_t);
            else                  v = va_arg(ap, unsigned int);
            first = utoa_rev(v, 10, 0, numbuf + sizeof(numbuf));
            flen  = strlen(first);
            break;
        }
        case 'x': case 'X': {
            unsigned long long v;
            if      (is_longlong) v = va_arg(ap, unsigned long long);
            else if (is_long)     v = va_arg(ap, unsigned long);
            else if (is_size)     v = (unsigned long long)va_arg(ap, size_t);
            else                  v = va_arg(ap, unsigned int);
            first = utoa_rev(v, 16, *p == 'X', numbuf + sizeof(numbuf));
            flen  = strlen(first);
            break;
        }
        case 'o': {
            unsigned long long v;
            if      (is_longlong) v = va_arg(ap, unsigned long long);
            else if (is_long)     v = va_arg(ap, unsigned long);
            else if (is_size)     v = (unsigned long long)va_arg(ap, size_t);
            else                  v = va_arg(ap, unsigned int);
            first = utoa_rev(v, 8, 0, numbuf + sizeof(numbuf));
            flen  = strlen(first);
            break;
        }
        case 'p': {
            void *vp = va_arg(ap, void *);
            unsigned long long v = (unsigned long long)(uintptr_t)vp;
            first = utoa_rev(v, 16, 0, numbuf + sizeof(numbuf));
            flen  = strlen(first);
            sink_flush(s, "0x", 2);
            emit_padded(s, first, flen, width > 2 ? width - 2 : 0, left, '0');
            continue;
        }
        case '%':
            sink_putc(s, '%');
            continue;
        default:
            sink_putc(s, '%');
            sink_putc(s, *p);
            continue;
        }

        /* Emit numeric: optional prefix, optional zero-pad, digits. */
        int total = (int)flen + (prefix ? 1 : 0);
        if (!left && zero && width > total) {
            if (prefix) sink_putc(s, prefix);
            for (int i = total; i < width; i++) sink_putc(s, '0');
            sink_flush(s, first, flen);
        } else if (!left && width > total) {
            for (int i = total; i < width; i++) sink_putc(s, ' ');
            if (prefix) sink_putc(s, prefix);
            sink_flush(s, first, flen);
        } else {
            if (prefix) sink_putc(s, prefix);
            sink_flush(s, first, flen);
            for (int i = total; i < width; i++) sink_putc(s, ' ');
        }
    }
    /* snprintf NUL-terminates inside the buffer; fd path: caller flushes */
    if (s->buf && s->cap > 0) {
        size_t end = s->pos < s->cap - 1 ? s->pos : s->cap - 1;
        s->buf[end] = 0;
    }
    return (int)s->pos;
}

/* ---------------------------------------------------------------- */
/* Public printf API                                                  */
/* ---------------------------------------------------------------- */

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    sink_t s = { .f = f, .buf = 0, .cap = 0, .pos = 0 };
    return do_format(&s, fmt, ap);
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    sink_t s = { .f = 0, .buf = buf, .cap = size, .pos = 0 };
    return do_format(&s, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    /* No bound — caller asserts the buffer is large enough. We pass a
     * very large cap so do_format never truncates. */
    return vsnprintf(buf, (size_t)-1 / 2, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}
