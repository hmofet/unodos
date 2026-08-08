#!/usr/bin/env python3
"""s06 - media, with REAL GUEST AUDIO (QEMU + unostream + an audiodev wav sink).

    python3 scene_media.py            -> out/s06.mp4, out/s06.wav (+ sidecars)
    python3 scene_media.py --no-audio    video only (diagnostic)

This is the only scene in the cut that captures sound, so the sound is the
part that has to be proved rather than assumed. Video rides unostream exactly
as scenes.py does (a fresh stream_recv on its own port, `stream start 10.0.2.2
<port> 30`, beats, `stream stop`). Audio is QEMU's own `wav` audiodev behind an
Intel HDA codec:

    -audiodev wav,id=snd0,path=out/s06.wav
    -device intel-hda -device hda-output,audiodev=snd0

which is what tools/audio_test.py:38-45 and tools/music_test.py:125-126 use to
prove the DAC path (pc64/AUDIO.md: HDA is probed first, AC'97 is the fallback;
`--ac97` here swaps to it). NOTE that every DEMO harness deliberately passes no
audio device at all, so this scene had to grow its own boot rather than borrow
scenes.py's.

TWO INDEPENDENT CLOCKS, and the reason this file measures rather than assumes.
The wav sink starts with the MACHINE and the video starts when the GUEST dials
the receiver, tens of seconds later. Nothing ties them together, so the music
is located in the wav (the longest loud run, widened to its real attack and
decay) and joined to the video at the one pair of events that is simultaneous
BY CONSTRUCTION: the `stop` beat and the end of the music. Stopping silences
the DAC in the frame the click lands, so those two are the same instant.

`play` is deliberately NOT the anchor, though it is still reported. The click
precedes the first sample by however long UnoAmp takes to open and decode the
file - ~1.4 s on this build, read off the player's own elapsed counter in the
captured frames - and anchoring there (which this file used to do) folds that
latency into the constant and starts the music early. The two numbers bracket
the honest range; their difference is that latency plus the wav sink's own
drift, which under TCG runs a few percent short.

THE RE-SKIN IS THE CENTREPIECE, AND WHY THE SKIN IS STAGED IN `SKINS\\`.
Two fixes landed on master (7390ebf0, 96e32db4, fb3eeb00) after the first cut
of this scene: bmp_decode() learned BI_RLE8/BI_RLE4, so a stock Winamp 2.9x
skin loads at all; and a `skin` URC verb re-skins a RUNNING player and repaints
it. Both were needed - the first cut of this file could only film the built-in
look and say why.

That creates a staging problem. `load_a_skin()` scans every volume ROOT when
the player opens (unoamp_app.c), so a `.wsz` at the root means UnoAmp comes up
ALREADY skinned and there is no transformation left to film. `skin list` and
`skin scan` are root-only for the same reason. But `skin load <vol> <path>`
passes its path straight to uno_fs_read, which reads a subdirectory fine - so a
skin parked in `SKINS\\` is invisible to the boot scan and reachable by the
verb. The player opens built-in, the music starts, and one command puts the
real Base 2.91 chassis on screen mid-shot. Nothing about the file changes: it
is the same untouched archive, decoded live from its ZIP by unoamp_skin.c.

`skin` is DRIVE-gated rather than #ifdef'd, so this works on a production build
too; the debug build is used here only because the scene also needs URC itself.

RESOLUTION: 1280x800, and the desktop is really that size - nothing here is
scaled. unostream sends the DESKTOP framebuffer (uno_fb_w x uno_fb_h), and
uefi_main.c makes that half the firmware's GOP mode (uefi_main.c:641), so
OVMF's default 1280x800 panel produced a 640x400 stream. The panel is doubled
instead - `-vga none -device VGA,edid=on,xres=2560,yres=1600,vgamem_mb=64`,
see demo_common.vga_args - and the halving lands on 1280x800, so the mp4 comes
off the wire at the size the whole cut is assembled at.

That also avoids raising the desktop through Control Panel > Display, which
would have been the alternative: that path has a 15-second "Keep this
resolution?" probation that silently reverts if the confirm misses, AND a
resolution change makes the guest emit a fresh unostream hello, which
stream_recv reads as a reset and splits the mp4 in two. Booting at the right
size has neither problem.
"""
import argparse, json, os, subprocess, sys, threading, time

