#!/usr/bin/env python3
"""Deterministic headless renderer for the UnoDOS/Amiga port.

WinUAE (the port's normal verify path) is a GUI emulator whose screenshots
aren't reproducible run-to-run (wall-clock boot timing + a live desktop clock),
so it can't byte-verify a redraw refactor. This harness plays the part of the
Amiga hardware around a Unicorn (QEMU) 68000 core, exactly the way
macplus/harness.py does for the Mac Plus:

  - AmigaDOS hunk loader: loads build/UnoDOS68K_test (a `-Fhunkexe` image),
    applies HUNK_RELOC32 fixups, at a fast-RAM base clear of chip RAM.
  - exec: `move.l 4.w,a6 / jsr -30(a6)` (Supervisor) lands on a `jmp (a5)` stub,
    so the kernel runs its `super:` body directly (it's bare-metal after that -
    Supervisor + banging the custom chips at $DFF000).
  - custom chips ($DFF000): VPOSR/VHPOSR free-run (so the pre-interrupt splash's
    beam-poll delay terminates), SERDATR reports TBE-ready, SERDAT bytes are the
    serial stream; everything else reads 0 / writes are dropped. No interrupts
    are injected - the autotest scene draws *synchronously* at boot, before the
    tick-driven main loop, so none are needed.
  - stop: the AUTOTEST build emits "UNODOS68K: ATDONE" on serial the instant the
    scene has finished drawing and is about to enter main_loop; we halt there and
    read the framebuffer, so the capture is at a fixed, deterministic point.
  - framebuffer: 5 bitplanes x 8000 B at the fixed chip address $60000, palette
    from the copper list at $76000 (COLORxx register/value pairs) -> 320x200 RGB
    PNG. Both addresses are absolute equates in the port, so relocation-proof.

Usage: render.py <UnoDOS68K_test hunk-exe> <out.png> [--trace] [--max N]
"""
import os, re, struct, sys, zlib
from unicorn import *
from unicorn.m68k_const import *

# ---- memory map ------------------------------------------------------------
RAM_LO      = 0x000000      # chip + slow: vectors, app slots, framebuffer
RAM_LO_SZ   = 0x400000      # 4 MB covers $60000 planes / $76000 copper / stack
KERN_BASE   = 0x00200000    # load the relocatable kernel here (fast RAM)
EXEC_BASE   = 0x00100000    # fake SysBase; Supervisor stub at EXEC_BASE-30
CIA_BASE    = 0x00BF0000    # CIA-A/B window (reads -> 0)
CIA_SZ      = 0x00010000
CUSTOM      = 0x00DFF000
CUSTOM_SZ   = 0x00001000

PLANE0      = 0x60000       # 5 contiguous planes
PLANE_SZ    = 8000
NPLANES     = 5
COPLIST     = 0x76000
SCRW, SCRH  = 320, 200
ROWB        = SCRW // 8     # 40

# custom register offsets (from $DFF000)
R_VPOSR   = 0x004
R_VHPOSR  = 0x006
R_SERDATR = 0x018
R_SERDAT  = 0x030
R_COLOR00 = 0x180


# ---- AmigaDOS hunk loader --------------------------------------------------
HUNK_CODE=0x3E9; HUNK_DATA=0x3EA; HUNK_BSS=0x3EB
HUNK_RELOC32=0x3EC; HUNK_SYMBOL=0x3F0; HUNK_DEBUG=0x3F1
HUNK_END=0x3F2; HUNK_HEADER=0x3F3
HUNK_RELOC32SHORT=0x3FC; HUNK_DREL32=0x3F7   # 16-bit-offset reloc32 (vasm emits 0x3F7)

