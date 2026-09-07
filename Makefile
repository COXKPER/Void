# VoidOS build — gcc host cross-compiled as freestanding x86_64, GNU ld,
# nasm for entry assembly, Limine as the boot protocol, xorriso for the ISO.
# skipped: cross-compiler build (x86_64-elf-gcc); add if host gcc ever stops
# defaulting to x86_64 (e.g. porting the build to an ARM host).

override KERNEL   := build/voidos.elf
override ISO      := build/voidos.iso
override CC       := gcc
override LD       := ld
override ASM      := nasm

override CFLAGS := -std=c2x -Wall -Wextra -O2 -g \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-pic -fno-pie \
	-mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
	-m64 -march=x86-64 \
	-Ikernel/include

override LDFLAGS := -nostdlib -z max-page-size=0x1000 \
	-T kernel/linker.ld

override ASMFLAGS := -f elf64 -g

# ── userland ELF test executables ────────────────────────────────────────
# Freestanding static non-PIE binaries, linked at a fixed base and embedded
# into the kernel image by kernel/elf/elf_blobs.asm.  max-page-size=0x1000
# keeps p_align at 4 KiB; the default 2 MiB would pad the file enormously.
override UCFLAGS := -std=c2x -Wall -Wextra -O2 \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-pic -fno-pie \
	-mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
	-m64 -march=x86-64 -Iuserland/include

override ULDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack

UELF := build/userland/elf_test.elf build/userland/elf_packed.elf build/userland/init.elf build/userland/ipc_test.elf build/userland/forkexec_test.elf build/userland/calc.elf build/userland/srv_test.elf build/userland/lifecycle_test.elf build/userland/vfs_test.elf build/userland/mm_test.elf build/userland/malloc_test.elf build/userland/tty_test.elf build/userland/ld_so.elf build/userland/dynamic_test.elf

# libc runtime objects (freestanding userland helpers); linked into the
# tests that need them.  Compiled with the same UCFLAGS as the tests.
LIBC_OBJ := build/userland/libc/mem.o build/userland/libc/malloc.o

# Position-independent libc + test for the dynamic-linker regression: UCFLAGS
# is -fno-pic -fno-pie (a static non-PIE), so the PIE side of the tree has its
# own flag set.  PICFLAGS is UCFLAGS with the two anti-PIC flags swapped for
# -fPIC; the -shared link produces the ET_DYN image the kernel loads at
# ELF_DYN_BASE and the rtld relocates.
override PICFLAGS := $(subst -fno-pic -fno-pie,-fPIC,$(UCFLAGS))
override PICLDFLAGS := -nostdlib -shared -z max-page-size=0x1000 -z noexecstack --hash-style=sysv
LIBC_PIC_OBJ := build/userland/libc/mem-pic.o build/userland/libc/malloc-pic.o

CSRC := $(shell find kernel -name '*.c')
ASMSRC := $(shell find kernel -name '*.asm')
OBJ := $(CSRC:%.c=build/%.c.o) $(ASMSRC:%.asm=build/%.asm.o)

.PHONY: all clean run run-uefi iso userland
all: $(ISO)
userland: $(UELF)

build/userland/elf_test.c.o: userland/elf_test.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/elf_test.elf: build/userland/elf_test.c.o userland/user.ld
	$(LD) $(ULDFLAGS) -T userland/user.ld $< -o $@

build/userland/elf_packed.elf: build/userland/elf_test.c.o userland/user_packed.ld
	$(LD) $(ULDFLAGS) -T userland/user_packed.ld $< -o $@

build/userland/init.c.o: userland/init/main.c userland/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c userland/init/main.c -o $@

build/userland/crt0.asm.o: userland/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/init.elf: build/userland/init.c.o build/userland/crt0.asm.o userland/init.ld
	$(LD) $(ULDFLAGS) -T userland/init.ld build/userland/crt0.asm.o build/userland/init.c.o -o $@

build/userland/ipc_test.c.o: userland/ipc_test.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/ipc_test.elf: build/userland/ipc_test.c.o build/userland/crt0.asm.o userland/user.ld
	$(LD) $(ULDFLAGS) -T userland/user.ld build/userland/crt0.asm.o $< -o $@

build/userland/forkexec_test.c.o: userland/forkexec_test.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/forkexec_test.elf: build/userland/forkexec_test.c.o build/userland/crt0.asm.o userland/user.ld
	$(LD) $(ULDFLAGS) -T userland/user.ld build/userland/crt0.asm.o $< -o $@

build/userland/calc.c.o: userland/services/calc.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/calc.elf: build/userland/calc.c.o build/userland/crt0.asm.o userland/user.ld
	$(LD) $(ULDFLAGS) -T userland/user.ld build/userland/crt0.asm.o $< -o $@

build/userland/srv_test.c.o: userland/services/srv_test.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/srv_test.elf: build/userland/srv_test.c.o build/userland/crt0.asm.o userland/user.ld
	$(LD) $(ULDFLAGS) -T userland/user.ld build/userland/crt0.asm.o $< -o $@