HERE  = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)

from unoauto_remote import UnoAutoLink                      # noqa: E402
from stream_recv import StreamReceiver, write_png           # noqa: E402
from demo_common import (OUT, PROBE, PC64, ESP, OVMF_CODE, OVMF_VARS,  # noqa: E402
                         S06_URC, S06_STREAM, S06_DISK, S06_FAT, S06_VARS,
                         GOP_W, GOP_H, VID_W, VID_H, vga_args,
                         Qmp, Beats, build_fat_disk, probe, clean_outputs,
                         wav_measure, sh)

S06_QMP = "/tmp/unodos-demo-s06-qmp.sock"

# pc64/build/esp is shared with other lanes and gets rebuilt under us (it was a
# UNO_DEBUG=0 tree by the time this scene needed re-recording). s06 needs a
# DEBUG build - it drives the whole scene over URC - so it prefers its own:
#   git worktree add ~/unodos-s06dbg --detach <sha>
#   cp -r pc64/fw-blobs ~/unodos-s06dbg/pc64/
#   cd ~/unodos-s06dbg/pc64 && UNO_DEBUG=1 sh ./build.sh
DBG_ESP = os.path.expanduser("~/unodos-s06dbg/pc64/build/esp")

# Where the skin is parked ON THE GUEST. NOT the volume root: the boot scan
# takes the first .wsz it finds at any root, and a player that opens already
# skinned has no transformation to film. See the module docstring.
SKIN_DIR = "SKINS"
SKIN_NAME = "BASE291.WSZ"

# EFI scan codes (the `key` verb's scan field) - map_key in uefi_main.c, the
# same table scenes.py uses.
S_UP, S_DOWN, S_RIGHT, S_LEFT = 0x01, 0x02, 0x03, 0x04
S_ESC = 0x17

DEBUG_CFG = ("nohud\n"          # no red perf HUD / stress status line
             "nostress\n"       # the fuzz driver would open apps on camera
             "noshutdown\n"
             "nonet\n"
             "remote=10.0.2.2:%d\n" % S06_URC)

# The Winamp 2.91 skin and the track, both at the volume ROOT on purpose:
# UnoAmp scans roots only (skins in unoamp_app.c:288, the playlist in
# seed_playlist()) and has no file-open dialog, so anything nested is invisible
# to it. FLYHIGH.MP3 ends up the ONLY playable file at any root, which is what
# makes `g_sel == 0` and a single Play click deterministic.
SKIN = os.path.join(PC64, "wads", "BASE291.WSZ")
# The track lives outside the repo (it is not ours to commit). Candidates in
# preference order; --mp3 overrides. The last is the copy build.sh's staging
# already left on the ESP, so a machine without the scratchpad still records.
MP3_CANDIDATES = [
    "/mnt/c/Users/arin/AppData/Local/Temp/claude/C--Users-arin/"
    "3256c99b-ac54-4616-bece-ea49f590907d/scratchpad/demo-assets/FLYHIGH.MP3",
    os.path.join(PC64, "wads", "FLYHIGH.MP3"),
    os.path.join(ESP, "FLYHIGH.MP3"),
]


def find_mp3(override=None):
    for p in ([override] if override else []) + MP3_CANDIDATES:
        if p and os.path.exists(p):
            return p
    return None

# PICTURES\ in the order Photos will step through it. FAT has no sort - a
# directory lists in creation order and Photos walks the listing - so writing
# these in order IS the choreography. One file per decoder, the manual's own
# demo set (pc64/pictures/README.TXT).
#   Excluded: LAGOON.WEB, which unomedia RECOGNISES and declines (WebP/VP8 is
#   not decoded), and the BAD_*.* negative-test files a harness left in
#   build/esp/PICTURES - neither belongs in a demo reel, and stepping onto one
#   would put an error message on camera.
PICS = ["SUNSET.JPG",     # baseline JPEG
        "BLOOM.PNG",      # PNG with real alpha
        "ORBIT.GIF",      # animated GIF, 10 frames  <- the one we hold on
        "FLAG.BMP",       # 24-bit BMP
        "TILES.QOI",      # QOI
        "GRAD.TGA",       # TGA
        "MOON.PGM"]       # netpbm greyscale


