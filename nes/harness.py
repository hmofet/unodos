#!/usr/bin/env python3
"""ROM-free Nintendo Entertainment System test harness for the UnoDOS/NES port
(py65 6502 core). The companion to c64/harness.py — a fast, deterministic,
display-independent way to verify the PPU output of an AUTOTEST .nes build
without Mesen (whose GPU-surface capture is RDP-flaky; see the port README).

What it emulates (the slice the UnoDOS kernel touches — see nes/kernel.s):
  - PRG: the 32 KB at $8000 from the iNES file (header skipped).
  - CHR: the 8 KB pattern data; pattern table 0 (first 4 KB) holds the tiles.
  - PPU registers:
      $2002 PPUSTATUS  read -> bit7=1 (vblank) so the boot warmup `bit/bpl`
                        loops pass; the read also resets the $2006 hi/lo latch.
      $2005 PPUSCROLL  writes ignored (latch only).
      $2006 PPUADDR    two writes (hi then lo) set the VRAM pointer.
      $2007 PPUDATA    writes store to VRAM (nametable $2000-$2FFF mirror +
                        palette $3F00-$3F1F) and auto-increment the pointer by 1
                        (the kernel always runs with PPUCTRL inc=+1).
      $2000/$2001      writes tracked (NMI-enable / rendering mask) but otherwise
                        inert — the harness drives NMI itself.
  - NMI: the kernel's main loop spins in wait_vbl on v_vbl, which only the NMI
    sets. The harness fires one NMI per emulated frame (push PC+P, jump $FFFA),
    which both releases wait_vbl and advances the 60 Hz v_tick — exactly the
    real per-frame cadence.
  - $4015/$4000-$4003 APU writes are counted (apu_writes) so a test can assert
    the Music app actually programmed a tone. $4016 reads return 0 (AUTOTEST
    builds drive a scripted pad internally and never poll the controller).

Rendering: the 32x30 nametable is composed through pattern table 0 and the
background palette at $3F00 (NES master-palette indices -> RGB) to a 256x240 PNG.
The attribute table is all zeros (one palette), matching the port.

Script on stdin (one command per line, # comments):
  frames N      run N emulated frames (one NMI each)
  shot NAME     screenshot the composed nametable to <artdir>/NAME.png
  assert apu>0  fail unless the APU has been written at least once
  quit          finish

Usage: harness.py <rom.nes> <artdir> [--trace] [--frames-instrs N] < script
"""
import os, struct, sys, zlib
from py65.devices.mpu6502 import MPU
from py65.memory import ObservableMemory

# Instructions budgeted per emulated frame. A frame's heaviest path is a
# rendering-off full_redraw (clear_nametable writes 1 KB + a launcher/app draw)
# ~ a few thousand 6502 instructions; the rest of the budget is the wait_vbl
# spin, which is harmless. Generous so no frame is ever truncated mid-redraw.
FRAME_INSTRS = 60000

