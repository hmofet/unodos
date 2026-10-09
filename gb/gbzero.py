#!/usr/bin/env python3
"""Zero the Nintendo logo in a Game Boy ROM header and fix the global checksum.

rgbfix -v writes the 48-byte Nintendo logo at 0x104-0x133, because the Game
Boy's boot ROM refuses a cartridge without it. UnoDOS publishes this ROM, and
the project does not publish Nintendo artwork, so the logo is zeroed here, the
same way gba/gbafix.py leaves the GBA logo area empty. Emulators that skip the
boot ROM run it; real hardware will stop at the boot logo."""
import sys

f = sys.argv[1]
d = bytearray(open(f, "rb").read())
d[0x104:0x134] = bytes(0x30)
s = sum(b for i, b in enumerate(d) if i not in (0x14E, 0x14F)) & 0xFFFF
d[0x14E], d[0x14F] = s >> 8, s & 0xFF
open(f, "wb").write(d)
print("gbzero: %s logo zeroed, global checksum=0x%04X" % (f, s))
