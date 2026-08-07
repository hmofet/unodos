#!/usr/bin/env python3
"""scenes - the demo-video scene driver (see SCENES.md).

Boots the DEBUG image ONCE in QEMU (the same OVMF + SLIRP harness the other
gates use), then per requested scene: starts a fresh stream_recv on its own
host port, has the guest dial out (`stream start 10.0.2.2 <port> 30`), plays
the scene's beat list with deliberate video pacing (pointer travel is a glide
of many small moves, ~0.5-1.0 s settles between beats), then `stream stop`.

    python3 scenes.py --scene s02          one scene
    python3 scenes.py --all                the whole spine, one boot
    python3 scenes.py --list               what exists

Per scene, into out/:
    s<NN>.mp4            the recording (stream_recv -> ffmpeg)
    s<NN>.timing.jsonl   per-frame arrival log (stream_recv)
    s<NN>.beats.jsonl    one line per beat: {"beat": name, "t": wall-clock}
    s<NN>.png/.stats.json  final canvas + counters (stream_recv)

Run under WSL after `UNO_DEBUG=1 ./build.sh` (qemu-system-x86_64, OVMF,
sgdisk, mtools, ffmpeg - the remote_qemu.py toolchain).

The traps this encodes (do not relearn them):
  - the injected-pointer queue is 32 deep with a 2-frame dwell: sweep moves
    are paced ~35 ms apart host-side and never burst more than ~20 moves.
  - a click is THREE injections (move, press, release) - the shell samples
    per frame, so a press+release inside one sample cancels out (urcui.py).
  - launch by ID over URC (`launch files`), except where the Start menu
    itself is the shot; the menu ends at Shut Down - never arrow past it.
  - there is NO ALT over URC (`key` carries a ctrl flag only), so Alt-Tab is
    driven as F2 (the shell's no-Alt switcher) and Alt+Ctrl+Fn window moves
    as the title-bar right-click menu ("To desktop N").
  - the browser address bar has no select-all: End + 40 backspaces first.
  - QEMU slirp is outbound-only; the guest reaches the receiver at 10.0.2.2.
"""
import argparse, json, os, shutil, subprocess, sys, threading, time

HERE  = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
PC64  = os.path.dirname(TOOLS)
REPO  = os.path.dirname(PC64)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)

import remote_qemu as RQ                        # noqa: E402  (paths + boot_qemu)
from unoauto_remote import UnoAutoLink          # noqa: E402
from stream_recv import StreamReceiver, write_png  # noqa: E402

OUT    = os.path.join(HERE, "out")
PROBE  = os.path.join(OUT, "probe")            # dev screenshots (not deliverables)
SPORT0 = 5460                                  # first stream port; +1 per scene

# EFI scan codes (the `key` verb's scan field) - see map_key in uefi_main.c
S_UP, S_DOWN, S_RIGHT, S_LEFT = 0x01, 0x02, 0x03, 0x04
S_HOME, S_END, S_PGUP, S_PGDN = 0x05, 0x06, 0x09, 0x0A
S_F1, S_F2, S_F3, S_F4        = 0x0B, 0x0C, 0x0D, 0x0E
S_ESC                          = 0x17

# ---------------------------------------------------------------------------
# disk staging.  remote_qemu.build_disk() writes ITS OWN DEBUG.CFG, so the one
# knob we need on top (noshutdown - the stress driver otherwise powers the box
# off ~19 s in when the appliance selftest keys are present) plus the office /
# appliance payload staging happen here, against build/esp, BEFORE build_disk
# copies the tree into the FAT image.
# ---------------------------------------------------------------------------
CORPUS = os.path.join(REPO, "unodoc", "test", "corpus")
OFFICE = [("fmt.doc", "FMT.DOC"), ("pic.doc", "PIC.DOC"),
          ("formulas.xls", "FORMULAS.XLS"), ("small.ppt", "SMALL.PPT")]


def stage_office():
    """Corpus documents -> build/esp/DOCS\\ (for the Files browse beat) AND
    the ESP ROOT (for the office apps' shared Open dialog, which lists a
    volume's root only - uofile.c). The small ones are also `put` onto the
    RAM volume at runtime (s04_pre): the dialog's Look-in STARTS there, and
    a 3-row RAM listing photographs better than a 15-file root. SMALL.PPT
    cannot ride the RAM disk (per-file cap is 256 KB, pc64_io.c FILE_MAX),
    hence the root copies + the Look-in switch in the UnoShow beat."""
    dst = os.path.join(RQ.ESP, "DOCS")
    os.makedirs(dst, exist_ok=True)
    staged = []
    for src, name in OFFICE:
        p = os.path.join(CORPUS, src)
        if os.path.exists(p):
            shutil.copyfile(p, os.path.join(dst, name))
            shutil.copyfile(p, os.path.join(RQ.ESP, name))
            staged.append(name)
    return staged


def stage_wad():
    """DUUM.PY's IWAD, if the developer has one in pc64/wads (never downloaded
    here). Returns the staged ESP name or None."""
    for fn, dst in (("DOOM1.WAD", "DOOM1.WAD"), ("freedoom1.wad", "FREEDOOM.WAD")):
        p = os.path.join(PC64, "wads", fn)
        if os.path.exists(p):
            shutil.copyfile(p, os.path.join(RQ.ESP, dst))
            return dst
    return None


