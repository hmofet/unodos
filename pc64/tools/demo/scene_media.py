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
The wav sink starts with the MACHINE (QEMU opens it before OVMF runs) and the
video starts when the GUEST dials the receiver, tens of seconds later. Nothing
ties them together, so at the end the music's onset is located in the wav
(the longest loud run) and compared with the wall clock of the `play` beat.
The measured offset goes in out/s06.stats.json as `av_offset_seconds`: add it
to a video timestamp to get the wav timestamp.

THE SKIN BEAT DOES NOT HAPPEN, AND THE REASON IS A BUG. Two separate things
block it, both in the unoamp lane and neither in the driving:

  1. BASE291.WSZ DOES NOT LOAD AT ALL. Ten of its thirteen sheets - MAIN.BMP
     included, and MAIN is the load gate - are BI_RLE8 BMPs, and
     unoamp_skin.c's bmp_decode() refuses any compressed BMP outright:
     `if (comp != 0) return 0;   /* RLE skins do not exist */`.
     They do; a stock Winamp 2.9x skin is full of them. Reproduced host-side
     in one second, no emulator involved:
         cc -I. -I../unomedia -o /tmp/skintest tools/skintest.c unoamp_skin.c \\
            ../unomedia/um_inflate.c ../unomedia/unomedia.c
         /tmp/skintest wads/BASE291.WSZ      -> FAILED, MAIN.BMP did not decode
     Un-RLE the ten sheets and the same archive loads 13/13 with VISCOLOR and
     PLEDIT, which is the whole diagnosis.
  2. EVEN FIXED, IT COULD NOT RE-SKIN ON CAMERA. `load_a_skin()` runs from
     `unoamp_start()`, guarded by a one-shot `g_started` and called once from
     `unoamp_ui_build()`. Reopening the player does not re-run it, nothing
     resets `g_started`, and `unoamp_skin_load()` is exported to neither the
     URC verb table nor the `uno` Python module. A skin is chosen once per
     boot, so a live re-skin has nowhere to be triggered from.

The scene still stages the real BASE291.WSZ at the volume ROOT (uno_fs lists
roots only - unoamp_app.c:288 - so a nested folder is invisible to it), because
that is what makes the bug visible and what will produce a skinned chassis the
day bmp_decode learns RLE8. Today the player is filmed in its built-in look and
the beat is named for what it is.
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
                         Qmp, Beats, build_fat_disk, probe, clean_outputs,
                         wav_measure, sh)

S06_QMP = "/tmp/unodos-demo-s06-qmp.sock"

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


