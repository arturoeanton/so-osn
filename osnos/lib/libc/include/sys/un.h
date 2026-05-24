#pragma once

#include <sys/types.h>

/* struct sockaddr_un — POSIX AF_UNIX address. Layout match Linux. */
#define UNIX_PATH_MAX 108

struct sockaddr_un {
    unsigned short sun_family;   /* AF_UNIX */
    char           sun_path[UNIX_PATH_MAX];
};
