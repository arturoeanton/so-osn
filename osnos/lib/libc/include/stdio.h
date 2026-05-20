#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * osnos libc — stdio with a real FILE * layer.
 *
 * FILE wraps an fd plus a single shared buffer that flips role between
 * read and write according to the last call. Mixing reads and writes
 * on the same handle is supported but each direction switch implies
 * a flush (write) or a kernel seek-back (read) — call fflush() if
 * you care about ordering.
 *
 * Buffering modes:
 *   _IOFBF — fully buffered (default for fopen'd files)
 *   _IOLBF — line buffered (default for terminals; flush on '\n')
 *   _IONBF — unbuffered (default for stderr)
 *
 * stdin / stdout / stderr are real FILE * pointers (no longer ints).
 * Old code using fprintf(stderr, ...) keeps working — stderr is the
 * same identifier, only its type changed.
 */

#define EOF (-1)
#define BUFSIZ 512

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

typedef struct __osnos_file FILE;

extern FILE *const stdin;
extern FILE *const stdout;
extern FILE *const stderr;

/* fopen / freopen / fclose ----------------------------------------- */
FILE *fopen  (const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
int   fclose (FILE *f);

/* Bulk I/O --------------------------------------------------------- */
size_t fread (void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);

/* Character / line I/O --------------------------------------------- */
int   fgetc  (FILE *f);
int   getc   (FILE *f);
int   getchar(void);
char *fgets  (char *buf, int size, FILE *f);

int   fputc  (int c, FILE *f);
int   putc   (int c, FILE *f);
int   putchar(int c);
int   fputs  (const char *s, FILE *f);
int   puts   (const char *s);

int   ungetc (int c, FILE *f);

/* Buffer control --------------------------------------------------- */
int   fflush (FILE *f);
int   setvbuf(FILE *f, char *buf, int mode, size_t size);
void  setbuf (FILE *f, char *buf);

/* Positioning ------------------------------------------------------ */
int   fseek (FILE *f, long off, int whence);
long  ftell (FILE *f);
void  rewind(FILE *f);

/* Status ----------------------------------------------------------- */
int   feof    (FILE *f);
int   ferror  (FILE *f);
void  clearerr(FILE *f);
int   fileno  (FILE *f);

/* Formatted output ------------------------------------------------- */
int  printf  (const char *fmt, ...)
     __attribute__((format(printf, 1, 2)));
int  fprintf (FILE *f, const char *fmt, ...)
     __attribute__((format(printf, 2, 3)));
int  vprintf (const char *fmt, va_list ap);
int  vfprintf(FILE *f, const char *fmt, va_list ap);

int  snprintf (char *buf, size_t size, const char *fmt, ...)
     __attribute__((format(printf, 3, 4)));
int  vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

int  sprintf  (char *buf, const char *fmt, ...)
     __attribute__((format(printf, 2, 3)));
int  vsprintf (char *buf, const char *fmt, va_list ap);

/* perror */
void perror(const char *prefix);

/* remove (alias for unlink) */
int  remove (const char *path);