def load_hunkexe(data, base):
    """Return (entry, writes) where writes is a list of (addr, bytes) to place
    in RAM. Hunks are laid out contiguously from `base`; entry = hunk 0 start."""
    p = [0]
    def u32():
        v = struct.unpack_from(">I", data, p[0])[0]; p[0]+=4; return v
    def u16():
        v = struct.unpack_from(">H", data, p[0])[0]; p[0]+=2; return v
    if u32() != HUNK_HEADER:
        raise SystemExit("not a HUNK_HEADER executable")
    while u32() != 0:            # resident library names (0-terminated list)
        pass
    table_size = u32(); first = u32(); last = u32()
    n = last - first + 1
    sizes = [ (u32() & 0x3FFFFFFF) * 4 for _ in range(n) ]   # longs->bytes, mask mem flags
    # assign each hunk a base (contiguous)
    hbase = []; cur = base
    for s in sizes:
        hbase.append(cur); cur += s
    writes = []
    hi = 0
    while p[0] < len(data):
        t = u32() & 0x3FFFFFFF
        if t == HUNK_END:
            hi += 1; continue
        if t in (HUNK_CODE, HUNK_DATA):
            nlongs = u32(); nbytes = nlongs*4
            blob = data[p[0]:p[0]+nbytes]; p[0]+=nbytes
            writes.append((hbase[hi], blob))
        elif t == HUNK_BSS:
            u32()                                   # length in longs (already zero RAM)
        elif t == HUNK_RELOC32:
            while True:
                cnt = u32()
                if cnt == 0: break
                tgt = u32()                         # hunk the offsets point into
                for _ in range(cnt):
                    off = u32()
                    writes.append(("reloc", hbase[hi], off, hbase[tgt]))
        elif t in (HUNK_RELOC32SHORT, HUNK_DREL32):
            start = p[0]
            while True:
                cnt = u16()
                if cnt == 0: break
                tgt = u16()
                for _ in range(cnt):
                    off = u16()
                    writes.append(("reloc", hbase[hi], off, hbase[tgt]))
            if (p[0] - start) & 2:                  # pad to a longword boundary
                p[0] += 2
        elif t == HUNK_SYMBOL:
            while True:
                ln = u32()
                if ln == 0: break
                p[0] += ln*4 + 4                    # name longs + value
        elif t == HUNK_DEBUG:
            nlongs = u32(); p[0] += nlongs*4
        else:
            raise SystemExit("unhandled hunk type 0x%X at %d" % (t, p[0]-4))
    return hbase[0], writes, sizes


def png_rgb(path, w, h, rows_rgb):
    def chunk(tag, d):
        c = tag + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c))
    raw = b"".join(b"\x00" + rows_rgb[y] for y in range(h))
    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)   # 8-bit truecolour
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", hdr) + chunk(b"IDAT", zlib.compress(raw, 9)) +
        chunk(b"IEND", b""))


def find_symbol(exe_path, name):
    """Resolve a kernel symbol's section offset from build/kernel.lst (vasm
    listing). The dump has `OFFSET  symbol` lines. Re-read per build so the
    address survives code edits (unlike a hardcoded offset)."""
    lst = os.path.join(os.path.dirname(exe_path), "kernel.lst")
    if not os.path.exists(lst):
        return None
    rx = re.compile(r"^([0-9A-Fa-f]{8}) %s$" % re.escape(name))
    for line in open(lst, "r", errors="ignore"):
        m = rx.match(line.strip())
        if m:
            return int(m.group(1), 16)
    return None


