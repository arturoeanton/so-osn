#include "exec.h"

#include "../fs/vfs.h"
#include "../include/osnos_status.h"
#include "../lib/string.h"
#include "../micro/fd.h"
#include "../micro/fpu.h"
#include "../micro/gdt.h"
#include "../micro/ipc.h"
#include "elf.h"
#include "../micro/kmalloc.h"
#include "../micro/pipe.h"
#include "../micro/pmm.h"
#include "../micro/reaper.h"
#include "../micro/scheduler.h"
#include "../micro/task.h"
#include "../micro/tss.h"
#include "../micro/vmm.h"
#include "../net/socket.h"
#include "builtin.h"

#define USER_CODE_VIRT    0x400000ULL
#define USER_STACK_VIRT   0x500000ULL
#define USER_STACK_TOP    0x501000ULL
#define USER_KSTACK_BYTES 16384

/*
 * Default base of the user heap (sys_brk). High enough to stay clear
 * of any reasonable PT_LOAD layout (binaries land near 0x400000) and
 * comfortably below the user stack at 0x7FFFE000. Each task starts
 * with heap_brk == heap_start (zero-sized heap), and sbrk grows from
 * there.
 */
#define USER_HEAP_BASE    0x10000000ULL

/* ---------------------------------------------------------------- */
/* User-space task trampoline                                       */
/* ---------------------------------------------------------------- */

/*
 * Runs once when the scheduler dispatches a user task.
 *
 *   1. Loads the task's kernel stack into TSS.RSP0 so future ring 3 →
 *      ring 0 transitions land on a private stack.
 *   2. Swaps CR3 to the task's pml4.
 *   3. iretq's into user mode at the entry point.
 *
 * Never returns through the scheduler. Control comes back to the
 * scheduler only via sys_exit (proc_exit_current_user does the longjmp)
 * or a fault from user mode (idt.c fault_try_recover hands the task
 * to proc_exit_current_user too).
 */
/*
 * Resume a previously-suspended user task. The kernel stack of the
 * task is fresh (RSP = kstack_top); we hand-roll the iret frame +
 * GPR restore from t->saved_*. Packing everything into a small flat
 * buffer keeps the inline-asm input count manageable; %r15 is the
 * pointer into the buffer (loaded last so we don't clobber it
 * mid-restore).
 */
__attribute__((noreturn))
static void user_task_resume(task_t *t) {
    tss_set_rsp0(t->kernel_stack_top);
    uint64_t pml4_phys = (uint64_t)t->pml4 - pmm_hhdm_offset();

    uint64_t buf[20];
    buf[0]  = t->saved_iret_ss;
    buf[1]  = t->saved_iret_rsp;
    buf[2]  = t->saved_iret_rflags;
    buf[3]  = t->saved_iret_cs;
    buf[4]  = t->saved_iret_rip;
    buf[5]  = t->saved_rax;                /* rax on entry to user */
    buf[6]  = t->saved_rbx;
    buf[7]  = t->saved_rcx;
    buf[8]  = t->saved_rdx;
    buf[9]  = t->saved_rsi;
    buf[10] = t->saved_rdi;
    buf[11] = t->saved_rbp;
    buf[12] = t->saved_r8;
    buf[13] = t->saved_r9;
    buf[14] = t->saved_r10;
    buf[15] = t->saved_r11;
    buf[16] = t->saved_r12;
    buf[17] = t->saved_r13;
    buf[18] = t->saved_r14;
    buf[19] = t->saved_r15;

    t->saved_valid = 0;     /* consume; next dispatch is "fresh" again */

    __asm__ volatile (
        "mov  %0, %%cr3\n\t"
        "movq %1, %%r15\n\t"
        "pushq (%%r15)\n\t"             /* ss */
        "pushq 8(%%r15)\n\t"            /* user rsp */
        "pushq 16(%%r15)\n\t"           /* rflags */
        "pushq 24(%%r15)\n\t"           /* cs */
        "pushq 32(%%r15)\n\t"           /* rip */
        "movq  40(%%r15), %%rax\n\t"
        "movq  48(%%r15), %%rbx\n\t"
        "movq  56(%%r15), %%rcx\n\t"
        "movq  64(%%r15), %%rdx\n\t"
        "movq  72(%%r15), %%rsi\n\t"
        "movq  80(%%r15), %%rdi\n\t"
        "movq  88(%%r15), %%rbp\n\t"
        "movq  96(%%r15), %%r8\n\t"
        "movq  104(%%r15), %%r9\n\t"
        "movq  112(%%r15), %%r10\n\t"
        "movq  120(%%r15), %%r11\n\t"
        "movq  128(%%r15), %%r12\n\t"
        "movq  136(%%r15), %%r13\n\t"
        "movq  144(%%r15), %%r14\n\t"
        "movq  152(%%r15), %%r15\n\t"
        "iretq"
        :
        : "r"(pml4_phys), "r"(buf)
        : "memory"
    );
    __builtin_unreachable();
}

