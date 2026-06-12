#include "serial.h"

#include <stdint.h>

#include "pic.h"
#include "../micro/idt.h"

/* I/O port helpers — same pattern as pic.c / rtl8139.c. */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* 16550 register layout (offset from base port). */
#define UART_DATA       0   /* RBR (read) / THR (write) when DLAB=0  */
#define UART_DLL        0   /* divisor low byte when DLAB=1          */
#define UART_IER        1   /* interrupt enable register             */
#define UART_DLH        1   /* divisor high byte when DLAB=1         */
#define UART_FCR        2   /* FIFO control register (write-only)    */
#define UART_LCR        3   /* line control                           */
#define UART_MCR        4   /* modem control                          */
#define UART_LSR        5   /* line status (read-only)               */

#define LSR_DR          0x01    /* data ready in RBR                  */
#define LSR_THRE        0x20    /* transmit hold register empty       */

#define LCR_8N1         0x03    /* 8 data bits, no parity, 1 stop bit */
#define LCR_DLAB        0x80    /* divisor latch access bit            */

/* Captured at init so subsequent putc/getc don't need the port arg.
 * osnos uses one UART; if we ever want multi-port, switch to per-call. */
static uint16_t s_port = 0;

void serial_init(uint16_t port) {
    s_port = port;

    /* Disable interrupts — we poll. */
    outb(port + UART_IER, 0x00);

    /* Set divisor for 38400 baud: divisor = 115200 / 38400 = 3. */
    outb(port + UART_LCR, LCR_DLAB);
    outb(port + UART_DLL, 3);
    outb(port + UART_DLH, 0);

    /* 8N1, DLAB cleared. */
    outb(port + UART_LCR, LCR_8N1);

    /* Enable + reset 16550 FIFOs with 14-byte trigger. */
    outb(port + UART_FCR, 0xC7);

    /* DTR + RTS + OUT2 (OUT2 gates the IRQ line on PCs; we keep it
     * for symmetry even with IRQs disabled — harmless). */
    outb(port + UART_MCR, 0x0B);
}

void serial_putc(char c) {
    if (s_port == 0) return;

    /* CRLF expansion so host terminals see proper line breaks. */
    if (c == '\n') {
        while ((inb(s_port + UART_LSR) & LSR_THRE) == 0) { /* spin */ }
        outb(s_port + UART_DATA, '\r');
    }
    while ((inb(s_port + UART_LSR) & LSR_THRE) == 0) { /* spin */ }
    outb(s_port + UART_DATA, (uint8_t)c);
}

void serial_puts(const char *s, size_t n) {
    if (s_port == 0 || s == 0) return;
    for (size_t i = 0; i < n; i++) serial_putc(s[i]);
}

/* ------------------------------------------------------------------ */
/* IRQ-driven RX (FASE 15.0).                                          */
/*                                                                     */
/* The 16550 RX FIFO is only 16 bytes. Polled draining (serial-in      */
/* feeder, every ~10 ms) loses bytes whenever ring-0 stays busy past   */
/* the FIFO's depth — most visibly with the GUI running, where serial  */
/* console input dropped ~half its characters. IRQ4 now drains the     */
/* FIFO into this 1 KiB software ring the moment bytes arrive;         */
/* serial_try_getc consumes from the ring first and falls back to a    */
/* direct LSR poll (pre-IRQ boot phases, or IRQ init not called).      */
/* ------------------------------------------------------------------ */

#define RX_RING_SIZE 1024   /* power of two — masked indices */
static volatile uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head;     /* written by the IRQ handler  */
static volatile uint32_t rx_tail;     /* read by serial_try_getc     */
static uint64_t          rx_overruns; /* software-ring drops (diag)  */

__asm__ (
    ".global serial_irq_entry\n"
    "serial_irq_entry:\n"
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
    "    call serial_irq_handle\n"
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

extern void serial_irq_entry(void);

void serial_irq_handle(void) {
    /* Drain the HW FIFO completely — one IRQ may cover many bytes
     * (14-byte trigger level set in FCR). */
    while (inb(s_port + UART_LSR) & LSR_DR) {
        uint8_t b = inb(s_port + UART_DATA);
        uint32_t next = (rx_head + 1) & (RX_RING_SIZE - 1);
        if (next == rx_tail) {
            rx_overruns++;        /* software ring full — drop byte */
        } else {
            rx_ring[rx_head] = b;
            rx_head = next;
        }
    }
    pic_send_eoi(4);
}

void serial_enable_rx_irq(void) {
    if (s_port == 0) return;
    idt_set_handler(0x24, (void *)serial_irq_entry, /*dpl=*/0);
    /* IER bit 0 — interrupt on received-data-available. OUT2 (MCR)
     * was already set at init, which gates the IRQ line on PCs. */
    outb(s_port + UART_IER, 0x01);
    pic_unmask(4);
}

uint64_t serial_rx_overruns(void) { return rx_overruns; }

bool serial_try_getc(uint8_t *out) {
    if (s_port == 0 || out == 0) return false;
    /* IRQ-fed software ring first. */
    if (rx_tail != rx_head) {
        *out = rx_ring[rx_tail];
        rx_tail = (rx_tail + 1) & (RX_RING_SIZE - 1);
        return true;
    }
    /* Fallback: direct FIFO poll (pre-IRQ boot, or IRQs not enabled). */
    if ((inb(s_port + UART_LSR) & LSR_DR) == 0) return false;
    *out = inb(s_port + UART_DATA);
    return true;
}
