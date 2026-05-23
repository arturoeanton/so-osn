#include "serial.h"

#include <stdint.h>

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

bool serial_try_getc(uint8_t *out) {
    if (s_port == 0 || out == 0) return false;
    if ((inb(s_port + UART_LSR) & LSR_DR) == 0) return false;
    *out = inb(s_port + UART_DATA);
    return true;
}
