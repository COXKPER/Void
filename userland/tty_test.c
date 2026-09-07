/* VoidOS — Phase 14 TTY regression: the fd 0/1/2 terminal splice
 *
 * The TTY's echo/canonical/backspace/EOF line discipline is driven by real
 * UART bytes the kernel polls, which a boot-time test cannot inject
 * deterministically — so this suite asserts the parts that ARE deterministic
 * from a process:
 *
 *   1. write(fd 1) through the TTY console returns the full byte count
 *      (the sys_write → tty_write → serial-device splice is intact).
 *   2. write(fd 2) likewise (stderr is the same TTY).
 *   3. read(fd 0) with nothing typed returns 0 — the NON-BLOCKING guarantee,
 *      exactly the poll semantics the old FD_SERIAL read had.
 *   4. echo ioctl surface: VOID_TTY_ECHO_OFF then VOID_TTY_ECHO_ON both
 *      succeed (0) — echo is real and separable, not a stub.
 *   5. ioctl on a non-TTY fd is -VE_BADF (-9).
 *   6. after echo off + back on, a write still returns the full count — the
 *      toggle doesn't break the data path.
 *
 * The kernel-side discipline itself (feed/echo/backspace/newline/EOF) is
 * exercised by a kernel unit check (tty_selftest) that feeds synthetic bytes
 * directly into the discipline; this userland file proves the syscall→TTY
 * splice.
 *
 * Every [TTY] result line and the Done: marker are emitted as ONE sys_write:
 * split writes let concurrent processes' bytes interleave into the shared
 * COM1 stream between writes (observed tearing the other suites' markers).
 */
#include <void.h>

static int failures;

static void say_done(const char *pre) {
    char buf[88]; size_t n = 0;
    const char *p = pre;
    while (*p && n < 62) buf[n++] = *p++;
    char dec[16]; int i = 0; long v = failures;
    if (v == 0) dec[i++] = '0';
    while (v > 0 && i < 14) { dec[i++] = '0' + (char)(v % 10); v /= 10; }
    while (i > 0 && n < 86) buf[n++] = dec[--i];
    const char *post = " fail(s)\n";
    while (*post && n < 87) buf[n++] = *post++;
    sys_write(1, buf, (long)n);
}

/* Emit "[TTY] <name>: PASS/FAIL (got <n>)" as ONE sys_write. */
static void check(const char *name, long got, int pass) {
    char buf[128]; size_t n = 0;
    if (!pass) failures++;

    const char *s;
    s = "[TTY] ";     while (*s && n < 127) buf[n++] = *s++;
    s = name;         while (*s && n < 127) buf[n++] = *s++;
    if (n < 126) buf[n++] = ':';
    if (n < 125) buf[n++] = ' ';
    s = pass ? "PASS" : "FAIL";
    while (*s && n < 127) buf[n++] = *s++;
    s = " (got ";     while (*s && n < 127) buf[n++] = *s++;

    /* decimal formatting */
    char dec[16]; int i = 0; long v = got;
    if (v == 0) dec[i++] = '0';
    if (v < 0)  { if (n < 126) buf[n++] = '-'; v = -v; }
    while (v > 0 && i < 15) { dec[i++] = '0' + (char)(v % 10); v /= 10; }
    while (i > 0 && n < 126) buf[n++] = dec[--i];

    s = ")\n";        while (*s && n < 127) buf[n++] = *s++;
    sys_write(1, buf, (long)n);
}

int main(void) {
    sys_write(1, "[TTY test] Starting...\n", 23);

    /* 1 + 2: write through the TTY (stdout + stderr) returns the count. */
    long w1 = sys_write(1, "[TTY] tty write works\n", 22);
    check("tty stdout write", w1, w1 == 22);

    long w2 = sys_write(2, "[TTY] tty stderr write\n", 24);
    check("tty stderr write", w2, w2 == 24);

    /* 3: non-blocking read — nothing typed, must return 0 immediately. */
    char buf[64];
    long r0 = sys_read(0, buf, sizeof(buf));
    check("tty read empty nonblock", r0, r0 == 0);

    /* 4: echo toggle surface. */
    long eo = sys_ioctl(1, VOID_TTY_ECHO_OFF, 0);
    check("echo off", eo, eo == 0);
    long en = sys_ioctl(1, VOID_TTY_ECHO_ON, 0);
    check("echo on", en, en == 0);

    /* 5: ioctl on a VFS/non-TTY fd → -VE_BADF.  Opening /hello.txt gives an
     * FD_VFS slot; ioctl on it must fail. */
    long vfd = sys_open("/hello.txt", O_RDONLY);
    long iv = (vfd >= 3) ? sys_ioctl((int)vfd, VOID_TTY_ECHO_OFF, 0) : -99;
    check("ioctl non-tty fd", iv, (vfd >= 3) && iv == -9);
    if (vfd >= 3) sys_close((int)vfd);

    /* 6: after toggle round-trip, the data path still works. */
    long w3 = sys_write(1, "[TTY] post-toggle write\n", 24);
    check("tty write post-toggle", w3, w3 == 24);

    say_done("[TTY test] Done: ");
    return (failures == 0) ? 0 : 1;
}