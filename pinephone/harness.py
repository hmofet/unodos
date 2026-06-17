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
import sys, struct, zlib
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

state = {"keys": b"", "kidx": 0, "kcool": 0}

KEYMAP = {"w": b"w", "a": b"a", "s": b"s", "d": b"d",
          "\r": b"\r", "\n": b"\r", " ": b" ", "<": b"\x08"}


def parse_keys(argv):
    rest, seq = [], b""
    for a in argv:
        if a.startswith("--keys="):
            for ch in a[len("--keys="):]:
                seq += KEYMAP.get(ch, ch.encode("latin-1"))
        else:
            rest.append(a)
    return seq, rest


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
    keys, argv = parse_keys(sys.argv)
    rom_path, out_path = argv[1], argv[2]
    budget = int(float(argv[3]) * 1_000_000) if len(argv) > 3 else 160_000_000
    state["keys"] = keys

    data = open(rom_path, "rb").read()
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(DRAM, DRAM_SZ, UC_PROT_ALL)
    uc.mem_map(DE2_BASE, DE2_SZ, UC_PROT_ALL)
    uc.mmio_map(UART_PAGE, 0x1000, uart_read, None, uart_write, None)
    uc.mem_write(LOAD, data)
    uc.reg_write(UC_ARM64_REG_SP, 0x40200000)

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


if __name__ == "__main__":
    main()
