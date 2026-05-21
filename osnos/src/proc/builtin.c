#include "builtin.h"

#include "../lib/string.h"

/*
 * Registry of /bin entries. After FASE 7's tool migration, every
 * non-demo program is a libc-linked ELF (USERELF). The remaining
 * kernel-side or hand-asm flavours are kept as demos:
 *
 *   - flat user blobs (USER) — minimal ring-3 asm; useful to verify
 *     the syscall path end-to-end without involving libc / ELF
 *   - hello_elf (USERELF, bare crt0 inside the source) — proves the
 *     ELF loader doesn't depend on libc
 *
 * Everything else moved into tests/ as standalone libc programs.
 */

/* ---------------------------------------------------------------- */
/* Hand-asm flat user blobs                                          */
/* ---------------------------------------------------------------- */

/*
 * /bin/ring3hello — fast-path SYSCALL.
 *
 * Identical output to the int-0x80 variant below; difference is that
 * here the user code uses the SYSCALL instruction (LSTAR/STAR/FMASK
 * path) instead of the legacy interrupt gate. Both ABIs share the same
 * dispatcher and register layout.
 */
__asm__ (
    ".section .text\n"
    ".global user_hello_start\n"
    ".global user_hello_end\n"
    "user_hello_start:\n"
    "    movq $1, %rax\n"                /* sys_write */
    "    movq $1, %rdi\n"                /* fd = stdout */
    "    leaq user_hello_msg(%rip), %rsi\n"
    "    movq $17, %rdx\n"               /* len("hello from ring3\n") */
    "    syscall\n"
    "    movq $60, %rax\n"               /* sys_exit */
    "    movq $0, %rdi\n"
    "    syscall\n"
    "user_hello_msg:\n"
    "    .ascii \"hello from ring3\\n\"\n"
    "user_hello_end:\n"
);

extern const uint8_t user_hello_start[];
extern const uint8_t user_hello_end[];

/*
 * /bin/ring3int80 — legacy int-0x80 compat path.
 */
__asm__ (
    ".section .text\n"
    ".global user_int80_start\n"
    ".global user_int80_end\n"
    "user_int80_start:\n"
    "    movq $1, %rax\n"
    "    movq $1, %rdi\n"
    "    leaq user_int80_msg(%rip), %rsi\n"
    "    movq $19, %rdx\n"               /* len("hello via int 0x80\n") */
    "    int  $0x80\n"
    "    movq $60, %rax\n"
    "    movq $0, %rdi\n"
    "    int  $0x80\n"
    "user_int80_msg:\n"
    "    .ascii \"hello via int 0x80\\n\"\n"
    "user_int80_end:\n"
);

extern const uint8_t user_int80_start[];
extern const uint8_t user_int80_end[];

/*
 * /bin/ring3fault — deliberate ring-3 fault. Used by the test suite
 * to verify that the kernel kills the offending task and recovers.
 */
__asm__ (
    ".section .text\n"
    ".global user_fault_start\n"
    ".global user_fault_end\n"
    "user_fault_start:\n"
    "    movq $1, %rax\n"
    "    movq $1, %rdi\n"
    "    leaq user_fault_msg(%rip), %rsi\n"
    "    movq $21, %rdx\n"
    "    int  $0x80\n"
    "    movq $0xdead, %rax\n"
    "    movb (%rax), %bl\n"
    "    movq $60, %rax\n"
    "    movq $1, %rdi\n"
    "    int  $0x80\n"
    "user_fault_msg:\n"
    "    .ascii \"ring3fault: faulting\\n\"\n"
    "user_fault_end:\n"
);

extern const uint8_t user_fault_start[];
extern const uint8_t user_fault_end[];

/* ---------------------------------------------------------------- */
/* Embedded ELF blobs (built from tests/ sources via the Makefile)   */
/* ---------------------------------------------------------------- */

#define DECLARE_ELF(name) \
    extern const uint8_t _binary_##name##_elf_start[]; \
    extern const uint8_t _binary_##name##_elf_end[]

DECLARE_ELF(user_hello);
DECLARE_ELF(hello_libc);
DECLARE_ELF(hello);
DECLARE_ELF(echo);
DECLARE_ELF(true);
DECLARE_ELF(false);
DECLARE_ELF(init);
DECLARE_ELF(cat);
DECLARE_ELF(touch);
DECLARE_ELF(mkdir);
DECLARE_ELF(rmdir);
DECLARE_ELF(rm);
DECLARE_ELF(mv);
DECLARE_ELF(cp);
DECLARE_ELF(ls);
DECLARE_ELF(calc);
DECLARE_ELF(osh);
DECLARE_ELF(sleep);
DECLARE_ELF(kill);
DECLARE_ELF(top);
DECLARE_ELF(ovi);
DECLARE_ELF(tcc);
DECLARE_ELF(head);
DECLARE_ELF(libctest);
DECLARE_ELF(udptest);
DECLARE_ELF(echotcp);
DECLARE_ELF(selecttest);
DECLARE_ELF(selectserver);
DECLARE_ELF(tcpclient);
DECLARE_ELF(httpd);
DECLARE_ELF(ttytest);
DECLARE_ELF(envtest);

