#include "string.h"

size_t os_strlen(const char *s) {
    size_t len = 0;

    while (s[len] != 0) {
        len++;
    }

    return len;
}

int os_strcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return (unsigned char)*a - (unsigned char)*b;
        }

        a++;
        b++;
    }

    return (unsigned char)*a - (unsigned char)*b;
}

int os_strncmp(const char *a, const char *b, size_t n) {
    while (n > 0 && *a && *b) {
        if (*a != *b) {
            return (unsigned char)*a - (unsigned char)*b;
        }

        a++;
        b++;
        n--;
    }

    if (n == 0) {
        return 0;
    }

    return (unsigned char)*a - (unsigned char)*b;
}

int os_streq(const char *a, const char *b) {
    return os_strcmp(a, b) == 0;
}

bool os_strstarts(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) {
            return false;
        }

        s++;
        prefix++;
    }

    return true;
}

char *os_strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }

        s++;
    }

    if ((char)c == 0) {
        return (char *)s;
    }

    return 0;
}

char *os_strrchr(const char *s, int c) {
    const char *last = 0;

    while (*s) {
        if (*s == (char)c) {
            last = s;
        }

        s++;
    }

    if ((char)c == 0) {
        return (char *)s;
    }

    return (char *)last;
}

size_t os_strlcpy(char *dst, const char *src, size_t dst_size) {
    size_t src_len = 0;

    while (src[src_len]) {
        src_len++;
    }

    if (dst_size == 0) {
        return src_len;
    }

    size_t copy = src_len;

    if (copy > dst_size - 1) {
        copy = dst_size - 1;
    }

    for (size_t i = 0; i < copy; i++) {
        dst[i] = src[i];
    }

    dst[copy] = 0;

    return src_len;
}

size_t os_strlcat(char *dst, const char *src, size_t dst_size) {
    size_t dst_len = 0;

    while (dst_len < dst_size && dst[dst_len]) {
        dst_len++;
    }

    size_t src_len = 0;

    while (src[src_len]) {
        src_len++;
    }

    if (dst_len == dst_size) {
        return dst_size + src_len;
    }

    size_t space = dst_size - dst_len - 1;
    size_t copy = src_len;

    if (copy > space) {
        copy = space;
    }

    for (size_t i = 0; i < copy; i++) {
        dst[dst_len + i] = src[i];
    }

    dst[dst_len + copy] = 0;

    return dst_len + src_len;
}

void os_memset(void *ptr, int value, size_t size) {
    unsigned char *p = (unsigned char *)ptr;

    for (size_t i = 0; i < size; i++) {
        p[i] = (unsigned char)value;
    }
}

size_t os_format_u64(uint64_t value, char *out, size_t out_size) {
    if (out_size == 0) return 0;

    char digits[24];
    size_t k = 0;

    if (value == 0) {
        digits[k++] = '0';
    } else {
        while (value > 0 && k < sizeof(digits)) {
            digits[k++] = (char)('0' + (value % 10));
            value /= 10;
        }
    }

    size_t w = 0;
    while (k > 0 && w + 1 < out_size) {
        out[w++] = digits[--k];
    }
    out[w] = 0;
    return w;
}

void os_memcpy(void *dest, const void *src, size_t size) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    for (size_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
}
