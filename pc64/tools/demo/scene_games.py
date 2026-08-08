#!/usr/bin/env python3
"""s12 - the games, PLAYED, with REAL GUEST AUDIO.

    python3 scene_games.py                  -> out/final2/s12.mp4 + s12.wav
    python3 scene_games.py --probe          short holds, same shape (a rehearsal)
    python3 scene_games.py --no-audio       video only (diagnostic)

Dostris, Pac-Man and OutLast, each started, each actually played, with the
guest's own sound recorded beside the video.

WHY THIS SCENE HAS ITS OWN BOOT, like s06. Every other demo harness passes no
audio device at all. The sound here is QEMU's own wav sink behind an Intel HDA
codec, exactly the pair tools/audio_test.py uses to prove the DAC path:

    -audiodev wav,id=snd0,path=out/final2/s12.wav
    -device intel-hda -device hda-output,audiodev=snd0

THEY ARE .UNO MODULES, NOT NATIVE GAMES - and that one fact decides almost
everything below. pc64_games.c still carries native Dostris/Pac-Man/OutLast
canvases, but `app_game()` (pc64_uui.c:3073) maps ONLY Runner3D onto them:

    static int app_game(int a)
    { int g = (a == EX_RUNNER) ? GAME_RUNNER : -1; ... }

so the three that ship are the bridge modules `apps/dostris.c`, `apps/pacman.c`
and `apps/outlast.c` - "the classic games run as .UNO modules through the
bridge so ALL apps load from storage, the decoupling contract". Two consequences
this scene had to be rebuilt around, both caught by looking at extracted frames
rather than at the beat log:

  1. THEY DO NOT GO FULLSCREEN. `open_app` fullscreens a window only when
     `app_game(a) >= 0`, which is false for all three, and the comment there
     says why: "Bridge apps + the browser stay windowed (they draw at a fixed
     size)". A module paints at absolute offsets inside its own rect
     (`DT_CELL 16`, `DT_BX 10`), so maximizing one would give a big empty
     window with a small game in the corner - worse, not better. They stay
     windowed, and the scene instead DRAGS each window off the icon field into
     the middle of the screen, which is where a 1280x800 frame wants it.
  2. UAF_GAME IS STILL SET on all three (pc64_uui.c:466), so "fullscreen-
     preferred" is true of the descriptor and false of this build's behaviour.
     A scene written from the descriptor would have filmed the wrong thing.

WHICH GAMES MAKE A NOISE (read out of the modules, then MEASURED here):

    Dostris   gm_start(kKoro)   - Korobeiniki, looped.                    SOUND
    OutLast   gm_start(kDrive)  - Sunset Drive, looped.                   SOUND
    Pac-Man   nothing at all    - `apps/pacman.c` calls gm_stop() on game SILENT
                                  over and nothing else. No music, and no
                                  waka: the NATIVE Pac-Man in pc64_games.c
                                  fires uno_seq_beep on every dot, the MODULE
                                  that actually ships does not.

So the middle third of a games scene is mute, which is exactly what the brief
said to report plainly rather than paper over - and why the scene ends on the
TRACKER (`apps/tracker.c`, the 4-channel pattern sequencer): `d` loads its demo
pattern and space plays it, which puts unambiguous music in the scene on its
own terms instead of pretending a silent game was not silent.

THE ORPHANED SONG, and why one command is sent between two games. `gm_start`
is `uno_seq_play` on the ONE global sequencer, and `apps/dostris.c` has no
`closed` handler - the AppInterface's closed slot is null - so closing Dostris
leaves Korobeiniki looping over whatever opens next. The first rehearsal of
this scene measured Pac-Man's window at -12 dB and would have reported a silent
game as loud; the sound was Dostris', still playing. The scene now silences the
sequencer explicitly (`py import uno; uno.quiet()` -> `uno_seq_stop`, mod_uno.c
:441) after Dostris closes, so Pac-Man is measured as what it is. Filed for the
apps lane in pc64/UNOAUTOMATE-REQUESTS.md; nothing here works around it beyond
this one command.

TEMPO IS THE FRAME RATE. uno_seq_tick() runs once per shell frame
(pc64_uui.c:6624) and the song tables are written in 1/60 s ticks, so the music
plays at whatever the shell is drawing - about 22 fps at 1280x800 with a stream
attached, i.e. roughly a third of the written tempo. That is the guest's real
behaviour on this hardware, so it is what gets filmed; `stream_fps` is reported
next to the audio rather than buried.

THE A/V ANCHOR is the last beat, `music-stop`, and it is simultaneous BY
CONSTRUCTION. Closing the Tracker runs `tracker_closed -> tk_stop ->
music_quiet -> uno_seq_stop`, i.e. note-off in the same frame the close lands,
so the instant of that beat is the instant the DAC goes quiet and the last loud
window in the wav is that same instant. The keys that START a game are
deliberately not the anchor, for the reason s06 does not anchor on Play: a
start PRECEDES its first sample by the app's own open latency, and anchoring
there folds that latency into the constant.

RESOLUTION: 1280x800 natively, by doubling the emulated panel via EDID
(demo_common.vga_args) so uefi_main.c's "the desktop is half the GOP mode" rule
lands exactly on the size the cut is assembled at. Nothing is upscaled. See
demo_common's "ONE RESOLUTION" note.
"""
import argparse, json, os, subprocess, sys, threading, time

