#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/";

    struct stat st;
    int r = stat(path, &st);
    printf("stat(%s) = %d errno=%d mode=0x%x size=%ld\n",
           path, r, errno, (unsigned)st.st_mode, (long)st.st_size);

    DIR *d = opendir(path);
    if (!d) {
        printf("opendir failed errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    printf("opendir OK\n");
    struct dirent *de;
    int n = 0;
    while ((de = readdir(d))) {
        printf("  %s\n", de->d_name);
        if (++n > 30) break;
    }
    closedir(d);
    return 0;
}
