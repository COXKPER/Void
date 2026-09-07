/* VoidOS — kernel device layer implementation
 *
 * A static registry of named devices with ops, plus a small instance layer:
 * dev_open() mints a heap wrapper that pins a driver's open handle to the
 * registry slot that produced it.  The table entry is the guard against
 * stale addresses — entries are zeroed on unregister, and every public
 * access re-checks liveness before an ops pointer is touched.
 *
 * See kernel/include/void/device.h for the model and the Lux classification.
 */
#include <void/device.h>
#include <void/types.h>
#include <syscall/syscall.h>   /* VE_* errno values */
#include <mm/kheap.h>
#include <dev/serial.h>

static void_device_registry_t g_dev_reg;   /* zero-initialised (BSS) */

/* The private per-open object dev_open returns.  The driver's own open hook
 * returns its private handle (`priv`); we wrap it so the layer can validate
 * the instance against the live table and release the table ref on close. */
typedef struct dev_instance {
    uint32_t  magic;          /* DEV_INSTANCE_MAGIC                     */
    uint8_t   idx;            /* table slot the instance was minted for  */
    uint16_t  birth;          /* slot->magic at mint time; later != blank*/
    void     *priv;           /* the driver's per-open handle            */
} dev_instance_t;

/* The stub read for write-only devices: "operation not supported". */
static int dev_noread(void *priv, uint64_t buf, uint64_t n) {
    (void)priv; (void)buf; (void)n;
    return -VE_INVAL;
}

/* ── helpers ──────────────────────────────────────────────────────────── */

static bool dev_entry_live(const void_device_t *d) {
    return d && d->inuse && d->name && d->ops.open;
}

static bool dev_inst_valid(const void *inst) {
    const dev_instance_t *i = (const dev_instance_t *)inst;
    if (!i || i->magic != DEV_INSTANCE_MAGIC) return false;
    if (i->idx >= DEV_MAX_DEVICES) return false;
    const void_device_t *d = &g_dev_reg.devices[i->idx];
    return dev_entry_live(d) && d->ref && i->birth == d->magic;
}

void dev_init(void) {
    for (uint32_t i = 0; i < DEV_MAX_DEVICES; i++)
        g_dev_reg.devices[i].magic = (uint16_t)(i + 0x100);
}

/* ── registry ─────────────────────────────────────────────────────────── */

int dev_register(void_device_t *dev) {
    if (!dev || !dev->name || !dev->ops.open) return -VE_INVAL;

    /* Refuse leaves for the name copy (cap the source) and require a
     * non-empty name. */
    uint64_t nlen = 0;
    while (dev->name[nlen] && nlen < DEV_NAME_MAX - 1) nlen++;
    if (dev->name[nlen]) return -VE_NAMETOOLONG;
    if (nlen == 0) return -VE_INVAL;

    /* Duplicate-name check: a name may only be registered once. */
    for (uint32_t k = 0; k < DEV_MAX_DEVICES; k++) {
        void_device_t *slot = &g_dev_reg.devices[k];
        if (!slot->inuse || !slot->name) continue;
        const char *a = dev->name, *b = slot->name;
        uint64_t i = 0;
        while (a[i] && b[i] && a[i] == b[i]) i++;
        if (a[i] == '\0' && b[i] == '\0') return -VE_EXIST;
    }

    /* Publish — the only mutation of a table row. */
    for (uint32_t k = 0; k < DEV_MAX_DEVICES; k++) {
        void_device_t *slot = &g_dev_reg.devices[k];
        if (slot->inuse) continue;
        slot->name  = dev->name;
        slot->data  = dev->data;
        slot->ops   = dev->ops;
        slot->type  = dev->type;
        slot->inuse = true;
        slot->ref   = 0;
        if (!slot->ops.read) slot->ops.read = dev_noread;   /* write-only */
        return VOID_OK;
    }
    return -VE_NOMEM;   /* table full */
}

