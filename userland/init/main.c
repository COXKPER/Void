/* VoidOS — first userland process (init)
 *
 * Freestanding executable entry point. Calls getpid/write/sched_yield/exit
 * to verify Ring 3 syscall ABI, process identity, and clean shutdown.
 */

#include <void.h>

int main(void) {
    /* Get our PID. */
    long pid = sys_getpid();

    /* Write a banner. */
    const char msg[] = "[init] Hello from Ring 3!\r\n";
    sys_write(1, msg, sizeof(msg) - 1);

    /* Write PID in hex. */
    char buf[64];
    int len = 0;

    const char prefix[] = "[init] PID = 0x";
    for (int i = 0; prefix[i]; i++) buf[len++] = prefix[i];

    /* Convert PID to hex (up to 64-bit). */
    uint64_t p = (uint64_t)pid;
    int shift = 60;
    int started = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t nibble = (p >> shift) & 0xF;
        shift -= 4;
        if (nibble == 0 && !started) continue;
        started = 1;
        buf[len++] = nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10);
    }
    buf[len++] = '\r';
    buf[len++] = '\n';

    sys_write(1, buf, len);

    /* Yield a few times to let scheduler interleave. */
    for (int i = 0; i < 3; i++) {
        sys_sched_yield();
    }

    /* Write exit status. */
    const char exit_msg[] = "[init] exiting with status 42\r\n";
    sys_write(1, exit_msg, sizeof(exit_msg) - 1);

    /* Exit with status 42 (known value for testing). */
    sys_exit(42);

    return 0; /* never reached */
}
