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

NO RED HUD. The image is the DEBUG build (build/esp, the one every other scene
in the cut is recorded from), with the DEBUG.CFG below - `nohud` is the flag
that removes the red perf HUD and the stress status line from the frame
(pc64/DEBUG.md, pc64_uui.c:6407). A UNO_DEBUG=0 build would also have no HUD
but would not be the same binary as the rest of the cut.

Isolation: its own QMP socket and its own disk image, so it can run beside a
scenes.py session (which owns /tmp/remote_*.img and port 5399).
"""
import argparse, json, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from demo_common import (OUT, PROBE, ESP, VID_W, VID_H, FPS, OVMF_CODE,      # noqa: E402
                         OVMF_VARS, S01_QMP, S01_DISK, S01_FAT, S01_VARS,
                         Qmp, Beats, build_fat_disk, screendump, frame_stats,
                         sig_diff, encode_concat, probe, clean_outputs, sh)

# The same four keys the demo stick carries (pc64/tools/demo/deploy.sh), minus
# `remote=`: s01 drives nothing, so there is no receiver to dial and an
# unanswered dial is just noise in the log. `nonet` keeps the slow boot network
# probe out of the shot, exactly as scenes.py does for s02-s10.
DEBUG_CFG = ("nohud\n"          # no red perf HUD / stress status line: filmable
             "nostress\n"       # the fuzz driver would open apps on camera
             "noshutdown\n"     # never power itself off mid-take
             "nonet\n")


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


def capture(q, frames_dir, max_secs, quiet_secs, verbose=True, big=0.20):
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
    """
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
        if verbose and i % 25 == 0:
            print("  capture: %d frames, %.1fs, %.1f fps"
                  % (len(out), t - t0, len(out) / (t - t0)))
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

    # 1. firmware: the first frame that is not black at all
    for p, w, h, t, mean, sig in frames:
        if mean > 6:
            got["firmware-handoff"] = t
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
    for k in ("firmware-handoff", "unodos-splash", "boot-progress",
              "desktop-first-paint", "desktop-settled"):
        if k in got:
            beats.mark(k, t=got[k])
    return got


def build_video(frames, out_mp4, head_black=3, tail_pad=1.0):
    """concat-demuxer list with REAL per-frame durations -> a 30 fps mp4.

    Trim the dead black head: OVMF holds a black screen for a while before it
    draws anything, and that is not footage. A few black frames are kept so the
    cut opens on black rather than snapping on mid-firmware.
    """
    first_lit = 0
    for i, (p, w, h, t, mean, sig) in enumerate(frames):
        if mean > 6:
            first_lit = i
            break
    start = max(0, first_lit - head_black)
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
    ap.add_argument("--quiet-secs", type=float, default=4.0,
                    help="hold this long on a still desktop, then stop")
    ap.add_argument("--keep-frames", action="store_true")
    a = ap.parse_args(argv)

    if not os.path.isdir(ESP):
        raise SystemExit("no build/esp - run UNO_DEBUG=1 ./build.sh first")
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(PROBE, exist_ok=True)
    base = os.path.join(OUT, "s01")
    clean_outputs(base)
    # The PPM sequence lands on the WSL-native filesystem, NOT in out/. A
    # 1280x800 PPM is 3 MB and out/ is on /mnt/c: writing the sequence through
    # the 9p mount, not screendump itself, was the first capture's bottleneck.
    frames_dir = "/tmp/demo_s01_frames"

    print("staging %s" % S01_DISK)
    build_fat_disk(S01_DISK, S01_FAT, DEBUG_CFG)

    beats = Beats(base + ".beats.jsonl")
    q = None
    qemu = None
    t_boot = time.time()
    try:
        qemu = boot(S01_DISK, S01_QMP)
        q = Qmp(S01_QMP)
        print("qemu up, capturing")
        frames = capture(q, frames_dir, a.max_secs, a.quiet_secs)
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
    st = {"scene": "s01", "frames_captured": len(frames),
          "frames_kept": len(kept),
          "capture_seconds": round(t_last - t_first, 2),
          "capture_fps": round(fps_real, 2),
          "screendump_sizes": ["%dx%d" % s for s in sizes],
          "boot_to_first_frame": round(t_first - t_boot, 2),
          "beats": {k: round(v - t_first, 2) for k, v in got.items()},
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
