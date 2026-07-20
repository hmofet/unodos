#!/usr/bin/env python3
"""ROM-free PowerPC Mac test harness for UnoDOS/ppcmac (Unicorn PPC32, big-endian).

The PowerPC port boots over Open Firmware (no Mac OS): OF enters the client program
with the IEEE-1275 client-interface entry in r5, and the kernel makes a few OF calls
to find the `screen` device and read its framebuffer address + linebytes, then draws
into that surface. A real Mac renders to a display no headless grab can read, so —
like every other fresh port — this verifies headlessly on a from-scratch core:

  * run the real PowerPC payload on a Unicorn PPC32 big-endian core,
  * EMULATE the Open Firmware client interface: r5 points at a one-instruction `blr`
    trampoline; a code hook there reads the CI argument array (r3), services
    `finddevice` / `getprop` (handing back a fixed framebuffer base + pitch), and
    returns — so fb_init gets a real surface to draw into,
  * run an instruction budget (wait_vblank is a spin loop, so frames advance and the
    AUTOTEST pad plays out), then render the framebuffer to a PNG.

Usage: python ppcmac/harness.py <unodos.bin> <out.png> [instr_millions]
"""
import sys, struct, zlib, math
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UC_PROT_ALL, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_5, UC_PPC_REG_PC

W, H = 640, 480
PITCH = W * 4
LOAD     = 0x00100000
RAM_BASE = 0x00000000
RAM_SIZE = 0x02000000           # 32 MB covers low mem + kernel + stack + vars + FB
FB_PA    = 0x01000000
OF_ENTRY = 0x00000100           # the OF client-interface trampoline (a single blr)
SOUND_PAGE = 0x80800000         # codec PCM data port (16-bit BE samples) — emulated
WAV_RATE = 8000
MAX_SAMPLES = WAV_RATE * 8

state = {"keys": b"", "kidx": 0, "kcool": 0, "pcm": []}

KEYMAP = {"w": b"w", "a": b"a", "s": b"s", "d": b"d",
          "\r": b"\r", "\n": b"\r", " ": b" ", "<": b"\x08"}

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(freq):
    if freq <= 0:
        return "rest"
    midi = round(69 + 12 * math.log2(freq / 440.0))
    return "%s%d" % (NOTE_NAMES[midi % 12], midi // 12 - 1)


def sound_read(uc, offset, size, ud):
    return 0


def sound_write(uc, offset, size, value, ud):
    if offset == 0 and len(state["pcm"]) < MAX_SAMPLES:
        s = value & 0xFFFF                # the kernel `sth`s big-endian; sign-extend
        if s >= 0x8000:
            s -= 0x10000
        state["pcm"].append(s)


def write_wav(path, samples):
    pcm = b"".join(struct.pack("<h", max(-32767, min(32767, int(s)))) for s in samples)
    with open(path, "wb") as f:
        f.write(b"RIFF"); f.write(struct.pack("<I", 36 + len(pcm))); f.write(b"WAVE")
        f.write(b"fmt "); f.write(struct.pack("<IHHIIHH", 16, 1, 1, WAV_RATE,
                                              WAV_RATE * 2, 2, 16))
        f.write(b"data"); f.write(struct.pack("<I", len(pcm))); f.write(pcm)


def analyze_pcm(samples, path):
    if not samples:
        print("  (no audio captured)")
        return []
    write_wav(path, samples)
    win = WAV_RATE // 20
    per_win = []
    for i in range(0, len(samples) - win, win):
        seg = samples[i:i + win]
        crossings = sum(1 for j in range(1, len(seg))
                        if (seg[j - 1] < 0) != (seg[j] < 0))
        per_win.append(note_name(crossings * WAV_RATE / (2 * win)))
    runs, i = [], 0
    while i < len(per_win):
        j = i
        while j < len(per_win) and per_win[j] == per_win[i]:
            j += 1
        runs.append((per_win[i], j - i))
        i = j
    notes = [nm for nm, n in runs if n >= 2]
    print("  audio: %.1fs PCM -> %s" % (len(samples) / WAV_RATE, path))
    print("  tune:  " + " ".join(notes[:40]))
    return notes


def parse_keys(argv):
    rest, seq, wav = [], b"", None
    for a in argv:
        if a.startswith("--keys="):
            for ch in a[len("--keys="):]:
                seq += KEYMAP.get(ch, ch.encode("latin-1"))
        elif a.startswith("--audio="):
            wav = a[len("--audio="):]
        else:
            rest.append(a)
    return seq, wav, rest


def write_png(path, w, h, rgb):
    raw = bytearray()
    row = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y*row:(y+1)*row]
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def be32(uc, addr):
    return struct.unpack(">I", uc.mem_read(addr, 4))[0]


