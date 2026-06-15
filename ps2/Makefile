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
# Core is now APP-FREE (unodos.c #includes app_loader.c); ee_modload.c is the
# platform hook uno_load_module() over mc0:/UnoDOS/Apps, and the 11 app modules
# link in as separate objects.  On the EE each module exports a DISTINCT entry
# uno_app_main_<name> (UNO_APP_SYM) so the ee_modload registry resolves it
# (a real relocatable-EE overlay loader is a separate effort - see ee_modload.c).
EE_OBJS = build/fb.o build/mac_compat.o build/mac_io.o build/unodos.o \
          build/ee_platform.o build/ee_usb.o build/ee_audio.o build/ee_modload.o \
          build/mod_images.o \
          build/app_sysinfo.o build/app_clock.o build/app_files.o \
          build/app_notepad.o build/app_music.o build/app_dostris.o \
          build/app_outlast.o build/app_pacman.o build/app_tracker.o \
          build/app_paint.o build/app_theme.o

# USB HID + audio drivers, embedded into the ELF (bin2c) so a FreeMcBoot launch
# needs no external module files. Source images ship with PS2SDK under iop/irx.
# (LIBSD, the SPU2 driver audsrv needs, lives in rom0 and is SifLoadModule'd.)
IRX_DIR = $(PS2SDK)/iop/irx
IRX_GEN = build/usbd_irx.c build/ps2kbd_irx.c build/ps2mouse_irx.c build/audsrv_irx.c

SPLASH_BIN  = build/unodos-ps2-splash.elf
SPLASH_OBJS = build/main.o build/fb.o build/uno_splash.o

GSKIT ?= $(PS2DEV)/gsKit
EE_INCS   += -I$(GSKIT)/include -Ibuild
EE_LDFLAGS += -L$(GSKIT)/lib
EE_LIBS  = -lgskit -ldmakit -lpad -lmc -lkbd -lmouse -laudsrv -lpacket -lgraph -ldraw -ldma -lkernel
# the desktop core is the colour port driven by the EE platform hooks; pass
# EXTRA_DEF=-DUNO_AUTOTEST_PACMAN (etc.) for a baked-in screenshot variant.
EE_CFLAGS += -O2 -Wall -Wno-unused-value -Wno-multichar -DUNO_COLOR=1 -DUNO_EE $(EXTRA_DEF)

all: $(EE_BIN)

splash: $(SPLASH_BIN)

