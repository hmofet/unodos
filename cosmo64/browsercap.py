#!/usr/bin/env python3
"""browsercap.py -- drive the pc64 browser on aarch64 and photograph it
(runs ON quill).

qharness.py's QHARNESS_LAUNCH gate proves the browser OPENS: the verb answers,
the guest lives, the screen changed. That is the right thing for a merge gate
and it is not enough to believe a renderer. This walks the browser through its
own built-in pages with the keyboard and saves a full-resolution grab of each,
because the interesting failures here are all visual -- a layout that computes
but paints nothing, a script that runs and writes into a document nobody draws,
a font metric that is right on x86 and wrong under -mstrict-align.

The virt board has NO USB and therefore no NIC (qharness.py says so at its
serial setup), so nothing here fetches over the network: these are the `uno:`
pages the browser generates itself, which exercise the whole pipeline above the
socket -- HTML parse, the cascade, layout, paint, and unojs on the script page.
HTTP is proven on hardware, where there is an adapter.

    python3 browsercap.py <payload.bin> [outdir] [loc,loc,...]

Default stops: uno:start, uno:sample, uno:script, uno:engine. Writes
<outdir>/browser-<stem>.png per stop. NOT part of the merge gate (qharness.py
is): this is how a change to the browser, unoweb or unojs is eyeballed without
a flash.

TYPE THE LOCATION, DO NOT OPEN THE ADDRESS BAR FIRST. x86's browser harnesses
(pc64/tools/browser_engine_urc.py) go Ctrl-L, End, 32 x Backspace, then the
text -- 45 round trips, each of which the guest answers on a frame boundary.
That is fine on a PC and it is not fine here: under QEMU this payload runs at
about 2 presents a second with a browser window up, and 45 verbs a stop took
longer than the whole run was allowed. pc64_browser.c's own key handler makes
it unnecessary -- a printable character arriving while the address bar is NOT
focused focuses it AND clears it (the UI_EV_CHAR case) -- so typing the
location outright is 12 verbs and lands in an empty field.
"""
import json, os, socket, struct, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qharness import fdt_blob, write_png, LOAD, LOG_ZONE, LOG_SIZE, PRAM_SIG

# The default walk. The last three are the point of the browser slice: switch
# the renderer to unoweb and re-render the SAME sample page, so the run covers
# both painters -- the flow painter (a direct walk of the tree, the default)
# and the full pipeline (cascade, block layout, display list). A stop is
# numbered in its filename, so the same location can appear twice.
STOPS = ["uno:start", "uno:sample", "uno:script", "uno:engine",
         "uno:engine/render/unoweb", "uno:sample", "uno:script"]