HERE  = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)

from unoauto_remote import UnoAutoLink                      # noqa: E402
from stream_recv import StreamReceiver                      # noqa: E402
from demo_common import (PROBE, ESP, OVMF_CODE, OVMF_VARS,  # noqa: E402
                         S12_URC, S12_STREAM, S12_DISK, S12_FAT, S12_VARS,
                         GOP_W, GOP_H, VID_W, VID_H, vga_args,
                         Qmp, Beats, build_fat_disk, probe, clean_outputs,
                         wav_measure, volumedetect, sh)
from scene_media import Driver, DBG_ESP                     # noqa: E402

S12_QMP = "/tmp/unodos-demo-s12-qmp.sock"

# EFI scan codes (the `key` verb's scan field) - map_key in uefi_main.c.
S_UP, S_DOWN, S_RIGHT, S_LEFT = 0x01, 0x02, 0x03, 0x04
S_ESC = 0x17

DEBUG_CFG = ("nohud\n"          # a red perf HUD over every frame is unfilmable
             "nostress\n"       # the fuzz driver opens apps on camera
             "noshutdown\n"
             "nonet\n"
             "remote=10.0.2.2:%d\n" % S12_URC)


def boot_qemu(disk, wav, ac97=False, no_audio=False, gop=(GOP_W, GOP_H)):
    sh(["cp", OVMF_VARS, S12_VARS])
    if os.path.exists(S12_QMP):
        os.remove(S12_QMP)
    if wav and os.path.exists(wav):
        os.remove(wav)
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + S12_VARS,
        "-drive", "format=raw,file=" + disk,
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
    ] + vga_args(*gop) + [
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % S12_QMP,
    ]
    if not no_audio:
        cmd += ["-audiodev", "wav,id=snd0,path=" + wav]
        cmd += (["-device", "AC97,audiodev=snd0"] if ac97 else
                ["-device", "intel-hda", "-device", "hda-output,audiodev=snd0"])
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


