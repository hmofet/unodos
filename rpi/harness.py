#!/usr/bin/env python3
"""ROM-free Raspberry Pi (AArch64) test harness for UnoDOS/rpi (Unicorn ARM64).

A real Pi renders to an HDMI surface the firmware allocates and that no headless
RDP grab can read. So — exactly as the GBA port runs its ARM ROM on Unicorn, and
the C64/Apple II ports use a py65 core — this verifies the port headlessly: it runs
the real kernel8.img on a Unicorn Cortex-A (AArch64), EMULATES the two MMIO channels
the kernel actually touches, and renders the resulting framebuffer to a PNG.

  * VideoCore mailbox (0x3F00B880): on a property-channel write, parse the tag list,
    answer the "allocate framebuffer" + "get pitch" tags (hand back a fixed base),
    so fb_init gets a real surface to draw into.
  * BCM system timer (0x3F003004): hand back a monotonically advancing 1MHz counter
    so wait_vblank paces one frame per loop (real hardware genuinely waits ~16 ms).

PWM/clock writes (the Music tone path) land in a harmless RAM sink. The AUTOTEST
images drive the pad themselves; the harness only services MMIO and runs the budget.

Usage: python rpi/harness.py <kernel8.img> <out.png> [instr_millions]
"""
import sys, struct, zlib
from unicorn import (Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL,
                     UC_HOOK_MEM_UNMAPPED)
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_PC

W, H = 640, 480
PITCH = W * 4
LOAD     = 0x80000
RAM_BASE = 0x80000
RAM_SIZE = 0x400000           # kernel + stack + vars + mailbox buffer + fbinfo
FB_PA    = 0x08000000
FB_SIZE  = (W * H * 4 + 0xFFFF) & ~0xFFFF
PERI_SINK_BASE = 0x3F100000   # GPIO/PWM/clock writes land here (ignored)
PERI_SINK_SIZE = 0x00200000
TIMER_PAGE = 0x3F003000
MBOX_PAGE  = 0x3F00B000

MBOX_READ_OFF   = 0x880 - 0x000   # within MBOX_PAGE (0x3F00B000) -> 0xB880-0xB000
# offsets within MBOX_PAGE (page base 0x3F00B000)
OFF_READ   = 0x3F00B880 - MBOX_PAGE
OFF_STATUS = 0x3F00B898 - MBOX_PAGE
OFF_WRITE  = 0x3F00B8A0 - MBOX_PAGE
OFF_CLO    = 0x3F003004 - TIMER_PAGE

state = {"pending_read": 0, "clock": 0}


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


def service_mailbox(uc, value):
    """Parse the property message at (value & ~0xF) and fill FB base + pitch."""
    bufaddr = value & ~0xF
    size = int.from_bytes(uc.mem_read(bufaddr, 4), "little")
    if size < 8 or size > 0x1000:
        size = 256
    data = bytearray(uc.mem_read(bufaddr, size))

    def rd(o):
        return int.from_bytes(data[o:o+4], "little")
    def wr(o, v):
        data[o:o+4] = (v & 0xFFFFFFFF).to_bytes(4, "little")

    wr(4, 0x80000000)                     # overall response: success
    off = 8
    while off + 12 <= len(data):
        tag = rd(off)
        if tag == 0:
            break
        vsize = rd(off + 4)
        if tag == 0x40001:                # allocate framebuffer -> base, size
            wr(off + 12, FB_PA)
            wr(off + 16, FB_SIZE)
            wr(off + 8, 0x80000008)
        elif tag == 0x40008:              # get pitch
            wr(off + 12, PITCH)
            wr(off + 8, 0x80000004)
        off += 12 + ((vsize + 3) & ~3)
    uc.mem_write(bufaddr, bytes(data))
    state["pending_read"] = value


def mbox_read(uc, offset, size, ud):
    if offset == OFF_STATUS:
        return 0                          # never full, never empty -> waits pass
    if offset == OFF_READ:
        return state["pending_read"]
    return 0


def mbox_write(uc, offset, size, value, ud):
    if offset == OFF_WRITE:
        service_mailbox(uc, value & 0xFFFFFFFF)


def timer_read(uc, offset, size, ud):
    if offset == OFF_CLO:
        state["clock"] = (state["clock"] + 20000) & 0xFFFFFFFF
        return state["clock"]
    return 0


def timer_write(uc, offset, size, value, ud):
    pass


def main():
    rom_path, out_path = sys.argv[1], sys.argv[2]
    budget = int(float(sys.argv[3]) * 1_000_000) if len(sys.argv) > 3 else 60_000_000

    data = open(rom_path, "rb").read()
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(RAM_BASE, RAM_SIZE, UC_PROT_ALL)
    uc.mem_map(FB_PA, FB_SIZE, UC_PROT_ALL)
    uc.mem_map(PERI_SINK_BASE, PERI_SINK_SIZE, UC_PROT_ALL)
    uc.mmio_map(TIMER_PAGE, 0x1000, timer_read, None, timer_write, None)
    uc.mmio_map(MBOX_PAGE, 0x1000, mbox_read, None, mbox_write, None)
    uc.mem_write(LOAD, data)
    uc.reg_write(UC_ARM64_REG_SP, 0x00200000)

    def on_unmapped(uc, access, address, size, value, ud):
        print("  !! unmapped access @ 0x%X (size %d) pc=0x%X"
              % (address, size, uc.reg_read(UC_ARM64_REG_PC)))
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    CHUNK = 2_000_000
    pc = LOAD
    ran = 0
    while ran < budget:
        try:
            uc.emu_start(pc, LOAD + RAM_SIZE, count=CHUNK)
        except Exception as e:
            print("  (stopped at ~%dM: %s)" % (ran // 1_000_000, e))
            break
        pc = uc.reg_read(UC_ARM64_REG_PC)
        ran += CHUNK

    fb = uc.mem_read(FB_PA, W * H * 4)
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
