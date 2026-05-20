#include "fat.h"

#include "../drivers/block_ata.h"
#include "../lib/string.h"

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

/*
 * Locate slot `idx` in `dir` and copy its raw 32 bytes. Does NOT
 * interpret the marker byte — callers decide.
 *   0 → slot exists; lba/off filled
 *   1 → idx is past the directory's allocated storage (root region
 *       exhausted, or cluster chain truly ended). Caller may then
 *       choose to extend the dir.
 *  -1 → I/O error
 */
static int dir_slot_locate(const fat_dirent_t *dir, uint32_t idx,
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
        for (uint32_t k = 0; k < DIRENT_SIZE; k++) slot_out[k] = e[k];
        *lba_out = lba;
        *off_out = i * DIRENT_SIZE;
        return 0;
    }

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
    for (uint32_t k = 0; k < DIRENT_SIZE; k++) slot_out[k] = e[k];
    *lba_out = lba;
    *off_out = i * DIRENT_SIZE;
    return 0;
}

/* read_dir_slot: same as locate, plus 0x00 first-byte → 1 (end-of-dir).
 * This is what iteration / lookup want — bail at the first 0x00 marker
 * because everything past it is guaranteed unused. */
static int read_dir_slot(const fat_dirent_t *dir, uint32_t idx,
                          uint8_t slot_out[DIRENT_SIZE],
                          uint32_t *lba_out, uint32_t *off_out) {
    int rc = dir_slot_locate(dir, idx, slot_out, lba_out, off_out);
    if (rc != 0) return rc;
    if (slot_out[0] == 0x00) return 1;
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

/* ================================================================ */
/* Write side (FASE 8.4)                                            */
/* ================================================================ */

/* errno-style returns; sign-flipped to fit the int convention. */
#define FAT_OK         0
#define FAT_EIO        -5
#define FAT_ENOENT     -2
#define FAT_EEXIST     -17
#define FAT_ENOTDIR    -20
#define FAT_EISDIR     -21
#define FAT_EINVAL     -22
#define FAT_ENOSPC     -28
#define FAT_ENOTEMPTY  -39

/*
 * Write `value` into FAT entry for `cluster`, replicating across every
 * FAT copy (mirror). Without the mirror update, the on-disk image
 * fails fsck and any OS that mounts the second FAT first sees stale
 * chains.
 */
static int fat_set_entry(uint16_t cluster, uint16_t value) {
    uint8_t buf[SECTOR_SIZE];
    for (uint8_t fat_idx = 0; fat_idx < fs.num_fats; fat_idx++) {
        uint32_t fat_base = fs.fat_lba +
                            (uint32_t)fat_idx * fs.fat_size_sectors;
        uint32_t byte_off = (uint32_t)cluster * 2;
        uint32_t sec      = fat_base + byte_off / SECTOR_SIZE;
        uint32_t in_sec   = byte_off % SECTOR_SIZE;

        if (block_ata_read_sector(sec, buf) != 0) return -1;
        buf[in_sec]     = (uint8_t)(value);
        buf[in_sec + 1] = (uint8_t)(value >> 8);
        if (block_ata_write_sector(sec, buf) != 0) return -1;
    }
    return 0;
}

/* Find first free FAT entry (== 0) starting at cluster 2 and claim it
 * by marking it as EOF. Returns the cluster number, or 0 if disk full. */
static uint16_t fat_alloc_cluster(void) {
    uint32_t max_entries = (uint32_t)fs.fat_size_sectors * SECTOR_SIZE / 2;
    if (max_entries > 65525) max_entries = 65525;

    uint8_t buf[SECTOR_SIZE];
    uint32_t cached_sec = (uint32_t)-1;

    for (uint32_t c = 2; c < max_entries; c++) {
        uint32_t byte_off = c * 2;
        uint32_t sec      = fs.fat_lba + byte_off / SECTOR_SIZE;
        uint32_t in_sec   = byte_off % SECTOR_SIZE;

        if (sec != cached_sec) {
            if (block_ata_read_sector(sec, buf) != 0) return 0;
            cached_sec = sec;
        }
        uint16_t val = (uint16_t)buf[in_sec] | ((uint16_t)buf[in_sec + 1] << 8);
        if (val == 0) {
            if (fat_set_entry((uint16_t)c, 0xFFFF) != 0) return 0;
            return (uint16_t)c;
        }
    }
    return 0;
}

/* Walk chain, mark every entry as free (0). Stops at EOF or invalid. */
static int fat_free_chain(uint16_t first) {
    uint16_t cur = first;
    while (cur >= 2 && cur < FAT16_EOF_MIN) {
        uint16_t next;
        if (fat_get_entry(cur, &next) != 0) return -1;
        if (fat_set_entry(cur, 0) != 0) return -1;
        cur = next;
    }
    return 0;
}

/* Allocate a chain of `count` linked clusters. *first_out gets the head.
 * On any failure frees what was allocated so far. count==0 → first=0. */
static int fat_alloc_chain(uint32_t count, uint16_t *first_out) {
    *first_out = 0;
    if (count == 0) return 0;

    uint16_t first = 0;
    uint16_t prev  = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint16_t c = fat_alloc_cluster();
        if (c == 0) {
            if (first != 0) fat_free_chain(first);
            return FAT_ENOSPC;
        }
        if (first == 0) {
            first = c;
        } else if (fat_set_entry(prev, c) != 0) {
            fat_free_chain(first);
            return FAT_EIO;
        }
        prev = c;
    }
    *first_out = first;
    return 0;
}

