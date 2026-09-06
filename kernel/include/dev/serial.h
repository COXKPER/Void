/* VoidOS — Serial COM1 (UART 8250/16550) driver
 * Provides early debug output before any framebuffer or console exists.
 * All output goes to COM1 (I/O port 0x3F8) which QEMU maps to stdio
 * with the -serial stdio flag.
 */
#ifndef VOID_SERIAL_H
#define VOID_SERIAL_H 1

#include <stdint.h>

/* COM1 base port */
#define COM1 0x3F8

void     serial_init(void);
void     serial_putchar(char c);
void     serial_puts(const char *s);
int      serial_getchar(void);  /* non-blocking read; -1 if no data */

/* kprintf — minimal kernel printf (no floating point, no %n)
 * Supported: %d %u %x %p %s %c %%                          */
void     kprintf(const char *fmt, ...);

#endif /* VOID_SERIAL_H */
