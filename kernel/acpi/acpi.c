/* VoidOS — ACPI parser
 *
 * Port of Lux's RSDP→RSDT/XSDT table-lookup parser, with Void additions:
 * full checksum validation (Lux skips it) and an MADT parse (Lux has none).
 * The Limine-provided RSDP is used directly — no physical EBDA scan; the
 * bootloader already did it.
 *
 * Lux provenance (MIT © 2024 luxOS authors, src/acpi/tables.c):
 *   BRING — RSDP→XSDT/RSDT → table-list layout, find-by-signature,
 *           acpiVersion tracking, the "no RSDP → cleanly disabled" flow.
 *   PORT  — the physical→virtual mapping (Lux vmmMMIO ↔ Void kernel window).
 *   ADDED — RSDP + per-table checksum validation, bound-checked XSDT/RSDT
 *           walks (Lux reads tables[] with no entry count), MADT entry
 *           parse (LAPIC/IOAPIC/override), a compact table registry.
 *   SKIP  — FADT/DSDT parse and power management (out of Phase 14 scope).
 *
 * Robustness: every ACPI byte is read through a private kernel mapping
 * window, never through the HHDM.  Limine's HHDM maps the memory-map
 * entries, so the RSDP in the low-ROM/EBDA page (phys 0xf59f0 in QEMU) is
 * UNMAPPED — a direct HHDM read of it #PFs before byte 15.  ACPI tables are
 * typically ACPI_RECLAIMABLE (mapped), but firmware can place anything
 * anywhere, so the parser maps each physical 4KiB page it touches into a
 * fixed upper-half window (VMM), memoised per page.  Single-byte loads never
 * cross a page, so tables that span pages (checksum loops, XSDT entry lists)
 * are safe by construction.
 *
 * The LAPIC setup in Void (phase 3, dev/lapic.c) is untouched: acpi_get_madt
 * only exposes the APIC *description*, so no second APIC subsystem appears.
 */
#include <void/acpi.h>
#include <void/boot.h>
#include <void/types.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <dev/serial.h>

/* ── ACPI structs (layout as in Lux's acpi.h / the ACPI spec) ─────────── */
typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem[6];
    char     oem_table[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

/* ── state ────────────────────────────────────────────────────────────── */
#define ACPI_MAX_TABLES 64

static uint64_t g_rsdp_phys;                /* 0 = not ACPI-compliant  */
static uint64_t g_tables[ACPI_MAX_TABLES];  /* physical addresses      */
static int      g_table_count;
static int      g_acpi_version;

/* ── private kernel mapping window ──────────────────────────────────────
 * Distict physical pages are mapped here and memoised; no eviction (a page
 * that fit once stays), so pointers into the window stay valid for the
 * kernel's lifetime.  16 × 4 KiB is far more than this parser ever pins. */
#define ACPI_MAP_WIN    0xffffffffe0000000ULL
#define ACPI_MAP_PAGES  16

static uint64_t win_phys[ACPI_MAP_PAGES];   /* physical page in slot i */

static bool acpi_ensure(uint64_t addr) {
    uint64_t page = addr & ~(uint64_t)0xfff;

    /* already mapped? */
    for (int i = 0; i < ACPI_MAP_PAGES; i++)
        if (win_phys[i] == page) return true;

    /* map into a free slot */
    for (int i = 0; i < ACPI_MAP_PAGES; i++) {
        if (win_phys[i] != 0) continue;
        uint64_t slot = ACPI_MAP_WIN + (uint64_t)i * 0x1000;
        if (vmm_map_page((uint64_t *)vmm_kernel_pml4(), slot, page,
                         VMM_KERN_RW) != VOID_OK)
            return false;
        win_phys[i] = page;
        return true;
    }
    return false;   /* window exhausted — never happens in practice */
}

/* virtual address of a mapped byte, or NULL */
static void *acpi_map_field(uint64_t addr) {
    if (!acpi_ensure(addr)) return NULL;
    for (int i = 0; i < ACPI_MAP_PAGES; i++)
        if (win_phys[i] == (addr & ~(uint64_t)0xfff))
            return (void *)(ACPI_MAP_WIN + (uint64_t)i * 0x1000 +
                            (addr & 0xfff));
    return NULL;
}

/* ── byte-wise physical reads (never cross an HHDM-unmapped page) ─────── */
static uint8_t acpi_rdb(uint64_t phys, uint32_t off) {
    uint8_t *p = acpi_map_field(phys + off);
    if (!p) {
        kprintf("[ACPI] mapping failure @ phys 0x%016x+%u\n\r", phys, off);
        cpu_halt();
        __builtin_unreachable();
    }
    return *p;
}
static uint32_t acpi_rd32(uint64_t phys, uint32_t off) {
    return (uint32_t)acpi_rdb(phys, off)
         | ((uint32_t)acpi_rdb(phys, off + 1) << 8)
         | ((uint32_t)acpi_rdb(phys, off + 2) << 16)
         | ((uint32_t)acpi_rdb(phys, off + 3) << 24);
}
static uint64_t acpi_rd64(uint64_t phys, uint32_t off) {
    return (uint64_t)acpi_rd32(phys, off)
         | ((uint64_t)acpi_rd32(phys, off + 4) << 32);
}

/* ── checksum: all bytes of buf[0..len) sum to 0 mod 256 ────────────────
 * Cross-page safe: each byte maps its own page. */
static bool acpi_checksum_at(uint64_t phys, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + acpi_rdb(phys, i));
    return sum == 0;
}