/* Copy `len` bytes from `src` into the cluster chain starting at
 * `first`, padding the final sector with zeros. Caller pre-allocated
 * enough clusters. */
static int write_data_into_chain(uint16_t first, const char *src, uint32_t len) {
    uint16_t cluster = first;
    uint32_t written = 0;
    uint8_t  sbuf[SECTOR_SIZE];

    while (written < len) {
        if (cluster < 2 || cluster >= FAT16_EOF_MIN) return FAT_EIO;

        uint32_t lba_start = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < fs.sectors_per_cluster && written < len; s++) {
            uint32_t chunk = SECTOR_SIZE;
            if (chunk > len - written) chunk = len - written;

            for (uint32_t k = 0; k < chunk; k++) {
                sbuf[k] = (uint8_t)src[written + k];
            }
            for (uint32_t k = chunk; k < SECTOR_SIZE; k++) sbuf[k] = 0;

            if (block_ata_write_sector(lba_start + s, sbuf) != 0) return FAT_EIO;
            written += chunk;
        }

        if (written < len) {
            uint16_t next;
            if (fat_get_entry(cluster, &next) != 0) return FAT_EIO;
            cluster = next;
        }
    }
    return 0;
}

/* ----- dirent slot writes ----- */

static int read_modify_write_dirent(uint32_t lba, uint32_t off,
                                     const uint8_t raw[DIRENT_SIZE]) {
    uint8_t buf[SECTOR_SIZE];
    if (block_ata_read_sector(lba, buf) != 0) return FAT_EIO;
    for (uint32_t i = 0; i < DIRENT_SIZE; i++) buf[off + i] = raw[i];
    if (block_ata_write_sector(lba, buf) != 0) return FAT_EIO;
    return 0;
}

static void build_dirent_raw(uint8_t raw[DIRENT_SIZE],
                              const char *name11, uint8_t attr,
                              uint16_t first_cluster, uint32_t size) {
    for (int i = 0; i < DIRENT_SIZE; i++) raw[i] = 0;
    for (int i = 0; i < 11; i++) raw[i] = (uint8_t)name11[i];
    raw[11] = attr;
    /* 12..21 left zero — case, create-time, last-access-date.
     * 22..25 left zero — write time/date (we have no clock yet). */
    raw[26] = (uint8_t)(first_cluster);
    raw[27] = (uint8_t)(first_cluster >> 8);
    raw[28] = (uint8_t)(size);
    raw[29] = (uint8_t)(size >> 8);
    raw[30] = (uint8_t)(size >> 16);
    raw[31] = (uint8_t)(size >> 24);
}

/* Locate a slot for a new entry in `dir`. Reuses the first 0xE5
 * (deleted) slot, else claims the first 0x00 slot encountered (and
 * the post-mformat-zero invariant keeps the next slot as 0x00 so the
 * end-of-dir marker is preserved). Returns FAT_ENOSPC if exhausted. */
static int find_free_dir_slot(const fat_dirent_t *dir,
                               uint32_t *lba_out, uint32_t *off_out) {
    uint32_t idx = 0;
    for (;;) {
        uint8_t slot[DIRENT_SIZE];
        uint32_t lba, off;
        int rc = dir_slot_locate(dir, idx, slot, &lba, &off);
        if (rc < 0) return FAT_EIO;
        if (rc == 1) return FAT_ENOSPC;     /* dir would need extending */
        if (slot[0] == 0xE5 || slot[0] == 0x00) {
            *lba_out = lba;
            *off_out = off;
            return 0;
        }
        idx++;
    }
}

/* Resolve `parent_path` (which may end with '/') and split out the
 * last component as `base_out`. Examples:
 *   "/foo/bar"  → parent dir from path "/foo", base "bar"
 *   "/bar"      → root, base "bar"
 *   "/"         → invalid (no base)
 */
