/* VoidOS — user executable link script (Phase 5A)
 *
 * Static, non-PIE, loaded at a fixed 4 MiB base.  Each output section gets
 * its own PT_LOAD with distinct permissions so the loader's per-segment
 * permission handling is actually exercised:
 *
 *   .text    R+X   executable code
 *   .rodata  R     read-only — a write here must fault
 *   .data    R+W   initialised, writable
 *   .bss     R+W   p_memsz > p_filesz, so the loader must zero the tail
 *
 * .bss deliberately shares the data segment: that is what makes p_filesz
 * smaller than p_memsz and turns BSS initialisation into a real test.
 */
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5);   /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R+W */
}

SECTIONS {
    . = 0x400000;

    .text : { *(.text*) } :text

    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) } :rodata

    . = ALIGN(0x1000);
    .data : { *(.data*) } :data
    .bss  : { *(.bss*) *(COMMON) } :data

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