# ---------------------------------------------------------------------------
# Where the game window lands, MEASURED - and measured OFF CAMERA.
#
# The bridge apps all open at one fixed origin (their AppInterface rect), which
# is resolution-independent, so a single measurement covers all three. It is
# still measured rather than written down, because a literal read off a
# 640x400 probe shot is exactly the class of mistake SCENES.md keeps a section
# about.
#
# Two rules this obeys, both learned the hard way elsewhere in this directory:
#   - a pair of grabs taken seconds apart ALWAYS differs in the taskbar, which
#     carries a clock that reticks every second. The bottom band is excluded,
#     or the "window" comes out 1200 px wide.
#   - it runs BEFORE the stream exists. A screen grab over URC is a few hundred
#     round trips; doing it mid-take would put a hitch in the recording.
# ---------------------------------------------------------------------------
def diff_box(a, b, w, h, skip_bottom=40, scale=1):
    """Bounding box of the pixels that differ between two RGBA grabs."""
    stride = w * 4
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(0, h - skip_bottom):
        ra = a[y * stride:(y + 1) * stride]
        rb = b[y * stride:(y + 1) * stride]
        if ra == rb:
            continue
        if y < y0:
            y0 = y
        y1 = y
        for x in range(w):
            if ra[x * 4:x * 4 + 3] != rb[x * 4:x * 4 + 3]:
                if x < x0:
                    x0 = x
                break
        for x in range(w - 1, -1, -1):
            if ra[x * 4:x * 4 + 3] != rb[x * 4:x * 4 + 3]:
                if x > x1:
                    x1 = x
                break
    if x1 < 0:
        return None
    return (x0 * scale, y0 * scale, (x1 + 1) * scale, (y1 + 1) * scale)


