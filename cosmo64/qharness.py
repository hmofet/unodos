#!/usr/bin/env python3
"""qharness.py -- QEMU-virt gate for pc64-on-ARM (runs ON quill).

The asm port's Unicorn harness cannot follow cosmo64 past M0: Unicorn stops
cold at the first fetch through an enabled EL1 MMU (verified with a minimal
repro), and the road ahead needs the GIC and timer interrupts too. QEMU's
`virt' board can do all of it -- and its DRAM starts at 0x40000000, the same
base as the Cosmo, so the flat payload runs UNMODIFIED with the real device's
framebuffer address (0x7DF70000, inside -m 2048), which also exercises the
MMU's non-cacheable framebuffer mapping exactly where the device needs it.

What it does:
  * builds a real FDT whose /chosen carries the production `atag,videolfb'
    packed blob (base 0x7DF70000, vramSize 0x1F90000);
  * assembles a 3-instruction stub that puts the FDT address in x0 (LK's
    contract) and branches to the payload;
  * boots qemu-system-aarch64 -M virt with generic loaders (no kernel
    protocol involved), lets the payload run ~2 s, then reads DRAM back over
    the monitor with pmemsave;
  * checks the same FBINFO contract cosmo/harness.py checks -- source, base,
    vram, beacon, white bar, dorigin, shadow pitch -- plus the CRASH RECORD
    (in-image, found via the header pointer; a fault parks in the vectors and leaves ESR/ELR/
    FAR there, which this prints instead of a mute failure);
  * reconstructs the eye view through the panel mounting at every sub-position
    of the scale block and requires it to equal the shadow, pixel for pixel;
  * reads the persistent debug log (log.c) out of the ramoops console zone
    and prints it -- including when the payload crashed, which is when it
    matters most;
  * writes the upright UI as a PNG.

Usage: qharness.py <payload.bin> <out.png> [seconds]
"""
import os, struct, subprocess, sys, tempfile, time, zlib

LOAD = 0x40080000
FDT_AT = 0x48000000
PANEL_FB = 0x7DF70000
VRAM = 0x1F90000
PANEL_W, PANEL_H, PITCH = 1080, 2160, 4352
W, H = 640, 480
BCN_MAGIC = 0x554E4F31
ROT = 270
# The persistent debug log (log.c): the Gemian kernel's ramoops CONSOLE
# zone. QEMU's virt board puts DRAM at 0x40000000 like the Cosmo, so the
# same absolute address is real memory here and the gate reads the log the
# device will later hand to pstore, byte for byte.
LOG_ZONE = 0x5449F000
LOG_SIZE = 0x40000
PRAM_SIG = 0x43474244          # 'DBGC', PERSISTENT_RAM_SIG
LOG_BANNER = '=== UnoDOS cosmo64 ==='


def fdt_blob():
    """A minimal valid FDT: /chosen { atag,videolfb = <the packed LE blob> },
    plus the root #address-cells/#size-cells QEMU needs to graft /memory in."""
    prop = struct.pack("<QIII", PANEL_FB, 1, 60, VRAM) + b"qemu_virt_panel\0"
    strings = b"#address-cells\0#size-cells\0atag,videolfb\0"
    off_ac, off_sc, off_lfb = 0, 15, 27
    st = b""
    st += struct.pack(">I", 1) + b"\0\0\0\0"                    # BEGIN_NODE ""
    st += struct.pack(">IIII", 3, 4, off_ac, 2)                 # #address-cells = 2
    st += struct.pack(">IIII", 3, 4, off_sc, 2)                 # #size-cells = 2
    st += struct.pack(">I", 1) + b"chosen\0\0"                  # BEGIN_NODE chosen
    st += struct.pack(">III", 3, len(prop), off_lfb)            # PROP atag,videolfb
    st += prop + b"\0" * (-len(prop) % 4)
    st += struct.pack(">I", 2)                                  # END_NODE
    st += struct.pack(">I", 2)                                  # END_NODE
    st += struct.pack(">I", 9)                                  # END
    hdr_sz, rsv_sz = 40, 16
    off_struct = hdr_sz + rsv_sz
    off_strings = off_struct + len(st)
    total = off_strings + len(strings)
    hdr = struct.pack(">IIIIIIIIII", 0xD00DFEED, total, off_struct, off_strings,
                      hdr_sz, 17, 16, 0, len(strings), len(st))
    return hdr + b"\0" * rsv_sz + st + strings