__attribute__((noreturn))
static void user_task_trampoline(void) {
    task_t *t = task_current();
    if (!t || !t->pml4 || !t->kernel_stack_top) {
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /*
     * Honor kill_pending before bouncing to ring 3. Catches the case
     * where a task was killed while BLOCKED (sleep wake-up via
     * sys_kill) — there's no syscall return / timer IRQ in CPL=3 to
     * trip the usual delivery points, so the trampoline does it here.
     */
    if (t->kill_pending) {
        proc_exit_current_user(130);
        __builtin_unreachable();
    }

    /* Resume previously-suspended task (sleep wake-up etc.). */
    if (t->saved_valid) user_task_resume(t);   /* never returns */

    /* Fresh start. */
    if (!t->user_entry || !t->user_stack_top) {
        for (;;) __asm__ volatile ("cli; hlt");
    }

    tss_set_rsp0(t->kernel_stack_top);
    uint64_t pml4_phys = (uint64_t)t->pml4 - pmm_hhdm_offset();

    __asm__ volatile (
        "mov %0, %%cr3\n\t"
        "pushq %3\n\t"              /* SS  = GDT_UDATA */
        "pushq %1\n\t"              /* RSP = task user stack top */
        "pushq $0x202\n\t"          /* RFLAGS (IF=1) */
        "pushq %4\n\t"              /* CS  = GDT_UCODE */
        "pushq %2\n\t"              /* RIP = task entry */
        "iretq"
        :
        : "r"(pml4_phys),
          "r"(t->user_stack_top),
          "r"(t->user_entry),
          "i"(GDT_UDATA),
          "i"(GDT_UCODE)
        : "memory"
    );

    __builtin_unreachable();
}

/* ---------------------------------------------------------------- */
/* task_create_user                                                  */
/* ---------------------------------------------------------------- */

/*
 * Spawn a ring-3 task running the given user-code blob.
 *
 *   - Creates a fresh address space (high half cloned from kernel)
 *   - Maps one page at USER_CODE_VIRT containing the code (padded
 *     with int3 so any fall-through traps)
 *   - Maps one page at USER_STACK_VIRT as the user stack
 *   - Allocates USER_KSTACK_BYTES of kernel stack for the task
 *   - Wires task->entry to the user trampoline above
 *
 * Returns pid on success or negated osnos_status_t on error.
 */
static int64_t task_create_user(
    const char *name,
    const uint8_t *code_start,
    const uint8_t *code_end
) {
    uint64_t *user_pml4 = address_space_create();
    if (!user_pml4) return -(int64_t)OSNOS_ENOMEM;

    /* User code page */
    uint64_t code_phys = pmm_alloc_page();
    if (!code_phys) {
        address_space_destroy(user_pml4);
        return -(int64_t)OSNOS_ENOMEM;
    }
    uint8_t *code_virt = (uint8_t *)(code_phys + pmm_hhdm_offset());

    size_t code_size = (size_t)(code_end - code_start);
    if (code_size > PAGE_SIZE) code_size = PAGE_SIZE;
    for (size_t i = 0; i < code_size; i++) code_virt[i] = code_start[i];
    for (size_t i = code_size; i < PAGE_SIZE; i++) code_virt[i] = 0xCC;

    if (!vmm_map(user_pml4, USER_CODE_VIRT, code_phys, PTE_U)) {
        pmm_free_page(code_phys);
        address_space_destroy(user_pml4);
        return -(int64_t)OSNOS_ENOMEM;
    }

    /* User stack page */
    uint64_t stack_phys = pmm_alloc_page();
    if (!stack_phys) {
        address_space_destroy(user_pml4);
        return -(int64_t)OSNOS_ENOMEM;
    }
    if (!vmm_map(user_pml4, USER_STACK_VIRT, stack_phys, PTE_W | PTE_U)) {
        pmm_free_page(stack_phys);
        address_space_destroy(user_pml4);
        return -(int64_t)OSNOS_ENOMEM;
    }

    /* Per-task kernel stack */
    void *kstack = kmalloc(USER_KSTACK_BYTES);
    if (!kstack) {
        address_space_destroy(user_pml4);
        return -(int64_t)OSNOS_ENOMEM;
    }

    int pid = task_create(name, user_task_trampoline);
    if (pid < 0) {
        kfree(kstack);
        address_space_destroy(user_pml4);
        return -(int64_t)OSNOS_EMFILE;
    }

    task_t *t = task_by_pid((uint64_t)pid);
    t->pml4              = user_pml4;
    t->kernel_stack_top  = (uint64_t)kstack + USER_KSTACK_BYTES;
    t->kernel_stack_base = kstack;
    t->user_entry        = USER_CODE_VIRT;
    t->user_stack_top    = USER_STACK_TOP;
    t->heap_start        = USER_HEAP_BASE;
    t->heap_brk          = USER_HEAP_BASE;

    return pid;
}

/* ---------------------------------------------------------------- */
/* build_argv_block — System V x86_64 init layout on the user stack  */
/* ---------------------------------------------------------------- */

/*
 * Lays out argc/argv/envp on the user stack page exactly as the
 * Linux x86_64 init protocol expects. _start (crt0.S in lib/libc)
 * reads argc from (%rsp), argv from %rsp+8, envp from %rsp+8+(argc+1)*8.
 *
 * Layout (low → high addresses):
 *   [argc] [argv[0] argv[1] ... argv[argc-1] NULL] [NULL_envp] [strings...]
 *   ^-- RSP                                                    ^-- user_stack_top
 *
 * Strings are NUL-terminated and live at the top of the page. The
 * pointer block lives just below; argc sits at the lowest used
 * address. The trampoline iretq's with RSP = address of argc.
 *
 * Returns the new RSP, or 0 if the block doesn't fit.
 */
#define MAX_ARGV 16
#define MAX_ENVP 32
#define ARGV_BLOCK_MAX 2048

/*
 * Build the Linux init protocol on the user stack: argc, argv[],
 * envp[], plus the backing strings. _start (crt0.S) reads:
 *   argc        at (%rsp)
 *   argv[]      at %rsp + 8                          (NULL-terminated)
 *   envp[]      at %rsp + 8 + (argc + 1) * 8         (NULL-terminated)
 *
 * Layout in memory (low → high):
 *   [argc] [argv[0..argc-1] NULL] [envp[0..envc-1] NULL] [strings...]
 *   ^-- RSP                                              ^-- user_stack_top
 *
 * `envp` is a NULL-terminated array of "KEY=VAL" strings, or NULL for
 * an empty environment.
 *
 * Returns the new RSP (argc address), or 0 if the block doesn't fit.
 */
static uint64_t build_argv_block(uint64_t   *pml4,
                                 uint64_t    user_stack_top,
                                 const char *prog_name,
                                 const char *args,
                                 const char *const *envp)
{
    const char *tokens[MAX_ARGV];
    size_t      token_lens[MAX_ARGV];
    int         argc = 0;

    /* argv[0] = the name we were exec'd as (e.g. "cat"). */
    tokens[argc]     = prog_name;
    token_lens[argc] = os_strlen(prog_name);
    argc++;

    /*
     * Tokenize args on whitespace, honoring "..." and '...' as a
     * single token whose body keeps spaces. No backslash escapes —
     * the closing quote of the same kind ends the token.
     */
    const char *p = args ? args : "";
    while (*p && argc < MAX_ARGV) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char quote = 0;
        if (*p == '"' || *p == '\'') { quote = *p; p++; }

        const char *start = p;
        if (quote) {
            while (*p && *p != quote) p++;
            tokens[argc]     = start;
            token_lens[argc] = (size_t)(p - start);
            if (*p == quote) p++;
        } else {
            while (*p && *p != ' ' && *p != '\t') p++;
            tokens[argc]     = start;
            token_lens[argc] = (size_t)(p - start);
        }
        argc++;
    }

    /* Snapshot envp[] pointers + lengths. envp may be NULL. */
    const char *envs[MAX_ENVP];
    size_t      env_lens[MAX_ENVP];
    int         envc = 0;
    if (envp) {
        while (envp[envc] && envc < MAX_ENVP) {
            envs[envc]     = envp[envc];
            env_lens[envc] = os_strlen(envp[envc]);
            envc++;
        }
    }

    /* Total bytes for ALL strings (argv + envp), each NUL-terminated. */
    size_t strings_total = 0;
    for (int i = 0; i < argc; i++) strings_total += token_lens[i] + 1;
    for (int i = 0; i < envc; i++) strings_total += env_lens[i] + 1;

    /* Stay well inside the 4 KiB user stack page. */
    size_t block_size =
        8 /* argc */ +
        8 * (size_t)(argc + 1) /* argv + NULL */ +
        8 * (size_t)(envc + 1) /* envp + NULL */ +
        ((strings_total + 7) & ~(size_t)7);
    if (block_size > ARGV_BLOCK_MAX) return 0;

    /* HHDM mapping for the stack page so we can write user virt
     * addresses while running on the kernel pml4. */
    uint64_t page_virt = user_stack_top - PAGE_SIZE;
    uint64_t phys      = vmm_lookup(pml4, page_virt) & PTE_ADDR_MASK;
    if (!phys) return 0;
    uint8_t *page = (uint8_t *)(phys + pmm_hhdm_offset());

    /* Strings region: 8-byte-aligned bottom, top at user_stack_top. */
    uint64_t strings_base_virt =
        (user_stack_top - strings_total) & ~(uint64_t)7;

    /* Pointer area: envp_null, envp[envc-1..0], argv_null, argv[argc-1..0], argc. */
    uint64_t envp_null_virt = strings_base_virt - 8;
    uint64_t envp0_virt     = envp_null_virt    - 8 * (uint64_t)envc;
    uint64_t argv_null_virt = envp0_virt        - 8;
    uint64_t argv0_virt     = argv_null_virt    - 8 * (uint64_t)argc;
    uint64_t argc_virt      = argv0_virt        - 8;

    /* Copy strings, recording the virt address of each. */
    uint64_t arg_ptrs[MAX_ARGV];
    uint64_t env_ptrs[MAX_ENVP];
    uint64_t cursor = strings_base_virt;
    for (int i = 0; i < argc; i++) {
        arg_ptrs[i] = cursor;
        size_t off = cursor - page_virt;
        for (size_t j = 0; j < token_lens[i]; j++) {
            page[off + j] = (uint8_t)tokens[i][j];
        }
        page[off + token_lens[i]] = 0;
        cursor += token_lens[i] + 1;
    }
    for (int i = 0; i < envc; i++) {
        env_ptrs[i] = cursor;
        size_t off = cursor - page_virt;
        for (size_t j = 0; j < env_lens[i]; j++) {
            page[off + j] = (uint8_t)envs[i][j];
        }
        page[off + env_lens[i]] = 0;
        cursor += env_lens[i] + 1;
    }

    /* Write the pointer block. */
    *(uint64_t *)(page + (envp_null_virt - page_virt)) = 0;
    for (int i = 0; i < envc; i++) {
        *(uint64_t *)(page + (envp0_virt + 8 * (uint64_t)i - page_virt)) =
            env_ptrs[i];
    }
    *(uint64_t *)(page + (argv_null_virt - page_virt)) = 0;
    for (int i = 0; i < argc; i++) {
        *(uint64_t *)(page + (argv0_virt + 8 * (uint64_t)i - page_virt)) =
            arg_ptrs[i];
    }
    *(uint64_t *)(page + (argc_virt - page_virt)) = (uint64_t)argc;

    return argc_virt;
}

