#!/usr/bin/env python3
"""ROM-free PinePhone (Allwinner A64, AArch64) test harness for UnoDOS/pinephone.

Like the Pi port runs its kernel on a Unicorn Cortex-A, this verifies the PinePhone
port headlessly. The A64 is simpler to model than the Pi here: there is no GPU
mailbox and no peripheral-timer poll — the kernel programs the DE2 mixer UI layer
to scan out a fixed DRAM framebuffer (PINE_FB), and paces frames off the ARM
architectural generic timer (cntpct_el0), which Unicorn advances on its own. So the
harness just:

  * maps DRAM (kernel + stack + vars + the framebuffer) and a harmless RAM sink over
    the DE2 register block (the layer pokes land here),
  * runs the real payload for an instruction budget (cntpct_el0 advances, so
    wait_vblank returns one frame per loop and the AUTOTEST pad plays out),
  * renders the DE2 framebuffer at PINE_FB to a PNG.

Usage: python pinephone/harness.py <unodos.bin> <out.png> [instr_millions]
"""
import sys, struct, zlib, math
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL, UC_HOOK_MEM_UNMAPPED
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_PC

W, H = 480, 640
LOAD     = 0x40080000
DRAM     = 0x40000000
DRAM_SZ  = 0x01000000           # 16 MB covers kernel + stack + vars + framebuffer
PINE_FB  = 0x40400000
DE2_BASE = 0x01000000           # display engine register block (sunk to RAM)
DE2_SZ   = 0x00200000
UART_PAGE = 0x01C28000          # A64 UART0 (16550, input) — emulated
OFF_UART_RBR = 0x01C28000 - UART_PAGE   # 0x00
OFF_UART_LSR = 0x01C28014 - UART_PAGE   # 0x14
I2S_PAGE  = 0x01C22000          # A64 I2S0 (audio TX FIFO) — emulated
OFF_I2S_TXFIFO = 0x01C22020 - I2S_PAGE  # 0x20
WAV_RATE  = 8000
MAX_SAMPLES = WAV_RATE * 8       # cap the capture at 8 s (the tune then loops)

state = {"keys": b"", "kidx": 0, "kcool": 0, "pcm": []}

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(freq):
    if freq <= 0:
        return "rest"
    midi = round(69 + 12 * math.log2(freq / 440.0))
    return "%s%d" % (NOTE_NAMES[midi % 12], midi // 12 - 1)


def i2s_read(uc, offset, size, ud):
    return 0


def i2s_write(uc, offset, size, value, ud):
    if offset == OFF_I2S_TXFIFO and len(state["pcm"]) < MAX_SAMPLES:
        s = value & 0xFFFF
        if s >= 0x8000:
            s -= 0x10000                 # sign-extend the 16-bit sample
        state["pcm"].append(s)


def write_wav(path, samples):
    pcm = b"".join(struct.pack("<h", max(-32767, min(32767, int(s)))) for s in samples)
    with open(path, "wb") as f:
        f.write(b"RIFF"); f.write(struct.pack("<I", 36 + len(pcm))); f.write(b"WAVE")
        f.write(b"fmt "); f.write(struct.pack("<IHHIIHH", 16, 1, 1, WAV_RATE,
                                              WAV_RATE * 2, 2, 16))
        f.write(b"data"); f.write(struct.pack("<I", len(pcm))); f.write(pcm)


def analyze_pcm(samples, path):
    """Capture proof: write the PCM to WAV and recover the note sequence from the
    actual samples (windowed zero-crossing rate -> frequency -> nearest note)."""
    if not samples:
        print("  (no audio captured)")
        return []
    write_wav(path, samples)
    win = WAV_RATE // 20                  # 50 ms windows
    per_win = []
    for i in range(0, len(samples) - win, win):
        seg = samples[i:i + win]
        crossings = sum(1 for j in range(1, len(seg))
                        if (seg[j - 1] < 0) != (seg[j] < 0))
        per_win.append(note_name(crossings * WAV_RATE / (2 * win)))
    # collapse to runs and drop 1-window boundary blips (each real note holds many)
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

KEYMAP = {"w": b"w", "a": b"a", "s": b"s", "d": b"d",
          "\r": b"\r", "\n": b"\r", " ": b" ", "<": b"\x08"}


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


def uart_read(uc, offset, size, ud):
    if offset == OFF_UART_LSR:
        if state["kcool"] > 0:
            state["kcool"] -= 1
            return 0x60                    # THRE|TEMT, data-ready (bit0) clear
        if state["kidx"] < len(state["keys"]):
            return 0x61                    # data ready (bit0 set)
        return 0x60
    if offset == OFF_UART_RBR:
        if state["kidx"] < len(state["keys"]):
            b = state["keys"][state["kidx"]]
            state["kidx"] += 1
            state["kcool"] = 4
            return b
        return 0
    return 0


def uart_write(uc, offset, size, value, ud):
    pass


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


def main():
    keys, wav, argv = parse_keys(sys.argv)
    rom_path, out_path = argv[1], argv[2]
    storage, tail = None, []
    for a in argv[3:]:
        if a.startswith("--storage="): storage = a.split("=",1)[1]
        else: tail.append(a)
    budget = int(float(tail[0]) * 1_000_000) if tail else 160_000_000
    state["keys"] = keys

    data = open(rom_path, "rb").read()
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(DRAM, DRAM_SZ, UC_PROT_ALL)
    uc.mem_map(DE2_BASE, DE2_SZ, UC_PROT_ALL)
    uc.mmio_map(UART_PAGE, 0x1000, uart_read, None, uart_write, None)
    uc.mmio_map(I2S_PAGE, 0x1000, i2s_read, None, i2s_write, None)
    uc.mem_write(LOAD, data)
    uc.reg_write(UC_ARM64_REG_SP, 0x40200000)
    import os
    FS_PA, FS_SIZE = 0x40302000, 0x1000
    if storage and os.path.exists(storage):
        with open(storage,"rb") as f: uc.mem_write(FS_PA, f.read()[:FS_SIZE])

    def on_unmapped(uc, access, address, size, value, ud):
        print("  !! unmapped access @ 0x%X (size %d) pc=0x%X"
              % (address, size, uc.reg_read(UC_ARM64_REG_PC)))
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    CHUNK = 4_000_000
    pc = LOAD
    ran = 0
    while ran < budget:
        try:
            uc.emu_start(pc, DRAM + DRAM_SZ, count=CHUNK)
        except Exception as e:
            print("  (stopped at ~%dM: %s)" % (ran // 1_000_000, e))
            break
        pc = uc.reg_read(UC_ARM64_REG_PC)
        ran += CHUNK

    fb = uc.mem_read(PINE_FB, W * H * 4)
    rgb = bytearray(W * H * 3)
    for i in range(W * H):
        w = fb[i*4] | (fb[i*4+1] << 8) | (fb[i*4+2] << 16)
        rgb[i*3]   = (w >> 16) & 0xFF
        rgb[i*3+1] = (w >> 8) & 0xFF
        rgb[i*3+2] = w & 0xFF
    write_png(out_path, W, H, rgb)
    print("wrote %s (%dx%d) after ~%dM instrs" % (out_path, W, H, ran // 1_000_000))
    if storage:
        with open(storage,"wb") as f: f.write(bytes(uc.mem_read(FS_PA, FS_SIZE)))
        print("flushed FS -> %s" % storage)
    if wav:
        analyze_pcm(state["pcm"], wav)


if __name__ == "__main__":
    main()
