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
import sys, struct, zlib, math
from unicorn import (Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE)
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_PC

W, H = 640, 480
PITCH = W * 4
LOAD     = 0x80000
RAM_BASE = 0x80000
RAM_SIZE = 0x400000           # kernel + stack + vars + mailbox buffer + fbinfo
FB_PA    = 0x08000000
FB_SIZE  = (W * H * 4 + 0xFFFF) & ~0xFFFF
PERI_SINK_A_BASE = 0x3F100000  # clock + GPIO writes land here (ignored)
PERI_SINK_A_SIZE = 0x00101000  # [0x3F100000, 0x3F201000)
UART_PAGE  = 0x3F201000        # PL011 (input) — emulated
PERI_SINK_B_BASE = 0x3F202000  # PWM writes land here (ignored)
PERI_SINK_B_SIZE = 0x000FE000  # [0x3F202000, 0x3F300000)
TIMER_PAGE = 0x3F003000
MBOX_PAGE  = 0x3F00B000
OFF_UART_DR = 0x3F201000 - UART_PAGE   # 0x00
OFF_UART_FR = 0x3F201018 - UART_PAGE   # 0x18

MBOX_READ_OFF   = 0x880 - 0x000   # within MBOX_PAGE (0x3F00B000) -> 0xB880-0xB000
# offsets within MBOX_PAGE (page base 0x3F00B000)
OFF_READ   = 0x3F00B880 - MBOX_PAGE
OFF_STATUS = 0x3F00B898 - MBOX_PAGE
OFF_WRITE  = 0x3F00B8A0 - MBOX_PAGE
OFF_CLO    = 0x3F003004 - TIMER_PAGE

PWM_CTL    = 0x3F20C000        # PWEN1|MSEN1 (0x81) enables the M/S square wave
PWM_RNG1   = 0x3F20C010        # period; output freq = PWM_CLK / RNG1
PWM_CLK_HZ = 9600000           # 19.2 MHz oscillator / DIVI=2 (matches the kernel)
WAV_RATE   = 8000              # WAV sample rate for the reconstruction
FPS        = 60

# ---- DWC2 USB host model (matches kernel.s USB_BASE block) ------------------
USB_PAGE   = 0x3F980000        # DWC2 register block (PERIPH + 0x980000)
USB_SIZE   = 0x10000
OFF_GRSTCTL = 0x010
OFF_HPRT    = 0x440
OFF_HC0CHAR = 0x500
OFF_HC0SPLT = 0x504
OFF_HC0INT  = 0x508
OFF_HC0TSIZ = 0x510
OFF_HC0DMA  = 0x514

# Descriptors the modelled hub + keyboard hand back during enumeration.
HUB_DEV = bytes([18,1, 0x00,0x02, 9,0,1, 64, 0x24,0x04, 0x14,0x95,
                 0,0, 0,0,0, 1])
HUB_CFG = bytes([9,2, 25,0, 1,1,0, 0xE0,0,           # config (25 total)
                 9,4, 0,0,1, 9,0,0, 0,               # interface (hub class 9)
                 7,5, 0x81,3, 1,0, 12])              # endpoint (intr IN ep1)
HUB_DESC = bytes([9,0x29, 5, 0,0, 0, 0, 0xFF, 0])    # 5 downstream ports
KBD_DEV = bytes([18,1, 0x00,0x02, 0,0,0, 8, 0x6d,0x04, 0x1a,0xc0,
                 0,1, 1,2,0, 1])
KBD_CFG = bytes([9,2, 34,0, 1,1,0, 0xA0,50,          # config (34 total)
                 9,4, 0,0,1, 3,1,1, 0,               # interface (HID boot kbd)
                 9,0x21, 0x11,0x01, 0, 1, 0x22, 63,0,# HID descriptor
                 7,5, 0x81,3, 8,0, 10])              # endpoint (intr IN ep1 mps8)
USB_USAGE = {"w":0x1A, "a":0x04, "s":0x16, "d":0x07,
             "\r":0x28, "\n":0x28, " ":0x2C, "<":0x2A}

state = {"pending_read": 0, "clock": 0, "keys": b"", "kidx": 0, "kcool": 0,
         "clo_reads": 0, "audio": [],     # audio: list of (frame, "freq"|"off", value)
         "usbkeys": [], "uki": 0, "ukcool": 0, "setup": None, "usbreg": {}}


NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(freq):
    if freq <= 0:
        return "rest"
    midi = round(69 + 12 * math.log2(freq / 440.0))
    return "%s%d" % (NOTE_NAMES[midi % 12], midi // 12 - 1)


MAX_AUDIO_EVENTS = 48                  # ~one pass of the 30-note tune (then it loops)


def audio_write_hook(uc, access, address, size, value, ud):
    if len(state["audio"]) >= MAX_AUDIO_EVENTS:
        return
    frame = state["clo_reads"] // 2
    if address == PWM_RNG1 and value > 0:
        state["audio"].append((frame, "freq", PWM_CLK_HZ / value))
    elif address == PWM_CTL and (value & 0x1) == 0:
        state["audio"].append((frame, "off", 0))


def write_wav(path, samples):
    pcm = b"".join(struct.pack("<h", max(-32767, min(32767, int(s)))) for s in samples)
    with open(path, "wb") as f:
        f.write(b"RIFF"); f.write(struct.pack("<I", 36 + len(pcm))); f.write(b"WAVE")
        f.write(b"fmt "); f.write(struct.pack("<IHHIIHH", 16, 1, 1, WAV_RATE,
                                              WAV_RATE * 2, 2, 16))
        f.write(b"data"); f.write(struct.pack("<I", len(pcm))); f.write(pcm)


def reconstruct_audio(path):
    """Turn the captured PWM-register timeline into a square-wave WAV and a note list.
    Each note spans from its RNG1 write to the next event; freq = PWM_CLK / RNG1."""
    ev = state["audio"]
    if not ev:
        print("  (no audio captured)")
        return []
    end_frame = ev[-1][0] + 16
    segs = []
    for i, (frame, kind, val) in enumerate(ev):
        nxt = ev[i + 1][0] if i + 1 < len(ev) else end_frame
        dur_s = max(0.0, (nxt - frame) / FPS)
        segs.append((val if kind == "freq" else 0.0, dur_s))
    samples = []
    phase = 0.0
    for freq, dur_s in segs:
        n = int(dur_s * WAV_RATE)
        if freq <= 0:
            samples.extend([0] * n)
            continue
        step = freq / WAV_RATE
        for _ in range(n):
            phase = (phase + step) % 1.0
            samples.append(7000 if phase < 0.5 else -7000)
    write_wav(path, samples)
    notes = [(f, note_name(f), round(d, 2)) for f, d in segs if f > 0]
    print("  audio: %d notes, %.1fs -> %s" % (len(notes), len(samples) / WAV_RATE, path))
    print("  tune:  " + " ".join(n for _, n, _ in notes))
    return notes


def uart_read(uc, offset, size, ud):
    """Emulate the PL011 RX so a scripted key sequence reaches the real driver.
    A key is presented for one poll, then a few empty polls (so edges register)."""
    if offset == OFF_UART_FR:
        if state["kcool"] > 0:
            state["kcool"] -= 1
            return 0x10                    # RXFE set: empty
        if state["kidx"] < len(state["keys"]):
            return 0                       # a byte is due (RXFE clear)
        return 0x10
    if offset == OFF_UART_DR:
        if state["kidx"] < len(state["keys"]):
            b = state["keys"][state["kidx"]]
            state["kidx"] += 1
            state["kcool"] = 4             # empty frames before the next key
            return b
        return 0
    return 0


def uart_write(uc, offset, size, value, ud):
    pass


def usb_response(setup, split):
    """Build the control-IN data the modelled device returns for this SETUP.
    `split` (HCSPLT enabled) means the transfer targets the keyboard downstream
    of the hub; otherwise it targets the hub itself (root device)."""
    bmreq, breq, wval, widx = setup
    desc_type = wval >> 8
    if breq == 6:                          # GET_DESCRIPTOR
        if desc_type == 1:                 # DEVICE
            return KBD_DEV if split else HUB_DEV
        if desc_type == 2:                 # CONFIGURATION (+ following descriptors)
            return KBD_CFG if split else HUB_CFG
        if desc_type == 0x29:              # HUB class descriptor
            return HUB_DESC
    if breq == 0 and bmreq == 0xA3:        # GET_STATUS on a hub port
        # connected + enabled + powered, full speed (bit9 low-speed clear)
        return bytes([0x03, 0x01, 0x00, 0x00])
    return b""


def usb_hid_report():
    """One 8-byte HID boot report from the scripted USB key sequence. Each key is
    presented for one poll then released for a few (so nav edges register)."""
    rpt = bytearray(8)
    if state["ukcool"] > 0:
        state["ukcool"] -= 1               # release frame
    elif state["uki"] < len(state["usbkeys"]):
        rpt[2] = state["usbkeys"][state["uki"]]
        state["uki"] += 1
        state["ukcool"] = 3
    return bytes(rpt)


def usb_service_channel(uc):
    """A host channel was enabled (HCCHAR CHENA written): carry out the transfer
    the channel registers describe, then post XFRC so the driver's poll completes."""
    r = state["usbreg"]
    char = r.get(OFF_HC0CHAR, 0)
    tsiz = r.get(OFF_HC0TSIZ, 0)
    dma  = r.get(OFF_HC0DMA, 0) & 0x3FFFFFFF     # strip the bus alias -> ARM phys
    hcsplt = r.get(OFF_HC0SPLT, 0)
    splt  = (hcsplt >> 31) & 1
    compl = (hcsplt >> 16) & 1
    dir_in  = (char >> 15) & 1
    eptype  = (char >> 18) & 3
    pid     = (tsiz >> 29) & 3
    xfersize = tsiz & 0x7FFFF
    if splt and not compl:
        # START-SPLIT phase: the hub just ACKs; the payload moves on the CSPLIT.
        r[OFF_HC0INT] = 0x20               # ACK
        r[OFF_HC0CHAR] = char & ~0x80000000
        return
    if eptype == 0:                        # control
        if pid == 3:                       # SETUP stage: latch the request
            pkt = uc.mem_read(dma, 8)
            state["setup"] = (pkt[0], pkt[1], pkt[2] | (pkt[3] << 8),
                              pkt[4] | (pkt[5] << 8))
        elif dir_in and xfersize > 0 and state["setup"] is not None:
            resp = usb_response(state["setup"], splt)[:xfersize]
            if resp:
                uc.mem_write(dma, bytes(resp))
    elif eptype == 3 and dir_in:           # interrupt IN: deliver a HID report
        uc.mem_write(dma, usb_hid_report())
    r[OFF_HC0INT] = 0x21                    # XFRC | ACK
    r[OFF_HC0CHAR] = char & ~0x80000000    # clear CHENA (channel done)


def usb_read(uc, offset, size, ud):
    if offset == OFF_GRSTCTL:
        return 0x80000000                  # AHBIDLE set, CSFTRST clear (reset done)
    if offset == OFF_HPRT:
        return 0x00001005                  # PWR | ENA | CONNSTS, high speed (spd=0)
    return state["usbreg"].get(offset, 0)


def usb_write(uc, offset, size, value, ud):
    state["usbreg"][offset] = value & 0xFFFFFFFF
    if offset == OFF_HC0CHAR and (value & 0x80000000):
        usb_service_channel(uc)


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
        state["clo_reads"] += 1            # ~2 CLO reads per wait_vblank -> per frame
        state["clock"] = (state["clock"] + 20000) & 0xFFFFFFFF
        return state["clock"]
    return 0


def timer_write(uc, offset, size, value, ud):
    pass


KEYMAP = {"w": b"w", "a": b"a", "s": b"s", "d": b"d",
          "\r": b"\r", "\n": b"\r", " ": b" ", "<": b"\x08"}


def parse_keys(argv):
    """Pull --keys=SEQ (serial input injection) and --audio=PATH (reconstruct the
    PWM tone to a WAV) out of argv."""
    rest, seq, wav, usb = [], b"", None, []
    for a in argv:
        if a.startswith("--keys="):
            for ch in a[len("--keys="):]:
                seq += KEYMAP.get(ch, ch.encode("latin-1"))
        elif a.startswith("--usbkeys="):
            for ch in a[len("--usbkeys="):]:
                usb.append(USB_USAGE.get(ch, 0))
        elif a.startswith("--audio="):
            wav = a[len("--audio="):]
        else:
            rest.append(a)
    return seq, wav, usb, rest


FS_PA   = 0x340000            # USV1 disk region (kernel fs.inc.s FSBASE)
FS_SIZE = 0x1000

def main():
    keys, wav, usbkeys, argv = parse_keys(sys.argv)
    rom_path, out_path = argv[1], argv[2]
    # optional "--storage=PATH": a persisted SD-card image for the USV1 disk
    storage, tail = None, []
    for a in argv[3:]:
        if a.startswith("--storage="):
            storage = a.split("=", 1)[1]
        else:
            tail.append(a)
    budget = int(float(tail[0]) * 1_000_000) if tail else 60_000_000
    state["keys"] = keys
    state["usbkeys"] = usbkeys

    data = open(rom_path, "rb").read()
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(RAM_BASE, RAM_SIZE, UC_PROT_ALL)
    uc.mem_map(FB_PA, FB_SIZE, UC_PROT_ALL)
    uc.mem_map(PERI_SINK_A_BASE, PERI_SINK_A_SIZE, UC_PROT_ALL)
    uc.mem_map(PERI_SINK_B_BASE, PERI_SINK_B_SIZE, UC_PROT_ALL)
    uc.mmio_map(UART_PAGE, 0x1000, uart_read, None, uart_write, None)
    uc.mmio_map(TIMER_PAGE, 0x1000, timer_read, None, timer_write, None)
    uc.mmio_map(MBOX_PAGE, 0x1000, mbox_read, None, mbox_write, None)
    uc.mmio_map(USB_PAGE, USB_SIZE, usb_read, None, usb_write, None)
    uc.mem_write(LOAD, data)
    uc.reg_write(UC_ARM64_REG_SP, 0x00200000)

    # preload the persisted USV1 disk so fs_init keeps it ("power on" with an SD card)
    import os
    if storage and os.path.exists(storage):
        with open(storage, "rb") as f:
            uc.mem_write(FS_PA, f.read()[:FS_SIZE])

    def on_unmapped(uc, access, address, size, value, ud):
        print("  !! unmapped access @ 0x%X (size %d) pc=0x%X"
              % (address, size, uc.reg_read(UC_ARM64_REG_PC)))
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    if wav:
        uc.hook_add(UC_HOOK_MEM_WRITE, audio_write_hook, None, PWM_CTL, PWM_RNG1 + 4)

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
    if storage:                          # flush the USV1 disk back ("SD write-back")
        with open(storage, "wb") as f:
            f.write(bytes(uc.mem_read(FS_PA, FS_SIZE)))
        print("flushed FS -> %s" % storage)
    if wav:
        reconstruct_audio(wav)


if __name__ == "__main__":
    main()
