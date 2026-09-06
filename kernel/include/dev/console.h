/* VoidOS — Console output routing layer
 *
 * Provides a unified kprintf that routes to VGA framebuffer or COM1 serial
 * based on kernel boot parameters (kout=vga or kout=serial).
 *
 * Default: kout=serial (for early debugging)
 * Later: kout=vga (production mode)
 */
#ifndef VOID_CONSOLE_H
#define VOID_CONSOLE_H 1

#include <stdint.h>

/* ── output backend selection ────────────────────────────────────────────── */
typedef enum {
    KOUT_SERIAL = 0,   /* COM1 UART */
    KOUT_VGA    = 1,   /* GOP framebuffer */
} kout_backend_t;

/* ── initialization and configuration ────────────────────────────────────── */

/* Parse kernel boot parameters and set kprintf output backend.
 * Called early from kernel_main before any kprintf output.
 * Respects kout=serial or kout=vga from boot command line (if available).
 * Default: KOUT_SERIAL (diagnostic mode). */
void console_init(void);

/* Get the current output backend. */
kout_backend_t console_get_backend(void);

/* Force the output backend (for testing/debugging). */
void console_set_backend(kout_backend_t backend);

/* ── kprintf — unified kernel printf ──────────────────────────────────────
 * Routes formatted output through the selected backend.
 * Supported: %d %u %x %p %s %c %%  */
void kprintf(const char *fmt, ...);

#endif /* VOID_CONSOLE_H */