#undef DECLARE_ELF

/* ---------------------------------------------------------------- */
/* Registry                                                           */
/* ---------------------------------------------------------------- */

#define USER(n,    s, e, desc)  { n, s, e, 0, 0, desc }
#define USERELF(n, s, e, desc)  { n, 0, 0, s, e, desc }
#define ELF(n, desc) \
    USERELF(#n, _binary_##n##_elf_start, _binary_##n##_elf_end, desc)

static const builtin_t builtins[] = {
    /* Hand-asm flat blobs — proof of the ring-3 + syscall path. */
    USER("ring3hello", user_hello_start, user_hello_end,
         "ring-3 task: prints via SYSCALL fast path"),
    USER("ring3int80", user_int80_start, user_int80_end,
         "ring-3 task: prints via legacy int 0x80 path"),
    USER("ring3fault", user_fault_start, user_fault_end,
         "deliberately faults in ring 3; tests handler recovery"),

    /* Bare ELF demo — no libc, hand-rolled _start. */
    USERELF("hello_elf",
            _binary_user_hello_elf_start, _binary_user_hello_elf_end,
            "first real ring-3 ELF (no libc)"),

    /* Libc demo with formatted printf + malloc + free. */
    ELF(hello_libc, "libc demo: printf, malloc, strcpy, formats"),

    /* Tools — every one is an ELF that links against the osnos libc. */
    ELF(hello, "prints hello, world"),
    ELF(echo,  "prints its arguments"),
    ELF(true,  "exits 0"),
    ELF(false, "exits 1"),
    ELF(init,  "first process (no-op today)"),
    ELF(cat,   "prints the contents of files"),
    ELF(touch, "creates an empty file (or no-op if exists)"),
    ELF(mkdir, "creates a directory"),
    ELF(rmdir, "removes an empty directory"),
    ELF(rm,    "removes one or more files"),
    ELF(mv,    "moves/renames a file or directory"),
    ELF(cp,    "copies a file"),
    ELF(ls,    "lists directory entries (uses opendir/readdir)"),
    ELF(calc,  "integer arithmetic evaluator (+, -, *, /, %, parens)"),
    ELF(osh,   "mini script interpreter: vars, if/while, print"),
    ELF(sleep, "sleep SECONDS (cooperative hlt-loop)"),
    ELF(kill,  "kill PID (sets task kill_pending; delivers at next return)"),
    ELF(top,   "live task viewer (Ctrl+C to exit)"),
    ELF(ovi,   "vim-style modal text editor: ovi FILE"),
    ELF(tcc,   "C compiler (TinyCC) — STUB, real port pending"),
    ELF(head,  "print the first N lines of stdin / FILE"),
    ELF(libctest, "libc smoke test (FILE*, qsort, setjmp, inet_pton, etc.)"),
    ELF(udptest,  "UDP echo on port 1234 via socket/bind/recvfrom/sendto"),
    ELF(echotcp,  "TCP echo on port 80 via socket/bind/listen/accept/recv/send"),
    ELF(selecttest, "select() demo: multiplex TCP listen + stdin"),
    ELF(selectserver, "Beej's selectserver.c — multi-client chat on TCP 9034"),
    ELF(tcpclient,    "outbound TCP demo: tcpclient HOST PORT"),
    ELF(httpd,        "minimal HTTP/1.0 server, serves /sd/ files"),
    ELF(ttytest,      "demo termios canonical vs raw mode"),
    ELF(envtest,      "dump environ + setenv/unsetenv smoke test"),
};

#undef USER
#undef USERELF
#undef ELF

#define BUILTIN_COUNT (sizeof(builtins) / sizeof(builtins[0]))

const builtin_t *builtin_find(const char *name) {
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (os_streq(builtins[i].name, name)) return &builtins[i];
    }
    return 0;
}

size_t builtin_count(void) { return BUILTIN_COUNT; }

const builtin_t *builtin_at(size_t idx) {
    if (idx >= BUILTIN_COUNT) return 0;
    return &builtins[idx];
}