# The NES master palette (2C02), 64 entries, RGB. Standard Mesen-ish values.
NES_PALETTE = [
    (0x62,0x62,0x62),(0x00,0x1F,0xB2),(0x24,0x04,0xC8),(0x52,0x00,0xB2),
    (0x73,0x00,0x76),(0x80,0x00,0x24),(0x73,0x0B,0x00),(0x52,0x28,0x00),
    (0x24,0x44,0x00),(0x00,0x57,0x00),(0x00,0x5C,0x00),(0x00,0x53,0x24),
    (0x00,0x3C,0x76),(0x00,0x00,0x00),(0x00,0x00,0x00),(0x00,0x00,0x00),
    (0xAB,0xAB,0xAB),(0x0D,0x57,0xFF),(0x4B,0x30,0xFF),(0x8A,0x13,0xFF),
    (0xBC,0x08,0xD6),(0xD2,0x12,0x69),(0xC7,0x2E,0x00),(0x9D,0x54,0x00),
    (0x60,0x7B,0x00),(0x20,0x98,0x00),(0x00,0xA3,0x00),(0x00,0x99,0x42),
    (0x00,0x7D,0xB4),(0x00,0x00,0x00),(0x00,0x00,0x00),(0x00,0x00,0x00),
    (0xFF,0xFF,0xFF),(0x53,0xAE,0xFF),(0x90,0x85,0xFF),(0xD3,0x65,0xFF),
    (0xFF,0x57,0xFF),(0xFF,0x5D,0xCF),(0xFF,0x77,0x57),(0xFA,0x9E,0x00),
    (0xBD,0xC7,0x00),(0x7A,0xE7,0x00),(0x43,0xF6,0x11),(0x26,0xEF,0x7E),
    (0x2C,0xD5,0xF6),(0x4E,0x4E,0x4E),(0x00,0x00,0x00),(0x00,0x00,0x00),
    (0xFF,0xFF,0xFF),(0xB6,0xE1,0xFF),(0xCE,0xD1,0xFF),(0xE9,0xC3,0xFF),
    (0xFF,0xBC,0xFF),(0xFF,0xBD,0xF4),(0xFF,0xC6,0xC3),(0xFF,0xD5,0x9A),
    (0xE9,0xE6,0x81),(0xCE,0xF4,0x81),(0xB6,0xFB,0x9A),(0xA9,0xFA,0xC3),
    (0xA9,0xF0,0xF4),(0xB8,0xB8,0xB8),(0x00,0x00,0x00),(0x00,0x00,0x00),
]


def png_rgb(path, w, h, getrow):
    rows = b"".join(b"\x00" + bytes(getrow(y)) for y in range(h))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) +
                chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


