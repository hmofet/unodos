#!/usr/bin/env python3
"""ROM-free Apple II test harness for the UnoDOS/AppleII port (py65 6502 core).

Plays the part of the Disk II controller ROM + boot firmware around a py65
MPU:
  - ROM autoload: copies disk[0:4096] (track 0 = boot.s) to $0800, sets
    X = slot*16 (slot 6 -> $60, the standard slot) and PC = $0801 - exactly
    what the real P5A 16-sector autoload does before jumping in.
  - Disk II data latch ($C08C + slot*16, i.e. $C0EC for slot 6): returns the
    next GCR nibble of the CURRENT TRACK's nibble stream. "Current track" is
    read directly from zero-page $F1 (zpTrkCur in boot.s) rather than by
    modeling the 4-phase stepper: by the time readsec reads the data latch,
    seek() has already brought zpTrkCur to the target and boot.s neither
    reads nor cares about the other Disk II soft-switch values (motor,
    drive-select, phase on/off, Q7) - it just discards them. This is a
    deliberate simplification: it validates the byte-level RWTS protocol
    (sync hunting, 4-and-4, 6-and-2, DOS 3.3 skew) which is the harness's
    job per docs/PORTS-PLAN.md; cycle/phase-accurate stepper timing is
    AppleWin's job before real hardware.
  - Keyboard: $C000 (data, bit7 = key-waiting) / $C010 (clear strobe).
  - Speaker $C030: read toggles the (unmodeled) speaker cone; the harness
    just counts accesses (Apple2.beep_count) so tests can assert a beep
    happened (kernel.s's `beep` blocks on a cycle-timed toggle loop).
  - SysInfo seed bytes: $FBB3/$FBC0 (harness-advertised "ROM" ID, since
    there is no ROM in a ROM-free harness - kernel.s may only READ these).
  - Framebuffer: hi-res page 1 ($2000-$3FFF), de-interleaved 280x192 -> PNG.

Script on stdin (one command per line, # comments):
  wait N            run N ticks (TICK_INSTRS MPU steps each)
  shot NAME         screenshot to <artdir>/NAME.png
  key K             press+release a key (letters, digits, return, esc,
                    space, left, right, up, down, tab, backspace)
  keys STRING       press each character of STRING in turn
  assert beep>0     fail unless the speaker ($C030) has toggled at least once
  quit              finish

Usage: harness.py <disk.dsk> <artdir> [--trace] < script
       harness.py --selftest      (GCR encode/decode round-trip checks)
"""
import os, struct, sys, zlib
from py65.devices.mpu6502 import MPU
from py65.memory import ObservableMemory

SLOT = 6
# TICK_INSTRS calibration (HANDOFF.md SS6b): measured against the M1 kernel
# (TICKS_PER_SEC=1000 in kernel.s) - boot + desktop/AUTOTEST setup costs
# ~280,000 6502 instruction-steps before clock_secs first reaches 1; each
# subsequent soft-clock second (1000 main-loop passes, including the once-
# per-second draw_clock_content redraw) costs ~11,250 steps. TICK_INSTRS=
# 5000 makes `wait 60` (300,000 steps) clear setup with clock_secs ~= 2, and
# `wait 30` (150,000 steps) afterward advances the soft clock by ~13 more -
# comfortably "later" for tests/m1.script's m1_clock assertion.
TICK_INSTRS = 5000

# KEY_INSTRS calibration: a keypress's handle_key dispatch (which redraws
# whatever changed - icon highlight, a newly opened window, the desktop area
# behind a closed window) must run to completion before the *next* `shot`,
# not just until the next `wait`. Measured worst cases on the M1 kernel:
# icon-selection toggle ~6,450 steps, opening a window ~25,940 steps, closing
# the topmost window ~16,670 steps. 30,000 covers all three with margin.
KEY_INSTRS = 30000

