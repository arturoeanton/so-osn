#include "rtl8139.h"

#include "../micro/idt.h"
#include "../micro/pmm.h"
#include "pci.h"
#include "pic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VENDOR_RTL  0x10ECu
#define DEV_8139    0x8139u

/* I/O port offsets from BAR0. */
#define R_IDR0      0x00     /* MAC bytes 0..5 */
#define R_TSD0      0x10     /* TX status descriptor 0 (then +4 / +8 / +c) */
#define R_TSAD0     0x20     /* TX start addr  0 (then +4 / +8 / +c) */
#define R_RBSTART   0x30     /* RX ring base (phys) */
#define R_CMD       0x37
#define R_CAPR      0x38     /* RX read pointer — offset -0x10 inside ring */
#define R_IMR       0x3C     /* Interrupt mask */
#define R_ISR       0x3E     /* Interrupt status (write-1-to-clear) */
#define R_TCR       0x40
#define R_RCR       0x44
#define R_CONFIG1   0x52

/* CMD bits. */
#define CMD_BUFE    0x01     /* read-only: RX buffer empty */
#define CMD_TE      0x04
#define CMD_RE      0x08
#define CMD_RESET   0x10

/* ISR / IMR bits. */
#define ISR_ROK     0x0001
#define ISR_RER     0x0002
#define ISR_TOK     0x0004
#define ISR_TER     0x0008
#define ISR_RXOVW   0x0010

/* RCR bits. */
#define RCR_AAP     0x01     /* accept all packets (promiscuous) */
#define RCR_APM     0x02     /* accept physical match */
#define RCR_AM      0x04     /* accept multicast */
#define RCR_AB      0x08     /* accept broadcast */
#define RCR_WRAP    0x80     /* allow chip to overflow past buffer end */
/* RBLEN bits 11..12: 00 = 8 KiB + 16. */

/* TSD bits. */
#define TSD_OWN     (1u << 13)  /* chip sets when DMA finished */

#define RX_RING_PAGES  3              /* 8 KiB + 16 + 1500 ≈ 9.5 KiB → 3 pages */
#define RX_RING_BYTES  (RX_RING_PAGES * PAGE_SIZE)
#define RX_USABLE      8192           /* RBLEN nominal capacity */
#define TX_BUF_SIZE    1792           /* max ethernet frame + slack */
#define NUM_TX_SLOTS   4

static struct {
    bool             present;
    uint16_t         io_base;
    uint8_t          irq_line;
    uint8_t          mac[6];
    rtl8139_rx_fn    rx_cb;

    uint8_t  *rx_buf_virt;
    uint64_t  rx_buf_phys;
    uint16_t  rx_offset;       /* current read offset inside RX ring */

    uint8_t  *tx_buf_virt[NUM_TX_SLOTS];
    uint64_t  tx_buf_phys[NUM_TX_SLOTS];
    uint8_t   tx_cur;          /* next slot to use, round-robin */

    uint64_t  irq_count;
    uint64_t  rx_packets;
    uint64_t  tx_packets;
    uint64_t  rx_bytes;
    uint64_t  tx_bytes;
    uint64_t  errors;
} dev;

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" :: "a"(v), "Nd"(port));
}
static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ------------------------------------------------------------------ */
/* IRQ handler — asm stub mirrors the pattern used by timer_entry.    */
/* ------------------------------------------------------------------ */

