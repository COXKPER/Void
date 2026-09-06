; VoidOS — embedded user ELF images
;
; The Phase 5A loader reads from a kernel-resident buffer, so the test
; executables are linked separately (userland/*.ld) and pasted into .rodata
; here.  Once the VFS lands these come off a filesystem instead and this file
; goes away.
;
; Paths are relative to the project root because that is nasm's cwd in the
; Makefile.  Both blobs are page-aligned so their embedded program headers
; keep the file-offset congruence the loader checks.

bits 64

section .rodata

align 4096
global elf_test_start
global elf_test_end
elf_test_start:
    incbin "build/userland/elf_test.elf"
elf_test_end:

align 4096
global elf_packed_start
global elf_packed_end
elf_packed_start:
    incbin "build/userland/elf_packed.elf"
elf_packed_end:

align 4096
global elf_init_start
global elf_init_end
elf_init_start:
    incbin "build/userland/init.elf"
elf_init_end:

align 4096
global elf_ipc_test_start
global elf_ipc_test_end
elf_ipc_test_start:
    incbin "build/userland/ipc_test.elf"
elf_ipc_test_end:

align 4096
global elf_forkexec_test_start
global elf_forkexec_test_end
elf_forkexec_test_start:
    incbin "build/userland/forkexec_test.elf"
elf_forkexec_test_end:
