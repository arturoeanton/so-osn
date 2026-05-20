#pragma once

#include <sys/types.h>

/* d_type values (Linux DT_*). */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12

struct dirent {
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

typedef struct DIR DIR;

DIR           *opendir (const char *path);
struct dirent *readdir (DIR *dir);
int            closedir(DIR *dir);
