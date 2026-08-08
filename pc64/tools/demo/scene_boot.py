#!/usr/bin/env python3
"""s01 - cold boot, firmware handoff to settled desktop (QEMU, QMP screendump).

    python3 scene_boot.py                 -> out/s01.mp4 (+ sidecars)
    python3 scene_boot.py --keep-frames   leave the PPM sequence in out/s01_frames

WHY THIS SCENE CANNOT USE unostream. unostream lives in the shell
(pc64/unostream.c) and is reached over URC, and neither exists until the
desktop does - which is the exact stretch of time this scene is about. So the
boot is photographed from OUTSIDE the machine, with QMP `screendump`, the same
way pc64/docs_shots.py takes the manual's figures.

WHAT THAT COSTS. screendump is not synchronous in QEMU 8.2: the command
returns and the PPM is filled in behind it, so every frame is a dump followed
by a poll until the file parses whole (docs_shots.py:170-192, same reason).
That caps the capture in the single-digit-fps range, and this file does NOT
pretend otherwise: the concat list it hands ffmpeg gives every frame its REAL
measured duration, so the mp4 runs at the speed the boot actually happened.
The achieved rate is measured and written into out/s01.stats.json.

WHICH BUILD, AND WHY IT IS NOT THE DEBUG ONE. `nohud` (pc64/DEBUG.md,
pc64_uui.c:6407) takes the red perf HUD and the stress status line out of the
DESKTOP, and the DEBUG.CFG below sets it - but it does not touch the boot
SPLASH, and the splash is most of this scene. uefi_main.c:430 stamps
"DEBUG / STRESS BUILD <build id>" across it in yellow under a bare `#ifdef
UNO_DEBUG`, with no runtime switch, so on a debug build that banner is in
every frame of a boot. So s01 defaults to a UNO_DEBUG=0 ESP built in a
SEPARATE git worktree (--esp), which leaves pc64/build/esp - shared with the
scenes.py lane - untouched. `--esp <dir>` records any tree; pointing it at
pc64/build/esp gives the debug boot instead, banner and all.

Isolation: its own QMP socket and its own disk image, so it can run beside a
scenes.py session (which owns /tmp/remote_*.img and port 5399).
"""
import argparse, json, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from demo_common import (OUT, PROBE, ESP, VID_W, VID_H, FPS, OVMF_CODE,      # noqa: E402
                         OVMF_VARS, S01_QMP, S01_DISK, S01_FAT, S01_VARS,
                         Qmp, Beats, build_fat_disk, screendump, frame_stats,
                         sig_diff, encode_concat, probe, clean_outputs, sh,
                         send_key)

# The demo stick's keys (pc64/tools/demo/deploy.sh) minus `remote=`: s01 drives
# nothing, so there is no receiver to dial and an unanswered dial is just noise.
# A UNO_DEBUG=0 image ignores this file entirely (it has no HUD, no fuzz driver
# and no auto power-off to switch off); it is written anyway so that --esp
# build/esp gives a filmable debug boot without a second code path.
DEBUG_CFG = ("nohud\n"          # no red perf HUD / stress status line: filmable
             "nostress\n"       # the fuzz driver would open apps on camera
             "noshutdown\n"     # never power itself off mid-take
             "nonet\n")

# The production ESP s01 prefers, built by:
#   git worktree add ~/unodos-s01prod --detach HEAD
#   cp -r pc64/fw-blobs ~/unodos-s01prod/pc64/
#   cd ~/unodos-s01prod/pc64 && UNO_DEBUG=0 sh ./build.sh
PROD_ESP = os.path.expanduser("~/unodos-s01prod/pc64/build/esp")


