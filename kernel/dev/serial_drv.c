/* VoidOS — serial device driver (device-layer wrapper around serial.c)
 *
 * See serial_drv.h for the model.  The register-level driver stays serial.c;
 * this module publishes it into the device registry and owns the single
 * console instance the kernel data plane uses.
 */
#include <dev/serial_drv.h>
#include <dev/serial.h>
#include <void/device.h>
#include <void/types.h>
#include <mm/kheap.h>
#include <syscall/syscall.h>   /* VE_* errno values */

static void_device_t g_serial_dev;     /* registered once, lives for boot */
static void         *g_console_inst;   /* the kernel's fd 0/1/2 instance  */

/* ── ops ──────────────────────────────────────────────────────────────── */

/* open: the stream is the shared port; the handle is a token.  The driver
 * never allocates, so the token is just a sentinel address. */
static void *serial_open(const void *dev) {
    (void)dev;
    static int token;
    return &token;
}

static int serial_close(void *priv) { (void)priv; return 0; }

/* read: non-blocking, gathers available chars up to n, stops at newline —
 * the exact loop the previous sys_read FD_SERIAL body used. */
static int serial_read(void *priv, uint64_t buf, uint64_t n) {
    (void)priv;
    char *out = (char *)buf;
    uint64_t got = 0;
    while (got < n) {
        int ch = serial_getchar();
        if (ch == -1) break;               /* no more data right now */
        out[got++] = (char)ch;
        if (ch == '\n') break;             /* shell convention: line at a time */
    }
    return (int)got;
}

/* write: one byte per putchar, returning the count written. */
static int serial_write(void *priv, const void *buf, uint64_t n) {
    (void)priv;
    const char *s = (const char *)buf;
    for (uint64_t i = 0; i < n; i++) serial_putchar(s[i]);
    return (int)n;
}

static int serial_ioctl(void *priv, uint64_t req, void *arg) {
    (void)priv; (void)req; (void)arg;
    return -VE_INVAL;
}

/* ── registration ─────────────────────────────────────────────────────── */

void serial_drv_register(void) {
    g_serial_dev.name = "serial";
    g_serial_dev.data = NULL;              /* the port is the driver state */
    g_serial_dev.type = DEV_CHAR;
    g_serial_dev.ops.open  = serial_open;
    g_serial_dev.ops.close = serial_close;
    g_serial_dev.ops.read  = serial_read;
    g_serial_dev.ops.write = serial_write;
    g_serial_dev.ops.ioctl = serial_ioctl;
    g_serial_dev.magic = 0;
    g_serial_dev.ref   = 0;
    g_serial_dev.inuse = false;

    int r = dev_register(&g_serial_dev);
    if (r != VOID_OK) {
        /* boot has no console fallback past here except kprintf */
        kprintf("[SERIAL] device registration failed (%d)\n\r", r);
        return;
    }

    g_console_inst = dev_open("serial");
    if (!g_console_inst)
        kprintf("[SERIAL] console instance open failed\n\r");
    else
        kprintf("[SERIAL] console device ready\n\r");
}

void *serial_drv_instance(void) { return g_console_inst; }