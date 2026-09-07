/* VoidOS — ACPI API
 *
 * Minimal ACPI: RSDP → XSDT/RSDT → table lookup, plus an MADT entry walk.
 * Healthy, cleanly-disabled on non-ACPI platforms (acpi_init returns -1 when
 * the Limine RSDP is absent or invalid — never a panic).
 *
 * The LAPIC driver (Phase 3) is the *only* consumer of the APIC description;
 * this module exposes it, it does not implement a second APIC subsystem.
 */
#ifndef VOID_ACPI_H
#define VOID_ACPI_H 1

#include <void/types.h>

/* acpi_init — validate the RSDP + table list, build a registry.
 * Returns 0 on success, -1 when there is no RSDP or a fatal checksum
 * failure.  Call once at boot, after pmm/vmm (HHDM is required). */
int acpi_init(void);

/* acpi_get_rsdp — physical address of the Root System Description Pointer,
 * or 0 if ACPI is disabled. */
uint64_t acpi_get_rsdp(void);

/* acpi_find_table — the table whose 4-byte signature matches `sig`, at
 * zero-based `index` (0 = first).  Returns a validated table header, or
 * NULL.  Only valid after a successful acpi_init. */
void *acpi_find_table(const char *sig, int index);

/* MADT entry types (as handed to the callback) */
#define ACPI_MADT_LAPIC        0   /* processor local APIC    */
#define ACPI_MADT_IOAPIC       1   /* IO APIC                 */
#define ACPI_MADT_INT_OVERRIDE 2   /* interrupt source override */

typedef struct {
    uint8_t       type;      /* ACPI_MADT_*                        */
    uint8_t       length;    /* entry byte length                   */
    const void   *data;      /* the raw entry (struct per type)     */
} acpi_madt_entry_t;

/* Callback for acpi_get_madt.  Receives LAPIC/IOAPIC/override entries. */
typedef void (*acpi_madt_cb_t)(const acpi_madt_entry_t *e, void *arg);

/* acpi_get_madt — iterate the MADT's fixed-size entries.
 * `cb` is called per LAPIC/IOAPIC/override entry (may be NULL to just
 * count).  `override_count`, when non-NULL, receives the number of
 * interrupt-source-override entries.  Returns 0 when the MADT exists and
 * was walked, -1 when there is none. */
int acpi_get_madt(acpi_madt_cb_t cb, void *arg, int *override_count);

/* acpi_selftest — deterministic [ACPI] PASS/FAIL lines, run after a
 * successful acpi_init (before the scheduler).  Asserts the parser self-
 * consistency: signature lookup round-trips, MADT walk counts, and the
 * missing-table path fails cleanly. */
void acpi_selftest(void);

#endif /* VOID_ACPI_H */