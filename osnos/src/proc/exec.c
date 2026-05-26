#include "exec.h"

#include "../fs/vfs.h"
#include "../include/osnos_fcntl.h"
#include "../include/osnos_status.h"
#include "../lib/string.h"
#include "../micro/fd.h"
#include "../micro/fpu.h"
#include "../micro/gdt.h"
#include "../micro/syscall.h"
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
/* Layout MUST match sigframe_t in src/micro/syscall.c (used by
 * sys_rt_sigreturn to restore). Total 160 bytes (5 iret + 15 GPRs). */
typedef struct {
    uint64_t rip, cs, rflags, rsp, ss;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8,  r9,  r10, r11, r12, r13, r14, r15;
} signal_frame_t;

/* Copy `n` bytes into another task's user-virtual address via its
 * pml4 (we are running on kernel pml4 or a different task's). Walks
 * vmm_lookup page-by-page so a sigframe straddling a page boundary
 * still lands correctly. Returns 1 on success, 0 if any page is
 * unmapped (caller drops the signal silently). */
static int write_other_user(uint64_t *pml4, uint64_t user_va,
                             const void *src, size_t n) {
    const uint8_t *s = (const uint8_t *)src;
    while (n > 0) {
        uint64_t page = user_va & ~0xFFFULL;
        uint64_t phys = vmm_lookup(pml4, page);
        if (!phys) return 0;
        size_t in_page = 0x1000 - (user_va & 0xFFFu);
        size_t take    = (n < in_page) ? n : in_page;
        uint8_t *dst = (uint8_t *)((phys & 0xFFFFFFFFFFFFF000ULL) +
                                    (user_va & 0xFFFu) +
                                    pmm_hhdm_offset());
        for (size_t i = 0; i < take; i++) dst[i] = s[i];
        s        += take;
        user_va  += take;
        n        -= take;
    }
    return 1;
}