class NES:
    def __init__(self, rom_path, artdir, trace=False):
        self.artdir = artdir
        self.trace = trace
        os.makedirs(artdir, exist_ok=True)

        data = open(rom_path, "rb").read()
        assert data[:4] == b"NES\x1a", "not an iNES file"
        prg_banks = data[4]
        prg = data[16:16 + prg_banks * 16384]
        chr_off = 16 + prg_banks * 16384
        self.chr = data[chr_off:chr_off + 8192]
        assert len(self.chr) >= 4096, "no CHR pattern table 0"

        self.mem = ObservableMemory()
        # NROM-256: 32 KB PRG straight to $8000 (mirror if a 16 KB image).
        if len(prg) == 16384:
            prg = prg + prg
        self.mem.write(0x8000, list(prg[:32768]))

        self.vram = bytearray(0x1000)       # nametable space $2000-$2FFF (mirrored)
        self.pal = bytearray(0x20)          # palette $3F00-$3F1F
        self.ppu_addr = 0
        self.ppu_latch_hi = True            # next $2006 write is the high byte
        self.apu_writes = 0
        self.steps = 0
        self.fail = None

        self.mem.subscribe_to_read([0x2002], self._read_status)
        self.mem.subscribe_to_write([0x2006], self._write_addr)
        self.mem.subscribe_to_write([0x2007], self._write_data)
        self.mem.subscribe_to_read([0x2007], self._read_data)
        self.mem.subscribe_to_read([0x4016], lambda a: 0)
        self.mem.subscribe_to_write(list(range(0x4000, 0x4014)), self._apu)
        self.mem.subscribe_to_write([0x4015], self._apu)

        self.mpu = MPU(memory=self.mem)
        self.mpu.pc = self.mem[0xFFFC] | (self.mem[0xFFFD] << 8)
        self.mpu.sp = 0xFF

    # ---- PPU register intercepts ----
    def _read_status(self, addr):
        self.ppu_latch_hi = True            # reading $2002 resets the $2006 latch
        return 0x80                          # vblank set -> boot warmup loops pass

    def _write_addr(self, addr, value):
        if self.ppu_latch_hi:
            self.ppu_addr = (self.ppu_addr & 0x00FF) | (value << 8)
        else:
            self.ppu_addr = (self.ppu_addr & 0xFF00) | value
        self.ppu_latch_hi = not self.ppu_latch_hi
        return None

    def _store(self, addr, value):
        a = addr & 0x3FFF
        if a >= 0x3F00:
            self.pal[a & 0x1F] = value
        elif 0x2000 <= a < 0x3000:
            self.vram[a - 0x2000] = value

    def _write_data(self, addr, value):
        self._store(self.ppu_addr, value)
        self.ppu_addr = (self.ppu_addr + 1) & 0x3FFF   # PPUCTRL inc = +1
        return None

    def _read_data(self, addr):
        a = self.ppu_addr & 0x3FFF
        self.ppu_addr = (self.ppu_addr + 1) & 0x3FFF
        if a >= 0x3F00:
            return self.pal[a & 0x1F]
        if 0x2000 <= a < 0x3000:
            return self.vram[a - 0x2000]
        return 0

    def _apu(self, addr, value):
        self.apu_writes += 1
        return None

    # ---- run ----
    def _nmi(self):
        sp = self.mpu.sp
        pc = self.mpu.pc
        self.mem[0x0100 + sp] = (pc >> 8) & 0xFF
        self.mem[0x0100 + ((sp - 1) & 0xFF)] = pc & 0xFF
        self.mem[0x0100 + ((sp - 2) & 0xFF)] = self.mpu.p & ~0x10  # B clear
        self.mpu.sp = (sp - 3) & 0xFF
        self.mpu.p |= 0x04                                          # set I
        self.mpu.pc = self.mem[0xFFFA] | (self.mem[0xFFFB] << 8)

    def step(self, n):
        for _ in range(n):
            pc = self.mpu.pc
            if self.mem[pc] == 0x00:               # BRK = crash
                self.fail = "BRK (crash) @ $%04X" % pc
                raise RuntimeError(self.fail)
            self.mpu.step()
            self.steps += 1

    def frames(self, n, budget):
        for _ in range(n):
            self._nmi()
            self.step(budget)

    # ---- output ----
    def _tile_pixel(self, tile, x, y):
        base = tile * 16
        p0 = self.chr[base + y]
        p1 = self.chr[base + 8 + y]
        return ((p1 >> (7 - x)) & 1) << 1 | ((p0 >> (7 - x)) & 1)

    def shot(self, name):
        pal_rgb = [NES_PALETTE[self.pal[i] & 0x3F] for i in range(4)]

        def getrow(y):
            out = bytearray()
            cellrow = y >> 3
            ty = y & 7
            for cx in range(32):
                tile = self.vram[cellrow * 32 + cx]
                for tx in range(8):
                    out += bytes(pal_rgb[self._tile_pixel(tile, tx, ty)])
            return out
        path = os.path.join(self.artdir, name + ".png")
        png_rgb(path, 256, 240, getrow)
        print("[shot] %s" % path)


def main():
    argv = sys.argv[1:]
    trace = "--trace" in argv
    budget = FRAME_INSTRS
    if "--frames-instrs" in argv:
        i = argv.index("--frames-instrs")
        budget = int(argv[i + 1])
        del argv[i:i + 2]
    args = [a for a in argv if not a.startswith("--")]
    rom, artdir = args[0], args[1]
    nes = NES(rom, artdir, trace)
    # boot: run past sei/cld/txs and the RAM-clear before the first NMI.
    nes.step(4000)
    for line in sys.stdin:
        line = line.split("#")[0].strip()
        if not line:
            continue
        parts = line.split()
        cmd, rest = parts[0], parts[1:]
        if cmd == "frames":
            nes.frames(int(rest[0]), budget)
        elif cmd == "shot":
            nes.shot(rest[0])
        elif cmd == "assert":
            what = rest[0]
            if what == "apu>0":
                assert nes.apu_writes > 0, "expected at least one APU write"
                print("[assert] apu>0 OK (%d writes)" % nes.apu_writes)
            else:
                raise SystemExit("unknown assert: %s" % what)
        elif cmd == "quit":
            break
        else:
            raise SystemExit("unknown command: %s" % cmd)
    print("[harness] done (%d steps, %d APU writes)" % (nes.steps, nes.apu_writes))


if __name__ == "__main__":
    main()
