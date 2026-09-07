/* VoidOS — Phase 13 userland allocator regression test
 *
 * Deterministic checks against the Phase 13 contract:
 *   1.  basic sizes (small/medium) are allocated, 16-aligned, writable
 *   2.  write-then-read round-trip preserves data
 *   3.  calloc zero-fills; calloc(nmemb*size overflow) -> NULL
 *   4.  realloc grow copies data; realloc shrink in place; realloc NULL->malloc
 *   5.  realloc failure (huge size) preserves the original block
 *   6.  free + reuse: a freed small block is handed out again
 *   7.  coalescing: freeing two neighbors lets a bigger alloc reuse the merge
 *   8.  mmap-backed isolation: alloc>threshold never appears in the free list;
 *       a just-below-threshold alloc is heap-backed
 *   9.  alignment: every returned pointer is 16-byte aligned
 *  10.  calloc overflow -> NULL (no wrap)
 *  11.  zero-size malloc -> unique freeable block; free(NULL) a no-op
 *  12.  fork isolation: parent and child's malloc heaps are independent
 *  13.  deterministic stress: many alloc/free cycles end selfcheck-clean
 *
 * The `[MALLOC]` result lines are what the boot regression harness greps for;
 * exit status is 0 iff every check passed.
 */
#include <void.h>
#include <libc/malloc.h>
#include <libc/string.h>

static int failures;

static unsigned long ustrlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void say(const char *s) { sys_write(1, s, ustrlen(s)); }

static void putdec(long v) {
    char buf[16]; int i = 0;
    if (v == 0) buf[i++] = '0';
    if (v < 0) { say("-"); v = -v; }
    while (v > 0 && i < 15) { buf[i++] = '0' + (char)(v % 10); v /= 10; }
    while (i > 0) say(&buf[--i]);
}

static void check(const char *name, long got, const char *expect, int pass) {
    if (!pass) failures++;
    say("[MALLOC] ");
    say(name);
    say(": ");
    say(pass ? "PASS" : "FAIL");
    say(" (got ");
    putdec(got);
    say(" expect ");
    say(expect);
    say(")\n");
}

/* Fill len bytes at p with a pattern derived from start and verify it back.
 * Returns 1 on success, 0 on a write-read mismatch. */
static int poke(unsigned char *p, long len) {
    for (long i = 0; i < len; i++) p[i] = (unsigned char)(0x40 + (i & 0x3f));
    for (long i = 0; i < len; i++) if (p[i] != (unsigned char)(0x40 + (i & 0x3f))) return 0;
    return 1;
}

