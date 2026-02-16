# UnoDOS Makefile
# Build system for PC XT GUI Operating System

NASM = nasm
QEMU = qemu-system-i386

# Output files
BOOT_BIN = build/boot.bin
STAGE2_BIN = build/stage2.bin
KERNEL_BIN = build/kernel.bin
FLOPPY_IMG = build/unodos.img
FLOPPY_144 = build/unodos-144.img
BUILD_INFO = kernel/build_info.inc

# HD boot files
MBR_BIN = build/mbr.bin
VBR_BIN = build/vbr.bin
STAGE2_HD_BIN = build/stage2_hd.bin
HD_IMG = build/unodos-hd.img

# Directories
BUILD_DIR = build
BOOT_DIR = boot
KERNEL_DIR = kernel
APPS_DIR = apps

# Build number from file
BUILD_NUMBER := $(shell cat BUILD_NUMBER 2>/dev/null || echo 0)

# Application binaries
HELLO_BIN = build/hello.bin
CLOCK_BIN = build/clock.bin
LAUNCHER_BIN = build/launcher.bin
BROWSER_BIN = build/browser.bin
MOUSE_TEST_BIN = build/mouse_test.bin
MUSIC_BIN = build/music.bin
MKBOOT_BIN = build/mkboot.bin
SETTINGS_BIN = build/settings.bin

# Floppy sizes
FLOPPY_360K = 368640
FLOPPY_144M = 1474560

.PHONY: all clean run debug floppy144 check-deps help apps test-app hd-image run-hd

all: $(FLOPPY_IMG)

# Check for required dependencies
check-deps:
	@which $(NASM) > /dev/null 2>&1 || (echo "Error: nasm not found. Install with: sudo apt install nasm" && exit 1)

check-qemu:
	@which $(QEMU) > /dev/null 2>&1 || (echo "Error: qemu not found. Install with: sudo apt install qemu-system-x86" && exit 1)

help:
	@echo "UnoDOS Build System"
	@echo ""
	@echo "Floppy Targets:"
	@echo "  all        - Build 360KB floppy image (default)"
	@echo "  floppy144  - Build 1.44MB floppy image"
	@echo "  run        - Build and run in QEMU (360KB)"
	@echo "  run144     - Build and run in QEMU (1.44MB)"
	@echo ""
	@echo "Hard Drive Targets:"
	@echo "  hd-image   - Build 64MB FAT16 HD image"
	@echo "  run-hd     - Build and run HD image in QEMU"
	@echo ""
	@echo "Other:"
	@echo "  apps       - Build all applications"
	@echo "  debug      - Run with QEMU monitor"
	@echo "  sizes      - Show binary sizes"
	@echo "  clean      - Remove build artifacts"
	@echo ""
	@echo "Requirements: nasm, qemu-system-x86, python3"
	@echo "  sudo apt install nasm qemu-system-x86 python3"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Assemble boot sector
$(BOOT_BIN): $(BOOT_DIR)/boot.asm | $(BUILD_DIR) check-deps
	$(NASM) -f bin -o $@ $<

# Assemble stage 2 loader (minimal, 2KB)
$(STAGE2_BIN): $(BOOT_DIR)/stage2.asm | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

# Generate build info include file
$(BUILD_INFO): BUILD_NUMBER VERSION
	@echo "; Auto-generated build info - DO NOT EDIT" > $@
	@echo "BUILD_NUMBER_STR: db 'Build: $(shell printf '%03d' $(BUILD_NUMBER))', 0" >> $@
	@echo "VERSION_STR: db 'UnoDOS v$(shell cat VERSION)', 0" >> $@

# Assemble kernel (28KB)
# Font files now in kernel directory
$(KERNEL_BIN): $(KERNEL_DIR)/kernel.asm $(KERNEL_DIR)/font8x8.asm $(KERNEL_DIR)/font4x6.asm $(BUILD_INFO) | $(BUILD_DIR)
	$(NASM) -f bin -I$(KERNEL_DIR)/ -o $@ $<

# Assemble test applications
$(HELLO_BIN): $(APPS_DIR)/hello.asm | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

$(CLOCK_BIN): $(APPS_DIR)/clock.asm | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

$(LAUNCHER_BIN): $(APPS_DIR)/launcher.asm $(BUILD_INFO) | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

$(BROWSER_BIN): $(APPS_DIR)/browser.asm | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

$(MOUSE_TEST_BIN): $(APPS_DIR)/mouse_test.asm | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

$(MUSIC_BIN): $(APPS_DIR)/music.asm | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

# mkboot depends on boot.bin and stage2.bin (embedded via incbin)
$(MKBOOT_BIN): $(APPS_DIR)/mkboot.asm $(BOOT_BIN) $(STAGE2_BIN) | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

$(SETTINGS_BIN): $(APPS_DIR)/settings.asm | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

