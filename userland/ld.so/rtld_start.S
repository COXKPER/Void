/* VoidOS — dynamic linker entry stub
 *
 * Unlike crt0.S this cannot call main() and forget the stack: the auxiliary
 * vector *is* the rtld's only input, and it lives at RSP.  So the stub hands
 * the untouched RSP to rtld_main() as its first argument, and jumps to the
 * relocated program entry rtld_main returns — with RSP restored to the value
 * the kernel set, because the program's own crt0 expects the System V frame
 * exactly where the kernel put it.
 */
.section .text
.global _rtld_start
.type _rtld_start, @function

_rtld_start:
    mov  %rsp, %rbx         /* callee-saved: survives rtld_main            */
    mov  %rsp, %rdi         /* arg0 = &argc (the initial frame)            */
    sub  $8, %rsp           /* ABI: callee sees RSP%16==0 after its push   */
    call rtld_main
    mov  %rbx, %rsp         /* hand the program the kernel's exact frame   */
    jmp  *%rax              /* rtld_main returned the real entry point     */

.size _rtld_start, . - _rtld_start