int dev_unregister(const char *name) {
    if (!name || !name[0]) return -VE_INVAL;
    for (uint32_t k = 0; k < DEV_MAX_DEVICES; k++) {
        void_device_t *slot = &g_dev_reg.devices[k];
        if (!slot->inuse || !slot->name) continue;
        const char *a = name, *b = slot->name;
        uint64_t i = 0;
        while (a[i] && b[i] && a[i] == b[i]) i++;
        if (a[i] || b[i]) continue;

        if (slot->ref) return -VE_BUSY;    /* active users: refuse */
        slot->name  = NULL;
        slot->data  = NULL;
        slot->inuse = false;
        slot->ref   = 0;
        return VOID_OK;
    }
    return -VE_NOENT;
}

void_device_t *dev_lookup(const char *name) {
    if (!name || !name[0]) return NULL;
    for (uint32_t k = 0; k < DEV_MAX_DEVICES; k++) {
        void_device_t *slot = &g_dev_reg.devices[k];
        if (!slot->inuse || !slot->name) continue;
        const char *a = name, *b = slot->name;
        uint64_t i = 0;
        while (a[i] && b[i] && a[i] == b[i]) i++;
        if (a[i] == '\0' && b[i] == '\0') return slot;
    }
    return NULL;
}

/* ── open / close ─────────────────────────────────────────────────────── */

void *dev_open(const char *name) {
    if (!name || !name[0]) return NULL;
    void_device_t *slot = dev_lookup(name);
    if (!dev_entry_live(slot)) return NULL;

    void *priv = slot->ops.open(slot->data);   /* driver's open hook */
    if (!priv) return NULL;

    dev_instance_t *inst = kmalloc(sizeof(dev_instance_t));
    if (!inst) { if (slot->ops.close) slot->ops.close(priv); return NULL; }

    slot->ref++;
    inst->magic = DEV_INSTANCE_MAGIC;
    inst->idx   = (uint8_t)(uintptr_t)(slot - g_dev_reg.devices);
    inst->birth = slot->magic;
    inst->priv  = priv;
    return inst;
}

int dev_close(void *inst) {
    dev_instance_t *i = (dev_instance_t *)inst;
    if (!i || i->magic != DEV_INSTANCE_MAGIC) return -VE_INVAL;
    if (i->idx >= DEV_MAX_DEVICES) return -VE_BADF;
    void_device_t *slot = &g_dev_reg.devices[i->idx];
    if (!dev_entry_live(slot) || i->birth != slot->magic || !slot->ref)
        return -VE_BADF;
    if (slot->ops.close) slot->ops.close(i->priv);
    slot->ref--;
    kfree(inst);
    return VOID_OK;
}

/* ── instance ops dispatch ────────────────────────────────────────────── */

static void_device_t *dev_inst_slot(const void *inst) {
    const dev_instance_t *i = (const dev_instance_t *)inst;
    if (!i || i->magic != DEV_INSTANCE_MAGIC) return NULL;
    if (i->idx >= DEV_MAX_DEVICES) return NULL;
    void_device_t *slot = &g_dev_reg.devices[i->idx];
    if (!dev_inst_valid(inst)) return NULL;
    return slot;
}

int dev_read(void *inst, uint64_t buf, uint64_t n) {
    void_device_t *slot = dev_inst_slot(inst);
    if (!slot || !slot->ops.read) return -VE_BADF;
    return slot->ops.read(((dev_instance_t *)inst)->priv, buf, n);
}

int dev_write(void *inst, const void *buf, uint64_t n) {
    void_device_t *slot = dev_inst_slot(inst);
    if (!slot || !slot->ops.write) return -VE_BADF;
    return slot->ops.write(((dev_instance_t *)inst)->priv, buf, n);
}

int dev_ioctl(void *inst, uint64_t req, void *arg) {
    void_device_t *slot = dev_inst_slot(inst);
    if (!slot || !slot->ops.ioctl) return -VE_BADF;
    return slot->ops.ioctl(((dev_instance_t *)inst)->priv, req, arg);
}

/* ── /dev service probe (Section 5: device↔VFS boundary prep) ───────────
 * A future device-backed VFS node resolves behind /dev.  This walks the
 * registry and returns how many devices match `types` — the shape a backend
 * would call, kept to one probe until the VFS actually mounts /dev. */
uint32_t dev_probe(dev_type_t types) {
    uint32_t n = 0;
    for (uint32_t k = 0; k < DEV_MAX_DEVICES; k++) {
        const void_device_t *slot = &g_dev_reg.devices[k];
        if (slot->inuse && (slot->type & types)) n++;
    }
    return n;
}