static int split_parent_base(const char *path,
                              char parent_path[64],
                              char base_out[64]) {
    /* Skip leading slashes. */
    while (*path == '/') path++;
    if (*path == 0) return FAT_EINVAL;

    /* Find last '/' in the remainder. */
    const char *last_slash = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    if (!last_slash) {
        parent_path[0] = '/';
        parent_path[1] = 0;
        int i;
        for (i = 0; i < 62 && path[i]; i++) base_out[i] = path[i];
        base_out[i] = 0;
        return base_out[0] ? 0 : FAT_EINVAL;
    }

    /* parent = "/" + path[..last_slash) */
    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len + 2 > 64) return FAT_EINVAL;
    parent_path[0] = '/';
    for (size_t i = 0; i < parent_len; i++) parent_path[1 + i] = path[i];
    parent_path[1 + parent_len] = 0;

    const char *base = last_slash + 1;
    int i;
    for (i = 0; i < 62 && base[i]; i++) base_out[i] = base[i];
    base_out[i] = 0;
    return base_out[0] ? 0 : FAT_EINVAL;
}

/* ----- public write API ----- */

int fat_unlink_path(const char *path) {
    if (!fs_ready) return FAT_EIO;

    fat_dirent_t de;
    if (fat_lookup(path, &de) != 0) return FAT_ENOENT;
    if (de.is_dir) return FAT_EISDIR;
    if (de.dirent_lba == 0) return FAT_EINVAL;       /* root sentinel */

    if (fat_free_chain(de.first_cluster) != 0) return FAT_EIO;

    /* Patch the first byte of the dirent to 0xE5 (deleted). RMW of the
     * sector that contains it — block_ata_read_sector always transfers
     * 512 B so the buffer MUST be sector-sized, never DIRENT_SIZE. */
    uint8_t sb[SECTOR_SIZE];
    if (block_ata_read_sector(de.dirent_lba, sb) != 0) return FAT_EIO;
    sb[de.dirent_offset] = 0xE5;
    if (block_ata_write_sector(de.dirent_lba, sb) != 0) return FAT_EIO;
    return 0;
}

/*
 * Common path for write/append: prepare `*existing` (whether the target
 * exists, and where its dirent lives). Returns FAT_OK and fills the
 * out params; on missing-path returns FAT_OK with `exists=false` and
 * the parent_dir/name11 fields populated for create.
 */
typedef struct {
    bool          exists;
    fat_dirent_t  de;            /* valid only when exists==true */
    fat_dirent_t  parent_dir;    /* valid in both cases */
    char          name11[11];    /* 8.3 form of last component */
} write_target_t;

static int resolve_write_target(const char *path, write_target_t *out) {
    fat_dirent_t de;
    if (fat_lookup(path, &de) == 0) {
        if (de.is_dir) return FAT_EISDIR;
        if (de.dirent_lba == 0) return FAT_EINVAL;
        out->exists     = true;
        out->de         = de;
        out->parent_dir.is_dir = true;
        out->parent_dir.first_cluster = 0;   /* unused on overwrite */
        for (int i = 0; i < 11; i++) out->name11[i] = ' ';
        return 0;
    }

    char parent_path[64];
    char base[64];
    int rc = split_parent_base(path, parent_path, base);
    if (rc != 0) return rc;

    if (fat_lookup(parent_path, &out->parent_dir) != 0) return FAT_ENOENT;
    if (!out->parent_dir.is_dir) return FAT_ENOTDIR;

    if (!name_to_83(base, out->name11)) return FAT_EINVAL;
    out->exists = false;
    return 0;
}

/* Write/replace file content. Strategy:
 *   1. Allocate the new chain (if any data).
 *   2. Write data into the chain.
 *   3. Update / create the dirent so it points at the new chain.
 *   4. Free the old chain (overwrite case only).
 * Crash between step 1 and step 3 leaks clusters but never corrupts
 * the visible filesystem.
 */
