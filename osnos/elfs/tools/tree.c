/*
 * tools/tree.c — recursive directory listing with indentation.
 *
 *   tree              — current dir
 *   tree PATH         — start at PATH
 *
 * Depth-limited (so a circular alias doesn't infinite-loop) and
 * width-bounded (skips dirs with too many entries to keep output
 * sane on the framebuffer console).
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TREE_MAX_DEPTH   8
#define TREE_MAX_ENTRIES 256

static int n_dirs;
static int n_files;

static void walk(const char *path, int depth) {
    if (depth > TREE_MAX_DEPTH) return;
    DIR *d = opendir(path);
    if (!d) return;
    int seen = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0 && seen < TREE_MAX_ENTRIES) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 ||
            (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        for (int i = 0; i < depth; i++) printf("  ");
        printf("%s\n", e->d_name);
        seen++;
        /* Build child path. */
        char child[PATH_MAX];
        size_t pl = strlen(path);
        if (pl + 1 + strlen(e->d_name) + 1 >= sizeof(child)) continue;
        size_t pos = 0;
        for (size_t i = 0; i < pl; i++) child[pos++] = path[i];
        if (pos == 0 || child[pos - 1] != '/') child[pos++] = '/';
        for (size_t i = 0; e->d_name[i]; i++) child[pos++] = e->d_name[i];
        child[pos] = 0;

        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            n_dirs++;
            walk(child, depth + 1);
        } else {
            n_files++;
        }
    }
    closedir(d);
}

int main(int argc, char **argv) {
    const char *root = (argc >= 2) ? argv[1] : ".";
    printf("%s\n", root);
    walk(root, 1);
    printf("\n%d directories, %d files\n", n_dirs, n_files);
    return 0;
}
