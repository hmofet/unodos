#!/usr/bin/env python3
"""Fix a GBA ROM header so emulators accept it: set the fixed 0x96 byte and the
header complement checksum at 0xBD. (mGBA booting a ROM directly does not verify
the Nintendo logo, so we leave 0x04-0x9F zeroed.)"""
import sys

f = sys.argv[1]
d = bytearray(open(f, "rb").read())
if len(d) < 0xC0:
    d += bytes(0xC0 - len(d))
d[0xB2] = 0x96
chk = 0
for i in range(0xA0, 0xBD):
    chk = (chk + d[i]) & 0xFF
d[0xBD] = (-(0x19 + chk)) & 0xFF
open(f, "wb").write(d)
print("gbafix: %s header checksum=0x%02X (%d bytes)" % (f, d[0xBD], len(d)))
