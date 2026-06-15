#!/usr/bin/env python3
"""ROM-free Commodore 64 test harness for the UnoDOS/C64 port (py65 6510 core).

A real C64 boots into BASIC, you LOAD the PRG and RUN it (the BASIC stub does
SYS 2061). There is no DOS auto-boot like the Apple II, and full VIC-II/CIA/SID
emulation is far more than regression screenshots need, so - like the Apple II
port - this ships its own minimal ROM-free harness around a py65 MPU:

  - loads the .prg at its $0801 load address and jumps straight to `start`
    ($080D), skipping the BASIC interpreter (the stub is for real hardware).
  - VIC-II raster: $D012 (low 8 bits) + $D011 bit 7 (bit 8) advance with CPU
    steps, wrapping at 312 lines (PAL) or 263 (NTSC) - this is what the
    kernel's PAL/NTSC auto-detect samples. Default PAL; pass --ntsc for NTSC.
  - CIA #1 keyboard matrix: the kernel writes a column-select to $DC00 and
    reads rows from $DC01 (active low); the harness returns the row bits for
    whatever keys a `key`/`keys` command is holding down.
  - CIA #1 Time-of-Day: $DC0B/$DC0A/$DC09/$DC08 (hours/min/sec/tenths, BCD)
    present a real clock advancing from CPU steps, with the hardware read
    latch (reading hours latches, reading tenths releases) the kernel relies
    on. Base time 12:00:00.
  - SID ($D400-$D418) writes are counted (beep_count) so a test can assert a
    blip happened (sid_click).
  - framebuffer: hi-res bitmap ($6000-$7F3F) + per-cell colour from screen RAM
    ($4000-$43E7) rendered through the 16-colour C64 palette to an RGB PNG.

Script on stdin (one command per line, # comments):
  wait N            run N ticks (TICK_INSTRS MPU steps each)
  shot NAME         screenshot to <artdir>/NAME.png
  key K             press+release one key (return, esc, left, right, up,
                    down, space)
  keys K1 K2 ...    press several keys in turn
  assert beep>0     fail unless the SID has been written at least once
  assert pal        fail unless the kernel detected PAL (is_pal == 1)
  assert ntsc       fail unless the kernel detected NTSC
  quit              finish

Usage: harness.py <prog.prg> <artdir> [--trace] [--ntsc] < script
"""
import os, struct, sys, zlib
from py65.devices.mpu6502 import MPU
from py65.memory import ObservableMemory

# Calibration. TICK_INSTRS = MPU instruction-steps per `wait` tick. The boot
# path (PAL/NTSC raster sampling + desktop draw) costs the most up front; a
# leading `wait` in the script absorbs it. STEPS_PER_SEC ties the CIA TOD clock
# to CPU steps so `wait` advances HH:MM:SS predictably.
TICK_INSTRS = 7000      # boot (full-screen dither + window draws) ~ 360k steps
KEY_INSTRS = 220000     # a full-screen app redraw clears the 8KB bitmap (~120k steps)
STEPS_PER_SEC = 90000
STEPS_PER_TENTH = STEPS_PER_SEC // 10

# C64 16-colour palette (Pepto), RGB.
PALETTE = [
    (0x00, 0x00, 0x00), (0xFF, 0xFF, 0xFF), (0x68, 0x37, 0x2B), (0x70, 0xA4, 0xB2),
    (0x6F, 0x3D, 0x86), (0x58, 0x8D, 0x43), (0x35, 0x28, 0x79), (0xB8, 0xC7, 0x6F),
    (0x6F, 0x4F, 0x25), (0x43, 0x39, 0x00), (0x9A, 0x67, 0x59), (0x44, 0x44, 0x44),
    (0x6C, 0x6C, 0x6C), (0x9A, 0xD2, 0x84), (0x6C, 0x5E, 0xB5), (0x95, 0x95, 0x95),
]

