#!/usr/bin/env python3
"""Duum demo recorder - a standalone, rerunnable driver for the Duum
(Python-Doom-on-UnoDOS) demo video.  Separate from the OS demo (scenes.py):
this one boots the DEBUG image once, launches DUUM.UNO, and records a spine of
Duum-only scenes over unostream, one mp4 per scene.

Reuses the same low-level plumbing every pc64 gate uses: remote_qemu (disk +
boot), unoauto_remote.UnoAutoLink (the URC control link), stream_recv
(the unostream receiver).  Runs under Linux with KVM (quill).

  python3 duum_demo.py --list
  python3 duum_demo.py --all
  python3 duum_demo.py --scene s03
  python3 duum_demo.py --rehearse "u=UUUUUU r=RRRR f=FFFF"   # drive + shoot PNGs
"""
import argparse, json, os, socket, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, TOOLS)

import remote_qemu as RQ                                   # noqa: E402
from unoauto_remote import UnoAutoLink                     # noqa: E402
from stream_recv import StreamReceiver, write_png          # noqa: E402

OUT = os.path.join(HERE, "out", "duum")
WAV = os.path.join(OUT, "audio.wav")     # the guest's own audio, whole run
URC_PORT = 5410
STREAM_BASE = 5420
GOP_W, GOP_H = 2560, 1600            # -> 1280x800 desktop (half the panel)

# UnoDOS scancodes + Duum keys
S_UP, S_DOWN, S_RIGHT, S_LEFT = 0x01, 0x02, 0x03, 0x04
K_FIRE = ord('f')
K_USE = ord(' ')
KEYMAP = {"U": (S_UP, 0), "D": (S_DOWN, 0), "R": (S_RIGHT, 0), "L": (S_LEFT, 0),
          "F": (0, K_FIRE), ".": (0, ord('.')), ",": (0, ord(',')),
          " ": (0, K_USE), "S": (0, K_USE),
          # the pause menu: Esc opens and backs out, Enter activates. The
          # arrows above double as its cursor (the engine reads scan 1-4),
          # so the menu needs no keys of its own beyond these two.
          "X": (0x17, 27), "N": (0, 13)}
for _dg in "123456":                    # weapon select
    KEYMAP[_dg] = (0, ord(_dg))


