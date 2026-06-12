#include "ps2_irq.h"

#include "keyboard.h"
#include "mouse.h"
#include "pic.h"

#include "../fs/devfs.h"
#include "../micro/idt.h"

/*
 * See ps2_irq.h for the design rationale. The asm entry stubs mirror
 * rtl8139_irq_entry: save caller-saved GPRs (+r12 used as the rsp
 * scratch), align, call the C handler, restore, iretq. These handlers
 * never preempt — they push an event into a devfs ring and return to
 * whatever was running (kernel or user).
 */

#define PS2_STATUS      0x64
#define STAT_OUTBUF     0x01    /* bit 0: data available on 0x60 */
#define STAT_AUX_DATA   0x20    /* bit 5: byte is from AUX (mouse) */

#define PS2_CMD_READ_CB  0x20   /* read 8042 command byte → 0x60 */
#define PS2_CMD_WRITE_CB 0x60   /* write 8042 command byte ← 0x60 */
#define CB_INT_KBD       0x01   /* bit 0: IRQ1 on keyboard byte */
#define CB_INT_AUX       0x02   /* bit 1: IRQ12 on AUX byte */

static uint64_t kbd_irq_count;
static uint64_t mouse_irq_count;

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}

/* Wait until the 8042 input buffer is free so it will accept a
 * command/data byte. Bounded — a wedged controller must not hang
 * boot. */
static void wait_input_clear(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & 0x02)) return;
    }
}

/* Wait until a response byte is available on 0x60. Bounded. */
static bool wait_output_full(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & STAT_OUTBUF) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* IRQ 1 — keyboard byte ready.                                       */
/* ------------------------------------------------------------------ */

__asm__ (
    ".global ps2_kbd_irq_entry\n"
    "ps2_kbd_irq_entry:\n"
    "    pushq %rax\n"
    "    pushq %rcx\n"
    "    pushq %rdx\n"
    "    pushq %rsi\n"
    "    pushq %rdi\n"
    "    pushq %r8\n"
    "    pushq %r9\n"
    "    pushq %r10\n"
    "    pushq %r11\n"
    "    pushq %r12\n"
    "    movq %rsp, %r12\n"
    "    andq $-16, %rsp\n"
    "    call ps2_kbd_irq_handle\n"
    "    movq %r12, %rsp\n"
    "    popq %r12\n"
    "    popq %r11\n"
    "    popq %r10\n"
    "    popq %r9\n"
    "    popq %r8\n"
    "    popq %rdi\n"
    "    popq %rsi\n"
    "    popq %rdx\n"
    "    popq %rcx\n"
    "    popq %rax\n"
    "    iretq\n"
);

extern void ps2_kbd_irq_entry(void);

void ps2_kbd_irq_handle(void) {
    kbd_irq_count++;
    /* One IRQ per byte normally, but drain defensively in case bytes
     * queued while interrupts were off. keyboard_poll consumes one
     * byte per call and only emits an event when its state machine
     * completes (skips break codes / 0xE0 prefixes), so loop on the
     * STATUS register, not on its return value. */
    for (int i = 0; i < 8; i++) {
        uint8_t st = inb(PS2_STATUS);
        if (!(st & STAT_OUTBUF) || (st & STAT_AUX_DATA)) break;
        keyboard_event_t ev;
        if (keyboard_poll(&ev)) devfs_input_push(ev);
    }
    pic_send_eoi(IRQ_KEYBOARD);
}

/* ------------------------------------------------------------------ */
/* IRQ 12 — AUX (mouse) byte ready.                                   */
/* ------------------------------------------------------------------ */

__asm__ (
    ".global ps2_mouse_irq_entry\n"
    "ps2_mouse_irq_entry:\n"
    "    pushq %rax\n"
    "    pushq %rcx\n"
    "    pushq %rdx\n"
    "    pushq %rsi\n"
    "    pushq %rdi\n"
    "    pushq %r8\n"
    "    pushq %r9\n"
    "    pushq %r10\n"
    "    pushq %r11\n"
    "    pushq %r12\n"
    "    movq %rsp, %r12\n"
    "    andq $-16, %rsp\n"
    "    call ps2_mouse_irq_handle\n"
    "    movq %r12, %rsp\n"
    "    popq %r12\n"
    "    popq %r11\n"
    "    popq %r10\n"
    "    popq %r9\n"
    "    popq %r8\n"
    "    popq %rdi\n"
    "    popq %rsi\n"
    "    popq %rdx\n"
    "    popq %rcx\n"
    "    popq %rax\n"
    "    iretq\n"
);

extern void ps2_mouse_irq_entry(void);

void ps2_mouse_irq_handle(void) {
    mouse_irq_count++;
    /* mouse_poll consumes one AUX byte per call and emits an event
     * once the 3-byte packet completes — so a full packet spans three
     * IRQs. Same status-driven drain as the keyboard side. */
    for (int i = 0; i < 8; i++) {
        uint8_t st = inb(PS2_STATUS);
        if (!(st & STAT_OUTBUF) || !(st & STAT_AUX_DATA)) break;
        mouse_event_t ev;
        if (mouse_poll(&ev)) devfs_mouse_push(ev);
    }
    pic_send_eoi(12);
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */

void ps2_irq_init(void) {
    /* Flip the 8042 command-byte interrupt-enable bits. keyboard_init
     * and mouse_init already enabled both ports and streaming — this
     * only changes WHO learns about a ready byte (IRQ vs. poll). */
    wait_input_clear();
    outb(PS2_STATUS, PS2_CMD_READ_CB);
    uint8_t cb = wait_output_full() ? inb(0x60) : 0x47;
    cb |= CB_INT_KBD | CB_INT_AUX;
    wait_input_clear();
    outb(PS2_STATUS, PS2_CMD_WRITE_CB);
    wait_input_clear();
    outb(0x60, cb);

    idt_set_handler(0x21, (void *)ps2_kbd_irq_entry,   /*dpl=*/0);
    idt_set_handler(0x2C, (void *)ps2_mouse_irq_entry, /*dpl=*/0);
    pic_unmask(IRQ_KEYBOARD);   /* IRQ 1 */
    pic_unmask(12);             /* IRQ 12 — pic_unmask also opens the
                                 * master's cascade line (IRQ 2). */
}

uint64_t ps2_kbd_irqs  (void) { return kbd_irq_count;   }
uint64_t ps2_mouse_irqs(void) { return mouse_irq_count; }