# ---------------------------------------------------------------------------
# GCR 6-and-2 / 4-and-4 helpers - mirror boot.s exactly (encode is the
# inverse of boot.s's decode; see boot.s's wrtab/flip2/skew + rd44 +
# the twos/sixes/fix1 sequence).
# ---------------------------------------------------------------------------
WRTAB = [
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6,
    0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC,
    0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
    0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
]
assert len(WRTAB) == 64
FLIP2 = [0, 2, 1, 3]
SKEW = [0x0, 0x7, 0xE, 0x6, 0xD, 0x5, 0xC, 0x4,
        0xB, 0x3, 0xA, 0x2, 0x9, 0x1, 0x8, 0xF]

DECTAB = [0] * 256
for _y, _b in enumerate(WRTAB):
    DECTAB[_b] = _y


def enc44(d):
    """4-and-4 encode one byte -> (b1, b2), inverse of boot.s's rd44."""
    return ((d >> 1) | 0xAA, d | 0xAA)


def dec44(b1, b2):
    """Mirrors boot.s's rd44 exactly."""
    return (((b1 << 1) | 1) & 0xFF) & b2


def nibblize_data(payload):
    """256-byte sector payload -> 343 GCR nibbles (86 "twos" + 256 "sixes"
    + 1 checksum), inverse of boot.s's t1/s1/fix1 decode sequence.

    boot.s's t1/s1 loops don't store DECTAB[nibble] directly - each decoded
    value is XORed against a running accumulator (zpTmp, carried from t1
    into s1) and the XORed result is what gets stored/used. That running
    value is exactly the standard DOS 3.3 "diff" chain: zpTmp_k == v_k (the
    raw pre-encode value), so encoding must emit WRTAB[v_k ^ v_(k-1)] with
    v_0 = 0, and the 343rd (checksum) nibble is WRTAB[v_342]."""
    assert len(payload) == 256
    rdbuf2 = [0] * 86
    for j in range(86):
        bits10 = FLIP2[payload[j] & 3]
        bits32 = FLIP2[payload[j + 86] & 3]
        bits54 = FLIP2[payload[j + 172] & 3] if j + 172 <= 255 else 0
        rdbuf2[j] = (bits54 << 4) | (bits32 << 2) | bits10
    sixes = [b >> 2 for b in payload]
    out = []
    prev = 0
    for j in range(85, -1, -1):           # RDBUF2 in reverse (t1's order)
        v = rdbuf2[j]
        out.append(WRTAB[v ^ prev])
        prev = v
    for v in sixes:
        out.append(WRTAB[v ^ prev])
        prev = v
    out.append(WRTAB[prev])               # checksum nibble (boot.s skips it)
    return out


def denibblize_data(nibbles342):
    """Inverse of nibblize_data - mirrors boot.s's decode (checksum, the
    343rd nibble, is not part of this 342-long input and is not checked,
    same as boot.s)."""
    assert len(nibbles342) == 342
    prev = 0
    vals = []
    for n in nibbles342:
        v = DECTAB[n] ^ prev
        vals.append(v)
        prev = v
    rdbuf2 = [0] * 86
    for k in range(86):
        rdbuf2[85 - k] = vals[k]
    sixes = vals[86:342]
    out = bytearray(256)
    for i in range(256):
        if i < 86:
            idx, shift = i, 0
        elif i < 172:
            idx, shift = i - 86, 2
        else:
            idx, shift = i - 172, 4
        twobits = FLIP2[(rdbuf2[idx] >> shift) & 3]
        out[i] = ((sixes[i] << 2) | twobits) & 0xFF
    return bytes(out)


def nibblize_addr(track, sector, volume=0xFE):
    chk = volume ^ track ^ sector
    out = [0xD5, 0xAA, 0x96]
    for v in (volume, track, sector, chk):
        b1, b2 = enc44(v)
        out += [b1, b2]
    out += [0xDE, 0xAA, 0xEB]
    return out