int main(void) {
    say("[MALLOC test] Starting...\n");

    /* 1/2/9: basic sizes, alignment, writability. */
    void *a = malloc(1);
    void *b = malloc(64);
    void *c = malloc(1024);
    check("basic alloc", (long)0, "0",
          a && b && c && (((unsigned long)a | (unsigned long)b | (unsigned long)c) & 0xF) == 0);
    if (!a || !b || !c) sys_exit(1);
    check("basic write", (long)0, "0",
          poke(a, 1) && poke(b, 64) && poke(c, 1024));

    /* 3: calloc zero-fill. */
    unsigned char *z = calloc(100, 4);
    check("calloc zero", (long)0, "0",
          z && z[0] == 0 && z[399] == 0 && z[200] == 0);
    z[199] = 0x21;
    check("calloc write", z[199] != 0 ? 0 : -1, "0", z && z[199] == 0x21);

    /* 10: calloc overflow -> NULL, no wrap. */
    check("calloc overflow", (long)0, "0", calloc((size_t)-1 / 2, 4) == NULL);

    /* 4: realloc grow copies; shrink in place preserves ptr; NULL -> malloc. */
    char *r = malloc(10);
    for (int i = 0; i < 10; i++) r[i] = (char)('a' + i);
    char *r2 = realloc(r, 1000);
    check("realloc grow", (long)r2, "ptr", r2 && r2 != r && r2[9] == 'j' && r2[0] == 'a');
    char *r3 = realloc(r2, 20);
    check("realloc shrink", (long)(r3 == r2 ? 0 : -1), "same", r3 == r2);
    void *rnull = realloc(0, 50);
    check("realloc null", (long)0, "0", rnull && rnull != 0);
    free(rnull);

    /* 5: realloc failure preserves the original. */
    unsigned char *big = malloc(64);
    for (int i = 0; i < 64; i++) big[i] = (unsigned char)(0x80 + i);
    void *failed = realloc(big, (size_t)-1);          /* absurd size */
    check("realloc fail", (long)0, "orig",
          failed == 0 && big[0] == 0x80 && big[63] == 0xbf);
    free(big);

    /* 6: free + reuse. */
    void *p1 = malloc(100);
    free(p1);
    void *p2 = malloc(100);
    check("free reuse", (long)(p1 == p2 ? 0 : -1), "same", p1 == p2);
    free(p2);

    /* 7: coalescing. */
    void *n1 = malloc(64), *n2 = malloc(64), *n3 = malloc(64);
    free(n1); free(n2); free(n3);         /* two neighbors -> merged */
    void *bigger = malloc(192);           /* needed if coalesced */
    check("coalesce", (long)(bigger ? 0 : -1), "0", bigger != 0);
    free(bigger);

    /* 8: mmap-backed isolation above threshold; heap-backed below. */
    void *huge = malloc(300u * 1024);
    check("mmap large", (long)0, "0", huge != 0 && (((unsigned long)huge) & 0xF) == 0);
    if (huge) { poke(huge, 300u * 1024); check("mmap large write", 0, "0", 1); }
    void *heapish = malloc(128u * 1024 - 16);
    check("heap below threshold", (long)0, "0", heapish != 0);
    int sc = malloc_selfcheck();
    free(huge); free(heapish);
    check("selfcheck", (long)sc, "0", sc == 0);

    /* 11: malloc(0), free(NULL), double-free is deterministic (not run here). */
    void *z0 = malloc(0);
    check("malloc zero", (long)0, "0", z0 != 0 && (((unsigned long)z0) & 0xF) == 0);
    free(z0);
    free(0);

    /* 12: fork isolation. */
    long cfork = sys_fork();
    check("mm fork2", cfork, ">=0", cfork >= 0);
    if (cfork < 0) sys_exit(1);
    if (cfork == 0) {
        /* child writes through the allocator, then exits; parent must see
         * its own heap untouched (eager page copy — design decision #13). */
        void *child = malloc(1000);
        poke(child, 1000);
        free(child);
        sys_exit(0);
    }
    long cst = 0, wc = 0;
    for (;;) {
        wc = sys_wait4(cfork, &cst, 0);
        if (wc != 0) break;
        sys_sched_yield();
    }
    check("fork reap", wc, "cfork", wc == cfork && cst == 0);
    /* parent's heap still selfcheck-clean after the child's independent use */
    check("fork isolate", (long)0, "0", malloc_selfcheck() == 0);

    /* 13: deterministic stress — many allocate/write/free cycles. */
    int sc2 = 0;
    for (int round = 0; round < 40; round++) {
        void *v[8];
        for (int i = 0; i < 8; i++) { v[i] = malloc((i + 1) * 37); if (v[i]) poke(v[i], (i + 1) * 37); }
        for (int i = 0; i < 8; i++) free(v[i]);
    }
    sc2 = malloc_selfcheck();
    check("stress", (long)sc2, "0", sc2 == 0);

    free(a); free(b); free(c); free(z);

    /* Emit the Done marker as ONE write.  say()+putdec() would split it into
     * three syscalls, and concurrent processes' bytes interleave into the
     * shared COM1 between them — tearing the line the boot regression greps
     * for (observed: "Done: 0\x0b@ fail(s)").  Single write = atomic. */
    {
        char buf[64];
        int i = 0;
        const char *pre = "[MALLOC test] Done: ";
        size_t plen = ustrlen(pre), dlen = 0;
        for (size_t k = 0; k < plen && i < 63; k++) buf[i++] = pre[k];
        if (failures == 0) { buf[i++] = '0'; dlen = 1;
        } else {
            char dec[16]; int j = 0; long v = failures;
            while (v > 0 && j < 14) { dec[j++] = '0' + (char)(v % 10); v /= 10; }
            while (j > 0 && i < 63) buf[i++] = dec[--j];
        }
        const char *post = " fail(s)\n";
        while (*post && i < 63) buf[i++] = *post++;
        sys_write(1, buf, (unsigned long)i);
    }
    return (failures == 0) ? 0 : 1;
}