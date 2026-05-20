#include "ramfs.h"

#include "../lib/string.h"

/*
 * Storage invariants:
 *   - files[] has RAMFS_MAX_FILES slots, indexed 0..RAMFS_MAX_FILES-1.
 *   - Each slot's `used` flag is the authoritative liveness check.
 *   - A live slot's index never changes for the life of the entry. This means
 *     a `const ramfs_file_t *` returned by ramfs_find remains valid until the
 *     caller (or someone else) deletes that specific entry.
 *   - Deletion marks `used=false` and clears the slot. Slot is reusable.
 *   - Creation finds the first free slot. There is no notion of "order of
 *     creation" — listings reflect the slot order, which can interleave after
 *     deletions.
 */

static ramfs_file_t files[RAMFS_MAX_FILES];

static void clear_slot(ramfs_file_t *slot) {
    slot->used = false;
    slot->is_dir = false;
    slot->name[0] = 0;
    slot->data[0] = 0;
    slot->size = 0;
}

static int find_free_slot(void) {
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used) {
            return (int)i;
        }
    }
    return -1;
}

static int path_is_child_of(const char *child, const char *parent) {
    size_t i = 0;

    while (parent[i]) {
        if (child[i] != parent[i]) {
            return 0;
        }
        i++;
    }

    return child[i] == '/';
}

static int glob_match(const char *pattern, const char *name) {
    if (*pattern == 0) {
        return *name == 0;
    }

    if (*pattern == '*') {
        while (*(pattern + 1) == '*') {
            pattern++;
        }

        if (*(pattern + 1) == 0) {
            while (*name) {
                if (*name == '/') return 0;
                name++;
            }
            return 1;
        }

        while (*name) {
            if (glob_match(pattern + 1, name)) return 1;
            if (*name == '/') return 0;
            name++;
        }

        return glob_match(pattern + 1, name);
    }

    if (*pattern != *name) {
        return 0;
    }

    return glob_match(pattern + 1, name + 1);
}

static void split_path_last(
    const char *path,
    char *dir,
    size_t dir_size,
    char *base,
    size_t base_size
) {
    size_t len = os_strlen(path);
    size_t last_slash = 0;
    int found = 0;

    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') {
            last_slash = i;
            found = 1;
        }
    }

    if (!found) {
        dir[0] = 0;
        os_strlcpy(base, path, base_size);
        return;
    }

    if (last_slash == 0) {
        os_strlcpy(dir, "/", dir_size);
    } else {
        size_t k = 0;
        while (k < last_slash && k + 1 < dir_size) {
            dir[k] = path[k];
            k++;
        }
        dir[k] = 0;
    }

    os_strlcpy(base, path + last_slash + 1, base_size);
}

static int file_is_direct_child_of(const char *name, const char *dir) {
    size_t dir_len = os_strlen(dir);

    if (os_streq(dir, "/")) {
        if (name[0] != '/') return 0;
        const char *rest = name + 1;
        if (rest[0] == 0) return 0;
        for (size_t i = 0; rest[i]; i++) {
            if (rest[i] == '/') return 0;
        }
        return 1;
    }

    size_t i = 0;
    while (dir[i]) {
        if (name[i] != dir[i]) return 0;
        i++;
    }

    if (name[i] != '/') return 0;

    const char *rest = name + dir_len + 1;
    if (rest[0] == 0) return 0;

    for (size_t j = 0; rest[j]; j++) {
        if (rest[j] == '/') return 0;
    }

    return 1;
}

static const char *basename_of(const char *path) {
    size_t len = os_strlen(path);
    size_t last_slash = 0;

    for (size_t j = 0; j < len; j++) {
        if (path[j] == '/') last_slash = j;
    }

    return path + last_slash + 1;
}

void ramfs_init(void) {
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        clear_slot(&files[i]);
    }
}

bool ramfs_create_file(const char *name, const char *data) {
    if (ramfs_find(name)) return false;

    if (os_strlen(name) + 1 > RAMFS_NAME_SIZE) return false;

    int idx = find_free_slot();
    if (idx < 0) return false;

    ramfs_file_t *file = &files[idx];
    file->used = true;
    file->is_dir = false;
    os_strlcpy(file->name, name, RAMFS_NAME_SIZE);
    os_strlcpy(file->data, data, RAMFS_DATA_SIZE);
    file->size = os_strlen(file->data);

    return true;
}

const ramfs_file_t *ramfs_find(const char *name) {
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used && os_streq(files[i].name, name)) {
            return &files[i];
        }
    }
    return 0;
}

bool ramfs_touch(const char *name) {
    if (ramfs_find(name)) return true;
    return ramfs_create_file(name, "");
}

bool ramfs_write_file(const char *name, const char *data) {
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used && !files[i].is_dir && os_streq(files[i].name, name)) {
            os_strlcpy(files[i].data, data, RAMFS_DATA_SIZE);
            files[i].size = os_strlen(files[i].data);
            return true;
        }
    }
    return ramfs_create_file(name, data);
}

