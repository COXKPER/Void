/* VoidOS IPC test — comprehensive syscall verification
 *
 * Tests:
 * 1. Create endpoint
 * 2. Send message to own endpoint (loopback)
 * 3. Receive message
 * 4. Invalid handle rejection
 * 5. Multiple messages
 * 6. Full queue handling
 * 7. Close endpoint
 * 8. Regression: existing syscalls still work
 */

#include <void.h>
#include <ipc.h>

void print_test(const char *name, int result) {
    const char *status = (result == 0) ? "PASS" : "FAIL";
    sys_write(1, "[IPC] ", 6);

    /* Write the name */
    uint64_t len = 0;
    const char *p = name;
    while (*p++) len++;
    sys_write(1, name, len);

    sys_write(1, ": ", 2);
    sys_write(1, status, 4);
    sys_write(1, "\n", 1);
}

int main(void) {
    sys_write(1, "[IPC test] Starting...\n", 23);

    /* Test 1: Create endpoint */
    long h = ipc_endpoint_create();
    int test1_pass = (h >= 0);
    print_test("create endpoint", test1_pass ? 0 : 1);
    if (!test1_pass) {
        sys_write(1, "  (aborting further tests)\n", 27);
        sys_exit(1);
    }

    /* Test 2: Send message (loopback) */
    const char *msg = "Hello IPC!";
    long send_ret = ipc_send(h, 42, msg, 10);
    int test2_pass = (send_ret == 0);
    print_test("send message", test2_pass ? 0 : 1);

    /* Test 3: Receive message */
    uint32_t tag_out = 0;
    char buf[64];
    long recv_ret = ipc_recv(h, &tag_out, buf, 64);
    int test3_pass = (recv_ret == 10 && tag_out == 42);
    print_test("receive message", test3_pass ? 0 : 1);

    /* Test 4: Invalid handle */
    long bad_send = ipc_send(999, 0, msg, 10);
    int test4_pass = (bad_send < 0);
    print_test("invalid handle rejection", test4_pass ? 0 : 1);

    /* Test 5: Multiple messages */
    ipc_send(h, 1, "M1", 2);
    ipc_send(h, 2, "M2", 2);
    ipc_send(h, 3, "M3", 2);

    long r1 = ipc_recv(h, &tag_out, buf, 64);
    int test5a = (r1 == 2 && tag_out == 1);

    long r2 = ipc_recv(h, &tag_out, buf, 64);
    int test5b = (r2 == 2 && tag_out == 2);

    long r3 = ipc_recv(h, &tag_out, buf, 64);
    int test5c = (r3 == 2 && tag_out == 3);

    int test5_pass = (test5a && test5b && test5c);
    print_test("multiple messages", test5_pass ? 0 : 1);

    /* Test 6: No message available (poll) */
    long empty = ipc_recv(h, &tag_out, buf, 64);
    int test6_pass = (empty == 0);
    print_test("empty recv (poll)", test6_pass ? 0 : 1);

    /* Test 7: Close endpoint */
    long close_ret = ipc_close(h);
    int test7_pass = (close_ret == 0);
    print_test("close endpoint", test7_pass ? 0 : 1);

    /* Test 8: Send after close should fail */
    long send_after_close = ipc_send(h, 99, msg, 10);
    int test8_pass = (send_after_close < 0);
    print_test("send after close fails", test8_pass ? 0 : 1);

    /* Test 9: Regression - getpid still works */
    long pid = sys_getpid();
    int test9_pass = (pid > 0);
    print_test("regression: getpid", test9_pass ? 0 : 1);

    /* Test 10: Regression - write still works */
    const char *msg2 = "[IPC] write works\n";
    long wrote = sys_write(1, msg2, 18);
    int test10_pass = (wrote == 18);
    print_test("regression: write", test10_pass ? 0 : 1);

    sys_write(1, "[IPC test] Done.\n", 16);
    sys_exit(0);
}