/* ---------------------------------------------------------------- */
/* task_create_user_elf                                              */
/* ---------------------------------------------------------------- */

/*
 * Spawn a ring-3 task from an in-kernel ELF64 ET_EXEC blob.
 *
 * elf_load builds the address space (mapping every PT_LOAD plus a
 * stack page); we own the kstack allocation and the task slot. On
 * failure all partial state is released.
 */
static int64_t task_create_user_elf(
    const char    *name,
    const uint8_t *elf_data,
    size_t         elf_size,
    const char    *args,
    const char *const *envp
) {
    uint64_t *user_pml4   = 0;
    uint64_t  entry       = 0;
    uint64_t  stack_top   = 0;

    osnos_status_t s = elf_load(elf_data, elf_size, &user_pml4, &entry, &stack_top);
    if (s != OSNOS_OK) return -(int64_t)s;

    /*
     * Place the System V init block (argc/argv/envp) at the top of
     * the user stack page. The trampoline iretq's with RSP = address
     * of argc; crt0.S reads everything from there.
     */
    uint64_t init_rsp = build_argv_block(user_pml4, stack_top, name, args, envp);
    if (init_rsp == 0) {
        address_space_destroy(user_pml4);
        return -(int64_t)OSNOS_E2BIG;
    }

    void *kstack = kmalloc(USER_KSTACK_BYTES);
    if (!kstack) {
        address_space_destroy(user_pml4);
        return -(int64_t)OSNOS_ENOMEM;
    }

    int pid = task_create(name, user_task_trampoline);
    if (pid < 0) {
        kfree(kstack);
        address_space_destroy(user_pml4);
        return -(int64_t)OSNOS_EMFILE;
    }

    task_t *t = task_by_pid((uint64_t)pid);
    t->pml4              = user_pml4;
    t->kernel_stack_top  = (uint64_t)kstack + USER_KSTACK_BYTES;
    t->kernel_stack_base = kstack;
    t->user_entry        = entry;
    t->user_stack_top    = init_rsp;
    t->heap_start        = USER_HEAP_BASE;
    t->heap_brk          = USER_HEAP_BASE;

    /* Seed FPU state — first FXRSTOR on this task's first dispatch
     * needs sane bytes, not whatever the BSS happens to hold. */
    fpu_state_init(t->fpu_state);

    /* Seed cwd from envp's PWD entry, falling back to "/". Lets the
     * child resolve relative paths via getcwd() without the libc
     * having to consult $PWD. */
    os_strlcpy(t->cwd, "/", OSNOS_PATH_MAX);
    if (envp) {
        for (int i = 0; envp[i]; i++) {
            const char *e = envp[i];
            if (os_strncmp(e, "PWD=", 4) == 0) {
                os_strlcpy(t->cwd, e + 4, OSNOS_PATH_MAX);
                break;
            }
        }
    }

    return pid;
}