def boot(disk, qmp_sock):
    sh(["cp", OVMF_VARS, S01_VARS])
    for p in (qmp_sock,):
        if os.path.exists(p):
            os.remove(p)
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + S01_VARS,
        "-drive", "format=raw,file=" + disk,
        # the same NIC the rest of the cut boots with, so the boot this films
        # is the boot those scenes started from
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % qmp_sock,
    ]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def wait_desktop(q, probe_ppm, max_secs=90.0, quiet_secs=2.0, big=0.20):
    """Poll screendumps until the desktop is up and still. Same two-part test
    the capture loop uses - a LARGE repaint (splash -> desktop) followed by
    quiet - because quiet alone cannot tell an arrival from one of the boot's
    multi-second pauses. Returns the settled frame's signature, or None."""
    t0 = time.time()
    prev = None
    seen_big = False
    quiet_since = None
    while time.time() - t0 < max_secs:
        r = screendump(q, probe_ppm)
        if r is None:
            time.sleep(0.2)
            continue
        w, h, px, t = r
        mean, sig = frame_stats(px, w, h)
        d = sig_diff(prev, sig)
        prev = sig
        if mean > 8 and d > big:
            seen_big = True
        if seen_big and mean > 8 and d < 0.004:
            if quiet_since is None:
                quiet_since = t
            elif t - quiet_since >= quiet_secs:
                return sig
        else:
            quiet_since = None
        time.sleep(0.15)
    return None


def seed_used_session(disk, qmp_sock, verbose=True):
    """FIRST BOOT, not recorded: use the machine once, the way a person would,
    so that the SECOND boot restores a bare desktop.

    WHY THIS IS NEEDED AND WHY IT IS NOT A CHEAT. A machine with no SHELL.CFG
    opens the Control Panel (pc64_uui.c session_load: `if (got < 0) {
    open_app(APP_CTRL); ... }`), and so does one whose saved session is EMPTY -
    `open=` with nothing after it still falls through to `if (!any)
    open_app(APP_CTRL)`. There is no setting, and no file, that makes pc64 boot
    to nothing. So the opening shot cannot be a bare desktop by configuration.

    It CAN be one by use. `Alt+Ctrl+F2` moves the focused window to desktop 2
    and follows it; `Ctrl+F1` comes back. Both are documented shortcuts
    (pc64_uui.c:5841), both are two keystrokes, and the move calls
    `session_save()`, which writes `desk.control=1` and `cur_desk=0` into
    SHELL.CFG on the disk - the same file, written by the same function, that a
    person doing the same thing would leave behind. The next boot restores
    exactly that: desktop 1 empty, the Control Panel still open on desktop 2.

    Nothing is hand-written here. The OS writes its own session file; this only
    supplies the keystrokes, over QMP, because a production build has no URC.
    Returns True if the desktop went bare before the reboot.
    """
    probe_ppm = "/tmp/demo_s01_seed.ppm"
    qemu = boot(disk, qmp_sock)
    try:
        q = Qmp(qmp_sock)
        if wait_desktop(q, probe_ppm) is None:
            print("  seed: the desktop never settled - leaving the disk fresh")
            return False
        send_key(q, "alt", "ctrl", "f2")      # move the window to desktop 2
        time.sleep(2.0)
        send_key(q, "ctrl", "f1")             # and come back to desktop 1
        time.sleep(3.0)                       # let session_save reach the disk
    finally:
        try:
            q.cmd("quit")
            q.close()
        except Exception:                     # noqa: BLE001
            pass
        time.sleep(0.5)
        qemu.kill()
        try:
            os.unlink(probe_ppm)
        except OSError:
            pass
    # Verify against the FILE the next boot will actually read, not against
    # pixels. A first pass compared the before/after screendumps and called a
    # working seed a failure: the Control Panel is ~10% of a 1280x800 frame and
    # the row-sum signature barely moves when it goes. SHELL.CFG is
    # unambiguous - and it is the thing session_load consumes.
    cfg = read_shell_cfg(disk)
    ok = bool(cfg) and "desk.control=" in cfg and "cur_desk=0" in cfg
    if verbose:
        print("  seed: SHELL.CFG %s" %
              ("written by the guest, Control Panel parked on desktop 2"
               if ok else "MISSING or unexpected - the recorded boot will show "
               "the out-of-box Control Panel:\n%s" % (cfg or "<no file>")))
    return ok


