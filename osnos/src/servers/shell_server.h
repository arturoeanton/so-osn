#pragma once

#include <stdint.h>

void shell_server_init(void);

void shell_server_tick(void);

/* PID of the foreground user task the shell is currently blocking on,
 * or 0 if the shell itself is foreground. The TTY uses this to decide
 * where to deliver Ctrl+C (ISIG) — it never targets a kernel task. */
uint64_t shell_fg_pid(void);
