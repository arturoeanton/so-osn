#include <dirent.h>
#include <stdio.h>

static int ls_one(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: cannot open %s\n", path);
        return 1;
    }
    for (;;) {
        struct dirent *e = readdir(d);
        if (!e) break;
        printf("%s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
    }
    closedir(d);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return ls_one("/");
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argc > 2) printf("%s:\n", argv[i]);
        if (ls_one(argv[i])) rc = 1;
        if (argc > 2 && i + 1 < argc) putchar('\n');
    }
    return rc;
}
