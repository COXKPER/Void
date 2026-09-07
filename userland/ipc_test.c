/* VoidOS IPC test — comprehensive syscall verification
 *
 * Tests:
 * 1. Create endpoint
 * 2. Send message to own endpoint (loopback)
 * 3. Receive message (tag + data preserved)
 * 4. Invalid handle rejection
 * 5. Multiple messages (FIFO order)
 * 6. Empty recv (poll returns 0)
 * 7. Send with invalid user pointer → -EFAULT
 * 8. Close endpoint
 * 9. Send after close fails
 * 10. Regression: getpid still works
 * 11. Regression: write still works
 */

#include <void.h>
#include <ipc.h>


static int failures;

/* Print "<pre><count> fail(s)\n" as ONE sys_write.  The old split across
 * three calls let other processes' bytes interleave into the shared COM1
 * stream between the writes, tearing the "[IPC test] Done:" line the boot
 * regression greps for. */
static void say_done(const char *pre) {
    char buf[48]; size_t n = 0;
    while (*pre && n < 46) buf[n++] = *pre++;
    char dec[16]; int i = 0;
    if (failures == 0) dec[i++] = '0';
    else { long v = failures; while (v > 0 && i < 14) { dec[i++] = '0' + (char)(v % 10); v /= 10; } }
    while (i > 0 && n < 47) buf[n++] = dec[--i];
    const char *post = " fail(s)\n";
    while (*post && n < 48) buf[n++] = *post++;
    sys_write(1, buf, (long)n);
}

/* All test names are const string literals held in .rodata.  Each one is
 * printed via its own sys_write call — no runtime strlen over user memory,
 * which is exactly the path being exercised. */

static void check(const char *name, int len, long got, const char *expect, int pass) {
    if (!pass) failures++;
    sys_write(1, "[IPC] ", 6);
    sys_write(1, name, len);
    sys_write(1, ": ", 2);
    sys_write(1, pass ? "PASS" : "FAIL", 4);
    sys_write(1, " (got ", 6);
    print_dec(got);
    sys_write(1, " expect ", 8);
    sys_write(1, expect, len);
    sys_write(1, ")\n", 2);
}

int main(void) {
    sys_write(1, "[IPC test] Starting...\n", 23);

    /* Test 1: Create endpoint */
    long h = ipc_endpoint_create();
    check("create endpoint", 15, h, "-1", h >= 0);

    /* Test 2: Send message (loopback) */
    const char msg[] = { 'H', 'i' };
    long s1 = ipc_send((int)h, 7, msg, 2);
    check("send message", 12, s1, "0", s1 == 0);

    /* Test 3: Receive message */
    uint32_t tag = 99;
    char rbuf[8];
    long r1 = ipc_recv((int)h, &tag, rbuf, 8);
    check("recv message", 12, r1, "2", r1 == 2 && tag == 7 && rbuf[0] == 'H' && rbuf[1] == 'i');

    /* Test 4: Invalid handle */
    long bad_send = ipc_send(999, 0, msg, 2);
    check("invalid handle", 14, bad_send, "-9", bad_send == -9);

    /* Test 5: Multiple messages (FIFO) */
    ipc_send((int)h, 1, "M1", 2);
    ipc_send((int)h, 2, "M2", 2);
    ipc_send((int)h, 3, "M3", 2);
    tag = 0;
    long ra = ipc_recv((int)h, &tag, rbuf, 8);
    int fifo_a = (ra == 2 && tag == 1);
    tag = 0;
    long rb = ipc_recv((int)h, &tag, rbuf, 8);
    int fifo_b = (rb == 2 && tag == 2);
    tag = 0;
    long rc = ipc_recv((int)h, &tag, rbuf, 8);
    int fifo_c = (rc == 2 && tag == 3);
    check("fifo order", 10, ra, "2", fifo_a && fifo_b && fifo_c);

    /* Test 6: Empty recv (poll returns 0) */
    long empty = ipc_recv((int)h, &tag, rbuf, 8);
    check("empty recv", 10, empty, "0", empty == 0);

    /* Test 7: Invalid user pointer → -EFAULT (-14).  Send from a NULL data
     * pointer: user_range_ok rejects it because NULL is below USER_MIN. */
    long e1 = ipc_send((int)h, 5, (const void *)0, 4);
    check("bad ptr -EFAULT", 15, e1, "-14", e1 == -14);

    /* Test 8: Close endpoint */
    long cl = ipc_close((int)h);
    check("close endpoint", 14, cl, "0", cl == 0);

    /* Test 9: Send after close fails with EBADF (-9) */
    long sc = ipc_send((int)h, 9, "X", 1);
    check("send-after-close", 16, sc, "-9", sc == -9);

    /* Test 10: Regression - getpid */
    long pid = sys_getpid();
    check("getpid", 6, pid, "0", pid > 0);

    /* Test 11: Regression - write */
    long w = sys_write(1, "[IPC] write works\n", 18);
    check("write", 5, w, "18", w == 18);

    say_done("[IPC test] Done: ");
    return (failures == 0) ? 0 : 1;
}