int fat_write_path(const char *path, const char *buf, uint32_t len) {
    if (!fs_ready) return FAT_EIO;

    write_target_t t;
    int rc = resolve_write_target(path, &t);
    if (rc != 0) return rc;

    uint32_t cluster_size = (uint32_t)fs.sectors_per_cluster * SECTOR_SIZE;
    uint32_t clusters_needed = (len + cluster_size - 1) / cluster_size;

    uint16_t new_chain = 0;
    rc = fat_alloc_chain(clusters_needed, &new_chain);
    if (rc != 0) return rc;

    if (len > 0) {
        rc = write_data_into_chain(new_chain, buf, len);
        if (rc != 0) {
            fat_free_chain(new_chain);
            return rc;
        }
    }

    if (t.exists) {
        uint16_t old_chain = t.de.first_cluster;
        uint8_t raw[DIRENT_SIZE];
        uint8_t sb[SECTOR_SIZE];
        if (block_ata_read_sector(t.de.dirent_lba, sb) != 0) {
            fat_free_chain(new_chain);
            return FAT_EIO;
        }
        for (uint32_t i = 0; i < DIRENT_SIZE; i++) {
            raw[i] = sb[t.de.dirent_offset + i];
        }
        raw[26] = (uint8_t)(new_chain);
        raw[27] = (uint8_t)(new_chain >> 8);
        raw[28] = (uint8_t)(len);
        raw[29] = (uint8_t)(len >> 8);
        raw[30] = (uint8_t)(len >> 16);
        raw[31] = (uint8_t)(len >> 24);
        rc = read_modify_write_dirent(t.de.dirent_lba, t.de.dirent_offset, raw);
        if (rc != 0) {
            fat_free_chain(new_chain);
            return rc;
        }
        if (old_chain >= 2 && old_chain < FAT16_EOF_MIN) {
            fat_free_chain(old_chain);
        }
        return 0;
    }

    /* Create a fresh dirent in parent. */
    uint32_t slot_lba, slot_off;
    rc = find_free_dir_slot(&t.parent_dir, &slot_lba, &slot_off);
    if (rc != 0) {
        fat_free_chain(new_chain);
        return rc;
    }

    uint8_t raw[DIRENT_SIZE];
    build_dirent_raw(raw, t.name11, ATTR_ARCH, new_chain, len);
    rc = read_modify_write_dirent(slot_lba, slot_off, raw);
    if (rc != 0) {
        fat_free_chain(new_chain);
        return rc;
    }
    return 0;
}

/* Append: cheap implementation. Read existing → tack on → rewrite.
 * Bounded by the 8 KiB scratch buffer; bigger appends fail. Enough
 * for `echo … >>` use-cases. */
#define FAT_APPEND_SCRATCH 8192

int fat_append_path(const char *path, const char *buf, uint32_t len) {
    if (!fs_ready) return FAT_EIO;

    static char scratch[FAT_APPEND_SCRATCH];

    fat_dirent_t de;
    int looked_up = fat_lookup(path, &de);

    uint32_t existing_size = 0;
    if (looked_up == 0) {
        if (de.is_dir) return FAT_EISDIR;
        existing_size = de.size;
        if (existing_size > FAT_APPEND_SCRATCH) return FAT_ENOSPC;
        if (existing_size > 0) {
            int n = fat_read_file(&de, 0, scratch, existing_size);
            if (n < 0 || (uint32_t)n != existing_size) return FAT_EIO;
        }
    }
    if (existing_size + len > FAT_APPEND_SCRATCH) return FAT_ENOSPC;
    for (uint32_t i = 0; i < len; i++) {
        scratch[existing_size + i] = buf[i];
    }
    return fat_write_path(path, scratch, existing_size + len);
}

/* mkdir: create empty subdir cluster with "." and ".." entries. */
int fat_mkdir_path(const char *path) {
    if (!fs_ready) return FAT_EIO;

    fat_dirent_t existing;
    if (fat_lookup(path, &existing) == 0) return FAT_EEXIST;

    char parent_path[64], base[64];
    int rc = split_parent_base(path, parent_path, base);
    if (rc != 0) return rc;

    fat_dirent_t parent;
    if (fat_lookup(parent_path, &parent) != 0) return FAT_ENOENT;
    if (!parent.is_dir) return FAT_ENOTDIR;

    char name11[11];
    if (!name_to_83(base, name11)) return FAT_EINVAL;

    uint16_t dir_cluster = fat_alloc_cluster();
    if (dir_cluster == 0) return FAT_ENOSPC;

    /* Write . and .. + zeros to the whole cluster (zero = end-of-dir). */
    uint8_t sec[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) sec[i] = 0;

    char dot11[11]    = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    char dotdot11[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };

    uint8_t dot_raw[DIRENT_SIZE];
    uint8_t dotdot_raw[DIRENT_SIZE];
    build_dirent_raw(dot_raw, dot11, ATTR_DIR, dir_cluster, 0);
    /* ".." → parent's first cluster (0 means "the root", FAT convention). */
    build_dirent_raw(dotdot_raw, dotdot11, ATTR_DIR, parent.first_cluster, 0);

    for (uint32_t i = 0; i < DIRENT_SIZE; i++) sec[i] = dot_raw[i];
    for (uint32_t i = 0; i < DIRENT_SIZE; i++) sec[DIRENT_SIZE + i] = dotdot_raw[i];

    /* Cluster spans `sectors_per_cluster` sectors. Write . / .. in the
     * first sector; zero the rest. */
    uint32_t lba_start = cluster_to_lba(dir_cluster);
    if (block_ata_write_sector(lba_start, sec) != 0) {
        fat_free_chain(dir_cluster);
        return FAT_EIO;
    }
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) sec[i] = 0;
    for (uint32_t s = 1; s < fs.sectors_per_cluster; s++) {
        if (block_ata_write_sector(lba_start + s, sec) != 0) {
            fat_free_chain(dir_cluster);
            return FAT_EIO;
        }
    }

    /* Add dirent to parent. */
    uint32_t slot_lba, slot_off;
    rc = find_free_dir_slot(&parent, &slot_lba, &slot_off);
    if (rc != 0) {
        fat_free_chain(dir_cluster);
        return rc;
    }
    uint8_t raw[DIRENT_SIZE];
    build_dirent_raw(raw, name11, ATTR_DIR, dir_cluster, 0);
    rc = read_modify_write_dirent(slot_lba, slot_off, raw);
    if (rc != 0) {
        fat_free_chain(dir_cluster);
        return rc;
    }
    return 0;
}