__asm__ (
    ".global rtl8139_irq_entry\n"
    "rtl8139_irq_entry:\n"
    /* Save caller-saved regs + r12 (we use it as rsp scratch below). */
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
    "    call rtl8139_irq_handle\n"
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

extern void rtl8139_irq_entry(void);

static void drain_rx(void) {
    /*
     * Packets in the RX ring have the shape:
     *   [hdr 2B][len 2B][payload (len bytes including the 4-byte CRC)]
     * After consuming one, advance the read offset to 4-byte-align past
     * the payload and update CAPR (offset -0x10 by chip convention).
     */
    while (!(inb(dev.io_base + R_CMD) & CMD_BUFE)) {
        uint8_t *p = dev.rx_buf_virt + dev.rx_offset;
        uint16_t header = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        uint16_t length = (uint16_t)p[2] | ((uint16_t)p[3] << 8);

        if (!(header & 0x0001)) {
            /* RX error — bail and rely on chip's RER recovery. */
            dev.errors++;
            break;
        }

        dev.rx_packets++;
        if (length >= 4) dev.rx_bytes += (uint32_t)(length - 4);

        /* Frame payload starts at p+4 (after hdr+len); chip-appended
         * 4-byte CRC is at the tail. Hand the trimmed frame to the
         * stack callback. */
        if (dev.rx_cb && length > 4) {
            dev.rx_cb(p + 4, (size_t)(length - 4));
        }

        /* Skip 4-byte header + payload, then 4-byte align. */
        uint32_t step = (uint32_t)length + 4u;
        dev.rx_offset = (uint16_t)((dev.rx_offset + step + 3u) & ~3u);
        if (dev.rx_offset >= RX_USABLE) dev.rx_offset -= RX_USABLE;

        outw(dev.io_base + R_CAPR, (uint16_t)(dev.rx_offset - 0x10));
    }
}

void rtl8139_set_rx_callback(rtl8139_rx_fn fn) {
    dev.rx_cb = fn;
}

void rtl8139_irq_handle(void) {
    if (!dev.present) {
        pic_send_eoi(dev.irq_line);
        return;
    }

    uint16_t isr = inw(dev.io_base + R_ISR);
    /* Write-1-to-clear — ack everything we saw. */
    outw(dev.io_base + R_ISR, isr);

    dev.irq_count++;

    if (isr & ISR_TOK) {
        /* Count one TX completion per IRQ. Multiple slots may finish
         * between IRQs but the chip only asserts TOK once until we
         * ack — so this undercounts on burst TX, fine for diagnostics. */
        dev.tx_packets++;
    }
    if (isr & (ISR_RER | ISR_TER | ISR_RXOVW)) {
        dev.errors++;
    }
    if (isr & ISR_ROK) {
        drain_rx();
    }

    pic_send_eoi(dev.irq_line);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

bool rtl8139_init(void) {
    dev.present = false;

    pci_addr_t pa;
    if (!pci_find_device(VENDOR_RTL, DEV_8139, &pa)) return false;

    pci_enable_bus_master(&pa);

    uint32_t bar0 = pci_read_bar(&pa, 0);
    if (!(bar0 & 0x1u)) return false;       /* memory BAR; we need I/O */
    dev.io_base  = (uint16_t)(bar0 & 0xFFFCu);
    dev.irq_line = pci_read_irq(&pa);

    /* Wake the chip — required before any other access. */
    outb(dev.io_base + R_CONFIG1, 0x00);

    /* Software reset — chip clears CMD_RESET when complete. */
    outb(dev.io_base + R_CMD, CMD_RESET);
    int spins = 100000;
    while (spins-- > 0 && (inb(dev.io_base + R_CMD) & CMD_RESET)) { }
    if (inb(dev.io_base + R_CMD) & CMD_RESET) return false;

    for (int i = 0; i < 6; i++) {
        dev.mac[i] = inb(dev.io_base + R_IDR0 + i);
    }

    /* Allocate physically contiguous RX ring. */
    dev.rx_buf_phys = pmm_alloc_pages_contig(RX_RING_PAGES);
    if (!dev.rx_buf_phys) return false;
    dev.rx_buf_virt = (uint8_t *)(dev.rx_buf_phys + pmm_hhdm_offset());
    for (size_t i = 0; i < RX_RING_BYTES; i++) dev.rx_buf_virt[i] = 0;
    outl(dev.io_base + R_RBSTART, (uint32_t)dev.rx_buf_phys);

    /* Each TX slot needs a single contiguous buffer; one page each is
     * plenty (4 KiB ≫ 1518 max ethernet frame) and per-page allocation
     * is by definition contiguous. */
    for (int i = 0; i < NUM_TX_SLOTS; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return false;
        dev.tx_buf_phys[i] = phys;
        dev.tx_buf_virt[i] = (uint8_t *)(phys + pmm_hhdm_offset());
        outl(dev.io_base + R_TSAD0 + i * 4, (uint32_t)phys);
    }
    dev.tx_cur = 0;

    /* RX config: accept physical match + broadcast, wrap-around enabled,
     * 8 KiB + 16 ring. AAP off for now (no promiscuous). */
    outl(dev.io_base + R_RCR, RCR_APM | RCR_AB | RCR_WRAP);

    /* TX config: standard defaults — interframe gap, no loopback. */
    outl(dev.io_base + R_TCR, 0x03000700);

    /* Enable RX + TX engines. */
    outb(dev.io_base + R_CMD, CMD_RE | CMD_TE);

    /* Reset read offset; CAPR uses the (-0x10) on-chip convention. */
    dev.rx_offset = 0;
    outw(dev.io_base + R_CAPR, (uint16_t)(0u - 0x10u));

    /* Enable IRQ sources: RX OK, TX OK, plus error flavours so the
     * counters reflect them. */
    outw(dev.io_base + R_IMR,
         (uint16_t)(ISR_ROK | ISR_TOK | ISR_RER | ISR_TER | ISR_RXOVW));

    /* Install IDT entry + unmask the PIC line. */
    int vec;
    if (dev.irq_line < 8) {
        vec = PIC_VECTOR_BASE_MASTER + dev.irq_line;
    } else {
        vec = PIC_VECTOR_BASE_SLAVE + (dev.irq_line - 8);
    }
    idt_set_handler(vec, (void *)rtl8139_irq_entry, /*dpl=*/0);
    pic_unmask(dev.irq_line);

    dev.present = true;
    return true;
}

bool        rtl8139_present(void)    { return dev.present; }
const uint8_t *rtl8139_mac(void)     { return dev.mac; }
uint16_t    rtl8139_io_base(void)    { return dev.io_base; }
uint8_t     rtl8139_irq_line(void)   { return dev.irq_line; }

uint64_t    rtl8139_irqs(void)       { return dev.irq_count;  }
uint64_t    rtl8139_rx_packets(void) { return dev.rx_packets; }
uint64_t    rtl8139_tx_packets(void) { return dev.tx_packets; }
uint64_t    rtl8139_rx_bytes(void)   { return dev.rx_bytes;   }
uint64_t    rtl8139_tx_bytes(void)   { return dev.tx_bytes;   }
uint64_t    rtl8139_errors(void)     { return dev.errors;     }

bool rtl8139_tx(const void *buf, size_t len) {
    if (!dev.present)         return false;
    if (len == 0)             return false;
    if (len > TX_BUF_SIZE)    return false;

    /*
     * Hunt for a free slot among all NUM_TX_SLOTS. Original code only
     * looked at tx_cur and bailed if busy — losing packets whenever
     * the chip hadn't drained that specific slot yet. Iterate so we
     * use whichever slot is currently available.
     *
     * OWN=1 means "CPU owns this slot" (free or done). OWN=0 + size!=0
     * means "chip is using it" (busy). After init all slots have OWN=1
     * and size=0, so they all match the free condition.
     */
    uint8_t slot = dev.tx_cur;
    int tried = 0;
    while (tried < NUM_TX_SLOTS) {
        uint32_t tsd = inl(dev.io_base + R_TSD0 + slot * 4);
        if ((tsd & TSD_OWN) != 0 || (tsd & 0x1FFFu) == 0) break;
        slot = (uint8_t)((slot + 1) % NUM_TX_SLOTS);
        tried++;
    }
    if (tried == NUM_TX_SLOTS) {
        /* All four slots busy — chip is saturated. Drop the frame;
         * upper layer (TCP) will retransmit. */
        return false;
    }

    uint8_t *dst = dev.tx_buf_virt[slot];
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) dst[i] = src[i];

    /*
     * Fire by writing to TSD: low 13 bits = size. The chip pads short
     * frames to 60 bytes itself; on completion it sets the OWN bit and
     * raises a TOK IRQ.
     */
    outl(dev.io_base + R_TSD0 + slot * 4, (uint32_t)len);

    /* Next time start the hunt one slot ahead. */
    dev.tx_cur = (uint8_t)((slot + 1) % NUM_TX_SLOTS);
    (void)0;
    dev.tx_bytes += (uint64_t)len;
    return true;
}
