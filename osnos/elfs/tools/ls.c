#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Mini-libc ls:
 *   sin flags:  name [name...] one-per-line
 *   -l:         long format: mode size name
 *   -a:         incluye dotfiles
 *   -1:         force one-per-line (default ya lo es)
 * No soporta -h, -R, -t. Suficiente para `ls -l file`, `ls -la /home`. */

static int show_all = 0;
static int long_fmt = 0;

static void print_mode(unsigned m, char out[11]) {
    out[0] = S_ISDIR(m) ? 'd' : (S_ISCHR(m) ? 'c' : '-');
    out[1] = (m & 0400) ? 'r' : '-';
    out[2] = (m & 0200) ? 'w' : '-';
    out[3] = (m & 0100) ? 'x' : '-';
    out[4] = (m & 0040) ? 'r' : '-';
    out[5] = (m & 0020) ? 'w' : '-';
    out[6] = (m & 0010) ? 'x' : '-';
    out[7] = (m & 0004) ? 'r' : '-';
    out[8] = (m & 0002) ? 'w' : '-';
    out[9] = (m & 0001) ? 'x' : '-';
    out[10] = 0;
}

static void join_path(char *out, size_t cap, const char *dir, const char *name) {
    size_t l = strlen(dir);
    if (l == 1 && dir[0] == '/') {
        snprintf(out, cap, "/%s", name);
    } else if (l > 0 && dir[l - 1] == '/') {
        snprintf(out, cap, "%s%s", dir, name);
    } else {
        snprintf(out, cap, "%s/%s", dir, name);
    }
}

static void print_long(const char *dir, const char *name) {
    char path[512];
    join_path(path, sizeof(path), dir, name);
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("?????????? ?     ? %s\n", name);
        return;
    }
    char mode[11];
    print_mode(st.st_mode, mode);
    printf("%s %8lld %s%s\n",
            mode, (long long)st.st_size, name,
            S_ISDIR(st.st_mode) ? "/" : "");
}

static int ls_one(const char *path) {
    struct stat pst;
    if (stat(path, &pst) == 0 && !S_ISDIR(pst.st_mode)) {
        if (long_fmt) {
            char mode[11];
            print_mode(pst.st_mode, mode);
            printf("%s %8lld %s\n", mode, (long long)pst.st_size, path);
        } else {
            printf("%s\n", path);
        }
        return 0;
    }
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: cannot open %s\n", path);
        return 1;
    }
    for (;;) {
        struct dirent *e = readdir(d);
        if (!e) break;
        if (!show_all && e->d_name[0] == '.') continue;
        if (long_fmt) {
            print_long(path, e->d_name);
        } else {
            printf("%s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
        }
    }
    closedir(d);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') break;
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (int k = 1; argv[i][k]; k++) {
            switch (argv[i][k]) {
            case 'l': long_fmt = 1; break;
            case 'a': show_all = 1; break;
            case '1': break;
            default:
                fprintf(stderr, "ls: unknown flag -%c\n", argv[i][k]);
                return 1;
            }
        }
    }
    if (i >= argc) return ls_one(".");
    int rc = 0;
    int n_paths = argc - i;
    for (; i < argc; i++) {
        if (n_paths > 1) printf("%s:\n", argv[i]);
        if (ls_one(argv[i])) rc = 1;
        if (n_paths > 1 && i + 1 < argc) putchar('\n');
    }
    return rc;
}