/* ---------------------------------------------------------------- */
/* proc_exit_current_user                                            */
/* ---------------------------------------------------------------- */

__attribute__((noreturn))
void proc_exit_current_user(int exit_code) {
    task_t *t = task_current();
    /*
     * Caller (sys_exit, fault handler) has already confirmed there is a
     * current ring-3 task. Treat anything else as a fatal kernel bug.
     */
    uint64_t *user_pml4   = t->pml4;
    void     *kstack_base = t->kernel_stack_base;

    /*
     * Close every fd the dying task held open. Without this, a task
     * killed mid-blocking-syscall (e.g. httpd in accept loop killed by
     * Ctrl+C) would leak its listen socket, its accepted children, any
     * open files. Most painfully, the listen socket's local_port stays
     * occupied so the next re-exec gets EADDRINUSE at bind().
     *
     * The fd table is global today (no per-process namespace), but the
     * shell server doesn't open fds (talks via IPC), so it's safe to
     * sweep every non-stdio entry on exit.
     */
    for (int fd = 3; fd < OSNOS_MAX_FDS; fd++) {
        osnos_fd_t *f = fd_get(fd);
        if (!f) continue;
        if (f->is_socket && f->sock_idx >= 0) {
            sock_close(f->sock_idx);
        }
        fd_free(fd);
    }

    /* Close any pipe endpoints the task held so the peer sees EOF
     * (reader exit) or EPIPE (writer exit) and can make progress
     * instead of looping on EAGAIN forever. */
    if (t->pipe_out) { pipe_close_writer(t->pipe_out); t->pipe_out = 0; }
    if (t->pipe_in)  { pipe_close_reader(t->pipe_in);  t->pipe_in  = 0; }

    t->state             = TASK_DEAD;
    t->pml4              = 0;
    t->kernel_stack_top  = 0;
    t->kernel_stack_base = 0;
    t->exit_code         = exit_code;

    uint64_t kpml4_phys =
        (uint64_t)vmm_kernel_pml4() - pmm_hhdm_offset();
    __asm__ volatile ("mov %0, %%cr3" :: "r"(kpml4_phys) : "memory");

    address_space_destroy(user_pml4);

    /*
     * Hand the per-task kernel stack to the reaper — we cannot kfree
     * it inline because we are still standing on it. The next
     * scheduler_tick (running on the scheduler's own stack after the
     * longjmp below) will drain the pending list.
     */
    reaper_add_kstack(kstack_base);

    ipc_msg_t msg;
    msg.from    = t->pid;
    msg.to      = SERVER_SHELL;
    msg.type    = IPC_PROC_EXITED;
    msg.arg0    = (uint64_t)exit_code;
    msg.arg1    = t->pid;
    msg.data[0] = 0;
    ipc_send(&msg);

    sched_resume_jump();
}

