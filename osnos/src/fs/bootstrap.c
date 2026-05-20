#include "bootstrap.h"

#include "../lib/string.h"
#include "binfs.h"
#include "devfs.h"
#include "ramfs_vfs.h"
#include "sysfs.h"
#include "vfs.h"

static void seed_file(const char *path, const char *content) {
    vfs_write(path, content, os_strlen(content));
}

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

    vfs_mkdir("/home");

    seed_file("/home/README.TXT",
        "Welcome to osnos.\nThis is a tiny RAM filesystem.\n");
    seed_file("/home/HELLO.TXT", "Hello from ramfs!\n");
}
