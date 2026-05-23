#include "idt.h"

#include <stdbool.h>

#include "../drivers/framebuffer.h"
#include "../drivers/serial.h"
#include "../lib/string.h"
#include "../proc/exec.h"
#include "extable.h"
#include "gdt.h"
#include "task.h"

/*
 * IDT entry layout (long mode, 16 bytes):
 *   bits  0..15  offset[0..15]
 *   bits 16..31  segment selector (kernel CS)
 *   bits 32..34  IST index (0 = use legacy interrupt stack)
 *   bits 35..39  zero
 *   bits 40..43  gate type (0xE = 64-bit interrupt gate)
 *   bit  44      zero
 *   bits 45..46  DPL (0 for exceptions, 3 if you want user-callable)
 *   bit  47      P  (present)
 *   bits 48..63  offset[16..31]
 *   bits 64..95  offset[32..63]
 *   bits 96..127 zero
 */

typedef struct {
    uint16_t off_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_high;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

#define IDT_ENTRIES 256

static idt_entry_t idt[IDT_ENTRIES] __attribute__((aligned(16)));
static idtr_t      idtr;
static uint64_t    exception_count = 0;

#define TYPE_INTERRUPT_GATE 0x8E  /* P=1 DPL=00 type=1110 */

/*
 * Install a vector with explicit DPL. Most exceptions use DPL=0 so user
 * code cannot synthesize them via `int N` — only hardware-generated
 * exceptions (e.g. #PF, #UD) bypass the DPL check.
 *
 * Vectors that programs are explicitly allowed to invoke from ring 3
 * via the `INT n` family — int3 / into / int 0x80 etc. — need DPL=3.
 */
static void install_with_dpl(int vec, void *handler, int dpl) {
    uint64_t addr = (uint64_t)(uintptr_t)handler;
    idt[vec].off_lo    = (uint16_t)(addr & 0xFFFF);
    idt[vec].selector  = GDT_KCODE;
    idt[vec].ist       = 0;
    idt[vec].type_attr = (uint8_t)(TYPE_INTERRUPT_GATE | ((dpl & 3) << 5));
    idt[vec].off_mid   = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vec].off_high  = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vec].zero      = 0;
}

static void install(int vec, void *handler) {
    install_with_dpl(vec, handler, 0);
}

/* ---------------------------------------------------------------- */
/* Panic helpers — direct to framebuffer, no IPC.                   */
/* ---------------------------------------------------------------- */

/* Re-entrancy guard. If a fault happens INSIDE the panic path (e.g.
 * framebuffer_draw_string takes a page fault because FB state is
 * corrupt), recursing back through put_str would stack-overflow and
 * triple-fault. While `in_panic` is set we skip the framebuffer path
 * entirely and only spin out to serial — that's pure inb/outb and
 * can't fault. */
static volatile int in_panic = 0;

static void put_str(const char *s, uint32_t color) {
    if (!s) return;
    size_t n = os_strlen(s);
    if (!in_panic) {
        framebuffer_draw_string(s, color);
    }
    serial_puts(s, n);
}

/* Called at the top of every panic handler. Disables interrupts and
 * raises the in_panic flag so put_str stops touching the framebuffer.
 * Safe to call multiple times (idempotent). */
static void panic_enter(void) {
    __asm__ volatile ("cli");
    in_panic = 1;
}

static void put_hex(uint64_t value, uint32_t color) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        uint8_t n = (value >> (i * 4)) & 0xF;
        buf[17 - i] = (char)(n < 10 ? '0' + n : 'a' + (n - 10));
    }
    buf[18] = 0;
    put_str(buf, color);
}

static void put_dec(uint64_t value, uint32_t color) {
    char buf[24];
    os_format_u64(value, buf, sizeof(buf));
    put_str(buf, color);
}