def build_track(track, data4096):
    """4096-byte track image (16 logical 256-byte sectors) -> (stream,
    addr_chk_pos). stream is the GCR nibble stream, physical sectors 0..15
    in stream order; physical sector P carries logical sector SKEW[P] (DOS
    3.3 soft interleave, boot.s). addr_chk_pos maps a stream position (the
    index of an address field's 'DE' epilogue byte, i.e. where DiskII.pos
    sits right after rwts_rd44 x4 reads vol/track/sector/checksum) to that
    sector's physical number P - the kernel-side write path (rwts_write)
    reads $C08F (enter write mode) at exactly that position, so this is how
    the harness learns which physical sector a write capture targets
    (HANDOFF-M2 SS1)."""
    assert len(data4096) == 4096
    stream = []
    addr_chk_pos = {}
    for phys in range(16):
        logical = SKEW[phys]
        payload = data4096[logical * 256:(logical + 1) * 256]
        stream += [0xFF] * 10
        addr = nibblize_addr(track, phys)
        stream += addr[:11]                # D5 AA 96 + 8 4-and-4 bytes
        addr_chk_pos[len(stream)] = phys   # position of the 'DE' epilogue byte
        stream += addr[11:]                # DE AA EB
        stream += [0xFF] * 6
        stream += [0xD5, 0xAA, 0xAD]
        stream += nibblize_data(payload)
        stream += [0xDE, 0xAA, 0xEB]
    stream += [0xFF] * 16
    return stream, addr_chk_pos


def nibblize_track(track, data4096):
    """Stream only (no write-path index) - used by selftest's round trip."""
    return build_track(track, data4096)[0]


def denibblize_track(stream):
    """Inverse of nibblize_track - mirrors boot0's readsec hunt/decode
    loop closely enough to validate nibblize_track. Returns the 4096-byte
    logical-order track image."""
    n = len(stream)
    out = bytearray(4096)
    i = 0
    for _ in range(16):
        while not (stream[i] == 0xD5 and stream[(i + 1) % n] == 0xAA
                   and stream[(i + 2) % n] == 0x96):
            i = (i + 1) % n
        i = (i + 3) % n

        def rd():
            nonlocal i
            b = stream[i]
            i = (i + 1) % n
            return b

        vol = dec44(rd(), rd())
        trk = dec44(rd(), rd())
        sec = dec44(rd(), rd())
        chk = dec44(rd(), rd())
        assert chk == (vol ^ trk ^ sec), f"addr checksum bad @ track {track}"
        while not (stream[i] == 0xD5 and stream[(i + 1) % n] == 0xAA
                   and stream[(i + 2) % n] == 0xAD):
            i = (i + 1) % n
        i = (i + 3) % n
        nibbles = [rd() for _ in range(342)]
        rd()                                # checksum nibble, not verified
        payload = denibblize_data(nibbles)
        logical = SKEW[sec]
        out[logical * 256:(logical + 1) * 256] = payload
    return bytes(out)


def selftest():
    import random
    print("[selftest] 4-and-4 round trip (all 256 values)...")
    for d in range(256):
        assert dec44(*enc44(d)) == d, f"4-and-4 round trip failed for {d:#x}"
    print("[selftest] 6-and-2 round trip (edge + random payloads)...")
    payloads = [bytes([0] * 256), bytes([0xFF] * 256),
                bytes(range(256)), bytes((255 - i) & 0xFF for i in range(256))]
    rng = random.Random(0)
    for _ in range(8):
        payloads.append(bytes(rng.randrange(256) for _ in range(256)))
    for p in payloads:
        nibs = nibblize_data(p)
        assert len(nibs) == 343
        back = denibblize_data(nibs[:342])
        assert back == p, "6-and-2 round trip mismatch"
    print("[selftest] full track nibblize/denibblize round trip...")
    for track in (0, 1, 17, 34):
        data = bytes((track * 7 + i) & 0xFF for i in range(4096))
        stream = nibblize_track(track, data)
        global track_for_assert
        back = denibblize_track(stream)
        assert back == data, f"track {track} round trip mismatch"
    print("[selftest] OK")
    selftest_write()