BITMAP = 0x6000
SCREEN = 0x4000

# Keyboard matrix positions (column, row) per key name. Cursor-left/up include
# the left-shift key, exactly as a real C64 produces them.
KEYS = {
    'return': [(0, 1)],
    'esc':    [(7, 7)],   # RUN/STOP
    'stop':   [(7, 7)],
    'right':  [(0, 2)],   # CRSR <>
    'left':   [(0, 2), (1, 7)],  # CRSR <> + L-SHIFT
    'down':   [(0, 7)],   # CRSR ^v
    'up':     [(0, 7), (1, 7)],  # CRSR ^v + L-SHIFT
    'space':  [(7, 4)],
    'save':   [(0, 4)],   # F1
    'del':    [(0, 0)],   # INST/DEL (backspace)
    'backspace': [(0, 0)],
}

# Printable characters -> matrix position (col, row), mirroring the kernel's
# keymap so `type "..."` and single-char `key` work. Letters are lowercase here
# but the kernel decodes them to uppercase ASCII.
CHARS = {
    'a': (1, 2), 'b': (3, 4), 'c': (2, 4), 'd': (2, 2), 'e': (1, 6), 'f': (2, 5),
    'g': (3, 2), 'h': (3, 5), 'i': (4, 1), 'j': (4, 2), 'k': (4, 5), 'l': (5, 2),
    'm': (4, 4), 'n': (4, 7), 'o': (4, 6), 'p': (5, 1), 'q': (7, 6), 'r': (2, 1),
    's': (1, 5), 't': (2, 6), 'u': (3, 6), 'v': (3, 7), 'w': (1, 1), 'x': (2, 7),
    'y': (3, 1), 'z': (1, 4),
    '0': (4, 3), '1': (7, 0), '2': (7, 3), '3': (1, 0), '4': (1, 3), '5': (2, 0),
    '6': (2, 3), '7': (3, 0), '8': (3, 3), '9': (4, 0),
    '+': (5, 0), '-': (5, 3), '.': (5, 4), ':': (5, 5), '@': (5, 6), ',': (5, 7),
    '*': (6, 1), ';': (6, 2), '=': (6, 5), '/': (6, 7), ' ': (7, 4),
}


