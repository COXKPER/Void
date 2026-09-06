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

UELF := build/userland/elf_test.elf build/userland/elf_packed.elf build/userland/init.elf

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
