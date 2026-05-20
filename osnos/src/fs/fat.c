#include "fat.h"

#include "../drivers/block_ata.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Dirent layout (32 bytes):
 *   0..10   8.3 short name (8 name + 3 ext, space-padded)
 *   11      attribute byte
 *   12..21  reserved / case / create-time
 *   22..23  write time           (we ignore)
 *   24..25  write date           (we ignore)
 *   26..27  first cluster (low)  — FAT16 uses only this half
 *   28..31  file size (LE)
 */
#define DIRENT_SIZE         32

#define ATTR_RO             0x01
#define ATTR_HIDDEN         0x02
#define ATTR_SYSTEM         0x04
#define ATTR_VOL            0x08
#define ATTR_DIR            0x10
#define ATTR_ARCH           0x20
#define ATTR_LFN            0x0F    /* RO|HIDDEN|SYS|VOL — special tag */

#define FAT16_EOF_MIN       0xFFF8
#define FAT16_BAD           0xFFF7

#define SECTOR_SIZE         512

typedef struct {
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint32_t total_sectors;
    uint16_t fat_size_sectors;

    /* Derived. */
    uint32_t fat_lba;             /* first FAT */
    uint32_t root_dir_lba;        /* fixed-region root dir start */
    uint32_t root_dir_sectors;
    uint32_t data_lba;            /* logical LBA of cluster #2 - 2*spc */
} fat_state_t;

static fat_state_t fs;
static bool fs_ready = false;

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint32_t cluster_to_lba(uint16_t cluster) {
    /* Data region starts at the LBA computed from cluster 2. */
    return fs.data_lba + (uint32_t)(cluster - 2) * fs.sectors_per_cluster;
}

/*
 * Look up the FAT entry for `cluster`. *next receives the 16-bit value
 * (next cluster, 0=free, or 0xFFF8..0xFFFF=EOF). Returns 0 on success.
 */
static int fat_get_entry(uint16_t cluster, uint16_t *next) {
    uint32_t byte_off = (uint32_t)cluster * 2;
    uint32_t sec      = fs.fat_lba + byte_off / SECTOR_SIZE;
    uint32_t in_sec   = byte_off % SECTOR_SIZE;

    uint8_t buf[SECTOR_SIZE];
    if (block_ata_read_sector(sec, buf) != 0) return -1;
    *next = rd16(buf + in_sec);
    return 0;
}

