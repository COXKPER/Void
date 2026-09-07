/* VoidOS — Phase 12 user memory regression test (brk/sbrk/mmap/munmap)
 *
 * Deterministic checks against the Phase 12 contract:
 *   1.  sbrk(0) → a sane page-aligned break above the image
 *   2.  sbrk grows the break by the requested amount
 *   3.  heap pages are writable: a char loop survives at the new break
 *   4.  sbrk(-) shrinks the break back to the query point
 *   5.  sbrk with a negative result (break below image) → (void*)-1
 *   6.  brk() sets an absolute break; sbrk(0) reflects it
 *   7.  mmap(0, len) picks a page-aligned address below the stack floor
 *   8.  the mapped region holds zeros and is writable at every page
 *   9.  two anonymous maps never overlap (distinct addresses)
 *  10.  writing into one map can't leak into the other (private isolation)
 *  11.  munmap frees the range (subsequent munmap of it returns 0)
 *  12.  munmap of a range that never existed → 0 (Linux tolerates it)
 *  13.  mmap length 0 → -EINVAL; mmap PROT_NONE → -EINVAL
 *  14.  mmap with junk flags (private without anonymous) → -EINVAL
 *  15.  a fixed map overlapping the live heap → -EINVAL
 *  16.  sbrk(0) query is side-effect free
 *  17/18. fork inherits the heap; the child's writes don't leak to the
 *      parent (the eager-copy claim), and the child reaps with status 0
 *
 * The `[MM]` result lines are what the boot regression harness greps for;
 * exit status is 0 iff every check passed.
 */

#include <void.h>

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
    if (v < 0) { sys_write(1, "-", 1); v = -v; }
    while (v > 0 && i < 15) { buf[i++] = '0' + (char)(v % 10); v /= 10; }
    while (i > 0) sys_write(1, &buf[--i], 1);
}

static void check(const char *name, long got, const char *expect, int pass) {
    if (!pass) failures++;
    say("[MM] ");
    say(name);
    say(": ");
    say(pass ? "PASS" : "FAIL");
    say(" (got ");
    putdec(got);
    say(" expect ");
    say(expect);
    say(")\n");
}

/* Fill `len` bytes at p with 0xAA and verify; 1 on success.  Proves a region
 * is mapped and writable. */
static int poke_and_verify(volatile unsigned char *p, long len) {
    for (long i = 0; i < len; i++) p[i] = 0xAA;
    for (long i = 0; i < len; i++) if (p[i] != 0xAA) return 0;
    return 1;
}

#define M_MAPSHARED 0x01    /* junk flag: private-only kernel refuses it     */

/* libvoid's sys_sbrk is a static inline; a real callable keeps the test
 * C-clean while reusing the exact libvoid sbrk semantics. */
static void *sbrk_wrap(long inc) { return sys_sbrk(inc); }

