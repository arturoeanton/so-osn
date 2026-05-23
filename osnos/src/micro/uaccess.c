#include "uaccess.h"

#include <stdint.h>

#include "extable.h"
#include "task.h"
#include "vmm.h"

/*
 * Byte-by-byte memcpy with a fault-recoverable inner loop.
 *
 * Signature (System V x86_64):
 *   rdi = dst, rsi = src, rdx = n  ->  rax (osnos_status_t)
 *
 * The protected span [__uaccess_copy_bytes, __uaccess_copy_bytes_end)
 * is registered in the extable at boot. If any of the memory loads or
 * stores inside that span page-faults, the page-fault handler rewrites
 * the iret frame's RIP to __uaccess_copy_bytes_fault, so the function
 * "returns" with OSNOS_EFAULT (14) instead of crashing the kernel.
 *
 * The helper has no prologue / no stack frame: RSP at entry == RSP at
 * any in-loop RIP, so the redirected `ret` pops the C caller's return
 * address correctly. Only caller-saved registers (rax/rdi/rsi/rdx) are
 * touched, so the SysV ABI for callees is satisfied without push/pop.
 */
__asm__ (
    ".text\n"
    ".global __uaccess_copy_bytes\n"
    ".global __uaccess_copy_bytes_end\n"
    ".global __uaccess_copy_bytes_fault\n"
    "__uaccess_copy_bytes:\n"
    "    testq %rdx, %rdx\n"
    "    jz 9f\n"
    "1:  movb (%rsi), %al\n"
    "    movb %al, (%rdi)\n"
    "    incq %rsi\n"
    "    incq %rdi\n"
    "    decq %rdx\n"
    "    jnz 1b\n"
    "__uaccess_copy_bytes_end:\n"
    "9:  xorl %eax, %eax\n"
    "    ret\n"
    "__uaccess_copy_bytes_fault:\n"
    "    movl $14, %eax\n"             /* OSNOS_EFAULT */
    "    ret\n"
);

extern uint64_t __uaccess_copy_bytes(void *dst, const void *src, uint64_t n);
extern uint8_t  __uaccess_copy_bytes_end[];
extern uint8_t  __uaccess_copy_bytes_fault[];

void uaccess_init(void) {
    extable_register(
        (uintptr_t)__uaccess_copy_bytes,
        (uintptr_t)__uaccess_copy_bytes_end,
        (uintptr_t)__uaccess_copy_bytes_fault);
}

static int user_range_ok(uintptr_t addr, size_t n) {
    if (n == 0) return 1;
    uintptr_t end;
    if (__builtin_add_overflow(addr, n, &end)) return 0;
    return end <= OSNOS_USER_VIRT_MAX;
}

/*
 * When a kernel task (pml4 == 0) makes a syscall — today this only
 * happens from the shell's self-test, which exercises sys_* directly —
 * the "user pointer" is actually a kernel string literal in the high
 * half. Skip the user-range check in that case so kernel callers can
 * use the same syscall surface as user tasks. The fault-recovery span
 * still protects against bad addresses.
 */
static int in_kernel_caller(void) {
    task_t *t = task_current();
    return !t || t->pml4 == 0;
}

osnos_status_t copy_from_user(void *dst, const void *user_src, size_t n) {
    if (!dst && n > 0) return OSNOS_EFAULT;
    if (!user_src && n > 0) return OSNOS_EFAULT;
    if (!in_kernel_caller() &&
        !user_range_ok((uintptr_t)user_src, n)) return OSNOS_EFAULT;
    return (osnos_status_t)__uaccess_copy_bytes(dst, user_src, (uint64_t)n);
}

osnos_status_t copy_to_user(void *user_dst, const void *src, size_t n) {
    if (!user_dst && n > 0) return OSNOS_EFAULT;
    if (!src && n > 0) return OSNOS_EFAULT;
    if (!in_kernel_caller() &&
        !user_range_ok((uintptr_t)user_dst, n)) return OSNOS_EFAULT;
    return (osnos_status_t)__uaccess_copy_bytes(user_dst, src, (uint64_t)n);
}

osnos_status_t copy_string_from_user(char *dst, const char *user_src,
                                      size_t maxlen) {
    if (maxlen == 0) return OSNOS_OK;
    if (!dst || !user_src) return OSNOS_EFAULT;
    if (!in_kernel_caller() &&
        !user_range_ok((uintptr_t)user_src, 1)) return OSNOS_EFAULT;

    /* Byte-by-byte copy that stops at the first NUL. Each
     * __uaccess_copy_bytes(.., 1) call is protected by extable, so a
     * fault on any single byte returns EFAULT without panic. Reading
     * "noexisto\0" (9 bytes) only reads 9 bytes — no over-read into
     * the next (possibly-unmapped) page like a fixed-size copy would.
     *
     * On every iteration we re-validate the next byte's user range,
     * to catch the case where the string straddles past OSNOS_USER_
     * VIRT_MAX (unlikely but cheap to guard). */
    for (size_t i = 0; i < maxlen; i++) {
        if (!in_kernel_caller() &&
            !user_range_ok((uintptr_t)(user_src + i), 1)) {
            return OSNOS_EFAULT;
        }
        uint8_t c = 0;
        osnos_status_t s = (osnos_status_t)__uaccess_copy_bytes(
            &c, user_src + i, 1);
        if (s != OSNOS_OK) return s;
        dst[i] = (char)c;
        if (c == 0) return OSNOS_OK;
    }
    /* Hit maxlen without NUL — force-terminate, signal OK. Caller can
     * detect truncation by checking strlen(dst) == maxlen-1 if it
     * cares. */
    dst[maxlen - 1] = 0;
    return OSNOS_OK;
}
