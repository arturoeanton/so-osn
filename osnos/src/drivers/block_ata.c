#include "block_ata.h"

#include <stdbool.h>
#include <stdint.h>

/* Primary IDE channel — only one we touch. */
#define ATA_PRI_CMD_BASE    0x1F0
#define ATA_PRI_CTRL_BASE   0x3F6

/* Command-block register offsets from ATA_PRI_CMD_BASE. */
#define ATA_REG_DATA        0   /* 16-bit data transfer port */
#define ATA_REG_ERROR       1   /* read: last error */
#define ATA_REG_SECCOUNT    2
#define ATA_REG_LBA_LO      3
#define ATA_REG_LBA_MID     4
#define ATA_REG_LBA_HI      5
#define ATA_REG_DRIVE       6   /* drive select + LBA[27:24] */
#define ATA_REG_STATUS      7   /* read */
#define ATA_REG_COMMAND     7   /* write */

/* Status register bits. */
#define ATA_SR_BSY          0x80
#define ATA_SR_DRDY         0x40
#define ATA_SR_DF           0x20    /* drive fault */
#define ATA_SR_DRQ          0x08
#define ATA_SR_ERR          0x01

/* Drive/head select bits. */
#define ATA_DRIVE_MASTER    0xA0
#define ATA_DRIVE_LBA       0x40

/* Commands. */
#define ATA_CMD_READ_SECTORS    0x20    /* LBA28 PIO read */
#define ATA_CMD_WRITE_SECTORS   0x30    /* LBA28 PIO write */
#define ATA_CMD_FLUSH_CACHE     0xE7
#define ATA_CMD_IDENTIFY        0xEC

/*
 * Poll budgets. PIO operations finish in microseconds on QEMU; these
 * loops exist purely so a wedged controller doesn't hang the boot.
 * Generous but bounded.
 */
#define ATA_POLL_BUDGET 1000000

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
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

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" :: "a"(value), "Nd"(port));
}

static struct {
    bool     present;
    uint64_t sectors;
    char     model[41];     /* 40 bytes + NUL */
} ata;

/*
 * After issuing a command we need a ~400ns settle before reading
 * status. Four alt-status reads cost ~30ns each on real silicon and
 * are the canonical way to spend that delay on x86.
 */
static inline void ata_io_wait(void) {
    inb(ATA_PRI_CTRL_BASE);
    inb(ATA_PRI_CTRL_BASE);
    inb(ATA_PRI_CTRL_BASE);
    inb(ATA_PRI_CTRL_BASE);
}

