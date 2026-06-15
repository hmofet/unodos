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
#   make cdi        -> build/unodos-dc.cdi   a bootable CD image (mkdcdisc), for
#                      emulators (Flycast/lxdream/redream) and burning to CD-R.
#   make clean

TARGET   = build/unodos-dc.elf
OBJS     = build/dc_main.o build/fb.o build/mac_compat.o build/mac_io.o build/unodos.o

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

cdi: $(TARGET)
	mkdcdisc -e $(TARGET) -o build/unodos-dc.cdi -N

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

clean: rm-elf
	-rm -f build/*.o build/unodos-dc.cdi