def read_shell_cfg(disk, part_off=2048 * 512):
    """The SHELL.CFG the guest wrote, read back off the disk image with
    mtools. `disk@@<byte offset>` is how mtools addresses a partition."""
    r = subprocess.run(["mtype", "-i", "%s@@%d" % (disk, part_off),
                        "::/SHELL.CFG"], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    return r.stdout.decode("latin-1") if r.returncode == 0 else ""


def capture(q, frames_dir, max_secs, quiet_secs, verbose=True, big=0.20,
            target_fps=30.0):
    """Dump frames as fast as screendump will settle, and stop once the DESKTOP
    has been still for `quiet_secs` (or `max_secs` elapse).

    "The desktop", not "the picture": the first cut stopped on any quiet lit
    frame and finished 33 s in, on a splash that was still sitting at the
    storage-probe stage - a boot under TCG pauses for whole seconds at a time
    and quiet alone cannot tell a pause from an arrival. So a stop needs a
    LARGE frame-to-frame change (`big`) to have happened first. The splash's
    own animation - a progress segment, a stage caption - moves a few percent
    of the frame; splash -> desktop repaints all of it, so the two are not
    close and the threshold is not delicate.

    PACING. Unpaced, this loop sustained ~90 screendumps/second on this box
    once the PPM sequence was moved off the 9p /mnt/c mount (the first take,
    writing frames into out/, managed 3.4). Ninety is well above both the
    guest's own refresh and the 30 fps container, and each 1280x800 PPM is
    3 MB, so the loop is paced to `target_fps` instead: still every frame the
    output can carry, at a third of the disk. The measured rate goes in
    out/s01.stats.json either way.
    """
    period = 1.0 / target_fps if target_fps else 0.0
    os.makedirs(frames_dir, exist_ok=True)
    for f in os.listdir(frames_dir):
        os.remove(os.path.join(frames_dir, f))
    out = []
    t0 = time.time()
    quiet_since = None
    prev_sig = None
    seen_big = False
    i = 0
    while time.time() - t0 < max_secs:
        due = time.time() + period
        path = os.path.join(frames_dir, "%05d.ppm" % i)
        r = screendump(q, path)
        if r is None:
            time.sleep(0.2)
            continue
        w, h, px, t = r
        mean, sig = frame_stats(px, w, h)
        out.append((path, w, h, t, mean, sig))
        d = sig_diff(prev_sig, sig)
        prev_sig = sig
        if mean > 8 and d > big:
            seen_big = True
            if verbose:
                print("  capture: big repaint at %.1fs (d=%.3f)" % (t - t0, d))
        # "settled" = a lit picture that has stopped changing, AFTER the big
        # repaint. Black counts as neither: OVMF sits on a black screen for
        # seconds before it hands over, and stopping there would film nothing.
        if seen_big and mean > 8 and d < 0.004:
            if quiet_since is None:
                quiet_since = t
            elif t - quiet_since >= quiet_secs:
                if verbose:
                    print("  capture: settled after %.1fs, %d frames"
                          % (t - t0, len(out)))
                break
        else:
            quiet_since = None
        i += 1
        if verbose and i % 60 == 0:
            print("  capture: %d frames, %.1fs, %.1f fps"
                  % (len(out), t - t0, len(out) / (t - t0)))
        slack = due - time.time()
        if slack > 0:
            time.sleep(slack)
    return out


def mark_beats(frames, beats):
    """Derive the scene's beats FROM THE CAPTURED FRAMES, after the fact.

    Nothing here can move a pixel; these are labels on a recording that already
    exists, measured the same way docs_shots.wait_splash measures the splash
    (the deep-navy backdrop at a corner pixel). If a heuristic misses, the
    video is unaffected and only the sidecar is wrong - which is the right way
    round for a label.
    """
    if not frames:
        return {}
    got = {}

    def corner_navy(path, w):
        # re-read the pixel rather than trusting the subsampled signature
        from demo_common import ppm_read
        r = ppm_read(path)
        if not r:
            return False
        fw, fh, px = r
        o = (10 * fw + 10) * 3
        rr, gg, bb = px[o], px[o + 1], px[o + 2]
        return bb > 34 and bb > rr + 12 and gg < 90

    # 1. firmware: the first frame that is not black at all. The threshold is
    #    LOW on purpose - OVMF's own mark is a small logo on a black field and
    #    barely lifts the mean, so a threshold tuned for the splash labels the
    #    splash as the firmware and the two beats land on the same frame.
    for p, w, h, t, mean, sig in frames:
        if mean > 0.5:
            got["firmware-logo"] = t
            break
    # 2. splash: the first UnoDOS navy backdrop
    for p, w, h, t, mean, sig in frames:
        if mean > 6 and corner_navy(p, w):
            got["unodos-splash"] = t
            break
    # 3. boot progress: the splash frames that keep changing (the progress the
    #    loader paints over the backdrop)
    if "unodos-splash" in got:
        for p, w, h, t, mean, sig in frames:
            if t > got["unodos-splash"] + 0.5:
                got["boot-progress"] = t
                break
    # 4. first paint of the desktop: the biggest single frame-to-frame change
    #    after the splash. The splash -> desktop cut is by far the largest
    #    thing that happens in a boot, so the maximum is not a close call.
    base = got.get("unodos-splash", frames[0][3])
    best, bt = 0.0, None
    prev = None
    for p, w, h, t, mean, sig in frames:
        if prev is not None and t > base + 0.3:
            d = sig_diff(prev, sig)
            if d > best:
                best, bt = d, t
        prev = sig
    if bt is not None:
        got["desktop-first-paint"] = bt
    # 5. settled: the first frame after that which is quiet for ~1 s
    if bt is not None:
        prev = None
        quiet_from = None
        for p, w, h, t, mean, sig in frames:
            if t <= bt:
                prev = sig
                continue
            d = sig_diff(prev, sig)
            prev = sig
            if d < 0.004:
                if quiet_from is None:
                    quiet_from = t
                elif t - quiet_from >= 1.0:
                    got["desktop-settled"] = quiet_from
                    break
            else:
                quiet_from = None
    for k in ("firmware-logo", "unodos-splash", "boot-progress",
              "desktop-first-paint", "desktop-settled"):
        if k in got:
            beats.mark(k, t=got[k])
    return got


def build_video(frames, out_mp4, head_black_secs=1.0, tail_pad=1.0):
    """concat-demuxer list with REAL per-frame durations -> a 30 fps mp4.

    Trim the dead black head: firmware holds a black screen for several seconds
    before it hands over and that is not footage. `head_black_secs` of it stays,
    because a cold boot that opens on a dark screen and then lights up reads as
    a cold boot, while one that snaps straight to a splash reads as a cut.
    """
    first_lit = len(frames) - 1
    for i, (p, w, h, t, mean, sig) in enumerate(frames):
        if mean > 6:
            first_lit = i
            break
    t_lit = frames[first_lit][3]
    start = first_lit
    while start > 0 and t_lit - frames[start - 1][3] < head_black_secs:
        start -= 1
    kept = frames[start:]
    if len(kept) < 2:
        raise RuntimeError("nothing captured worth encoding (%d frames)" % len(kept))

    listfile = out_mp4 + ".concat.txt"
    with open(listfile, "w") as f:
        for i, (p, w, h, t, mean, sig) in enumerate(kept):
            nxt = kept[i + 1][3] if i + 1 < len(kept) else t + tail_pad
            f.write("file '%s'\n" % p)
            f.write("duration %.4f\n" % max(0.01, nxt - t))
        f.write("file '%s'\n" % kept[-1][0])       # concat quirk: repeat last
    encode_concat(listfile, out_mp4)
    return kept, listfile


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--max-secs", type=float, default=150.0,
                    help="hard stop on the capture loop")
    ap.add_argument("--quiet-secs", type=float, default=8.0,
                    help="hold this long on the settled desktop, then stop")
    ap.add_argument("--target-fps", type=float, default=30.0,
                    help="pace the screendump loop (0 = as fast as it will go)")
    ap.add_argument("--keep-frames", action="store_true")
    ap.add_argument("--esp", default=None,
                    help="ESP tree to boot (default: the UNO_DEBUG=0 tree at "
                         "%s if present, else pc64/build/esp)" % PROD_ESP)
    ap.add_argument("--suffix", default="",
                    help="write s01<suffix>.* instead of s01.* (for an "
                         "alternate take)")
    ap.add_argument("--session", choices=("used", "fresh"), default="used",
                    help="'used': an unrecorded first boot moves the "
                         "Control-Panel window to desktop 2 (Alt+Ctrl+F2, "
                         "Ctrl+F1), so the RECORDED boot restores a bare "
                         "desktop - what a machine that has been switched on "
                         "once looks like. 'fresh': record the very first "
                         "boot, which opens the Control Panel by design.")
    a = ap.parse_args(argv)

    esp = a.esp or (PROD_ESP if os.path.isdir(PROD_ESP) else ESP)
    if not os.path.isdir(esp):
        raise SystemExit("no ESP tree at %s - run ./build.sh first" % esp)
    # BUILD.TXT is written by the DEBUG build only, so its absence is itself
    # the answer to "which build is this?".
    buildtxt = os.path.join(esp, "BUILD.TXT")
    if os.path.exists(buildtxt):
        build_id = "UNO_DEBUG=1 | " + open(buildtxt).read().strip().replace("\n", " | ")
    else:
        efi = os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI")
        build_id = "UNO_DEBUG=0 (no BUILD.TXT) | BOOTX64.EFI %d bytes" % (
            os.path.getsize(efi) if os.path.exists(efi) else -1)
    print("esp: %s\nbuild: %s" % (esp, build_id))
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(PROBE, exist_ok=True)
    base = os.path.join(OUT, "s01" + a.suffix)
    clean_outputs(base)
    # The PPM sequence lands on the WSL-native filesystem, NOT in out/. A
    # 1280x800 PPM is 3 MB and out/ is on /mnt/c: writing the sequence through
    # the 9p mount, not screendump itself, was the first capture's bottleneck.
    frames_dir = "/tmp/demo_s01_frames"

    print("staging %s" % S01_DISK)
    build_fat_disk(S01_DISK, S01_FAT, DEBUG_CFG, esp=esp)
    seeded = False
    if a.session == "used":
        print("seeding a used session (unrecorded first boot)")
        seeded = seed_used_session(S01_DISK, S01_QMP)
        # The disk is NOT rebuilt after this: the SHELL.CFG the guest just
        # wrote is the whole point, and re-authoring the image would erase it.

    beats = Beats(base + ".beats.jsonl")
    q = None
    qemu = None
    t_boot = time.time()
    try:
        qemu = boot(S01_DISK, S01_QMP)
        q = Qmp(S01_QMP)
        print("qemu up, capturing")
        frames = capture(q, frames_dir, a.max_secs, a.quiet_secs,
                         target_fps=a.target_fps)
    finally:
        try:
            if q:
                q.cmd("quit")
                q.close()
        except Exception:                          # noqa: BLE001
            pass
        time.sleep(0.4)
        if qemu:
            qemu.kill()

    if not frames:
        raise SystemExit("captured no frames at all - did QEMU start?")
    t_first, t_last = frames[0][3], frames[-1][3]
    fps_real = (len(frames) - 1) / (t_last - t_first) if t_last > t_first else 0

    with open(base + ".timing.jsonl", "w") as f:
        for i, (p, w, h, t, mean, sig) in enumerate(frames):
            f.write(json.dumps({"i": i, "t": t, "w": w, "h": h,
                                "mean": round(mean, 2),
                                "bytes": os.path.getsize(p)}) + "\n")
    got = mark_beats(frames, beats)
    beats.close()

    kept, listfile = build_video(frames, base + ".mp4")
    # the final canvas, as a PNG, like stream_recv leaves for the other scenes
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", kept[-1][0],
                    base + ".png"])

    info = probe(base + ".mp4")
    sizes = sorted(set((w, h) for _, w, h, _, _, _ in frames))
    st = {"scene": "s01" + a.suffix, "esp": esp, "build": build_id,
          "session": a.session, "session_seeded": seeded,
          "shell_cfg": read_shell_cfg(S01_DISK).replace("\r\n", " ").strip(),
          "frames_captured": len(frames),
          "frames_kept": len(kept),
          "capture_seconds": round(t_last - t_first, 2),
          "capture_fps": round(fps_real, 2),
          "screendump_sizes": ["%dx%d" % s for s in sizes],
          "boot_to_first_frame": round(t_first - t_boot, 2),
          # relative to the first KEPT frame, i.e. to t=0 of the mp4 - which is
          # what an editor needs. The .beats.jsonl sidecar keeps absolute wall
          # clock, the same convention scenes.py writes for s02-s10.
          "beats": {k: round(v - kept[0][3], 2) for k, v in got.items()},
          "mp4": base + ".mp4", "mp4_bytes": info.get("bytes"),
          "dur": info.get("dur"), "w": info.get("w"), "h": info.get("h"),
          "fps": info.get("rate")}
    with open(base + ".stats.json", "w") as f:
        json.dump(st, f, indent=2)
    print(json.dumps(st, indent=2))
    if not a.keep_frames:
        shutil.rmtree(frames_dir, ignore_errors=True)
        try:
            os.unlink(listfile)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