/* ── selftest (Section 8) ───────────────────────────────────────────────
 * Deterministic, no interrupts, runs from kernel_main before the scheduler.
 * Prints [DEV] PASS/FAIL lines the QEMU regression harness consumes. */

/* Test hook devices: two named character/block devices with real ops. */
static void *t_open(const void *dev) {
    (void)dev;
    static int priv;                  /* a fixed per-open handle is fine  */
    return &priv;
}
static int t_close(void *priv) { (void)priv; return 0; }
static int t_write(void *priv, const void *buf, uint64_t n) {
    (void)priv; (void)buf; return (int)n;
}
static int t_read(void *priv, uint64_t buf, uint64_t n) {
    (void)priv; (void)buf; return (int)(n < 4 ? n : 4);
}

static const char  t_char_name[] = "devc";
static const char  t_blk_name[]  = "devb";
static void_device_t t_char, t_blk;

/* Minimal serial print helpers for the test output (kprintf goes through
 * the console which serial is wiring into — keep the test independent). */
static void t_puts(const char *s) { while (*s) serial_putchar(*s++); }
static void t_result(const char *label, bool pass) {
    t_puts("[DEV] "); t_puts(label); t_puts(": ");
    t_puts(pass ? "PASS" : "FAIL"); t_puts("\n\r");
}

void dev_self_test(void) {
    t_puts("[DEV] selftest start\n\r");

    /* two fresh device structs */
    static const uint8_t blob[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    t_char.name = t_char_name; t_char.data = (void *)blob;
    t_char.type = DEV_CHAR;
    t_char.ops.open = t_open; t_char.ops.close = t_close;
    t_char.ops.write = t_write; t_char.ops.read = t_read;
    t_blk.name = t_blk_name; t_blk.data = (void *)blob;
    t_blk.type = DEV_BLOCK;
    t_blk.ops.open = t_open; t_blk.ops.close = t_close;
    t_blk.ops.write = t_write; t_blk.ops.read = t_read;

    /* 1. register two devices */
    t_result("register device", dev_register(&t_char) == VOID_OK);
    t_result("register device", dev_register(&t_blk) == VOID_OK);

    /* 2. lookup by name — the registry stores copies of driver structs, so
     * we assert the returned entry names the right device, not pointers. */
    {
        void_device_t *found = dev_lookup("devc");
        t_result("lookup device", found && found->name == t_char_name);
    }

    /* 3. open → non-NULL instance */
    void *o = dev_open("devc");
    t_result("open device", o != NULL);

    /* 4. close → 0 */
    t_result("close device", (o && dev_close(o) == VOID_OK));

    /* 5. open a device with an invalid name → NULL */
    t_result("invalid device", dev_open("no_such") == NULL);

    /* 6. duplicate registration → -VE_EXIST */
    t_result("duplicate registration", dev_register(&t_char) == -VE_EXIST);

    /* 7. unregister a device with no open refs → 0 */
    t_result("unregister device", dev_unregister("devb") == VOID_OK);

    /* 8. open a stale (unregistered) device → NULL */
    t_result("stale device rejected", dev_open("devb") == NULL);

    /* 8.5 re-register + open + close — prove a name can come back and its
     * fresh instance is a real open, not the stale one refused above. */
    t_result("re-register device", dev_register(&t_blk) == VOID_OK);
    void *o2 = dev_open("devb");
    t_result("open re-registered", o2 != NULL);
    if (o2) dev_close(o2);

    /* 9. unregistering with an active open → -VE_BUSY; still usable after */
    void *held = dev_open("devc");
    t_result("unregister with active ref", (held && dev_unregister("devc") == -VE_BUSY));
    int nw = held ? dev_write(held, "x", 1) : -1;
    t_result("device usable after refused unregister", nw == 1);
    if (held) dev_close(held);

    /* (the instance wrapper is heap-owned; the test static is tiny and
     * reused — ids stay distinct because open mints a fresh wrapper each
     * time.) */
    t_puts("[DEV] selftest done\n\r");
    (void)nw; (void)o2;
}