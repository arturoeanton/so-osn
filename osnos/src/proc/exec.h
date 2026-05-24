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
 * Spawn with stdin/stdout redirected to files. Paths are absolute
 * (or NULL/"" for no redirect on that stream). stdout_append=1
 * matches the shell's `>>` (don't truncate); =0 is `>` (truncate
 * at exec time so prior output is cleared).
 *
 * The new task inherits the redirect paths via task_t fields and
 * sys_read/sys_write pick them up automatically.
 */
int64_t proc_execve_redir(const char *path, const char *args,
                           const char *const *envp,
                           const char *stdin_path,
                           const char *stdout_path,
                           int stdout_append);

/*
 * N-stage pipeline: spawn `s0 | s1 | ... | s(n-1)`. All tasks run
 * concurrently with N-1 kernel pipes shuttling stdout → stdin
 * between adjacent stages. The shell tracks every pid so the
 * prompt only redraws after the DOWNSTREAM-most one exits.
 *
 *   paths    = array of N absolute paths (one per stage).
 *   args     = array of N argv-tails (the bytes after the cmd).
 *   n_stages = 2..MAX_PIPELINE_STAGES.
 *   envp     = shared environment for all stages.
 *   pids_out = optional [n_stages] array; receives each stage's
 *              pid on success (downstream is pids_out[n-1]).
 *
 * Returns the pid of the LAST stage (the one whose exit ends the
 * pipeline) on success. Negative on failure; partial state is
 * torn down (created pipes closed, already-spawned upstream tasks
 * flagged kill_pending). pids_out is left undefined on failure.
 */
#define MAX_PIPELINE_STAGES 4

int64_t proc_execve_pipeline(const char *const paths[],
                              const char *const args[],
                              int n_stages,
                              const char *const *envp,
                              int64_t pids_out[]);

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

/*
 * proc_execve_replace — Linux execve(2) semantics. Replaces the
 * current task's user-mode image with the program at `path`, keeping
 * the same pid, kernel stack, fd table, cwd, and FXSAVE area lifecycle.
 *
 *   args  : space-separated argv-tail (argv[1..N], same shape as
 *           proc_execve). build_argv_block tokenizes it.
 *   envp  : NULL-terminated kernel array of "KEY=VAL" strings.
 *
 * On success this function NEVER returns — it calls sched_resume_jump
 * after swapping in the new pml4. On failure (file not found, bad
 * ELF, OOM) the OLD task image is fully preserved and the function
 * returns a negated osnos_status_t.
 */
int64_t proc_execve_replace(const char *path, const char *args,
                              const char *const *envp);

/*
 * proc_execve_replace_argv — variante de proc_execve_replace que
 * recibe argv como ARRAY de strings (no string aplanado), preservando
 * exactamente los boundaries que entregó la app. Usado por sys_execve
 * (Linux execve(2) entrega argv[]); sin esto, una arg como "echo X"
 * se rompía en dos tokens al re-tokenizar el string aplanado.
 *
 *   argv : array NULL-terminated. argv[0] es el program name; el resto
 *          son las args. Cada elemento es un string crudo (no quoting).
 *   envp : igual que proc_execve_replace.
 */
int64_t proc_execve_replace_argv(const char *path,
                                  const char *const *argv,
                                  const char *const *envp);
