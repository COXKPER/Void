/* VoidOS — libc string functions (freestanding, no SSE/vectorization)
 *
 * Phase 13 brings the minimum string/memory primitives the userland
 * allocator and its tests need.  The compiler is built with -mno-sse
 * -mno-sse2, so every helper is a plain byte loop: no inline asm, no
 * __attribute__((target(...))), no vector intrinsics.  The kernel's user
 * heap/mmap pages are 4 KiB and 16-byte-aligned, but we make no assumption
 * about the alignment of the destination for memcpy — a byte loop is always
 * correct and always simple pending future optimization.
 */
#ifndef LIBC_STRING_H
#define LIBC_STRING_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── memory ────────────────────────────────────────────────────────── */

/* Copy exactly n bytes from src to dst.  The regions must not overlap
 * (use memmove for that).  Returns dst. */
void *memcpy(void *dst, const void *src, size_t n);

/* Copy n bytes from src to dst even when the regions overlap.  Returns
 * dst.  Safe for forward and backward copies. */
void *memmove(void *dst, const void *src, size_t n);

/* Fill the first n bytes of s with c (treated as unsigned char).
 * Returns s. */
void *memset(void *s, int c, size_t n);

/* Compare the first n bytes of a and b lexicographically (unsigned).
 * Returns 0 if equal, <0 if a[i]<b[i], >0 otherwise. */
int memcmp(const void *a, const void *b, size_t n);

/* ── strings ───────────────────────────────────────────────────────── */

/* Length of s, not counting the terminating NUL. */
size_t strlen(const char *s);

/* Copy src (including its NUL) into dst.  Caller must guarantee dst has
 * room for strlen(src)+1 bytes.  Returns dst.  Standard footgun; the
 * allocator tests use it only with fixed buffers. */
char *strcpy(char *dst, const char *src);

/* Copy at most n bytes of src into dst.  If src is shorter than n, the
 * remainder is zero-filled.  If src is n bytes or longer, dst is NOT
 * NUL-terminated (standard strncpy).  Returns dst. */
char *strncpy(char *dst, const char *src, size_t n);

/* Lexicographic compare (unsigned chars).  Returns 0 if equal, <0 if
 * a<b, >0 if a>b. */
int strcmp(const char *a, const char *b);

/* Lexicographic compare of at most n bytes.  Returns 0 if the two are
 * equal for the first n bytes (or both hit NUL first). */
int strncmp(const char *a, const char *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* LIBC_STRING_H */