#!/usr/bin/env python3
"""Pack a 32KB PRG + 8KB CHR into an iNES (.nes) ROM (NROM, mapper 0)."""
import sys
prg, chr_, out = sys.argv[1], sys.argv[2], sys.argv[3]
p = open(prg, "rb").read()
c = open(chr_, "rb").read()
assert len(p) == 32768, f"PRG must be 32KB, got {len(p)}"
assert len(c) == 8192, f"CHR must be 8KB, got {len(c)}"
hdr = bytes([0x4E,0x45,0x53,0x1A, 2, 1, 0x00, 0x00, 0,0,0,0,0,0,0,0])
open(out, "wb").write(hdr + p + c)
print(f"wrote {out}: {16+len(p)+len(c)} bytes (NROM-256)")