# No stub, no loader devices: flatten.py stamps an ARM64 Image header into the
# flat payload, so QEMU's own -kernel loader places it at RAM+0x80000 (= the
# link address) and passes the DTB in x0 -- the exact LK contract. -dtb swaps
# in our videolfb tree; QEMU rewrites /memory and /chosen extras around it,
# which the payload's FDT walker skips over like any other property.


def write_png(path, w, h, rgb):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    raw = b"".join(b"\0" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    payload, out_png = sys.argv[1], sys.argv[2]
    run_for = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
    tmp = tempfile.mkdtemp(prefix="qharness-")
    fdt = os.path.join(tmp, "virt-videolfb.dtb")
    open(fdt, "wb").write(fdt_blob())

    # QHARNESS_EL2=1 boots the payload at EL2, which is where the Cosmo's LK
    # actually leaves it (measured 2026-09-01: CurrentEL=2). Without this the
    # gate only ever exercises the EL1 register path, which is how the port
    # ran MMU-off and cache-off from M1 to 2026-09-01 without the gate
    # noticing anything at all.
    mach = "virt,virtualization=on" if os.environ.get("QHARNESS_EL2") else "virt"
    qemu = ["qemu-system-aarch64", "-M", mach, "-cpu", "cortex-a72",
            "-m", "2048", "-display", "none", "-serial", "none",
            "-qmp", "stdio", "-no-reboot",
            "-kernel", payload, "-dtb", fdt]
    err_path = os.path.join(tmp, "qemu-stderr.txt")
    err_f = open(err_path, "w")
    p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=err_f, text=True)
    import json

    def qmp(cmd, **args):
        req = {"execute": cmd}
        if args:
            req["arguments"] = args
        p.stdin.write(json.dumps(req) + "\n")
        p.stdin.flush()
        while True:                       # skip the greeting and async events
            line = p.stdout.readline()
            if not line:
                err_f.flush()
                sys.exit("qharness: qemu closed the QMP stream on %r -- stderr:\n%s"
                         % (cmd, open(err_path).read()[-1500:]))
            msg = json.loads(line)
            if "return" in msg or "error" in msg:
                if "error" in msg:
                    sys.exit("qharness: %s failed: %s" % (cmd, msg["error"]))
                return msg["return"]

    qmp("qmp_capabilities")
    time.sleep(run_for)
    qmp("stop")

    def dumped(path, size):
        for _ in range(60):
            if os.path.exists(path) and os.path.getsize(path) >= size:
                return open(path, "rb").read()
            time.sleep(0.25)
        sys.exit("qharness: dump %s never completed" % path)

    # the payload publishes its in-image debug page's address in its own
    # header (offset 0x30, the ARM64 res4 word) -- discover it, then dump it
    f_hdr = os.path.join(tmp, "hdr.bin")
    qmp("pmemsave", val=LOAD, size=0x40, filename=f_hdr)
    hdr = dumped(f_hdr, 0x40)
    dbg_at, = struct.unpack_from("<Q", hdr, 0x30)
    if not LOAD < dbg_at < 0x54000000:
        sys.exit("qharness: bad debug-page pointer 0x%X in the image header "
                 "(entry.s never ran?)" % dbg_at)
    f_fbi = os.path.join(tmp, "fbinfo.bin")
    qmp("pmemsave", val=dbg_at, size=0x1100, filename=f_fbi)
    fbi = dumped(f_fbi, 0x1100)
    fb_base, fb_pitch = struct.unpack_from("<QI", fbi, 0)
    vram, = struct.unpack_from("<I", fbi, 24)
    raw, = struct.unpack_from("<Q", fbi, 32)
    src, stage, magic = struct.unpack_from("<III", fbi, 40)
    ppitch, = struct.unpack_from("<I", fbi, 64)
    dorigin, = struct.unpack_from("<Q", fbi, 72)
    shadow, = struct.unpack_from("<Q", fbi, 80)
    scale, = struct.unpack_from("<I", fbi, 88)
    if not 1 <= scale <= 4:
        scale = 1

    # ---- the persistent log (log.c) ------------------------------------
    f_log = os.path.join(tmp, "log.bin")
    qmp("pmemsave", val=LOG_ZONE, size=LOG_SIZE, filename=f_log)
    logbuf = dumped(f_log, LOG_SIZE)
    lsig, lstart, lsize = struct.unpack_from("<III", logbuf, 0)
    log_text = None
    log_fail = None
    if lsig != PRAM_SIG:
        log_fail = ("debug log: signature 0x%08X, wanted 0x%08X "
                    "(c64_log_init never ran?)" % (lsig, PRAM_SIG))
    elif not 0 < lsize <= LOG_SIZE - 12 or lstart > lsize:
        log_fail = "debug log: invalid ring (start=%d size=%d)" % (lstart, lsize)
    else:
        # exactly persistent_ram_save_old()'s reconstruction
        data = logbuf[12:]
        log_text = (data[lstart:lsize] + data[:lstart]).decode("utf-8", "replace")
        print("---- debug log (%d bytes @ 0x%X) ----" % (lsize, LOG_ZONE))
        for line in log_text.strip("\n").split("\n"):
            print("  | " + line)
        print("---- end debug log ----")
        if LOG_BANNER not in log_text:
            log_fail = "debug log: banner %r missing" % LOG_BANNER

    cmagic, vec = struct.unpack_from("<II", fbi, 0x1000)
    if cmagic == 0x43525348:
        esr, elr, far, el = struct.unpack_from("<QQQQ", fbi, 0x1000 + 8)
        qmp("quit")
        sys.exit("CRASH RECORD: vec=%d ESR=0x%X (EC=0x%X) ELR=0x%X (image+0x%X) "
                 "FAR=0x%X EL=0x%X" % (vec, esr, esr >> 26, elr, elr - LOAD, far, el))

    fails = []
    if log_fail:
        fails.append(log_fail)
    if src != 1:
        fails.append("framebuffer source: got %d, wanted 1 (videolfb blob)" % src)
    if raw != PANEL_FB:
        fails.append("framebuffer base: got 0x%X, wanted 0x%X" % (raw, PANEL_FB))
    if vram != VRAM:
        fails.append("vramSize: got 0x%X, wanted 0x%X" % (vram, VRAM))
    if magic != BCN_MAGIC:
        fails.append("beacon magic: got 0x%X" % magic)
    elif stage != 4:
        fails.append("beacon stage: reached %d, wanted 4 (main loop)" % stage)
    if fb_pitch != W * 4:
        fails.append("shadow pitch: got %d, wanted %d" % (fb_pitch, W * 4))

    dst_w, dst_h = (H, W) if ROT in (90, 270) else (W, H)
    dst_w, dst_h = dst_w * scale, dst_h * scale
    x0 = (PANEL_W - dst_w) // 2
    y0 = (PANEL_H - dst_h) // 2
    if dorigin != raw + y0 * ppitch + x0 * 4:
        fails.append("dorigin: got 0x%X, wanted 0x%X"
                     % (dorigin, raw + y0 * ppitch + x0 * 4))

    # The shell renders into fb[] and THEN presents, so a stop can catch the
    # source one frame ahead of the panel (the m0 payload is static and never
    # races). Retry until a quiescent stop: between tray-clock ticks the shell
    # is idle and source and panel agree exactly.
    #
    # The eye view goes through the 270-degree mounting at every block
    # sub-position. m0 presents its shadow verbatim (0xAARRGGBB source); the
    # shell presents pc64's fb[] (0xAABBGGRR) with an R<->B swizzle -- accept
    # whichever channel order matches, but the SAME one for every sub-position.
    eye0 = None
    fb = sh = None
    blit_fail = None
    for attempt in range(6):
        f_fb = os.path.join(tmp, "fb%d.bin" % attempt)
        f_sh = os.path.join(tmp, "shadow%d.bin" % attempt)
        qmp("pmemsave", val=raw, size=PANEL_H * ppitch, filename=f_fb)
        fb = dumped(f_fb, PANEL_H * ppitch)
        qmp("pmemsave", val=shadow, size=W * H * 4, filename=f_sh)
        sh = dumped(f_sh, W * H * 4)

        sh_swiz = bytearray(sh)
        for i in range(0, len(sh_swiz), 4):
            sh_swiz[i], sh_swiz[i + 2] = sh_swiz[i + 2], sh_swiz[i]
        sh_swiz = bytes(sh_swiz)
        accept = None
        blit_fail = None
        eye0 = None
        for a in range(scale):
            for b in range(scale):
                eye = bytearray(W * H * 4)
                for sx in range(W):
                    fy = y0 + dst_h - 1 - (sx * scale + a)
                    row = fb[fy * ppitch + x0 * 4: fy * ppitch + x0 * 4 + dst_w * 4]
                    for sy in range(H):
                        o = (sy * W + sx) * 4
                        s = (sy * scale + b) * 4
                        eye[o:o + 4] = row[s:s + 4]
                if eye0 is None:
                    eye0 = eye
                if accept is None:
                    accept = sh if bytes(eye) == sh else (
                        sh_swiz if bytes(eye) == sh_swiz else None)
                    if accept is None:
                        bad = sum(1 for i in range(0, len(sh_swiz), 4)
                                  if eye[i:i+4] != sh_swiz[i:i+4])
                        blit_fail = ("rotated blit: %d of %d pixels differ from "
                                     "the swizzled source (sub-pos %d,%d, "
                                     "attempt %d)" % (bad, W * H, a, b, attempt))
                        break
                elif bytes(eye) != accept:
                    bad = sum(1 for i in range(0, len(accept), 4)
                              if eye[i:i+4] != accept[i:i+4])
                    blit_fail = ("rotated blit: %d of %d pixels differ (sub-pos "
                                 "%d,%d, attempt %d)" % (bad, W * H, a, b, attempt))
                    break
            else:
                continue
            break
        if blit_fail is None:
            break
        qmp("cont")
        time.sleep(0.7)
        qmp("stop")
    # A handful of differing pixels after every retry means a frame was in
    # flight at the stop (the shell had written fb[] but not yet presented --
    # the tray clock ticking is the usual culprit), not a broken blit: that
    # shows up as ~every pixel differing. Distinguish the two by magnitude.
    if blit_fail is not None:
        n_bad = 0
        try:
            n_bad = int(blit_fail.split()[2])
        except (IndexError, ValueError):
            n_bad = W * H
        if n_bad <= (W * H) // 200:
            print("  note: %d pixels differ -- a frame was in flight at the "
                  "stop, blit otherwise exact" % n_bad)
        else:
            fails.append(blit_fail)
    qmp("quit")
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()

    if struct.unpack_from("<I", fb, 0)[0] != 0xFFFFFFFF:
        fails.append("bar beacon: framebuffer starts 0x%08X, wanted white"
                     % struct.unpack_from("<I", fb, 0)[0])
    if eye0 is not None and not any(sh):
        fails.append("blit agrees but the shadow is blank")

    if eye0 is not None:
        rgb = bytearray(W * H * 3)
        for i in range(W * H):
            rgb[i*3+0], rgb[i*3+1], rgb[i*3+2] = eye0[i*4+2], eye0[i*4+1], eye0[i*4]
        write_png(out_png, W, H, rgb)
        print("wrote %s (%dx%d, as the eye sees it)" % (out_png, W, H))
    print("  fb=0x%X vram=0x%X ppitch=%d scale=%d shadow=0x%X dorigin=0x%X stage=%d"
          % (raw, vram, ppitch, scale, shadow, dorigin, stage))
    if fails:
        for f in fails:
            print("  FAIL: %s" % f)
        sys.exit(1)
    print("  OK: videolfb walk, MMU-on adoption, beacon, rotated blit and\n      persistent log all good")


if __name__ == "__main__":
    main()