# Create 360KB floppy image (target platform)
# Layout: sector 1 = boot, sectors 2-5 = stage2 (2KB), sectors 6-37 = kernel (16KB)
$(FLOPPY_IMG): $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	@echo "Creating 360KB floppy image..."
	dd if=/dev/zero of=$@ bs=512 count=720 2>/dev/null
	dd if=$(BOOT_BIN) of=$@ bs=512 count=1 conv=notrunc 2>/dev/null
	dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=5 conv=notrunc 2>/dev/null
	@echo "Created $@ (360KB)"

# Create 1.44MB floppy image (for modern hardware testing)
$(FLOPPY_144): $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(LAUNCHER_BIN) $(CLOCK_BIN) $(HELLO_BIN) $(BROWSER_BIN) $(MOUSE_TEST_BIN) $(MUSIC_BIN) $(MKBOOT_BIN) $(SETTINGS_BIN)
	@echo "Creating 1.44MB floppy image..."
	dd if=/dev/zero of=$@ bs=512 count=2880 2>/dev/null
	dd if=$(BOOT_BIN) of=$@ bs=512 count=1 conv=notrunc 2>/dev/null
	dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=5 conv=notrunc 2>/dev/null
	@echo "Adding FAT12 filesystem with apps..."
	python3 tools/add_floppy_fs.py $@ $(LAUNCHER_BIN) LAUNCHER.BIN $(CLOCK_BIN) CLOCK.BIN $(HELLO_BIN) TEST.BIN $(BROWSER_BIN) BROWSER.BIN $(MOUSE_TEST_BIN) MOUSE.BIN $(MUSIC_BIN) MUSIC.BIN $(MKBOOT_BIN) MKBOOT.BIN $(SETTINGS_BIN) SETTINGS.BIN
	@echo "Created $@ (1.44MB)"

floppy144: $(FLOPPY_144)

# Run in QEMU (emulating older hardware)
# Note: Using -M isapc for proper PC/XT BIOS boot compatibility
run: $(FLOPPY_IMG) check-qemu
	$(QEMU) -M isapc \
		-m 640K \
		-drive file=$(FLOPPY_IMG),format=raw,if=floppy \
		-boot a \
		-display gtk

# Run with 1.44MB image
run144: $(FLOPPY_144) check-qemu
	$(QEMU) -M isapc \
		-m 640K \
		-drive file=$(FLOPPY_144),format=raw,if=floppy \
		-boot a \
		-display gtk

# Debug mode with QEMU monitor
debug: $(FLOPPY_IMG) check-qemu
	$(QEMU) -M isapc \
		-m 640K \
		-drive file=$(FLOPPY_IMG),format=raw,if=floppy \
		-boot a \
		-monitor stdio \
		-d int,cpu_reset

# Test FAT12 filesystem with two drives
test-fat12: $(FLOPPY_IMG) build/test-fat12.img check-qemu
	$(QEMU) -M isapc \
		-m 640K \
		-drive file=$(FLOPPY_IMG),format=raw,if=floppy,index=0 \
		-drive file=build/test-fat12.img,format=raw,if=floppy,index=1 \
		-boot a \
		-display gtk

# Test FAT12 with multi-cluster file (>512 bytes)
test-fat12-multi: $(FLOPPY_IMG) build/test-fat12-multi.img check-qemu
	$(QEMU) -M isapc \
		-m 640K \
		-drive file=$(FLOPPY_IMG),format=raw,if=floppy,index=0 \
		-drive file=build/test-fat12-multi.img,format=raw,if=floppy,index=1 \
		-boot a \
		-display gtk

# Rebuild multi-cluster test image
build/test-fat12-multi.img: tools/create_multicluster_test.py
	python3 tools/create_multicluster_test.py $@

# Build all applications
apps: $(HELLO_BIN) $(CLOCK_BIN) $(LAUNCHER_BIN) $(BROWSER_BIN) $(MOUSE_TEST_BIN) $(MUSIC_BIN) $(MKBOOT_BIN) $(SETTINGS_BIN)
	@echo "Built applications:"
	@echo "  $(HELLO_BIN) ($$(wc -c < $(HELLO_BIN)) bytes)"
	@echo "  $(CLOCK_BIN) ($$(wc -c < $(CLOCK_BIN)) bytes)"
	@echo "  $(LAUNCHER_BIN) ($$(wc -c < $(LAUNCHER_BIN)) bytes)"
	@echo "  $(BROWSER_BIN) ($$(wc -c < $(BROWSER_BIN)) bytes)"
	@echo "  $(MOUSE_TEST_BIN) ($$(wc -c < $(MOUSE_TEST_BIN)) bytes)"
	@echo "  $(MUSIC_BIN) ($$(wc -c < $(MUSIC_BIN)) bytes)"
	@echo "  $(MKBOOT_BIN) ($$(wc -c < $(MKBOOT_BIN)) bytes)"
	@echo "  $(SETTINGS_BIN) ($$(wc -c < $(SETTINGS_BIN)) bytes)"

# Create app test floppy image (FAT12 with HELLO.BIN)
build/app-test.img: $(HELLO_BIN)
	@echo "Creating app test floppy image..."
	python3 tools/create_app_test.py $@ $(HELLO_BIN)