/* ---------------------------------------------------------------- */
/* proc_exec — dispatches user blob vs user ELF                     */
/* ---------------------------------------------------------------- */

/*
 * Maximum ELF blob we'll slurp from the VFS for an out-of-/bin
 * exec. Has to fit in the kernel heap (whose cap is 4 MiB today),
 * minus headroom for argv/envp/per-task structures. 2 MiB is
 * comfortably enough for TCC-class binaries (~200 KiB) and any
 * reasonable hand-written program.
 */
#define EXEC_VFS_BLOB_MAX (2 * 1024 * 1024)

int64_t proc_execve(const char *path, const char *args,
                     const char *const *envp) {
    if (!path) return -(int64_t)OSNOS_EFAULT;

    stdin_clear();

    /* Path 1 — builtin /bin/ entry (zero-copy: the ELF blob sits
     * in the kernel image already). Tried first because it's the
     * fast path and how the shell launches every "type the name"
     * command today. */
    if (os_strstarts(path, "/bin/")) {
        const char *name = path + 5;
        const builtin_t *b = builtin_find(name);
        if (b) {
            if (b->elf_start) {
                return task_create_user_elf(b->name,
                                            b->elf_start,
                                            (size_t)(b->elf_end - b->elf_start),
                                            args,
                                            envp);
            }
            if (b->user_start) {
                (void)args;
                return task_create_user(b->name, b->user_start, b->user_end);
            }
            return -(int64_t)OSNOS_ENOENT;
        }
        /* Fall through — maybe /bin is a mount with non-builtin
         * entries one day. Today binfs only serves builtins. */
    }

    /*
     * Path 2 — arbitrary path on the VFS. Read the file into a
     * kernel-side scratch buffer and hand the bytes to elf_load,
     * which copies them into the new task's address space page
     * by page. The scratch is freed before this function returns
     * regardless of outcome.
     */
    vfs_stat_t st;
    osnos_status_t s = vfs_stat(path, &st);
    if (s != OSNOS_OK)              return -(int64_t)s;
    if (st.type == VFS_NODE_DIR)    return -(int64_t)OSNOS_EISDIR;
    if (st.type != VFS_NODE_REG)    return -(int64_t)OSNOS_ENOEXEC;
    if (st.size == 0)               return -(int64_t)OSNOS_ENOEXEC;
    if (st.size > EXEC_VFS_BLOB_MAX) return -(int64_t)OSNOS_E2BIG;

    size_t blob_size = (size_t)st.size;
    uint8_t *blob = (uint8_t *)kmalloc(blob_size);
    if (!blob) return -(int64_t)OSNOS_ENOMEM;

    size_t got = 0;
    s = vfs_read(path, (char *)blob, blob_size, &got);
    if (s != OSNOS_OK || got != blob_size) {
        kfree(blob);
        return -(int64_t)(s != OSNOS_OK ? s : OSNOS_EIO);
    }

    /* Derive a short display name from the basename. */
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }

    int64_t pid = task_create_user_elf(base, blob, blob_size, args, envp);
    kfree(blob);
    return pid;
}