int fat_init(void) {
    fs_ready = false;

    if (!block_ata_present()) return -1;

    uint8_t boot[SECTOR_SIZE];
    if (block_ata_read_sector(0, boot) != 0) return -1;
    if (boot[510] != 0x55 || boot[511] != 0xAA) return -1;

    fs.bytes_per_sector    = rd16(boot + 0x0B);
    fs.sectors_per_cluster = boot[0x0D];
    fs.reserved_sectors    = rd16(boot + 0x0E);
    fs.num_fats            = boot[0x10];
    fs.root_entries        = rd16(boot + 0x11);
    uint16_t small_total   = rd16(boot + 0x13);
    fs.fat_size_sectors    = rd16(boot + 0x16);
    uint32_t big_total     = rd32(boot + 0x20);

    if (fs.bytes_per_sector != SECTOR_SIZE)     return -1;
    if (fs.sectors_per_cluster == 0)            return -1;
    if (fs.reserved_sectors == 0)               return -1;
    if (fs.num_fats == 0)                       return -1;
    if (fs.fat_size_sectors == 0)               return -1; /* FAT32 */
    if (fs.root_entries == 0)                   return -1; /* FAT32 */

    fs.total_sectors = small_total ? small_total : big_total;
    if (fs.total_sectors == 0) return -1;

    fs.fat_lba          = fs.reserved_sectors;
    fs.root_dir_lba     = fs.fat_lba + (uint32_t)fs.num_fats * fs.fat_size_sectors;
    fs.root_dir_sectors = (fs.root_entries * DIRENT_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE;
    fs.data_lba         = fs.root_dir_lba + fs.root_dir_sectors;

    /* The FAT type is defined strictly by the cluster count, not by
     * any string in the BPB. FAT16 range: [4085, 65525). */
    uint32_t data_sectors    = fs.total_sectors - fs.data_lba;
    uint32_t total_clusters  = data_sectors / fs.sectors_per_cluster;
    if (total_clusters < 4085 || total_clusters >= 65525) return -1;

    fs_ready = true;
    return 0;
}

bool fat_present(void) { return fs_ready; }

/* ---------------------------------------------------------------- */
/* Name conversion                                                  */
/* ---------------------------------------------------------------- */

static char upcase(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/*
 * Pack a path component into the 11-byte on-disk form.
 *   "readme.txt"  -> "README  TXT"
 *   "foo"         -> "FOO        "
 * Returns true on success; false if the component is too long or
 * contains a forbidden character.
 */
static bool name_to_83(const char *in, char *out11) {
    for (int i = 0; i < 11; i++) out11[i] = ' ';

    int i = 0;
    while (*in && *in != '.') {
        if (i >= 8) return false;
        out11[i++] = upcase(*in++);
    }
    if (*in == '.') {
        in++;
        int j = 0;
        while (*in) {
            if (j >= 3) return false;
            out11[8 + j++] = upcase(*in++);
        }
    }
    return true;
}

/*
 * Render an 11-byte on-disk name as "READMEX.TXT". `out` must hold at
 * least 13 bytes. Trailing spaces trimmed from base and ext.
 */
static void name_from_83(const uint8_t *raw, char *out) {
    int o = 0;
    int base_end = 8;
    while (base_end > 0 && raw[base_end - 1] == ' ') base_end--;
    for (int i = 0; i < base_end; i++) out[o++] = (char)raw[i];

    int ext_end = 11;
    while (ext_end > 8 && raw[ext_end - 1] == ' ') ext_end--;
    if (ext_end > 8) {
        out[o++] = '.';
        for (int i = 8; i < ext_end; i++) out[o++] = (char)raw[i];
    }
    out[o] = 0;
}

static bool name_eq_11(const uint8_t *raw, const char *name11) {
    for (int i = 0; i < 11; i++) {
        if ((char)raw[i] != name11[i]) return false;
    }
    return true;
}

static void parse_dirent(const uint8_t *raw, uint32_t lba, uint32_t off,
                          fat_dirent_t *out) {
    name_from_83(raw, out->name);
    out->is_dir        = (raw[11] & ATTR_DIR) != 0;
    out->first_cluster = rd16(raw + 26);
    out->size          = rd32(raw + 28);
    out->dirent_lba    = lba;
    out->dirent_offset = off;
}

static bool dirent_is_valid(const uint8_t *e) {
    if (e[0] == 0xE5) return false;                          /* deleted */
    if ((e[11] & ATTR_LFN) == ATTR_LFN) return false;        /* LFN slot */
    if (e[11] & ATTR_VOL) return false;                      /* volume label */
    return true;
}

/* ---------------------------------------------------------------- */
/* Directory iteration                                              */
/* ---------------------------------------------------------------- */

/*
 * The root dir lives in a fixed-size region right after the FAT(s).
 * The "global linear entry index" (cursor) covers exactly
 * root_dir_sectors * (SECTOR_SIZE/DIRENT_SIZE) slots.
 *
 * A non-root dir's storage is a cluster chain whose contents are an
 * array of dirent slots. We linearise the chain so the cursor stays a
 * plain integer.
 */

#define ENTRIES_PER_SECTOR  (SECTOR_SIZE / DIRENT_SIZE)

/* Read one raw 32-byte slot at linear index `idx` in `dir`. Returns
 *   0 if the slot was read into `slot_out` and lives at `lba/off`,
 *   1 if `idx` is past end-of-directory (0x00 marker or chain end),
 *  -1 on I/O error. */
static int read_dir_slot(const fat_dirent_t *dir, uint32_t idx,
                          uint8_t slot_out[DIRENT_SIZE],
                          uint32_t *lba_out, uint32_t *off_out) {
    uint8_t buf[SECTOR_SIZE];

    if (dir->first_cluster == 0) {
        uint32_t total = fs.root_dir_sectors * ENTRIES_PER_SECTOR;
        if (idx >= total) return 1;

        uint32_t s = idx / ENTRIES_PER_SECTOR;
        uint32_t i = idx % ENTRIES_PER_SECTOR;
        uint32_t lba = fs.root_dir_lba + s;
        if (block_ata_read_sector(lba, buf) != 0) return -1;

        uint8_t *e = buf + i * DIRENT_SIZE;
        if (e[0] == 0x00) return 1;          /* end marker */
        for (uint32_t k = 0; k < DIRENT_SIZE; k++) slot_out[k] = e[k];
        *lba_out = lba;
        *off_out = i * DIRENT_SIZE;
        return 0;
    }

    /* Subdirectory: walk cluster chain. */
    uint32_t entries_per_cluster = fs.sectors_per_cluster * ENTRIES_PER_SECTOR;
    uint32_t cluster_idx = idx / entries_per_cluster;
    uint32_t in_cluster  = idx % entries_per_cluster;

    uint16_t cluster = dir->first_cluster;
    for (uint32_t c = 0; c < cluster_idx; c++) {
        if (cluster < 2 || cluster >= FAT16_EOF_MIN) return 1;
        uint16_t next;
        if (fat_get_entry(cluster, &next) != 0) return -1;
        cluster = next;
    }
    if (cluster < 2 || cluster >= FAT16_EOF_MIN) return 1;

    uint32_t s = in_cluster / ENTRIES_PER_SECTOR;
    uint32_t i = in_cluster % ENTRIES_PER_SECTOR;
    uint32_t lba = cluster_to_lba(cluster) + s;
    if (block_ata_read_sector(lba, buf) != 0) return -1;
    uint8_t *e = buf + i * DIRENT_SIZE;
    if (e[0] == 0x00) return 1;
    for (uint32_t k = 0; k < DIRENT_SIZE; k++) slot_out[k] = e[k];
    *lba_out = lba;
    *off_out = i * DIRENT_SIZE;
    return 0;
}

int fat_readdir(const fat_dirent_t *dir, uint32_t cursor,
                fat_dirent_t *out, uint32_t *next_cursor) {
    if (!fs_ready) return -1;
    if (!dir->is_dir) return -1;

    uint32_t idx = cursor;
    for (;;) {
        uint8_t slot[DIRENT_SIZE];
        uint32_t lba, off;
        int rc = read_dir_slot(dir, idx, slot, &lba, &off);
        if (rc == 1)  return -1;  /* end of directory */
        if (rc < 0)   return -1;
        idx++;
        if (!dirent_is_valid(slot)) continue;
        parse_dirent(slot, lba, off, out);
        *next_cursor = idx;
        return 0;
    }
}

/* ---------------------------------------------------------------- */
/* Path lookup                                                      */
/* ---------------------------------------------------------------- */

static int dir_find_by_83(const fat_dirent_t *dir, const char *name11,
                          fat_dirent_t *out) {
    uint32_t idx = 0;
    for (;;) {
        uint8_t slot[DIRENT_SIZE];
        uint32_t lba, off;
        int rc = read_dir_slot(dir, idx, slot, &lba, &off);
        if (rc == 1) return -1;
        if (rc < 0)  return -1;
        idx++;
        if (!dirent_is_valid(slot)) continue;
        if (!name_eq_11(slot, name11)) continue;
        parse_dirent(slot, lba, off, out);
        return 0;
    }
}

static void root_sentinel(fat_dirent_t *out) {
    out->name[0] = '/';
    out->name[1] = 0;
    out->is_dir = true;
    out->size = 0;
    out->first_cluster = 0;
    out->dirent_lba = 0;
    out->dirent_offset = 0;
}

int fat_lookup(const char *path, fat_dirent_t *out) {
    if (!fs_ready) return -1;
    if (!path)     return -1;

    while (*path == '/') path++;
    if (*path == 0) {
        root_sentinel(out);
        return 0;
    }

    fat_dirent_t cur;
    root_sentinel(&cur);

    while (*path) {
        char comp[64];
        size_t n = 0;
        while (*path && *path != '/' && n < sizeof(comp) - 1) {
            comp[n++] = *path++;
        }
        comp[n] = 0;
        while (*path == '/') path++;
        if (n == 0) continue;

        if (!cur.is_dir) return -1;

        char name11[11];
        if (!name_to_83(comp, name11)) return -1;

        fat_dirent_t next;
        if (dir_find_by_83(&cur, name11, &next) != 0) return -1;
        cur = next;
    }

    *out = cur;
    return 0;
}

/* ---------------------------------------------------------------- */
/* File read                                                        */
/* ---------------------------------------------------------------- */

int fat_read_file(const fat_dirent_t *de, uint32_t off,
                  char *buf, uint32_t len) {
    if (!fs_ready) return -1;
    if (de->is_dir) return -1;
    if (off >= de->size) return 0;
    if (len > de->size - off) len = de->size - off;
    if (len == 0) return 0;

    uint32_t cluster_size = (uint32_t)fs.sectors_per_cluster * SECTOR_SIZE;
    uint32_t clusters_to_skip = off / cluster_size;
    uint32_t in_cluster_off   = off % cluster_size;

    uint16_t cluster = de->first_cluster;
    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        if (cluster < 2 || cluster >= FAT16_EOF_MIN) return -1;
        uint16_t next;
        if (fat_get_entry(cluster, &next) != 0) return -1;
        cluster = next;
    }

    uint32_t total = 0;
    uint8_t  sbuf[SECTOR_SIZE];

    while (total < len) {
        if (cluster < 2 || cluster >= FAT16_EOF_MIN) break;

        uint32_t lba_start = cluster_to_lba(cluster);
        uint32_t sec_in    = in_cluster_off / SECTOR_SIZE;
        uint32_t sec_off   = in_cluster_off % SECTOR_SIZE;

        while (total < len && sec_in < fs.sectors_per_cluster) {
            if (block_ata_read_sector(lba_start + sec_in, sbuf) != 0)
                return -1;
            uint32_t chunk = SECTOR_SIZE - sec_off;
            if (chunk > len - total) chunk = len - total;
            for (uint32_t i = 0; i < chunk; i++) {
                buf[total + i] = (char)sbuf[sec_off + i];
            }
            total += chunk;
            sec_off = 0;
            sec_in++;
        }
        in_cluster_off = 0;

        if (total < len) {
            uint16_t next;
            if (fat_get_entry(cluster, &next) != 0) return -1;
            cluster = next;
        }
    }

    return (int)total;
}
