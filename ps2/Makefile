# UnoDOS/PS2 - EE ELF (PS2SDK / ee-gcc + gsKit). Needs $PS2SDK set (the ps2dev
# release or a PS2SDK install). Driven by `./build.sh ee [FEATURE]`, which
# regenerates build/font_data.h first.
#
#   make            -> build/unodos-ps2.elf       the full desktop (M1): the
#                      portable core unodos.c + Mac-compat shim + ee_platform,
#                      GS-presented, DualShock 2 driven. -DUNO_EE -DUNO_COLOR=1
#                      (+ any -DUNO_AUTOTEST_* passed in EXTRA_DEF for a
#                      self-driving screenshot build).
#   make splash     -> build/unodos-ps2-splash.elf the M0 hello-GS splash
#                      (standalone main.c), kept for bring-up reference.

EE_BIN  = build/unodos-ps2.elf
EE_OBJS = build/fb.o build/mac_compat.o build/mac_io.o build/unodos.o build/ee_platform.o build/ee_usb.o

# USB HID drivers, embedded into the ELF (bin2c) so a FreeMcBoot launch needs no
# external module files. Source images ship with PS2SDK under iop/irx.
IRX_DIR = $(PS2SDK)/iop/irx
IRX_GEN = build/usbd_irx.c build/ps2kbd_irx.c build/ps2mouse_irx.c

SPLASH_BIN  = build/unodos-ps2-splash.elf
SPLASH_OBJS = build/main.o build/fb.o build/uno_splash.o

GSKIT ?= $(PS2DEV)/gsKit
EE_INCS   += -I$(GSKIT)/include -Ibuild
EE_LDFLAGS += -L$(GSKIT)/lib
EE_LIBS  = -lgskit -ldmakit -lpad -lmc -lkbd -lmouse -lpacket -lgraph -ldraw -ldma -lkernel
# the desktop core is the colour port driven by the EE platform hooks; pass
# EXTRA_DEF=-DUNO_AUTOTEST_PACMAN (etc.) for a baked-in screenshot variant.
EE_CFLAGS += -O2 -Wall -Wno-unused-value -Wno-multichar -DUNO_COLOR=1 -DUNO_EE $(EXTRA_DEF)

all: $(EE_BIN)

splash: $(SPLASH_BIN)

clean:
	rm -f build/*.o $(EE_BIN) $(SPLASH_BIN)

build/%.o: %.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

# embed each IOP driver image as a C array (size_<name> + <name>[])
build/usbd_irx.c: $(IRX_DIR)/usbd.irx
	bin2c $< $@ usbd_irx
build/ps2kbd_irx.c: $(IRX_DIR)/ps2kbd.irx
	bin2c $< $@ ps2kbd_irx
build/ps2mouse_irx.c: $(IRX_DIR)/ps2mouse.irx
	bin2c $< $@ ps2mouse_irx

# ee_usb.c #includes the generated images, so they must exist first
build/ee_usb.o: ee_usb.c $(IRX_GEN)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