/* Legacy wrapper — proc_exec is now proc_execve with an empty
 * environment. Callers that want PATH/HOME/PWD should switch to
 * proc_execve and pass their own envp. */
int64_t proc_exec(const char *path, const char *args) {
    return proc_execve(path, args, 0);
}

/*
 * proc_execve_redir — same as proc_execve, plus arranges that the
 * spawned task's sys_read(0) / sys_write(1) talk to files instead
 * of the TTY/console.
 *
 * Strategy: spawn via the existing proc_execve, then bolt the
 * redirect paths onto the child's task struct before it actually
 * runs. Since proc_execve returns immediately with the child in
 * READY state, we can mutate task_by_pid(pid) safely.
 */
int64_t proc_execve_pipe(const char *left_path,  const char *left_args,
                          const char *right_path, const char *right_args,
                          const char *const *envp,
                          int64_t *left_pid_out)
{
    if (left_pid_out) *left_pid_out = -1;

    pipe_t *pipe = pipe_create();
    if (!pipe) return -(int64_t)OSNOS_ENOMEM;
    /* pipe_create starts with ref_w=ref_r=1 — those references
     * belong to the two tasks we're about to spawn. Each task's
     * exit handler drops the relevant side. */

    /* Spawn LEFT — writer. */
    int64_t lpid = proc_execve(left_path, left_args, envp);
    if (lpid < 0) {
        /* Pipe never attached; drop both refs to free the slot. */
        pipe_close_writer(pipe);
        pipe_close_reader(pipe);
        return lpid;
    }
    task_t *lt = task_by_pid((uint64_t)lpid);
    if (lt) lt->pipe_out = pipe;

    /* Spawn RIGHT — reader. */
    int64_t rpid = proc_execve(right_path, right_args, envp);
    if (rpid < 0) {
        /* Tear down left: it'll discover EPIPE when it tries to
         * write, or just exit normally if it doesn't write much.
         * Either way we can't run the pipeline. Free our refs. */
        if (lt) {
            lt->pipe_out = 0;
            lt->kill_pending = 1;
        }
        pipe_close_writer(pipe);
        pipe_close_reader(pipe);
        return rpid;
    }
    task_t *rt = task_by_pid((uint64_t)rpid);
    if (rt) rt->pipe_in = pipe;

    if (left_pid_out) *left_pid_out = lpid;
    return rpid;
}