static const char *exception_name(int vec) {
    switch (vec) {
        case 0:  return "Divide by zero";
        case 1:  return "Debug";
        case 2:  return "NMI";
        case 3:  return "Breakpoint";
        case 4:  return "Overflow";
        case 5:  return "Bound range exceeded";
        case 6:  return "Invalid opcode";
        case 7:  return "Device not available";
        case 8:  return "Double fault";
        case 10: return "Invalid TSS";
        case 11: return "Segment not present";
        case 12: return "Stack-segment fault";
        case 13: return "General protection fault";
        case 14: return "Page fault";
        case 16: return "x87 FPU error";
        case 17: return "Alignment check";
        case 18: return "Machine check";
        case 19: return "SIMD FPU error";
        case 20: return "Virtualization";
        case 21: return "Control protection";
        default: return "(unknown)";
    }
}

static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static void hcf(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

static void panic_header(int vec) {
    panic_enter();
    exception_count++;
    put_str("\n\n*** EXCEPTION ", 0xff5555);
    put_dec((uint64_t)vec, 0xff5555);
    put_str(" ", 0xff5555);
    put_str(exception_name(vec), 0xff5555);
    put_str(" ***\n", 0xff5555);
}

/* ---------------------------------------------------------------- */
/* Per-vector handlers (clang __attribute__((interrupt)))           */
/* ---------------------------------------------------------------- */

/*
 * Interrupt frame as defined by SysV / clang for x86_64 interrupt
 * handlers. RIP/CS/RFLAGS/RSP/SS are pushed by the CPU.
 */
struct interrupt_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

static void print_origin(uint64_t cs) {
    bool from_user = (cs & 3) == 3;
    put_str(from_user ? "from USER mode (ring 3)\n"
                      : "from kernel mode (ring 0)\n",
            from_user ? 0x55ff55 : 0xffff00);
}

/*
 * Common pre-handler logic for recoverable exception vectors.
 *
 * If the fault hit a kernel RIP registered in the extable, rewrite
 * frame->rip to the recovery address and signal the caller to return
 * cleanly (so the interrupted helper resumes at the fixup label).
 *
 * Otherwise, if the fault originated in ring 3 (CPL=3), print a one-
 * line origin report and tear down the offending user task. Never
 * returns in that path — control long-jumps back to scheduler_loop.
 *
 * Returns true when the handler should iretq-return (extable fixup
 * applied). Returns false when the handler should fall through to the
 * panic path (kernel-mode fault with no fixup).
 */
static bool fault_try_recover(int vec,
                              struct interrupt_frame *frame,
                              uint64_t err)
{
    if ((frame->cs & 3) == 0) {
        uintptr_t recovery;
        if (extable_lookup((uintptr_t)frame->rip, &recovery)) {
            frame->rip = recovery;
            return true;
        }
    }

    if ((frame->cs & 3) == 3) {
        task_t *t = task_current();
        if (t && t->pml4) {
            put_str("\n*** ring-3 task killed: ", 0xff9955);
            put_str(exception_name(vec), 0xff9955);
            if (vec == 14) {
                put_str(" cr2=", 0xff9955); put_hex(read_cr2(), 0xffff00);
            }
            put_str(" rip=", 0xff9955); put_hex(frame->rip, 0xffff00);
            put_str(" err=", 0xff9955); put_hex(err, 0xffff00);
            put_str("\n", 0xff9955);
            proc_exit_current_user(128 + 11);   /* SIGSEGV-style exit */
            __builtin_unreachable();
        }
    }

    return false;
}

#define VECTOR_NO_ERROR(N) \
    static __attribute__((interrupt)) \
    void exc_##N(struct interrupt_frame *frame) { \
        if (fault_try_recover(N, frame, 0)) return; \
        panic_header(N); \
        print_origin(frame->cs); \
        put_str("rip=", 0xff5555); put_hex(frame->rip, 0xffff00); \
        put_str(" cs=",  0xff5555); put_hex(frame->cs,  0xffff00); \
        put_str(" rsp=", 0xff5555); put_hex(frame->rsp, 0xffff00); \
        put_str("\n",    0xff5555); \
        hcf(); \
    }

#define VECTOR_WITH_ERROR(N) \
    static __attribute__((interrupt)) \
    void exc_##N(struct interrupt_frame *frame, uint64_t err) { \
        if (fault_try_recover(N, frame, err)) return; \
        panic_header(N); \
        print_origin(frame->cs); \
        put_str("err=", 0xff5555); put_hex(err, 0xffff00); \
        put_str(" rip=", 0xff5555); put_hex(frame->rip, 0xffff00); \
        put_str("\n",    0xff5555); \
        hcf(); \
    }