bool ramfs_append_file(const char *name, const char *data) {
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used && !files[i].is_dir && os_streq(files[i].name, name)) {
            size_t pos = os_strlen(files[i].data);
            size_t k = 0;
            while (data[k] && pos + 1 < RAMFS_DATA_SIZE) {
                files[i].data[pos++] = data[k++];
            }
            files[i].data[pos] = 0;
            files[i].size = os_strlen(files[i].data);
            return true;
        }
    }
    return ramfs_create_file(name, data);
}

bool ramfs_delete_file(const char *name) {
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used && !files[i].is_dir && os_streq(files[i].name, name)) {
            clear_slot(&files[i]);
            return true;
        }
    }
    return false;
}

bool ramfs_mkdir(const char *name) {
    if (ramfs_find(name)) return false;

    if (os_strlen(name) + 1 > RAMFS_NAME_SIZE) return false;

    int idx = find_free_slot();
    if (idx < 0) return false;

    ramfs_file_t *dir = &files[idx];
    dir->used = true;
    dir->is_dir = true;
    dir->data[0] = 0;
    dir->size = 0;
    os_strlcpy(dir->name, name, RAMFS_NAME_SIZE);

    return true;
}

bool ramfs_rmdir(const char *name) {
    if (os_streq(name, "/")) return false;

    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used && path_is_child_of(files[i].name, name)) {
            return false;
        }
    }

    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used && files[i].is_dir && os_streq(files[i].name, name)) {
            clear_slot(&files[i]);
            return true;
        }
    }
    return false;
}

static int name_already_listed(const char *out, const char *name, size_t name_len) {
    size_t i = 0;

    while (out[i]) {
        size_t j = 0;
        while (out[i + j] && out[i + j] != '\n' &&
               j < name_len && out[i + j] == name[j]) {
            j++;
        }

        if (j == name_len && (out[i + j] == '\n' || out[i + j] == '/')) {
            return 1;
        }

        while (out[i] && out[i] != '\n') i++;
        if (out[i] == '\n') i++;
    }

    return 0;
}

size_t ramfs_list_dir(const char *path, char *out, size_t out_size) {
    out[0] = 0;

    size_t written = 0;
    size_t path_len = os_strlen(path);

    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        if (os_streq(files[i].name, path)) continue;
        if (!os_strstarts(files[i].name, path)) continue;

        const char *rest = files[i].name + path_len;
        if (path_len == 1 && rest[0] == '/') rest++;
        if (path_len > 1 && rest[0] == '/') rest++;
        if (rest[0] == 0) continue;

        size_t name_len = 0;
        while (rest[name_len] && rest[name_len] != '/') name_len++;
        if (name_len == 0) continue;

        if (name_already_listed(out, rest, name_len)) continue;

        for (size_t j = 0; j < name_len && written + 1 < out_size; j++) {
            out[written++] = rest[j];
        }

        if (rest[name_len] == '/' && written + 1 < out_size) {
            out[written++] = '/';
        } else if (files[i].is_dir && written + 1 < out_size) {
            out[written++] = '/';
        }

        if (written + 1 < out_size) out[written++] = '\n';

        out[written] = 0;
    }

    return written;
}

bool ramfs_copy_file(const char *src, const char *dst) {
    const ramfs_file_t *source = ramfs_find(src);
    if (!source || source->is_dir) return false;

    const ramfs_file_t *target = ramfs_find(dst);
    if (target && target->is_dir) return false;

    if (target) {
        return ramfs_write_file(dst, source->data);
    }

    return ramfs_create_file(dst, source->data);
}

bool ramfs_move(const char *src, const char *dst) {
    if (os_streq(src, dst)) return false;
    if (ramfs_find(dst)) return false;

    if (os_strlen(dst) + 1 > RAMFS_NAME_SIZE) return false;

    ramfs_file_t *source = 0;
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used && os_streq(files[i].name, src)) {
            source = &files[i];
            break;
        }
    }
    if (!source) return false;

    if (source->is_dir) {
        size_t src_len = os_strlen(src);
        size_t dst_len = os_strlen(dst);

        /* Pre-check: any descendant path that would exceed RAMFS_NAME_SIZE? */
        for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
            if (!files[i].used) continue;
            if (!path_is_child_of(files[i].name, src)) continue;

            size_t name_len = os_strlen(files[i].name);
            size_t tail_len = name_len - src_len;
            if (dst_len + tail_len + 1 > RAMFS_NAME_SIZE) {
                return false;
            }
        }

        /* Safe to mutate. */
        for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
            if (!files[i].used) continue;
            if (!path_is_child_of(files[i].name, src)) continue;

            char new_name[RAMFS_NAME_SIZE];
            size_t k = 0;
            size_t l = 0;

            while (dst[l] && k + 1 < RAMFS_NAME_SIZE) {
                new_name[k++] = dst[l++];
            }

            const char *tail = files[i].name + src_len;
            size_t t = 0;
            while (tail[t] && k + 1 < RAMFS_NAME_SIZE) {
                new_name[k++] = tail[t++];
            }

            new_name[k] = 0;
            os_strlcpy(files[i].name, new_name, RAMFS_NAME_SIZE);
        }
    }

    os_strlcpy(source->name, dst, RAMFS_NAME_SIZE);
    return true;
}

