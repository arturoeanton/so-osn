#include "fs_server.h"

#include <stddef.h>
#include <stdbool.h>

#include "../fs/vfs.h"
#include "../include/osnos_status.h"
#include "../lib/string.h"
#include "../micro/ipc.h"

static void format_count(size_t n, const char *suffix, char *out, size_t out_size) {
    char digits[20];
    size_t k = 0;

    if (n == 0) {
        digits[k++] = '0';
    } else {
        while (n > 0 && k < sizeof(digits)) {
            digits[k++] = (char)('0' + (n % 10));
            n /= 10;
        }
    }

    size_t w = 0;
    while (k > 0 && w + 1 < out_size) {
        out[w++] = digits[--k];
    }

    size_t s = 0;
    while (suffix[s] && w + 1 < out_size) {
        out[w++] = suffix[s++];
    }

    out[w] = 0;
}

static char *after_filename(char *data) {
    char *p = data;
    while (*p) p++;
    return p + 1;
}

static void set_response(
    ipc_msg_t *response,
    osnos_status_t status,
    const char *text
) {
    response->arg0 = (uint64_t)status;
    os_strlcpy(response->data, text, IPC_DATA_SIZE);
}

void fs_server_init(void) {
}

void fs_server_tick(void) {
    ipc_msg_t msg;

    while (ipc_recv(SERVER_FS, &msg)) {
        ipc_msg_t response;

        response.from = SERVER_FS;
        response.to = msg.from;
        response.type = IPC_FS_RESPONSE;
        response.arg0 = (uint64_t)OSNOS_OK;
        response.arg1 = 0;
        response.data[0] = 0;

        if (msg.type == IPC_FS_LIST) {
            const char *path = msg.data[0] ? msg.data : "/";

            if (vfs_path_has_wildcard(path)) {
                size_t matches = vfs_glob_list(path, response.data, IPC_DATA_SIZE);
                if (matches == 0) {
                    set_response(&response, OSNOS_ENOENT, "no match\n");
                } else {
                    response.arg1 = matches;
                }
            } else {
                size_t written = vfs_list_dir(path, response.data, IPC_DATA_SIZE);
                response.arg1 = written;
            }

            ipc_send(&response);
            continue;
        }

        if (msg.type == IPC_FS_READ) {
            if (vfs_path_has_wildcard(msg.data)) {
                size_t matches = vfs_glob_read(msg.data, response.data, IPC_DATA_SIZE);
                if (matches == 0) {
                    set_response(&response, OSNOS_ENOENT, "no match\n");
                } else {
                    response.arg1 = matches;
                }
                ipc_send(&response);
                continue;
            }

            size_t got = 0;
            osnos_status_t s = vfs_read(msg.data, response.data, IPC_DATA_SIZE - 1, &got);

            if (s == OSNOS_OK) {
                response.data[got] = 0;
                response.arg1 = got;
            } else {
                set_response(&response, s, "file not found\n");
            }

            ipc_send(&response);
            continue;
        }

        if (msg.type == IPC_FS_TOUCH) {
            osnos_status_t s = vfs_touch(msg.data);
            if (s == OSNOS_OK) {
                set_response(&response, OSNOS_OK, "touch ok\n");
            } else {
                set_response(&response, s, "touch failed\n");
            }
            ipc_send(&response);
            continue;
        }

        if (msg.type == IPC_FS_WRITE) {
            char *filename = msg.data;
            char *content = after_filename(msg.data);

            osnos_status_t s = vfs_write(filename, content, os_strlen(content));
            if (s == OSNOS_OK) {
                set_response(&response, OSNOS_OK, "write ok\n");
            } else {
                set_response(&response, s, "write failed\n");
            }
            ipc_send(&response);
            continue;
        }

        if (msg.type == IPC_FS_APPEND) {
            char *filename = msg.data;
            char *content = after_filename(msg.data);

            osnos_status_t s = vfs_append(filename, content, os_strlen(content));
            if (s == OSNOS_OK) {
                set_response(&response, OSNOS_OK, "append ok\n");
            } else {
                set_response(&response, s, "append failed\n");
            }
            ipc_send(&response);
            continue;
        }

        if (msg.type == IPC_FS_DELETE) {
            if (vfs_path_has_wildcard(msg.data)) {
                size_t n = vfs_glob_unlink(msg.data);
                if (n == 0) {
                    set_response(&response, OSNOS_ENOENT, "no match\n");
                } else {
                    response.arg1 = n;
                    format_count(n, " deleted\n", response.data, IPC_DATA_SIZE);
                }
                ipc_send(&response);
                continue;
            }

            osnos_status_t s = vfs_unlink(msg.data);
            if (s == OSNOS_OK) {
                set_response(&response, OSNOS_OK, "delete ok\n");
            } else {
                set_response(&response, s, "delete failed\n");
            }
            ipc_send(&response);
            continue;
        }

        if (msg.type == IPC_FS_MKDIR) {
            osnos_status_t s = vfs_mkdir(msg.data);
            if (s == OSNOS_OK) {
                set_response(&response, OSNOS_OK, "mkdir ok\n");
            } else {
                set_response(&response, s, "mkdir failed\n");
            }
            ipc_send(&response);
            continue;
        }

        if (msg.type == IPC_FS_RMDIR) {
            osnos_status_t s = vfs_rmdir(msg.data);
            if (s == OSNOS_OK) {
                set_response(&response, OSNOS_OK, "rmdir ok\n");
            } else {
                set_response(&response, s, "rmdir failed\n");
            }
            ipc_send(&response);
            continue;
        }

        if (msg.type == IPC_FS_TREE) {
            const char *path = msg.data[0] ? msg.data : "/";
            size_t written = vfs_tree(path, response.data, IPC_DATA_SIZE);
            if (written == 0) {
                set_response(&response, OSNOS_ENOENT, "empty\n");
            } else {
                response.arg1 = written;
            }
            ipc_send(&response);
            continue;
        }

        if (msg.type == IPC_FS_COPY) {
            char *src = msg.data;
            char *dst = after_filename(msg.data);

            osnos_status_t s = vfs_copy(src, dst);
            if (s == OSNOS_OK) {
                set_response(&response, OSNOS_OK, "cp ok\n");
            } else {
                set_response(&response, s, "cp failed\n");
            }
            ipc_send(&response);
            continue;
        }

        if (msg.type == IPC_FS_MOVE) {
            char *src = msg.data;
            char *dst = after_filename(msg.data);

            osnos_status_t s = vfs_move(src, dst);
            if (s == OSNOS_OK) {
                set_response(&response, OSNOS_OK, "mv ok\n");
            } else {
                set_response(&response, s, "mv failed\n");
            }
            ipc_send(&response);
            continue;
        }

        set_response(&response, OSNOS_EINVAL, "unknown op\n");
        ipc_send(&response);
    }
}
