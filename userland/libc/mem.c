/* VoidOS — libc memory and string primitives (freestanding, no SSE)
 *
 * Phase 13 minimum runtime: the string/memory helpers the userland
 * allocator and its tests need.  Compiled with the same -mno-sse
 * -mno-sse2 -mno-mmx flags as the rest of userland, so every function
 * is a plain byte/word loop with no vectorization, no libgcc helpers,
 * and no inline asm.  Correctness over micro-optimization: the kernel
 * maps 16-byte-aligned pages, but these loops make no alignment
 * assumption and always handle overlap per their contracts.
 */
#include <libc/string.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d <= s) {                       /* forward copy is safe */
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {                            /* backward copy preserves overlap */
        while (n--)
            d[n] = s[n];
    }
    return dst;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    unsigned char v = (unsigned char)c;
    for (size_t i = 0; i < n; i++)
        p[i] = v;
    return s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a;
    const unsigned char *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return (x[i] < y[i]) ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    do {
        *d++ = *src;
    } while (*src++);
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    unsigned char ca = (unsigned char)*a;
    unsigned char cb = (unsigned char)*b;
    return (ca < cb) ? -1 : (ca > cb) ? 1 : 0;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            unsigned char ca = (unsigned char)a[i];
            unsigned char cb = (unsigned char)b[i];
            return (ca < cb) ? -1 : 1;
        }
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}