def selftest_write():
    """HANDOFF-M2 SS1: before the kernel encoder exists, unit-test the
    harness write path by replaying a synthetic capture (nibblize_data
    output framed with sync/prologue/epilogue bytes, as rwts_write emits
    it) through DiskII's enter/write/exit handlers, and assert the payload
    lands in the in-memory image at the right (track, logical sector)."""
    print("[selftest] write-path capture/commit round trip...")
    image = bytearray(35 * 4096)
    track = 5
    disk = DiskII(image, {0xF1: track})
    for phys in (0, 3, 15):
        disk._ensure_track(track)          # (re)build from the current image
        pos = next(p for p, ph in disk.addr_chk_pos.items() if ph == phys)
        disk.pos = pos
        disk.enter_write_mode(0)
        payload = bytes((phys * 17 + i * 3 + 7) & 0xFF for i in range(256))
        capture = [0xFF] * 5 + [0xD5, 0xAA, 0xAD] + nibblize_data(payload) + [0xDE, 0xAA, 0xEB]
        for b in capture:
            disk.write_nibble(0, b)
        disk.exit_write_mode(0)
        logical = SKEW[phys]
        off = track * 4096 + logical * 256
        assert bytes(image[off:off + 256]) == payload, f"write commit mismatch (phys {phys})"
        assert track not in disk.track_cache, "track cache not invalidated"
    print("[selftest] OK")


# ---------------------------------------------------------------------------
# PNG output (lifted from macplus/harness.py)
# ---------------------------------------------------------------------------
def png_gray(path, w, h, getpix):
    rows = b""
    for y in range(h):
        rows += b"\x00" + bytes(getpix(y))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    hdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) +
                chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------------------
# Keyboard - Apple II raw key codes (bit7 = "key waiting" flag at $C000)
# ---------------------------------------------------------------------------
KEYS = {
    'return': 0x8D, 'esc': 0x9B, 'space': 0xA0, 'backspace': 0x88,
    'left': 0x88, 'right': 0x95, 'up': 0x8B, 'down': 0x8A, 'tab': 0x89,
    'openbracket': 0xDB, 'closebracket': 0xDD,
}
for _c in "0123456789":
    KEYS[_c] = 0x80 | ord(_c)
for _c in "abcdefghijklmnopqrstuvwxyz":
    KEYS[_c] = 0x80 | ord(_c.upper())


class DiskII:
    """Read AND write side of the Disk II data latch (HANDOFF-M2 SS1).

    Read path unchanged from M1: $C08C+slot*16 serves the next GCR nibble
    of the current track's stream (current track read from zero page $F1 -
    the zpTrkCur ABI). Write path: rwts_write reads $C08F+slot*16 ("Q7 on")
    to enter write mode - at that instant DiskII.pos is the addr_chk_pos
    recorded by build_track for whichever physical sector's address field
    was just hunted, so that's the write target. Nibbles written to
    $C08D+slot*16 while in write mode are captured; reading $C08E+slot*16
    ("Q7 off") commits the capture: find D5 AA AD, denibblize 342 nibbles,
    store the 256-byte payload into self.image at (track, SKEW[phys]) and
    invalidate that track's cached stream so subsequent reads see it."""

    def __init__(self, image, mem, slot=SLOT):
        self.image = image
        self.mem = mem
        self.track_cache = {}
        self.cur_track = -1
        self.pos = 0
        self.stream = None
        self.addr_chk_pos = {}
        self.write_mode = False
        self.write_phys = None
        self.capture = []

    def _ensure_track(self, track):
        if track != self.cur_track:
            self.cur_track = track
            if track not in self.track_cache:
                off = track * 4096
                data = bytes(self.image[off:off + 4096])
                self.track_cache[track] = build_track(track, data)
            self.stream, self.addr_chk_pos = self.track_cache[track]
            self.pos = 0

    def read_data(self, addr):
        track = self.mem[0xF1]              # zpTrkCur
        self._ensure_track(track)
        b = self.stream[self.pos]
        self.pos = (self.pos + 1) % len(self.stream)
        return b

    def enter_write_mode(self, addr):
        self.write_mode = True
        self.write_phys = self.addr_chk_pos.get(self.pos)
        self.capture = []
        return 0

    def write_nibble(self, addr, value):
        if self.write_mode:
            self.capture.append(value)
        return None

    def exit_write_mode(self, addr):
        if self.write_mode and self.capture:
            self._commit_write(self.cur_track, self.write_phys, self.capture)
        self.write_mode = False
        self.capture = []
        return 0

    def _commit_write(self, track, phys, capture):
        if phys is None:
            return
        for i in range(len(capture) - 2):
            if capture[i] == 0xD5 and capture[i + 1] == 0xAA and capture[i + 2] == 0xAD:
                start = i + 3
                break
        else:
            return
        if start + 342 > len(capture):
            return
        payload = denibblize_data(capture[start:start + 342])
        logical = SKEW[phys]
        off = track * 4096 + logical * 256
        self.image[off:off + 256] = payload
        self.track_cache.pop(track, None)
        if self.cur_track == track:
            self.cur_track = -1            # force a rebuild from self.image


