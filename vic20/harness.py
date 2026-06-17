#!/usr/bin/env python3
"""ROM-free VIC-20 test harness for UnoDOS/VIC-20 (py65 6502 core).

Like the C64/Apple II ports, this verifies headlessly: it loads the .prg at
$1201, jumps straight to `start` ($120D), services the VIC raster ($9004, so the
kernel's wait_frame advances) and the joystick ports ($9111/$9120, returning
"nothing pressed" — the AUTOTEST ROMs drive the pad themselves), runs the 6502,
then renders the 22x23 character matrix (screen $1E00 + colour RAM $9600 + the
char set at $3000 + the $900F background) to a PNG through the VIC palette.

Usage: python vic20/harness.py <prog.prg> <out.png> [instr_millions]
"""
import sys, struct, zlib
from py65.devices.mpu6502 import MPU
from py65.memory import ObservableMemory

SCREEN, COLOR, CHARSET = 0x1E00, 0x9600, 0x3000
PALETTE = [(0,0,0),(255,255,255),(120,40,40),(80,160,170),(120,60,150),(80,150,75),
           (50,40,135),(180,190,110),(120,80,30),(180,130,80),(180,110,110),(135,200,195),
           (170,120,210),(150,210,140),(115,105,200),(220,225,170)]

def png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0); raw += rgb[y*w*3:(y+1)*w*3]
    def ch(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t+d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + ch(b"IDAT", zlib.compress(bytes(raw), 6)) + ch(b"IEND", b""))

def main():
    prg, out = sys.argv[1], sys.argv[2]
    budget = int(float(sys.argv[3]) * 1_000_000) if len(sys.argv) > 3 else 8_000_000
    data = open(prg, "rb").read()
    load = data[0] | (data[1] << 8)
    body = data[2:]
    mem = ObservableMemory()
    mem.write(load, list(body))
    raster = [0]
    def rd(addr):
        if addr == 0x9004:           # VIC raster (high bits): cycle so wait_frame ticks
            raster[0] = (raster[0] + 1) & 0x7F
            return raster[0]
        if addr in (0x9111, 0x9120): # joysticks: nothing pressed (active-low -> $FF)
            return 0xFF
        return None
    mem.subscribe_to_read([0x9004, 0x9111, 0x9120], rd)
    mpu = MPU(memory=mem)
    mpu.pc = 0x120D                  # start (skip the BASIC stub)
    n = 0
    while n < budget:
        mpu.step()
        n += 1
    bg = PALETTE[(mem[0x900F] >> 4) & 15]
    W, H = 22*8, 23*8
    rgb = bytearray(W*H*3)
    for row in range(23):
        for cx in range(22):
            off = row*22 + cx
            chcode = mem[SCREEN + off]
            fg = PALETTE[mem[COLOR + off] & 15]
            base = CHARSET + chcode*8
            for y in range(8):
                bits = mem[base + y]
                for x in range(8):
                    c = fg if (bits & (0x80 >> x)) else bg
                    p = ((row*8 + y)*W + (cx*8 + x))*3
                    rgb[p], rgb[p+1], rgb[p+2] = c
    png(out, W, H, rgb)
    print("wrote %s (%dx%d) after %d instrs" % (out, W, H, n))

if __name__ == "__main__":
    main()