def boot_qemu(disk, wav, ac97=False, no_audio=False):
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

    def sweep(self, x1, y1, btn=0, step=12, pace=0.035, burst=20):
        x0, y0 = self.px, self.py
        dx, dy = x1 - x0, y1 - y0
        n = max(1, int(max(abs(dx), abs(dy)) / step))
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
    d.launch("unoamp", settle=3.0)          # skin attempt + playlist scan
    # Named for what is on screen. The staged BASE291.WSZ is refused by
    # bmp_decode (BI_RLE8 - see the module docstring), so this is the built-in
    # theme-coloured chassis, not the Winamp skin.
    d.beat("unoamp-opens-builtin-look", settle=2.0)

    # Glide FIRST, mark the beat, then press. The beat's wall clock is what
    # pins the video clock to the wav clock later, so it has to sit next to the
    # button press and not a second and a half of pointer travel before it.
    play = uamp_btn(fbw, T_PLAY)
    d.sweep(*play)
    d.beat("play-flyhigh-mp3", settle=0.0)
    d.click(*play, glide=False, settle=1.2)

    # The elapsed-time digits and the position bar are what move here. The
    # visualiser well is drawn but effectively frozen: measured over 6 s of
    # this recording it changed on 11 of 179 frames, because the player only
    # asks the shell to repaint when its title MARQUEE advances
    # (unoamp_ui_tick) and "FLYHIGH" is short enough to fit without scrolling.
    d.beat("hold-playing", settle=0.2)
    time.sleep(4.5)

    d.beat("open-the-10-band-eq", settle=0.2)
    d.click(*uamp_eq(fbw), settle=1.0)
    time.sleep(3.5)                          # the EQ window docks below

    d.beat("stop", settle=0.2)
    d.click(*uamp_btn(fbw, T_STOP), settle=0.8)
    # The skin's own close box: it calls unoamp_ui_close(), which takes the
    # player AND its EQ/playlist windows down together. The shell's `close`
    # verb would only remove whichever one happens to be focused.
    d.beat("close-unoamp", settle=0.2)
    d.click(*uamp_close(fbw), settle=1.2)
    d.close_all(3)              # belt and braces: the first take left a
                                # "UnoAmp" chip on the taskbar through Photos

    d.beat("launch-photos", settle=0.3)
    d.launch("photos", settle=3.5)           # opens straight into PICTURES\
    d.beat("jpeg", settle=0.2)
    time.sleep(2.0)
    for tag, hold in (("png-alpha", 2.0),
                      ("animated-gif", 4.5),   # hold: the animation IS the beat
                      ("bmp", 2.2)):
        d.beat(tag, settle=0.2)
        d.key(0, S_RIGHT, settle=0.4)
        time.sleep(hold)
    d.beat("close", settle=0.2)
    d.close_all()


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
    a = ap.parse_args(argv)

    if not os.path.isdir(ESP):
        raise SystemExit("no build/esp - run UNO_DEBUG=1 ./build.sh first")
    mp3 = find_mp3(a.mp3)
    if not mp3:
        raise SystemExit("no FLYHIGH.MP3 found (tried %s)" % MP3_CANDIDATES)
    if not os.path.exists(SKIN):
        raise SystemExit("missing skin: %s" % SKIN)
    print("assets: skin=%s track=%s" % (SKIN, mp3))
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(PROBE, exist_ok=True)
    base = os.path.join(OUT, "s06")
    clean_outputs(base)
    wav = base + ".wav"

    print("staging %s" % S06_DISK)
    build_fat_disk(S06_DISK, S06_FAT, DEBUG_CFG,
                   extra=[(SKIN, "::/BASE291.WSZ"), (mp3, "::/FLYHIGH.MP3")],
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
    t_qemu = time.time()
    beats = Beats(base + ".beats.jsonl")
    err = None
    try:
        qemu = boot_qemu(S06_DISK, wav, ac97=a.ac97, no_audio=a.no_audio)
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
    st = {"scene": "s06", "mp4": base + ".mp4",
          "mp4_bytes": info.get("bytes"), "dur": info.get("dur"),
          "w": info.get("w"), "h": info.get("h"), "fps": info.get("rate"),
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
        thresh = max(300.0, peak * 0.12)
        i0, n = longest_loud_run(env, thresh)
        if n:
            music_t = i0 * m["win_ms"] / 1000.0
            st["audio"]["music_onset_s"] = round(music_t, 2)
            st["audio"]["music_length_s"] = round(n * m["win_ms"] / 1000.0, 2)
            st["audio"]["onset_threshold_rms"] = round(thresh, 1)
            # the whole point of the two clocks: video t of the play beat,
            # minus wav t of the music, is the constant the editor needs.
            play_wall = next((t for nm, t in beats.marks
                              if nm == "play-flyhigh-mp3"), None)
            if play_wall and rx and rx.t_first:
                vt = play_wall - rx.t_first
                st["audio"]["play_beat_video_s"] = round(vt, 2)
                st["av_offset_seconds"] = round(vt - music_t, 2)
                st["av_offset_meaning"] = (
                    "wav_time = video_time - av_offset_seconds "
                    "(i.e. the wav starts av_offset_seconds BEFORE the video)")
            st["audio"]["qemu_start_to_stream_s"] = (
                round(rx.t_first - t_qemu, 2) if rx and rx.t_first else None)
        st["audio_ok"] = bool(m["peak"] > 1000 and n)
    with open(base + ".stats.json", "w") as f:
        json.dump(st, f, indent=2)
    print(json.dumps(st, indent=2))
    return 0 if not err else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