/* rmdir: empty subdir, then unlink-like teardown. */
int fat_rmdir_path(const char *path) {
    if (!fs_ready) return FAT_EIO;

    fat_dirent_t de;
    if (fat_lookup(path, &de) != 0) return FAT_ENOENT;
    if (!de.is_dir) return FAT_ENOTDIR;
    if (de.dirent_lba == 0) return FAT_EINVAL;       /* root */

    /* Scan dir entries; any valid one other than "." / ".." → not empty. */
    uint32_t idx = 0;
    for (;;) {
        uint8_t slot[DIRENT_SIZE];
        uint32_t lba, off;
        int rc = read_dir_slot(&de, idx, slot, &lba, &off);
        if (rc < 0) return FAT_EIO;
        if (rc == 1) break;
        idx++;
        if (!dirent_is_valid(slot)) continue;
        /* "." / ".." — name11 first byte is '.' and (slot[1]==' ' or '.') */
        if (slot[0] == '.' && (slot[1] == ' ' || slot[1] == '.')) continue;
        return FAT_ENOTEMPTY;
    }

    if (fat_free_chain(de.first_cluster) != 0) return FAT_EIO;

    uint8_t sb[SECTOR_SIZE];
    if (block_ata_read_sector(de.dirent_lba, sb) != 0) return FAT_EIO;
    sb[de.dirent_offset] = 0xE5;
    if (block_ata_write_sector(de.dirent_lba, sb) != 0) return FAT_EIO;
    return 0;
}

/*
 * Rename / move within the FAT volume.
 *   - Same parent → O(1): overwrite the 11 name bytes in src's dirent.
 *   - Cross-directory → reuse the existing FAT chain by copying src's
 *     dirent (with the new name) into a free slot in dst's parent,
 *     then marking src's dirent 0xE5.
 *
 * The cross-dir order is deliberate: src-deleted FIRST, then dst
 * written. A crash mid-rename leaks the file (visible to fsck) rather
 * than cross-linking it (both directories pointing at the same chain).
 * We pre-reserve the dst slot before deleting src so ENOSPC fails
 * fast without losing data.
 *
 * Overwrite of an existing dst is rejected (EEXIST). The caller is
 * expected to unlink dst first if it wants Unix-style overwrite.
 * Renaming a path to itself (case-insensitive, same on-disk dirent)
 * is a no-op success.
 */
