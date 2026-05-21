#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../include/osnos_limits.h"
#include "fd.h"

#define MAX_TASKS 16

typedef void (*task_entry_t)(void);

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_STOPPED,    /* Ctrl+Z'd; scheduler skips until SIGCONT */
    TASK_DEAD
} task_state_t;

typedef struct task {
    uint64_t pid;
    const char *name;
    task_entry_t entry;
    task_state_t state;
    uint64_t dispatches;   /* # of times entry() has been invoked */
    uint64_t *pml4;        /* per-task address space; NULL = use kernel_pml4 */

    /* Exit code reported via IPC_PROC_EXITED. */
    int    exit_code;

    /*
     * Ring-3 task fields:
     *   kernel_stack_top  -- top of the per-task kernel stack; written
     *                        to TSS.RSP0 when this task is dispatched
     *                        (CPU lands here on ring 3 → 0 transition)
     *   kernel_stack_base -- pointer returned by kmalloc; handed to the
     *                        reaper at task death so it can be kfree'd
     *                        once we are no longer standing on it.
     *   pml4              -- already declared above; non-NULL means
     *                        "this is a ring-3 user task"
     */
    uint64_t  kernel_stack_top;
    void     *kernel_stack_base;

    /*
     * Ring-3 entry parameters. For flat user blobs these point at the
     * fixed legacy slot (USER_CODE_VIRT / USER_STACK_TOP); for ELF
     * user tasks the loader sets them to e_entry and the allocated
     * user stack top respectively. user_task_trampoline reads these
     * before iretq.
     */
    uint64_t  user_entry;
    uint64_t  user_stack_top;

    /*
     * User-mode heap window. heap_start is fixed at task creation
     * (above all PT_LOAD segments, below the user stack). heap_brk is
     * the current "break" — sys_brk grows or shrinks it on demand,
     * mapping/unmapping pages along the way. malloc in user libc sits
     * on top via sbrk().
     */
    uint64_t  heap_start;
    uint64_t  heap_brk;

    /*
     * Suspension state for sleeping / yielded user tasks.
     *
     * When a user task suspends via a syscall (today: sys_nanosleep),
     * the kernel snapshots its full iret frame + 15 GPRs into here
     * and longjmps back to the scheduler. The kernel stack of the
     * task is abandoned for this round.
     *
     * Resume path: user_task_trampoline, when dispatched again, sees
     * saved_valid == 1, pushes the iret frame onto a fresh kstack,
     * restores GPRs (rax overwritten with `saved_return`), and
     * iretq's back to user. saved_valid is then cleared.
     *
     * wakeup_at_ms: timer_ms() target at which the scheduler should
     * mark this task READY again. Tasks with wakeup_at_ms == 0 are
     * not on a timer (just plain BLOCKED).
     */
    int       saved_valid;
    uint64_t  wakeup_at_ms;

    /*
     * Sticky "kill me" flag set by the shell when the user presses
     * Ctrl+C while a foreground task is running. Checked on every
     * return-to-user path (syscall handler, timer IRQ, fault recovery)
     * — if set, the path routes to proc_exit_current_user(130) instead
     * of resuming. SIGINT convention exit code = 128 + 2 = 130.
     */
    int       kill_pending;
    /*
     * Stop pending — set when Ctrl+Z (VSUSP) hits while this task
     * is the foreground process. user_task_trampoline checks it on
     * dispatch and, if set, transitions state→TASK_STOPPED + longjmps
     * back to the scheduler instead of returning to userland. The
     * shell's `fg` / `bg` cmds (or any SIGCONT delivery) flip state
     * back to TASK_READY and the task resumes via its saved_iret_*.
     */
    int       stop_pending;
    /* iret frame */
    uint64_t  saved_iret_rip;
    uint64_t  saved_iret_cs;
    uint64_t  saved_iret_rflags;
    uint64_t  saved_iret_rsp;
    uint64_t  saved_iret_ss;
    /* user GPRs */
    uint64_t  saved_rax;
    uint64_t  saved_rbx;
    uint64_t  saved_rcx;
    uint64_t  saved_rdx;
    uint64_t  saved_rsi;
    uint64_t  saved_rdi;
    uint64_t  saved_rbp;
    uint64_t  saved_r8;
    uint64_t  saved_r9;
    uint64_t  saved_r10;
    uint64_t  saved_r11;
    uint64_t  saved_r12;
    uint64_t  saved_r13;
    uint64_t  saved_r14;
    uint64_t  saved_r15;

    /* Per-task current working directory. Used by sys_getcwd /
     * sys_chdir so user programs can resolve relative paths without
     * the libc having to know $PWD. Kernel tasks (servers) ignore
     * this field; only user tasks read it. */
    char      cwd[OSNOS_PATH_MAX];

    /*
     * Per-task stdin/stdout redirection — populated by the shell
     * when parsing `cmd < in.txt > out.txt`. When set, sys_read(0)
     * pulls from the file instead of the TTY, and sys_write(1)
     * writes to the file instead of the console.
     *
     * Empty string ([0] == 0) means "no redirect, use normal
     * TTY/console path". stdout_append distinguishes `>` (truncate
     * at exec time, then track offset) from `>>` (append from
     * current EOF). Offsets are tracked per-task too, since the fd
     * table itself stays global today.
     */
    char      stdin_redir [OSNOS_PATH_MAX];
    char      stdout_redir[OSNOS_PATH_MAX];
    int       stdout_append;     /* 1 for `>>`, 0 for `>` */
    uint64_t  stdin_redir_off;   /* read cursor into stdin file */
    uint64_t  stdout_redir_off;  /* write cursor into stdout file */

    /*
     * Per-task FPU/SSE state for FXSAVE/FXRSTOR. The instruction
     * dumps/loads 512 bytes aligned to 16. We snapshot the dying
     * task's HW regs into here when task_run_next switches to a
     * different task, and reload the incoming task's bytes before
     * resuming it. Kernel tasks don't touch FP (built with
     * -mno-sse), so their slot ends up holding a "clean" state.
     *
     * Initial contents come from a FNINIT+FXSAVE capture at
     * task_create_user_elf time so a freshly-spawned task's first
     * dispatch loads a sane FPU rather than uninitialised bytes.
     */
    uint8_t   fpu_state[512] __attribute__((aligned(16)));

    /*
     * Anonymous mmap regions. Bump-allocator: mmap_next points at
     * the next free virtual address, mmap_regions[] remembers each
     * live region so munmap can free its pages. VA isn't reclaimed
     * on munmap — simple, leaky, but enough for typical workloads
     * (TCC, scratch buffers, "big malloc-replacement") that mmap a
     * few times and free at exit. Refill of the slot table when an
     * entry munmaps so the cap of 16 is per-snapshot, not lifetime.
     */
#define TASK_MMAP_MAX 16
    uint64_t  mmap_next;
    struct {
        uint64_t addr;       /* 0 = empty slot */
        uint64_t len;
    } mmap_regions[TASK_MMAP_MAX];

    /*
     * Per-task file descriptor table (FASE 10.0.a).
     *
     * Replaces the old kernel-global fd[] table — each task now owns
     * its own slots. fd 0/1/2 are wired by fd_init_for_task at task
     * creation; fd 3+ get allocated by sys_open / sys_socket / etc.
     *
     * Kernel-resident tasks (servers, shell pre-FASE-10) get the same
     * layout for uniformity; they normally don't issue read/write
     * syscalls but having 0/1/2 set up means cmd_test inside the
     * shell can exercise the fd API against its own task without
     * special-casing.
     */
    osnos_fd_t fds[OSNOS_MAX_FDS];
} task_t;