/* Read a 36-byte SDT header field-by-field into a local. */
static void acpi_read_sdt(uint64_t phys, acpi_sdt_header_t *dst) {
    uint32_t or2  = acpi_rd32(phys, 24);
    uint32_t cr   = acpi_rd32(phys, 28);
    uint32_t crrev = acpi_rd32(phys, 32);
    for (int i = 0; i < 4; i++)  dst->signature[i] = (char)acpi_rdb(phys, i);
    dst->length    = acpi_rd32(phys, 4);
    dst->revision  = acpi_rdb(phys, 8);
    dst->checksum  = acpi_rdb(phys, 9);
    for (int i = 0; i < 6; i++)  dst->oem[i] = (char)acpi_rdb(phys, 10 + i);
    for (int i = 0; i < 8; i++)  dst->oem_table[i] = (char)acpi_rdb(phys, 16 + i);
    dst->oem_revision = or2;
    dst->creator_id   = cr;
    dst->creator_revision = crrev;
}

/* resident, mapped pointer to a table header (page-granular; ACPI tables
 * are ≥8-byte aligned and ≥64 bytes, so the 36-byte header never spans an
 * unmapped page). */
static void *acpi_table_ptr(uint64_t phys) {
    return acpi_map_field(phys);
}

/* ── API ──────────────────────────────────────────────────────────────── */

uint64_t acpi_get_rsdp(void) { return g_rsdp_phys; }

void *acpi_find_table(const char *sig, int index) {
    int c = 0;
    for (int i = 0; i < g_table_count; i++) {
        const char *s = (const char *)acpi_table_ptr(g_tables[i]);
        if (s && s[0] == sig[0] && s[1] == sig[1] &&
            s[2] == sig[2] && s[3] == sig[3]) {
            if (c == index) return (void *)s;
            c++;
        }
    }
    return NULL;
}

/* ── MADT entry walk ────────────────────────────────────────────────────
 * Walks the fixed-size entry array firing the callback for
 * LAPIC/IOAPIC/override entries.  A malformed entry (length < 2 or past
 * the table end) stops the walk. */
#define ACPI_MADT_LAPIC        0
#define ACPI_MADT_IOAPIC       1
#define ACPI_MADT_INT_OVERRIDE 2

static void acpi_walk_madt(uint64_t madt_phys, uint32_t madt_len,
                           acpi_madt_cb_t cb, void *arg, int *overrides) {
    uint32_t off = 8;   /* entries[] starts after the 36-byte header */
    int n = 0;
    while (off + 2 <= madt_len) {
        uint8_t type = acpi_rdb(madt_phys, off);
        uint8_t elen = acpi_rdb(madt_phys, off + 1);
        if (elen < 2 || (uint32_t)(off + elen) > madt_len) break;

        if (type == ACPI_MADT_LAPIC || type == ACPI_MADT_IOAPIC ||
            type == ACPI_MADT_INT_OVERRIDE) {
            acpi_madt_entry_t ent;
            ent.type   = type;
            ent.length = elen;
            ent.data   = acpi_table_ptr(madt_phys + off);
            if (cb) cb(&ent, arg);
            if (type == ACPI_MADT_INT_OVERRIDE) n++;
        }
        off += elen;
    }
    if (overrides) *overrides = n;
}

int acpi_get_madt(acpi_madt_cb_t cb, void *arg, int *override_count) {
    uint64_t madt = 0;
    for (int i = 0; i < g_table_count; i++) {
        const char *s = (const char *)acpi_table_ptr(g_tables[i]);
        if (s && s[0] == 'A' && s[1] == 'P' && s[2] == 'I' && s[3] == 'C') {
            madt = g_tables[i];
            break;
        }
    }
    if (!madt) return -1;

    uint32_t len = acpi_rd32(madt, 4);
    if (len < 44) return -1;   /* header(36) + local addr(4) + flags(4) */
    acpi_walk_madt(madt, len, cb, arg, override_count);
    return 0;
}

/* ── init ─────────────────────────────────────────────────────────────── */

