#include "bootstrap.h"

#include "../lib/string.h"
#include "aliasfs.h"
#include "binfs.h"
#include "devfs.h"
#include "fat.h"
#include "fat_vfs.h"
#include "ramfs_vfs.h"
#include "sysfs.h"
#include "vfs.h"

static void seed_file(const char *path, const char *content) {
    vfs_write(path, content, os_strlen(content));
}

/* Seed only if the file doesn't already exist (so we don't trample a
 * user's edits across boots when /home lives on disk). */
static void seed_if_absent(const char *path, const char *content) {
    vfs_stat_t st;
    if (vfs_stat(path, &st) == OSNOS_OK) return;
    seed_file(path, content);
}

/* Storage for the /home → /sd/home bind mount. Lives in BSS so it
 * survives forever (the VFS layer keeps a pointer into here). */
static aliasfs_t home_alias_slot;

void bootstrap_fs(void) {
    vfs_init();

    /* ramfs at "/" serves everything except more specific mounts below. */
    vfs_mount("/", &ramfs_vfs_ops, 0);

    /* sysfs at "/sys" — synthetic, read-only, no ramfs storage needed. */
    vfs_mount("/sys", &sysfs_vfs_ops, 0);

    /* devfs at "/dev" — synthetic character devices. */
    vfs_mount("/dev", &devfs_vfs_ops, 0);

    /* binfs at "/bin" — synthetic, backed by the builtin registry. */
    vfs_mount("/bin", &binfs_vfs_ops, 0);

    /* /sd — FAT16 on the ATA primary master, when present. Mounting
     * only if the parser bound to a valid BPB keeps the mount table
     * tidy when there's no disk attached. */
    bool fat_mounted = (fat_init() == 0);
    if (fat_mounted) {
        vfs_mount("/sd", &fat_vfs_ops, 0);
    }

    /*
     * /home backing:
     *   - With a disk: alias /home → /sd/home. Reads, writes, mkdir,
     *     mv all resolve onto FAT16 and persist across reboots.
     *   - Without a disk: plain ramfs entry (the "/" mount handles it).
     */
    if (fat_mounted) {
        /* Make sure /sd/home exists on disk. mkdir is idempotent —
         * returns OSNOS_EEXIST if already there. */
        vfs_mkdir("/sd/home");

        if (aliasfs_init(&home_alias_slot, "/home", "/sd/home")) {
            vfs_mount("/home", &aliasfs_ops, &home_alias_slot);
        }

        /* First-boot seed lives on disk; idempotent on subsequent boots. */
        seed_if_absent("/home/README.TXT",
            "Welcome to osnos.\nYour home lives on FAT16 — files persist across reboots.\n");
        seed_if_absent("/home/HELLO.TXT", "Hello from FAT-backed /home!\n");
        /* Sample shell rc — user-editable. shellsrv replays each
         * line at boot (FASE 10.4). Default seeds a useful env so
         * scripts that read $HOME / $PATH / $SHELL work out of box. */
        seed_if_absent("/home/.oshrc",
            "# osnos shell startup — runs at every shellsrv boot.\n"
            "# Edit to add your own commands / env vars.\n"
            "export PATH=/bin\n"
            "export HOME=/home\n"
            "export SHELL=/bin/shellsrv\n"
            "export OSNAME=osnos\n");
    } else {
        /* Diskless: same ramfs-backed /home as before. */
        vfs_mkdir("/home");
        seed_file("/home/README.TXT",
            "Welcome to osnos.\nThis is a tiny RAM filesystem (no disk attached).\n");
        seed_file("/home/HELLO.TXT", "Hello from ramfs!\n");
        seed_file("/home/.oshrc",
            "# osnos shell startup (RAM-only — won't persist across reboots)\n"
            "export PATH=/bin\n"
            "export HOME=/home\n"
            "export SHELL=/bin/shellsrv\n"
            "export OSNAME=osnos\n");
    }
}
