/* VoidOS — PT_INTERP marker for the dynamic test
 *
 * A `-shared` object normally has no PT_INTERP (that's for ET_DYN
 * executables), but Void's kernel keys the tlrt handover off a PT_INTERP
 * phdr.  This tiny object contributes a `.interp` section holding the rtld's
 * path, so linking it into dynamic_test.so produces the PT_INTERP the kernel
 * looks for.  The path is a lie in the best sense: nothing is opened from a
 * filesystem, the kernel maps the basename (`ld-void.so`) to the embedded
 * blob it already carries.
 */
__attribute__((section(".interp"), used))
static const char void_ld_path[] = "/lib/ld-void.so";