def boot_qemu(disk, wav, ac97=False, no_audio=False, gop=(GOP_W, GOP_H)):
    sh(["cp", OVMF_VARS, S06_VARS])
    if os.path.exists(S06_QMP):
        os.remove(S06_QMP)
    if wav and os.path.exists(wav):
        os.remove(wav)
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + S06_VARS,
        "-drive", "format=raw,file=" + disk,
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
    ] + vga_args(*gop) + [
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % S06_QMP,
    ]
    if not no_audio:
        cmd += ["-audiodev", "wav,id=snd0,path=" + wav]
        cmd += (["-device", "AC97,audiodev=snd0"] if ac97 else
                ["-device", "intel-hda", "-device", "hda-output,audiodev=snd0"])
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


class Driver(object):
    """The input conventions scenes.py encodes, reused verbatim (they are the
    guest's constraints, not scenes.py's opinions): a click is THREE injections
    because the shell samples pointer state once per frame; pointer travel is a
    glide of ~12 px steps ~35 ms apart with a breather every 20, because the
    injected-pointer queue is 32 deep with a 2-frame dwell."""

    def __init__(self, link, beats=None, verbose=True):
        self.link, self.beats, self.verbose = link, beats, verbose
        self.w = self.h = 0
        self.px = self.py = 0

    def beat(self, name, settle=0.8):
        t = self.beats.mark(name) if self.beats else time.time()
        if settle:
            time.sleep(settle)
        return t

    def key(self, uni=0, scan=0, ctrl=0, settle=0.15):
        self.link.key(scan, uni, ctrl, timeout=8)
        time.sleep(settle)

    def move(self, x, y, btn=0, settle=0.12):
        x, y = int(x), int(y)
        self.link.pointer(x, y, btn, timeout=8)
        self.px, self.py = x, y
        time.sleep(settle)

    def glide_step(self):
        """Pixels per pointer step, scaled to the desktop.

        The 12 px in scenes.py is a distance on a 640-wide desktop, and this
        scene now films a 1280-wide one: keeping it literal doubles the number
        of injections for the same journey, and pointer travel is host-paced,
        so every glide takes twice as long in wall clock. (Measured: it was
        most of the 8.8 s this scene grew when the desktop doubled.) Scaling
        the step keeps the pointer's SPEED ACROSS THE SCREEN the same, which is
        what the pacing was tuned for, and leaves the constraint that actually
        matters untouched - the injected-pointer queue cares about the RATE of
        injections, not how far each one moves.
        """
        return max(12, int(round(12 * (self.w or 640) / 640.0)))

    def sweep(self, x1, y1, btn=0, step=None, pace=0.035, burst=20):
        x0, y0 = self.px, self.py
        dx, dy = x1 - x0, y1 - y0
        n = max(1, int(max(abs(dx), abs(dy)) / (step or self.glide_step())))
        for i in range(1, n + 1):
            self.move(x0 + dx * i // n, y0 + dy * i // n, btn, settle=pace)
            if i % burst == 0:
                time.sleep(0.4)
        self.move(x1, y1, btn, settle=0.1)

    def click(self, x, y, glide=True, settle=0.5):
        if glide:
            self.sweep(x, y)
        self.move(x, y, 0, settle=0.15)
        self.move(x, y, 1, settle=0.2)
        self.move(x, y, 0, settle=settle)

    def launch(self, app_id, settle=3.0):
        self.link.command("launch", app_id, timeout=15)
        time.sleep(settle)

    def windows(self):
        return [r["name"] for r in self.link.probe(timeout=10) if r["kind"] == 1]

    def close_all(self, n=8):
        for _ in range(n):
            if not self.windows():
                return
            self.link.command("close", timeout=10)
            time.sleep(0.6)

    def shot(self, tag):
        w, h, rgba = self.link.screen_grab(1, timeout=40)
        p = os.path.join(PROBE, tag + ".png")
        write_png(p, w, h, rgba)
        print("  shot: " + p)
        return p


# ---------------------------------------------------------------------------
# UnoAmp geometry. Every control in a Winamp 2 skin sits at a FIXED offset in
# the 275x116 main window - that is what the skin format is - so these are the
# constants out of unoamp_ui.c, not measurements off a screenshot:
#   kBtnX[6] = {16,39,62,85,108,136}, BTN_Y 88, buttons 23x18
#   EQ_X 219 / PL_X 242 at EQPL_Y 58, 23x12
#   close  (WIN_W-11, 3, 9, 9)
# The window opens at (120,60) and unoamp_ui_build picks g_scale from the
# panel width (>=2400 -> 3, >=1100 -> 2, else 1), so screen = 120 + x*scale.
UAMP_ORG = (120, 60)
UAMP_WIN_W = 275
UAMP_BTN_X = [16, 39, 62, 85, 108, 136]
UAMP_BTN_Y = 88
T_PREV, T_PLAY, T_PAUSE, T_STOP, T_NEXT, T_EJECT = range(6)


def uamp_scale(fb_w):
    return 3 if fb_w >= 2400 else 2 if fb_w >= 1100 else 1


def uamp_pt(fb_w, x, y):
    s = uamp_scale(fb_w)
    return (UAMP_ORG[0] + x * s, UAMP_ORG[1] + y * s)


def uamp_btn(fb_w, i):
    return uamp_pt(fb_w, UAMP_BTN_X[i] + 11, UAMP_BTN_Y + 9)


def uamp_eq(fb_w):
    return uamp_pt(fb_w, 219 + 11, 58 + 6)


def uamp_close(fb_w):
    return uamp_pt(fb_w, UAMP_WIN_W - 11 + 4, 3 + 4)


# ---------------------------------------------------------------------------
def s06(d):
    fbw = d.w
    # A machine with no saved session boots with the Control Panel open
    # (pc64_uui.c session_load: no SHELL.CFG -> open_app(APP_CTRL)), and there
    # is no setting that boots to a bare desktop. Clear it BEFORE the first
    # beat, or every shot in the scene has a stray window behind it.
    d.close_all()
    d.beat("launch-unoamp", settle=0.4)
    d.launch("unoamp", settle=2.6)          # playlist scan; no skin to find
    # The player comes up in its built-in, theme-coloured look, because the only
    # .wsz on the machine is in SKINS\ where the boot scan does not look.
    d.beat("unoamp-builtin-look", settle=1.4)

    # Glide FIRST, mark the beat, then press. The beat's wall clock is what
    # pins the video clock to the wav clock later, so it has to sit next to the
    # button press and not a second and a half of pointer travel before it.
    play = uamp_btn(fbw, T_PLAY)
    d.sweep(*play)
    d.beat("play-flyhigh-mp3", settle=0.0)
    d.click(*play, glide=False, settle=1.2)

    # The elapsed-time digits and the position bar are what move here. The
    # visualiser well is drawn but effectively frozen: measured over 6 s of an
    # earlier take it changed on 11 of 179 frames, because the player only asks
    # the shell to repaint when its title MARQUEE advances (unoamp_ui_tick) and
    # "FLYHIGH" is short enough to fit without scrolling. Re-measured below and
    # reported either way - the skin does not change that path.
    d.beat("hold-playing", settle=0.2)
    time.sleep(3.2)

    # THE SHOT. One command, mid-playback, and the whole chassis changes: the
    # real Base 2.91 sheets replace the theme-coloured fallback while the track
    # keeps playing. `skin load` repaints, so the transformation lands inside a
    # single frame rather than needing a nudge.
    d.beat("skin-load-base291-wsz", settle=0.0)
    r = d.link.command("skin", "load", d.skin_vol, SKIN_PATH, timeout=20)
    print("    skin load -> %r" % (r,))
    if not (r and r[0].startswith("skinned")):
        raise RuntimeError("skin load refused: %r" % (r,))
    time.sleep(4.6)                          # HOLD: this is the scene's point

    d.beat("open-the-10-band-eq", settle=0.2)
    d.click(*uamp_eq(fbw), settle=1.0)
    time.sleep(3.0)                          # the skinned EQMAIN docks below

    # Glide FIRST, then mark, then press - same shape as the play beat, and for
    # a sharper reason: this beat is the A/V ANCHOR. Stopping kills the sound in
    # the frame it lands, so `stop` in the video and the end of the music in the
    # wav are the same instant, which is the pair the offset is measured from.
    # (Play is NOT such a pair: the click precedes the first sample by however
    # long the player takes to open and decode the file - measured at ~1.4 s on
    # this build, and that bias used to go straight into av_offset_seconds.)
    stop_pt = uamp_btn(fbw, T_STOP)
    d.sweep(*stop_pt)
    d.beat("stop", settle=0.0)
    d.click(*stop_pt, glide=False, settle=0.8)
    # The skin's own close box: it calls unoamp_ui_close(), which takes the
    # player AND its EQ/playlist windows down together. The shell's `close`
    # verb would only remove whichever one happens to be focused.
    d.beat("close-unoamp", settle=0.2)
    d.click(*uamp_close(fbw), settle=1.2)
    d.close_all(3)              # belt and braces: the first take left a
                                # "UnoAmp" chip on the taskbar through Photos

    # Photos pays for the re-skin. Three formats instead of four (BMP goes; the
    # brief's floor was "3-4 including the animated GIF") and shorter holds -
    # the GIF keeps the longest one, because an animation needs time to read as
    # an animation and it is the only beat here that is not a still.
    d.beat("launch-photos", settle=0.3)
    d.launch("photos", settle=3.0)           # opens straight into PICTURES\
    d.beat("jpeg", settle=0.2)
    time.sleep(1.5)
    for tag, hold in (("png-alpha", 1.5),
                      ("animated-gif", 3.8)):  # hold: the animation IS the beat
        d.beat(tag, settle=0.2)
        d.key(0, S_RIGHT, settle=0.4)
        time.sleep(hold)
    d.beat("close", settle=0.2)
    d.close_all()


SKIN_PATH = "%s\\%s" % (SKIN_DIR, SKIN_NAME)


def find_skin_vol(link, verbose=True):
    """Which volume the skin loads from - established by LOADING it, off
    camera, and then undone with `skin off` before the take.

    A volume index is this boot's mount order, not a property of the disk, so
    the on-camera `skin load` cannot be allowed to be the first attempt: a
    refusal mid-take costs the whole recording. The obvious oracle, asking
    `uno.size` whether the file is there, does not work - the `py` verb is a
    ONE-LINE exec (REMOTE.md) and every multi-line probe came back empty, for
    a path that turned out to be perfectly readable. So the preflight is the
    real operation, which answers exactly the question that matters and cannot
    disagree with the take.

    Cheap to undo, and safe to get wrong: REMOTE.md says a refused `load`
    leaves the built-in look rather than the previous skin, and `skin` is safe
    with no player open (which is the case here - this runs before UnoAmp).
    """
    for v in link.vols(timeout=15):
        try:
            r = link.command("skin", "load", v["vol"], SKIN_PATH, timeout=30)
        except Exception as e:                     # noqa: BLE001
            if verbose:
                print("  vol %d (kind %d): %s" % (v["vol"], v["kind"], e))
            continue
        ok = bool(r) and r[0].startswith("skinned")
        if verbose:
            print("  vol %d (kind %d, %r): skin load -> %r"
                  % (v["vol"], v["kind"], v["name"].strip(), r))
        if ok:
            link.command("skin", "off", timeout=10)   # back to built-in
            return v["vol"]
    return None


def probe_skin(link):
    """--probe: everything the skin beat needs, and nothing else. A 50-second
    boot is much cheaper than a failed take."""
    print("vols     : %r" % (link.vols(timeout=15),))
    print("skin list: %r" % (link.command("skin", "list", timeout=15),))
    print("status   : %r" % (link.command("skin", "status", timeout=10),))
    print("skin vol : %r" % (find_skin_vol(link),))
    print("status   : %r" % (link.command("skin", "status", timeout=10),))


def widen_run(env, i0, n, soft):
    """Grow a loud run outwards while the envelope stays above `soft`.

    The run finder needs a HIGH threshold to be sure it has found the music
    and not the boot chime, and a high threshold clips the attack and the
    decay - which are exactly the two edges the offset is measured against.
    Widening at a floor just above true silence puts them back. (The silence
    in this capture really is 0.0 RMS, so "just above" is not delicate.)
    """
    a, b = i0, i0 + n
    while a > 0 and env[a - 1] >= soft:
        a -= 1
    while b < len(env) and env[b] >= soft:
        b += 1
    return a, b - a


def longest_loud_run(env, thresh):
    """(start index, length) of the longest run of windows at or above
    `thresh`. The music is by far the longest loud thing in the capture (the
    boot chime is a fraction of a second), so this finds it without being told
    where to look - which is the point: the wav clock is not the video clock
    and nothing may assume a relationship it has not measured."""
    best = (0, 0)
    run_start = None
    for i, v in enumerate(env + [0.0]):
        if v >= thresh:
            if run_start is None:
                run_start = i
        else:
            if run_start is not None:
                if i - run_start > best[1]:
                    best = (run_start, i - run_start)
                run_start = None
    return best


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ac97", action="store_true",
                    help="use -device AC97 instead of Intel HDA (AUDIO.md's "
                         "fallback path)")
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--mp3", help="the track to stage at the volume root")
    ap.add_argument("--boot-timeout", type=float, default=240.0)
    ap.add_argument("--esp", default=None,
                    help="ESP tree to boot (default: the UNO_DEBUG=1 tree at "
                         "%s if present, else pc64/build/esp)" % DBG_ESP)
    ap.add_argument("--probe", action="store_true",
                    help="boot, report what the skin beat needs, quit")
    ap.add_argument("--out-dir", metavar="DIR", default="out/final",
                    help="where the artifacts land, absolute or relative to "
                         "tools/demo (default out/final - beside the rest of "
                         "the final cut)")
    ap.add_argument("--gop", default="%dx%d" % (GOP_W, GOP_H),
                    help="panel size to force on QEMU's VGA via EDID. The "
                         "DESKTOP - which is what unostream sends - is half of "
                         "it (uefi_main.c:641), so the default %dx%d is how "
                         "this scene records natively at %dx%d. '0x0' keeps "
                         "QEMU's own panel and the 640x400 desktop with it."
                         % (GOP_W, GOP_H, VID_W, VID_H))
    a = ap.parse_args(argv)

    try:
        gop = tuple(int(v) for v in a.gop.lower().split("x"))
        if len(gop) != 2:
            raise ValueError
    except ValueError:
        raise SystemExit("--gop wants WxH (e.g. 2560x1600, or 0x0)")

    esp = a.esp or (DBG_ESP if os.path.isdir(DBG_ESP) else ESP)
    if not os.path.isdir(esp):
        raise SystemExit("no ESP tree at %s - run UNO_DEBUG=1 ./build.sh" % esp)
    buildtxt = os.path.join(esp, "BUILD.TXT")
    if not os.path.exists(buildtxt):
        raise SystemExit(
            "%s has no BUILD.TXT, so it is a UNO_DEBUG=0 tree. s06 drives the "
            "whole scene over URC, which a production build gates behind a "
            "token typed at the console - build a debug tree (see DBG_ESP in "
            "this file) and pass --esp." % esp)
    build_id = open(buildtxt).read().strip().replace("\n", " | ")
    mp3 = find_mp3(a.mp3)
    if not mp3:
        raise SystemExit("no FLYHIGH.MP3 found (tried %s)" % MP3_CANDIDATES)
    if not os.path.exists(SKIN):
        raise SystemExit("missing skin: %s" % SKIN)
    print("esp: %s\nbuild: %s" % (esp, build_id))
    print("assets: skin=%s track=%s" % (SKIN, mp3))

    out_dir = a.out_dir if os.path.isabs(a.out_dir) \
        else os.path.join(HERE, a.out_dir)
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(PROBE, exist_ok=True)
    base = os.path.join(out_dir, "s06")
    clean_outputs(base)
    wav = base + ".wav"

    print("staging %s" % S06_DISK)
    build_fat_disk(S06_DISK, S06_FAT, DEBUG_CFG, esp=esp,
                   # The skin goes in SKINS\, NOT at the root: a root .wsz is
                   # taken by the boot scan and the player opens already
                   # skinned, which is the one thing this scene must not do.
                   extra=[(SKIN, "::/%s/%s" % (SKIN_DIR, SKIN_NAME)),
                          (mp3, "::/FLYHIGH.MP3")],
                   mkdirs=(SKIN_DIR,),
                   skip=("DOOM1.WAD",),          # 11 MB, and nothing here plays it
                   ordered_dir=("PICTURES", PICS))

    link = UnoAutoLink("127.0.0.1", S06_URC)
    try:
        link.listen()
    except OSError as e:
        raise SystemExit("cannot bind 127.0.0.1:%d (%s) - a previous run is "
                         "still alive" % (S06_URC, e))

    qemu = None
    q = None
    rx = None
    skin_after = None
    t_qemu = time.time()
    beats = Beats(base + ".beats.jsonl")
    err = None
    try:
        qemu = boot_qemu(S06_DISK, wav, ac97=a.ac97, no_audio=a.no_audio,
                         gop=gop)
        q = Qmp(S06_QMP)
        print("qemu up (audio=%s), waiting for the guest to dial in"
              % ("none" if a.no_audio else ("ac97" if a.ac97 else "hda")))
        if not link.wait_connected(a.boot_timeout):
            raise SystemExit("the guest never dialled in - is this the DEBUG "
                             "build, and is DEBUG.CFG's remote= this port?")
        link.wait_hello(30.0)
        time.sleep(2.5)
        d = Driver(link, beats)
        d.w, d.h = link.screen_info(timeout=20)
        d.px, d.py = d.w // 2, d.h // 2
        print("desktop %dx%d, UnoAmp scale %d" % (d.w, d.h, uamp_scale(d.w)))
        # Ask the guest, before the stream exists, whether the panel trick
        # actually took. A wrong answer here is a whole take recorded at the
        # wrong size, and the guest's own screen_info is the only witness that
        # cannot be argued with.
        if gop[0] and (d.w, d.h) != (gop[0] // 2, gop[1] // 2):
            raise SystemExit(
                "the desktop came up %dx%d, not the %dx%d that halving a "
                "%dx%d panel should give - did OVMF refuse the EDID (check "
                "vgamem_mb) or has apply_desktop's default changed?"
                % (d.w, d.h, gop[0] // 2, gop[1] // 2, gop[0], gop[1]))

        # WHICH VOLUME THE SKIN IS ON, asked rather than assumed. The volume
        # index is this boot's mount order, not a property of the disk, and a
        # `skin load` against the wrong one is a refusal mid-take. uno.size is
        # the cheapest question that can only be answered by the file existing.
        if a.probe:
            probe_skin(link)
            return 0
        d.skin_vol = find_skin_vol(link)
        if d.skin_vol is None:
            raise RuntimeError("no volume carries %s\\%s - staging failed"
                               % (SKIN_DIR, SKIN_NAME))
        # And prove the player starts BARE-CHESTED, so a skinned opening frame
        # can never be mistaken for a re-skin that did not happen.
        print("skin: before = %r" % (link.command("skin", "status", timeout=10),))

        rx = StreamReceiver(S06_STREAM, out=base + ".mp4", host="127.0.0.1")
        rx.listen()
        th = threading.Thread(target=rx.serve_once,
                              kwargs={"accept_timeout": 60.0}, daemon=True)
        th.start()
        r = link.command("stream", "start", "10.0.2.2", S06_STREAM, 30,
                         timeout=10)
        if not (r and r[0].startswith("dialing")):
            raise RuntimeError("stream start refused: %r" % r)
        for _ in range(120):
            if rx.connected:
                break
            time.sleep(0.15)
        if not rx.connected:
            raise RuntimeError("guest never connected to the receiver")
        time.sleep(1.0)
        try:
            s06(d)
        except Exception as e:                     # noqa: BLE001
            err = e
            print("  s06 body FAILED: %r" % e)
        time.sleep(1.0)
        try:
            skin_after = link.command("skin", "status", timeout=10)
            print("skin: after = %r" % (skin_after,))
        except Exception:                          # noqa: BLE001
            pass
        try:
            link.command("stream", "stop", timeout=8)
        except Exception:                          # noqa: BLE001
            pass
        th.join(30.0)
    finally:
        beats.close()
        # `quit` over QMP, not kill(): the wav backend patches its RIFF/data
        # sizes on close, and a killed QEMU leaves a header claiming zero bytes.
        try:
            if q:
                q.cmd("quit")
                q.close()
        except Exception:                          # noqa: BLE001
            pass
        time.sleep(1.0)
        if qemu:
            try:
                qemu.wait(timeout=10)
            except Exception:                      # noqa: BLE001
                qemu.kill()
        link.close()

    info = probe(base + ".mp4")
    st = {"scene": "s06", "esp": esp, "build": build_id,
          "mp4": base + ".mp4",
          "mp4_bytes": info.get("bytes"), "dur": info.get("dur"),
          "w": info.get("w"), "h": info.get("h"), "fps": info.get("rate"),
          "panel": ("%dx%d" % gop) if gop[0] else "qemu-default",
          "skin_after": skin_after,
          # stream_recv rescales each segment's timestamps at close (8956f168),
          # so the container is already wall-clock truth. Recorded so nothing
          # downstream retimes it a second time.
          "retimed_by_receiver": bool(getattr(rx, "retimed", False)),
          "frames": rx.frames if rx else 0,
          "keyframes": rx.keyframes if rx else 0,
          "deltas": rx.deltas if rx else 0,
          "decode_errors": rx.decode_errors if rx else 0,
          "segments": rx.segments if rx else 0,
          "audio_device": "none" if a.no_audio else ("ac97" if a.ac97 else "hda")}
    if rx and rx.t_first and rx.t_last and rx.t_last > rx.t_first:
        st["stream_fps"] = round((rx.frames - 1) / (rx.t_last - rx.t_first), 1)
    if err:
        st["error"] = repr(err)

    if not a.no_audio and os.path.exists(wav):
        m = wav_measure(wav)
        env = m.pop("env")
        st["audio"] = m
        peak = max(env) if env else 0.0
        # High threshold to FIND the music (the boot chime is the only other
        # loud thing in the file and the music is much the longer), then
        # widened at a near-silence floor to recover the attack and the decay.
        thresh = max(300.0, peak * 0.12)
        i0, n = longest_loud_run(env, thresh)
        if n:
            i0, n = widen_run(env, i0, n, max(60.0, peak * 0.02))
            music_t = i0 * m["win_ms"] / 1000.0
            music_end = (i0 + n) * m["win_ms"] / 1000.0
            st["audio"]["music_onset_s"] = round(music_t, 2)
            st["audio"]["music_end_s"] = round(music_end, 2)
            st["audio"]["music_length_s"] = round(n * m["win_ms"] / 1000.0, 2)
            st["audio"]["onset_threshold_rms"] = round(thresh, 1)

            # THE TWO CLOCKS, and which pair of events is allowed to join them.
            #
            # The wav sink starts with the MACHINE and the video starts when
            # the guest dials the receiver, so the constant between them has to
            # be measured against something that happens in both - and the two
            # candidates are not equally good:
            #
            #   STOP  is simultaneous BY CONSTRUCTION. The click silences the
            #         DAC in the frame it lands, so `stop` in the video and the
            #         end of the music in the wav are the same instant. This is
            #         the anchor.
            #   PLAY  is not. The click precedes the first sample by however
            #         long UnoAmp takes to open and decode the file - measured
            #         at ~1.4 s on this build by reading the player's own
            #         elapsed counter off the frames. Anchoring here (which is
            #         what this file used to do) puts that latency straight
            #         into the offset and starts the music early.
            #
            # Both are reported, because their DIFFERENCE is worth seeing: it
            # is that open latency plus whatever the wav sink's own clock has
            # drifted, and the sink does drift - under TCG it writes short, so
            # no single constant is exact across the whole passage.
            def vt(name):
                w = next((t for nm, t in beats.marks if nm == name), None)
                return (w - rx.t_first) if (w and rx and rx.t_first) else None

            vt_play, vt_stop = vt("play-flyhigh-mp3"), vt("stop")
            if vt_play is not None:
                st["audio"]["play_beat_video_s"] = round(vt_play, 2)
                st["av_offset_from_play_beat_s"] = round(vt_play - music_t, 2)
            if vt_stop is not None:
                st["audio"]["stop_beat_video_s"] = round(vt_stop, 2)
                st["av_offset_seconds"] = round(vt_stop - music_end, 2)
                st["av_offset_anchor"] = "stop beat vs end of music"
                st["av_offset_meaning"] = (
                    "wav_time = video_time - av_offset_seconds; to lay the wav "
                    "under the cut, trim %.2f s off its front"
                    % -round(vt_stop - music_end, 2))
                if vt_play is not None:
                    st["av_offset_spread_s"] = round(
                        (vt_stop - music_end) - (vt_play - music_t), 2)
                    st["av_offset_spread_meaning"] = (
                        "stop-anchored minus play-anchored = the player's open "
                        "latency plus the wav sink's drift over the passage; "
                        "the two anchors bracket the honest range")
            st["audio"]["qemu_start_to_stream_s"] = (
                round(rx.t_first - t_qemu, 2) if rx and rx.t_first else None)
        st["audio_ok"] = bool(m["peak"] > 1000 and n)
    with open(base + ".stats.json", "w") as f:
        json.dump(st, f, indent=2)
    print(json.dumps(st, indent=2))
    return 0 if not err else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
