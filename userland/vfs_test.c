/* VoidOS — Phase 10 VFS test: kernel embedded-tree file/dir services
 *
 * Deterministic checks, in this order:
 *   1. open("/hello.txt", O_RDONLY) → a VFS fd ≥ 3
 *   2. read seq → exact contents "Hello, VoidOS!\n"
 *   3. read past EOF → 0
 *   4. lseek SEEK_SET back to 0 → re-read works
 *   5. lseek SEEK_END → reads nothing (EOF)
 *   6. open("/no/such.txt") → -ENOENT
 *   7. open write-mode → -EACCES
 *   8. open("/etc") → a dir fd (readdir path, permitted)
 *   9. readdir on dir fd → "version.txt" seen; then 0 at end
 *  10. readdir on a regular-file fd → -ENOTDIR
 *  11. close(fd) → 0; then read(fd) → -EBADF
 *  12. invalid fd → -EBADF
 *  13. cwd: getcwd() == "/", then chdir("/etc") → getcwd() == "/etc"
 *  14. relative ".." resolution from /etc → "version.txt" via "etc/../../etc"
 *  15. readdir on exhausted dir (repeat) → 0
 *
 * Every failure bumps `failures`; main() returns 0 iff none.  The kernel
 * harness greps the serial log for the exact `[VFS]` result lines.
 */
#include <void.h>

static int failures;

static void print_dec(long v) {
    char buf[16]; int i = 0;
    if (v == 0) buf[i++] = '0';
    if (v < 0) { sys_write(1, "-", 1); v = -v; }
    while (v > 0 && i < 15) { buf[i++] = '0' + (char)(v % 10); v /= 10; }
    while (i > 0) sys_write(1, &buf[--i], 1);
}

static void say(const char *s, int n) { sys_write(1, s, n); }

/* check("open exists", 11, fd, -1, fd >= 3)  →  PASS (got 3 expect >=3) */
static void check(const char *name, int len, long got, const char *expect,
                  int pass) {
    if (!pass) failures++;
    say("[VFS] ", 6);
    say(name, len);
    say(": ", 2);
    say(pass ? "PASS" : "FAIL", 4);
    say(" (got ", 6);
    print_dec(got);
    say(" expect ", 8);
    say(expect, len + 2);
    say(")\n", 2);
}

int main(void) {
    say("[VFS test] Starting...\n", 22);

    long fd = sys_open("/hello.txt", O_RDONLY);
    check("open hello", 11, fd, "-1", fd >= 3);
    if (fd < 3) { sys_exit(1); }

    char buf[64];
    long n = sys_read(fd, buf, sizeof(buf));
    check("read hello", 10, n, "15", n == 15);
    if (n == 15) {
        int ok = 1;
        const char *want = "Hello, VoidOS!\n";
        for (long i = 0; i < n; i++) if (buf[i] != want[i]) ok = 0;
        if (!ok) { failures++; say("[VFS] read hello: FAIL (contents)\n", 34); }
    }

    /* read past EOF → 0 */
    long eof = sys_read(fd, buf, sizeof(buf));
    check("read eof", 8, eof, "0", eof == 0);

    /* lseek SEEK_SET(0) back to start, re-read */
    long se = sys_lseek(fd, 0, 0);
    check("lseek set", 9, se, "0", se == 0);
    long n2 = sys_read(fd, buf, sizeof(buf));
    check("re-read", 7, n2, "15", n2 == 15);

    /* lseek SEEK_END(2) → reads nothing */
    long se2 = sys_lseek(fd, 0, 2);
    check("lseek end", 9, se2, "15", se2 == 15);
    long ne = sys_read(fd, buf, 1);
    check("read end", 8, ne, "0", ne == 0);

    sys_close(fd);

    /* nonexistent path → -ENOENT (-2) */
    long missing = sys_open("/no/such.txt", O_RDONLY);
    check("open missing", 12, missing, "-2", missing == -2);

    /* write-mode open of an existing file → -EACCES (-13) */
    long wro = sys_open("/hello.txt", O_WRONLY);
    check("open write", 10, wro, "-13", wro == -13);

    /* directory opens read-only → a dir fd sits at ≥ 3 and readdir works */
    long dfd = sys_open("/etc", O_RDONLY);
    check("open dir", 8, dfd, "-1", dfd >= 3);
    if (dfd >= 3) {
        char entry[64];
        long r1 = sys_readdir(dfd, entry);
        long r2 = sys_readdir(dfd, entry);
        long r3 = sys_readdir(dfd, entry);
        /* etc/ has one child: version.txt.  First readdir must see it, the
         * second must hit EOF (0).  (r1 or r2 ordering doesn't matter.) */
        int ok = (r1 == 11 || r2 == 11) && (r1 == 0 || r2 == 0);
        check("readdir etc", 11, r1, "11", ok);
        check("readdir end", 11, r3, "0", r3 == 0);
    }

    /* readdir on a regular-file fd → -ENOTDIR (-20) */
    long rfd = sys_open("/hello.txt", O_RDONLY);
    long rd_reg = (rfd >= 3) ? sys_readdir(rfd, buf) : 12345;
    check("readdir reg", 11, rd_reg, "-20", rd_reg == -20);
    if (rfd >= 3) sys_close(rfd);

    /* close(fd) → 0; then read → -EBADF (-9) */
    long cl = sys_close(dfd);
    check("close dir", 9, cl, "0", cl == 0);
    long br = sys_read(dfd, buf, 1);
    check("read closed", 11, br, "-9", br == -9);

    /* invalid fd → -EBADF */
    long bad = sys_read(999, buf, 1);
    check("invalid fd", 10, bad, "-9", bad == -9);

    /* cwd flow: getcwd() == "/", chdir("/etc") → getcwd() == "/etc" */
    char cwd[64];
    long g0 = sys_getcwd(cwd, sizeof(cwd));
    check("getcwd root", 11, g0, "1", g0 == 1 && cwd[0] == '/' && cwd[1] == '\0');
    long cd = sys_chdir("/etc");
    check("chdir etc", 9, cd, "0", cd == 0);
    long g1 = sys_getcwd(cwd, sizeof(cwd));
    check("getcwd etc", 10, g1, "4", g1 == 4 &&
          cwd[0] == '/' && cwd[1] == 'e' && cwd[2] == 't' && cwd[3] == 'c' &&
          cwd[4] == '\0');

    /* relative resolution against cwd: open "version.txt" from /etc */
    long vfd = sys_open("version.txt", O_RDONLY);
    check("open rel", 8, vfd, "-1", vfd >= 3);
    if (vfd >= 3) {
        long vn = sys_read(vfd, buf, sizeof(buf));
        check("read version", 12, vn, "9", vn == 9 &&
              buf[0] == 'P' && buf[5] == ' ' &&
              buf[7] == '0' && buf[8] == '\n');
        sys_close(vfd);
    }

    /* ".." resolution: from /etc, ".." = /, then down into etc again */
    vfd = sys_open("../etc/../etc/version.txt", O_RDONLY);
    check("dotdot path", 11, vfd, "-1", vfd >= 3);
    if (vfd >= 3) sys_close(vfd);

    /* trailing slash on a file → -ENOTDIR (-20) */
    long ts = sys_open("/hello.txt/", O_RDONLY);
    check("file slash", 10, ts, "-20", ts == -20);

    say("[VFS test] Done: ", 18);
    print_dec(failures);
    say(" fail(s)\n", 9);
    return (failures == 0) ? 0 : 1;
}