int64_t proc_execve_redir(const char *path, const char *args,
                           const char *const *envp,
                           const char *stdin_path,
                           const char *stdout_path,
                           int stdout_append)
{
    /* Pre-create / truncate the stdout file. POSIX `>` truncates,
     * `>>` leaves existing content; either way the file must exist
     * before the child starts writing. */
    if (stdout_path && stdout_path[0]) {
        vfs_stat_t st;
        bool exists = (vfs_stat(stdout_path, &st) == OSNOS_OK);
        if (!exists || !stdout_append) {
            osnos_status_t s = vfs_write(stdout_path, "", 0);
            if (s != OSNOS_OK) return -(int64_t)s;
        }
    }
    /* For stdin: just verify the file is readable; sys_read does the
     * actual pull. Catches `cmd < nope.txt` cleanly. */
    if (stdin_path && stdin_path[0]) {
        vfs_stat_t st;
        if (vfs_stat(stdin_path, &st) != OSNOS_OK) {
            return -(int64_t)OSNOS_ENOENT;
        }
    }

    int64_t pid = proc_execve(path, args, envp);
    if (pid < 0) return pid;

    task_t *t = task_by_pid((uint64_t)pid);
    if (!t) return pid;

    if (stdout_path && stdout_path[0]) {
        os_strlcpy(t->stdout_redir, stdout_path, OSNOS_PATH_MAX);
        t->stdout_append    = stdout_append;
        t->stdout_redir_off = 0;
    }
    if (stdin_path && stdin_path[0]) {
        os_strlcpy(t->stdin_redir, stdin_path, OSNOS_PATH_MAX);
        t->stdin_redir_off = 0;
    }
    return pid;
}