clean:
	rm -f build/*.o $(EE_BIN) $(SPLASH_BIN) build/apps_mc/*.uno

# Package the app modules as appNN.uno images for the memory card under
# mc0:/UnoDOS/Apps (copy build/apps_mc/*.uno there).  The EE links the modules
# into the ELF via the registry; these images are read by ee_modload.c's storage
# touch and feed the EE overlay loader once it lands (see ee_modload.c).
package: $(EE_OBJS)
	@mkdir -p build/apps_mc
	cp build/app_sysinfo.o build/apps_mc/app00.uno
	cp build/app_clock.o   build/apps_mc/app01.uno
	cp build/app_files.o   build/apps_mc/app02.uno
	cp build/app_notepad.o build/apps_mc/app03.uno
	cp build/app_music.o   build/apps_mc/app04.uno
	cp build/app_dostris.o build/apps_mc/app05.uno
	cp build/app_outlast.o build/apps_mc/app06.uno
	cp build/app_pacman.o  build/apps_mc/app07.uno
	cp build/app_tracker.o build/apps_mc/app08.uno
	cp build/app_paint.o   build/apps_mc/app09.uno
	cp build/app_theme.o   build/apps_mc/app10.uno

build/%.o: %.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

# --- embedded module images for first-boot card population ------------------
# Each app module's relocatable .o IS the appNN.uno image (same bytes `package`
# copies to the card).  bin2c them into one C unit (mod_images.c) so the EE port
# can WRITE them to mc0:/UnoDOS/Apps/appNN.uno on first boot - after which the
# overlay loader reads them back off the card and relocates (genuine round-trip,
# no external card prep).  app##_uno + size_app##_uno per bin2c convention.
# the 11 app .o files the bin2c step embeds (each IS its appNN.uno image)
EE_OBJS_APPS = build/app_sysinfo.o build/app_clock.o build/app_files.o \
               build/app_notepad.o build/app_music.o build/app_dostris.o \
               build/app_outlast.o build/app_pacman.o build/app_tracker.o \
               build/app_paint.o build/app_theme.o
EE_MODS = sysinfo clock files notepad music dostris outlast pacman tracker paint theme

build/mod_images.c: $(EE_OBJS_APPS)
	@mkdir -p build
	@i=0; : > build/mod_images.c; \
	for m in $(EE_MODS); do \
	  n=$$(printf "%02d" $$i); \
	  bin2c build/app_$$m.o build/mod_$$n.c mod$${n}_uno >/dev/null; \
	  cat build/mod_$$n.c >> build/mod_images.c; \
	  i=$$((i+1)); \
	done; \
	echo '#include "uno_mod_images.h"' >> build/mod_images.c; \
	echo 'const UnoModImage gUnoModImages[11] = {' >> build/mod_images.c; \
	i=0; for m in $(EE_MODS); do n=$$(printf "%02d" $$i); \
	  echo "  { mod$${n}_uno, (long)sizeof(mod$${n}_uno) }," >> build/mod_images.c; \
	  i=$$((i+1)); done; \
	echo '};' >> build/mod_images.c

build/mod_images.o: build/mod_images.c uno_mod_images.h
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

# Each app module compiles with its entry symbol renamed so the 11 modules can
# co-exist in one ELF (no dynamic linker on the EE).  ee_modload.c's registry
# references uno_app_main_<name>.
build/app_sysinfo.o: apps/sysinfo.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DUNO_APP_SYM=uno_app_main_sysinfo -c $< -o $@
build/app_clock.o: apps/clock.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DUNO_APP_SYM=uno_app_main_clock -c $< -o $@
build/app_files.o: apps/files.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DUNO_APP_SYM=uno_app_main_files -c $< -o $@
build/app_notepad.o: apps/notepad.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DUNO_APP_SYM=uno_app_main_notepad -c $< -o $@
build/app_music.o: apps/music.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DUNO_APP_SYM=uno_app_main_music -c $< -o $@
build/app_dostris.o: apps/dostris.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DUNO_APP_SYM=uno_app_main_dostris -c $< -o $@
build/app_outlast.o: apps/outlast.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DUNO_APP_SYM=uno_app_main_outlast -c $< -o $@
build/app_pacman.o: apps/pacman.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DUNO_APP_SYM=uno_app_main_pacman -c $< -o $@
build/app_tracker.o: apps/tracker.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DUNO_APP_SYM=uno_app_main_tracker -c $< -o $@
build/app_paint.o: apps/paint.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DUNO_APP_SYM=uno_app_main_paint -c $< -o $@
build/app_theme.o: apps/theme.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DUNO_APP_SYM=uno_app_main_theme -c $< -o $@

# embed each IOP driver image as a C array (size_<name> + <name>[])
build/usbd_irx.c: $(IRX_DIR)/usbd.irx
	bin2c $< $@ usbd_irx
build/ps2kbd_irx.c: $(IRX_DIR)/ps2kbd.irx
	bin2c $< $@ ps2kbd_irx
build/ps2mouse_irx.c: $(IRX_DIR)/ps2mouse.irx
	bin2c $< $@ ps2mouse_irx
build/audsrv_irx.c: $(IRX_DIR)/audsrv.irx
	bin2c $< $@ audsrv_irx

# ee_usb.c / ee_audio.c #include the generated images, so they must exist first
build/ee_usb.o: ee_usb.c build/usbd_irx.c build/ps2kbd_irx.c build/ps2mouse_irx.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@
build/ee_audio.o: ee_audio.c build/audsrv_irx.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