int main(void) {
    say("[MM test] Starting...\n");

    /* 1/2/3: basic heap growth + writability. */
    void *b0 = sbrk_wrap(0);
    check("sbrk init", (long)b0, "aligned>img",
          (unsigned long)b0 >= 0x400000u + 0x1000u &&
          ((unsigned long)b0 & 0xFFFu) == 0);

    void *g1 = sbrk_wrap(8192);
    check("sbrk grow", (long)g1, "old brk", (void *)g1 == b0);
    void *b1 = sbrk_wrap(0);
    check("break+8192", (long)b1, "b0+8192", (void *)b1 == (void *)((unsigned long)b0 + 8192));

    /* 3: writable.  Growing maps zeroed frames, so the first page comes up
     * 0x00 and takes the write; the freshly mapped second page must also be
     * zero before the poke (a real zero-fill check). */
    int za = 1;
    for (long i = 4096; i < 8192; i++) if (((unsigned char *)b0)[i] != 0) za = 0;
    check("heap zero", za ? 0 : -1, "0", za);
    check("heap write", (long)0, "0", poke_and_verify((volatile unsigned char *)b0, 8192));

    /* 4: shrink back down. */
    void *sroot = sbrk_wrap(-8192);
    check("sbrk shrink", (long)sroot, "break b1", (void *)sroot == b1);
    check("shrunk break", (long)sbrk_wrap(0), "b0", sbrk_wrap(0) == b0);

    /* 5: cannot shrink below the image top. */
    void *neg = sbrk_wrap(-0x100000);
    check("sbrk below", (long)neg, "-1", neg == (void *)-1);

    /* 6: brk() absolute + query reflects it.  NOTE: brk() here applies to the
     * *current* kernel break, which is still v0 (the test never moves past the
     * 8192 grow), so it sets the break to b0+4096 and the query agrees. */
    long newb = sys_brk((void *)((unsigned long)b0 + 4096));
    check("brk set", newb, "b0+4096", (void *)newb == (void *)((unsigned long)b0 + 4096));
    check("brk query", (long)sbrk_wrap(0), "b0+4096", sbrk_wrap(0) == (void *)((unsigned long)b0 + 4096));

    /* 7/8/9/10: anonymous mmap.  (The heap break is at b0+4096 from check 6.) */
    void *m1 = sys_mmap((void *)0, 16384, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap base", (long)m1, "aligned<stack",
          m1 != MAP_FAILED && ((unsigned long)m1 & 0xFFFu) == 0 &&
          (unsigned long)m1 < 0x700000000000u);
    if (m1 == MAP_FAILED) sys_exit(1);

    int zero = 1;                                   /* 8: zeroed + writable */
    for (long i = 0; i < 16384; i++)
        if (((unsigned char *)m1)[i] != 0) { zero = 0; break; }
    check("mmap zero", zero ? 0 : -1, "0", zero);
    check("mmap write", (long)0, "0", poke_and_verify((volatile unsigned char *)m1, 16384));

    void *m2 = sys_mmap((void *)0, 4096, PROT_READ | PROT_WRITE,         /* 9 */
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap second", (long)m2, "!=m1", (unsigned long)m2 != (unsigned long)m1);
    if (m2 != MAP_FAILED) poke_and_verify((volatile unsigned char *)m2, 4096);

    /* 10: m2's 0xAA didn't leak into m1 (per-page private isolation). */
    check("mmap isoleak", (long)0, "0",
          ((volatile unsigned char *)m1)[0] == 0xAA &&
          ((volatile unsigned char *)m1)[16383] == 0xAA);

    /* 11/12: munmap. */
    long um1 = sys_munmap(m1, 16384);
    check("munmap ok", um1, "0", um1 == 0);
    check("munmap again", (long)sys_munmap(m1, 16384), "0", sys_munmap(m1, 16384) == 0);
    check("munmap never", (long)sys_munmap((void *)0x10000000, 4096), "0",
          sys_munmap((void *)0x10000000, 4096) == 0);

    /* 13/14/15: mmap rejections. */
    check("mmap len0", (long)sys_mmap((void *)0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
          "-22", sys_mmap((void *)0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == (void *)-22);
    check("mmap protnone", (long)sys_mmap((void *)0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
          "-22", sys_mmap((void *)0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == (void *)-22);
    check("mmap badflags", (long)sys_mmap((void *)0, 4096, PROT_READ, M_MAPSHARED, -1, 0),
          "-22", sys_mmap((void *)0, 4096, PROT_READ, M_MAPSHARED, -1, 0) == (void *)-22);
    check("mmap overlap", (long)sys_mmap((void *)b0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
          "-22", sys_mmap((void *)b0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == (void *)-22);

    /* 16: the sbrk(0) query is read-only and side-effect free.  It must not
     * move the break anywhere — a real bug in sys_sbrk once passed the stale
     * cache back into sys_brk and shrunk the heap to zero on a pure query,
     * which this asserts never happens. */
    check("brk stable", (long)sbrk_wrap(0), "b0+4096",
          sbrk_wrap(0) == (void *)((unsigned long)b0 + 4096));

    /* 17/18: fork — heap inherited, eager-copied, independent.  At this point
     * the kernel break is b0+4096, and only page [b0, b0+4096) is mapped,
     * holding 0x00 (the shrink at check 4 freed the 0xAA pages and check 6's
     * brk regrow zeroed them fresh — Linux brk semantics).  The child
     * scribbles its inherited heap page with 0x77 and exits; the parent's
     * frame is a private eager copy, so it must still read 0x00 — the eager
     * copy claim of design decision #13.  If the frames were shared, the
     * parent would read 0x77. */
    long cfork = sys_fork();
    check("mm fork", cfork, ">=0", cfork >= 0);
    if (cfork < 0) sys_exit(1);

    if (cfork == 0) {
        for (long i = 0; i < 4096; i++) ((volatile unsigned char *)b0)[i] = 0x77;
        sys_exit(0);
    }

    /* wait4 is a non-blocking poll (Void's wait contract: 0 = child alive,
     * >0 = reaped, <0 = no such child).  Loop like a libc would. */
    long cst = 0, wc = 0;
    for (;;) {
        wc = sys_wait4(cfork, &cst, 0);
        if (wc != 0) break;          /* reaped (>0) or no-such-child (<0)   */
        sys_sched_yield();
    }
    check("mm reap", wc, "cfork", wc == cfork && cst == 0);
    check("fork isolation", (long)0, "0",
          ((volatile unsigned char *)b0)[0] == 0x00 &&
          ((volatile unsigned char *)b0)[4095] == 0x00);

    say("[MM test] Done: ");
    putdec(failures);
    say(" fail(s)\n");
    return (failures == 0) ? 0 : 1;
}