int fat_rename_path(const char *src, const char *dst) {
    if (!fs_ready) return FAT_EIO;

    fat_dirent_t src_de;
    if (fat_lookup(src, &src_de) != 0) return FAT_ENOENT;
    if (src_de.dirent_lba == 0) return FAT_EINVAL;   /* root */

    /* If dst resolves to the SAME on-disk dirent as src (different
     * spellings of the same 8.3 name, or trailing slashes), no-op. */
    fat_dirent_t dst_de;
    if (fat_lookup(dst, &dst_de) == 0) {
        if (dst_de.dirent_lba == src_de.dirent_lba &&
            dst_de.dirent_offset == src_de.dirent_offset) {
            return 0;
        }
        return FAT_EEXIST;
    }

    /* Resolve dst into parent dir + new 8.3 name. */
    char dst_parent_path[64];
    char dst_base[64];
    int rc = split_parent_base(dst, dst_parent_path, dst_base);
    if (rc != 0) return rc;

    fat_dirent_t dst_parent;
    if (fat_lookup(dst_parent_path, &dst_parent) != 0) return FAT_ENOENT;
    if (!dst_parent.is_dir) return FAT_ENOTDIR;

    char dst_name11[11];
    if (!name_to_83(dst_base, dst_name11)) return FAT_EINVAL;

    /* Read src's dirent sector once; we'll need its 32 raw bytes
     * either way (fast path patches in place; cross-dir uses them
     * as the template for the new entry). */
    uint8_t sb_src[SECTOR_SIZE];
    if (block_ata_read_sector(src_de.dirent_lba, sb_src) != 0) return FAT_EIO;

    /* Discover src's parent so we can compare against dst_parent. */
    char src_parent_path[64];
    char src_base[64];
    rc = split_parent_base(src, src_parent_path, src_base);
    if (rc != 0) return rc;
    fat_dirent_t src_parent;
    if (fat_lookup(src_parent_path, &src_parent) != 0) return FAT_EIO;

    /* Fast path: same parent. Both first_cluster fields agree (root
     * sentinel is 0, identical across both). One sector RMW. */
    if (src_parent.first_cluster == dst_parent.first_cluster) {
        for (int i = 0; i < 11; i++) {
            sb_src[src_de.dirent_offset + i] = (uint8_t)dst_name11[i];
        }
        if (block_ata_write_sector(src_de.dirent_lba, sb_src) != 0) {
            return FAT_EIO;
        }
        return 0;
    }

    /* Cross-directory: reserve dst slot, mark src deleted, write dst. */
    uint32_t dst_slot_lba, dst_slot_off;
    rc = find_free_dir_slot(&dst_parent, &dst_slot_lba, &dst_slot_off);
    if (rc != 0) return rc;

    uint8_t new_raw[DIRENT_SIZE];
    for (int i = 0; i < DIRENT_SIZE; i++) {
        new_raw[i] = sb_src[src_de.dirent_offset + i];
    }
    for (int i = 0; i < 11; i++) new_raw[i] = (uint8_t)dst_name11[i];

    /* Mark src deleted FIRST. Bounded leak window beats cross-link. */
    sb_src[src_de.dirent_offset] = 0xE5;
    if (block_ata_write_sector(src_de.dirent_lba, sb_src) != 0) return FAT_EIO;

    if (read_modify_write_dirent(dst_slot_lba, dst_slot_off, new_raw) != 0) {
        return FAT_EIO;
    }
    return 0;
}

/* ================================================================ */
/* fsck (FASE 8.5) — read-only audit                                */
/* ================================================================ */

#define FSCK_MAX_DEPTH       16
#define FSCK_MAX_CHAIN_HOPS  65525  /* upper bound on FAT16 cluster count */

/*
 * Cluster reachability bitmap. One bit per FAT entry index; bit set ⇔
 * cluster was visited while walking a dirent chain. After the dir
 * tree walk, any FAT entry whose value is non-zero and non-bad but
 * whose bit stayed clear is a leak.
 *
 * 65525 bits ≈ 8 KiB BSS — small enough to keep static and large enough
 * to cover the full FAT16 cluster space.
 */
#define FSCK_BITMAP_BYTES    ((65525 + 7) / 8)
static uint8_t fsck_used_map[FSCK_BITMAP_BYTES];

static void fsck_clear_used(void) {
    for (uint32_t i = 0; i < FSCK_BITMAP_BYTES; i++) fsck_used_map[i] = 0;
}

static bool fsck_is_used(uint16_t cluster) {
    return (fsck_used_map[cluster / 8] >> (cluster % 8)) & 1u;
}

static void fsck_set_used(uint16_t cluster) {
    fsck_used_map[cluster / 8] |= (uint8_t)(1u << (cluster % 8));
}

/*
 * Walk `first`'s cluster chain. Marks every visited cluster in the
 * bitmap and counts the chain length. A revisit (cross-link OR cycle)
 * bumps `*crosslinks` and stops — the fsck_is_used check is what makes
 * the walk safe against corrupted FATs that loop back on themselves.
 * Bad / out-of-range references bump `*bad_refs`.
 */
static uint32_t fsck_walk_chain(uint16_t first,
                                  uint32_t *crosslinks,
                                  uint32_t *bad_refs) {
    uint32_t len = 0;
    uint16_t cur = first;

    for (uint32_t hops = 0; hops < FSCK_MAX_CHAIN_HOPS; hops++) {
        if (cur < 2 || cur >= FAT16_EOF_MIN) break;
        if (cur == FAT16_BAD) { (*bad_refs)++; break; }
        if (fsck_is_used(cur)) { (*crosslinks)++; break; }

        fsck_set_used(cur);
        len++;

        uint16_t next;
        if (fat_get_entry(cur, &next) != 0) { (*bad_refs)++; break; }
        cur = next;
    }
    return len;
}

typedef struct {
    fat_dirent_t  dir;
    uint32_t      cursor;
} fsck_frame_t;