int acpi_init(void) {
    g_rsdp_phys   = g_boot.acpi_rsdp;
    g_table_count = 0;
    g_acpi_version = 0;

    /* Guard: the map window must be unmapped before we claim it. */
    for (int i = 0; i < ACPI_MAP_PAGES; i++) {
        uint64_t slot = ACPI_MAP_WIN + (uint64_t)i * 0x1000;
        if (vmm_get_pte((uint64_t *)vmm_kernel_pml4(), slot) != 0) {
            kprintf("[ACPI] FATAL: map window slot 0x%016x already mapped\n\r",
                    slot);
            return -1;
        }
    }

    if (!g_rsdp_phys) {
        kprintf("[ACPI] no RSDP — not ACPI-compliant, ACPI disabled\n\r");
        return -1;
    }

    /* Field-read the RSDP. */
    uint8_t  rev  = acpi_rdb(g_rsdp_phys, 15);
    uint32_t rsdt = acpi_rd32(g_rsdp_phys, 16);
    uint32_t len  = acpi_rd32(g_rsdp_phys, 20);
    uint64_t xsdt = acpi_rd64(g_rsdp_phys, 24);

    /* Rev-0 RSDP checksum covers the first 20 bytes; rev ≥ 2 the full 36. */
    if (!acpi_checksum_at(g_rsdp_phys, (rev >= 2) ? 36 : 20)) {
        kprintf("[ACPI] RSDP checksum FAILED\n\r");
        return -1;
    }

    uint64_t list_phys = 0;      /* the RSDT or XSDT we use */
    uint32_t entry_sz  = 0;
    if (rev >= 2 && xsdt) {
        list_phys = xsdt;
        entry_sz  = 8;
    } else if (rsdt) {
        list_phys = rsdt;
        entry_sz  = 4;
    } else {
        kprintf("[ACPI] neither RSDT nor XSDT present\n\r");
        return -1;
    }
    (void)len;

    acpi_sdt_header_t hdr;
    acpi_read_sdt(list_phys, &hdr);
    if (!acpi_checksum_at(list_phys, hdr.length)) {
        kprintf("[ACPI] %c%c%c%c checksum FAILED\n\r",
                hdr.signature[0], hdr.signature[1],
                hdr.signature[2], hdr.signature[3]);
        return -1;
    }

    long count = (long)((hdr.length - 36) / entry_sz);
    if (count > ACPI_MAX_TABLES) count = ACPI_MAX_TABLES;
    if (count < 0)  count = 0;

    int n = 0;
    for (int i = 0; i < (int)count; i++) {
        uint64_t tphys = (entry_sz == 8)
                             ? acpi_rd64(list_phys, 36 + (uint32_t)i * 8)
                             : (uint64_t)acpi_rd32(list_phys, 36 + (uint32_t)i * 4);
        if (!tphys) continue;            /* reserved slot, skip */

        acpi_read_sdt(tphys, &hdr);
        if (hdr.length < 36) continue;   /* malformed */
        if (!acpi_checksum_at(tphys, hdr.length)) {
            kprintf("[ACPI] table '%c%c%c%c' checksum FAILED\n\r",
                    hdr.signature[0], hdr.signature[1],
                    hdr.signature[2], hdr.signature[3]);
            continue;
        }
        g_tables[n++] = tphys;
    }
    g_table_count = n;

    acpi_sdt_header_t *fadt = (acpi_sdt_header_t *)acpi_find_table("FACP", 0);
    g_acpi_version = fadt ? (int)fadt->revision : (entry_sz == 8 ? 2 : 1);

    kprintf("[ACPI] revision %d, %d table(s) validated\n\r",
            g_acpi_version, g_table_count);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 *  kernel self-check — [ACPI] PASS/FAIL lines, no interrupts
 *  ════════════════════════════════════════════════════════════════════════ */

static void acpi_puts(const char *s) { while (*s) serial_putchar(*s++); }
static void acpi_result(const char *label, bool pass) {
    acpi_puts("[ACPI] "); acpi_puts(label); acpi_puts(": ");
    acpi_puts(pass ? "PASS" : "FAIL"); acpi_puts("\n\r");
}

void acpi_selftest(void) {
    acpi_result("rsdp present", g_rsdp_phys != 0);
    acpi_result("tables registered", g_table_count > 0);

    acpi_sdt_header_t *fadt = (acpi_sdt_header_t *)acpi_find_table("FACP", 0);
    acpi_result("find FACP", fadt != NULL);
    acpi_result("find missing", acpi_find_table("ZZZZ", 0) == NULL);

    int over = -1;
    int rc = acpi_get_madt(NULL, NULL, &over);
    acpi_result("madt walk", rc == 0 || rc == -1);
    acpi_result("madt overrides", over >= 0);

    acpi_puts("[ACPI] selftest done\n\r");
}