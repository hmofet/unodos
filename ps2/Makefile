# UnoDOS/PS2 - EE ELF (PS2SDK / ee-gcc + gsKit). Needs $PS2SDK set (the
# ps2dev Docker image or a PS2SDK install). Run via `./build.sh ee`, which
# regenerates build/font_data.h first. UNVERIFIED on the dev machine that
# wrote it (no ee-gcc / PCSX2 / BIOS) - see README.md.

EE_BIN  = build/unodos-ps2.elf
EE_OBJS = build/main.o build/fb.o build/uno_splash.o

GSKIT ?= $(PS2DEV)/gsKit
EE_INCS   += -I$(GSKIT)/include
EE_LDFLAGS += -L$(GSKIT)/lib
EE_LIBS  = -lgskit -ldmakit -lpad -lpacket -lgraph -ldraw -ldma -lkernel
EE_CFLAGS += -O2 -Wall -Wno-unused-value

all: $(EE_BIN)

clean:
	rm -f build/*.o $(EE_BIN)

build/%.o: %.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
