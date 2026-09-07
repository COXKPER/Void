/* VoidOS — serial device driver (device-layer wrapper around serial.c)
 *
 * The one hardware driver Void integrates into the device layer in this
 * phase.  It wraps the Phase-4 serial register driver (serial.c) unchanged
 * — same init, same polling UART — so console behavior is byte-identical:
 * kprintf/serial output/VGA routing all keep working through the console
 * layer untouched.  What changes is how the syscall data plane reaches the
 * port: sys_read(0)/sys_write(1) now go through dev_read/dev_write on a
 * kernel-minted console instance instead of calling serial.c directly.
 *
 * The console is a single shared terminal, so the instance is kernel-owned,
 * opened once at boot, never closed — no process holds a device reference,
 * so process exit cannot leak one.  A future per-open device (an fd type
 * beyond FD_SERIAL) reuses dev_open/dev_close per process; this driver only
 * has to exist once.
 *
 * ops borrow from serial.c directly:
 *   open   — a token handle; the stream itself is the shared port
 *   read   — non-blocking gather, stops at '\n' or when no data, mirrors the
 *            previous sys_read loop exactly
 *   write  — one byte per serial_putchar, mirrors sys_write exactly
 *   ioctl  — none (-VE_INVAL)
 */
#ifndef VOID_SERIAL_DRV_H
#define VOID_SERIAL_DRV_H 1

#include <void/device.h>

/* Register the "serial" device in the kernel registry and mint the single
 * kernel console instance for fd 0/1/2.  Call once at boot after dev_init()
 * (needs the heap for the instance wrapper). */
void serial_drv_register(void);

/* The kernel's console instance (fd 0/1/2 route here).  Never NULL after
 * serial_drv_register(). */
void *serial_drv_instance(void);

#endif /* VOID_SERIAL_DRV_H */