def stage_vm():
    """The appliance payload (bzImage/initrd/rootfs from build/), mirroring
    tools/vm_stage.py's staging (not its DEBUG.CFG edit - ours below carries
    the keys), PLUS the VMS.CFG registry row vm_stage.py does not write (an
    empty vmgr list gives Enter nothing to start). Empty path fields mean
    "the staged default payload". Returns True iff the pieces are present."""
    bz = os.path.join(PC64, "build", "bzImage")
    ird = os.path.join(PC64, "build", "initrd.gz")
    if not (os.path.exists(bz) and os.path.exists(ird)):
        return False
    vmdir = os.path.join(RQ.ESP, "EFI", "UNODOS", "VM")
    os.makedirs(vmdir, exist_ok=True)
    shutil.copyfile(bz, os.path.join(vmdir, "BZIMAGE"))
    shutil.copyfile(ird, os.path.join(vmdir, "INITRD"))
    rfs = os.path.join(PC64, "build", "rootfs.img")
    if os.path.exists(rfs):
        shutil.copyfile(rfs, os.path.join(vmdir, "ROOTFS.IMG"))
    with open(os.path.join(vmdir, "VMS.CFG"), "w", newline="\r\n") as f:
        f.write("linux||||512|1\n")
    return True


# s09 needs a HYPERVISOR-CAPABLE boot, which the shared harness's TCG boot is
# not: TCG silently drops +vmx (every hv_test.py TCG row ends "eligible: no"),
# unovirt's carve floor is 1800 MB (so -m 512 is a guaranteed refusal), and
# eligibility also demands a UNO_DETACH=1 build. The unovirt harnesses all use
# `-m 4096 -cpu host -enable-kvm` (hv_remote.py / hv_test.py). Opt in with
# UNO_DEMO_KVM=1 on an Intel host with nested KVM, after
# `UNO_DEBUG=1 UNO_DETACH=1 ./build.sh`; the AMD/SVM backend has never
# completed a VMRUN, so an AMD host stays skipped.
DEMO_KVM = os.environ.get("UNO_DEMO_KVM") == "1"


