#include "builtin.h"

#include "../lib/string.h"

/*
 * Registry of /bin entries — POST FASE 2 disk-resident.
 *
 * Since FASE 2 the kernel is no longer the canonical store for ring-3
 * binaries: sd.img holds the full set of ELFs in /sd/bin, populated by
 * the GNUmakefile at build time. Only a **recovery ROM** stays embedded
 * inside the kernel image so the system can boot even when:
 *   - the disk is missing entirely (diskless QEMU run)
 *   - /sd/bin got wiped / corrupted
 *   - mtools wasn't available at build time and sd.img couldn't be
 *     populated
 *
 * The ROM set is the minimum to make a usable system:
 *   - consrv, kbdsrv, shellsrv   (the 3 ring-3 servers spawned by kmain)
 *   - banner                     (.oshrc default greeting)
 * Plus the three hand-asm flat blobs (used to verify the ring-3 path
 * without involving the ELF loader at all) and the bare user_hello
 * ELF (proves the ELF loader doesn't depend on libc). These three
 * weigh ~150 bytes each so they're free to keep.
 *
 * Every other tool (~57 ELFs: ls, cat, env, grep, ...) lives ONLY on
 * disk. exec.c prefers the VFS path; if a /bin/<name> is asked for
 * and isn't on disk, builtin_find returns NULL → ENOENT.
 */

/* ---------------------------------------------------------------- */
/* Hand-asm flat user blobs                                          */
/* ---------------------------------------------------------------- */

/*
 * /bin/ring3hello — fast-path SYSCALL.
 */
__asm__ (
    ".section .text\n"
    ".global user_hello_start\n"
    ".global user_hello_end\n"
    "user_hello_start:\n"
    "    movq $1, %rax\n"
    "    movq $1, %rdi\n"
    "    leaq user_hello_msg(%rip), %rsi\n"
    "    movq $17, %rdx\n"
    "    syscall\n"
    "    movq $60, %rax\n"
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
    "    movq $19, %rdx\n"
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
 * /bin/ring3fault — deliberate ring-3 fault for testing recovery.
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
/* Embedded ELF blobs — ROM only (4 critical files + bare demo)      */
/* ---------------------------------------------------------------- */

#define DECLARE_ELF(name) \
    extern const uint8_t _binary_##name##_elf_start[]; \
    extern const uint8_t _binary_##name##_elf_end[]

/* Bare ELF demo — no libc, hand-rolled _start. Tiny (~1 KiB). */
DECLARE_ELF(user_hello);

/* ROM recovery set: the 3 ring-3 servers + banner. ~150 KiB total. */
DECLARE_ELF(consrv);
DECLARE_ELF(kbdsrv);
DECLARE_ELF(shellsrv);
DECLARE_ELF(banner);

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

    /* Recovery ROM — only the bare minimum to boot. Everything else
     * lives in /sd/bin (populated by the build script). */
    ELF(consrv,   "ring-3 console server (recovery ROM)"),
    ELF(kbdsrv,   "ring-3 keyboard server (recovery ROM)"),
    ELF(shellsrv, "ring-3 shell (recovery ROM)"),
    ELF(banner,   "osnos welcome banner (recovery ROM)"),
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