def measure_window(d, app_id, grab_scale=2):
    """(x0, y0, x1, y1) of the window `app_id` opens, on the live desktop.

    `grab_scale=2` halves each axis before the QOI encode, so the grab is a
    quarter of the bytes over a link that moves them 2880 at a time. The box is
    scaled back up; 2 px of slack on a 336 px window is nothing next to what
    the round trips would cost at scale 1.
    """
    w, h, before = d.link.screen_grab(grab_scale, timeout=60)
    d.link.command("launch", app_id, timeout=15)
    time.sleep(2.4)
    w2, h2, after = d.link.screen_grab(grab_scale, timeout=60)
    d.link.command("close", timeout=10)
    time.sleep(0.8)
    if (w, h) != (w2, h2):
        return None
    box = diff_box(before, after, w, h,
                   skip_bottom=max(8, 40 // grab_scale), scale=grab_scale)
    if not box:
        return None
    bw, bh = box[2] - box[0], box[3] - box[1]
    if bw < 120 or bh < 120 or bw > d.w - 40:
        # Too wide to be a window is the diagnostic that caught the taskbar
        # clock in scenes.py; keep it, and fail loudly rather than drag blind.
        print("  measure_window(%s): implausible box %r - ignoring" % (app_id, box))
        return None
    return box


def drag(d, x0, y0, x1, y1, step=64, hold=0.25):
    """One press-move-release, read as a single gesture.

    The holds either side are scenes.py's convention and are not decoration:
    the shell samples pointer state once per frame, so a press and a move
    inside one sample are one event and the window never picks up. The approach
    glide is deliberately coarser than a normal pointer move - it is travel
    between two beats, not part of either, and the injected-pointer queue cares
    about the RATE of injections, not how far each one moves."""
    d.sweep(x0, y0, step=step, pace=0.028)
    d.move(x0, y0, 0, settle=0.1)
    d.move(x0, y0, 1, settle=hold)
    d.sweep(x1, y1, btn=1, step=step, pace=0.028)
    d.move(x1, y1, 1, settle=hold)
    d.move(x1, y1, 0, settle=0.2)


# ---------------------------------------------------------------------------
# The three choreographies.
#
# A module game reads its input very differently from an app: it is STARTED by
# a character ('n' - dostris_key/pacman_key/outlast_key all take it first),
# steered by arrow keys, and there is no key repeat over URC, so "drive the
# car" is literally a stream of discrete presses. The settle between presses is
# what makes it read as play rather than as a burst.
# ---------------------------------------------------------------------------
def play_dostris(d, seconds):
    """Place pieces. Dostris' own gravity is far too slow to film - one row per
    `dt_drop_interval()` at level 1 - so the scene plays the way a person does:
    slide, rotate, and SLAM (space, apps/dostris.c: `while(dt_fits(...)) row++;
    dt_lock()`), which drops the piece and locks it at once. That is a placed
    piece every ~1.7 s, a visibly rising stack, and a real chance of a line
    clear."""
    t_end = time.time() + seconds
    # Alternate the target column so the stack builds ACROSS the board. A tower
    # tops out, and `dt_spawn` ends the game the moment it does - mid-shot, with
    # the music stopping behind it.
    plan = [(-3, 1), (2, 0), (-1, 2), (3, 1), (0, 3), (-2, 0), (1, 2), (-3, 1)]
    i = 0
    while time.time() < t_end:
        dx, rot = plan[i % len(plan)]
        i += 1
        for _ in range(rot):
            d.key(0, S_UP, settle=0.22)
            if time.time() >= t_end:
                return
        step = S_LEFT if dx < 0 else S_RIGHT
        for _ in range(abs(dx)):
            d.key(0, step, settle=0.22)
            if time.time() >= t_end:
                return
        d.key(0, S_DOWN, settle=0.25)       # a visible nudge before the slam
        d.key(ord(' '), 0, settle=0.55)     # slam + lock


def play_pacman(d, seconds):
    """Eat dots. Pac-Man starts at tile (14,19) facing LEFT in a four-tile stub;
    the dots are elsewhere, so the route matters.

    Read off kPmMaze: from (14,19) the only way out of that stub is LEFT to
    column 12 and then UP, which opens onto row 17 - the long unbroken dot
    corridor that runs almost the width of the maze, with a power pellet at
    each end. The route is re-driven rather than pressed once, because a
    direction is only taken at a tile boundary (the `pm_walkable` gate in
    pm_step): a press that arrives mid-tile against a wall is forgotten."""
    t_end = time.time() + seconds
    route = [(S_LEFT, 1.1), (S_UP, 1.5), (S_LEFT, 5.0),      # into row 17, west
             (S_DOWN, 1.2), (S_LEFT, 1.4), (S_UP, 1.6),
             (S_RIGHT, 4.5), (S_DOWN, 1.4), (S_RIGHT, 2.5),
             (S_UP, 1.6), (S_RIGHT, 3.0), (S_DOWN, 1.6)]
    i = 0
    while time.time() < t_end:
        scan, hold = route[i % len(route)]
        i += 1
        d.key(0, scan, settle=0.0)
        time.sleep(min(hold, max(0.0, t_end - time.time())))


def play_outlast(d, seconds):
    """Drive. UP is +4 to the speed (capped at 60), LEFT/RIGHT move the car 9
    units across a 40..280 lane, and the run is on a 60-second clock - so the
    shot is: get up to speed first, then weave through the traffic.

    Accelerating is what makes the road scroll, and the road scrolling is the
    whole picture; a car at speed 0 is a still life of a sunset."""
    t_end = time.time() + seconds
    for _ in range(9):                       # 0 -> 36 of 60, briskly
        d.key(0, S_UP, settle=0.18)
    weave = [(S_RIGHT, 3), (S_UP, 2), (S_LEFT, 4), (S_RIGHT, 2),
             (S_UP, 1), (S_LEFT, 3), (S_RIGHT, 3), (S_DOWN, 1), (S_UP, 2)]
    i = 0
    while time.time() < t_end:
        scan, n = weave[i % len(weave)]
        i += 1
        for _ in range(n):
            d.key(0, scan, settle=0.3)
            if time.time() >= t_end:
                return
        time.sleep(0.35)


# (launch id, beat stem, player, settle after 'n', silence the sequencer after
#  closing it?). Dostris is the one that needs the silencing: it starts a looped
# song and has no `closed` handler to stop it, so without this the next game is
# filmed - and MEASURED - over Korobeiniki.
GAMES = [
    ("dostris", "dostris", play_dostris, 0.6, True),
    ("pacman",  "pacman",  play_pacman,  1.7, False),   # PM_READY holds 40 frames
    ("outlast", "outlast", play_outlast, 0.6, False),   # outlast_closed -> gm_stop
]


def quiet(d):
    """Stop the global sequencer, off camera. One line, because the `py` verb
    is a ONE-LINE exec (REMOTE.md) - every multi-line probe in this directory
    has come back empty for a path that read perfectly."""
    try:
        r = d.link.command("py", "import uno; uno.quiet()", timeout=20)
        print("    py uno.quiet() -> %r" % (r,))
        return True
    except Exception as e:                            # noqa: BLE001
        print("    py uno.quiet() FAILED: %r" % e)
        return False


def s12(d, hold, tracker_hold, win_org):
    """win_org: (x, y) of the top-left of a freshly-opened bridge window, or
    None to leave the windows where they open."""
    # A machine with no saved session boots with the Control Panel open
    # (session_load: no SHELL.CFG -> open_app(APP_CTRL)). Clear it BEFORE the
    # first beat or every frame has a stray window in it.
    d.close_all()
    # Where a game window should END UP. Centring is done as a translation
    # rather than to an absolute rect because the three windows are three
    # different sizes and only one of them was measured; they are within ~180 px
    # of each other, so one delta puts all three near the middle of the frame.
    if win_org:
        grab = (win_org[0] + 110, win_org[1] + 13)     # title bar, left of the
        target = (d.w // 2 - 230 + 110, d.h // 2 - 230 + 13)   # window buttons
    # PACING. Everything between two games is dead time - an empty desktop, a
    # window appearing, a drag - and take 1 spent 8 s on each of those handoffs
    # against 12 s of the game itself. Nothing that is ON SCREEN was shortened
    # here; what came out is settle after the guest has already finished (a
    # window is up well before 2 s) and the pointer's travel between beats.
    for app_id, stem, player, s_new, needs_quiet in GAMES:
        d.beat("launch-" + stem, settle=0.2)
        d.launch(app_id, settle=1.3)
        if win_org:
            # Off the icon field and into the middle of the screen. The games
            # are windowed (see the module docstring) and a 336x410 window in
            # the top-left corner of a 1280x800 frame is not a games shot.
            drag(d, grab[0], grab[1], target[0], target[1])
        d.beat(stem + "-new-game", settle=0.2)
        d.key(ord('n'), 0, settle=s_new)
        d.beat(stem + "-play", settle=0.0)      # <- the measured window opens
        player(d, hold)
        d.beat(stem + "-close", settle=0.0)
        d.link.command("close", timeout=10)
        time.sleep(0.5)
        if needs_quiet:
            quiet(d)
            d.beat(stem + "-sequencer-silenced", settle=0.2)

    # The music the games do not all have. `d` loads kTkDemo (the module opens
    # with an EMPTY pattern - kTkDemo is only reachable from that key), space
    # toggles play, and closing the window is `tracker_closed -> tk_stop ->
    # music_quiet`, which is the A/V anchor.
    d.beat("launch-tracker", settle=0.2)
    d.launch("tracker", settle=1.3)
    if win_org:
        drag(d, grab[0], grab[1], target[0], target[1])
    d.beat("tracker-load-pattern", settle=0.2)
    d.key(ord('d'), 0, settle=0.5)
    d.beat("tracker-play", settle=0.0)
    d.key(ord(' '), 0, settle=0.0)
    time.sleep(tracker_hold)
    d.beat("music-stop", settle=0.0)
    d.link.command("close", timeout=10)
    time.sleep(1.2)


# ---------------------------------------------------------------------------
def last_loud(env, win_ms, thresh):
    """End time (s) of the LAST window at or above `thresh`.

    s06 finds the longest loud RUN, because it films one continuous track. This
    scene's audio is several passages with real gaps between them - one of them
    a game that makes no sound at all - so "longest run" would find whichever
    passage happened to be longest and anchor on ITS end, not on the event the
    anchor is defined by. What is wanted is the moment the machine went quiet
    and stayed quiet, which is the last loud window and nothing else."""
    for i in range(len(env) - 1, -1, -1):
        if env[i] >= thresh:
            return (i + 1) * win_ms / 1000.0
    return None


def extract_frames(mp4, out_dir, marks, t_first, tag):
    """One PNG per named beat, pulled OUT OF THE RECORDING.

    The whole point: a beat log says what the driver SENT, not what the guest
    drew. This scene was rebuilt once already because a frame disagreed with a
    log that said everything worked, so the frames are produced here as a
    deliverable rather than left for someone to go looking for."""
    os.makedirs(out_dir, exist_ok=True)
    shots = []
    for name, t in marks:
        vt = t - t_first
        if vt < 0:
            continue
        p = os.path.join(out_dir, "%s_%s.png" % (tag, name.replace("-", "_")))
        r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error",
                            "-ss", "%.3f" % vt, "-i", mp4,
                            "-frames:v", "1", p], stderr=subprocess.PIPE)
        if r.returncode == 0 and os.path.exists(p):
            shots.append({"beat": name, "video_s": round(vt, 2), "png": p})
    return shots


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ac97", action="store_true",
                    help="use -device AC97 instead of Intel HDA")
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--hold", type=float, default=12.0,
                    help="seconds of actual PLAY per game (default 12)")
    ap.add_argument("--tracker-hold", type=float, default=6.0,
                    help="seconds the Tracker plays its demo pattern")
    ap.add_argument("--no-centre", action="store_true",
                    help="leave each game window where it opens (top-left, over "
                         "the desktop icons) instead of dragging it centre")
    ap.add_argument("--probe", action="store_true",
                    help="the same scene at 4 s a game: a rehearsal that "
                         "measures the audio without spending a full take")
    ap.add_argument("--boot-timeout", type=float, default=240.0)
    ap.add_argument("--esp", default=None,
                    help="ESP tree to boot (default: the UNO_DEBUG=1 tree at "
                         "%s if present, else pc64/build/esp)" % DBG_ESP)
    ap.add_argument("--out-dir", metavar="DIR", default="out/final2",
                    help="where the artifacts land, absolute or relative to "
                         "tools/demo (default out/final2)")
    ap.add_argument("--gop", default="%dx%d" % (GOP_W, GOP_H),
                    help="panel size to force on QEMU's VGA via EDID. The "
                         "DESKTOP is half of it (uefi_main.c:641), so the "
                         "default %dx%d records natively at %dx%d."
                         % (GOP_W, GOP_H, VID_W, VID_H))
    a = ap.parse_args(argv)

    try:
        gop = tuple(int(v) for v in a.gop.lower().split("x"))
        if len(gop) != 2:
            raise ValueError
    except ValueError:
        raise SystemExit("--gop wants WxH (e.g. 2560x1600, or 0x0)")
    hold = 4.0 if a.probe else a.hold
    tracker_hold = 3.0 if a.probe else a.tracker_hold

    esp = a.esp or (DBG_ESP if os.path.isdir(DBG_ESP) else ESP)
    if not os.path.isdir(esp):
        raise SystemExit("no ESP tree at %s - run UNO_DEBUG=1 ./build.sh" % esp)
    buildtxt = os.path.join(esp, "BUILD.TXT")
    if not os.path.exists(buildtxt):
        raise SystemExit(
            "%s has no BUILD.TXT, so it is a UNO_DEBUG=0 tree. s12 drives the "
            "whole scene over URC, which a production build gates behind a "
            "token typed at the console - build a debug tree and pass --esp."
            % esp)
    build_id = open(buildtxt).read().strip().replace("\n", " | ")
    print("esp: %s\nbuild: %s" % (esp, build_id))

    out_dir = a.out_dir if os.path.isabs(a.out_dir) \
        else os.path.join(HERE, a.out_dir)
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(PROBE, exist_ok=True)
    base = os.path.join(out_dir, "s12")
    clean_outputs(base)
    wav = base + ".wav"

    print("staging %s" % S12_DISK)
    build_fat_disk(S12_DISK, S12_FAT, DEBUG_CFG, esp=esp,
                   skip=("DOOM1.WAD",))     # 11 MB; nothing here opens it

    link = UnoAutoLink("127.0.0.1", S12_URC)
    try:
        link.listen()
    except OSError as e:
        raise SystemExit("cannot bind 127.0.0.1:%d (%s) - a previous run is "
                         "still alive" % (S12_URC, e))

    qemu = q = rx = None
    win_box = None
    t_qemu = time.time()
    beats = Beats(base + ".beats.jsonl")
    err = None
    try:
        qemu = boot_qemu(S12_DISK, wav, ac97=a.ac97, no_audio=a.no_audio,
                         gop=gop)
        q = Qmp(S12_QMP)
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
        print("desktop %dx%d" % (d.w, d.h))
        # Ask the guest, before the stream exists, whether the panel trick took.
        # A wrong answer here is a whole take at the wrong size.
        if gop[0] and (d.w, d.h) != (gop[0] // 2, gop[1] // 2):
            raise SystemExit(
                "the desktop came up %dx%d, not the %dx%d that halving a "
                "%dx%d panel should give - did OVMF refuse the EDID (check "
                "vgamem_mb) or has apply_desktop's default changed?"
                % (d.w, d.h, gop[0] // 2, gop[1] // 2, gop[0], gop[1]))

        # OFF CAMERA: where does a bridge window open? Measured by opening one
        # and diffing, then closed again - so the take never drags blind.
        d.close_all()
        time.sleep(0.6)
        if not a.no_centre:
            win_box = measure_window(d, "dostris")
            print("bridge window box: %r" % (win_box,))
            quiet(d)                       # the measurement started Dostris'
            time.sleep(0.4)                # window, not its song, but be sure

        rx = StreamReceiver(S12_STREAM, out=base + ".mp4", host="127.0.0.1")
        rx.listen()
        th = threading.Thread(target=rx.serve_once,
                              kwargs={"accept_timeout": 60.0}, daemon=True)
        th.start()
        r = link.command("stream", "start", "10.0.2.2", S12_STREAM, 30,
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
            s12(d, hold, tracker_hold, win_box[:2] if win_box else None)
        except Exception as e:                     # noqa: BLE001
            err = e
            print("  s12 body FAILED: %r" % e)
        time.sleep(0.8)
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
    st = {"scene": "s12", "esp": esp, "build": build_id,
          "mp4": base + ".mp4",
          "mp4_bytes": info.get("bytes"), "dur": info.get("dur"),
          "w": info.get("w"), "h": info.get("h"), "fps": info.get("rate"),
          "panel": ("%dx%d" % gop) if gop[0] else "qemu-default",
          "play_seconds_per_game": hold,
          "window_box_measured": win_box,
          "apps": [g[0] for g in GAMES] + ["tracker"],
          "retimed_by_receiver": bool(getattr(rx, "retimed", False)),
          "frames": rx.frames if rx else 0,
          "keyframes": rx.keyframes if rx else 0,
          "deltas": rx.deltas if rx else 0,
          "decode_errors": rx.decode_errors if rx else 0,
          "segments": rx.segments if rx else 0,
          "audio_device": "none" if a.no_audio else ("ac97" if a.ac97 else "hda")}
    if rx and rx.t_first and rx.t_last and rx.t_last > rx.t_first:
        st["stream_fps"] = round((rx.frames - 1) / (rx.t_last - rx.t_first), 1)
        # The shell's frame rate IS the music's tempo (uno_seq_tick runs once
        # per frame), so it is reported next to the audio, not buried.
        st["tempo_note"] = ("uno_seq_tick() runs once per shell frame, so the "
                            "songs play at stream_fps/60 of their written tempo")
    if err:
        st["error"] = repr(err)

    marks = list(beats.marks)
    t_first = rx.t_first if rx else None

    def vt(name):
        w = next((t for nm, t in marks if nm == name), None)
        return (w - t_first) if (w and t_first) else None

    if not a.no_audio and os.path.exists(wav):
        m = wav_measure(wav)
        env = m.pop("env")
        st["audio"] = m
        peak = max(env) if env else 0.0
        # Low threshold on purpose. The question this answers is "when did the
        # machine go quiet", and a threshold set high enough to find MUSIC would
        # skip past a quieter passage and move the anchor.
        thresh = max(60.0, peak * 0.02)
        st["audio"]["loud_threshold_rms"] = round(thresh, 1)
        end = last_loud(env, m["win_ms"], thresh)
        st["audio"]["last_loud_s"] = round(end, 2) if end else None
        st["audio"]["qemu_start_to_stream_s"] = (
            round(t_first - t_qemu, 2) if t_first else None)

        vt_stop = vt("music-stop")
        off = None
        if end is not None and vt_stop is not None:
            off = vt_stop - end
            st["av_offset_seconds"] = round(off, 2)
            st["av_offset_anchor"] = (
                "the `music-stop` beat vs the last loud window in the wav - "
                "closing the Tracker runs tracker_closed -> tk_stop -> "
                "music_quiet -> uno_seq_stop, i.e. note-off in the SAME frame "
                "the close lands, so those two are one instant")
            st["av_offset_meaning"] = (
                "wav_time = video_time - av_offset_seconds; to lay the wav "
                "under the cut, trim %.2f s off its front" % -off)
        st["audio_ok"] = bool(m["peak"] > 1000 and end)

        # PER APP. `volumedetect` is ffmpeg's own measurement over the slice of
        # wav each PLAY window maps to. Whole-file numbers cannot tell a scene
        # where everything made a noise from one where a single looping song
        # covered a game that made none - which is exactly what happened in the
        # first rehearsal of this scene.
        st["audio_whole_file"] = volumedetect(wav)
        windows = [(g[1], g[1] + "-play", g[1] + "-close") for g in GAMES]
        windows.append(("tracker", "tracker-play", "music-stop"))
        per = []
        for name, b0, b1 in windows:
            v0, v1 = vt(b0), vt(b1)
            row = {"app": name, "video_start_s": round(v0, 2) if v0 else None,
                   "video_end_s": round(v1, 2) if v1 else None}
            if v0 is not None and v1 is not None and off is not None:
                vd = volumedetect(wav, ss=v0 - off, t=(v1 - v0))
                row.update(vd)
                row["has_sound"] = bool(vd["max_db"] is not None and
                                        vd["max_db"] > -40.0)
            per.append(row)
        st["audio_per_app"] = per
        st["apps_with_sound"] = [r["app"] for r in per if r.get("has_sound")]
        st["apps_silent"] = [r["app"] for r in per if r.get("has_sound") is False]

    # Frames, from the recording, for every beat - plus one from the MIDDLE of
    # each play window, which is where "the game is actually being played" is
    # visible and a beat boundary is not.
    if t_first and os.path.exists(base + ".mp4"):
        mids = []
        for stem, b0, b1 in [(g[1], g[1] + "-play", g[1] + "-close")
                             for g in GAMES] + \
                            [("tracker", "tracker-play", "music-stop")]:
            v0, v1 = vt(b0), vt(b1)
            if v0 is not None and v1 is not None:
                mids.append((stem + "-mid", t_first + (v0 + v1) / 2.0))
                mids.append((stem + "-late",
                             t_first + v0 + (v1 - v0) * 0.85))
        st["frames_extracted"] = extract_frames(
            base + ".mp4", os.path.join(out_dir, "s12_frames"),
            marks + mids, t_first, "s12")

    with open(base + ".stats.json", "w") as f:
        json.dump(st, f, indent=2)
    print(json.dumps(st, indent=2))
    return 0 if not err else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