build/userland/lifecycle_test.c.o: userland/services/lifecycle_test.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/lifecycle_test.elf: build/userland/lifecycle_test.c.o build/userland/crt0.asm.o userland/user.ld
	$(LD) $(ULDFLAGS) -T userland/user.ld build/userland/crt0.asm.o $< -o $@

build/userland/vfs_test.c.o: userland/vfs_test.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/vfs_test.elf: build/userland/vfs_test.c.o build/userland/crt0.asm.o userland/user.ld
	$(LD) $(ULDFLAGS) -T userland/user.ld build/userland/crt0.asm.o $< -o $@

build/userland/mm_test.c.o: userland/mm_test.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/mm_test.elf: build/userland/mm_test.c.o build/userland/crt0.asm.o userland/user.ld
	$(LD) $(ULDFLAGS) -T userland/user.ld build/userland/crt0.asm.o $< -o $@

build/userland/malloc_test.c.o: userland/malloc_test.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/malloc_test.elf: build/userland/malloc_test.c.o $(LIBC_OBJ) build/userland/crt0.asm.o userland/user.ld
	$(LD) $(ULDFLAGS) -T userland/user.ld build/userland/crt0.asm.o $(LIBC_OBJ) $< -o $@

build/userland/tty_test.c.o: userland/tty_test.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/tty_test.elf: build/userland/tty_test.c.o build/userland/crt0.asm.o userland/user.ld
	$(LD) $(ULDFLAGS) -T userland/user.ld build/userland/crt0.asm.o $< -o $@

$(LIBC_OBJ): build/userland/libc/%.o: userland/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

# ── position-independent libc (the -fPIC twin of LIBC_OBJ) ────────
$(LIBC_PIC_OBJ): build/userland/libc/%-pic.o: userland/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PICFLAGS) -c $< -o $@

# ── the userland dynamic linker (ld-void.so) ──────────────────────
# A statically linked ET_EXEC at 0x400000: it must be loadable with no
# linker of its own.  The kernel loads it whenever a PIE's PT_INTERP names
# it; it relocates the PIE using the auxv the kernel seeded, then jumps to
# AT_ENTRY.  It is embedded like every other test blob (elf_blobs.asm).
build/userland/ld.so/rtld.c.o: userland/ld.so/rtld.c userland/ld.so/rtld.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c userland/ld.so/rtld.c -o $@

build/userland/ld.so/rtld_start.asm.o: userland/ld.so/rtld_start.S
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c userland/ld.so/rtld_start.S -o $@

build/userland/ld_so.elf: build/userland/ld.so/rtld.c.o build/userland/ld.so/rtld_start.asm.o userland/ld.so/rtld.ld
	$(LD) $(ULDFLAGS) -T userland/ld.so/rtld.ld build/userland/ld.so/rtld_start.asm.o build/userland/ld.so/rtld.c.o -o $@

# ── the dynamic-linker regression (dynamic_test) ────────────────────
# A PIE ("shared object that is the whole program"): -shared with a .interp,
# every libc function built -fPIC into the same image, entry = dynamic_main.
# Its four relocation types all name in-image symbols the rtld can resolve.
build/userland/dynamic_test.c.o: userland/dynamic_test.c
	@mkdir -p $(dir $@)
	$(CC) $(PICFLAGS) -c userland/dynamic_test.c -o $@

build/userland/dynamic_test.interp.o: userland/ld.so/interp.c
	@mkdir -p $(dir $@)
	$(CC) $(PICFLAGS) -c userland/ld.so/interp.c -o $@

build/userland/dynamic_test.elf: build/userland/dynamic_test.c.o $(LIBC_PIC_OBJ) build/userland/dynamic_test.interp.o
	$(LD) $(PICLDFLAGS) -e dynamic_main build/userland/dynamic_test.c.o $(LIBC_PIC_OBJ) build/userland/dynamic_test.interp.o -o $@

# incbin reads the linked user ELFs, so they must exist before nasm runs.
build/kernel/elf/elf_blobs.asm.o: $(UELF)

build/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

$(KERNEL): $(OBJ) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJ) -o $@

$(ISO): $(KERNEL)
	@rm -rf build/iso_root
	@mkdir -p build/iso_root/boot build/iso_root/EFI/BOOT
	cp $(KERNEL) build/iso_root/boot/voidos.elf
	cp scripts/limine.conf build/iso_root/boot/limine.conf
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin build/iso_root/boot/
	cp limine/BOOTX64.EFI build/iso_root/EFI/BOOT/
	cp limine/BOOTIA32.EFI build/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		build/iso_root -o $@

run: $(ISO)
	qemu-system-x86_64 -M q35 -m 256M -cdrom $(ISO) -serial stdio -no-reboot -boot d

run-uefi: $(ISO)
	qemu-system-x86_64 -M q35 -m 256M -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO) -serial stdio -no-reboot -boot d

clean:
	rm -rf build
