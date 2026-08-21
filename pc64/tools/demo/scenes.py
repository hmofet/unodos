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

from unoauto_remote import UnoAutoLink          # noqa: E402
from stream_recv import StreamReceiver, write_png  # noqa: E402

# remote_qemu is the QEMU path ONLY. On the metal driver host (devbuntu) there
# is no OVMF, no mtools and no build tree, and importing it there used to be a
# hard ImportError that stopped `--metal` before it bound a port. It is
# optional: every use is guarded by MODE == "qemu".
try:
    import remote_qemu as RQ                    # noqa: E402  (paths + boot_qemu)
except Exception:                               # noqa: BLE001
    RQ = None

OUT    = os.path.join(HERE, "out")             # --out-dir overrides
PROBE  = os.path.join(OUT, "probe")            # dev screenshots (not deliverables)
SPORT0 = 5460                                  # first stream port; +1 per scene

# ---- mode ------------------------------------------------------------------
# "qemu"  : boot the DEBUG image here, guest reaches the receiver at 10.0.2.2.
# "metal" : the X13 Yoga boots a stick whose DEBUG.CFG says
#           remote=<this-host>:5101, so it DIALS US. We only listen; there is
#           no disk to stage and no QEMU to boot, and the stream target is this
#           host's LAN address.
MODE = "qemu"
METAL_PORT = 5101
RES_CONFIRM_S = 15                             # pc64_uui.c's probation clock
STREAM_HOST = None                             # resolved at boot (or --stream-host)

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
# The corpus lives in the repo, but the METAL driver runs on devbuntu where
# there is no repo - deploy.sh rsyncs the four documents into ./corpus/, so
# prefer that when the repo path is absent.
CORPUS = os.path.join(REPO, "unodoc", "test", "corpus")
if not os.path.isdir(CORPUS):
    CORPUS = os.path.join(HERE, "corpus")
# resume.doc is the DEMO's own document (tools/demo/assets, built by
# mkdemo_doc.py), not part of unodoc's test corpus: the corpus exists to carry
# one of every formatting property for a parser to check, so its text reads
# "a BOLDWORD z" and shows a viewer nothing. The scene opens a CV instead,
# which uses the same formatting and is recognisable at a glance.
OFFICE = [("resume.doc", "RESUME.DOC"), ("budget.xls", "BUDGET.XLS"),
          ("fmt.doc", "FMT.DOC"), ("pic.doc", "PIC.DOC"),
          ("formulas.xls", "FORMULAS.XLS"), ("small.ppt", "SMALL.PPT")]


def office_src(name):
    """Where a demo document actually is.

    Two sources, because they are two different things: unodoc's generated
    test corpus (fmt.doc and friends, whatever the checkout has) and the
    demo's OWN assets directory, which carries the documents that exist to be
    looked at rather than parsed. The corpus wins when both have a name, so a
    checkout with a full corpus behaves exactly as it did."""
    for base in (CORPUS, os.path.join(HERE, "assets")):
        cand = os.path.join(base, name)
        if os.path.exists(cand):
            return cand
    return os.path.join(CORPUS, name)


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
        p = office_src(src)
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


ASSETS = os.path.join(HERE, "assets")

# The private key s13 logs in with. NOT in the repo and not committed: it is a
# real credential whose public half sits in devbuntu's authorized_keys. Point
# UNO_DEMO_SSH_KEY at it, or drop it at assets/ssh_demo_key (gitignored).
# It MUST have no passphrase - see sshstore.parse_openssh_ed25519.
SSH_KEY = os.environ.get("UNO_DEMO_SSH_KEY",
                         os.path.join(ASSETS, "ssh_demo_key"))
SSH_HOST, SSH_PORT, SSH_USER = "192.168.2.100", 22, "arin"
SSH_SESS, SSH_KEYNAME = "devbuntu", "demo"


def stage_sdk():
    """sdk/SAMPLE.C -> the ESP ROOT, so s07 can OPEN a shipped, known-good
    source file instead of typing a program in.

    Studio's Project pane lists ONE volume's root (refresh_project, studio.c)
    and there is no File > Open, so a source file that lives in SDK\\ is
    unreachable from the IDE. The SDK copy stays where it is; this is a second
    copy at the root purely so the pane has a row to open. It is also the file
    Ctrl-S writes back to (doc_save uses the basename against ed_vol), which
    keeps the demo's edit off the shipped SDK copy.

    Typing a whole program on camera was the old s07 and it is what broke the
    take: File > New silently missed at 1280x800, so the C source landed in
    the greeted SAMPLE.PY, packed as Python, and could never build."""
    src = os.path.join(PC64, "sdk", "SAMPLE.C")
    if not os.path.exists(src):
        return None
    shutil.copyfile(src, os.path.join(RQ.ESP, "SAMPLE.C"))
    return "SAMPLE.C"


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


