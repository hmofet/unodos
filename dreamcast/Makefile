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
# Core is APP-FREE (unodos.c #includes app_loader.c).  dc_modload.c is the
# platform hook uno_load_module(): it uses KOS's REAL dynamic-library loader
# (library_open -> elf_load) to read + RELOCATE an app module .KLF off the CD's
# ISO9660 filesystem at runtime.  NO app object is linked into 1ST_READ.BIN -
# the 11 apps live ONLY as build/app_NN.klf, staged on the CD under
# /UNODOS/APPS/APPNN.KLF.  (objdump of 1ST_READ.BIN shows zero app symbols.)
OBJS     = build/dc_main.o build/fb.o build/mac_compat.o build/mac_io.o build/unodos.o \
           build/dc_modload.o

# the 11 app modules, each a SEPARATE relocatable-ELF (.klf) loaded from CD
APP_NAMES = sysinfo clock files notepad music dostris outlast pacman tracker paint theme
APP_KLFS  = build/app_00.klf build/app_01.klf build/app_02.klf build/app_03.klf \
            build/app_04.klf build/app_05.klf build/app_06.klf build/app_07.klf \
            build/app_08.klf build/app_09.klf build/app_10.klf

# the genuine KOS .klf link recipe (see loadable/Makefile.prefab): a -r
# relocatable partial link via the shlelf_dc.xr script.  Use sh-elf-gcc DIRECTLY
# (not the kos-cc wrapper, whose specs drag the whole kernel image into the -r
# output); undefined refs (Toolbox/libc) are resolved at load by export_lookup.
KLF_LD   = $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-gcc
KLF_FLAGS = -ml -m4-single -O2 -Wl,-d -Wl,-r -nostartfiles -nodefaultlibs \
            -Wl,-T $(KOS_BASE)/loadable/shlelf_dc.xr

# image tools (from the KOS utils + the host genisoimage)
OBJCOPY  = $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-objcopy
SCRAMBLE = $(KOS_BASE)/utils/scramble/scramble
MAKEIP   = $(KOS_BASE)/utils/makeip/makeip

all: rm-elf $(TARGET) modules

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

# Each app is built as a SEPARATE relocatable-ELF module (.klf) - compiled with
# -DUNO_DC_MODULE (which activates the lib_get_name/lib_open/... KOS-library
# shim + the deferred uno_app_main bridge in apps/uno_mod.h) and partial-linked
# with the KOS loadable recipe.  All 11 export the SAME symbol `uno_app_main`
# (no UNO_APP_SYM rename) because each .klf is its own image, never co-linked.
modules: $(APP_KLFS)

build/app_00.klf: apps/sysinfo.c
	kos-cc $(UNO_CFLAGS) -DUNO_DC_MODULE -c $< -o build/m_sysinfo.o
	$(KLF_LD) $(KLF_FLAGS) -o $@ build/m_sysinfo.o -Wl,--start-group -lgcc -Wl,--end-group
build/app_01.klf: apps/clock.c
	kos-cc $(UNO_CFLAGS) -DUNO_DC_MODULE -c $< -o build/m_clock.o
	$(KLF_LD) $(KLF_FLAGS) -o $@ build/m_clock.o -Wl,--start-group -lgcc -Wl,--end-group
build/app_02.klf: apps/files.c
	kos-cc $(UNO_CFLAGS) -DUNO_DC_MODULE -c $< -o build/m_files.o
	$(KLF_LD) $(KLF_FLAGS) -o $@ build/m_files.o -Wl,--start-group -lgcc -Wl,--end-group
build/app_03.klf: apps/notepad.c
	kos-cc $(UNO_CFLAGS) -DUNO_DC_MODULE -c $< -o build/m_notepad.o
	$(KLF_LD) $(KLF_FLAGS) -o $@ build/m_notepad.o -Wl,--start-group -lgcc -Wl,--end-group
build/app_04.klf: apps/music.c
	kos-cc $(UNO_CFLAGS) -DUNO_DC_MODULE -c $< -o build/m_music.o
	$(KLF_LD) $(KLF_FLAGS) -o $@ build/m_music.o -Wl,--start-group -lgcc -Wl,--end-group
build/app_05.klf: apps/dostris.c
	kos-cc $(UNO_CFLAGS) -DUNO_DC_MODULE -c $< -o build/m_dostris.o
	$(KLF_LD) $(KLF_FLAGS) -o $@ build/m_dostris.o -Wl,--start-group -lgcc -Wl,--end-group
build/app_06.klf: apps/outlast.c
	kos-cc $(UNO_CFLAGS) -DUNO_DC_MODULE -c $< -o build/m_outlast.o
	$(KLF_LD) $(KLF_FLAGS) -o $@ build/m_outlast.o -Wl,--start-group -lgcc -Wl,--end-group
build/app_07.klf: apps/pacman.c
	kos-cc $(UNO_CFLAGS) -DUNO_DC_MODULE -c $< -o build/m_pacman.o
	$(KLF_LD) $(KLF_FLAGS) -o $@ build/m_pacman.o -Wl,--start-group -lgcc -Wl,--end-group
build/app_08.klf: apps/tracker.c
	kos-cc $(UNO_CFLAGS) -DUNO_DC_MODULE -c $< -o build/m_tracker.o
	$(KLF_LD) $(KLF_FLAGS) -o $@ build/m_tracker.o -Wl,--start-group -lgcc -Wl,--end-group