static int ata_wait_not_busy(void) {
    for (int i = 0; i < ATA_POLL_BUDGET; i++) {
        uint8_t s = inb(ATA_PRI_CMD_BASE + ATA_REG_STATUS);
        if (!(s & ATA_SR_BSY)) {
            if (s & (ATA_SR_ERR | ATA_SR_DF)) return -1;
            return 0;
        }
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < ATA_POLL_BUDGET; i++) {
        uint8_t s = inb(ATA_PRI_CMD_BASE + ATA_REG_STATUS);
        if (s & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

static void ata_select_drive_lba(uint32_t lba) {
    /* master + LBA mode + LBA[27:24] in low nibble. */
    outb(ATA_PRI_CMD_BASE + ATA_REG_DRIVE,
         (uint8_t)(ATA_DRIVE_MASTER | ATA_DRIVE_LBA |
                   ((lba >> 24) & 0x0F)));
    ata_io_wait();
}

static int ata_identify(void) {
    /* Select primary master with LBA bit cleared (IDENTIFY protocol). */
    outb(ATA_PRI_CMD_BASE + ATA_REG_DRIVE, ATA_DRIVE_MASTER);
    ata_io_wait();

    outb(ATA_PRI_CMD_BASE + ATA_REG_SECCOUNT, 0);
    outb(ATA_PRI_CMD_BASE + ATA_REG_LBA_LO,   0);
    outb(ATA_PRI_CMD_BASE + ATA_REG_LBA_MID,  0);
    outb(ATA_PRI_CMD_BASE + ATA_REG_LBA_HI,   0);

    outb(ATA_PRI_CMD_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    /* Floating bus when no drive: status reads 0xFF or 0x00. */
    uint8_t s = inb(ATA_PRI_CMD_BASE + ATA_REG_STATUS);
    if (s == 0 || s == 0xFF) return -1;

    if (ata_wait_not_busy() < 0) return -1;

    /* ATAPI / SATAPI etch their signature in LBA mid/high; we only
     * accept plain ATA disks here. */
    if (inb(ATA_PRI_CMD_BASE + ATA_REG_LBA_MID) != 0) return -1;
    if (inb(ATA_PRI_CMD_BASE + ATA_REG_LBA_HI)  != 0) return -1;

    if (ata_wait_drq() < 0) return -1;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) {
        id[i] = inw(ATA_PRI_CMD_BASE + ATA_REG_DATA);
    }

    /* LBA28 total addressable sectors at id[60..61] (32-bit LE). */
    ata.sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);

    /* Model string lives in id[27..46], 40 ASCII bytes, each 16-bit
     * word byte-swapped (per ATA spec — high byte is first char). */
    for (int i = 0; i < 20; i++) {
        ata.model[i*2 + 0] = (char)(id[27 + i] >> 8);
        ata.model[i*2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    ata.model[40] = 0;
    /* Trim trailing pad spaces. */
    for (int i = 39; i >= 0 && ata.model[i] == ' '; i--) {
        ata.model[i] = 0;
    }

    ata.present = true;
    return 0;
}

void block_ata_init(void) {
    ata.present = false;
    ata.sectors = 0;
    ata.model[0] = 0;
    (void)ata_identify();
}

bool        block_ata_present     (void) { return ata.present; }
uint64_t    block_ata_sector_count(void) { return ata.sectors; }
const char *block_ata_model       (void) { return ata.model;   }

int block_ata_read_sector(uint64_t lba, void *buf) {
    if (!ata.present)        return -1;
    if (lba >= ata.sectors)  return -1;
    if (lba >> 28)           return -1;

    if (ata_wait_not_busy() < 0) return -1;

    ata_select_drive_lba((uint32_t)lba);
    outb(ATA_PRI_CMD_BASE + ATA_REG_SECCOUNT, 1);
    outb(ATA_PRI_CMD_BASE + ATA_REG_LBA_LO,  (uint8_t)(lba));
    outb(ATA_PRI_CMD_BASE + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_PRI_CMD_BASE + ATA_REG_LBA_HI,  (uint8_t)(lba >> 16));
    outb(ATA_PRI_CMD_BASE + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    if (ata_wait_drq() < 0) return -1;

    uint16_t *p = (uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        p[i] = inw(ATA_PRI_CMD_BASE + ATA_REG_DATA);
    }
    ata_io_wait();
    return 0;
}

int block_ata_write_sector(uint64_t lba, const void *buf) {
    if (!ata.present)        return -1;
    if (lba >= ata.sectors)  return -1;
    if (lba >> 28)           return -1;

    if (ata_wait_not_busy() < 0) return -1;

    ata_select_drive_lba((uint32_t)lba);
    outb(ATA_PRI_CMD_BASE + ATA_REG_SECCOUNT, 1);
    outb(ATA_PRI_CMD_BASE + ATA_REG_LBA_LO,  (uint8_t)(lba));
    outb(ATA_PRI_CMD_BASE + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_PRI_CMD_BASE + ATA_REG_LBA_HI,  (uint8_t)(lba >> 16));
    outb(ATA_PRI_CMD_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    if (ata_wait_drq() < 0) return -1;

    const uint16_t *p = (const uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        outw(ATA_PRI_CMD_BASE + ATA_REG_DATA, p[i]);
    }

    /* Flush so the write is persisted before we return — write-through
     * semantics, no buffer cache at this layer. */
    outb(ATA_PRI_CMD_BASE + ATA_REG_COMMAND, ATA_CMD_FLUSH_CACHE);
    if (ata_wait_not_busy() < 0) return -1;
    return 0;
}
