/* VoidOS — kernel device layer API
 *
 * The minimal device abstraction Void introduces before implementing any
 * complex driver.  A device is a named object with a small ops table; the
 * kernel holds devices in a static registry so drivers can be registered,
 * looked up by name and opened.  Opening a device mints a wrapper instance
 * that the syscall console path (and, later, a VFS /dev backend) holds —
 * the same discipline the fd table and IPC handle table use, so nothing can
 * outlive its driver's unregister.
 *
 * This is deliberately NOT a Linux-style driver model (no node/bus/class,
 * no /dev tree, no permissions).  It exists so the console can be routed
 * through one seam and so a future driver can move to a user-space service
 * behind IPC: process → syscall → kernel device layer → driver now,
 * process → IPC → device service → kernel device layer → driver later.
 *
 * The driver ultimately owns its device struct and its per-open private
 * handle; the registry only stores the device's address, and `dev_unregister`
 * refuses to remove a device that still has open references.  A stale
 * instance is never reachable: entries are zeroed on unregister and every
 * public access re-checks liveness before touching an ops pointer.
 *
 * Lux provenance: Lux's kernel has no in-kernel driver table — the closest
 * shape (kernel/irq.c) forwards IRQ notifications to user-space drivers over
 * a socket, and platform.h is a CPU HAL.  The named-table + open-refcount
 * discipline below is Void-native, shaped like Void's existing IPC handle
 * table (Phase 7).  See CLAUDE.md Phase 11 for the classification.
 */
#ifndef VOID_DEVICE_H
#define VOID_DEVICE_H 1

#include <void/types.h>

/* Device type bits (a probe question, not a decision).  Serial is the only
 * registered device today; framebuffer and keyboard are named for the
 * drivers that will follow. */
typedef enum {
    DEV_NONE      = 0x00,
    DEV_CHAR      = 0x01,   /* character device (serial: a byte stream)  */
    DEV_FB        = 0x02,   /* framebuffer device (console, future)     */
    DEV_INPUT     = 0x04,   /* input device (keyboard, future)          */
    DEV_BLOCK     = 0x08,   /* block device (disk, future)              */
} dev_type_t;

/* Ops a driver may implement.  `priv` is the per-open handle the driver's
 * own `open` returns; the wrapper instance the layer hands out carries it.
 * All ops return int32_t (-errno on failure). */
typedef struct void_device_ops {
    void *(*open)   (const void *dev);    /* parent device data; NULL = refused */
    int   (*close)  (void *priv);         /* 0 = released                       */
    int   (*read)   (void *priv, uint64_t buf, uint64_t n);
    int   (*write)  (void *priv, const void *buf, uint64_t n);
    int   (*ioctl)  (void *priv, uint64_t req, void *arg);
} void_device_ops_t;

/* A registered device object.  `name` is string equality; the registry is
 * small and lookup is a linear scan, so a fixed set of the addresses the
 * drivers own is the simplest thing that works. */
typedef struct void_device {
    const char      *name;
    void            *data;       /* opaque driver state                    */
    void_device_ops_t ops;       /* ops driver implements (open required)  */
    dev_type_t       type;       /* probe identifier                       */
    uint16_t         magic;      /* slot seed, set by dev_init             */
    uint8_t          ref;        /* live open reference count              */
    bool             inuse;      /* slot occupied                          */
} void_device_t;

/* ── limits ───────────────────────────────────────────────────────────── */
#define DEV_MAX_DEVICES   32    /* static registry slots                   */
#define DEV_NAME_MAX      63    /* longest device name (incl. NUL)         */

/* The registry is a static array of device slots; entries are zeroed when a
 * device unregisters, so a stale address is never reachable. */
typedef struct void_device_registry {
    void_device_t devices[DEV_MAX_DEVICES];
} void_device_registry_t;

/* ── registry + instance API ────────────────────────────────────────────
 *
 * init      — seed the registry.  Call once at boot, before any register.
 * register  — add a device under its name; fails -VE_EXIST on a name
 *             collision.  Requires an `open` op (unopenable devices are no
 *             use to this layer).
 * unregister— remove a device by name.  Refuses if it still has open
 *             references (-VE_BUSY): the spec's "unregistering with active
 *             users handled safely" means the refcount is dropped first at
 *             close, and an open handle is never invalidated under its
 *             owner — a device is removed only when the last user has
 *             released it.
 * lookup    — find a device by name.  Returns NULL if absent.
 *
 * open      — find + validate a device (liveness, sanity) and mint one
 *             instance: a heap wrapper holding the driver's open handle and
 *             the registry slot.  Returns the wrapper, or NULL.  This is
 *             the seam a future VFS backend calls, exactly as FD_VFS stores
 *             a vfs_file_t*.
 * close     — release an instance: driver close, then drop the table ref.
 * read/write/ioctl — dispatch through the instance to the device's ops.
 *             Callers already hold the instance; it is shape-validated
 *             against the live table before any ops pointer is called.
 */
void   dev_init(void);
int    dev_register(void_device_t *dev);
int    dev_unregister(const char *name);
void_device_t *dev_lookup(const char *name);

void  *dev_open(const char *name);
int    dev_close(void *inst);
int    dev_read(void *inst, uint64_t buf, uint64_t n);
int    dev_write(void *inst, const void *buf, uint64_t n);
int    dev_ioctl(void *inst, uint64_t req, void *arg);

/* Magic stamped into every instance wrapper, so a forged/corrupt handle is
 * rejected with -VE_BADF before any ops table is dereferenced. */
#define DEV_INSTANCE_MAGIC 0x0871

/* Probe the registry (Section 5 device↔VFS boundary prep): count devices
 * whose type intersects `types`.  A future /dev vnode backend calls this to
 * enumerate what the registry currently holds. */
uint32_t dev_probe(dev_type_t types);

/* ── selftest ─────────────────────────────────────────────────────────── */
void dev_self_test(void);   /* prints [DEV] PASS/FAIL lines; add boot call */

#endif /* VOID_DEVICE_H */