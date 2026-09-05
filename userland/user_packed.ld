/* VoidOS — packed user link script (Phase 5A shared-page test)
 *
 * Like user.ld, but .text and .rodata deliberately abut *inside the same
 * 4 KiB page* so one physical frame backs two PT_LOAD segments with
 * different permissions.  Loading this exercises the loader's permission-
 * union path rather than its fresh-page path.
 *
 * The union of R+X and R is R+X, which is legal.  .data stays page-aligned
 * on purpose: letting a writable segment share the code page would union to
 * W+X, and the loader refuses such binaries outright.  That rejection is
 * covered separately by the synthetic negative tests.
 */
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5);   /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R+W */
}

SECTIONS {
    . = 0x400000;

    .text   : { *(.text*) }          :text
    .rodata : { *(.rodata*) }        :rodata

    . = ALIGN(0x1000);
    .data   : { *(.data*) }          :data
    .bss    : { *(.bss*) *(COMMON) } :data

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