build/app_09.klf: apps/paint.c
	kos-cc $(UNO_CFLAGS) -DUNO_DC_MODULE -c $< -o build/m_paint.o
	$(KLF_LD) $(KLF_FLAGS) -o $@ build/m_paint.o -Wl,--start-group -lgcc -Wl,--end-group
build/app_10.klf: apps/theme.c
	kos-cc $(UNO_CFLAGS) -DUNO_DC_MODULE -c $< -o build/m_theme.o
	$(KLF_LD) $(KLF_FLAGS) -o $@ build/m_theme.o -Wl,--start-group -lgcc -Wl,--end-group

# bootable selfboot ISO: ELF -> raw binary -> scrambled 1ST_READ.BIN, plus a
# default homebrew IP.BIN bootstrap, wrapped by genisoimage with -G IP.BIN.
iso: $(TARGET) modules
	$(OBJCOPY) -R .stack -O binary $(TARGET) build/uno_unscr.bin
	$(SCRAMBLE) build/uno_unscr.bin build/1ST_READ.BIN
	[ -f build/IP.BIN ] || $(MAKEIP) -f build/IP.BIN
	rm -rf build/iso_root && mkdir -p build/iso_root && cp build/1ST_READ.BIN build/iso_root/
	# stage the 11 app module .KLF images under /UNODOS/APPS; dc_modload.c
	# library_open()'s APP00.KLF .. APP10.KLF off the CD and relocates them at
	# runtime - genuine load-from-CD, no app code in 1ST_READ.BIN.
	mkdir -p build/iso_root/UNODOS/APPS
	cp build/app_00.klf build/iso_root/UNODOS/APPS/APP00.KLF
	cp build/app_01.klf build/iso_root/UNODOS/APPS/APP01.KLF
	cp build/app_02.klf build/iso_root/UNODOS/APPS/APP02.KLF
	cp build/app_03.klf build/iso_root/UNODOS/APPS/APP03.KLF
	cp build/app_04.klf build/iso_root/UNODOS/APPS/APP04.KLF
	cp build/app_05.klf build/iso_root/UNODOS/APPS/APP05.KLF
	cp build/app_06.klf build/iso_root/UNODOS/APPS/APP06.KLF
	cp build/app_07.klf build/iso_root/UNODOS/APPS/APP07.KLF
	cp build/app_08.klf build/iso_root/UNODOS/APPS/APP08.KLF
	cp build/app_09.klf build/iso_root/UNODOS/APPS/APP09.KLF
	cp build/app_10.klf build/iso_root/UNODOS/APPS/APP10.KLF
	genisoimage -C 0,11702 -V UNODOS -G build/IP.BIN -joliet -rock -l -o build/unodos-dc.iso build/iso_root

# Bootable .cdi via mkdcdisc (the recommended tool - the one Flycast/lxdream/
# redream load happily; verified booting in Flycast). -N disables the GD-ROM
# padding so the image stays ~3 MB instead of ~700 MB. Falls back to the .iso
# (cdi4dc) if mkdcdisc isn't on PATH.
cdi: $(TARGET) modules
	# stage the 11 .KLF app modules into a CD tree so mkdcdisc lays them on the
	# data track at /UNODOS/APPS/APPNN.KLF (dc_modload.c library_open()'s them).
	rm -rf build/cd_root && mkdir -p build/cd_root/UNODOS/APPS
	cp build/app_00.klf build/cd_root/UNODOS/APPS/APP00.KLF
	cp build/app_01.klf build/cd_root/UNODOS/APPS/APP01.KLF
	cp build/app_02.klf build/cd_root/UNODOS/APPS/APP02.KLF
	cp build/app_03.klf build/cd_root/UNODOS/APPS/APP03.KLF
	cp build/app_04.klf build/cd_root/UNODOS/APPS/APP04.KLF
	cp build/app_05.klf build/cd_root/UNODOS/APPS/APP05.KLF
	cp build/app_06.klf build/cd_root/UNODOS/APPS/APP06.KLF
	cp build/app_07.klf build/cd_root/UNODOS/APPS/APP07.KLF
	cp build/app_08.klf build/cd_root/UNODOS/APPS/APP08.KLF
	cp build/app_09.klf build/cd_root/UNODOS/APPS/APP09.KLF
	cp build/app_10.klf build/cd_root/UNODOS/APPS/APP10.KLF
	@if command -v mkdcdisc >/dev/null 2>&1; then \
	  mkdcdisc -e $(TARGET) -d build/cd_root/UNODOS -o build/unodos-dc.cdi -n UnoDOS -N ; \
	  echo "done: build/unodos-dc.cdi (with /UNODOS/APPS/*.KLF on the data track)" ; \
	else \
	  $(MAKE) iso ; \
	  command -v cdi4dc >/dev/null 2>&1 && cdi4dc build/unodos-dc.iso build/unodos-dc.cdi || \
	  echo "no mkdcdisc/cdi4dc - boot build/unodos-dc.iso (or .elf) in your emulator" ; \
	fi

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

clean: rm-elf
	-rm -f build/*.o build/*.klf build/unodos-dc.cdi build/unodos-dc.iso \
	       build/1ST_READ.BIN build/IP.BIN build/uno_unscr.bin
	-rm -rf build/iso_root build/cd_root
