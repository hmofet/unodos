#!/usr/bin/env python3
"""rncap.py -- capture a NATIVE FULLSCREEN app's panel over QMP (runs ON quill).

qharness.py drives the guest over URC and ends by reconstructing the panel to
check the blit. Neither fits a fullscreen native game: runner3d drops the
desktop to 540x270 and redraws every frame, which (a) makes the end-of-run
blit check compare against the wrong size, and (b) floods the one URC tx queue
with perf lines so a `screen grab` command's reply never surfaces. So a
fullscreen app cannot be screenshotted through the command channel at all.

This captures it the way CLAUDE.md's rule says to when a control channel
exists: straight off the panel. Boot QEMU virt (EL2, matching the device),
launch the app over URC fire-and-forget -- the launch verb answers before the
game takes the desktop -- then read the framebuffer with QMP `pmemsave` and
reconstruct the eye view from the payload's own published geometry (the debug
page qharness.py reads). No command reply is needed after the launch, so the
log flood is irrelevant.

    python3 rncap.py <payload.bin> <out.png> [app-id] [settle-seconds]

Default app is runner3d (the uno3d software rasteriser). It is NOT part of the
merge gate -- qharness.py is -- but it is how a change to uno3d or a native
game is eyeballed without a flash.
"""
import json, os, socket, struct, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qharness import fdt_blob, write_png          # geometry + PNG writer, shared

LOAD, PANEL_H, ROT = 0x40080000, 2160, 270
CRASH_MAGIC = 0x43525348                            # 'CRSH' (see qharness.py)


def _wait_conn(port, secs=20):
    end = time.time() + secs
    while time.time() < end:
        try:
            # settimeout(None) IS LOAD-BEARING. create_connection's timeout
            # stays ON the returned socket, and UnoAutoLink's reader thread
            # treats any OSError from recv() as the link closing -- and
            # socket.timeout IS an OSError. So a 2-second lull in a guest that
            # logs every 2 seconds silently kills the reader: no replies are
            # ever seen again, every verb times out, and the guest looks dead
            # while it is looping happily. Found 2026-09-04 driving the
            # browser; it had been luck, not health, up to then.
            s = socket.create_connection(("127.0.0.1", port), timeout=2)
            s.settimeout(None)
            return s
        except OSError:
            time.sleep(0.3)
    sys.exit("rncap: nothing listening on 127.0.0.1:%d" % port)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    payload, out_png = sys.argv[1], sys.argv[2]
    app = sys.argv[3] if len(sys.argv) > 3 else "runner3d"
    settle = float(sys.argv[4]) if len(sys.argv) > 4 else 4.0
    from unoauto_remote import UnoAutoLink

    tmp = tempfile.mkdtemp(prefix="rncap-")
    fdt = os.path.join(tmp, "v.dtb")
    open(fdt, "wb").write(fdt_blob())

    def free_port():
        s = socket.socket(); s.bind(("127.0.0.1", 0))
        n = s.getsockname()[1]; s.close(); return n
    uport, qport = free_port(), free_port()
    qemu = ["qemu-system-aarch64", "-M", "virt,virtualization=on",
            "-cpu", "cortex-a72", "-m", "2048", "-display", "none",
            "-serial", "tcp:127.0.0.1:%d,server,nowait" % uport,
            "-qmp", "tcp:127.0.0.1:%d,server,nowait" % qport,
            "-no-reboot", "-kernel", payload, "-dtb", fdt]
    p = subprocess.Popen(qemu, stderr=open(os.path.join(tmp, "err.txt"), "w"))
    try:
        link = UnoAutoLink()
        link.attach_stream(_wait_conn(uport))
        if not link.wait_hello(30):
            sys.exit("rncap: no HELLO from the guest")
        r = link.launch(app, timeout=20.0)          # answers before it fullscreens
        print("rncap: launch %s -> %s; settling %.1fs" %
              (app, " ".join(r) if r else "-", settle))
        time.sleep(settle)

        q = _wait_conn(qport); qf = q.makefile("rwb")

        def qmp(cmd, **a):
            req = {"execute": cmd}
            if a:
                req["arguments"] = a
            qf.write((json.dumps(req) + "\n").encode()); qf.flush()
            while True:
                m = json.loads(qf.readline())
                if "return" in m or "error" in m:
                    return m
        qf.readline(); qmp("qmp_capabilities"); qmp("stop")

        def dump(addr, size, name):
            f = os.path.join(tmp, name)
            qmp("pmemsave", val=addr, size=size, filename=f)
            for _ in range(60):
                if os.path.exists(f) and os.path.getsize(f) >= size:
                    return open(f, "rb").read()
                time.sleep(0.25)
            sys.exit("rncap: dump %s stalled" % name)

        # the payload publishes its debug-page address in its own image header
        # (offset 0x30), and geometry + a crash record live in that page --
        # exactly as qharness.py reads them
        dbg_at, = struct.unpack_from("<Q", dump(LOAD, 0x40, "hdr.bin"), 0x30)
        if not LOAD < dbg_at < 0x54000000:
            sys.exit("rncap: bad debug-page pointer 0x%X (entry.s never ran?)" % dbg_at)
        fbi = dump(dbg_at, 0x1100, "fbi.bin")
        raw, = struct.unpack_from("<Q", fbi, 32)
        ppitch, = struct.unpack_from("<I", fbi, 64)
        scale, scrw, scrh = struct.unpack_from("<III", fbi, 88)
        cmagic, vec = struct.unpack_from("<II", fbi, 0x1000)
        if cmagic == CRASH_MAGIC:
            esr, elr, far, el = struct.unpack_from("<QQQQ", fbi, 0x1008)
            sys.exit("rncap: CRASH vec=%d ESR=0x%X ELR=image+0x%X FAR=0x%X"
                     % (vec, esr, elr - LOAD, far))
        if not 1 <= scale <= 4:
            scale = 1
        print("rncap: geometry %dx%d scale %d ppitch %d raw 0x%X"
              % (scrw, scrh, scale, ppitch, raw))

        fb = dump(raw, PANEL_H * ppitch, "fb.bin")
        W, H = scrw, scrh
        dst_w, dst_h = (scrh, scrw) if ROT in (90, 270) else (scrw, scrh)
        dst_w, dst_h = dst_w * scale, dst_h * scale
        x0 = (1080 - dst_w) // 2
        y0 = (PANEL_H - dst_h) // 2
        rgb = bytearray(W * H * 3)          # eye view, sub-position (0,0)
        for sx in range(W):
            fy = y0 + dst_h - 1 - sx * scale
            row = fb[fy * ppitch + x0 * 4:]
            for sy in range(H):
                s = (sy * scale) * 4
                o = (sy * W + sx) * 3
                rgb[o], rgb[o + 1], rgb[o + 2] = row[s + 2], row[s + 1], row[s]  # BGRA->RGB
        write_png(out_png, W, H, rgb)
        bg = (b"\xc8\xc8\xc8", b"\x00\x00\x00")
        nonbg = sum(1 for i in range(0, len(rgb), 3) if rgb[i:i + 3] not in bg)
        print("rncap: wrote %s (%dx%d); %d of %d px non-background"
              % (out_png, W, H, nonbg, W * H))
        qmp("quit"); link.close()
    finally:
        p.kill()


if __name__ == "__main__":
    main()