# s14 (appliances) needs a HYPERVISOR-CAPABLE boot, which the shared harness's
# TCG boot is not: TCG silently drops +vmx (every hv_test.py TCG row ends
# "eligible: no"),
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
        # 3 GB, NOT 4. With -m 4096 this guest's audio is SILENT: the sink
        # records the whole run and every sample is zero, on the same build
        # and the same WAD that plays music at -m 3072 (measured twice each,
        # both launch paths). The ring is a static buffer in the loaded image
        # and AC'97's descriptors are 32-bit, so a machine with memory above
        # the 4 GB line is the suspect - filed in UNOAUTOMATE-REQUESTS.md,
        # and it matters far beyond this harness because every real machine
        # has more than 4 GB. s14's appliance scene sets its own -m.
        "qemu-system-x86_64", "-machine", "q35", "-m", "3072",
        "-cpu", "host", "-enable-kvm", "-smp", "4",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + RQ.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + RQ.VARS,
        "-drive", "format=raw,file=" + RQ.DISK,
        "-drive", "format=raw,file=" + RQ.DISK2,
        "-netdev", "user,id=n0" + os.environ.get("URC_HOSTFWD", ""),
        "-device", "e1000,netdev=n0",
        "-display", "none",
    ]
    # UNO_DEMO_WAV=<path> attaches a capture sink, for a scene whose subject
    # now makes noise (Duum plays the WAD's own effects and score since
    # 2026-08-19). AC'97 AND NOT INTEL HDA: with an intel-hda device attached
    # this guest never reaches URC dial-out under KVM - measured with private
    # disk paths so the shared-boot-disk race below cannot explain it, and
    # identically on a build from before the sound work. snd_pcm sits above
    # both backends, so the capture is of the same mixer either way.
    # UNOAUTOMATE-REQUESTS.md, 2026-08-19, carries the matrix.
    wav = os.environ.get("UNO_DEMO_WAV")
    if wav:
        cmd += ["-audiodev", "wav,id=snd0,path=" + wav,
                "-device", "AC97,audiodev=snd0"]
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
    # THIS is where the harness's DEBUG.CFG is authored, and the only place a
    # key survives: RQ.build_disk() writes its own DEBUG.CFG into the FAT
    # image, so a key added to build/esp by hand is silently overwritten.
    #   nostress  - the fuzz driver's real off switch (it would otherwise open
    #               a random app every few frames and fight the choreography)
    #   noshutdown- belt-and-braces on the stress auto power-off
    #   nohud     - hide the red perf HUD (and the stress status line under
    #               it). A debug build is the only one that dials out on its
    #               own, so it is the only one we can drive - but it paints
    #               that HUD into every frame. Telemetry is still collected;
    #               only the on-screen readout goes. (pc64/DEBUG.md; the boot
    #               log prints `hud_len=0 (HUD DISABLED)`.)
    cfg = os.path.join(os.path.dirname(RQ.DISK), "demo_debug.cfg")
    with open(cfg, "w", newline="\r\n") as f:
        f.write("remote=10.0.2.2:%d\nnonet\nnostress\nnoshutdown\nnohud\n"
                % RQ.PORT)
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
        if MODE == "metal":
            return self.boot_metal()
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
                p = office_src(src)
                if os.path.exists(p):
                    docsb.append(p)
        build_disk(docs_for_b=docsb or None)
        self.t_qemu = time.time()      # the capture sink's own zero, if any
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

    # ---- metal ------------------------------------------------------------
    def boot_metal(self, wait=600.0):
        """Wait for the metal box to dial IN. Nothing is booted or staged here:
        the stick's own DEBUG.CFG (nohud, nostress, noshutdown, ui-unlock,
        remote=<this-host>:5101) does that, and it re-dials ~45 s after every
        reboot - so this can be started before the box is, and simply waits."""
        global STREAM_HOST
        os.makedirs(OUT, exist_ok=True)
        os.makedirs(PROBE, exist_ok=True)
        self.link = UnoAutoLink("0.0.0.0", METAL_PORT)
        try:
            self.link.listen()
        except OSError as e:
            raise SystemExit(
                "cannot bind 0.0.0.0:%d (%s) - something else holds it. On "
                "devbuntu that is usually the watcher: "
                "pkill -f '[w]atcher.py'" % (METAL_PORT, e))
        print("metal: listening on 0.0.0.0:%d - waiting up to %.0f min for the "
              "box to dial in (it re-dials ~45 s after a reboot)"
              % (METAL_PORT, wait / 60.0))
        if not self.link.wait_connected(wait):
            raise SystemExit(
                "no dial-in within %.0f min. Check the box is powered on and "
                "its DEBUG.CFG says remote=<this-host>:%d" % (wait / 60.0,
                                                              METAL_PORT))
        self.link.wait_hello(60.0)
        time.sleep(2.5)
        # The stream target is OUR LAN address as the box can route to it.
        # Derived from the live connection (its peer is the box) rather than
        # hardcoded, so this works on any driver host / subnet.
        if STREAM_HOST is None:
            STREAM_HOST = self._host_ip_toward_box()
        self.w, self.h = self.screen_size()
        self.px, self.py = self.w // 2, self.h // 2
        self._menu = [i for i, _ in self.apps()]
        print("metal: box dialed in - %dx%d, %d apps, stream target %s"
              % (self.w, self.h, len(self._menu), STREAM_HOST))
        return self

    def _host_ip_toward_box(self):
        """This host's address ON THE PATH TO THE BOX. Uses the live socket's
        peer when we can see it (exact), else routes toward the LAN."""
        peer = None
        try:
            peer = self.link._sock.getpeername()[0]
        except Exception:                       # noqa: BLE001
            pass
        return UnoAutoLink._local_ip_toward(peer or "8.8.8.8")

    def probe_metal_assets(self):
        """Find what the STICK actually carries, rather than trusting the
        QEMU-era volume indices. On metal the assets are already there:
        DOOM1.WAD, BASE291.WSZ, FLYHIGH.MP3 and DOCS\\ at the volume root."""
        self.wad_staged = None
        self._esp_vol = None
        self.py_used = True                     # this probe IS `py` traffic -
                                                # see the note above pyeval()
        for v in self.link.vols(timeout=15):
            if v["kind"] == 0:                  # the RAM disk carries nothing
                continue
            try:
                # ONE LINE. The `py` verb is a single-line exec: a script with
                # an embedded newline returns EMPTY for paths that read
                # perfectly, so a multi-line probe reported "no assets" on a
                # stick that carries all of them and silently skipped Duum.
                out = self.link.eval(
                    'print(__import__("uno").size(%d,"DOOM1.WAD"),'
                    '__import__("uno").size(%d,"DOCS\\\\FMT.DOC"))'
                    % (v["vol"], v["vol"]), timeout=25)
                wad, doc = (int(t) for t in out[0].split())
            except Exception:                   # noqa: BLE001
                continue
            if wad > 0 or doc > 0:
                self._esp_vol = v["vol"]
                if wad > 0:
                    self.wad_staged = "DOOM1.WAD"
                print("metal: assets on vol %d (%s) wad=%d docs=%s"
                      % (v["vol"], v["name"].strip(), wad, doc > 0))
                break
        if self._esp_vol is None:
            print("metal: WARNING - found no volume carrying DOOM1.WAD or "
                  "DOCS\\, s08 will skip")

    def stream_target(self):
        """Where the guest should dial the receiver. QEMU SLIRP maps the host
        to 10.0.2.2; on metal it is a real LAN address."""
        return "10.0.2.2" if MODE == "qemu" else STREAM_HOST

    def stop(self):
        wav = os.environ.get("UNO_DEMO_WAV")
        if wav and getattr(self, "t_qemu", 0):
            # The sink starts writing when the GUEST opens the stream, not
            # when QEMU starts, so its zero cannot be assumed - but it stops
            # when QEMU dies, so t0 == t_end - (length of the wav). Record
            # both ends here and let the stitcher do that subtraction.
            try:
                with open(os.path.splitext(wav)[0] + ".json", "w") as f:
                    json.dump({"t0": self.t_qemu, "t_end": time.time(),
                               "wav": wav}, f, indent=1)
            except Exception:           # noqa: BLE001
                pass
        try:
            # NEVER power the metal box off: it is a physical machine somebody
            # has to walk to, and the whole point of the stick is that it stays
            # up and re-dials. Only the throwaway QEMU guest gets shut down.
            if self.link and MODE == "qemu":
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

    def screen_size(self, tries=4, timeout=20):
        """screen_info with retries. A single timed-out round trip used to
        abort a whole session from inside raise_resolution's verification -
        the guest was merely busy repainting a fresh 1280x800 desktop. The
        answer is worth waiting for, so ask again rather than give up."""
        last = None
        for i in range(tries):
            try:
                return self.link.screen_info(timeout=timeout)
            except Exception as e:              # noqa: BLE001
                last = e
                if self.verbose:
                    print("    screen_info timed out (%d/%d), retrying"
                          % (i + 1, tries))
                time.sleep(2.0)
        raise RuntimeError("screen_info failed %d times: %r" % (tries, last))

    def _cp_display_tab(self):
        """Open the Control Panel clamped to the Display tab, focus on the tab
        strip. Deterministic whatever tab it last remembered."""
        self.close_all()
        self.launch("control", settle=2.5)
        self.key(9, settle=0.5)                  # Tab -> the tab strip
        for _ in range(6):
            self.key(0, S_LEFT, settle=0.15)     # clamp at Display (tab 0)

    def _res_try(self, up_from_end):
        """Select the mode `up_from_end` rows above the LAST dropdown row,
        press Apply, and report the (w, h) it produced. Leaves the panel on
        probation - the caller Keeps or Reverts.

        Rows are addressed from the END because the mode list cannot be
        enumerated over URC (`uno.res_count` is not exported to the Python
        module - that is the pc64-python lane's file, not this one), but the
        dropdown CLAMPS at its last row, so pressing Down 14 times always
        lands there whatever the list length. The firmware's largest mode is
        typically at or near the end."""
        self._cp_display_tab()
        self.key(9, settle=0.5)                  # -> the Resolution dropdown
        for _ in range(14):
            self.key(0, S_DOWN, settle=0.12)     # clamp at the LAST row
        for _ in range(up_from_end):
            self.key(0, S_UP, settle=0.18)
        time.sleep(0.6)
        self.key(9, settle=0.5)                  # -> Apply
        self.key(13, settle=0.2)                 # commit; the shell reflows
        time.sleep(3.5)
        return self.screen_size()

    ACCENT = (76, 110, 245)            # the theme accent: Keep's fill

    def find_keep_button(self):
        """Locate the probation row's **Keep** button on a live grab.

        Keep is the only accent-FILLED control on the Display tab, and it is
        the lowest one on screen once the taskbar (whose Start chip is the
        same colour) is excluded. Returns its centre, or None.

        Three keyboard routes to this button were tried and all three failed
        SILENTLY - see raise_resolution's note. The button is found and
        clicked instead, which needs no assumption about focus at all."""
        w, h, rgba, sc = self.grab(1)
        # Keep the exact frame the decision was made on. When this locator
        # picks the wrong accent-filled control the mode silently reverts and
        # there is otherwise nothing to look at afterwards.
        try:
            write_png(os.path.join(PROBE, "keep_probe.png"), w, h, rgba)
        except Exception:                        # noqa: BLE001
            pass
        cr, cg, cb = self.ACCENT
        rows = {}
        for y in range(max(0, h - 40)):          # taskbar excluded
            ro = y * w * 4
            r = [x for x in range(w)
                 if abs(rgba[ro + x * 4] - cr) < 14
                 and abs(rgba[ro + x * 4 + 1] - cg) < 14
                 and abs(rgba[ro + x * 4 + 2] - cb) < 14]
            if r and len(r) <= 150:              # skip the window's underline
                rows[y] = r
        if not rows:
            return None
        ys = sorted(rows)
        groups = [[ys[0]]]
        for y in ys[1:]:
            if y - groups[-1][-1] <= 2:
                groups[-1].append(y)
            else:
                groups.append([y])
        g = groups[-1]                           # the lowest cluster = Keep
        xs = [x for y in g for x in rows[y]]
        pt = ((min(xs) + max(xs)) // 2 * sc, (g[0] + g[-1]) // 2 * sc)
        if self.verbose:
            print("  keep-probe: %d accent cluster(s), chose y=%d-%d x=%d-%d "
                  "-> %s" % (len(groups), g[0], g[-1], min(xs), max(xs), pt))
        return pt

    def _res_confirm(self, keep):
        """Confirm (Keep) or reject (Revert now) the mode on probation, by
        CLICKING - the one route that needs no assumption about focus."""
        pt = self.find_keep_button()
        if pt is None:
            print("resolution: could not find the Keep button on screen")
            return
        if not keep:
            # "Revert now" sits immediately right of Keep; a button-width over
            # lands on it. Doing nothing would also revert, but not promptly.
            pt = (pt[0] + 90, pt[1])
        self.click(pt[0], pt[1], settle=1.5)
        time.sleep(2.0)

    def raise_resolution(self, min_w=1024):
        """Pick a big mode and CONFIRM it, once, at session start - before any
        stream exists.

        Never mid-stream: a resolution change makes the guest emit a fresh
        hello, which stream_recv treats as a stream reset and splits the
        recording into `<name>-2.mp4`. That is the whole reason this runs here.

        Selecting and applying is KEYBOARD-driven (the screen changes size
        underneath the sequence, so any coordinate read beforehand would be
        stale halfway through). CONFIRMING is a located CLICK, because three
        separate keyboard routes to Keep each failed silently:
          - Tab-counting the visible layout lands on "Revert now": a
            successful Apply DISABLES the Apply button (pc64_uui.c:1132) and
            unoui refuses focus to a disabled widget (unoui_input.c:89), so
            Tab skips it and every count is one too many.
          - closing and reopening the panel for a deterministic walk raced the
            15 s probation clock.
          - a bare Enter does nothing: Keep's blue fill is the UI_F_DEFAULT
            ring (unoui.c:579), not focus - which is what made this look
            solved when it was not.

        The 15 s auto-revert is the safety net: if a mode comes up unreadable,
        or the click misses, doing NOTHING puts the old mode back."""
        if self.w >= min_w:
            print("resolution: already %dx%d, leaving it" % (self.w, self.h))
            return True
        start = (self.w, self.h)
        best = None
        for k in range(6):                       # last row first, then upward
            try:
                w, h = self._res_try(k)
            except Exception as e:               # noqa: BLE001
                print("resolution: probe %d failed (%s)" % (k, e))
                break
            if (w, h) == start:
                print("resolution: row -%d changed nothing (Apply disabled?)" % k)
                continue                         # no probation armed to undo
            if w >= min_w:
                self._res_confirm(keep=True)
                # OUTLAST THE PROBATION before believing it. RES_CONFIRM_S is
                # 15 s, and a mode that was applied but not confirmed reads as
                # the NEW size right up until it snaps back - so a check made
                # promptly cannot tell "kept" from "about to revert", and
                # reports success either way. Ask again on the far side.
                time.sleep(RES_CONFIRM_S + 4)
                w2, h2 = self.screen_size()
                if (w2, h2) == (w, h):
                    best = (w, h)
                    break
                print("resolution: %dx%d did NOT stick (now %dx%d) - the Keep "
                      "click missed and it auto-reverted" % (w, h, w2, h2))
                continue
            print("resolution: row -%d gave %dx%d, too small - reverting" % (k, w, h))
            self._res_confirm(keep=False)
        self.close_all()
        w, h = self.screen_size()
        self.w, self.h = w, h
        self.px, self.py = w // 2, h // 2
        ok = w >= min_w
        print("resolution: now %dx%d (%s)"
              % (w, h, "raised + confirmed" if ok
                 else "NOT raised - beats will be small"))
        return ok

    def reset(self):
        """Between scenes: every desktop back to empty, desktop 1 current."""
        for d in (1, 2, 3, 4):
            self.desk(d, settle=0.5)
            self.close_all()
        self.desk(1, settle=0.6)
        time.sleep(0.6)

    # ---- runtime geometry -------------------------------------------------
    # THE RESOLUTION PROBLEM. Every coordinate in this file was first read off
    # a 640x400 probe shot, and the metal box runs at 1280x800 or better. They
    # fall into three classes, and only the third needed fixing:
    #
    #  1. WINDOW-RELATIVE, and the window origin is HARDCODED in the app -
    #     Control Panel (150,24), Studio (24,20), UnoCalc (24,20), UnoShow
    #     (20,16), Files (120,64), Editor (90,36). Their menu bars, toolbars
    #     and grids are laid out from that top-left corner, so those clicks are
    #     resolution-INDEPENDENT and stay as literals.
    #  2. COMPUTED from the live size already (the snap edge is d.w - 4).
    #  3. SCREEN-CENTRED - the shared Open dialog (uod_open: x=(sw-dw)/2,
    #     y=(sh-dh)/3) and the title-bar popup (pop_show anchors at the click
    #     then clamps against the taskbar). These MOVE with resolution, and
    #     baked literals would have missed every one of them. They are derived
    #     at runtime below.
    DLG_W, DLG_H = 294, 244            # the Open dialog, measured at 640x400;
                                       # x0=(640-294)/2=173 matched the probe
                                       # shot to the pixel, so the formula (not
                                       # the literal) is what is trusted here.
    DLG_REL = {"row0":  (77, 60),      # offsets from the dialog's top-left
               "open":  (254, 176),
               "name":  (127, 176),
               "combo": (210, 35),
               "vol2":  (127, 91)}
    DLG_ROW_PITCH = 18

    def dlg(self, what, row=0):
        """A point inside the shared Open dialog, for THIS resolution."""
        x0 = max(0, (self.w - self.DLG_W) // 2)      # uod_open's own formula
        y0 = max(0, (self.h - self.DLG_H) // 3)
        dx, dy = self.DLG_REL[what]
        return (x0 + dx, y0 + dy + row * self.DLG_ROW_PITCH)

    def grab(self, scale=4):
        """A downscaled screen grab as (w, h, rgba, scale). Scale 4 keeps a
        1280x800 frame small enough to pull over URC in about a second, and
        every feature located from it is >= 20 px, so 4 px of quantisation is
        harmless."""
        w, h, rgba = self.link.screen_grab(scale, timeout=60)
        return w, h, rgba, scale

    @staticmethod
    def diff_box(a, b, thresh=24, skip_bottom=40):
        """Bounding box (native coords) of what changed between two grabs.
        This is how the popup is located: whatever appeared IS the popup.

        `skip_bottom` (native px) EXCLUDES THE TASKBAR, and it is not optional.
        The taskbar carries a clock that reticks every second, so on any pair
        of grabs a second apart the clock is also "what changed" - and the
        BOUNDING BOX of a 130 px menu plus a clock in the far corner is 600 px
        wide. rclick_menu then derived its row pitch from that box and clicked
        200 px below the real menu: the popup just closed and the command
        never ran, silently. Only an extracted frame showed it."""
        aw, ah, ar, sc = a
        bw, bh, br, _ = b
        if aw != bw or ah != bh:
            return None
        x0, y0, x1, y1 = aw, ah, -1, -1
        for y in range(max(0, ah - skip_bottom // max(1, sc))):
            ro = y * aw * 4
            for x in range(aw):
                o = ro + x * 4
                if (abs(ar[o] - br[o]) + abs(ar[o + 1] - br[o + 1]) +
                        abs(ar[o + 2] - br[o + 2])) > thresh:
                    if x < x0: x0 = x
                    if x > x1: x1 = x
                    if y < y0: y0 = y
                    if y > y1: y1 = y
        if x1 < 0:
            return None
        return (x0 * sc, y0 * sc, (x1 + 1) * sc, (y1 + 1) * sc)

    def slide_rect(self):
        """The white slide page inside UnoShow, located on a live grab.

        UnoShow's window is sized from the framebuffer and the page is centred
        inside it, so nothing about this is fixed across resolutions. The page
        is the one big pure-white run bounded by the editor's grey mat, which
        is unambiguous enough to find directly."""
        w, h, rgba, sc = self.grab(2)
        def white(o):
            return rgba[o] > 240 and rgba[o + 1] > 240 and rgba[o + 2] > 240
        def grey(o):
            r, g, b = rgba[o], rgba[o + 1], rgba[o + 2]
            return 90 < r < 190 and abs(r - g) < 12 and abs(g - b) < 12
        runs = {}
        for y in range(h):
            ro, x = y * w * 4, 0
            while x < w:
                if white(ro + x * 4):
                    s = x
                    while x < w and white(ro + x * 4):
                        x += 1
                    e = x - 1
                    if (e - s) > 40 and s > 0 and e < w - 1 and \
                            grey(ro + (s - 1) * 4) and grey(ro + (e + 1) * 4):
                        runs.setdefault((s, e), []).append(y)
                else:
                    x += 1
        if not runs:
            raise RuntimeError("no slide page found on screen")
        (s, e), ys = max(runs.items(), key=lambda kv: len(kv[1]))
        return (s * sc, min(ys) * sc, (e + 1) * sc, (max(ys) + 1) * sc)

    def slide_point(self, fx, fy):
        """A point at fractional (fx, fy) of UnoShow's slide page."""
        x0, y0, x1, y1 = self.slide_rect()
        return (int(x0 + (x1 - x0) * fx), int(y0 + (y1 - y0) * fy))

    def rclick_menu(self, x, y, row, nrows=16, settle=1.0):
        """Right-click at (x, y), LOCATE the popup that appeared, and click
        row `row` of it. The popup is anchored at the click but then clamped
        against the taskbar, so where it lands depends on the resolution and
        on how tall it is - which is exactly why this measures rather than
        assumes."""
        before = self.grab()
        self.rclick(x, y, settle=settle)
        after = self.grab()
        box = self.diff_box(before, after)
        if not box:
            raise RuntimeError("the title-bar menu did not appear at %d,%d"
                               % (x, y))
        bx0, by0, bx1, by1 = box
        # SANITY. A window menu is ~130 px wide; anything much wider means
        # something other than the popup repainted across the right-click and
        # the row pitch derived from this box is fiction. Say so loudly - the
        # failure is otherwise completely silent (the menu simply closes and
        # the command never runs).
        if (bx1 - bx0) > 320:
            print("    WARNING: located 'popup' is %d px wide - that is a "
                  "window repaint, not a menu. Row %d will miss."
                  % (bx1 - bx0, row))
        rh = float(by1 - by0 - 2) / nrows
        cx = bx0 + min(60, (bx1 - bx0) // 2)
        cy = int(by0 + 1 + row * rh + rh / 2)
        if self.verbose:
            print("    popup at %r, row %d -> (%d,%d)" % (box, row, cx, cy))
        self.click(cx, cy, settle=1.2)

    # ---- output -----------------------------------------------------------
    def wait_stable(self, timeout=25.0, poll=1.6, quiet=2, scale=4):
        """Block until the screen stops changing, or `timeout`.

        A network page load is the one beat whose length is not ours to
        choose: en.wikipedia.org over TLS under TCG took between 6 and 20 s
        across probe runs, and a fixed settle is wrong in both directions -
        too short records a half-drawn page, too long records dead air. Two
        identical scale-4 grabs in a row means the paint has finished.
        THE TASKBAR IS EXCLUDED, and that is the whole trick: it carries a
        CLOCK that reticks every second, so a whole-frame comparison never
        matches and this waited out its full timeout on a page that had
        finished loading twenty seconds earlier (measured: 28 s of dead air in
        the first s05 take). Only the rows above the bar are compared.

        Returns the seconds waited."""
        t0 = time.time()
        last, same = None, 0
        cut = max(1, 40 // scale)                # taskbar rows, at this scale
        while time.time() - t0 < timeout:
            time.sleep(poll)
            try:
                gw, gh, rgba = self.link.screen_grab(scale, timeout=60)
                cur = bytes(rgba[:max(0, (gh - cut)) * gw * 4])
            except Exception:                    # noqa: BLE001
                continue
            if last is not None and cur == last:
                same += 1
                if same >= quiet:
                    break
            else:
                same = 0
            last = cur
        dt = time.time() - t0
        if self.verbose:
            print("    wait_stable: %.1f s" % dt)
        return dt

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
        # QEMU dials the host over loopback; the metal box dials a LAN address,
        # so the receiver has to be bound on all interfaces there.
        rx = StreamReceiver(port, out=base + ".mp4",
                            host="127.0.0.1" if MODE == "qemu" else "0.0.0.0")
        rx.listen()
        th = threading.Thread(target=rx.serve_once,
                              kwargs={"accept_timeout": 60.0}, daemon=True)
        th.start()
        self._beats = open(base + ".beats.jsonl", "w")
        t0 = time.time()
        err = None
        try:
            r = self.link.command("stream", "start", self.stream_target(),
                                  port, 30, timeout=10)
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
              "w": rx.w, "h": rx.h,
              # unostream delivers well under the requested rate at 1280x800,
              # so the receiver rescales the container's timestamps to the
              # rate frames ACTUALLY arrived. Whether it had to is part of the
              # per-scene report, so record it next to the frame counts.
              "retimed_by_receiver": bool(getattr(rx, "retimed", False)),
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
    """WM: Start menu rises, two apps, drag-to-edge snap, F2 switcher,
    'To desktop 2' via the title-bar menu, desktop switch back.

    Trimmed from 50 s to ~36 s against measured per-beat costs (2026-08-07):
    the third app, the redundant focus click and the on-camera teardown went;
    every hero moment stayed. Teardown now happens in reset(), AFTER the
    stream stops, so it costs the cut nothing."""
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
    # Clock was a third launch making the same point as the second (-3.1 s).

    # drag the Editor by its title bar to the right edge -> snap tween.
    # The Editor window opens at (90,36) (pc64_write.c); its title bar is the
    # top ~18 px. No separate focus click: the Editor is the app just
    # launched, and drag() sweeps to the bar and presses there anyway, which
    # focuses it on mouse-down (-4.5 s).
    tx, ty = 200, 44
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
    # Snapped right, its bar spans the right half; right-click it there.
    # The popup is anchored at the click but then CLAMPED against the taskbar,
    # so where it lands depends on the resolution - rclick_menu locates it from
    # a before/after grab and clicks row 7 ("To desktop 2") of its 16.
    #   0 Restore  1 Minimize  2 Maximize  3 Snap left  4 Snap right  5 sep
    #   6..9 To desktop 1..4   10 sep  11..13 Group  14 sep  15 Close
    mx, my = d.w // 2 + 40, 8
    d.beat("titlebar-menu")
    # RAISE IT FIRST, with a plain click on the same bar. rclick_menu locates
    # the popup as "whatever changed across the right-click", so anything ELSE
    # that repaints in that window is measured as part of the popup. After the
    # F2 switcher the Editor is not the front window, and the right-click
    # raises it - which repainted the whole snapped-right window and gave a
    # 584x788 "popup". Row 7 of that is 200 px below the real menu, so the
    # click landed in the document, the menu just closed, and the window never
    # moved to desktop 2. Nothing in the beat log said so; only a frame did.
    d.click(mx, my, settle=1.0)
    d.beat("to-desktop-2")
    d.rclick_menu(mx, my, row=7, nrows=16)
    d.beat("switch-to-desktop-2")
    d.desk(2, settle=1.6)                            # the Editor lives here
    d.beat("switch-back-to-desktop-1")
    d.desk(1, settle=1.6)
    # No on-camera teardown (-5.7 s): reset() empties every desktop after the
    # stream has stopped, so the scene ends on the desktop switch instead of
    # on five seconds of windows closing.


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


# The office apps' own chrome is WINDOW-relative and every one of these
# windows opens at a hardcoded origin (UnoWord/UnoCalc 24,20; UnoShow 20,16),
# so these literals hold at any resolution. The shared Open DIALOG is
# screen-centred and does NOT - see Demo.dlg(), which derives it.
UOF_MENU_FILE = (54, 67)       # "File" on the uochrome menu bar
UOF_MENU_OPEN = (70, 109)      # its "Open..." row
UOCALC_A2 = (112, 201)         # grid cell A2
# D9 in budget.xls: the SUM over the line totals, MEASURED off a recorded
# frame rather than derived. The first attempt computed it from a guessed
# pitch (76 px per column, 16 per row) and landed on D8, the blank spacer
# row - an empty formula bar, and a beat that showed nothing. The real grid
# at this window size is 20 px per row (row 1 at y=181) and ~78 per column
# (D centres at x=347).
UOCALC_SUBTOTAL = (347, 341)
UOSHOW_TITLE_F = (0.505, 0.417)  # the "Click to add title" placeholder, as a
                                 # FRACTION of the slide page (measured at
                                 # 640x400, resolved live by slide_point)


def uof_open_row(d, row):
    """Click file-list row `row` in the shared Open dialog, then Open.

    The list mirrors the selected row into the File-name field, and arrow keys
    never reach the list in UnoWord/UnoCalc, so clicking the row is the only
    way in. Coordinates come from d.dlg(), which re-derives the dialog origin
    for the live resolution."""
    d.click(*d.dlg("row0", row), settle=0.7)
    d.click(*d.dlg("open"), settle=2.5)


def s04_pre(d):
    """Stage the RAM copies for the Open dialog before the stream rolls (a
    base64 `put` on camera is nothing to look at). ONLY the small two: the
    RAM disk's per-file cap is 256 KB (pc64_io.c FILE_MAX), which refuses
    pic.doc and small.ppt. Push order is list order, and README.TXT is
    seeded first, so the dialog rows are RESUME.DOC 1, BUDGET.XLS 2."""
    if not getattr(d, "office_staged", None):
        print("  s04: SKIP - office corpus not staged")
        return False
    pushed = list(getattr(d, "ram_pushed", []))
    for name in ["RESUME.DOC", "BUDGET.XLS"]:
        src = dict((s.upper(), office_src(s))
                   for s, _ in OFFICE)[name]
        d.link.push_file(0, name, src)
        pushed.append(name)
    # s07 opens a source file out of the SAME pane this fills, and the pane
    # lists in creation order - so who pushed what, in what order, is state
    # the later scene needs. Recorded here rather than re-derived there.
    d.ram_pushed = pushed
    return True


def s04_office(d):
    """Files shows the staged docs on the RAM volume; UnoWord opens fmt.doc
    with its formatting visible; UnoCalc opens formulas.xls and selects a
    formula cell.

    Trimmed from 65 s to ~40 s against measured per-beat costs (2026-08-07).
    Two beats went:
      - the select-all sweep (-7.8 s), which read as a SELECTION rather than
        the scroll it stood in for (UnoWord scrolls by mouse wheel only and
        URC has no wheel injection). A still hold on the open document shows
        the formatting better and costs a third as much.
      - the whole UnoShow block (-19.7 s): it was already the degraded
        substitute for the broken Open path (see the requests entry), it cost
        17 s of the 65, and the narration does not claim a presentation app."""
    d.beat("files-sees-the-documents")
    d.launch("files", settle=2.5)                    # RAM: README + the 2 docs
    time.sleep(2.0)                                  # FMT.DOC/FORMULAS.XLS
    d.ctrl("w", settle=1.0)

    # A CV, not the parser fixture. fmt.doc's text is literally "a BOLDWORD z"
    # and "CENTREPARA" - it exists to carry one of every formatting property
    # for unodoc's tests, and on camera it says nothing to a viewer. The CV
    # (tools/demo/assets/resume.doc, built by mkdemo_doc.py) uses the same
    # properties - a name at 22pt bold, right-aligned contact details, small
    # caps headings, italic dates, justified body - in a shape anyone
    # recognises at a glance.
    d.beat("unoword-open-the-cv")
    d.launch("uoword", settle=3.0)
    d.ctrl("o", settle=1.4)
    uof_open_row(d, 1)                               # RESUME.DOC (RAM row 1)
    time.sleep(3.5)                                  # hold: the whole page
    d.ctrl("w", settle=1.2)

    # A budget, not the decompiler fixture. formulas.xls is a column of
    # one-cell expressions with their operands beside them, because its job is
    # to make unodoc rebuild every formula shape BIFF8 can store; on camera it
    # is a column of numbers nobody can read. budget.xls (mkdemo_sheet.py) has
    # quantity times price on each line, a SUM over the lines, tax off the
    # subtotal and a total that adds the two - so the cell this clicks holds a
    # formula a viewer can check in their head.
    d.beat("unocalc-open-the-budget")
    d.launch("uocalc", settle=3.0)
    d.ctrl("o", settle=1.4)
    uof_open_row(d, 2)                               # BUDGET.XLS (RAM row 2)
    d.beat("click-a-formula-cell")
    d.click(*UOCALC_SUBTOTAL, settle=1.5)            # formula bar: =SUM(D3:D7)
    time.sleep(2.5)                                  # hold on the formula bar
    # No final Ctrl-W: reset() closes UnoCalc after the stream stops.

    # UnoShow is DELIBERATELY not in this scene any more (trimmed 2026-08-07).
    # It could not open small.ppt in this build - the native-FAT lister marks
    # every file as a directory and UnoShow's dialog bridge swallows Backspace,
    # so neither clicking a row nor typing a name reaches the file (both filed
    # in pc64/UNOAUTOMATE-REQUESTS.md, 2026-08-07). The standing beat was the
    # degraded substitute, authoring a titled slide, and at ~17 s of a 65 s
    # scene it was the most expensive thing here for the least payoff.
    #
    # To RESTORE it once those two bugs land, the beat is:
    #     d.launch("uoshow", settle=4.5)
    #     d.ctrl("o"); ... uof_open_row(d, <row>)      # the real Open path
    # `Demo.slide_rect` / `slide_point` / `UOSHOW_TITLE_F` are kept for it -
    # they locate the slide page on a live grab, which is what makes any
    # UnoShow beat survive a resolution change.


# The browser's maximize box. The Browser window opens at a FIXED origin and
# a FIXED size (it is not sized from the framebuffer), so like Studio's menu
# bar this is window-relative and holds at any desktop size - verified on a
# 1280x800 probe shot, where the box sits at the window's top right.
BROWSER_MAX_XY = (461, 27)

# The real site. `https://en.wikipedia.org/wiki/Unix` is what a viewer
# recognises, and the page loads whole (HTTP/1.1 200 OK, the full article and
# its table of contents are all in the document - the old 48 KB cut is gone,
# DOC_MAX/RAW_MAX are 1 MB now). What it costs is scrolling: Wikipedia's
# Vector skin emits the entire navigation sidebar BEFORE the article, so the
# <h1> is about seven PgDn down. That is why the window is maximized first.
WIKI_URL = "https://en.wikipedia.org/wiki/Unix"
WIKI_PGDN = 9                                    # measured: lands on the <h1>


def s05_browser(d, with_net=False):
    """The browser: local JS pages and the engine switch, then a REAL site.

    The network half was `--with-net`-gated and metal-only until 2026-08-08.
    It works here: the URC link itself is TCP over the same stack, so the
    guest is leased and routed before a scene ever runs (`nonet` in DEBUG.CFG
    only skips the boot NET TEST and the desktop's net_boot fallback, not the
    stack), and slirp's DNS resolves for a DEBUG image."""
    def goto(loc, settle=2.2):
        d.ctrl("l", settle=0.4)
        d.key(0, S_END, settle=0.1)
        # No select-all in the address bar. 40 is enough for every location
        # this scene types (the longest is the 38-character Wikipedia URL, and
        # nothing is typed after it), and each extra backspace is a URC round
        # trip - 20 of them across six navigations is five seconds of nothing.
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
    # ---- and now the open internet ------------------------------------
    d.beat("maximize-for-the-web")
    d.click(*BROWSER_MAX_XY, settle=1.2)
    d.beat("load-wikipedia")
    goto(WIKI_URL, settle=1.0)
    d.wait_stable(timeout=28.0)                      # TLS + a 200 KB document
    time.sleep(1.0)                                  # hold on the loaded page
    d.beat("scroll-to-the-article")
    for _ in range(WIKI_PGDN):
        d.key(0, S_PGDN, settle=0.45)
    time.sleep(1.8)                                  # hold on the Unix heading
    if with_net:
        d.beat("net-beats-are-in-the-spine-now")     # --with-net is a no-op
    d.beat("close")
    d.ctrl("w", settle=1.0)


STUDIO_FILE_XY = (54, 48)      # "File" menu title (d4_studio_filemenu)
STUDIO_NEW_XY = (60, 68)       # its "New" row

# The Project pane. Studio opens at a FIXED (24, 20) and the pane is laid out
# from that corner, so these are window-relative (SCENES.md class 1) and were
# read off a 1280x800 probe shot: row 0 at y=108, one mono line-height apart.
STUDIO_PROJ_X = 90
STUDIO_PROJ_Y0 = 108
STUDIO_PROJ_DY = 16

# The live edit. A COMMENT: it changes the file visibly, it is the one edit
# that cannot break the build, and every character of it renders (a comment is
# one highlighter span). Kept short so the scene spends its time on the build,
# not on typing - each character is a URC round trip.
STUDIO_EDIT_C = "/* a live edit, compiled on the box */\n"
STUDIO_EDIT_PY = "# the same IDE, the other language\n"


# ---------------------------------------------------------------------------
# THE URC `py` VERB POISONS STUDIO'S Ctrl-R. Read this before adding a scene.
# ---------------------------------------------------------------------------
# Measured 2026-08-08, three boots, and it is the single worst failure in this
# file because it is TOTALLY SILENT: the machine stops answering URC, stops
# sending video, and never comes back. The beat list runs on to the end against
# a dead box and the recording just stops mid-scene.
#
#   `py` (e.g. uno.size) ................. then Studio ^R on a .PY  -> WEDGED
#   nothing ............................... then Studio ^R on a .PY  -> fine
#
# It is not the app, the file, or the .UNO name: the identical ^R on the
# identical file works perfectly on a boot where the `py` verb was never used.
# What differs is who initialised PYRT's MicroPython VM first - pyrt_ensure()
# runs py_init() once and py_init records a stack top (pyrt_set_stack_top,
# apps/pyrt.c) on THAT call path. Entered later from the shell's key dispatch,
# which is a completely different stack, it never returns.
#
# Four workarounds were tried and all four still wedged: opening and closing a
# tiny PYAPP first; leaving one open on another desktop; leaving one open and
# DRAWN on the current desktop; and pre-packing the very same .UNO on the host
# and run_app-ing it first. Only "do not touch `py` first" works.
#
# THE RULE: a scene that presses Ctrl-R must run before any `py` traffic in the
# session, and its own `pre` must not use `py` either. `pyeval` below is the
# one door to the verb, so the flag it sets is complete, and s07/s09 shout if
# the rule has already been broken. Filed for the Studio/PYRT lane in
# pc64/UNOAUTOMATE-REQUESTS.md.
def pyeval(d, src, **k):
    """The ONE way this file reaches the URC `py` verb - see above."""
    d.py_used = True
    return d.link.eval(src, **k)


def needs_ctrl_r(d, scene):
    if getattr(d, "py_used", False):
        print("  %s: WARNING - the `py` verb has already run this session, so "
              "Ctrl-R will wedge the box. Record this scene before anything "
              "that uses `py` (s08/s13/s14)." % scene)


def esp_vol(d):
    """The volume the staged ESP tree mounts as, found once and cached. On
    metal probe_metal_assets() has already set it."""
    v = getattr(d, "_esp_vol", None)
    if v is None:
        v = next((x["vol"] for x in d.link.vols()
                  if x["kind"] == 1 and x["name"].strip() in ("NO NAME", "")), 1)
        d._esp_vol = v
    return v


def studio_row_of(d, name):
    """Which Project-pane row `name` is on.

    The pane lists ONE volume's root in CREATION order (refresh_project,
    studio.c), the OS seeds README.TXT at index 0, and s04_pre/s07_pre push
    after it - so the row is a function of what has actually been pushed, in
    order, which `d.ram_pushed` records. Never a literal."""
    ram = ["README.TXT"] + list(getattr(d, "ram_pushed", []))
    return ram.index(name)


def s07_pre(d):
    """Push the SDK's SAMPLE.C **and SAMPLE.PY** onto the RAM volume, off
    camera - the two halves of the same bouncing-ball app, one per language.

    WHY THE RAM VOLUME AND NOT THE ESP. Studio's Project pane lists exactly
    one volume - `proj_vol = ed_vol`, and at first open ed_vol is -1, so
    refresh_project() falls back to "the first WRITABLE volume", which is the
    RAM disk (the one carrying README.TXT). The ESP copy stage_sdk() makes is
    therefore invisible in the pane; the file has to be here to be openable.

    Order matters: the pane lists in creation order, README.TXT is seeded by
    the OS at index 0 and s04_pre pushes ahead of us, so the row index is
    computed from what has actually been pushed rather than assumed."""
    needs_ctrl_r(d, "s07")
    pushed = list(getattr(d, "ram_pushed", []))
    for name in ("SAMPLE.C", "SAMPLE.PY"):
        src = os.path.join(PC64, "sdk", name)
        if not os.path.exists(src):
            print("  s07: SKIP - pc64/sdk/%s is missing" % name)
            return False
        # push_file finalises with a READ-BACK VERIFY on the device and only
        # then reports "verified", so its return value is the landing check.
        # It used to be a `py` call to uno.size - which is exactly the traffic
        # that makes this scene's Ctrl-R kill the machine (see above).
        if not d.link.push_file(0, name, src):
            print("  s07: SKIP - %s did not verify on the RAM volume" % name)
            return False
        pushed.append(name)
    d.ram_pushed = pushed
    d.studio_row = studio_row_of(d, "SAMPLE.C")
    d.studio_py_row = studio_row_of(d, "SAMPLE.PY")
    print("  s07: SAMPLE.C row %d, SAMPLE.PY row %d"
          % (d.studio_row, d.studio_py_row))
    return True


def studio_open_row(d, row, hold=1.2):
    """Click Project row `row`, then Enter to open it.

    A row that is ALREADY selected activates on the FIRST click
    (studio.c: `if (row == proj_sel) proj_activate(); else proj_sel = row`)
    and proj_activate() sets g_focus = PANE_EDIT - so clicking row 0, which is
    pre-selected, opens README.TXT and every later key goes to the editor.
    Clicking a DIFFERENT row only selects it, and Enter (handled while
    g_focus == PANE_PROJ) is what opens it. Never call this on row 0."""
    d.click(STUDIO_PROJ_X, STUDIO_PROJ_Y0 + row * STUDIO_PROJ_DY, settle=1.1)
    d.key(13, settle=1.6)
    time.sleep(hold)


def s07_studio(d):
    """Studio: build and run the SAME app twice, once from UnoC and once from
    Python - open SAMPLE.C, edit it, ^S, ^B, ^R and the compiled app bounces;
    then open SAMPLE.PY, edit that, ^S, ^B, ^R and the Python one bounces.

    C AND PYTHON ARE BOTH FIRST-CLASS here and one language alone understates
    it, which is why this scene runs the whole loop twice. The two halves are
    genuinely different code paths in studio.c's do_build(): the C file goes
    through `ucc_compile` and reports "Built SAMPLE.UNO <summary>", the Python
    one through `studio_py_pack` and reports "Packed SAMPLE.UNO (Python - runs
    on PYRT.UNO)". Both write the same .UNO name (uno_name() cuts at the dot),
    which is fine because the second overwrites the first AFTER the first has
    been run and closed - and pc64_shell_run_user re-peeks the module flags, so
    the PYAPP bit routes it to PYRT rather than the classic loader.

    REWRITTEN 2026-08-08, because the old take shipped BROKEN. It typed a
    whole UnoC program in after File > New; the File > New click missed at
    1280x800 (the dropdown row is laid out from the mono line height, not from
    the 640x400 literal it was measured at), so the C source went into the
    SAMPLE.PY that Studio greets with, packed as a PYTHON app, and ended on
    "Run failed" with a SyntaxError. Nothing in the beat log said so - which is
    why BOTH halves here are verified from extracted frames, never from beats."""
    row_c = getattr(d, "studio_row", 3)
    row_py = getattr(d, "studio_py_row", 4)
    d.beat("launch-studio")
    d.launch("studio", settle=3.2)

    # ---- the C half ----------------------------------------------------
    d.beat("pick-sample-c-in-the-project")
    studio_open_row(d, row_c)
    d.beat("type-a-live-edit")
    d.text(STUDIO_EDIT_C, settle=0.06)
    d.beat("save")
    d.ctrl("s", settle=1.2)
    d.beat("build-the-c")
    d.ctrl("b", settle=0.2)
    time.sleep(3.2)                                  # ucc + the "Built" line
    d.beat("run-the-c")
    d.ctrl("r", settle=0.2)
    time.sleep(5.0)                                  # the window opens, the
                                                     # ball bounces

    # ---- and the same app again, in Python -----------------------------
    # Close the C app first: it holds the EX_USERAPP slot and its window sits
    # over Studio. close_top() targets exactly it (the app just launched IS the
    # top window), and the click on the Project row below re-focuses Studio, so
    # ^S/^B/^R reach Studio's key hook and not the app's.
    d.beat("close-the-c-app")
    d.close_top(settle=1.0)
    d.beat("pick-sample-py-in-the-project")
    studio_open_row(d, row_py, hold=1.5)
    d.beat("type-a-live-edit-in-python")
    d.text(STUDIO_EDIT_PY, settle=0.06)
    d.beat("save-the-python")
    d.ctrl("s", settle=1.2)
    d.beat("pack-the-python")
    d.ctrl("b", settle=0.2)
    time.sleep(2.2)                                  # studio_py_pack + the
                                                     # "Packed ..." line
    d.beat("run-the-python")
    d.ctrl("r", settle=0.2)
    time.sleep(6.0)                                  # PYRT opens the window and
                                                     # the Python ball bounces
    # THE BUILD LEFT A FILE ON THE VOLUME, and the next Studio scene indexes
    # its Project rows by position. SAMPLE.UNO is created by ^B (both halves
    # write the same name) and lands after SAMPLE.PY in creation order, so
    # every row below it shifts by one. s09 clicked the row it had computed
    # without this, opened the 2.5 KB BINARY sitting there instead of AUTO.PY,
    # and Studio said "Build failed: unexpected character in source" - on
    # camera, in a scene whose beat log was entirely green.
    d.ram_pushed = list(getattr(d, "ram_pushed", [])) + ["SAMPLE.UNO"]
    # No on-camera teardown: the scene ends on the app it just compiled,
    # running. reset() closes both windows after the stream stops.


def s15_arcade(d):
    """Ten seconds of a shipped game, as a breath between the boot and Duum.

    The cut went from the boot screen straight into Doom, which lands the
    film's biggest thing before a viewer knows what machine it is running on.

    PAC-MAN, and the two rejected alternatives are the interesting part.
    Runner3D was the first choice - real-time 3D with no graphics chip - and
    it is unusable here: it takes the display down to 320x200, so the stream
    reconnects mid-scene and the take arrives as three files at two
    resolutions. Dostris animates by itself too, but its music OUTLIVES it
    (a known open bug: the sequencer keeps playing after the window closes),
    and the very next scene records the guest's audio - it would have filmed
    Duum with Dostris playing underneath. Pac-Man stays in a desktop window,
    ticks on its own, and makes no sound at all.

    Launched by ID, never by row number: the app list is this boot's own."""
    d.beat("open-a-game")
    d.launch("pacman", settle=3.0)
    # It opens on a title screen that says "N: new game", so the first take
    # here was ten seconds of a static title. Press it, then actually play:
    # standing still would only show the ghosts closing in on a stationary
    # Pac-Man, which is a death rather than a game.
    d.key(ord("n"), settle=1.2)
    d.beat("the-game-runs", settle=0.3)
    for scan in (S_LEFT, S_LEFT, S_UP, S_RIGHT, S_RIGHT, S_DOWN, S_LEFT):
        d.key(0, scan, settle=0.85)
    d.beat("close-the-game", settle=0.3)
    d.ctrl("w", settle=1.2)


def s08_pre(d):
    """Guard: no WAD in pc64/wads means a clean no-op, never a download. If a
    WAD is present, pre-launch Duum here (before the stream) so its WAD
    directory parse + first raycast frame - slow under TCG - are not dead air
    on camera. Returns True to record if the Duum window came up."""
    if not getattr(d, "wad_staged", None):
        print("  s08: SKIP - no WAD (pc64/wads on QEMU, the stick's root on "
              "metal), scene no-ops by design")
        return False
    # THE PRE-LAUNCH IS GONE (2026-08-19), and so is the reason for it. This
    # used to fire uno.run_app() off camera because "a PYAPP has no Start-menu
    # row" - it is a document PYRT opens, so the shell gave it no way in and
    # the only honest launch was the call Files makes on a double-click. Duum
    # now carries an app descriptor and has a row and a desktop icon like
    # anything else, so the launch is a shot rather than a workaround, and it
    # happens on camera in s08_duum.
    return True


def s08_duum(d):
    """Duum is already up (s08_pre launched it pre-stream). Walk E1M1, shoot,
    and work a door, so the footage shows the GAME and not just the renderer.

    Rewritten 2026-08-18. The old version only walked, which was the whole of
    Duum at the time; it is now a full game (weapons, monsters that chase and
    shoot back, doors, lifts, pickups, the real STBAR, level progression), and
    a walk-only scene sells a renderer when there is a game to show.

    TURN COUNTS REWRITTEN 2026-08-18 (second pass). The old docstring's step
    sizes - "MOVE = 12 map units, TURN = 0.20 rad per press, a quarter turn is
    8 presses" - describe the step-per-keypress engine Duum USED to be. It now
    moves on HELD keys: one press marks the key held 0.30 s, which is ~96 map
    units or ~53 degrees, so the old 8-press turns were a 424-degree spin and
    the old 14-press walks overshot the level. Counts here are for the current
    engine.

    Also note the guest does NOT travel the same distance per press as the
    host shim: DUUM.PY clamps dt to 0.1 s, so at the guest's 5-14 fps its
    clock runs behind the harness's fixed 0.30 s sleeps. Anything aimed
    (firing) therefore goes STRAIGHT down the corridor rather than turning
    onto a mark, which is correct wherever the walk actually ends.

    The map is FREEDOOM's E1M1 (original geometry - only the format and
    texture names are shared with id's), so this is a corridor walk, not the
    old courtyard route.

    FOUR BEAT NAMES ARE LOAD-BEARING - duum-running, walk-into-the-room,
    turn-back-left and look-around each carry a narration cue in
    vo_script.json. Renaming one does not fail: mux_vo reports it and drops
    the line to the top of the scene, which is worse than an error because the
    film still builds. Add beats freely, rename these only with the script.

    EVERY TURN IS A LOOK-AND-RETURN. The corridor is barely wider than the
    player, and a single press is ~53 degrees, so turning and then WALKING
    puts you into a wall within two presses - the first take of this route
    spent 20 of its 28 seconds facing a grey wall. Turning and immediately
    turning back shows the world moving without ever leaving the corridor
    heading, which is what makes the walk safe wherever it actually ends.
    """
    # Launch it on camera, from its own row. This is the shot the scene never
    # had: the menu rises, walks to Duum, and the game comes up - which is
    # also the only visible proof of the app-descriptor work, since what a
    # descriptor buys is precisely the row being there at all.
    #
    # menu_index() resolves the row by ID against the app list the LIVE
    # machine reported at boot, never a baked count. Adding an app moves every
    # row below it, and a hardcoded number does not fail when that happens -
    # it opens the app next door and films it under the wrong name.
    d.beat("duum-from-the-menu")
    d.key(0, S_ESC, 1, settle=1.4)                   # Ctrl-Esc; menu tween
    for _ in range(d.menu_index("duum")):
        d.key(0, S_DOWN, settle=0.20)
    d.beat("launch-duum", settle=0.3)
    d.key(13)                                        # Enter
    # The WAD directory parse and the first raycast frame are real work and
    # they are not instant. Wait for the WINDOW rather than a fixed sleep, so
    # the beat carrying "And this is Doom." lands on the game being up rather
    # than on a desktop still opening it.
    for _ in range(40):
        if any("DUUM" in t.upper() for t in d.windows()):
            break
        time.sleep(1.0)
    else:
        print("  s08: Duum window never appeared - recording anyway")

    d.beat("duum-running")
    time.sleep(2.5)                                  # the status bar, before anything moves
    # RE-TUNED 2026-08-19 against a build where Duum plays the WAD's music.
    # The old 8 + 4 + 3 walk was authored for a silent guest; the MIDI synth
    # is real work on the same core, the frame rate drops, and DUUM.PY clamps
    # dt to 0.1 s - so each 0.30 s hold travels a different distance than it
    # used to and the take spent its whole middle facing a wall. 4 presses
    # reach the hall and 7 reach the corridor with the zombies in frame,
    # measured on this build in the Duum film's own scenes.
    # WALK FIRST, TURN LAST. A look-and-return between two walks reads well on
    # paper and does not survive this guest: the two presses are the same 0.30 s
    # hold, but they are consumed at different frame times, so the heading comes
    # back slightly off and the next three presses go into the room's far wall.
    # Three takes ended against one. Everything that moves the player now
    # happens before anything that turns them.
    d.beat("walk-into-the-room")
    for _ in range(4):
        d.key(0, S_UP, settle=0.30)
    d.beat("hold-the-hall", settle=1.2)              # was a look-and-return
    d.beat("walk-on")
    for _ in range(3):
        d.key(0, S_UP, settle=0.30)
    d.beat("fire-the-weapon")
    time.sleep(1.4)                                  # let a chaser walk into frame
    for _ in range(6):
        d.key(ord("f"), settle=0.45)                 # flash, recoil, ammo drops
    d.beat("turn-back-left")
    d.key(0, S_LEFT, settle=0.55)                    # look left...
    d.key(0, S_RIGHT, settle=0.35)                   # ...and back
    # NO WALKING AFTER THE FIREFIGHT. The firefight already happens in the
    # open room with the barrels, a step from its far wall, and every take
    # that moved afterwards closed on magnified grey texels with the last
    # narration line playing over them.
    d.beat("walk-through", settle=1.0)
    d.beat("work-a-door")
    for _ in range(2):
        d.key(ord(" "), settle=0.80)                 # use: doors, lifts, switches
    # END IN THE CORRIDOR, not against a wall. The first two takes kept
    # walking after the firefight and finished jammed into a corner, where a
    # wall a few units from the eye fills the frame with magnified texels and
    # the player has bled to a third of their health. The closing narration
    # lands here, so it holds on the corridor instead.
    # Turn AWAY from the wall to close on, and stay turned. The player is a
    # step from the room's far side by now, so a look-and-return finishes
    # facing crates a few units from the eye; two lefts point the camera back
    # across the room it just walked through, which is where the last line of
    # narration wants to be.
    d.beat("look-around")
    d.key(0, S_LEFT, settle=0.55)
    d.key(0, S_LEFT, settle=0.55)
    time.sleep(2.5)
    # No on-camera teardown: reset() closes Duum after the stream stops.


# ---------------------------------------------------------------------------
# s09 - unoautomate, driven by Python
# ---------------------------------------------------------------------------
# The demo is assets/AUTO.PY: a Python APP that first OBSERVES the running
# system through `unoauto.probe()` (heap, filesystems, the NIC's frame
# counters, the live window list) and then DRIVES it through
# `unoauto.key()` - it opens the Start menu with Ctrl+Esc, walks down to
# Clock and presses Enter - and finally probes again, so its own effect shows
# up in its own output.
#
# WHY THIS ONE. The brief was the most VISUALLY LEGIBLE demonstration, and
# the constraint that decides it is that the viewer must see the script and
# see it act IN THE SAME FRAME. Three candidates were considered:
#   - `unoscript` from the shell: legible, but its effect is text in a pane,
#     so there is nothing to watch.
#   - a driver-side script over URC: that is every other scene already, and
#     the Python would be off-screen on the host.
#   - THIS: the script is open in Studio on the left, the app it becomes is
#     printing its own transcript on the right, and the Start menu opens by
#     itself in between. Keyboard injection was chosen over `unoauto.launch`
#     (which takes an app INDEX and does its work invisibly) precisely because
#     a menu rising and walking on its own is the thing you can SEE.
#
# The script paces itself on `unoauto.uptime()`, in milliseconds, and never on
# a frame count: the guest's frame rate under TCG is neither known nor stable,
# so a frame-counted script is a different length on every run.
AUTO_PY = os.path.join(ASSETS, "AUTO.PY")


def s09_pre(d):
    """Push AUTO.PY onto the RAM volume with Clock's REAL Start-menu row
    substituted in, off camera.

    The row is not a constant: the Start menu is built from the app table, so
    the walk length depends on which apps this build ships. `menu_index` reads
    it from the live `apps list`, exactly as s02 does for Files."""
    if not os.path.exists(AUTO_PY):
        print("  s09: SKIP - %s is missing" % AUTO_PY)
        return False
    needs_ctrl_r(d, "s09")
    # STUDIO'S PANE FOLLOWS ed_vol, AND THE GREET PUTS ed_vol ON THE ESP.
    # refresh_project() sets `proj_vol = ed_vol`, and on the FIRST open it runs
    # BEFORE the greet (ed_vol == -1 -> the RAM disk), which is why s07 sees the
    # RAM listing. But the greet then loads SDK\SAMPLE.PY off the ESP and sets
    # ed_vol to it - so on any LATER open the pane lists THE ESP ROOT instead,
    # and a row index computed for the RAM disk opens whatever the ESP happens
    # to have there. Measured: it opened DOOM1.WAD, 11 MB of binary, and Studio
    # answered "Build failed: unexpected character in source".
    # What pins it back is opening a file ON the RAM disk, which s07 does. In
    # the spine s07 always runs first; running s09 alone, do the same off
    # camera, on a fresh Studio whose pane is still the RAM disk.
    if "SAMPLE.C" not in list(getattr(d, "ram_pushed", [])):
        if not s07_pre(d):
            return False
        d.launch("studio", settle=3.2)
        studio_open_row(d, studio_row_of(d, "SAMPLE.C"), hold=0.4)
        d.close_all()
    row = d.menu_index("clock")
    with open(AUTO_PY) as f:
        src = f.read()
    out, hit = [], False
    for line in src.splitlines(True):
        if line.startswith("CLOCK_ROW"):
            line = ("CLOCK_ROW = %d" % row).ljust(33) + \
                   "# Clock's row in the Start menu\n"
            hit = True
        out.append(line)
    if not hit:
        print("  s09: SKIP - no CLOCK_ROW line in AUTO.PY to substitute")
        return False
    tmp = os.path.join(OUT, "AUTO.PY")
    with open(tmp, "w") as f:
        f.write("".join(out))
    if not d.link.push_file(0, "AUTO.PY", tmp):   # push_file read-back-verifies
        print("  s09: SKIP - AUTO.PY did not verify on the RAM volume")
        return False
    d.ram_pushed = list(getattr(d, "ram_pushed", [])) + ["AUTO.PY"]
    d.auto_row = studio_row_of(d, "AUTO.PY")
    print("  s09: AUTO.PY on the RAM volume, Project row %d, "
          "Clock is Start-menu row %d" % (d.auto_row, row))
    return True


def s09_automate(d):
    """Open AUTO.PY in Studio, read it, run it - and watch the machine drive
    itself while the source stays on screen beside its output."""
    row = getattr(d, "auto_row", 5)
    d.beat("open-the-automation-script")
    d.launch("studio", settle=3.2)
    studio_open_row(d, row, hold=2.0)
    d.beat("read-the-script")
    for _ in range(6):                               # scroll into script()
        d.key(0, S_DOWN, settle=0.18)
    time.sleep(1.8)                                  # hold on the script body
    d.beat("run-it")
    d.ctrl("r", settle=0.2)
    time.sleep(2.5)                                  # pack + PYRT opens it

    # PYRT builds its window at a FIXED (40, 24), 544x420 at this desktop size
    # (pyrt.c tr_build: 520x380 of canvas, capped), directly on top of Studio's
    # (24, 20). Slide it right so the SOURCE and the TRANSCRIPT are both
    # readable - which is the whole point of the shot. Its title bar is the
    # ~22 px under y=24, and it is the topmost window there, so the grab cannot
    # land on Studio.
    #
    # IT MUST FINISH BEFORE THE SCRIPT TOUCHES THE SHELL. Pressing on this
    # title bar takes the keyboard focus, and the first take's drag landed
    # while the Start menu was already up: the menu lost focus, every injected
    # Down and the Enter went to this window instead, Clock never opened - and
    # the transcript still printed "the machine opened that one itself",
    # because the script has no way to know. AUTO.PY's observe phase is 8.5 s
    # for exactly this reason; the drag costs about 5.
    d.beat("put-them-side-by-side")
    d.drag(250, 35, 900, 60, settle=1.0)
    d.beat("the-machine-drives-itself")
    time.sleep(13.0)                                 # Ctrl+Esc, the walk down,
                                                     # Enter, and the re-probe
    time.sleep(1.5)                                  # hold on the final list


def s14_pre(d):
    """s14's UNRECORDED half: boot the guest and wait for its shell. Returns
    True to record the console half, False to skip the scene.

    The guest gets ~4 ms of every ~16 ms frame, so its boot takes minutes -
    which is dead air on video. So it boots BEFORE the stream starts.

    Runs ONLY with UNO_DEMO_KVM=1 (see boot_qemu above): under plain TCG the
    hypervisor is never eligible (TCG silently drops vmx, -m 512 is under the
    1800 MB carve floor, and eligibility needs a UNO_DETACH=1 build), so the
    scene logs the reason and no-ops rather than recording a refusal."""
    if not getattr(d, "vm_staged", None):
        print("  s14: SKIP - no bzImage/initrd.gz under pc64/build")
        return False
    if not DEMO_KVM:
        print("  s14: SKIP - hypervisor ineligible under TCG; set "
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
        _, w, h, rgba = d.shot("s14_boot_poll")
        region = bytes(rgba[len(rgba) // 3: 2 * len(rgba) // 3])
        if region == prev:
            settled += 1
            if settled >= 3:                         # quiet for ~30 s
                break
        else:
            settled = 0
        prev = region
    return True


def s14_console(d):
    """s14's RECORDED half (the stream starts between the two)."""
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


# ---------------------------------------------------------------------------
# s13 - a real SSH session, to a real machine on the LAN
# ---------------------------------------------------------------------------
# The SSH window is built at a FIXED origin (40, 30) with a 470x300 canvas
# (pc64_uui.c, EX_SSH), so everything inside it is SCENES.md class 1 -
# window-relative and resolution-independent. These were read off a 1280x800
# probe shot (out/probe/s13_*.png).
SSH_SESS_ROW0 = (150, 136)              # "devbuntu", row 0 of the Sessions pane
SSH_TAB_PLUS = (194, 80)                # the "+" after the Manage tab: CONNECT
SSH_MAX_XY = (507, 43)                  # the title bar's maximize box

# What the session runs on camera. Plain text only: the terminal is a
# scrollback of lines with NO escape parsing at all (draw_term in sshapp_ui.c
# walks newlines and turns \r and \t into spaces), so anything full-screen -
# vim, htop, top - would paint its control codes as literal rubbish.
#
# That is also why the first line is `exec sh`. Ubuntu 26.04's bash decorates
# every single prompt with an OSC 133 shell-integration blob - a 200-character
# `]3008;start=...;machineid=...;bootid=...` string before each command and an
# `end=...;exit=success` after it - plus bracketed-paste markers and SGR colour
# runs, and the first take rendered all of it verbatim between the answers.
# Plain POSIX sh has no PROMPT_COMMAND, no PS0, and no readline, so it emits
# none of it: from the second prompt on the transcript is exactly the commands
# and their output. (`PS1='$ '; PS0=; unset PROMPT_COMMAND; bind 'set
# enable-bracketed-paste off'` also works and is 70 characters to type.)
SSH_SETUP = "exec sh"
SSH_CMDS = ["hostname", "uname -a", "uptime", "ls /"]


def s16_unocode(d):
    """UnoCode: the workbench, a VS Code theme applied from an extension, an
    extension's JavaScript running, and the integrated terminal.

    It sits after s07 because the two say different things and the order
    matters: Studio is "this machine compiles for itself", UnoCode is "and the
    editor around that is a real one, that real VS Code extensions load into".
    Leading with UnoCode would make Studio look like the lesser editor, which
    is backwards - Studio is the thing with the compiler behind it.

    WHAT IS DELIBERATELY NOT FILMED. The extension host's fuel bound is the
    most interesting thing in the subsystem and it is invisible: what it does
    is NOT hang the desktop, and a shot of a desktop that has not hung is a
    shot of nothing. It stays in the narration for the Extensions view, which
    at least shows an extension's measured activation time on its row.

    KEY ENCODING. UnoCode reads Shift off the CASE of the character (the
    header of unocode/uc_main.c), so d.ctrl("P") IS Ctrl+Shift+P - the command
    palette - while d.ctrl("p") is Go to File. Two different commands, one
    letter apart in case only, and getting it wrong opens the wrong overlay and
    types the filter text into a document. Both are used below, on purpose.

    THE THEME IS APPLIED FROM THE TERMINAL, not the palette. The palette's
    theme picker needs an arrow walk through a list whose length depends on how
    many themes the extensions contributed, and a scene that counts Downs
    lands on the wrong row the moment anybody adds one - the same trap
    docs_shots.py's roster check exists for. `theme <name>` names it.

    TWO THINGS THE FIRST TAKE (2026-08-21) GOT WRONG, both invisible in the
    beat log and both obvious in an extracted frame:

    1. It switched straight to Nord, and UnoCode's DEFAULT theme is already
       dark. Two dark themes one after the other is a change only a diff can
       see, and the narration was going to claim the workbench repaints. So it
       now goes to the built-in LIGHT theme first: dark -> light is
       unmistakable, and Nord afterwards is then a visible second change AND
       the one that is actually an extension's file.
    2. Ctrl+` was pressed a SECOND time for the closing command, and
       uc_toggle_panel() closes the panel when it is already visible, on the
       terminal tab, and focused - which it was. The `js 6*7` went nowhere, the
       beat logged fine, and the frame shows a closed panel. Ctrl+` is now
       pressed EXACTLY ONCE, while the panel is shut, and every terminal
       command runs consecutively without anything stealing focus in between.

    Paced for video: every hold below is a viewer reading the screen, not a
    settle a test would need."""
    d.beat("open-unocode")
    d.launch("unocode", settle=6.0)
    time.sleep(3.5)                                  # the workbench, whole

    # Go to File, which is how anybody actually opens something here.
    d.beat("go-to-a-file-by-name")
    d.ctrl("p", settle=1.2)
    d.text("SAMPLE", settle=0.12)
    time.sleep(1.4)                                  # the filtered list
    d.key(13, settle=2.0)
    time.sleep(2.6)                                  # the file, highlighted

    d.beat("the-command-palette")
    d.ctrl("P", settle=1.2)                          # Ctrl+Shift+P
    d.text("theme", settle=0.14)
    time.sleep(2.0)                                  # the filtered commands
    d.key(0, scan=S_ESC, settle=1.0)

    d.beat("an-extension-runs-javascript")
    d.ctrl("P", settle=1.2)
    d.text("Say Hello", settle=0.14)
    time.sleep(1.2)
    d.key(13, settle=1.5)                            # activate + run + notify
    time.sleep(3.2)                                  # hold on the notification

    # ONE Ctrl+` for the rest of the scene. See note 2 above.
    d.beat("the-terminal-evaluates-javascript")
    d.ctrl("`", settle=1.4)                          # the integrated terminal
    d.text("js 6*7", settle=0.14)
    d.key(13, settle=1.0)
    time.sleep(2.8)                                  # 42, on the machine

    d.beat("the-editor-is-themeable")
    d.text("theme Light+", settle=0.12)              # uc_theme_select is
    d.key(13, settle=1.0)                            # forgiving about the
    time.sleep(3.0)                                  # "(default light)" suffix

    # The money shot: a colour theme written for VS Code, unmodified, repaints
    # the whole workbench - and this one came out of EXT\NORD, not the build.
    d.beat("a-vs-code-theme-repaints-it")
    d.text("theme Nord", settle=0.12)
    d.key(13, settle=1.0)
    time.sleep(3.4)                                  # hold on the new colours
    # No teardown on camera: reset() closes the window after the stream stops.


def s13_pre(d):
    """Author SSHSTORE.DAT on the host and stage it onto the volume the device
    will look for it on.

    THERE IS NO OTHER WAY IN. `sshapp_ui.c` lists sessions and keys and
    connects to the selected one; it has no "add session" and no "import key"
    control. `unossh_cmd.c` implements a full `ssh` verb (keygen / sessadd /
    run), and its header says unoautomate "lands a weak stub and a four-line
    dispatch clause once" - but `grep ssh unoauto_remote.c` is EMPTY, so that
    clause was never landed and the verb is unreachable over URC. The only
    other callers of ssh_sess_set / ssh_key_import in the tree are SPECTEST
    suites that seed a fixed 10.0.2.2:2222 session. So the store is authored
    off-device (sshstore.py) and pushed, which is staging, not faking: the
    device then loads, decrypts and uses it through its own code.

    THE VOLUME IS THE WHOLE TRICK. unossh_store.c's pick_vol() prefers the
    first NATIVE-FAT writable volume and only falls back to the RAM disk, and
    store_load() silently replaces a file that is not exactly sizeof(ssh_store)
    bytes with an empty store - so a push to the wrong volume presents as "no
    saved sessions", not as an error. pick_vol is reproduced here against the
    LIVE volume list rather than assumed."""
    if not os.path.exists(SSH_KEY):
        print("  s13: SKIP - no private key at %s (set UNO_DEMO_SSH_KEY). It "
              "must be an UNENCRYPTED ed25519 key whose public half is in "
              "%s@%s:~/.ssh/authorized_keys" % (SSH_KEY, SSH_USER, SSH_HOST))
        return False
    try:
        import sshstore
    except Exception as e:                           # noqa: BLE001
        print("  s13: SKIP - cannot import sshstore (%s)" % e)
        return False
    try:
        with open(SSH_KEY) as f:
            blob = sshstore.build(f.read(), SSH_KEYNAME, SSH_SESS,
                                  SSH_HOST, SSH_PORT, SSH_USER)
    except Exception as e:                           # noqa: BLE001
        print("  s13: SKIP - %s" % e)
        return False
    # pick_vol(), against the live list: a real partition first, then anything
    # writable above the RAM disk, then the RAM disk.
    vols = d.link.vols(timeout=15)
    tgt = next((v["vol"] for v in vols
                if v["vol"] > 0 and v["kind"] == 1 and v["writable"]), None)
    if tgt is None:
        tgt = next((v["vol"] for v in vols
                    if v["vol"] > 0 and v["writable"]), 0)
    tmp = os.path.join(OUT, "SSHSTORE.DAT")
    with open(tmp, "wb") as f:
        f.write(blob)
    # push_file read-back-verifies on the device before reporting "verified",
    # which is the check that matters: store_load() accepts the file ONLY at
    # exactly sizeof(ssh_store) and silently empties the store otherwise.
    if not d.link.push_file(tgt, "SSHSTORE.DAT", tmp):
        print("  s13: SKIP - the store did not verify on vol %d" % tgt)
        return False
    print("  s13: SSHSTORE.DAT (%d bytes) on vol %d, session %r -> %s@%s:%d"
          % (len(blob), tgt, SSH_SESS, SSH_USER, SSH_HOST, SSH_PORT))
    return True


def s13_ssh(d):
    """Connect to devbuntu over SSH and run a few commands whose output could
    only have come from another machine.

    QEMU's SLIRP is outbound-only, which is fine: this dials OUT to a LAN
    address, exactly as the URC link itself does."""
    d.beat("open-the-ssh-client")
    d.launch("ssh", settle=2.5)
    time.sleep(1.5)                                  # hold on Sessions + Keys
    d.beat("pick-the-saved-session")
    d.click(*SSH_SESS_ROW0, settle=1.2)
    d.beat("connect")
    d.click(*SSH_TAB_PLUS, settle=1.0)
    # ssh_connect + handshake + auth run SYNCHRONOUSLY inside the click
    # handler (sshapp_ui.c connect_selected), so the desktop holds one frame
    # for the whole exchange - curve25519 + ed25519 under TCG is seconds.
    time.sleep(9.0)
    d.beat("a-shell-on-another-machine")
    # The window is only 470x300 (EX_SSH's fixed size) and the remote output is
    # the point of the scene, so give the terminal the whole desktop. Maximize
    # AFTER connecting: the "+" is laid out from the left of the tab strip and
    # does not move, but there is no reason to risk it.
    d.click(*SSH_MAX_XY, settle=1.5)
    time.sleep(1.5)
    d.beat("plain-shell")
    d.text(SSH_SETUP, settle=0.07)
    d.key(13, settle=0.4)
    time.sleep(2.0)
    for cmd in SSH_CMDS:
        d.beat("run-" + cmd.split()[0])
        d.text(cmd, settle=0.07)
        d.key(13, settle=0.4)
        time.sleep(2.6)                              # the answer arrives on the
                                                     # next pump_connections()
    time.sleep(2.0)                                  # hold on the transcript
    # No on-camera teardown: reset() closes the window after the stream stops.


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
def poster_pre(d):
    """Stage the still the website uses as the film's poster: Duum in front,
    a formatted Word document open behind it.

    A poster is a screenshot like any other here, so it is SCRIPTED rather
    than cropped out of the footage by hand - rerun it and you get the same
    frame. It wants both halves of the argument in one picture: the thing
    that makes people look (a Doom level, textured, with the status bar) and
    the thing that makes them stay (a real .doc, bold and centred, in a word
    processor), on one desktop at the same time.

    Everything happens here, before the stream, because the scene body only
    has to hold still long enough to grab the frame.
    """
    if not getattr(d, "office_staged", None):
        print("  poster: SKIP - office corpus not staged")
        return False
    if not getattr(d, "wad_staged", None):
        print("  poster: SKIP - no WAD, and Duum is the front half of the shot")
        return False

    # Behind: UnoWord with FMT.DOC, the document whose own text names its
    # formatting. Pushed to RAM first, exactly as s04 does it.
    d.link.push_file(0, "FMT.DOC",
                     dict((x.upper(), office_src(x))
                          for x, _ in OFFICE)["FMT.DOC"])
    d.launch("uoword", settle=3.0)
    d.ctrl("o", settle=1.4)
    uof_open_row(d, 1)
    time.sleep(3.0)

    # In front: Duum. run_app is the same call a double-click in Files makes.
    pyeval(d, 'import uno; uno.run_app(%d, "APPS\\DUUM.UNO")' % esp_vol(d),
           timeout=30)
    for _ in range(30):
        if any("DUUM" in t.upper() for t in d.windows()):
            break
        time.sleep(2.0)
    # Walk a few steps so the frame is a room being played, not a spawn point.
    time.sleep(2.0)
    for _ in range(10):
        d.key(0, S_UP, settle=0.30)
    for _ in range(3):
        d.key(0, S_RIGHT, settle=0.30)

    # Duum opens top-left, exactly over the part of the document that carries
    # the formatting, so the page behind it reads as blank. Move it down and
    # right by its title bar: the formatted lines sit in the page's top-left,
    # and this clears them. Dropped mid-screen, well away from the edges,
    # because within 8 px of one the WM snaps the window instead.
    d.drag(300, 36, 880, 372, settle=1.2)
    time.sleep(1.5)
    return True


def poster_shot(d):
    """Grab it. The stream is running but the picture is the point."""
    d.beat("poster")
    time.sleep(1.0)
    p, w, h, _ = d.shot("poster")
    print("  poster: %s (%dx%d)" % (p, w, h))
    time.sleep(1.0)


SCENES = [
    ("s02", (None, s02_wm)),
    ("s03", (None, s03_themes)),
    ("s04", (s04_pre, s04_office)),
    ("s05", (None, s05_browser)),
    ("s07", (s07_pre, s07_studio)),
    # After s07 in the cut too. No _pre: UnoCode opens the SDK sources that
    # build.sh already stages on the ESP, so there is nothing to push first.
    ("s16", (None, s16_unocode)),
    ("s09", (s09_pre, s09_automate)),
    ("s15", (None, s15_arcade)),
    ("s08", (s08_pre, s08_duum)),
    ("s13", (s13_pre, s13_ssh)),
    ("s10", (None, s10_system_log)),
    # s14 (appliances) is LAST and out of the cut's spine: it is the only
    # scene that needs UNO_DEMO_KVM=1 plus a UNO_DETACH=1 build, so it skips
    # itself on every ordinary run. It kept the number it was recorded under
    # until 2026-08-08, when s09 became the automation scene.
    ("s14", (s14_pre, s14_console)),
    # Not in the cut: this one exists to produce the website's poster frame.
    # Run it on its own - `scenes.py --scene poster` - and take the PNG out of
    # out/<dir>/probe/poster.png.
    ("poster", (poster_pre, poster_shot)),
]


def main(argv):
    global MODE, METAL_PORT, STREAM_HOST, OUT, PROBE
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--scene", action="append", default=[],
                    help="run one scene (repeatable)")
    ap.add_argument("--all", action="store_true", help="run the whole spine")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--with-net", action="store_true",
                    help="(stub) networking beats are metal-only; logged+skipped")
    ap.add_argument("--metal", nargs="?", const=METAL_PORT, type=int,
                    metavar="PORT",
                    help="record REAL HARDWARE: do not boot or stage anything, "
                         "just listen on PORT (default %d) for the box to dial "
                         "in. Run this on the host its DEBUG.CFG names."
                         % METAL_PORT)
    ap.add_argument("--stream-host", metavar="IP",
                    help="address the box should push video to (default: this "
                         "host's address on the path to the box)")
    ap.add_argument("--min-width", type=int, default=1024,
                    help="raise the desktop to at least this width at session "
                         "start (0 = leave the resolution alone)")
    ap.add_argument("--out-dir", metavar="DIR",
                    help="where the recordings go (default tools/demo/out); "
                         "use a fresh directory to keep an earlier cut intact")
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

    if a.out_dir:
        OUT = a.out_dir if os.path.isabs(a.out_dir) \
            else os.path.join(HERE, a.out_dir)
        PROBE = os.path.join(OUT, "probe")
    if a.metal:
        MODE, METAL_PORT = "metal", a.metal
    if a.stream_host:
        STREAM_HOST = a.stream_host
    if MODE == "qemu" and RQ is None:
        ap.error("remote_qemu is unavailable (no QEMU/OVMF on this host?) - "
                 "this host can only drive --metal")

    d = Demo()
    if MODE == "qemu":
        d.office_staged = stage_office()
        d.wad_staged = stage_wad()
        d.sdk_staged = stage_sdk()
        d.vm_staged = stage_vm()
        print("staged: office=%s wad=%s sdk=%s vm=%s" %
              (d.office_staged, d.wad_staged, d.sdk_staged, d.vm_staged))
    else:
        # Nothing is staged on metal: the stick already carries DOOM1.WAD,
        # DOCS\ and the rest, and the office documents are pushed to the RAM
        # volume at runtime from ./corpus. The guards below are therefore
        # about what is REACHABLE, re-probed after the box dials in.
        d.office_staged = [n for s, n in OFFICE
                           if os.path.exists(office_src(s))]
        d.vm_staged = False                  # the appliance is a QEMU-only scene
    results = []
    t0 = time.time()
    try:
        d.boot()
        if MODE == "metal":
            d.probe_metal_assets()
        if a.min_width:
            d.raise_resolution(a.min_width)
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
