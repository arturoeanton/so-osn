#include "pic.h"

#include <stdint.h>

/* I/O port helpers — only ones we need. */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/*
 * Output a no-op to port 0x80 to give old ISA hardware a moment to
 * settle between PIC initialization commands. Reading 0x80 has no
 * side effect on modern systems but the write delay is harmless.
 */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

/* ICW1 bits. */
#define ICW1_ICW4  0x01      /* ICW4 will follow */
#define ICW1_INIT  0x10      /* start initialization sequence */

/* ICW4 bits. */
#define ICW4_8086  0x01      /* 8086/88 mode (not MCS-80/85) */

#define PIC_EOI    0x20      /* End-of-interrupt */

void pic_init(void) {
    /*
     * Standard 4-byte init sequence per chip:
     *   ICW1 (cmd port) — INIT + ICW4
     *   ICW2 (data)     — vector base
     *   ICW3 (data)     — master: bitmask of slave-attached lines (bit 2)
     *                     slave : cascade identity (2)
     *   ICW4 (data)     — 8086 mode
     */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();

    outb(PIC1_DATA, PIC_VECTOR_BASE_MASTER); io_wait();
    outb(PIC2_DATA, PIC_VECTOR_BASE_SLAVE);  io_wait();

    outb(PIC1_DATA, 1 << 2);                 io_wait();   /* slave on IRQ2 */
    outb(PIC2_DATA, 2);                       io_wait();   /* cascade ID */

    outb(PIC1_DATA, ICW4_8086);               io_wait();
    outb(PIC2_DATA, ICW4_8086);               io_wait();

    /* Mask every line. Callers unmask explicitly when they install
     * a handler for that line. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_mask(int irq) {
    if (irq < 0 || irq > 15) return;
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(1u << (irq & 7));
    outb(port, (uint8_t)(inb(port) | bit));
}

void pic_unmask(int irq) {
    if (irq < 0 || irq > 15) return;
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(1u << (irq & 7));
    outb(port, (uint8_t)(inb(port) & ~bit));

    /*
     * Unmasking a slave line also requires the master's cascade IRQ2
     * to be unmasked — otherwise the slave can never reach the CPU.
     */
    if (irq >= 8) {
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1u << 2)));
    }
}

void pic_send_eoi(int irq) {
    /* Slave lines need EOI on both chips, master only on its own. */
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}
