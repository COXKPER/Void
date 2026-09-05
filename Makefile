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

CSRC := $(shell find kernel -name '*.c')
ASMSRC := $(shell find kernel -name '*.asm')
OBJ := $(CSRC:%.c=build/%.c.o) $(ASMSRC:%.asm=build/%.asm.o)

.PHONY: all clean run run-uefi iso
all: $(ISO)

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
