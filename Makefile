CC = gcc
LD = ld
NASM = nasm
GRUBMKRESCUE = grub-mkrescue
XORRISO = xorriso
OBJCOPY = objcopy

CFLAGS = -std=gnu11 -ffreestanding -fno-pic -fno-pie -fstack-protector-strong -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-omit-frame-pointer -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -O2 -I.
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T linker.ld

C_SRCS = $(shell find . -type f -name '*.c' | sed 's#^./##' | sort)
ASM_SRCS = boot/boot.asm kernel/gdt_flush.asm kernel/isr.asm
C_OBJS = $(patsubst %.c,../build/%.o,$(C_SRCS))
ASM_OBJS = $(patsubst %.asm,../build/%.o,$(ASM_SRCS))
BIN_SRCS = $(shell find assets/wallpapers -type f -name '*.bin' | sed 's#^./##' | sort)
BIN_OBJS = $(patsubst %.bin,../build/%.bin.o,$(BIN_SRCS))
OBJS = $(C_OBJS) $(ASM_OBJS) $(BIN_OBJS)

all: ../nova4.iso

../build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

../build/boot/boot.o: boot/boot.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

../build/kernel/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

../build/%.bin.o: %.bin
	@mkdir -p $(dir $@)
	$(OBJCOPY) --input-target=binary --output-target=elf64-x86-64 --binary-architecture=i386:x86-64 $< $@

../build/nova4.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

../iso/boot/kernel.elf: ../build/nova4.elf
	@mkdir -p ../iso/boot/grub
	cp $< $@

../iso/boot/grub/grub.cfg:
	@mkdir -p ../iso/boot/grub
	printf 'set timeout=2\nset default=0\nset gfxmode=1920x1080x32\nset gfxpayload=1920x1080x32\n\nmenuentry "NovaOS 6.0" {\n  multiboot /boot/kernel.elf\n  boot\n}\n\nmenuentry "NovaOS 6.0 (maintenance)" {\n  multiboot /boot/kernel.elf\n  boot\n}\n' > $@

../nova4.iso: ../iso/boot/kernel.elf ../iso/boot/grub/grub.cfg
	$(GRUBMKRESCUE) -o $@ ../iso >/tmp/nova_grub_mkrescue.log 2>&1 || (cat /tmp/nova_grub_mkrescue.log && false)

clean:
	rm -rf ../build ../iso ../nova4.iso ../validation_userspace ../validation_qemu

test-usb-input: ../nova4.iso
	python3 tools/qemu_usb_input_test.py

test-system-monitor: ../nova4.iso
	python3 tools/qemu_system_monitor_test.py

test-userspace: ../nova4.iso
	python3 tools/qemu_userspace_test.py --iso ../nova4.iso --out-dir ../validation_userspace

test-persistence: ../nova4.iso
	python3 tools/qemu_persistence_test.py --iso ../nova4.iso --out-dir ../validation_persistence --disk ../validation_persistence/nova_persist.img

run-persistent: ../nova4.iso
	bash tools/run_persistent_qemu.sh ../nova4.iso ../validation_persistence/nova_persist.img

run-live-web: ../nova4.iso
	bash tools/run_live_web_qemu.sh ../nova4.iso

test-real-web: ../nova4.iso
	python3 tools/qemu_real_web_validation.py --iso ../nova4.iso --bridge-script tools/live_browser_bridge.py --store-root store --out-dir ../validation_real_web

.PHONY: all clean test-usb-input test-system-monitor test-userspace test-persistence run-persistent run-live-web test-real-web
