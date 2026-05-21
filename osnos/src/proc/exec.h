#pragma once

#include <stdint.h>

/*
 * Spawn a builtin by path.
 *
 * path must start with "/bin/" — anything else returns -ENOENT.
 * args is copied into kernel heap (kmalloc); the trampoline frees it
 * when the builtin returns. May be NULL or "" for argless commands.
 *
 * Returns the new pid (>= 1) on success, or negated osnos_status_t on error.
 *
 * Today the spawned task runs in ring 0 sharing the kernel address
 * space. In FASE 6 this is where ELF parsing + address-space setup
 * happens before scheduling the user task.
 */
int64_t proc_exec(const char *path, const char *args);

/*
 * Same as proc_exec but with an environment block. `envp` is a
 * NULL-terminated array of "KEY=VAL" strings, or NULL for no env.
 * The strings are copied onto the new task's user stack so the
 * caller is free to dispose of them after this returns.
 */
int64_t proc_execve(const char *path, const char *args,
                     const char *const *envp);

/*
 * Kill the currently-running ring-3 task: tear down its address space,
 * notify the shell, and long-jump back to scheduler_loop. Never returns.
 *
 * Callable from:
 *   - sys_exit (user-task path)
 *   - the IDT exception handlers, when a fault comes from CPL=3
 *
 * The per-task kernel stack we are running on is intentionally leaked
 * (we cannot free a stack while standing on it). A reaper pass will
 * collect those in FASE 6.3d.
 *
 * Assumes task_current()->pml4 != 0. Caller must check.
 */
__attribute__((noreturn))
void proc_exit_current_user(int exit_code);
