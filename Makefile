# UnoDOS Makefile
# Build system for PC XT GUI Operating System

NASM = nasm
QEMU = qemu-system-i386

# Output files
BOOT_BIN = build/boot.bin
STAGE2_BIN = build/stage2.bin
FLOPPY_IMG = build/unodos.img
FLOPPY_144 = build/unodos-144.img

# Directories
BUILD_DIR = build
BOOT_DIR = boot

# Floppy sizes
FLOPPY_360K = 368640
FLOPPY_144M = 1474560

.PHONY: all clean run debug floppy144 check-deps help

all: $(FLOPPY_IMG)

# Check for required dependencies
check-deps:
	@which $(NASM) > /dev/null 2>&1 || (echo "Error: nasm not found. Install with: sudo apt install nasm" && exit 1)

check-qemu:
	@which $(QEMU) > /dev/null 2>&1 || (echo "Error: qemu not found. Install with: sudo apt install qemu-system-x86" && exit 1)

help:
	@echo "UnoDOS Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build 360KB floppy image (default)"
	@echo "  floppy144  - Build 1.44MB floppy image"
	@echo "  run        - Build and run in QEMU (360KB)"
	@echo "  run144     - Build and run in QEMU (1.44MB)"
	@echo "  debug      - Run with QEMU monitor"
	@echo "  sizes      - Show binary sizes"
	@echo "  clean      - Remove build artifacts"
	@echo ""
	@echo "Requirements: nasm, qemu-system-x86"
	@echo "  sudo apt install nasm qemu-system-x86"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Assemble boot sector
$(BOOT_BIN): $(BOOT_DIR)/boot.asm | $(BUILD_DIR) check-deps
	$(NASM) -f bin -o $@ $<

# Assemble second stage loader
# -I flag adds include path for font files
$(STAGE2_BIN): $(BOOT_DIR)/stage2.asm $(BOOT_DIR)/font8x8.asm $(BOOT_DIR)/font4x6.asm | $(BUILD_DIR)
	$(NASM) -f bin -I$(BOOT_DIR)/ -o $@ $<

# Create 360KB floppy image (target platform)
$(FLOPPY_IMG): $(BOOT_BIN) $(STAGE2_BIN)
	@echo "Creating 360KB floppy image..."
	dd if=/dev/zero of=$@ bs=512 count=720 2>/dev/null
	dd if=$(BOOT_BIN) of=$@ bs=512 count=1 conv=notrunc 2>/dev/null
	dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null
	@echo "Created $@ (360KB)"

# Create 1.44MB floppy image (for modern hardware testing)
$(FLOPPY_144): $(BOOT_BIN) $(STAGE2_BIN)
	@echo "Creating 1.44MB floppy image..."
	dd if=/dev/zero of=$@ bs=512 count=2880 2>/dev/null
	dd if=$(BOOT_BIN) of=$@ bs=512 count=1 conv=notrunc 2>/dev/null
	dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null
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

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Show sizes
sizes: $(BOOT_BIN) $(STAGE2_BIN)
	@echo "Boot sector: $$(wc -c < $(BOOT_BIN)) bytes (max 512)"
	@echo "Stage 2:     $$(wc -c < $(STAGE2_BIN)) bytes"
	@echo "Total:       $$(($$(wc -c < $(BOOT_BIN)) + $$(wc -c < $(STAGE2_BIN)))) bytes"