class Keyboard:
    def __init__(self):
        self.latch = 0

    def read_data(self, addr):
        return self.latch

    def read_clear(self, addr):
        self.latch &= 0x7F
        return 0

    def write_clear(self, addr, value):
        self.latch &= 0x7F
        return None

    def press(self, code):
        self.latch = code


class Apple2:
    def __init__(self, disk_path, artdir, trace=False):
        self.disk = bytearray(open(disk_path, "rb").read())
        assert len(self.disk) == 35 * 4096, \
            f"expected a 140K (35-track) image, got {len(self.disk)} bytes"
        self.artdir = artdir
        self.trace = trace
        os.makedirs(artdir, exist_ok=True)

        self.mem = ObservableMemory()
        self.disk2 = DiskII(self.disk, self.mem)
        self.kbd = Keyboard()
        data_addr = 0xC08C + SLOT * 16       # $C0EC - read: next nibble
        wrmode_addr = 0xC08F + SLOT * 16     # $C0EF - read: Q7 on (enter write mode)
        rdmode_addr = 0xC08E + SLOT * 16     # $C0EE - read: Q7 off (exit write mode)
        nibout_addr = 0xC08D + SLOT * 16     # $C0ED - write: nibble out
        self.mem.subscribe_to_read([data_addr], self.disk2.read_data)
        self.mem.subscribe_to_read([wrmode_addr], self.disk2.enter_write_mode)
        self.mem.subscribe_to_read([rdmode_addr], self.disk2.exit_write_mode)
        self.mem.subscribe_to_write([nibout_addr], self.disk2.write_nibble)
        self.mem.subscribe_to_read([0xC000], self.kbd.read_data)
        self.mem.subscribe_to_read([0xC010], self.kbd.read_clear)
        self.mem.subscribe_to_write([0xC010], self.kbd.write_clear)
        self.beep_count = 0
        self.mem.subscribe_to_read([0xC030], self._read_speaker)

        # SysInfo machine-ID seed bytes (no ROM in a ROM-free harness;
        # kernel.s may only READ these, per HANDOFF.md SS6/SS9). $FBB3=$EA
        # alone means "II+" (sysinfo_detect doesn't consult $FBC0 for that
        # case); seed it $EA too for tidiness. M1 targets the II+ floor.
        self.mem[0xFBB3] = 0xEA
        self.mem[0xFBC0] = 0xEA

        # ROM autoload: track 0 (boot.s) -> $0800, X = slot*16, PC = $0801
        self.mem.write(0x0800, list(self.disk[0:4096]))
        self.mpu = MPU(memory=self.mem, pc=0x0801)
        self.mpu.x = SLOT * 16

        self.vars = None
        self.steps = 0
        self.fail = None

    def _read_speaker(self, addr):
        self.beep_count += 1
        return 0

    def step(self, n=1):
        for _ in range(n):
            if self.fail:
                raise RuntimeError(self.fail)
            pc = self.mpu.pc
            op = self.mem[pc]
            if op == 0x00:                  # BRK - treat as a crash
                self.fail = f"BRK (crash) @ {pc:#06x}"
                raise RuntimeError(self.fail)
            if self.trace:
                print(f"{pc:04x}: {op:02x}  a={self.mpu.a:02x} "
                      f"x={self.mpu.x:02x} y={self.mpu.y:02x}")
            self.mpu.step()
            self.steps += 1
        self._find_vars()

    def tick(self, n=1):
        self.step(n * TICK_INSTRS)

    def _find_vars(self):
        if self.vars is None:
            # Gate on $4000 == $4C ("jmp start2") so boot-scratch garbage at
            # $0800-$1FFF can't false-positive match "UDM1" before the
            # kernel is actually loaded and running.
            if (self.mem[0x4000] == 0x4C
                    and bytes(self.mem[0x4003:0x4007]) == b"UDM1"):
                lo, hi = self.mem[0x4007], self.mem[0x4008]
                self.vars = lo | (hi << 8)
                if self.trace:
                    print(f"[harness] vars @ {self.vars:#06x}")

    # ------------------------------------------------------------ input
    def key(self, name):
        code = KEYS.get(name)
        if code is None and name.startswith("ctrl-") and len(name) == 6:
            code = 0x80 | (ord(name[5].upper()) & 0x1F)
        if code is None:
            raise SystemExit(f"unknown key: {name}")
        self.kbd.press(code)
        self.step(KEY_INSTRS)

    def keys(self, s):
        for ch in s:
            self.key(ch.lower() if ch.isalpha() else ch)

    # ------------------------------------------------------------ output
    def shot(self, name):
        fb = bytes(self.mem[0x2000:0x4000])

        def row(y):
            out = bytearray()
            base = (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 + (y >> 6) * 0x28
            for c in range(40):
                b = fb[base + c]
                for bit in range(7):
                    out.append(255 if (b >> bit) & 1 else 0)
            return out
        path = os.path.join(self.artdir, name + ".png")
        png_gray(path, 280, 192, row)
        print(f"[shot] {path}")


def main():
    if "--selftest" in sys.argv:
        selftest()
        return
    argv = sys.argv[1:]
    trace = "--trace" in argv
    writeback = None
    if "--writeback" in argv:
        i = argv.index("--writeback")
        writeback = argv[i + 1]
        del argv[i:i + 2]
    args = [a for a in argv if a != "--trace"]
    disk, artdir = args[0], args[1]
    a2 = Apple2(disk, artdir, trace)
    for line in sys.stdin:
        line = line.split("#")[0].strip()
        if not line:
            continue
        cmd, *rest = line.split()
        if cmd == "wait":
            a2.tick(int(rest[0]))
        elif cmd == "shot":
            a2.shot(rest[0])
        elif cmd == "key":
            a2.key(rest[0])
        elif cmd == "keys":
            a2.keys(rest[0])
        elif cmd == "assert":
            if rest[0] == "beep>0":
                assert a2.beep_count > 0, "expected at least one $C030 toggle"
                print(f"[assert] beep>0 OK ({a2.beep_count} toggles)")
            else:
                raise SystemExit(f"unknown assert: {rest[0]}")
        elif cmd == "quit":
            break
        else:
            raise SystemExit(f"unknown command: {cmd}")
    print(f"[harness] done ({a2.steps} steps)")
    if writeback:
        with open(writeback, "wb") as f:
            f.write(a2.disk)
        print(f"[harness] wrote {writeback}")


if __name__ == "__main__":
    main()