def cstr(uc, addr):
    out = bytearray()
    while True:
        b = uc.mem_read(addr, 1)[0]
        if b == 0:
            break
        out.append(b)
        addr += 1
    return out.decode("latin-1")


def of_service(uc):
    """Service one Open Firmware client-interface call (r3 = &CI array)."""
    arr = uc.reg_read(UC_PPC_REG_3)
    nargs = be32(uc, arr + 4)
    name = cstr(uc, be32(uc, arr + 0))
    rets_off = 12 + 4 * nargs

    def wr(off, v):
        uc.mem_write(arr + off, struct.pack(">I", v & 0xFFFFFFFF))

    if name in ("finddevice", "instance-to-package", "open"):
        wr(rets_off, 0x11111111)              # a device/instance handle token
    elif name == "getprop":
        prop = cstr(uc, be32(uc, arr + 16))
        buf = be32(uc, arr + 20)
        val = {"address": FB_PA, "frame-buffer-adr": FB_PA, "linebytes": PITCH,
               "width": W, "height": H, "depth": 32,
               "stdout": 0x22222222, "stdin": 0x33333333}.get(prop, 0)
        uc.mem_write(buf, struct.pack(">I", val))
        wr(rets_off, 4)                       # property length returned
    elif name == "read":
        buf = be32(uc, arr + 16)              # read(ihandle, buf, len)
        if state["kcool"] > 0:
            state["kcool"] -= 1
            wr(rets_off, 0)                   # nothing ready this poll
        elif state["kidx"] < len(state["keys"]):
            uc.mem_write(buf, bytes([state["keys"][state["kidx"]]]))
            state["kidx"] += 1
            state["kcool"] = 4
            wr(rets_off, 1)                   # one byte delivered
        else:
            wr(rets_off, 0)
    uc.reg_write(UC_PPC_REG_3, 0)             # catch-result: success


def main():
    keys, wav, argv = parse_keys(sys.argv)
    rom_path, out_path = argv[1], argv[2]
    storage, tail = None, []
    for a in argv[3:]:
        if a.startswith("--storage="): storage = a.split("=",1)[1]
        else: tail.append(a)
    budget = int(float(tail[0]) * 1_000_000) if tail else 120_000_000
    state["keys"] = keys

    data = open(rom_path, "rb").read()
    uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
    uc.mem_map(RAM_BASE, RAM_SIZE, UC_PROT_ALL)
    uc.mmio_map(SOUND_PAGE, 0x1000, sound_read, None, sound_write, None)
    uc.mem_write(LOAD, data)
    uc.mem_write(OF_ENTRY, struct.pack(">I", 0x4E800020))   # blr
    uc.reg_write(UC_PPC_REG_5, OF_ENTRY)                    # OF client entry in r5
    import os
    FS_PA, FS_SIZE = 0x00402000, 0x1000
    if storage and os.path.exists(storage):
        with open(storage,"rb") as f: uc.mem_write(FS_PA, f.read()[:FS_SIZE])

    uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, ud: of_service(uc),
                None, OF_ENTRY, OF_ENTRY)

    def on_unmapped(uc, access, address, size, value, ud):
        print("  !! unmapped access @ 0x%X (size %d) pc=0x%X"
              % (address, size, uc.reg_read(UC_PPC_REG_PC)))
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    CHUNK = 4_000_000
    pc = LOAD
    ran = 0
    while ran < budget:
        try:
            uc.emu_start(pc, LOAD + 0x100000, count=CHUNK)
        except Exception as e:
            print("  (stopped at ~%dM: %s)" % (ran // 1_000_000, e))
            break
        pc = uc.reg_read(UC_PPC_REG_PC)
        ran += CHUNK

    fb = uc.mem_read(FB_PA, W * H * 4)
    rgb = bytearray(W * H * 3)
    for i in range(W * H):
        rgb[i*3]   = fb[i*4+1]   # R
        rgb[i*3+1] = fb[i*4+2]   # G
        rgb[i*3+2] = fb[i*4+3]   # B
    write_png(out_path, W, H, rgb)
    print("wrote %s (%dx%d) after ~%dM instrs" % (out_path, W, H, ran // 1_000_000))
    if storage:
        with open(storage,"wb") as f: f.write(bytes(uc.mem_read(FS_PA, FS_SIZE)))
        print("flushed FS -> %s" % storage)
    if wav:
        analyze_pcm(state["pcm"], wav)


if __name__ == "__main__":
    main()
