/* libvoid — minimal crt0 entry point
 *
 * Entry point for ELF executables. Kernel maps PT_LOAD segments, sets
 * RIP=e_entry (which lands here), RSP at user stack top.
 *
 * Stack alignment (x86_64 ABI):
 *   - RSP mod 16 must equal 8 at the start of a function
 *   - CALL pushes 8 bytes, so the function sees RSP mod 16 == 0
 *   - We are the entry point, not called, so RSP mod 16 == 0 initially
 *   - main() expects to be CALLed (RSP mod 16 == 8), so we adjust
 */
.section .text
.global _start
.type _start, @function

_start:
    /* RSP mod 16 == 0 at entry. Adjust so main() sees RSP mod 16 == 8. */
    sub $8, %rsp

    /* Call main(void) */
    call main

    /* If main returns, exit with its return value (in EAX).
     * SYS_exit = 60; RAX holds syscall number, RDI holds status. */
    mov %eax, %edi          /* status in RDI */
    mov $60, %rax           /* SYS_exit */
    syscall

    /* Never reached. */
    hlt

.size _start, . - _start