# ---------------------------------------------------------------------------
def build_disk():
    """RQ.build_disk(), then overwrite its DEBUG.CFG with the demo keys:
    remote dial-out (so the guest connects to our URC listener), nonet (skip
    the net self-test, the stack still comes up), nostress/noshutdown (kill the
    fuzz + auto power-off), nohud (hide the red perf overlay - the boot log
    prints hud_len=0)."""
    RQ.build_disk()
    cfg = "/tmp/duum_demo.cfg"
    with open(cfg, "w", newline="\r\n") as f:
        f.write("remote=10.0.2.2:%d\nnonet\nnostress\nnoshutdown\nnohud\n"
                % URC_PORT)
    subprocess.run(["mcopy", "-i", RQ.FAT, "-o", cfg, "::/DEBUG.CFG"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    part_start = 2048
    with open(RQ.FAT, "rb") as pf, open(RQ.DISK, "r+b") as df:
        df.seek(part_start * 512)
        while True:
            b = pf.read(1 << 20)
            if not b:
                break
            df.write(b)


def boot_qemu():
    subprocess.run(["cp", RQ.OVMF_VARS, RQ.VARS])
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "3072",
        "-cpu", "host", "-enable-kvm", "-smp", "4",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + RQ.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + RQ.VARS,
        "-drive", "format=raw,file=" + RQ.DISK,
        "-drive", "format=raw,file=" + RQ.DISK2,
        "-netdev", "user,id=n0",
        "-device", "e1000,netdev=n0",
        "-vga", "none",
        "-device", "VGA,edid=on,xres=%d,yres=%d,vgamem_mb=64" % (GOP_W, GOP_H),
        # The game's own voice. QEMU's wav sink records everything the guest's
        # DAC consumed, for the whole run, in ONE file - there is no per-scene
        # sink - so the recorder stamps the wall clock it started at and the
        # stitcher cuts each scene's audio out of it. See AUDIO in DUUM-DEMO.md.
        #
        # AC'97 AND NOT INTEL HDA, and this is not a preference. With an
        # intel-hda device attached the guest never reaches URC dial-out UNDER
        # KVM - measured 4 times out of 4, on this build and on one from before
        # any of the sound work, so it is not a regression from it - while the
        # same build with AC'97 dials in every time and the same HDA device
        # under TCG is fine. Filed in UNOAUTOMATE-REQUESTS.md. snd_pcm sits
        # above both backends, so the film records the same mixer either way.
        "-audiodev", "wav,id=snd0,path=" + WAV,
        "-device", "AC97,audiodev=snd0",
        "-display", "none",
    ]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


class Duum(object):
    def __init__(self):
        self.link = None
        self.qemu = None
        self.t_qemu = 0.0
        self.w = self.h = 0
        self._beats = None
        self._vol = None

    def boot(self):
        os.makedirs(OUT, exist_ok=True)
        if not os.path.isdir(RQ.ESP):
            raise SystemExit("no build/esp - run UNO_DEBUG=1 ./build.sh first")
        try:
            os.unlink(WAV)               # a stale wav would be sliced instead
        except OSError:
            pass
        self.link = UnoAutoLink("127.0.0.1", URC_PORT)
        self.link.listen()
        build_disk()
        self.t_qemu = time.time()        # the wav's own zero
        self.qemu = boot_qemu()
        if not self.link.wait_connected(180):
            raise SystemExit("guest never dialled in - is this the DEBUG build?")
        self.link.wait_hello(30.0)
        time.sleep(2.5)
        self.w, self.h = self.link.screen_info(timeout=15)
        print("booted: desktop %dx%d" % (self.w, self.h))
        return self

    def audio_index(self):
        """Where the wav's clock starts, so a scene can be cut out of it.

        The wav and the video are on INDEPENDENT clocks: the sink writes what
        the emulated DAC consumed, which under emulation runs a percent or two
        off wall clock, and every frame in timing.jsonl is wall clock. One
        number (t0) plus a measured drift is all the stitcher needs, and the
        drift is measurable because the run's own length is known."""
        idx = {"t0": self.t_qemu, "t_end": time.time(), "wav": WAV}
        with open(os.path.join(OUT, "audio.json"), "w") as f:
            json.dump(idx, f, indent=1)
        return idx

    def stop(self):
        try:
            self.audio_index()
        except Exception:
            pass
        try:
            if self.link:
                self.link.command("poweroff", timeout=2)
        except Exception:
            pass
        try:
            if self.link:
                self.link.close()
        except Exception:
            pass
        if self.qemu:
            self.qemu.terminate()
            try:
                self.qemu.wait(timeout=10)
            except Exception:
                self.qemu.kill()

    # ---- helpers ----------------------------------------------------------
    def esp_vol(self):
        if self._vol is None:
            self._vol = next((x["vol"] for x in self.link.vols()
                              if x["kind"] == 1
                              and x["name"].strip() in ("NO NAME", "")), 1)
        return self._vol

    def windows(self):
        return [r["name"] for r in self.link.probe(timeout=10)
                if r["kind"] == 1]

    def beat(self, name, settle=0.8):
        if self._beats:
            self._beats.write(json.dumps({"beat": name, "t": time.time()})
                              + "\n")
            self._beats.flush()
        print("  beat: " + name)
        if settle:
            time.sleep(settle)

    def click(self, x, y, settle=0.6):
        # move, press, release as THREE injections: the shell samples pointer
        # state per frame, so a press+release inside one sample cancels out.
        self.link.pointer(int(x), int(y), 0, timeout=8); time.sleep(0.15)
        self.link.pointer(int(x), int(y), 1, timeout=8); time.sleep(0.20)
        self.link.pointer(int(x), int(y), 0, timeout=8); time.sleep(settle)

    def park_cursor(self):
        """The captured framebuffer INCLUDES the pointer, so move it well off
        the Duum window (top-left) into empty desktop, outside the post crop."""
        self.link.pointer(1150, 680, 0, timeout=8)
        time.sleep(0.3)

    def clean_desktop(self):
        """Close every window (the shell restores the previous session), so
        Duum, launched next, is the ONLY window and lands at the deterministic
        first-cascade position - which makes a fixed post crop valid."""
        for _ in range(12):
            if not self.windows():
                break
            self.link.command("close", timeout=10)
            time.sleep(0.7)

    def key(self, letter, settle=0.30):
        e = KEYMAP.get(letter)
        if e is None:                       # any unmapped letter ('-') is a
            time.sleep(settle)              # WAIT of one press-time: lets a
            return                          # chaser close in, mid-sequence
        scan, uni = e
        self.link.key(scan, uni, 0, timeout=8)
        time.sleep(settle)

    def keys(self, seq, settle=0.30):
        for ch in seq:
            self.key(ch, settle=settle)

    def launch_duum(self):
        vol = self.esp_vol()
        self.link.eval('import uno; uno.run_app(%d, "APPS\\\\DUUM.UNO")' % vol,
                       timeout=30)
        for _ in range(40):                       # WAD parse + first frame
            if any("DUUM" in t.upper() for t in self.windows()):
                print("  Duum window up")
                time.sleep(2.0)
                return True
            time.sleep(2.0)
        print("  WARNING: Duum window never appeared")
        return False

    # ---- recording --------------------------------------------------------
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
                raise RuntimeError("guest never connected to receiver")
            time.sleep(1.0)
            fn(self)
            time.sleep(1.0)
        except Exception as e:
            err = e
        finally:
            try:
                self.link.command("stream", "stop", timeout=8)
            except Exception:
                pass
            th.join(30.0)
            self._beats.close()
            self._beats = None
        dur = time.time() - t0
        st = {"scene": name, "dur": round(dur, 1), "frames": rx.frames,
              "w": rx.w, "h": rx.h,
              "mp4_bytes": (os.path.getsize(base + ".mp4")
                            if os.path.exists(base + ".mp4") else 0)}
        if rx.t_first and rx.t_last and rx.t_last > rx.t_first:
            st["fps"] = round((rx.frames - 1) / (rx.t_last - rx.t_first), 1)
        if err:
            st["error"] = repr(err)
        print("  %s: %s" % (name, json.dumps(st)))
        return st


# ---------------------------------------------------------------------------
# scenes  (beats designed + verified on the host shim; movement is
# deterministic and 1:1 with the device, so the key sequences transfer)
# ---------------------------------------------------------------------------
# ROUTES ARE FOR FREEDOOM's E1M1, not id's.  Freedoom Phase 1 ships ORIGINAL
# maps (only the format and texture NAMES are shared), so none of the id-E1M1
# choreography transfers: there is no start-room-to-courtyard walk here, the
# level opens inland through a computer hall into a crate corridor.  Every
# sequence below was designed and verified frame by frame on the host shim
# (tools/duum_host.py) at the device's own key timing - one press = one 0.30 s
# hold = ~96 map units forward or ~53 degrees of turn - so it transfers 1:1.
def s01_title(d):
    """The Duum window is already open on E1M1's start room. Let it read, then
    a small look so it is obviously live 3D, not a screenshot.  One press is
    the smallest turn the key model can make (~53 deg)."""
    d.beat("duum-title", settle=4.5)
    d.beat("look-left", settle=0.4)
    d.keys("L", settle=0.35)
    d.beat("look-back", settle=0.6)
    d.keys("R", settle=0.35)
    d.beat("settle", settle=3.5)


def s02_render(d):
    """The renderer showcase: walk east out of the start room, through the
    computer hall (banked terminals, lit ceiling, a full-height window) and on
    towards the crate corridor.  Straight-line walking on purpose - turning
    mid-route then walking accumulates heading drift at 53 deg per press and
    ends up in a wall.

    Four presses, and the fight in s03 takes it from here: the scenes are ONE
    continuous session now, so this scene's arrival point is the next one's
    starting point.

    STOP BEFORE THE CORRIDOR. Duum is a game now: its zombies chase, and the
    ten-press version of this route walked straight into two of them and
    ended the take at 1% health with a bloodied face - which is a combat
    shot, not a renderer one, and the narration for a renderer scene cannot
    play over it. Seven presses reach the hall and hold there; the fight is
    s03's job."""
    d.beat("walk-in", settle=0.3)
    d.keys("UUUU")
    d.beat("the-hall", settle=1.6)
    d.beat("deeper-in", settle=0.3)
    d.beat("hold-vista", settle=1.5)


def s03_combat(d):
    """Combat: walk east down the corridor until a zombie closes to point
    blank, turn onto it and fire until it drops.

    This gets the shot ~15 takes could NOT get on id's E1M1 (see the
    duum-demo-video memory): there the enemies were across a courtyard and
    keys-only aiming could not frame a kill.  Here the corridor is straight
    and the zombies walk INTO you, so the kill lands dead centre.

    NO TURN, and the aim is never corrected: the host shim and the guest do
    NOT travel the same distance per press.  DUUM.PY clamps dt to 0.1 s, so
    at the guest's 5-14 fps its clock advances slower than wall clock while
    the harness sleeps a fixed 0.30 s - the first take turned onto a wall
    because it assumed the host's arrival point.  Walking straight and firing
    straight down the corridor is correct wherever the walk actually ends,
    and standing still lets the chaser close the gap."""
    # CONTINUES from s02, which stopped in the hall four presses in. Three
    # more reach the corridor the zombies walk down; the old eight assumed a
    # fresh start room and would end up in a wall from here.
    d.beat("advance", settle=0.3)
    d.keys("UUU")
    d.beat("enemies", settle=1.6)           # let the chaser walk into frame
    d.keys("FFFF", settle=0.26)
    d.beat("firefight", settle=0.2)
    d.keys("FFFFFF", settle=0.26)
    d.beat("hold", settle=0.8)


def s03_hero(d):
    """Hero-combat take, same beat names as s03_combat so a good take can be
    dropped in as s03 unchanged.

    Walks FURTHER than the host shim says is needed (the guest's clamped dt
    makes it travel less per press, and by a different amount every run
    depending on the frame rate it happens to get), fires an early pair to
    wake the corridor through DUUM.PY's noise_wake, then holds so the chasers
    close in before the main burst.  Straight line, no aiming turns."""
    d.beat("advance", settle=0.3)
    d.keys("UUUUUUUU")
    d.keys("UUUUUUUU")
    d.beat("enemies", settle=0.4)
    d.keys("FF", settle=0.30)                 # wake the corridor
    d.keys("--", settle=0.90)                 # let them walk in
    d.beat("firefight", settle=0.2)
    d.keys("FFFFFFFF", settle=0.30)
    d.beat("hold", settle=0.9)


def s04_menu(d):
    """The pause menu, and the Controls screen inside it.

    New since the last film, and the first time either has been exercised on
    this port at all - upstream filed the Controls screen as unverified on
    real hardware.  It lists what each action is bound to by asking the host
    (uno.bind_name), so the screen is a readout of pc64's own binding table,
    not a picture of one.

    THREE Escs get out of Controls, not two: Esc backs up one screen at a
    time (keys -> options -> main -> closed).  Two left the menu open with
    the arrows still driving its cursor, and a rehearsal spent its whole
    movement half pressing keys into a menu that was quietly eating them.

    Every settle here is long by gameplay standards on purpose: the viewer
    has to read a screen the player already knows."""
    d.beat("menu-open", settle=0.4)
    d.keys("X", settle=1.6)
    d.beat("menu-shown", settle=2.2)
    d.keys("D", settle=0.9)                  # cursor: Resume -> Options
    d.keys("N", settle=1.6)                  # into Options
    d.beat("menu-options", settle=2.4)
    d.keys("D", settle=0.9)                  # cursor: FPS counter -> Controls
    d.keys("N", settle=1.6)
    d.beat("menu-controls", settle=5.0)      # hold: the whole list is readable
    d.keys("XXX", settle=0.7)                # keys -> options -> main -> out
    d.beat("resumed", settle=1.6)
    d.keys("L", settle=0.5)                  # and the game is live again
    d.beat("back-in-the-game", settle=2.0)


def s05_hud(d):
    """Hold where the fight left off so the status bar reads clearly: health,
    ammo, armor, the arms panel and the face - and by now those numbers have
    a history, which a hold on an untouched start room did not.  A slow look
    keeps it live while the closing narration lands.  Single presses: a turn
    is ~53 deg now that movement is held-key continuous, so the old triples
    were a full spin."""
    d.beat("hud-hold", settle=3.5)
    d.keys("L", settle=0.5)
    d.beat("ports-line", settle=4.0)
    d.keys("R", settle=0.5)
    d.beat("reverse-line", settle=4.5)
    d.keys("L", settle=0.4)
    d.beat("hud-settle", settle=2.0)


# ONE continuous session: each scene picks up where the last one left off.
# See the note in main(); --relaunch restores per-scene restarts.
SCENES = [("s01", s01_title), ("s02", s02_render), ("s03", s03_combat),
          ("s04", s04_menu), ("s05", s05_hud)]


# ---------------------------------------------------------------------------
def rehearse(d, spec):
    """Drive Duum live and screenshot each beat, to design the run on-device.
    spec: space-separated label=keys pairs."""
    for r in d.link.probe(timeout=10):
        if r["kind"] == 1:
            print("  window: %r rect=%s" % (r.get("name"),
                  (r.get("v1"), r.get("v2"))))
    for i, pair in enumerate(spec.split()):
        label, _, seq = pair.partition("=")
        d.keys(seq)
        time.sleep(0.3)
        w, h, rgba = d.link.screen_grab(1, timeout=40)
        png = os.path.join(OUT, "reh_%02d_%s.png" % (i, label))
        write_png(png, w, h, rgba)
        print("  %s -> %s (%dx%d)" % (label, png, w, h))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--scene", default=None)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--rehearse", default=None)
    ap.add_argument("--probe-app", action="store_true",
                    help="test whether the py verb can reach the Duum app")
    ap.add_argument("--hero-takes", type=int, default=0,
                    help="record N hero-combat takes (one boot, relaunch each)")
    ap.add_argument("--no-maximize", action="store_true")
    ap.add_argument("--relaunch", action="store_true",
                    help="restart Duum before every scene (the old behaviour: "
                         "every scene opens on the start room, which reads as "
                         "a loop in the finished cut)")
    args = ap.parse_args()

    if args.list:
        for i, (n, fn) in enumerate(SCENES):
            print("%s  %s" % (n, (fn.__doc__ or "").strip().split("\n")[0]))
        return

    os.makedirs(OUT, exist_ok=True)
    d = Duum().boot()
    try:
        d.clean_desktop()
        d.launch_duum()
        if args.probe_app:
            for src in ("import sys; print([m for m in sys.modules if 'uum' in m])",
                        "print(sorted(k for k in __import__('sys').modules))",
                        "import sys; m=sys.modules.get('__main__'); "
                        "print('main has app:', hasattr(m,'app'), type(getattr(m,'app',None)).__name__)"):
                print("PY>", src)
                try:
                    print("  ->", d.link.eval(src, timeout=20))
                except Exception as e:
                    print("  ERR", e)
            return
        if args.rehearse is not None:
            rehearse(d, args.rehearse)
            return
        if args.hero_takes:
            for i in range(args.hero_takes):
                d.clean_desktop()
                d.launch_duum()
                d.park_cursor()
                d.record("s03_hero%d" % i, s03_hero, STREAM_BASE + (i % 4))
            return
        sel = set((args.scene or "").split(","))
        want = [s for s in SCENES if (args.all or s[0] in sel)]
        for i, (name, fn) in enumerate(want):
            # ONE session by default, and this is the difference between a
            # film and a loop. Relaunching between scenes made every scene
            # start from the same start room, so the finished cut looked like
            # the same twenty seconds of play repeated five times - which is
            # exactly what the first viewer said. Now the scenes CONTINUE:
            # the walk carries on from the title's view, the fight happens
            # where the walk arrived, and the menu opens over that.
            #
            # --relaunch restores the old behaviour for designing a single
            # scene against a known start.
            if args.relaunch or i == 0:
                d.clean_desktop()
                d.launch_duum()
                d.park_cursor()
            d.record(name, fn, STREAM_BASE + i)
    finally:
        d.stop()


if __name__ == "__main__":
    main()