typedef struct {
    uint32_t file_count;
    uint32_t dir_count;
    uint32_t crosslinks;
    uint32_t bad_refs;
    uint32_t size_errors;
    uint32_t deep_skip;
} fsck_walk_t;

/*
 * Iterative DFS over the directory tree. The fsck_frame_t × MAX_DEPTH
 * stack lives in whichever task calls fsck (≈ 1.3 KiB; safe on the
 * 16 KiB kernel kstack). Subdirectories beyond MAX_DEPTH are skipped
 * and counted in `deep_skip` so the report can mention it.
 */
static void fsck_walk_tree(fsck_walk_t *w) {
    fsck_frame_t stack[FSCK_MAX_DEPTH];
    int top = 0;
    stack[0].cursor = 0;
    stack[0].dir.is_dir = true;
    stack[0].dir.first_cluster = 0;   /* root sentinel */

    uint32_t cluster_size = (uint32_t)fs.sectors_per_cluster * SECTOR_SIZE;

    while (top >= 0) {
        fat_dirent_t ent;
        uint32_t next;
        int rc = fat_readdir(&stack[top].dir, stack[top].cursor, &ent, &next);
        if (rc != 0) { top--; continue; }
        stack[top].cursor = next;

        /* Skip "." and ".." — they'd re-mark already-used clusters and
         * trigger phantom cross-links. */
        if (ent.name[0] == '.' &&
            (ent.name[1] == 0 ||
             (ent.name[1] == '.' && ent.name[2] == 0))) {
            continue;
        }

        if (ent.is_dir) {
            w->dir_count++;
            if (ent.first_cluster >= 2) {
                fsck_walk_chain(ent.first_cluster,
                                  &w->crosslinks, &w->bad_refs);
            }
            if (top + 1 < FSCK_MAX_DEPTH) {
                top++;
                stack[top].dir    = ent;
                stack[top].cursor = 0;
            } else {
                w->deep_skip++;
            }
        } else {
            w->file_count++;
            uint32_t len = 0;
            if (ent.size > 0 && ent.first_cluster >= 2) {
                len = fsck_walk_chain(ent.first_cluster,
                                        &w->crosslinks, &w->bad_refs);
            }
            /* size==0 should have first_cluster==0. size>0 needs a
             * chain that covers it without leaving an unused trailing
             * cluster (i.e. size > (len-1) * cluster_size). */
            if (ent.size == 0 && ent.first_cluster != 0) {
                w->size_errors++;
            } else if (ent.size > 0 && len == 0) {
                w->size_errors++;
            } else if (ent.size > len * cluster_size) {
                w->size_errors++;
            } else if (len > 0 &&
                       ent.size <= (len - 1) * cluster_size) {
                w->size_errors++;
            }
        }
    }
}

static int fsck_cluster_counts(uint32_t *free_c, uint32_t *used_c,
                                 uint32_t *bad_c) {
    *free_c = 0;
    *used_c = 0;
    *bad_c  = 0;

    uint32_t max_entries = (uint32_t)fs.fat_size_sectors * SECTOR_SIZE / 2;
    if (max_entries > 65525) max_entries = 65525;

    uint8_t buf[SECTOR_SIZE];
    uint32_t cached_sec = (uint32_t)-1;

    for (uint32_t c = 2; c < max_entries; c++) {
        uint32_t byte_off = c * 2;
        uint32_t sec      = fs.fat_lba + byte_off / SECTOR_SIZE;
        uint32_t in_sec   = byte_off % SECTOR_SIZE;

        if (sec != cached_sec) {
            if (block_ata_read_sector(sec, buf) != 0) return -1;
            cached_sec = sec;
        }
        uint16_t v = (uint16_t)buf[in_sec] | ((uint16_t)buf[in_sec + 1] << 8);
        if (v == 0)              (*free_c)++;
        else if (v == FAT16_BAD) (*bad_c)++;
        else                     (*used_c)++;
    }
    return 0;
}

static uint32_t fsck_count_leaks(void) {
    uint32_t leaks = 0;
    uint32_t max_entries = (uint32_t)fs.fat_size_sectors * SECTOR_SIZE / 2;
    if (max_entries > 65525) max_entries = 65525;

    uint8_t buf[SECTOR_SIZE];
    uint32_t cached_sec = (uint32_t)-1;

    for (uint32_t c = 2; c < max_entries; c++) {
        uint32_t byte_off = c * 2;
        uint32_t sec      = fs.fat_lba + byte_off / SECTOR_SIZE;
        uint32_t in_sec   = byte_off % SECTOR_SIZE;

        if (sec != cached_sec) {
            if (block_ata_read_sector(sec, buf) != 0) return leaks;
            cached_sec = sec;
        }
        uint16_t v = (uint16_t)buf[in_sec] | ((uint16_t)buf[in_sec + 1] << 8);
        if (v == 0 || v == FAT16_BAD) continue;
        if (!fsck_is_used((uint16_t)c)) leaks++;
    }
    return leaks;
}