class Amiga:
    def __init__(self, exe_path, trace=False):
        self.trace = trace
        self.serial = bytearray()
        self.beam = 0
        self.stop_hit = False
        uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
        uc.ctl_set_cpu_model(UC_CPU_M68K_M68000)   # real 68000, not the ColdFire default
        self.uc = uc
        uc.mem_map(RAM_LO, RAM_LO_SZ)
        uc.mem_map(CIA_BASE, CIA_SZ)
        uc.mem_map(CUSTOM, CUSTOM_SZ)
        # load kernel
        entry, writes, sizes = load_hunkexe(open(exe_path, "rb").read(), KERN_BASE)
        for w in writes:
            if w[0] == "reloc":
                _, hb, off, tgt = w
                cur = struct.unpack(">I", uc.mem_read(hb+off, 4))[0]
                uc.mem_write(hb+off, struct.pack(">I", (cur+tgt) & 0xFFFFFFFF))
            else:
                uc.mem_write(w[0], w[1])
        # relocs may target words already written; apply after raw writes (order:
        # raw first). Re-do relocs now that all code is in place:
        for w in writes:
            if w[0] == "reloc":
                pass  # already applied above in-order; kept for clarity
        self.entry = entry
        # exec SysBase stub: mem[4] = EXEC_BASE; [EXEC_BASE-30] = jmp (a5) = $4ED5
        uc.mem_write(4, struct.pack(">I", EXEC_BASE))
        uc.mem_write(EXEC_BASE - 30, b"\x4E\xD5")
        # custom-chip MMIO
        uc.hook_add(UC_HOOK_MEM_READ, self._cread, begin=CUSTOM, end=CUSTOM+CUSTOM_SZ-1)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._cwrite, begin=CUSTOM, end=CUSTOM+CUSTOM_SZ-1)
        # `ticks` (the vblank frame counter) is bumped by the level-3 ISR, which
        # we don't run - the cursor it also updates is a hardware sprite, absent
        # from the bitplanes we render. Auto-advance ticks on read so the port's
        # frame-delays (fdd_delay etc.) terminate; deterministic per code path.
        toff = find_symbol(exe_path, "ticks")
        self.ticks_addr = (KERN_BASE + toff) if toff is not None else None
        if self.ticks_addr is not None:
            uc.hook_add(UC_HOOK_MEM_READ, self._tick_read,
                        begin=self.ticks_addr, end=self.ticks_addr + 3)
        if trace:
            uc.hook_add(UC_HOOK_CODE, self._trace)

    # --- custom chip register model ---
    def _vpos_long(self):
        """VPOSR:VHPOSR as the 32-bit value a `move.l $DFF004,dn` sees. The port
        beam-polls this (splash_wait_frames masks $0001FF00, i.e. the vertical
        position V8|V7..V0) for line 300, so sweep V 0..312 to guarantee the poll
        terminates. Deterministic; splash clears itself before the desktop draws."""
        v = self.beam
        self.beam = (self.beam + 1)
        if self.beam > 312:
            self.beam = 0
        vpos = v & 0x1FF
        hi = vpos >> 8          # VPOSR word: bit0 = V8 (mask only cares about it)
        lo = (vpos & 0xFF) << 8 # VHPOSR word: high byte = V7..V0
        return (hi << 16) | lo

    def _cread(self, uc, access, addr, size, value, user):
        off = addr - CUSTOM
        if off == R_VPOSR:
            longv = self._vpos_long()
            if size == 4:   uc.mem_write(addr, struct.pack(">I", longv))
            elif size == 2: uc.mem_write(addr, struct.pack(">H", longv >> 16))
            else:           uc.mem_write(addr, bytes([(longv >> 24) & 0xFF]))
            return
        if off == R_VHPOSR:
            longv = self._vpos_long()
            if size == 2:   uc.mem_write(addr, struct.pack(">H", longv & 0xFFFF))
            else:           uc.mem_write(addr, bytes([(longv >> 8) & 0xFF]))
            return
        if off == R_SERDATR:                            # TBE ready: bit 13 (=bit5 of hi byte)
            if size == 2: uc.mem_write(addr, b"\x20\x00")
            else:         uc.mem_write(addr, b"\x20")
            return
        uc.mem_write(addr, b"\x00" * size)              # everything else reads 0

    def _cwrite(self, uc, access, addr, size, value, user):
        off = addr - CUSTOM
        if off == R_SERDAT:
            self.serial.append(value & 0xFF)
            if self.serial.endswith(b"ATDONE\r\n"):
                self.stop_hit = True
                uc.emu_stop()
        # all other custom writes (colors, bplcon, dmacon...) are dropped

    def _tick_read(self, uc, access, addr, size, value, user):
        cur = struct.unpack(">I", uc.mem_read(self.ticks_addr, 4))[0]
        uc.mem_write(self.ticks_addr, struct.pack(">I", (cur + 1) & 0xFFFFFFFF))

    def _trace(self, uc, addr, size, user):
        print("PC=%06X" % addr)

    def run(self, max_insns):
        # AmigaDOS hands the program a valid stack; the kernel's first act
        # (start:) is `jsr Supervisor` which pushes, so seed SP before the super:
        # body resets it to STACKTOP itself.
        # Supervisor() would enter supervisor mode; our jmp(a5) stub bypasses
        # that, and super:'s first op (or.w #$0700,sr) is privileged. Set S
        # *before* SP - switching to supervisor selects the SSP, so A7 must be
        # written after, or the jsr pushes to a stale stack.
        try:
            self.uc.reg_write(UC_M68K_REG_SR, 0x2700)
        except Exception:
            pass
        self.uc.reg_write(UC_M68K_REG_A7, 0x7C000)
        try:
            self.uc.emu_start(self.entry, 0, count=max_insns)
        except UcError as e:
            pc = self.uc.reg_read(UC_M68K_REG_PC)
            raise SystemExit("emu error %s at PC=%06X\nserial: %r"
                             % (e, pc, bytes(self.serial)))

    # --- framebuffer -> PNG ---
    def palette(self):
        pal = [(0,0,0)] * 32
        cop = self.uc.mem_read(COPLIST, 0x400)
        i = 0
        while i + 4 <= len(cop):
            reg = struct.unpack_from(">H", cop, i)[0]
            val = struct.unpack_from(">H", cop, i+2)[0]
            if reg == 0xFFFF:                # copper end
                break
            if R_COLOR00 <= reg <= R_COLOR00 + 2*31 and (reg & 1) == 0:
                idx = (reg - R_COLOR00) // 2
                r = ((val >> 8) & 0xF) * 17
                g = ((val >> 4) & 0xF) * 17
                b = (val & 0xF) * 17
                pal[idx] = (r, g, b)
            i += 4
        return pal

    def dump_png(self, path):
        pal = self.palette()
        planes = [ self.uc.mem_read(PLANE0 + i*PLANE_SZ, PLANE_SZ) for i in range(NPLANES) ]
        rows = []
        for y in range(SCRH):
            row = bytearray(SCRW * 3)
            base = y * ROWB
            for x in range(SCRW):
                byte = base + (x >> 3)
                bit = 7 - (x & 7)
                idx = 0
                for pl in range(NPLANES):
                    idx |= ((planes[pl][byte] >> bit) & 1) << pl
                r, g, b = pal[idx]
                o = x * 3
                row[o] = r; row[o+1] = g; row[o+2] = b
            rows.append(bytes(row))
        png_rgb(path, SCRW, SCRH, rows)


def main():
    argv = sys.argv[1:]
    trace = "--trace" in argv
    maxn = 200_000_000
    if "--max" in argv:
        i = argv.index("--max"); maxn = int(argv[i+1]); del argv[i:i+2]
    argv = [a for a in argv if not a.startswith("--")]
    exe, out = argv[0], argv[1]
    a = Amiga(exe, trace)
    a.run(maxn)
    if not a.stop_hit:
        sys.stderr.write("WARN: ATDONE marker not seen; serial=%r\n" % bytes(a.serial[-80:]))
    a.dump_png(out)
    sys.stderr.write("serial: %s\n" % bytes(a.serial).decode("latin1").strip())
    print("wrote", out)


if __name__ == "__main__":
    main()