# --------------------------------------------------------------------------- PNG
def png_rgb(path, w, h, getrow):
    rows = b"".join(b"\x00" + bytes(getrow(y)) for y in range(h))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)   # 8-bit truecolour RGB
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) +
                chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def to_bcd(v):
    return ((v // 10) << 4) | (v % 10)


class Keyboard:
    """CIA #1 keyboard matrix. The kernel selects columns by writing a byte to
    $DC00 with the selected column's bit = 0, and reads $DC01 where a pressed
    key in a selected column pulls its row bit low."""
    def __init__(self):
        self.colsel = 0xFF
        self.pressed = set()        # set of (col, row)

    def write_pra(self, addr, value):
        self.colsel = value
        return None

    def read_prb(self, addr):
        result = 0xFF
        for col in range(8):
            if not (self.colsel & (1 << col)):     # this column is selected
                for (c, r) in self.pressed:
                    if c == col:
                        result &= ~(1 << r) & 0xFF
        return result


class C64:
    def __init__(self, prg_path, artdir, trace=False, ntsc=False, storage=None):
        self.artdir = artdir
        self.trace = trace
        self.raster_lines = 263 if ntsc else 312
        self.storage = storage          # sidecar file for the persisted FS region
        os.makedirs(artdir, exist_ok=True)

        self.mem = ObservableMemory()
        self.kbd = Keyboard()
        self.beep_count = 0
        self.steps = 0
        self.fail = None
        self.vars = None
        self.d011 = 0               # shadow of the last $D011 write (mode bits)
        self.tod_latched = None     # (h, m, s, t) BCD while the read latch is held

        # load the PRG at its load address, jump to `start` ($080D)
        data = open(prg_path, "rb").read()
        load = data[0] | (data[1] << 8)
        body = data[2:]
        assert load == 0x0801, "expected a $0801 PRG, got $%04X" % load
        self.mem.write(load, list(body))
        self.prg_end = load + len(body)

        # Persisted FS region ($C000-$CFFF): a battery-backed store the kernel's
        # fs_init keeps if it already holds a "USV1" catalog (else it formats +
        # seeds). Preload it from the sidecar so a "power cycle" (a second run
        # sharing --storage) survives; saved back on exit.
        if storage and os.path.exists(storage):
            data = open(storage, "rb").read()[:0x1000]
            self.mem.write(0xC000, list(data))

        # --- I/O intercepts ---
        self.mem.subscribe_to_write([0xDC00], self.kbd.write_pra)
        self.mem.subscribe_to_read([0xDC01], self.kbd.read_prb)
        self.mem.subscribe_to_read([0xD011], self._read_d011)
        self.mem.subscribe_to_write([0xD011], self._write_d011)
        self.mem.subscribe_to_read([0xD012], self._read_d012)
        self.mem.subscribe_to_read([0xDC0B], self._read_todhr)
        self.mem.subscribe_to_read([0xDC0A], self._read_todmin)
        self.mem.subscribe_to_read([0xDC09], self._read_todsec)
        self.mem.subscribe_to_read([0xDC08], self._read_tod10)
        self.mem.subscribe_to_write(list(range(0xD400, 0xD419)), self._write_sid)

        self.mpu = MPU(memory=self.mem, pc=0x080D)
        self.mpu.sp = 0xFF

    # ---- VIC raster (drives PAL/NTSC detection) ----
    # Advance ~1 raster line per 8 MPU instructions so the kernel's back-to-back
    # $D011 (bit 8) and $D012 (low byte) reads see a coherent raster value, and
    # a detection sweep covers every line including the PAL/NTSC maximum.
    def _raster(self):
        return (self.steps // 8) % self.raster_lines

    def _write_d011(self, addr, value):
        self.d011 = value
        return None

    def _read_d011(self, addr):
        bit8 = 0x80 if self._raster() > 0xFF else 0x00
        return (self.d011 & 0x7F) | bit8

    def _read_d012(self, addr):
        return self._raster() & 0xFF

    # ---- CIA #1 Time-of-Day (real clock, base 12:00:00, latched reads) ----
    def _now_bcd(self):
        total = self.steps // STEPS_PER_SEC
        s = total % 60
        m = (total // 60) % 60
        h = 12 + (total // 3600)
        h = ((h - 1) % 12) + 1
        t = (self.steps // STEPS_PER_TENTH) % 10
        return (to_bcd(h), to_bcd(m), to_bcd(s), to_bcd(t))

    def _read_todhr(self, addr):
        self.tod_latched = self._now_bcd()      # reading hours latches the time
        return self.tod_latched[0]

    def _read_todmin(self, addr):
        src = self.tod_latched or self._now_bcd()
        return src[1]

    def _read_todsec(self, addr):
        src = self.tod_latched or self._now_bcd()
        return src[2]

    def _read_tod10(self, addr):
        src = self.tod_latched or self._now_bcd()
        self.tod_latched = None                 # reading tenths releases the latch
        return src[3]

    # ---- SID ----
    def _write_sid(self, addr, value):
        self.beep_count += 1
        return None

    # ---- run ----
    def step(self, n=1):
        for _ in range(n):
            if self.fail:
                raise RuntimeError(self.fail)
            pc = self.mpu.pc
            if self.mem[pc] == 0x00:            # BRK = crash
                self.fail = "BRK (crash) @ $%04X" % pc
                raise RuntimeError(self.fail)
            self.mpu.step()
            self.steps += 1
        self._find_vars()

    def tick(self, n=1):
        self.step(n * TICK_INSTRS)

    def _find_vars(self):
        if self.vars is None:
            for a in range(0x0801, self.prg_end):
                if bytes(self.mem[a:a + 4]) == b"UDC1":
                    self.vars = self.mem[a + 4] | (self.mem[a + 5] << 8)
                    if self.trace:
                        print("[harness] vars @ $%04X" % self.vars)
                    break

    def var(self, off):
        return self.mem[self.vars + off]

    # ---- input ----
    def key(self, name):
        if name in KEYS:
            pos = KEYS[name]
        elif len(name) == 1 and name.lower() in CHARS:
            pos = [CHARS[name.lower()]]
        else:
            raise SystemExit("unknown key: %s" % name)
        self.kbd.pressed = set(pos)
        self.step(KEY_INSTRS)
        self.kbd.pressed = set()
        self.step(KEY_INSTRS // 4)              # let the release register

    def type_str(self, s):
        for ch in s:
            self.key(ch)

    # ---- output ----
    def shot(self, name):
        bm = bytes(self.mem[BITMAP:BITMAP + 8000])
        sc = bytes(self.mem[SCREEN:SCREEN + 1000])

        def getrow(y):
            out = bytearray()
            cellrow = y >> 3
            ylo = y & 7
            for x in range(320):
                cell = cellrow * 40 + (x >> 3)
                colbyte = sc[cell]
                addr = cellrow * 320 + (x >> 3) * 8 + ylo
                bit = (bm[addr] >> (7 - (x & 7))) & 1
                ci = (colbyte >> 4) if bit else (colbyte & 0x0F)
                out += bytes(PALETTE[ci])
            return out
        path = os.path.join(self.artdir, name + ".png")
        png_rgb(path, 320, 200, getrow)
        print("[shot] %s" % path)


def main():
    argv = sys.argv[1:]
    trace = "--trace" in argv
    ntsc = "--ntsc" in argv
    storage = None
    if "--storage" in argv:
        i = argv.index("--storage")
        storage = argv[i + 1]
        del argv[i:i + 2]
    args = [a for a in argv if not a.startswith("--")]
    prog, artdir = args[0], args[1]
    c = C64(prog, artdir, trace, ntsc, storage)
    for line in sys.stdin:
        line = line.split("#")[0].strip()
        if not line:
            continue
        parts = line.split()
        cmd, rest = parts[0], parts[1:]
        if cmd == "wait":
            c.tick(int(rest[0]))
        elif cmd == "shot":
            c.shot(rest[0])
        elif cmd == "key":
            c.key(rest[0])
        elif cmd == "keys":
            for k in rest:
                c.key(k)
        elif cmd == "type":
            # type the rest of the line verbatim (preserve spaces)
            c.type_str(line[len("type"):].strip())
        elif cmd == "assert":
            what = rest[0]
            if what == "beep>0":
                assert c.beep_count > 0, "expected at least one SID write"
                print("[assert] beep>0 OK (%d writes)" % c.beep_count)
            elif what == "pal":
                assert c.var(4) == 1, "expected is_pal==1, got %d" % c.var(4)
                print("[assert] pal OK")
            elif what == "ntsc":
                assert c.var(4) == 0, "expected is_pal==0, got %d" % c.var(4)
                print("[assert] ntsc OK")
            elif what.startswith("count="):
                n = int(what.split("=")[1])
                got = c.mem[0xC004]
                assert got == n, "expected FS file count %d, got %d" % (n, got)
                print("[assert] count=%d OK" % n)
            else:
                raise SystemExit("unknown assert: %s" % what)
        elif cmd == "quit":
            break
        else:
            raise SystemExit("unknown command: %s" % cmd)
    if c.storage:
        with open(c.storage, "wb") as f:
            f.write(bytes(c.mem[0xC000:0xD000]))
        print("[harness] saved FS -> %s" % c.storage)
    print("[harness] done (%d steps)" % c.steps)


if __name__ == "__main__":
    main()