static void tree_emit(
    const char *name,
    int is_dir,
    size_t depth,
    char *out,
    size_t *written,
    size_t out_size
) {
    for (size_t d = 0; d < depth; d++) {
        if (*written + 2 >= out_size) return;
        out[(*written)++] = ' ';
        out[(*written)++] = ' ';
    }

    size_t k = 0;
    while (name[k] && *written + 1 < out_size) {
        out[(*written)++] = name[k++];
    }

    if (is_dir && *written + 1 < out_size) out[(*written)++] = '/';
    if (*written + 1 < out_size) out[(*written)++] = '\n';

    out[*written] = 0;
}

/*
 * Iterative DFS over directories. Each stack frame carries (path, depth,
 * cursor) where `cursor` is the next files[] index to inspect for direct
 * children of `path`. Depth is bounded by RAMFS_MAX_FILES so the stack
 * cannot overflow.
 */
typedef struct {
    const char *path;
    size_t depth;
    size_t cursor;
} tree_frame_t;

size_t ramfs_tree(const char *path, char *out, size_t out_size) {
    out[0] = 0;
    size_t written = 0;

    const char *root = (path && path[0]) ? path : "/";

    tree_frame_t stack[RAMFS_MAX_FILES];
    int top = 0;
    stack[0].path = root;
    stack[0].depth = 0;
    stack[0].cursor = 0;

    while (top >= 0) {
        tree_frame_t *frame = &stack[top];
        int found_child = 0;

        for (size_t i = frame->cursor; i < RAMFS_MAX_FILES; i++) {
            if (!files[i].used) continue;
            if (!file_is_direct_child_of(files[i].name, frame->path)) continue;

            const char *base = basename_of(files[i].name);
            tree_emit(base, files[i].is_dir, frame->depth, out, &written, out_size);

            frame->cursor = i + 1;
            found_child = 1;

            if (files[i].is_dir && top + 1 < (int)RAMFS_MAX_FILES) {
                top++;
                stack[top].path = files[i].name;
                stack[top].depth = frame->depth + 1;
                stack[top].cursor = 0;
            }
            break;
        }

        if (!found_child) {
            top--;
        }
    }

    return written;
}

size_t ramfs_slot_index(const ramfs_file_t *f) {
    return (size_t)(f - files);
}

size_t ramfs_used_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used) n++;
    }
    return n;
}

const ramfs_file_t *ramfs_iter_child(const char *parent, size_t *cursor) {
    for (size_t i = *cursor; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        if (!file_is_direct_child_of(files[i].name, parent)) continue;

        *cursor = i + 1;
        return &files[i];
    }

    return 0;
}

bool ramfs_path_has_wildcard(const char *path) {
    if (!path) return false;

    while (*path) {
        if (*path == '*') return true;
        path++;
    }
    return false;
}

size_t ramfs_delete_glob(const char *pattern) {
    char dir[RAMFS_NAME_SIZE];
    char base[RAMFS_NAME_SIZE];

    split_path_last(pattern, dir, sizeof(dir), base, sizeof(base));

    size_t deleted = 0;

    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        if (files[i].is_dir) continue;
        if (!file_is_direct_child_of(files[i].name, dir)) continue;

        if (!glob_match(base, basename_of(files[i].name))) continue;

        clear_slot(&files[i]);
        deleted++;
    }

    return deleted;
}

size_t ramfs_read_glob(const char *pattern, char *out, size_t out_size) {
    char dir[RAMFS_NAME_SIZE];
    char base[RAMFS_NAME_SIZE];

    split_path_last(pattern, dir, sizeof(dir), base, sizeof(base));

    out[0] = 0;
    size_t written = 0;
    size_t matches = 0;

    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        if (files[i].is_dir) continue;
        if (!file_is_direct_child_of(files[i].name, dir)) continue;

        if (!glob_match(base, basename_of(files[i].name))) continue;

        size_t k = 0;
        while (files[i].data[k] && written + 1 < out_size) {
            out[written++] = files[i].data[k++];
        }
        out[written] = 0;
        matches++;
    }

    return matches;
}

size_t ramfs_list_glob(const char *pattern, char *out, size_t out_size) {
    char dir[RAMFS_NAME_SIZE];
    char base[RAMFS_NAME_SIZE];

    split_path_last(pattern, dir, sizeof(dir), base, sizeof(base));

    out[0] = 0;
    size_t written = 0;
    size_t matches = 0;

    for (size_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        if (!file_is_direct_child_of(files[i].name, dir)) continue;

        const char *file_base = basename_of(files[i].name);
        if (!glob_match(base, file_base)) continue;

        size_t k = 0;
        while (file_base[k] && written + 1 < out_size) {
            out[written++] = file_base[k++];
        }

        if (files[i].is_dir && written + 1 < out_size) out[written++] = '/';
        if (written + 1 < out_size) out[written++] = '\n';

        out[written] = 0;
        matches++;
    }

    return matches;
}