/* Lowest set bit (1-based). Returns 0 if mask == 0. */
static int lowest_signal(uint32_t mask) {
    for (int s = 1; s <= 31; s++) {
        if (mask & (1u << (s - 1))) return s;
    }
    return 0;
}

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

    /* Signal delivery: if a signal is pending, redirect the iretq to
     * the user handler (with a sigframe pushed on the user stack and
     * the trampoline epilogue as return address). Done BEFORE the
     * CR3 switch so we can still walk the kernel pml4 to discover the
     * target task's pages via vmm_lookup. */
    while (t->sig_pending) {
        int sig = lowest_signal(t->sig_pending);
        if (sig == 0) break;
        uint64_t handler  = t->sa_handler [sig - 1];
        uint64_t restorer = t->sa_restorer[sig - 1];

        /* SIG_IGN — clear bit and re-check for another signal. */
        if (handler == 1) {
            t->sig_pending &= ~(1u << (sig - 1));
            continue;
        }

        /* SIG_DFL — default disposition. */
        if (handler == 0) {
            t->sig_pending &= ~(1u << (sig - 1));
            /* Ignore by default: SIGCHLD, SIGURG, SIGWINCH. */
            if (sig == 17 /* SIGCHLD */ ||
                sig == 23 /* SIGURG  */ ||
                sig == 28 /* SIGWINCH */) {
                continue;
            }
            /* Stop by default: SIGSTOP / SIGTSTP / SIGTTIN / SIGTTOU.
             * POSIX job control — task transitions to TASK_STOPPED,
             * wait_change set so the parent's waitpid(WUNTRACED)
             * picks it up. We sched_resume_jump (no iretq) so the
             * task doesn't reach ring 3 until SIGCONT / sys_resume
             * un-stops it. */
            if (sig == 19 /* SIGSTOP */ || sig == 20 /* SIGTSTP */ ||
                sig == 21 /* SIGTTIN */ || sig == 22 /* SIGTTOU */) {
                t->state       = TASK_STOPPED;
                t->wait_change = 1 /* WAIT_STOPPED */;
                notify_parent_stop_continue(t);
                sched_resume_jump();
                __builtin_unreachable();
            }
            /* SIGCONT default: just clear; the un-stop happens when
             * sys_kill sees SIGCONT on a STOPPED task (see sys_kill).
             * If we got here, the task was already running — no-op. */
            if (sig == 18 /* SIGCONT */) {
                continue;
            }
            /* fall-through: terminate. Restore the kernel pml4 first
             * because proc_exit_current_user expects to be running
             * on a sane address space (it'll destroy t->pml4). */
            proc_exit_current_user(128 + sig);
            __builtin_unreachable();
        }

        /* User handler: build sigframe on the user stack, redirect.
         *
         * Stack layout (grows DOWN, addresses ↓):
         *   [high]  orig_rsp
         *           siginfo_t (128 B)        ← siginfo_va, pasada en rsi
         *           signal_frame_t (160 B)   ← sigframe_base
         *           restorer addr            ← new RSP (8-mod-16)
         *   [low ]  ...handler stack grows here, NO clobbera siginfo
         *
         * siginfo va ARRIBA del sigframe (no abajo del restorer) para
         * que el handler que use red-zone o crezca su frame no la pise.
         *
         * sys_rt_sigreturn no necesita cambios: cuando el handler hace
         * ret, rsp = restorer_slot + 8 = sigframe_base. La trampolina
         * hace syscall con ese rsp y sigreturn lee el sigframe desde
         * ahí (sin saber del siginfo de más arriba). */
        uint64_t orig_rsp      = buf[1];
        uint64_t siginfo_va    = (orig_rsp - 128) & ~15ULL;
        uint64_t sigframe_base = (siginfo_va - sizeof(signal_frame_t)) & ~15ULL;
        uint64_t restorer_slot = sigframe_base - 8;

        signal_frame_t f;
        f.rip = buf[4]; f.cs  = buf[3]; f.rflags = buf[2];
        f.rsp = buf[1]; f.ss  = buf[0];
        f.rax = buf[5]; f.rbx = buf[6]; f.rcx = buf[7];  f.rdx = buf[8];
        f.rsi = buf[9]; f.rdi = buf[10]; f.rbp = buf[11];
        f.r8  = buf[12]; f.r9 = buf[13]; f.r10 = buf[14]; f.r11 = buf[15];
        f.r12 = buf[16]; f.r13 = buf[17]; f.r14 = buf[18]; f.r15 = buf[19];

        /* Construir siginfo_t mínima. Layout musl x86_64: si_signo,
         * si_errno, si_code, pad, then union. Populate solo lo crítico:
         * si_signo (handler usa para identificar) + si_code (SI_USER=0
         * para kill, SI_KERNEL=128 para tty/fault) + zeros. */
        uint8_t siginfo_buf[128];
        for (int i = 0; i < 128; i++) siginfo_buf[i] = 0;
        /* musl: int si_signo, si_errno, si_code (3 × 4 bytes = 12). */
        *(int *)&siginfo_buf[0]  = sig;
        *(int *)&siginfo_buf[4]  = 0;          /* si_errno */
        *(int *)&siginfo_buf[8]  = 0;          /* si_code = SI_USER */

        if (!write_other_user(t->pml4, sigframe_base, &f, sizeof(f)) ||
            !write_other_user(t->pml4, siginfo_va, siginfo_buf, 128) ||
            !write_other_user(t->pml4, restorer_slot, &restorer, 8)) {
            /* User stack unmapped at the spot we'd push the frame —
             * drop the signal silently. */
            t->sig_pending &= ~(1u << (sig - 1));
            continue;
        }

        /* Reroute iretq: rip=handler, rsp=top of restorer slot,
         * rdi=signum, rsi=siginfo_va (SA_SIGINFO arg2), rdx=NULL
         * (ucontext_t no implementado — handlers que lo usen para
         * leer registros viejos no van a funcionar, pero ese es un
         * subset chico; la mayoría solo lee siginfo). */
        buf[4]  = handler;            /* rip */
        buf[1]  = restorer_slot;      /* rsp */
        buf[10] = (uint64_t)sig;      /* rdi */
        buf[9]  = siginfo_va;         /* rsi */
        buf[8]  = 0;                  /* rdx = NULL ucontext_t */

        t->sig_pending &= ~(1u << (sig - 1));
        break;                    /* one signal per dispatch; rest queued */
    }

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
        /* Translate to the actual pending signal: kill_pending is
         * set in tandem with sig_pending for SIGINT/SIGTERM/SIGKILL
         * (see sys_kill / tty_signal). The first bit set in
         * sig_pending tells us the real signum.
         *
         * Excepción crítica: si la app INSTALÓ un user handler
         * (sa_handler != SIG_DFL && != SIG_IGN), NO force-killeamos
         * — bajamos al código de signal delivery más abajo que va
         * a invocar el handler. Apps como lighttpd registran SIGINT
         * para hacer graceful shutdown; sin esta excepción Ctrl+C
         * los mataba sin darles chance de cerrar sockets, flush
         * logs, etc. SIGKILL (9) sigue siendo uncatchable. */
        int sig = 2 /* SIGINT default */;
        if (t->sig_pending) {
            for (int s = 1; s <= 31; s++) {
                if (t->sig_pending & (1u << (s - 1))) { sig = s; break; }
            }
        }
        uint64_t handler = t->sa_handler[sig - 1];
        int catchable    = (sig != 9 /* SIGKILL */);
        if (catchable && handler != 0 /* SIG_DFL */ && handler != 1 /* SIG_IGN */) {
            /* Limpiar kill_pending — dejamos el sig_pending bit
             * para que el lazo siguiente lo entregue al handler. */
            t->kill_pending = 0;
            /* fall through al signal delivery loop abajo */
        } else {
            t->kill_pending = 0;
            t->sig_pending &= ~(1u << (sig - 1));
            proc_exit_current_user(128 + sig);
            __builtin_unreachable();
        }
    }

    /*
     * Ctrl+Z arrived while this task was running. Mark it
     * STOPPED so the scheduler skips it until SIGCONT (fg / bg
     * shell cmd) flips the state back to READY. The task's
     * saved_iret_* still hold the preempt point, so resume goes
     * straight into user_task_resume on next dispatch.
     */
    if (t->stop_pending) {
        t->stop_pending = 0;
        t->state        = TASK_STOPPED;
        t->wait_change  = 1 /* WAIT_STOPPED */;
        notify_parent_stop_continue(t);
        sched_resume_jump();
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
/* Forward decl — implementación más abajo. */
static uint64_t build_argv_block_tokens(uint64_t *pml4,
                                         uint64_t user_stack_top,
                                         const char **tokens,
                                         const size_t *token_lens,
                                         int argc,
                                         const char *const *envp,
                                         const elf_load_result_t *aux_info);

/* build_argv_block_argv — variante que toma argv como ARRAY pre-parsed.
 * Necesario para sys_execve (Linux argv[]): preserva boundaries que
 * la build_argv_block string-version destruye al re-tokenizar.
 * `argv[0]` se respeta tal como viene; no se sustituye por basename. */
static uint64_t build_argv_block_argv(uint64_t   *pml4,
                                      uint64_t    user_stack_top,
                                      const char *const *argv,
                                      const char *const *envp)
{
    const char *tokens[MAX_ARGV];
    size_t      token_lens[MAX_ARGV];
    int         argc = 0;
    if (argv) {
        while (argv[argc] && argc < MAX_ARGV) {
            tokens[argc]     = argv[argc];
            token_lens[argc] = os_strlen(argv[argc]);
            argc++;
        }
    }
    return build_argv_block_tokens(pml4, user_stack_top, tokens, token_lens, argc, envp, 0);
}

/* Variante con info ELF para dynamic linking: emite auxv completo
 * (AT_PHDR, AT_PHENT, AT_PHNUM, AT_BASE, AT_ENTRY, AT_RANDOM) que
 * ld-musl.so necesita parsear al arrancar. Sin esto la interp sale
 * con error temprano. */
static uint64_t build_argv_block_argv_dyn(uint64_t   *pml4,
                                          uint64_t    user_stack_top,
                                          const char *const *argv,
                                          const char *const *envp,
                                          const elf_load_result_t *aux_info)
{
    const char *tokens[MAX_ARGV];
    size_t      token_lens[MAX_ARGV];
    int         argc = 0;
    if (argv) {
        while (argv[argc] && argc < MAX_ARGV) {
            tokens[argc]     = argv[argc];
            token_lens[argc] = os_strlen(argv[argc]);
            argc++;
        }
    }
    return build_argv_block_tokens(pml4, user_stack_top, tokens, token_lens, argc, envp, aux_info);
}

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
     * single token whose body keeps spaces. Inside double quotes,
     * `\"` and `\\` are recognised as backslash escapes (so the
     * literal `"` and `\` can be embedded). Single quotes are raw
     * — no escape mechanism (matches POSIX shell semantics).
     *
     * Tokens are decoded IN PLACE into a small kernel-side scratch
     * buffer (the per-task arg string came from copy_from_user so
     * it's writable kernel memory). The output `tokens[i]` and
     * `token_lens[i]` point into that buffer.
     */
    static char decoded[ARGV_BLOCK_MAX];
    size_t dec = 0;

    const char *p = args ? args : "";
    while (*p && argc < MAX_ARGV) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char quote = 0;
        if (*p == '"' || *p == '\'') { quote = *p; p++; }

        size_t tok_start = dec;
        if (quote == '"') {
            while (*p && *p != quote && dec < sizeof(decoded)) {
                if (*p == '\\' && p[1] && (p[1] == '"' || p[1] == '\\')) {
                    decoded[dec++] = p[1];
                    p += 2;
                } else {
                    decoded[dec++] = *p++;
                }
            }
            if (*p == quote) p++;
        } else if (quote == '\'') {
            while (*p && *p != quote && dec < sizeof(decoded)) {
                decoded[dec++] = *p++;
            }
            if (*p == quote) p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && dec < sizeof(decoded)) {
                decoded[dec++] = *p++;
            }
        }
        tokens[argc]     = &decoded[tok_start];
        token_lens[argc] = dec - tok_start;
        argc++;
    }

    return build_argv_block_tokens(pml4, user_stack_top, tokens, token_lens, argc, envp, 0);
}

/* build_argv_block_tokens — código común post-tokenizado. Recibe
 * arrays paralelos de strings + sus longitudes; serializa al stack
 * SysV-init (argc | argv[] | envp[] | aux | strings).
 *
 * `aux_info` opcional: si != NULL, emite auxv full (AT_PHDR/PHENT/
 * PHNUM/BASE/ENTRY/RANDOM) que ld-musl.so requiere. Sino, auxv
 * mínimo (solo AT_PAGESZ + terminator) — basta para static binaries.
 */
static uint64_t build_argv_block_tokens(uint64_t *pml4,
                                         uint64_t user_stack_top,
                                         const char **tokens,
                                         const size_t *token_lens,
                                         int argc,
                                         const char *const *envp,
                                         const elf_load_result_t *aux_info)
{
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

    /* Total bytes for ALL strings (argv + envp), each NUL-terminated.
     * Si vamos a emitir AT_RANDOM, sumamos 16 bytes para el blob de
     * "random" (necesario para musl stack canary; ponemos ceros + el
     * pid para que no sea idéntico entre tasks pero tampoco predecible
     * trivialmente). Lo plantamos en el área de strings. */
    size_t strings_total = 0;
    for (int i = 0; i < argc; i++) strings_total += token_lens[i] + 1;
    for (int i = 0; i < envc; i++) strings_total += env_lens[i] + 1;
    size_t random_offset = 0;
    if (aux_info) {
        /* Reserve 16 bytes for AT_RANDOM, 8-byte-aligned. */
        strings_total = (strings_total + 7) & ~(size_t)7;
        random_offset = strings_total;
        strings_total += 16;
    }

    /* AUX entries count: mínimo = 2 (AT_PAGESZ + AT_NULL).
     * Full   = 8 (AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_BASE,
     *             AT_ENTRY, AT_RANDOM, AT_NULL). */
    int aux_pairs = aux_info ? 8 : 2;
    size_t aux_bytes = (size_t)aux_pairs * 2 * 8;

    /* Stay well inside the 4 KiB user stack page. */
    size_t block_size =
        8 /* argc */ +
        8 * (size_t)(argc + 1) /* argv + NULL */ +
        8 * (size_t)(envc + 1) /* envp + NULL */ +
        aux_bytes +
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

    /* Pointer area layout (top → bottom):
     *   [strings_base]
     *   [auxv...]
     *   [envp_null]
     *   [envp[envc-1..0]]
     *   [argv_null]
     *   [argv[argc-1..0]]
     *   [argc]                ← argc_virt = RSP at entry
     */
    uint64_t auxv_end_virt   = strings_base_virt;
    uint64_t auxv_start_virt = auxv_end_virt - aux_bytes;
    uint64_t envp_null_virt  = auxv_start_virt   - 8;
    uint64_t envp0_virt      = envp_null_virt    - 8 * (uint64_t)envc;
    uint64_t argv_null_virt  = envp0_virt        - 8;
    uint64_t argv0_virt      = argv_null_virt    - 8 * (uint64_t)argc;
    uint64_t argc_virt       = argv0_virt        - 8;

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
    /* AT_RANDOM blob (16 bytes). Plantamos un patrón fijo + pid; no es
     * seguro criptográficamente pero alcanza para musl stack canary
     * (que solo verifica que NO sea cero entre llamadas). */
    uint64_t random_user_va = 0;
    if (aux_info) {
        random_user_va = strings_base_virt + random_offset;
        size_t off = random_user_va - page_virt;
        for (int b = 0; b < 16; b++) {
            page[off + b] = (uint8_t)(0xa5 ^ b);
        }
    }

    /* Write the auxv pairs (low → high addresses, so we can iterate). */
    uint64_t *aux_kva = (uint64_t *)(page + (auxv_start_virt - page_virt));
    int ai = 0;
    if (aux_info) {
        aux_kva[ai*2 + 0] = 3;  aux_kva[ai*2 + 1] = aux_info->phdr_user_va; ai++;     /* AT_PHDR */
        aux_kva[ai*2 + 0] = 4;  aux_kva[ai*2 + 1] = aux_info->phentsize;    ai++;     /* AT_PHENT */
        aux_kva[ai*2 + 0] = 5;  aux_kva[ai*2 + 1] = aux_info->phnum;        ai++;     /* AT_PHNUM */
        aux_kva[ai*2 + 0] = 6;  aux_kva[ai*2 + 1] = 4096;                   ai++;     /* AT_PAGESZ */
        aux_kva[ai*2 + 0] = 7;  aux_kva[ai*2 + 1] = aux_info->interp_base;  ai++;     /* AT_BASE */
        aux_kva[ai*2 + 0] = 9;  aux_kva[ai*2 + 1] = aux_info->entry;        ai++;     /* AT_ENTRY */
        aux_kva[ai*2 + 0] = 25; aux_kva[ai*2 + 1] = random_user_va;         ai++;     /* AT_RANDOM */
        aux_kva[ai*2 + 0] = 0;  aux_kva[ai*2 + 1] = 0;                      ai++;     /* AT_NULL */
    } else {
        aux_kva[ai*2 + 0] = 6;  aux_kva[ai*2 + 1] = 4096;                   ai++;     /* AT_PAGESZ */
        aux_kva[ai*2 + 0] = 0;  aux_kva[ai*2 + 1] = 0;                      ai++;     /* AT_NULL */
    }
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
    /* Reset MSR_FS_BASE snapshot — el nuevo address space no tiene
     * la TLS del parent, y musl __init_libc va a hacer arch_prctl
     * temprano en el startup. Sin esto, el child hereda el fs_base
     * del parent (apuntando a TLS válida en pml4 del parent pero
     * inválida en el pml4 nuevo del child) → page fault inmediato
     * al primer acceso a errno via %fs:offset. */
    t->fs_base           = 0;
    /* Reset TTY termios a defaults (canonical mode + ECHO). Esto
     * es el "exec boundary" análogo al de Linux donde cada nuevo
     * proceso hereda un terminal con echo+canonical por default.
     * Sin esto, si el shell padre setea raw+noecho para su line
     * editor, el child REPL (sqlite3, lua, etc.) hereda raw y
     * usuario no ve lo que tipea. Programs que necesitan raw
     * (vi, less, ash mismo) call tcsetattr explícitamente. */
    extern void tty_reset_to_defaults(void);
    tty_reset_to_defaults();

    /* POSIX job-control defaults: a freshly-spawned top-level task
     * is its own session leader of its own one-task process group.
     * fork(2) overwrites both with the parent's pgid/sid (POSIX
     * inheritance); see sys_fork. */
    t->pgid              = t->pid;
    t->sid               = t->pid;

    /* Parent linkage: if there's a current user-mode caller (i.e.
     * proc_execve was invoked from sys_spawn / from a shell
     * builtin via osn_spawn), point the new task at it so
     * getppid(2) returns the spawner. Kernel-spawned tasks
     * (kmain → consrv/kbdsrv/shellsrv via proc_execve with no
     * caller) get parent_pid = 0 ("orphan" from POSIX POV; the
     * reaper handles them directly).
     *
     * sys_fork(2) overwrites this anyway with the actual forker
     * pid, so we don't conflict with the fork code path. */
    {
        task_t *caller = task_current();
        t->parent_pid = (caller && caller->pml4) ? caller->pid : 0;
    }

    /* Seed FPU state — first FXRSTOR on this task's first dispatch
     * needs sane bytes, not whatever the BSS happens to hold. */
    fpu_state_init(t->fpu_state);

    /* Seed cwd:
     *   - Si el task ya tiene cwd (caso normal: viene de fork+exec),
     *     PRESERVAR. POSIX dice cwd se hereda a través de exec.
     *   - Si está vacío (spawn directo desde kernel), tomar PWD del
     *     envp como hint, o "/" como fallback.
     *
     * Antes resetábamos siempre a "/" y reconstruíamos desde PWD —
     * eso rompía `cd /home; make hello` porque ash no exporta PWD al
     * envp del child de manera consistente. */
    if (!t->cwd[0]) {
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

    /* Drop every pending IPC addressed to this pid. Otherwise events
     * queued by oxsrv (mouse moves, key presses, close events) for an
     * app that's exiting stay stuck in the 64-slot queue forever. After
     * a handful of open/close cycles the queue saturates and ipc_send
     * starts returning EAGAIN — observable as "gets slow after opening
     * and closing an app". */
    ipc_drop_for_pid(t->pid);

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
    /* Sweep fd 0..MAX so pipe ends attached to stdin/stdout (via
     * proc_execve_pipeline) also get released, not just regular
     * fds 3+. Under OFD, fd_free does the backend cleanup
     * automatically when the refcount hits 0 — no need to call
     * pipe_close_* / sock_close manually here. */
    for (int fd = 0; fd < OSNOS_MAX_FDS; fd++) {
        if (!t->fds[fd].used) continue;
        if (fd >= 3) {
            fd_free(t, fd);
        } else {
            /* stdin/stdout/stderr: release their OFD too so the
             * is_special default (or pipe/file the shell wired in)
             * doesn't leak. */
            int idx = t->fds[fd].ofd_idx;
            t->fds[fd].used     = false;
            t->fds[fd].ofd_idx  = -1;
            t->fds[fd].fd_flags = 0;
            if (idx >= 0) ofd_unref(idx);
        }
    }

    /* mmap regions ride on the user pml4 we destroy below, so the
     * physical pages would already get freed by address_space_destroy.
     * Still, zero out the bookkeeping so a slot recycler sees a clean
     * task struct. */
    for (int i = 0; i < TASK_MMAP_MAX; i++) {
        t->mmap_regions[i].addr = 0;
        t->mmap_regions[i].len  = 0;
    }
    t->mmap_next = 0;

    /*
     * State transition (POSIX zombie model):
     *
     *   parent alive + ring-3        →  TASK_ZOMBIE
     *      Slot stays around with exit_code visible until the parent
     *      calls waitpid(2). At that point sys_wait4 transitions the
     *      slot to TASK_DEAD and the reaper collects it.
     *
     *   no parent (orphan/init/kernel) →  TASK_DEAD
     *      Reaper collects directly. (Without an `init` process we
     *      can't reparent, so a dying parent's children just lose
     *      their wait waiter — they become orphans on exit.)
     */
    task_t *parent_t = task_by_pid(t->parent_pid);
    int has_live_parent =
        (t->parent_pid != 0) &&
        (parent_t != 0) &&
        (parent_t->pml4 != 0) &&
        (parent_t->state != TASK_DEAD) &&
        (parent_t->state != TASK_ZOMBIE) &&
        (parent_t->state != TASK_UNUSED);

    t->state             = has_live_parent ? TASK_ZOMBIE : TASK_DEAD;
    t->pml4              = 0;
    t->kernel_stack_top  = 0;
    t->kernel_stack_base = 0;
    t->exit_code         = exit_code;

    /*
     * SIGCHLD delivery to live parent. POSIX: when a child changes
     * state (exited, killed, stopped, continued), the parent gets
     * SIGCHLD. Default disposition for SIGCHLD is "ignore" — user_
     * task_resume's SIG_DFL branch already skips SIGCHLD without
     * killing the parent — so this is a no-op for programs that
     * don't install a handler. Programs that DO install one (typical
     * `signal(SIGCHLD, on_child)`) get woken up at the next iretq.
     *
     * Bit 16 = signal 17 (SIGCHLD).
     */
    if (has_live_parent) {
        parent_t->sig_pending |= 1u << (17 - 1);
    }

    /* If the dying task held the TTY foreground, clear it. Otherwise
     * the next Ctrl+C would chase a ghost pid and be silently lost —
     * the failure mode after `exec /bin/top` exits and shellsrv
     * respawns: the new shellsrv has a different pid, but
     * kernel_fg_pid still points to dead top. */
    extern uint64_t kernel_fg_pid;
    if (kernel_fg_pid == t->pid) kernel_fg_pid = 0;

    /*
     * If the parent is BLOCKED inside sys_wait4(2) waiting for ANY
     * child (-1) or specifically for us (== t->pid), wake it now.
     * Write the encoded status into *parent->wait_status_ptr via
     * the parent's pml4, set saved_rax = our pid so wait4 returns
     * the right value, and transition the parent → READY. The
     * waited-on child stays in ZOMBIE until the parent's syscall
     * reaping path flips it to DEAD.
     */
    if (has_live_parent &&
        parent_t->state == TASK_BLOCKED &&
        (parent_t->waiting_for_pid == -1 ||
         parent_t->waiting_for_pid == (int)t->pid))
    {
        /* Encode status (matches sys_wait4's encode_wait_status):
         *   exit_code 128..159 = signal-terminated → status = sig & 0x7f
         *      (WIFSIGNALED true, WTERMSIG returns sig)
         *   else                = normal exit       → status = (code<<8)
         *      (WIFEXITED true, WEXITSTATUS returns code)
         * Without this branch SIGTERM-killed children showed up as
         * exit=143 with WIFEXITED=true — wrong by POSIX. */
        int status_value;
        if (exit_code >= 128 && exit_code <= 128 + 31) {
            status_value = (exit_code - 128) & 0x7f;
        } else {
            status_value = (exit_code & 0xff) << 8;
        }

        if (parent_t->wait_status_ptr) {
            uint64_t va = (uint64_t)parent_t->wait_status_ptr;
            uint64_t phys = vmm_lookup(parent_t->pml4, va & ~0xFFFULL);
            if (phys) {
                int *kva = (int *)((phys & PTE_ADDR_MASK) +
                                    (va & 0xFFFu) +
                                    pmm_hhdm_offset());
                *kva = status_value;
            }
            /* If unmapped, silently drop the status — parent can't
             * have given us a valid pointer; nothing better we can
             * do without rolling the syscall back. */
        }

        parent_t->saved_rax        = t->pid;        /* wait4 return value */
        parent_t->waiting_for_pid  = 0;
        parent_t->wait_options     = 0;
        parent_t->wait_status_ptr  = 0;
        parent_t->state            = TASK_READY;

        /* We just delivered this child's status to the parent — the
         * waitpid syscall is going to return here. Transition the
         * child slot from ZOMBIE → DEAD so a follow-up
         * waitpid(WNOHANG) doesn't re-find it (POSIX: ECHILD when
         * nothing left to wait for). Reaper will recycle the slot
         * on its next pass. */
        t->state = TASK_DEAD;
    }

    uint64_t kpml4_phys =
        (uint64_t)vmm_kernel_pml4() - pmm_hhdm_offset();
    __asm__ volatile ("mov %0, %%cr3" :: "r"(kpml4_phys) : "memory");

    /* CLONE_VM: si todavía hay otro task vivo apuntando al mismo
     * pml4 (parent suspendido en vfork, o sibling clone), NO liberar
     * el AS — el último usuario en salir hará el destroy. */
    if (task_pml4_other_users(user_pml4, t->pid) == 0) {
        address_space_destroy(user_pml4);
    }

    /* CLONE_VFORK release: child salió sin execve. Despertar al
     * parent suspendido (clone() le devuelve mi pid igual). */
    if (t->vfork_waiter_pid) {
        task_t *waiter = task_by_pid(t->vfork_waiter_pid);
        if (waiter && waiter->state == TASK_BLOCKED) {
            waiter->saved_rax = t->pid;
            waiter->state     = TASK_READY;
        }
        t->vfork_waiter_pid = 0;
    }

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
 * minus headroom for argv/envp/per-task structures. SQLite is
 * ~5 MB linked against musl, BusyBox ~1.3 MB, TCC/Lua/jq ~1 MB.
 * 16 MiB cap leaves plenty of headroom para próximos vendor ports
 * (postgres-lite, perl, ruby) sin tener que tocar esto otra vez.
 */
#define EXEC_VFS_BLOB_MAX (16 * 1024 * 1024)

int64_t proc_execve(const char *path, const char *args,
                     const char *const *envp) {
    if (!path) return -(int64_t)OSNOS_EFAULT;

    stdin_clear();

    /* /bin: prefer the disk-resident copy when FAT is mounted (aliasfs
     * routes /bin → /sd/bin in that case). Only fall back to the
     * kernel-embedded blob if the disk file is missing — that way
     * the kernel acts as a recovery ROM for /bin entries the user
     * accidentally deleted, but day-to-day execution goes through
     * VFS exactly like a "real" UNIX. Diskless boots use the binfs
     * mount which still walks builtins from kernel memory. */
    if (os_strstarts(path, "/bin/")) {
        vfs_stat_t st;
        osnos_status_t s_check = vfs_stat(path, &st);
        int vfs_has_file = (s_check == OSNOS_OK && st.type == VFS_NODE_REG &&
                             st.size > 0);
        if (!vfs_has_file) {
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
        }
        /* Fall through to VFS path — loads /sd/bin/<name> from FAT. */
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

/* ---------------------------------------------------------------- */
/* proc_execve_replace — in-place ELF replacement (Linux execve)    */
/* ---------------------------------------------------------------- */

/*
 * Internal helper: resolve `path` to (blob, blob_size) honoring the
 * VFS-first / builtin-ROM-fallback rule used by proc_execve. On
 * success, *out_blob points at the bytes and *out_owned is set if
 * the caller must kfree(*out_blob) after consuming them.
 *
 * The split into a helper keeps proc_execve and proc_execve_replace
 * agreeing on lookup semantics without duplicating ~40 LOC.
 */
static osnos_status_t resolve_executable(const char *path,
                                          const uint8_t **out_blob,
                                          size_t *out_size,
                                          uint8_t **out_owned)
{
    *out_blob  = 0;
    *out_size  = 0;
    *out_owned = 0;

    if (os_strstarts(path, "/bin/")) {
        vfs_stat_t st;
        osnos_status_t s_check = vfs_stat(path, &st);
        int vfs_has_file = (s_check == OSNOS_OK && st.type == VFS_NODE_REG &&
                             st.size > 0);
        if (!vfs_has_file) {
            const char *bn = path + 5;
            const builtin_t *b = builtin_find(bn);
            if (b && b->elf_start) {
                *out_blob = b->elf_start;
                *out_size = (size_t)(b->elf_end - b->elf_start);
                return OSNOS_OK;
            }
            if (b) return OSNOS_ENOENT;   /* known name but no ELF (flat blob) */
        }
    }

    vfs_stat_t st;
    osnos_status_t s = vfs_stat(path, &st);
    if (s != OSNOS_OK)              return s;
    if (st.type == VFS_NODE_DIR)    return OSNOS_EISDIR;
    if (st.type != VFS_NODE_REG)    return OSNOS_ENOEXEC;
    if (st.size == 0)               return OSNOS_ENOEXEC;
    if (st.size > EXEC_VFS_BLOB_MAX) return OSNOS_E2BIG;

    size_t sz = (size_t)st.size;
    uint8_t *blob = (uint8_t *)kmalloc(sz);
    if (!blob) return OSNOS_ENOMEM;

    size_t got = 0;
    s = vfs_read(path, (char *)blob, sz, &got);
    if (s != OSNOS_OK || got != sz) {
        kfree(blob);
        return s != OSNOS_OK ? s : OSNOS_EIO;
    }

    *out_blob  = blob;
    *out_size  = sz;
    *out_owned = blob;
    return OSNOS_OK;
}

int64_t proc_execve_replace(const char *path, const char *args,
                              const char *const *envp)
{
    task_t *t = task_current();
    if (!t || !t->pml4) return -(int64_t)OSNOS_ESRCH;
    if (!path)          return -(int64_t)OSNOS_EFAULT;
    if (!args) args = "";

    /* 1. Resolve path → blob (VFS first, builtin ROM fallback). */
    const uint8_t *blob = 0;
    size_t blob_size = 0;
    uint8_t *blob_owned = 0;
    osnos_status_t s = resolve_executable(path, &blob, &blob_size, &blob_owned);
    if (s != OSNOS_OK) return -(int64_t)s;

    /* 2. Load into a SEPARATE new pml4. The old one stays live until
     *    everything below succeeds — execve must be all-or-nothing. */
    uint64_t *new_pml4 = 0;
    uint64_t  new_entry = 0;
    uint64_t  new_stack_top = 0;
    s = elf_load(blob, blob_size, &new_pml4, &new_entry, &new_stack_top);
    if (blob_owned) kfree(blob_owned);    /* blob copied into new_pml4 */
    if (s != OSNOS_OK) return -(int64_t)s;

    /* Derive basename for task name + as argv[0]. */
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }

    /* 3. Build the System V init block on the new stack. */
    uint64_t init_rsp = build_argv_block(new_pml4, new_stack_top, base, args, envp);
    if (init_rsp == 0) {
        address_space_destroy(new_pml4);
        return -(int64_t)OSNOS_E2BIG;
    }

    /* 4. SUCCESS — swap in the new image. POSIX execve preserves:
     *      pid, kernel_stack, fds[], cwd, kill_pending? (cleared),
     *      saved iret frame (cleared — fresh image starts at new_entry).
     *    Mmap regions, brk, stdin/stdout_redir all reset. */
    uint64_t *old_pml4 = t->pml4;
    t->pml4              = new_pml4;
    t->user_entry        = new_entry;
    t->user_stack_top    = init_rsp;
    t->heap_start        = USER_HEAP_BASE;
    t->heap_brk          = USER_HEAP_BASE;
    t->saved_valid       = 0;
    t->kill_pending      = 0;
    t->stop_pending      = 0;
    /* Reset TLS base — el address space cambió, fs_base del image
     * anterior no es válido en el nuevo. Mismo razonamiento que en
     * task_create_user_elf. musl __init_libc lo va a setear al
     * arrancar el nuevo programa via arch_prctl(ARCH_SET_FS). */
    t->fs_base           = 0;
    /* Reset TTY a canonical+echo — boundary entre shell (raw mode
     * para line editor) y el nuevo programa que generalmente quiere
     * echo de input. Ver comment en task_create_user_elf. */
    extern void tty_reset_to_defaults(void);
    tty_reset_to_defaults();
    t->mmap_next         = 0;
    for (int i = 0; i < TASK_MMAP_MAX; i++) {
        t->mmap_regions[i].addr = 0;
        t->mmap_regions[i].len  = 0;
    }
    t->stdin_redir [0]   = 0;
    t->stdout_redir[0]   = 0;
    t->stdout_append     = 0;
    t->stdin_redir_off   = 0;
    t->stdout_redir_off  = 0;

    /* FD_CLOEXEC: POSIX execve(2) closes any fd whose slot has
     * FD_CLOEXEC set. fd_free decrements the OFD's refcount, which
     * triggers backend cleanup (pipe_close / sock_close) if the OFD
     * has no other references. Sweep starts at 3 — stdin/stdout/
     * stderr can't be marked CLOEXEC in the usual sense (some libcs
     * forbid it; we just skip them for safety). */
    for (int fd = 3; fd < OSNOS_MAX_FDS; fd++) {
        if (!t->fds[fd].used) continue;
        if (t->fds[fd].fd_flags & OSNOS_FD_CLOEXEC) {
            fd_free(t, fd);
        }
    }

    /* POSIX execve(2): signals que estaban CAUGHT (handler ≠ DFL ≠ IGN)
     * se resetean a SIG_DFL en la nueva imagen — los punteros viejos
     * apuntan al text del binario anterior, que ya no está mapeado.
     * Mantener SIG_IGN (excepto SIGCHLD que en POSIX puede reset-
     * earse, pero por simplicidad y compat con Linux mantenemos).
     *
     * Sin esto, un fork+exec donde el parent tenía handlers (ej. ash
     * con job control) le pasa los punteros al child; al primer signal
     * el kernel iretq a memoria no mapeada → page fault. Confirmado
     * con `make hello`: SIGCHLD venía con handler=ash signal_handler
     * en busybox text. */
    for (int i = 0; i < 32; i++) {
        if (t->sa_handler[i] != 1 /* SIG_IGN */) {
            t->sa_handler [i] = 0;   /* SIG_DFL */
            t->sa_restorer[i] = 0;
        }
    }

    /* Refresh task name from the new image's basename. */
    size_t bn = 0;
    while (base[bn] && bn + 1 < OSNOS_TASK_NAME_MAX) {
        t->name[bn] = base[bn];
        bn++;
    }
    t->name[bn] = 0;

    fpu_state_init(t->fpu_state);

    /* 5. Free the abandoned address space (si soy el último usuario).
     *    CLONE_VM: el parent suspendido en vfork sigue apuntando al
     *    mismo pml4 — no liberarlo, ese parent va a re-tomar control
     *    en la AS original cuando lo despertemos abajo. Kernel high-
     *    half stays valid in CR3; old user low-half se destruye sólo
     *    si nadie más lo referencia. */
    if (task_pml4_other_users(old_pml4, t->pid) == 0) {
        address_space_destroy(old_pml4);
    }
    /* execve corta CLONE_VM: el child arranca su propio AS limpio. */
    t->pml4_shared = 0;

    /* 5b. CLONE_VFORK release: si soy un vfork-child, despertar al
     *     parent ahora que ya cargué la nueva imagen y solté el AS
     *     compartido. */
    if (t->vfork_waiter_pid) {
        task_t *waiter = task_by_pid(t->vfork_waiter_pid);
        if (waiter && waiter->state == TASK_BLOCKED) {
            waiter->saved_rax = t->pid;   /* clone() devuelve pid al parent */
            waiter->state     = TASK_READY;
        }
        t->vfork_waiter_pid = 0;
    }

    /* 6. Long-jump back to scheduler. Next dispatch enters
     *    user_task_trampoline → saved_valid=0 path → iretq with the
     *    new RIP/RSP. The current syscall stack frame is abandoned;
     *    the per-task kstack base is reused on next dispatch. */
    sched_resume_jump();
    /* unreachable */
}

/* proc_execve_replace_argv — fork de proc_execve_replace que toma
 * argv[] como ARRAY en vez de string aplanado. Necesario para
 * sys_execve preserve boundaries (apps que pasan "echo HELLO" como
 * UN argv element no quieren que se rompa en dos).
 *
 * Implementación: copia-paste de proc_execve_replace con el path
 * único `build_argv_block_argv` en vez de `build_argv_block`. NO se
 * refactoreó en helper común para evitar tocar el old path estable. */
int64_t proc_execve_replace_argv(const char *path,
                                  const char *const *argv,
                                  const char *const *envp)
{
    task_t *t = task_current();
    if (!t || !t->pml4) return -(int64_t)OSNOS_ESRCH;
    if (!path)          return -(int64_t)OSNOS_EFAULT;

    const uint8_t *blob = 0;
    size_t blob_size = 0;
    uint8_t *blob_owned = 0;
    osnos_status_t s = resolve_executable(path, &blob, &blob_size, &blob_owned);
    if (s != OSNOS_OK) return -(int64_t)s;

    /* Detectar PT_INTERP en el blob principal. Si lo tiene, cargar
     * el interpreter (ld-musl.so) también y arrancar ahí — el
     * interpreter resuelve dynamic libs y luego salta al main. */
    char interp_path[OSNOS_PATH_MAX];
    uint8_t *interp_blob_owned = 0;
    const uint8_t *interp_blob = 0;
    size_t interp_size = 0;
    osnos_status_t igs = elf_get_interp(blob, blob_size, interp_path, sizeof(interp_path));
    if (igs == OSNOS_OK) {
        /* Leer interpreter del VFS. */
        vfs_stat_t ist;
        if (vfs_stat(interp_path, &ist) != OSNOS_OK || ist.type != VFS_NODE_REG ||
            ist.size == 0 || ist.size > EXEC_VFS_BLOB_MAX) {
            if (blob_owned) kfree(blob_owned);
            return -(int64_t)OSNOS_ENOENT;
        }
        interp_size = (size_t)ist.size;
        interp_blob_owned = (uint8_t *)kmalloc(interp_size);
        if (!interp_blob_owned) {
            if (blob_owned) kfree(blob_owned);
            return -(int64_t)OSNOS_ENOMEM;
        }
        size_t igot = 0;
        if (vfs_read(interp_path, (char *)interp_blob_owned, interp_size, &igot)
              != OSNOS_OK || igot != interp_size) {
            kfree(interp_blob_owned);
            if (blob_owned) kfree(blob_owned);
            return -(int64_t)OSNOS_EIO;
        }
        interp_blob = interp_blob_owned;
    }

    uint64_t *new_pml4 = 0;
    elf_load_result_t er = {0};
    if (interp_blob) {
        s = elf_load_dyn(blob, blob_size, interp_blob, interp_size, &new_pml4, &er);
    } else {
        /* Path estático tradicional — auxv mínimo. */
        s = elf_load(blob, blob_size, &new_pml4, &er.entry, &er.stack_top);
        er.start_entry = er.entry;
    }
    if (blob_owned)        kfree(blob_owned);
    if (interp_blob_owned) kfree(interp_blob_owned);
    if (s != OSNOS_OK) return -(int64_t)s;

    uint64_t new_entry      = er.start_entry;
    uint64_t new_stack_top  = er.stack_top;

    /* basename para task name. */
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }

    uint64_t init_rsp;
    if (interp_blob) {
        init_rsp = build_argv_block_argv_dyn(new_pml4, new_stack_top, argv, envp, &er);
    } else {
        init_rsp = build_argv_block_argv(new_pml4, new_stack_top, argv, envp);
    }
    if (init_rsp == 0) {
        address_space_destroy(new_pml4);
        return -(int64_t)OSNOS_E2BIG;
    }

    uint64_t *old_pml4 = t->pml4;
    t->pml4              = new_pml4;
    t->user_entry        = new_entry;
    t->user_stack_top    = init_rsp;
    t->heap_start        = USER_HEAP_BASE;
    t->heap_brk          = USER_HEAP_BASE;
    t->saved_valid       = 0;
    t->kill_pending      = 0;
    t->stop_pending      = 0;
    t->fs_base           = 0;
    extern void tty_reset_to_defaults(void);
    tty_reset_to_defaults();
    t->mmap_next         = 0;
    for (int i = 0; i < TASK_MMAP_MAX; i++) {
        t->mmap_regions[i].addr = 0;
        t->mmap_regions[i].len  = 0;
    }
    t->stdin_redir [0]   = 0;
    t->stdout_redir[0]   = 0;
    t->stdout_append     = 0;
    t->stdin_redir_off   = 0;
    t->stdout_redir_off  = 0;

    for (int fd = 3; fd < OSNOS_MAX_FDS; fd++) {
        if (!t->fds[fd].used) continue;
        if (t->fds[fd].fd_flags & OSNOS_FD_CLOEXEC) {
            fd_free(t, fd);
        }
    }

    /* POSIX execve(2): reset CAUGHT signal handlers a SIG_DFL (los
     * punteros apuntan al text del binario viejo). Mantener SIG_IGN. */
    for (int i = 0; i < 32; i++) {
        if (t->sa_handler[i] != 1) {
            t->sa_handler [i] = 0;
            t->sa_restorer[i] = 0;
        }
    }

    size_t bn = 0;
    while (base[bn] && bn + 1 < OSNOS_TASK_NAME_MAX) {
        t->name[bn] = base[bn];
        bn++;
    }
    t->name[bn] = 0;

    fpu_state_init(t->fpu_state);

    if (task_pml4_other_users(old_pml4, t->pid) == 0) {
        address_space_destroy(old_pml4);
    }
    t->pml4_shared = 0;

    if (t->vfork_waiter_pid) {
        task_t *waiter = task_by_pid(t->vfork_waiter_pid);
        if (waiter && waiter->state == TASK_BLOCKED) {
            waiter->saved_rax = t->pid;
            waiter->state     = TASK_READY;
        }
        t->vfork_waiter_pid = 0;
    }

    sched_resume_jump();
    /* unreachable */
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
int64_t proc_execve_pipeline(const char *const paths[],
                              const char *const args[],
                              int n_stages,
                              const char *const *envp,
                              int64_t pids_out[])
{
    if (n_stages < 2 || n_stages > MAX_PIPELINE_STAGES) {
        return -(int64_t)OSNOS_EINVAL;
    }

    /*
     * Allocate the N-1 pipes that sit between adjacent stages. If
     * the pool runs out partway through, close what we've created
     * and bail.
     */
    pipe_t *pipes[MAX_PIPELINE_STAGES - 1] = {0};
    int pipes_created = 0;
    for (int i = 0; i < n_stages - 1; i++) {
        pipes[i] = pipe_create();
        if (!pipes[i]) {
            for (int j = 0; j < pipes_created; j++) {
                pipe_close_writer(pipes[j]);
                pipe_close_reader(pipes[j]);
            }
            return -(int64_t)OSNOS_ENOMEM;
        }
        pipes_created++;
    }

    /*
     * Spawn each stage and wire its endpoints. proc_execve leaves the
     * new task READY but not yet dispatched, so we can safely mutate
     * its fd table before the scheduler picks it up. fd 0 = read end
     * of the pipe behind this stage (i > 0); fd 1 = write end of the
     * pipe ahead of this stage (i < n - 1). Both override the default
     * `is_special` stdin/stdout slots installed by fd_init_for_task.
     */
    int64_t pids[MAX_PIPELINE_STAGES];
    for (int i = 0; i < n_stages; i++) pids[i] = -1;

    for (int i = 0; i < n_stages; i++) {
        pids[i] = proc_execve(paths[i], args[i], envp);
        if (pids[i] < 0) {
            /* Partial failure: detach pipe ends from already-spawned
             * stages so their exit cleanup doesn't try to close pipes
             * that we're about to close ourselves, then mark them
             * to die and drain every pipe slot back to the pool. */
            /* Cleanup: release each already-spawned stage's stdin/
             * stdout slots (these point at pipe OFDs we're tearing
             * down). Under OFD model, freeing the slot decrements
             * the OFD refcount which triggers pipe_close once both
             * ends are released; here we explicitly close the pipe
             * objects too because they were never wired into any
             * stage that runs to completion. */
            for (int j = 0; j < i; j++) {
                task_t *st = task_by_pid((uint64_t)pids[j]);
                if (st) {
                    /* Drop the stdin/stdout slots we'd just attached
                     * to pipe OFDs. fd_free safely handles unused
                     * slots and idempotent unref. */
                    if (j > 0)
                        fd_free(st, OSNOS_FD_STDIN);
                    if (j < n_stages - 1)
                        fd_free(st, OSNOS_FD_STDOUT);
                    st->kill_pending = 1;
                }
            }
            for (int j = 0; j < pipes_created; j++) {
                pipe_close_writer(pipes[j]);
                pipe_close_reader(pipes[j]);
            }
            return pids[i];
        }
        task_t *t = task_by_pid((uint64_t)pids[i]);
        if (t) {
            /* Wire the pipe endpoints into the new stage's std slots
             * via fresh OFDs (allocated from the pool). Replace any
             * existing OFD on that slot (the is_special default that
             * task_create_user_elf seeded). */
            if (i > 0) {
                /* Stage i reads from pipes[i-1] (read-end). */
                if (t->fds[OSNOS_FD_STDIN].used) {
                    int old = t->fds[OSNOS_FD_STDIN].ofd_idx;
                    if (old >= 0) ofd_unref(old);
                }
                int idx = ofd_alloc();
                if (idx >= 0) {
                    osnos_ofd_t *o = ofd_get(idx);
                    o->is_pipe   = true;
                    o->pipe_ref  = pipes[i - 1];
                    o->pipe_side = 0;
                    o->flags     = O_RDONLY;
                    t->fds[OSNOS_FD_STDIN].used    = true;
                    t->fds[OSNOS_FD_STDIN].ofd_idx = idx;
                }
            }
            if (i < n_stages - 1) {
                /* Stage i writes to pipes[i] (write-end). */
                if (t->fds[OSNOS_FD_STDOUT].used) {
                    int old = t->fds[OSNOS_FD_STDOUT].ofd_idx;
                    if (old >= 0) ofd_unref(old);
                }
                int idx = ofd_alloc();
                if (idx >= 0) {
                    osnos_ofd_t *o = ofd_get(idx);
                    o->is_pipe   = true;
                    o->pipe_ref  = pipes[i];
                    o->pipe_side = 1;
                    o->flags     = O_WRONLY;
                    t->fds[OSNOS_FD_STDOUT].used    = true;
                    t->fds[OSNOS_FD_STDOUT].ofd_idx = idx;
                }
            }
        }
    }

    if (pids_out) {
        for (int i = 0; i < n_stages; i++) pids_out[i] = pids[i];
    }
    return pids[n_stages - 1];
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
