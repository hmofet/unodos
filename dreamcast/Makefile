# UnoDOS/Dreamcast - KallistiOS ELF (sh-elf-gcc + libkos). Needs the KOS
# environment sourced ($KOS_BASE etc. - `source /opt/toolchains/dc/kos/environ.sh`).
# Driven by `./build.sh dc [FEATURE]`, which regenerates build/font_data.h first
# and sources the KOS environment.
#
#   make            -> build/unodos-dc.elf   the full desktop (M1): the portable
#                      core unodos.c + Mac-compat shim + dc_main, presented to the
#                      640x480 RGB565 framebuffer, maple-driven. -DUNO_DC
#                      -DUNO_COLOR=1 (+ any -DUNO_AUTOTEST_* in EXTRA_DEF for a
#                      self-driving screenshot build).
#   make iso        -> build/unodos-dc.iso   a bootable selfboot CD image
#                      (ELF -> raw -> scrambled 1ST_READ.BIN + IP.BIN -> ISO via
#                      the KOS tools + genisoimage), for emulators
#                      (Flycast/lxdream/redream) and burning to CD-R.
#   make cdi        -> build/unodos-dc.cdi   same, converted to .cdi (needs cdi4dc
#                      or mkdcdisc; falls back to the .iso otherwise).
#   make clean

TARGET   = build/unodos-dc.elf
OBJS     = build/dc_main.o build/fb.o build/mac_compat.o build/mac_io.o build/unodos.o

# image tools (from the KOS utils + the host genisoimage)
OBJCOPY  = $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-objcopy
SCRAMBLE = $(KOS_BASE)/utils/scramble/scramble
MAKEIP   = $(KOS_BASE)/utils/makeip/makeip

all: rm-elf $(TARGET)

include $(KOS_BASE)/Makefile.rules

# the desktop core is the colour port driven by the DC platform hooks; pass
# EXTRA_DEF=-DUNO_AUTOTEST_PACMAN (etc.) for a baked-in screenshot variant.
UNO_CFLAGS = -O2 -Wall -Wno-multichar -Wno-unused-value -DUNO_DC -DUNO_COLOR=1 -I. -Ibuild $(EXTRA_DEF)

rm-elf:
	-rm -f $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) $(KOS_LIBS)

build/%.o: %.c
	kos-cc $(UNO_CFLAGS) -c $< -o $@

# bootable selfboot ISO: ELF -> raw binary -> scrambled 1ST_READ.BIN, plus a
# default homebrew IP.BIN bootstrap, wrapped by genisoimage with -G IP.BIN.
iso: $(TARGET)
	$(OBJCOPY) -R .stack -O binary $(TARGET) build/uno_unscr.bin
	$(SCRAMBLE) build/uno_unscr.bin build/1ST_READ.BIN
	[ -f build/IP.BIN ] || $(MAKEIP) -f build/IP.BIN
	rm -rf build/iso_root && mkdir -p build/iso_root && cp build/1ST_READ.BIN build/iso_root/
	genisoimage -C 0,11702 -V UNODOS -G build/IP.BIN -joliet -rock -l -o build/unodos-dc.iso build/iso_root

# Bootable .cdi via mkdcdisc (the recommended tool - the one Flycast/lxdream/
# redream load happily; verified booting in Flycast). -N disables the GD-ROM
# padding so the image stays ~3 MB instead of ~700 MB. Falls back to the .iso
# (cdi4dc) if mkdcdisc isn't on PATH.
cdi: $(TARGET)
	@if command -v mkdcdisc >/dev/null 2>&1; then \
	  mkdcdisc -e $(TARGET) -o build/unodos-dc.cdi -n UnoDOS -N ; \
	  echo "done: build/unodos-dc.cdi" ; \
	else \
	  $(MAKE) iso ; \
	  command -v cdi4dc >/dev/null 2>&1 && cdi4dc build/unodos-dc.iso build/unodos-dc.cdi || \
	  echo "no mkdcdisc/cdi4dc - boot build/unodos-dc.iso (or .elf) in your emulator" ; \
	fi

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

clean: rm-elf
	-rm -f build/*.o build/unodos-dc.cdi build/unodos-dc.iso build/1ST_READ.BIN \
	       build/IP.BIN build/uno_unscr.bin
	-rm -rf build/iso_root