/*
 * Compare FAT[0] sector-by-sector against every other FAT copy.
 * `*divergent_sectors` is summed across all mirrors — e.g. with
 * num_fats=2 and 3 differing sectors, the result is 3.
 */
static int fsck_check_mirror(uint32_t *divergent_sectors) {
    *divergent_sectors = 0;
    if (fs.num_fats < 2) return 0;

    uint8_t buf_a[SECTOR_SIZE], buf_b[SECTOR_SIZE];
    for (uint32_t s = 0; s < fs.fat_size_sectors; s++) {
        uint32_t lba_a = fs.fat_lba + s;
        if (block_ata_read_sector(lba_a, buf_a) != 0) return -1;
        for (uint8_t k = 1; k < fs.num_fats; k++) {
            uint32_t lba_b = fs.fat_lba +
                             (uint32_t)k * fs.fat_size_sectors + s;
            if (block_ata_read_sector(lba_b, buf_b) != 0) return -1;
            for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
                if (buf_a[i] != buf_b[i]) { (*divergent_sectors)++; break; }
            }
        }
    }
    return 0;
}

/* ----- public entry point ----- */

static void emit_label_num(char *out, size_t out_size,
                            const char *label, uint64_t n) {
    char num[24];
    os_strlcat(out, label, out_size);
    os_format_u64(n, num, sizeof(num));
    os_strlcat(out, num, out_size);
    os_strlcat(out, "\n", out_size);
}

static void emit_count_or_none(char *out, size_t out_size,
                                const char *label, uint32_t n) {
    char num[24];
    os_strlcat(out, label, out_size);
    if (n == 0) {
        os_strlcat(out, "none\n", out_size);
    } else {
        os_format_u64(n, num, sizeof(num));
        os_strlcat(out, num, out_size);
        os_strlcat(out, "\n", out_size);
    }
}

void fat_fsck_report(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = 0;

    if (!fs_ready) {
        os_strlcpy(out, "fsck: FAT not mounted\n", out_size);
        return;
    }

    uint32_t free_c = 0, used_c = 0, bad_c = 0;
    if (fsck_cluster_counts(&free_c, &used_c, &bad_c) != 0) {
        os_strlcpy(out, "fsck: FAT read failed\n", out_size);
        return;
    }

    fsck_clear_used();

    fsck_walk_t w = { 0, 0, 0, 0, 0, 0 };
    fsck_walk_tree(&w);

    uint32_t leaks = fsck_count_leaks();

    uint32_t divergent = 0;
    int mirror_rc = fsck_check_mirror(&divergent);

    os_strlcat(out, "fsck /sd (FAT16)\n", out_size);
    os_strlcat(out, "================\n", out_size);

    emit_label_num(out, out_size, "files:         ", w.file_count);
    emit_label_num(out, out_size, "dirs:          ", w.dir_count);
    emit_label_num(out, out_size, "clusters free: ", free_c);
    emit_label_num(out, out_size, "clusters used: ", used_c);
    emit_label_num(out, out_size, "clusters bad:  ", bad_c);

    os_strlcat(out, "\nmirror:        ", out_size);
    if (fs.num_fats < 2) {
        os_strlcat(out, "n/a (single FAT)\n", out_size);
    } else if (mirror_rc != 0) {
        os_strlcat(out, "read error\n", out_size);
    } else if (divergent == 0) {
        os_strlcat(out, "OK\n", out_size);
    } else {
        char num[24];
        os_strlcat(out, "DIVERGENT (sectors=", out_size);
        os_format_u64(divergent, num, sizeof(num));
        os_strlcat(out, num, out_size);
        os_strlcat(out, ")\n", out_size);
    }

    emit_count_or_none(out, out_size, "cross-links:   ",  w.crosslinks);
    emit_count_or_none(out, out_size, "leaks:         ",  leaks);
    emit_count_or_none(out, out_size, "size mismatch: ",  w.size_errors);
    emit_count_or_none(out, out_size, "bad refs:      ",  w.bad_refs);

    if (w.deep_skip) {
        char num[24];
        os_strlcat(out, "\nNOTE: ", out_size);
        os_format_u64(w.deep_skip, num, sizeof(num));
        os_strlcat(out, num, out_size);
        os_strlcat(out, " subdir(s) past depth ", out_size);
        os_format_u64(FSCK_MAX_DEPTH, num, sizeof(num));
        os_strlcat(out, num, out_size);
        os_strlcat(out, " skipped\n", out_size);
    }

    os_strlcat(out, "\n(read-only audit)\n", out_size);
}