# Test application loader with QEMU
test-app: $(FLOPPY_IMG) build/app-test.img check-qemu
	$(QEMU) -M isapc \
		-m 640K \
		-drive file=$(FLOPPY_IMG),format=raw,if=floppy,index=0 \
		-drive file=build/app-test.img,format=raw,if=floppy,index=1 \
		-boot a \
		-display gtk

# Create clock app floppy image (FAT12 with HELLO.BIN - kernel expects this name)
build/clock-app.img: $(CLOCK_BIN)
	@echo "Creating clock app floppy image..."
	python3 tools/create_app_test.py $@ $(CLOCK_BIN) HELLO.BIN

# Test clock application with QEMU
test-clock: $(FLOPPY_144) build/clock-app.img check-qemu
	$(QEMU) -M isapc \
		-m 640K \
		-drive file=$(FLOPPY_144),format=raw,if=floppy,index=0 \
		-drive file=build/clock-app.img,format=raw,if=floppy,index=1 \
		-boot a \
		-display gtk

# Create launcher app floppy image (FAT12 with LAUNCHER.BIN + apps)
build/launcher-floppy.img: $(LAUNCHER_BIN) $(CLOCK_BIN) $(HELLO_BIN) $(BROWSER_BIN) $(MOUSE_TEST_BIN) $(MUSIC_BIN) $(MKBOOT_BIN) $(SETTINGS_BIN)
	@echo "Creating launcher floppy image..."
	python3 tools/create_app_test.py $@ $(LAUNCHER_BIN) LAUNCHER.BIN $(CLOCK_BIN) CLOCK.BIN $(HELLO_BIN) TEST.BIN $(BROWSER_BIN) BROWSER.BIN $(MOUSE_TEST_BIN) MOUSE.BIN $(MUSIC_BIN) MUSIC.BIN $(MKBOOT_BIN) MKBOOT.BIN $(SETTINGS_BIN) SETTINGS.BIN

# Legacy alias
build/launcher-app.img: build/launcher-floppy.img
	cp $< $@

# Test launcher application with QEMU
test-launcher: $(FLOPPY_144) build/launcher-floppy.img check-qemu
	$(QEMU) -M isapc \
		-m 640K \
		-drive file=$(FLOPPY_144),format=raw,if=floppy,index=0 \
		-drive file=build/launcher-floppy.img,format=raw,if=floppy,index=1 \
		-boot a \
		-display gtk

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(BUILD_INFO)

# Increment build number
bump-build:
	@echo $$(($$(cat BUILD_NUMBER) + 1)) > BUILD_NUMBER
	@echo "Build number: $$(cat BUILD_NUMBER)"

# Show sizes
sizes: $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	@echo "Boot sector: $$(wc -c < $(BOOT_BIN)) bytes (max 512)"
	@echo "Stage 2:     $$(wc -c < $(STAGE2_BIN)) bytes (loader)"
	@echo "Kernel:      $$(wc -c < $(KERNEL_BIN)) bytes"
	@echo "Total:       $$(($$(wc -c < $(BOOT_BIN)) + $$(wc -c < $(STAGE2_BIN)) + $$(wc -c < $(KERNEL_BIN)))) bytes"

# ============================================================================
# Hard Drive / IDE Support (v3.13.0)
# ============================================================================

# Build HD MBR
$(MBR_BIN): $(BOOT_DIR)/mbr.asm | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

# Build HD VBR
$(VBR_BIN): $(BOOT_DIR)/vbr.asm | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

# Build HD Stage2 loader
$(STAGE2_HD_BIN): $(BOOT_DIR)/stage2_hd.asm | $(BUILD_DIR)
	$(NASM) -f bin -o $@ $<

# Create bootable FAT16 hard drive image
$(HD_IMG): $(MBR_BIN) $(VBR_BIN) $(STAGE2_HD_BIN) $(KERNEL_BIN) apps
	@echo "Creating bootable FAT16 hard drive image..."
	python3 tools/create_hd_image.py $@

hd-image: $(HD_IMG)

# Run HD image in QEMU
run-hd: $(HD_IMG) check-qemu
	$(QEMU) -M isapc \
		-m 640K \
		-hda $(HD_IMG) \
		-boot c \
		-display gtk

# Run HD image with blank floppy (for testing mkboot)
run-hd-floppy: $(HD_IMG) check-qemu
	@dd if=/dev/zero of=build/blank-floppy.img bs=512 count=2880 2>/dev/null
	$(QEMU) -M isapc \
		-m 640K \
		-hda $(HD_IMG) \
		-drive file=build/blank-floppy.img,format=raw,if=floppy \
		-boot c \
		-display gtk

# Run HD image with PS/2 mouse enabled
run-hd-mouse: $(HD_IMG) check-qemu
	$(QEMU) -M isapc \
		-m 640K \
		-hda $(HD_IMG) \
		-boot c \
		-device usb-mouse \
		-display gtk