VECTOR_NO_ERROR(0)
VECTOR_NO_ERROR(1)
VECTOR_NO_ERROR(2)
VECTOR_NO_ERROR(3)
VECTOR_NO_ERROR(4)
VECTOR_NO_ERROR(5)
VECTOR_NO_ERROR(6)
VECTOR_NO_ERROR(7)
VECTOR_WITH_ERROR(8)        /* double fault */
VECTOR_WITH_ERROR(10)       /* invalid TSS */
VECTOR_WITH_ERROR(11)       /* segment not present */
VECTOR_WITH_ERROR(12)       /* stack-segment fault */
VECTOR_WITH_ERROR(13)       /* GPF */
VECTOR_NO_ERROR(16)
VECTOR_WITH_ERROR(17)
VECTOR_NO_ERROR(18)
VECTOR_NO_ERROR(19)
VECTOR_NO_ERROR(20)
VECTOR_WITH_ERROR(21)

/* Page fault has its own handler — we want CR2. */
static __attribute__((interrupt))
void exc_pf(struct interrupt_frame *frame, uint64_t err) {
    if (fault_try_recover(14, frame, err)) return;

    panic_header(14);
    print_origin(frame->cs);
    uint64_t fault_addr = read_cr2();
    put_str("addr=", 0xff5555); put_hex(fault_addr, 0xffff00);
    put_str(" err=", 0xff5555); put_hex(err, 0xffff00);
    put_str(" rip=", 0xff5555); put_hex(frame->rip, 0xffff00);
    put_str("\n",    0xff5555);
    put_str(err & 1 ? "  protection violation\n" : "  page not present\n", 0xff5555);
    put_str(err & 2 ? "  write\n" : "  read\n", 0xff5555);
    put_str(err & 4 ? "  user mode\n" : "  kernel mode\n", 0xff5555);
    hcf();
}

/* Generic catch-all for vectors we don't break out. */
static __attribute__((interrupt))
void exc_generic(struct interrupt_frame *frame) {
    (void)frame;
    panic_header(255);
    put_str("rip=", 0xff5555); put_hex(frame->rip, 0xffff00);
    put_str("\n",    0xff5555);
    hcf();
}

/* ---------------------------------------------------------------- */
/* Install                                                          */
/* ---------------------------------------------------------------- */

void idt_init(void) {
    for (int i = 0; i < IDT_ENTRIES; i++) install(i, exc_generic);

    install(0,  exc_0);
    install(1,  exc_1);
    install(2,  exc_2);
    install_with_dpl(3, exc_3, 3);   /* #BP callable from ring 3 (debuggers) */
    install_with_dpl(4, exc_4, 3);   /* #OF callable from ring 3 (INTO)      */
    install(5,  exc_5);
    install(6,  exc_6);
    install(7,  exc_7);
    install(8,  exc_8);
    install(10, exc_10);
    install(11, exc_11);
    install(12, exc_12);
    install(13, exc_13);
    install(14, exc_pf);
    install(16, exc_16);
    install(17, exc_17);
    install(18, exc_18);
    install(19, exc_19);
    install(20, exc_20);
    install(21, exc_21);

    /*
     * int 0x80 syscall entry (Linux-legacy ABI). DPL=3 so user code
     * can invoke it. Handler is in src/micro/int80.c — file-scope asm.
     */
    extern void int80_entry(void);
    install_with_dpl(0x80, int80_entry, 3);

    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base  = (uint64_t)(uintptr_t)&idt;

    __asm__ volatile ("lidt %0" :: "m"(idtr));
}

uint64_t idt_limit(void)            { return idtr.limit; }
uint64_t idt_base(void)             { return idtr.base;  }
uint64_t idt_exception_count(void)  { return exception_count; }

void idt_set_handler(int vec, void *handler, int dpl) {
    if (vec < 0 || vec >= IDT_ENTRIES) return;
    install_with_dpl(vec, handler, dpl);
}