void task_init(void);

int task_create(
    const char *name,
    task_entry_t entry
);

void task_set_state(
    uint64_t pid,
    task_state_t state
);

task_t *task_current(void);

void task_run_next(void);
void task_unblock(uint64_t pid);

/* Read-only slot accessor for introspection. NULL for unused/out-of-range. */
const task_t *task_slot(size_t idx);

/* Mutable lookup by pid; used by proc_exec to finish setup right after
 * task_create. Returns NULL if no such task. */
task_t *task_by_pid(uint64_t pid);

/*
 * Walk the task table and convert every TASK_DEAD slot back to
 * TASK_UNUSED. Called by the reaper after pending kstacks have been
 * kfree'd. Does NOT free any resource itself — proc_exit_current_user
 * is responsible for tearing down the address space and notifying
 * parents before the slot reaches DEAD.
 */
void task_reap_dead(void);

/*
 * Walk BLOCKED tasks. Any one whose wakeup_at_ms is non-zero and
 * has been reached (now_ms >= wakeup_at_ms) flips to TASK_READY.
 * Called from scheduler_tick every iteration. saved_valid stays
 * set so user_task_trampoline knows to replay the iret frame.
 */
void task_check_wakeups(uint64_t now_ms);

/* Diagnostic — total count of wakeups fired by task_check_wakeups. */
uint64_t task_wakeups_fired(void);
