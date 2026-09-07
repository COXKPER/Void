/* VoidOS — kernel terminal (TTY) API
 *
 * The TTY is the console splice fd 0/1/2 route through.  It owns the line
 * discipline (canonical input, echo, backspace, newline, EOF) on top of the
 * same serial console instance fd 1/2 already used, so output, echo and
 * `kprintf` all share one terminal.
 *
 * Reads are NON-BLOCKING by design: Void has no wait queues yet, and the
 * spec forbids redesigning the scheduler for the TTY.  `tty_read` pumps the
 * UART once and returns what is staged (0 when nothing is ready) — the same
 * poll semantics the previous FD_SERIAL read had.
 *
 * Only the ioctls below exist; everything else is -VE_INVAL (no invented
 * termios struct).  Echo on/off is the canonical toggle a test can flip.
 */
#ifndef VOID_TTY_H
#define VOID_TTY_H 1

#include <void/types.h>

/* tty_init — reset the line discipline.  Called once at boot after the
 * serial device is registered (the TTY splices on top of it). */
void tty_init(void);

/* tty_read — drain staged input.  `buf` is a kernel staging buffer the
 * caller copies from into user space.  Returns bytes (0 = nothing ready)
 * or -errno. */
int tty_read(uint64_t buf, uint64_t n);

/* tty_write — write n bytes to the console.  Returns bytes or -errno. */
int tty_write(const char *buf, uint64_t n);

/* tty_ioctl — negative-errno or 0 on success. */
int tty_ioctl(uint64_t req, void *arg);

/* ── the small terminal-state surface ───────────────────────────────────── */
#define VOID_TTY_ECHO_ON  0x545401   /* set echo on (canonical default) */
#define VOID_TTY_ECHO_OFF 0x545402   /* set echo off (read passes bytes) */

/* ── kernel self-check ────────────────────────────────────────────────────
 * Deterministic [TTY] PASS/FAIL lines, run after serial_drv_register + tty_init
 * (before the scheduler).  Feeds synthetic bytes straight into the discipline
 * to prove echo/backspace/newline/drain without real UART timing. */
void tty_selftest(void);

#endif /* VOID_TTY_H */