#pragma once

/*
 * sys/wait.h — POSIX wait(2) / waitpid(2) surface.
 *
 * Status word encoding (matches Linux, which matches POSIX):
 *   - bits 0..6  : terminating signal (0 if normal exit)
 *   - bits 8..15 : exit code (when WIFEXITED == 1)
 *
 * osnos kernel encodes via encode_wait_status() in sys_wait4:
 *   exit_code < 128         → (code & 0xff) << 8        → WIFEXITED
 *   exit_code 128..159       → (code - 128) & 0x7f       → WIFSIGNALED
 *
 * Options bitmask (only WNOHANG implemented; WUNTRACED reserved for
 * job control wait-for-stop, currently a no-op).
 */

#include <sys/types.h>

/* Options. */
#define WNOHANG     1
#define WUNTRACED   2
#define WCONTINUED  8

/* Status-decoding macros. */
#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((signed char)(((s) & 0x7f) + 1) >> 1) > 0)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)
#define WIFCONTINUED(s) ((s) == 0xffff)

pid_t wait(int *wstatus);
pid_t waitpid(pid_t pid, int *wstatus, int options);