def boot_qemu():
    if not DEMO_KVM:
        return RQ.boot_qemu()
    subprocess.run(["cp", RQ.OVMF_VARS, RQ.VARS])
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "4096",
        "-cpu", "host", "-enable-kvm",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + RQ.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + RQ.VARS,
        "-drive", "format=raw,file=" + RQ.DISK,
        "-drive", "format=raw,file=" + RQ.DISK2,
        "-netdev", "user,id=n0" + os.environ.get("URC_HOSTFWD", ""),
        "-device", "e1000,netdev=n0",
        "-display", "none",
    ]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def build_diskB(docs):
    """Author disk B as GPT + one ESP FAT32 labelled DOCS carrying `docs`
    (host paths), so it mounts as ONE clean native-FAT volume. UnoShow's
    Open dialog lists a volume's root only and cannot scroll (uodlg's list
    has no scroll offset), so a big document like small.ppt is unreachable
    from the crowded ESP root - a two-file DOCS volume puts it at row 0.
    Same GPT+ESP recipe RQ.build_disk uses for the boot disk."""
    SECTOR, MIB = 512, 1 << 20
    disk_sectors = 96 * 2048
    with open(RQ.DISK2, "wb") as f:
        f.truncate(disk_sectors * SECTOR)
    subprocess.run(["sgdisk", "--zap-all", RQ.DISK2],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:DOCS",
                    RQ.DISK2], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    part_start = 2048
    part_sectors = disk_sectors - part_start - 2048
    fat = "/tmp/demo_docs_fat.img"
    with open(fat, "wb") as f:
        f.truncate(part_sectors * SECTOR)
    subprocess.run(["mformat", "-i", fat, "-v", "DOCS", "-F",
                    "-T", str(part_sectors), "::"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for src in docs:
        subprocess.run(["mcopy", "-i", fat, "-o", src,
                        "::/" + os.path.basename(src).upper()],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(fat, "rb") as pf, open(RQ.DISK2, "r+b") as df:
        df.seek(part_start * SECTOR)
        while True:
            b = pf.read(MIB)
            if not b:
                break
            df.write(b)


def build_disk(docs_for_b=None):
    """RQ.build_disk(), then our DEBUG.CFG (same keys + noshutdown) mcopy'd
    over the one it wrote. Keeping RQ's builder authoritative for the disk
    geometry means this file cannot drift from the harness everyone else
    boots. Disk B becomes the DOCS volume (or blank) - either way
    deterministic, so a stale TESTVOL from a prior gate never leaks in."""
    if docs_for_b:
        build_diskB(docs_for_b)
    else:
        try:
            os.unlink(RQ.DISK2)
        except OSError:
            pass
    RQ.build_disk()
    # nostress = the fuzz driver's real off switch (it would otherwise open a
    # random app every few frames and fight the choreography); noshutdown
    # belts-and-braces the auto power-off on top.
    cfg = os.path.join(os.path.dirname(RQ.DISK), "demo_debug.cfg")
    with open(cfg, "w", newline="\r\n") as f:
        f.write("remote=10.0.2.2:%d\nnonet\nnostress\nnoshutdown\n" % RQ.PORT)
    subprocess.run(["mcopy", "-i", RQ.FAT, "-o", cfg, "::/DEBUG.CFG"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # re-splice the FAT image into the disk (build_disk already dd'd it once)
    part_start = 2048
    with open(RQ.FAT, "rb") as pf, open(RQ.DISK, "r+b") as df:
        df.seek(part_start * 512)
        while True:
            b = pf.read(1 << 20)
            if not b:
                break
            df.write(b)


# ---------------------------------------------------------------------------
# the driver
# ---------------------------------------------------------------------------
class Demo(object):
    def __init__(self, verbose=True):
        self.link = None
        self.qemu = None
        self.w = self.h = 0
        self.px = self.py = 320             # last commanded pointer position
        self.verbose = verbose
        self._beats = None
        self._menu = []                     # apps list (id order = menu order)

    # ---- lifecycle --------------------------------------------------------
    def boot(self):
        if not os.path.isdir(RQ.ESP):
            raise SystemExit("no build/esp - run UNO_DEBUG=1 ./build.sh first")
        os.makedirs(OUT, exist_ok=True)
        os.makedirs(PROBE, exist_ok=True)
        self.link = UnoAutoLink("127.0.0.1", RQ.PORT)
        try:
            self.link.listen()
        except OSError as e:
            # A stale run's listener (or its QEMU still dialing) makes the
            # guest connect to the WRONG process and this one wait forever -
            # which presents as "never dialled in" and cost an hour to see.
            raise SystemExit(
                "cannot bind 127.0.0.1:%d (%s) - a previous run is still "
                "alive; kill stale scenes.py/*_qemu.py and qemu processes "
                "first" % (RQ.PORT, e))
        # disk B carries the big office docs (pic.doc, small.ppt) as a clean
        # DOCS volume - too large for the RAM disk and unreachable in the
        # crowded, un-scrollable ESP-root Open dialog.
        docsb = []
        for src, _ in OFFICE:
            if src in ("pic.doc", "small.ppt"):
                p = os.path.join(CORPUS, src)
                if os.path.exists(p):
                    docsb.append(p)
        build_disk(docs_for_b=docsb or None)
        self.qemu = boot_qemu()
        if not self.link.wait_connected(180):
            raise SystemExit("the guest never dialled in - is this the DEBUG build?")
        self.link.wait_hello(30.0)
        time.sleep(2.5)                     # let the desktop settle
        self.w, self.h = self.link.screen_info(timeout=15)
        self.px, self.py = self.w // 2, self.h // 2
        self._menu = [i for i, _ in self.apps()]
        if self.verbose:
            print("booted: %dx%d, %d apps" % (self.w, self.h, len(self._menu)))
        return self

    def stop(self):
        try:
            if self.link:
                self.link.command("poweroff", timeout=2)
        except Exception:               # noqa: BLE001
            pass
        time.sleep(0.5)
        if self.qemu:
            self.qemu.kill()
        if self.link:
            self.link.close()

    # ---- beats ------------------------------------------------------------
    def beat(self, name, settle=0.8):
        t = time.time()
        if self._beats:
            self._beats.write(json.dumps({"beat": name, "t": t}) + "\n")
            self._beats.flush()
        if self.verbose:
            print("  beat: " + name)
        if settle:
            time.sleep(settle)

    # ---- input ------------------------------------------------------------
    def key(self, uni=0, scan=0, ctrl=0, settle=0.15):
        self.link.key(scan, uni, ctrl, timeout=8)
        time.sleep(settle)

    def ctrl(self, ch, settle=0.35):
        self.key(ord(ch), 0, 1, settle)

    def text(self, s, settle=0.06):
        for ch in s:
            if ch == "\n":
                self.key(13, settle=settle)
            else:
                self.key(ord(ch), settle=settle)

    def move(self, x, y, btn=0, settle=0.12):
        x, y = int(x), int(y)
        self.link.pointer(x, y, btn, timeout=8)
        self.px, self.py = x, y
        time.sleep(settle)

    def sweep(self, x1, y1, btn=0, step=12, pace=0.035, burst=20):
        """Glide the pointer from its current position to (x1, y1) as many
        small moves, so the cursor visibly travels. Paced for the 32-deep
        injection queue (dwell 2): ~35 ms between moves, a breather every
        `burst` moves so the queue never overflows silently."""
        x0, y0 = self.px, self.py
        dx, dy = x1 - x0, y1 - y0
        dist = max(abs(dx), abs(dy))
        n = max(1, int(dist / step))
        for i in range(1, n + 1):
            self.move(x0 + dx * i // n, y0 + dy * i // n, btn, settle=pace)
            if i % burst == 0:
                time.sleep(0.4)
        self.move(x1, y1, btn, settle=0.1)

    def click(self, x, y, glide=True, settle=0.5):
        """Glide there, then move/press/release as THREE injections."""
        if glide:
            self.sweep(x, y)
        self.move(x, y, 0, settle=0.15)
        self.move(x, y, 1, settle=0.2)
        self.move(x, y, 0, settle=settle)

    def dblclick(self, x, y, glide=True, settle=0.6):
        self.click(x, y, glide=glide, settle=0.15)
        self.click(x, y, glide=False, settle=settle)

    def rclick(self, x, y, glide=True, settle=0.7):
        if glide:
            self.sweep(x, y)
        self.move(x, y, 0, settle=0.15)
        self.move(x, y, 2, settle=0.25)          # right button = bit 1
        self.move(x, y, 0, settle=settle)

    def drag(self, x0, y0, x1, y1, settle=1.0):
        """Press at (x0,y0), glide with the button held, release at (x1,y1).
        The hold before/after the travel is what makes it read as a grab."""
        self.sweep(x0, y0)
        self.move(x0, y0, 0, settle=0.2)
        self.move(x0, y0, 1, settle=0.35)        # grab
        self.sweep(x1, y1, btn=1)
        self.move(x1, y1, 1, settle=0.35)        # arrive, still held
        self.move(x1, y1, 0, settle=settle)      # drop

    # ---- shell ------------------------------------------------------------
    def apps(self):
        out = []
        for line in self.link.command("apps", "list", timeout=15):
            s = line.strip()
            if not s or s.startswith("end"):
                continue
            i, _, name = s.partition(" ")
            if i:
                out.append((i, name.strip()))
        return out

    def menu_index(self, app_id):
        if app_id not in self._menu:
            raise SystemExit("no app %r in this build (%s)" % (app_id, self._menu))
        return self._menu.index(app_id)

    def launch(self, app_id, settle=3.0):
        self.link.command("launch", app_id, timeout=15)
        time.sleep(settle)

    def windows(self):
        return [r["name"] for r in self.link.probe(timeout=10) if r["kind"] == 1]

    def close_top(self, settle=0.8):
        self.link.command("close", timeout=10)
        time.sleep(settle)

    def close_all(self):
        for _ in range(8):
            if not self.windows():
                return
            self.close_top(settle=0.6)

    def desk(self, n, settle=0.9):
        """Switch to virtual desktop n (1-based): Ctrl+F1..F4."""
        self.key(0, S_F1 + (n - 1), 1, settle)

    def reset(self):
        """Between scenes: every desktop back to empty, desktop 1 current."""
        for d in (1, 2, 3, 4):
            self.desk(d, settle=0.5)
            self.close_all()
        self.desk(1, settle=0.6)
        time.sleep(0.6)

    # ---- output -----------------------------------------------------------
    def shot(self, tag):
        """Probe screenshot (development + state polling; NOT the recording)."""
        w, h, rgba = self.link.screen_grab(1, timeout=40)
        p = os.path.join(PROBE, tag + ".png")
        write_png(p, w, h, rgba)
        if self.verbose:
            print("  shot: " + p)
        return p, w, h, rgba

    # ---- per-scene recording ---------------------------------------------
    def record(self, name, fn, port):
        base = os.path.join(OUT, name)
        for ext in (".mp4", ".timing.jsonl", ".beats.jsonl", ".png",
                    ".stats.json"):
            try:
                os.unlink(base + ext)
            except OSError:
                pass
        rx = StreamReceiver(port, out=base + ".mp4", host="127.0.0.1")
        rx.listen()
        th = threading.Thread(target=rx.serve_once,
                              kwargs={"accept_timeout": 60.0}, daemon=True)
        th.start()
        self._beats = open(base + ".beats.jsonl", "w")
        t0 = time.time()
        err = None
        try:
            r = self.link.command("stream", "start", "10.0.2.2", port, 30,
                                  timeout=10)
            if not (r and r[0].startswith("dialing")):
                raise RuntimeError("stream start refused: %r" % r)
            for _ in range(100):
                if rx.connected:
                    break
                time.sleep(0.15)
            if not rx.connected:
                raise RuntimeError("guest never connected to the receiver")
            time.sleep(1.0)                       # a settled opening frame
            fn(self)
            time.sleep(1.0)                       # a settled closing frame
        except Exception as e:                    # noqa: BLE001
            err = e
        finally:
            try:
                self.link.command("stream", "stop", timeout=8)
            except Exception:                     # noqa: BLE001
                pass
            th.join(30.0)
            self._beats.close()
            self._beats = None
        dur = time.time() - t0
        st = {"scene": name, "dur": round(dur, 1), "frames": rx.frames,
              "keyframes": rx.keyframes, "deltas": rx.deltas,
              "decode_errors": rx.decode_errors,
              "mp4": base + ".mp4",
              "mp4_bytes": (os.path.getsize(base + ".mp4")
                            if os.path.exists(base + ".mp4") else 0)}
        if rx.t_first and rx.t_last and rx.t_last > rx.t_first:
            st["fps"] = round((rx.frames - 1) / (rx.t_last - rx.t_first), 1)
        if err:
            st["error"] = repr(err)
        print("  %s: %s" % (name, json.dumps(st)))
        return st


# ---------------------------------------------------------------------------
# scenes
# ---------------------------------------------------------------------------
def s02_wm(d):
    """WM: Start menu rises, three apps, drag-to-edge snap, F2 switcher,
    'To desktop 2' via the title-bar menu, desktop switch, close all."""
    # the Start menu IS the shot here: open it, walk to Files, Enter.
    d.beat("start-menu-rises")
    d.key(0, S_ESC, 1, settle=1.4)                   # Ctrl-Esc; menu tween
    n = d.menu_index("files")
    for _ in range(n):
        d.key(0, S_DOWN, settle=0.22)
    d.beat("launch-files-from-menu", settle=0.3)
    d.key(13, settle=2.5)                            # Enter
    d.beat("launch-editor")
    d.launch("editor", settle=2.2)
    d.beat("launch-clock")
    d.launch("clock", settle=2.2)

    # drag the Editor by its title bar to the right edge -> snap tween.
    # The Editor window opens at (90,36) (pc64_write.c); its title bar is the
    # top ~18 px. Click it first: launching Clock left Clock focused.
    tx, ty = 200, 44
    d.beat("focus-editor")
    d.click(tx, ty, settle=0.8)
    d.beat("drag-editor-to-right-edge-snap")
    d.drag(tx, ty, d.w - 4, d.h // 2 - 60, settle=1.4)   # <8 px arms SNAP_R

    # Alt-Tab cannot be driven (no ALT over URC): F2 is the shell's own
    # no-Alt switcher; each press steps, it commits on its timer.
    d.beat("switcher-f2")
    d.key(0, S_F2, settle=0.5)                       # overlay up, step 1
    d.key(0, S_F2, settle=0.5)                       # step 2 (commit timer is
    time.sleep(1.6)                                  # ~0.8 s; stay under it
                                                     # between steps)

    # move the (snapped) Editor to desktop 2 via its title-bar context menu.
    # Snapped right, its bar spans the right half; right-click it there. With
    # the rclick at (w/2+40, 8) the popup lands at a stable spot; the desktop
    # rows sit at y = 152/174/196/218 for To-desktop-1..4 (measured off
    # d4_snap_popmenu, upscaled + read).
    mx, my = d.w // 2 + 40, 8
    d.beat("titlebar-menu")
    d.rclick(mx, my, settle=1.0)
    d.beat("to-desktop-2")
    d.click(*POP_TO_DESK2, settle=1.2)
    d.beat("switch-to-desktop-2")
    d.desk(2, settle=1.6)                            # the Editor lives here
    d.beat("switch-back-to-desktop-1")
    d.desk(1, settle=1.6)
    d.beat("close-all")
    d.close_all()                                    # clock + files here
    d.desk(2, settle=0.8)
    d.close_all()                                    # the moved editor
    d.desk(1, settle=0.8)


POP_TO_DESK2 = (410, 174)      # "To desktop 2" in the title-bar context menu,
                               # popup anchored by rclick at (w/2+40, 8)


def s03_themes(d):
    """Control Panel: theme tour with holds, a wallpaper, back to default."""
    d.beat("open-control-panel")
    d.launch("control", settle=2.5)
    d.key(9, settle=0.4)                             # Tab -> the tab strip
    for _ in range(6):
        d.key(0, S_LEFT, settle=0.15)                # clamp at Display
    d.key(0, S_RIGHT, settle=0.5)                    # -> Personalization
    d.key(9, settle=0.5)                             # -> Theme dropdown
    # kThemes order: 0 Aurora Light 1 Aurora Dark 2 UnoDOS 3 Mac OS 7
    # 4 Mac Plus 5 Windows 3.1 6 Amiga 7 C64 8 Apple II 9 NeXTSTEP
    steps = [("aurora-dark", 1), ("macos7", 2), ("win31", 2), ("c64", 2)]
    for tag, downs in steps:
        d.beat("theme-" + tag)
        for _ in range(downs):
            d.key(0, S_DOWN, settle=0.5)
        time.sleep(1.5)                              # hold on the redress
    d.beat("theme-aurora-light")
    for _ in range(7):
        d.key(0, S_UP, settle=0.35)
    time.sleep(1.5)
    d.beat("wallpaper-on")
    d.key(9, settle=0.35)                            # -> Dark mode check
    d.key(9, settle=0.35)                            # -> Wallpaper dropdown
    d.key(0, S_DOWN, settle=0.6)                     # wallpaper 1
    time.sleep(1.5)
    d.beat("wallpaper-off")
    d.key(0, S_UP, settle=0.6)                       # back to theme default
    d.beat("close")
    d.ctrl("w", settle=1.0)


# The shared Open dialog (uofile.c), coordinates measured off d3_* probe
# shots (640x400, default font). The dialog's Look-in starts on volume 0
# (RAM: README.TXT row 0, then the s04_pre pushes); the list mirrors the
# selected row into the name field, and arrow keys never reach the list in
# UnoWord or UnoCalc - so the drive there is: click the row, click Open.
# UnoShow's key bridge DOES map arrows, which is what the ESP walk rides.
UOF_ROW0 = (250, 112)          # first file-list row (click centre)
UOF_OPEN = (427, 228)          # the Open button
UOF_ROW_PITCH = 18             # verified: +18 hits row 1, +36 row 2
UOF_MENU_FILE = (54, 67)       # "File" on the uochrome menu bar
UOF_MENU_OPEN = (70, 109)      # its "Open..." row
UOF_COMBO_ARROW = (383, 87)    # the Look-in combo's drop arrow
UOF_COMBO_DOCS = (300, 143)    # popup row 2: the DOCS volume (disk B; row 0
                               # RAM, row 1 NO NAME/ESP, row 2 DOCS)
UOCALC_A2 = (112, 201)         # grid cell A2 (holds =(1+2)*3)


def uof_open_row(d, row):
    """Click file-list row `row` in the shared Open dialog, then Open."""
    d.click(UOF_ROW0[0], UOF_ROW0[1] + row * UOF_ROW_PITCH, settle=0.7)
    d.click(*UOF_OPEN, settle=2.5)


def s04_pre(d):
    """Stage the RAM copies for the Open dialog before the stream rolls (a
    base64 `put` on camera is nothing to look at). ONLY the small two: the
    RAM disk's per-file cap is 256 KB (pc64_io.c FILE_MAX), which refuses
    pic.doc and small.ppt. Push order is list order, and README.TXT is
    seeded first, so the dialog rows are FMT.DOC 1, FORMULAS.XLS 2."""
    if not getattr(d, "office_staged", None):
        print("  s04: SKIP - office corpus not staged")
        return False
    for name in ["FMT.DOC", "FORMULAS.XLS"]:
        src = dict((s.upper(), os.path.join(CORPUS, s))
                   for s, _ in OFFICE)[name]
        d.link.push_file(0, name, src)
    return True


def s04_office(d):
    """Files shows the staged docs on the RAM volume; UnoWord opens fmt.doc
    (select-all in place of scrolling: UnoWord scrolls by mouse WHEEL only,
    which URC cannot inject, and fmt.doc is one page anyway); UnoCalc opens
    formulas.xls and clicks a formula cell; UnoShow opens small.ppt from the
    clean DOCS volume (disk B)."""
    d.beat("files-sees-the-documents")
    d.launch("files", settle=2.5)                    # RAM: README + the 2 docs
    time.sleep(2.0)                                  # FMT.DOC/FORMULAS.XLS
    d.ctrl("w", settle=1.0)

    d.beat("unoword-open-fmt-doc")
    d.launch("uoword", settle=3.0)
    d.ctrl("o", settle=1.4)
    uof_open_row(d, 1)                               # FMT.DOC (RAM row 1)
    d.beat("select-all-sweep")                       # in place of the wheel-
    d.click(300, 240, glide=True, settle=0.6)        # only scroll (see above)
    d.ctrl("a", settle=1.2)
    time.sleep(1.0)
    d.ctrl("w", settle=1.2)

    d.beat("unocalc-open-formulas-xls")
    d.launch("uocalc", settle=3.0)
    d.ctrl("o", settle=1.4)
    uof_open_row(d, 2)                               # FORMULAS.XLS (RAM row 2)
    d.beat("click-a-formula-cell")
    d.click(*UOCALC_A2, settle=1.5)                  # formula bar: =(1+2)*3
    time.sleep(1.2)
    d.ctrl("w", settle=1.2)

    d.beat("unoshow-open-small-ppt")
    d.launch("uoshow", settle=3.0)
    d.click(*UOF_MENU_FILE, settle=0.8)              # UnoShow's Ctrl+O label
    d.click(*UOF_MENU_OPEN, settle=1.4)              # is decorative - click
    d.click(*UOF_COMBO_ARROW, settle=0.8)            # Look-in ...
    d.click(*UOF_COMBO_DOCS, settle=1.2)             # ... -> the DOCS volume
    # DOCS holds PIC.DOC + SMALL.PPT; the type filter shows presentations, so
    # SMALL.PPT is at/near row 0. UnoShow's key bridge maps Down, so a click
    # into the list to focus + select, then Open.
    d.click(*UOF_ROW0, settle=0.8)                   # row 0
    d.key(0, S_DOWN, settle=0.5)                     # ensure SMALL.PPT selected
    d.click(*UOF_OPEN, settle=3.0)
    time.sleep(2.0)
    d.beat("close")
    d.ctrl("w", settle=1.0)


def s05_browser(d, with_net=False):
    """Local uno: pages: JS demo, engine switch to QuickJS, same page again."""
    def goto(loc, settle=2.2):
        d.ctrl("l", settle=0.4)
        d.key(0, S_END, settle=0.1)
        for _ in range(40):
            d.key(8, settle=0.02)
        d.text(loc, settle=0.06)
        d.key(13, settle=settle)

    d.beat("launch-browser")
    d.launch("browser", settle=2.5)
    d.beat("uno-script-js-page")
    goto("uno:script", settle=2.5)                   # JS writes the page body
    d.beat("scroll-generated-table")
    for _ in range(6):
        d.key(0, S_DOWN, settle=0.35)
    time.sleep(0.8)
    d.beat("uno-engine-page")
    goto("uno:engine", settle=2.0)
    d.beat("switch-to-quickjs")
    goto("uno:engine/quickjs", settle=2.2)           # the switch IS a page
    d.beat("uno-script-on-quickjs")
    goto("uno:script", settle=2.5)
    for _ in range(4):
        d.key(0, S_DOWN, settle=0.35)
    if with_net:
        d.beat("SKIP-net-beats-are-metal-only")      # --with-net stub
    d.beat("close")
    d.ctrl("w", settle=1.0)


STUDIO_PROG = (
    '#include "UNO.H"\n'
    '\n'
    'static void demo_draw(UnoWin *w)\n'
    '{\n'
    'text_at(w->bounds.left + 16,\n'
    'w->bounds.top + TBAR_H + 24,\n'
    '"Hello from Studio!", C_WHITE, C_BLUE, true);\n'
    '}\n'
    '\n'
    'static const AppInterface kApp = {\n'
    'demo_draw, 0, 0, 0, 0, 0,\n'
    '"Demo", { 40, 40, 340, 200 }\n'
    '};\n'
    '\n'
    'const AppInterface *uno_app_main(const KernelApi *k)\n'
    '{\n'
    'gK = k;\n'
    'return &kApp;\n'
    '}\n')


def s07_studio(d):
    """Studio: File > New, type a small UnoC app, ^B build, ^R run, close."""
    d.beat("launch-studio")
    d.launch("studio", settle=3.0)
    # File > New via the in-window menu bar (measured in dev; Studio opens at
    # (24,20), menu bar directly under the title bar, "File" first).
    # File > New via the in-window menu bar (measured off d4_studio_filemenu:
    # Studio opens at (24,20), "File" title at (54,48), "New" row at (60,68)).
    d.beat("file-new")
    d.click(*STUDIO_FILE_XY, settle=0.8)
    d.click(*STUDIO_NEW_XY, settle=0.9)
    d.text("DEMO.C", settle=0.10)                    # the status-bar name box
    d.key(13, settle=1.0)
    d.beat("type-the-program")
    d.text(STUDIO_PROG, settle=0.035)
    d.beat("save")
    d.ctrl("s", settle=1.0)
    d.beat("build")
    d.ctrl("b", settle=0.2)
    time.sleep(3.0)                                  # ucc + status line
    d.beat("run")
    d.ctrl("r", settle=0.2)
    time.sleep(3.0)                                  # the app window opens
    d.beat("close-the-app")
    d.close_top(settle=1.0)
    d.beat("close-studio")
    d.close_all()


STUDIO_FILE_XY = (54, 48)      # "File" menu title (d4_studio_filemenu)
STUDIO_NEW_XY = (60, 68)       # its "New" row


def s08_pre(d):
    """Guard: no WAD in pc64/wads means a clean no-op, never a download. If a
    WAD is present, pre-launch Duum here (before the stream) so its WAD
    directory parse + first raycast frame - slow under TCG - are not dead air
    on camera. Returns True to record if the Duum window came up."""
    if not getattr(d, "wad_staged", None):
        print("  s08: SKIP - no WAD in pc64/wads, scene no-ops by design")
        return False
    # A PYAPP has no Start-menu row (it is a document PYRT opens), and the
    # reliable launch is the exact call Files makes on a double-click:
    # uno.run_app(vol, "APPS\\DUUM.UNO"), which pc64_shell_run_user runs on
    # PYRT. The ESP is the boot volume; find it rather than assume an index.
    espv = next((v["vol"] for v in d.link.vols()
                 if v["kind"] == 1 and v["name"].strip() in ("NO NAME", "")), 1)
    d._esp_vol = espv
    d.link.eval('import uno; uno.run_app(%d, "APPS\\\\DUUM.UNO")' % espv,
                timeout=30)
    for _ in range(30):                              # WAD parse + first frame
        if any(t.startswith("Duum") for t in d.windows()):
            return True
        time.sleep(2.0)
    print("  s08: Duum window did not appear after run_app - recording anyway")
    return True


def s08_duum(d):
    """Duum is already up (s08_pre launched it pre-stream). Show it running,
    then walk + turn through the map with the arrow keys (DUUM.PY: Up/Down
    move, Left/Right turn)."""
    d.beat("duum-running")
    time.sleep(1.5)
    d.beat("walk-forward")
    for _ in range(8):
        d.key(0, S_UP, settle=0.35)
    d.beat("turn")
    for _ in range(5):
        d.key(0, S_RIGHT, settle=0.35)
    d.beat("walk-on")
    for _ in range(6):
        d.key(0, S_UP, settle=0.35)
    time.sleep(1.0)
    d.beat("close")
    d.close_all()


def s09_pre(d):
    """s09's UNRECORDED half: boot the guest and wait for its shell. Returns
    True to record the console half, False to skip the scene.

    The guest gets ~4 ms of every ~16 ms frame, so its boot takes minutes -
    which is dead air on video. So it boots BEFORE the stream starts.

    Runs ONLY with UNO_DEMO_KVM=1 (see boot_qemu above): under plain TCG the
    hypervisor is never eligible (TCG silently drops vmx, -m 512 is under the
    1800 MB carve floor, and eligibility needs a UNO_DETACH=1 build), so the
    scene logs the reason and no-ops rather than recording a refusal."""
    if not getattr(d, "vm_staged", None):
        print("  s09: SKIP - no bzImage/initrd.gz under pc64/build")
        return False
    if not DEMO_KVM:
        print("  s09: SKIP - hypervisor ineligible under TCG; set "
              "UNO_DEMO_KVM=1 after UNO_DEBUG=1 UNO_DETACH=1 ./build.sh "
              "on an Intel/nested-KVM host")
        return False
    d.launch("vmgr", settle=3.0)
    d.key(13, settle=1.0)          # Enter: start row 0, auto-switch to console
    # poll probe shots until the console region stops changing (the seeded
    # boot script - UNODOS-GUEST-SHELL-OK, uname, mounts - has run its course)
    prev = None
    settled = 0
    for _ in range(90):                              # up to ~15 min of wall
        time.sleep(10.0)
        _, w, h, rgba = d.shot("s09_boot_poll")
        region = bytes(rgba[len(rgba) // 3: 2 * len(rgba) // 3])
        if region == prev:
            settled += 1
            if settled >= 3:                         # quiet for ~30 s
                break
        else:
            settled = 0
        prev = region
    return True


def s09_console(d):
    """s09's RECORDED half (the stream starts between the two)."""
    d.beat("guest-console")
    time.sleep(1.5)
    d.beat("type-ls")
    d.text("ls /", settle=0.15)
    d.key(13, settle=0.5)
    time.sleep(4.0)                                  # the answer scrolls in
    d.beat("type-uname")
    d.text("uname -a", settle=0.15)
    d.key(13, settle=0.5)
    time.sleep(4.0)
    d.beat("close")
    d.key(0, S_ESC, settle=0.8)                      # console -> list view
    d.close_all()


def s10_system_log(d):
    """System readout, then the live log viewer with real navigations."""
    d.beat("open-system")
    d.launch("system", settle=2.5)
    time.sleep(2.0)                                  # hold on the readout
    d.beat("open-logview")
    d.launch("logview", settle=2.5)
    d.beat("raise-level-to-info")
    d.text("=", settle=0.6)                          # More: notice -> info
    d.beat("generate-log-lines-in-browser")
    d.launch("browser", settle=2.5)
    d.ctrl("l", settle=0.4)
    d.key(0, S_END, settle=0.1)
    for _ in range(40):
        d.key(8, settle=0.02)
    d.text("uno:sample", settle=0.06)
    d.key(13, settle=1.8)
    d.ctrl("l", settle=0.4)
    d.key(0, S_END, settle=0.1)
    for _ in range(40):
        d.key(8, settle=0.02)
    d.text("uno:engine", settle=0.06)
    d.key(13, settle=1.8)
    d.beat("close-browser-watch-the-tail")
    d.ctrl("w", settle=1.5)                          # logview behind, tail moved
    time.sleep(2.0)
    d.beat("close-all")
    d.close_all()


# name -> (pre, body). `pre` runs before the stream starts (None = nothing);
# returning False skips the scene (already logged why).
SCENES = [
    ("s02", (None, s02_wm)),
    ("s03", (None, s03_themes)),
    ("s04", (s04_pre, s04_office)),
    ("s05", (None, s05_browser)),
    ("s07", (None, s07_studio)),
    ("s08", (s08_pre, s08_duum)),
    ("s09", (s09_pre, s09_console)),
    ("s10", (None, s10_system_log)),
]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--scene", action="append", default=[],
                    help="run one scene (repeatable)")
    ap.add_argument("--all", action="store_true", help="run the whole spine")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--with-net", action="store_true",
                    help="(stub) networking beats are metal-only; logged+skipped")
    a = ap.parse_args(argv)
    names = [n for n, _ in SCENES]
    if a.list:
        print("\n".join(names))
        return 0
    want = names if a.all else a.scene
    if not want:
        ap.error("--scene sNN or --all")
    bad = [wname for wname in want if wname not in names]
    if bad:
        ap.error("unknown scene(s) %s (have: %s)" % (bad, " ".join(names)))

    d = Demo()
    d.office_staged = stage_office()
    d.wad_staged = stage_wad()
    d.vm_staged = stage_vm()
    print("staged: office=%s wad=%s vm=%s" %
          (d.office_staged, d.wad_staged, d.vm_staged))
    results = []
    t0 = time.time()
    try:
        d.boot()
        for i, (wname, (pre, fn)) in enumerate(SCENES):
            if wname not in want:
                continue
            print("=== scene %s" % wname)
            if pre is not None and not pre(d):
                results.append({"scene": wname, "skipped": True})
                d.reset()
                continue
            body = (lambda dd, f=fn: f(dd, with_net=a.with_net)) \
                if wname == "s05" else fn
            results.append(d.record(wname, body, SPORT0 + i))
            d.reset()
    finally:
        d.stop()
    print("\ntotal: %.1f min" % ((time.time() - t0) / 60.0))
    for r in results:
        print(json.dumps(r))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