def _wait_conn(port, secs=25):
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
    sys.exit("browsercap: nothing listening on 127.0.0.1:%d" % port)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    payload = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else "."
    stops = sys.argv[3].split(",") if len(sys.argv) > 3 else STOPS
    from unoauto_remote import UnoAutoLink

    tmp = tempfile.mkdtemp(prefix="browsercap-")
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
    bad = 0

    def postmortem(why):
        """A guest that stops answering URC has told us nothing yet. The
        payload keeps its whole story in guest RAM -- the crash record in its
        in-image debug page, the log ring in the Gemian ramoops console zone
        -- and QMP can read both with the guest wedged, which is the entire
        reason qharness.py reads them the same way. Never guess at a dead box
        that can still be photographed."""
        print("---- post-mortem: %s ----" % why)
        try:
            q = _wait_conn(qport, 10); qf = q.makefile("rwb")
            qf.readline()
            for cmd in ('{"execute":"qmp_capabilities"}',):
                qf.write((cmd + chr(10)).encode()); qf.flush(); qf.readline()

            def qmp(cmd, **a):
                req = {"execute": cmd}
                if a:
                    req["arguments"] = a
                qf.write((json.dumps(req) + chr(10)).encode()); qf.flush()
                while True:
                    line = qf.readline()
                    if not line:
                        return {}
                    r = json.loads(line.decode())
                    if "event" not in r:
                        return r

            def dump(addr, size, name):
                f = os.path.join(tmp, name)
                qmp("pmemsave", val=addr, size=size, filename=f)
                for _ in range(60):
                    if os.path.exists(f) and os.path.getsize(f) >= size:
                        return open(f, "rb").read()
                    time.sleep(0.25)
                return b""

            hdr = dump(LOAD, 0x40, "hdr.bin")
            if len(hdr) >= 0x38:
                dbg_at, = struct.unpack_from("<Q", hdr, 0x30)
                if LOAD < dbg_at < 0x54000000:
                    fbi = dump(dbg_at, 0x1100, "fbi.bin")
                    if len(fbi) >= 0x1030:
                        cmagic, vec = struct.unpack_from("<II", fbi, 0x1000)
                        if cmagic == 0x43525348:
                            esr, elr, far, el = struct.unpack_from("<QQQQ", fbi, 0x1008)
                            print("CRASH RECORD: vec=%d ESR=0x%X (EC=0x%X) "
                                  "ELR=0x%X (image+0x%X) FAR=0x%X EL=0x%X"
                                  % (vec, esr, esr >> 26, elr, elr - LOAD, far, el))
                        else:
                            print("no crash record (the guest did not fault)")
            lb = dump(LOG_ZONE, LOG_SIZE, "log.bin")
            if len(lb) >= 12:
                lsig, lstart, lsize = struct.unpack_from("<III", lb, 0)
                if lsig == PRAM_SIG and 0 < lsize <= LOG_SIZE - 12 and lstart <= lsize:
                    data = lb[12:]
                    text = (data[lstart:lsize] + data[:lstart]).decode("utf-8", "replace")
                    tail = text.strip(chr(10)).split(chr(10))[-40:]
                    print("---- last 40 log lines ----")
                    for line in tail:
                        print("  | " + line)
                else:
                    print("debug log unreadable (sig 0x%08X)" % lsig)
        except Exception as e:
            print("post-mortem failed: %s" % e)
        print("---- end post-mortem ----")
    try:
        link = UnoAutoLink()
        link.attach_stream(_wait_conn(uport))
        if not link.wait_hello(30):
            sys.exit("browsercap: no HELLO from the guest")

        slow = [0]

        def key(uni, scan=0, ctrl=0, settle=0.2):
            """A key verb answers on a frame boundary, and a frame here can be
            half a second. A timeout is therefore NOT death -- urcui.py's
            _inject says the same thing about the metal runs -- so count it
            and carry on; alive() is what decides the box is gone."""
            try:
                link.key(int(scan), int(uni), int(ctrl), timeout=20)
            except Exception:
                slow[0] += 1
            time.sleep(settle)

        r = link.launch("browser", timeout=20.0)
        print("browsercap: launch browser -> %s" % (" ".join(r) if r else "-"))
        time.sleep(3.0)

        prev = None
        for i, loc in enumerate(stops):
            t0 = time.time()
            # ESC FIRST, and it is not decoration. A printable character
            # focuses and CLEARS the address bar only when the bar is not
            # already focused -- and a restored session can come back with the
            # caret sitting in it (seen on the device: the bar held a stray
            # "d", so a typed location APPENDED and became dhttps://...,
            # which then failed DNS and read like a network fault). ESC
            # unfocuses, so the next character starts from empty.
            key(0, scan=0x17, settle=0.15)     # ESC (UEFI scan code)
            for ch in loc:                     # first char focuses AND clears
                key(ord(ch), settle=0.15)
            key(13)                            # Enter: navigate
            time.sleep(2.5)
            try:
                w, h, rgba = link.screen_grab(scale=1, timeout=90.0)
            except Exception as e:
                print("browsercap: %-12s NO GRAB (%s)" % (loc, e)); bad += 1
                if not link.alive():
                    postmortem("no screen grab and no uptime after %s" % loc)
                    break
                continue
            # Two numbers, and the second is the one that means anything.
            # "content" (pixels that are not the desktop colour) only says a
            # window is there. "changed" is against the PREVIOUS stop, which
            # is what catches the failure this harness exists for: a browser
            # that navigates, believes it navigated, and paints the same
            # pixels it painted before.
            bg = bytes(rgba[0:3])
            ink = sum(1 for k in range(0, len(rgba), 4) if bytes(rgba[k:k + 3]) != bg)
            rgb = bytes(b for k, b in enumerate(rgba) if k % 4 != 3)
            chg = -1 if prev is None else sum(1 for k in range(0, len(rgb), 3)
                                              if rgb[k:k + 3] != prev[k:k + 3])
            path = os.path.join(outdir, "browser-%02d-%s.png"
                                % (i + 1, loc.replace("uno:", "").replace("/", "-")))
            write_png(path, w, h, rgb)
            print("browsercap: %-26s %dx%d, %d px content, %s changed, %.0fs -> %s"
                  % (loc, w, h, ink,
                     "first" if chg < 0 else str(chg), time.time() - t0, path))
            if ink == 0:
                bad += 1
            if chg == 0:
                print("browsercap:   ^ IDENTICAL to the previous stop"); bad += 1
            prev = rgb
        if slow[0]:
            print("browsercap: %d key verbs timed out and were not re-sent "
                  "(a slow frame, not a lost keystroke)" % slow[0])
        if link.alive():
            print("browsercap: the guest still answers")
        else:
            print("browsercap: the guest stopped answering")
            postmortem("guest unresponsive at the end of the run")
            bad += 1
        link.close()
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except Exception:
            p.kill()
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
