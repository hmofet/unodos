#!/usr/bin/env python3
"""s11 - the ports outro. No emulator: a montage over COMMITTED screenshots.

    python3 scene_outro.py            -> out/s11.mp4 (+ out/s11.beats.jsonl)
    python3 scene_outro.py --check    just verify every source image exists

Every frame is a real screenshot that is already in this repository, taken on
(or in an emulator of) the machine it is captioned with. Nothing is drawn,
mocked up or re-created here; the only thing this file adds to a frame is the
platform's name, and the final card.

WHERE THE IMAGES COME FROM. `docs/assets/img/port_*.png` is the curated set the
manual's ports page uses (docs/build_site.py, PAGES["ports.html"]), and it
covers seven machines. The manual's own table lists twenty-two, so the rest are
taken from each port's committed shots - the same pictures its AUDIT/HANDOFF
docs cite. Hardware names and their spelling come from that table, so this
montage and the manual cannot drift apart.

BUILD SHAPE. Two ffmpeg passes rather than one enormous filtergraph:
  1. per platform, one 640x400 card PNG - the screenshot fitted (never cropped,
     never stretched) onto black, with a caption bar burned in;
  2. those cards, each held `--hold` seconds, chained through xfade.
Splitting it keeps every intermediate inspectable, which matters when the input
is twenty-odd images of wildly different sizes.
"""
import argparse, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from demo_common import (OUT, REPO, VID_W, VID_H, FPS, font_path, probe,  # noqa: E402
                         Beats, clean_outputs)

# (caption, repo-relative image, crop-or-None). Order is roughly by era, so
# the montage reads as a lineage rather than a shuffled grid. Every name is
# the "Hardware" column of the manual's ports table (docs/build_site.py).
#
# THE CROPS ARE HOST-EMULATOR CHROME, NOTHING ELSE. Several ports' committed
# shots are window captures - a Mesen or BlastEm window, title bar and menu bar
# included - and a montage of emulator windows is a montage of Windows, not of
# UnoDOS. Each crop rect below was MEASURED (the first row/column that is not
# the light-grey Windows chrome), never eyeballed, and removes only chrome:
# every pixel that survives is a pixel the port drew. Where a native-resolution
# capture of the same screen exists it is preferred over cropping (NES, SNES).
PLATFORMS = [
    ("IBM PC/XT",           "docs/assets/img/classic_xt.png",   None),
    ("Commodore VIC-20",    "vic20/build/desktop.png",          None),
    ("Commodore 64",        "docs/assets/img/port_c64.png",     None),
    ("Commodore Amiga",     "amiga/build/desktop.png",          "720:560:0:0"),
    ("Apple II",            "apple2/shots/da_desktop.png",      None),
    ("Apple IIGS",          "docs/assets/img/port_iigs.png",    None),
    ("Compact Macintosh",   "docs/assets/img/port_mac.png",     None),
    ("Power Macintosh",     "docs/assets/img/port_ppcmac.png",  None),
    ("Nintendo NES",        "nes/build/h_launcher.png",         None),
    ("Super Nintendo",      "snes/build/m3_theme.png",          None),
    ("Nintendo Game Boy",   "gb/build/desktop.png",             "672:607:0:83"),
    ("Nintendo GBA",        "gba/build/desktop.png",            None),
    ("Sega Master System",  "sms/build/desktop.png",            "656:488:0:31"),
    ("Sega Game Gear",      "gg/build/desktop.png",             "672:607:0:83"),
    ("Sega Mega Drive",     "genesis/build/desktop.png",        "656:488:0:31"),
    ("NEC TurboGrafx-16",   "pce/build/desktop.png",            None),
    ("Bandai WonderSwan",   "ws/build/desktop.png",             None),
    ("Sega Dreamcast",      "docs/assets/img/port_dreamcast.png", None),
    ("Sony PlayStation 2",  "ps2/shots/m1_desktop.png",         None),
    ("Raspberry Pi",        "docs/assets/img/port_rpi.png",     None),
    ("PinePhone",           "docs/assets/img/port_pinephone.png", None),
    ("Modern PC (UEFI)",    "docs/assets/img/desktop.png",      None),
]

# The closing card. Plain on purpose - the montage is the claim, this only
# names it. The line is the manual's own ports-page lede, cut to one sentence.
CARD_TITLE = "UnoDOS"
CARD_LINE  = "One GUI-first operating system, written from scratch,"
CARD_LINE2 = "running on more than twenty kinds of hardware."


def esc(s):
    """drawtext's text= is parsed twice; a colon or a quote in a caption ends
    the option. None of ours carry one today, so this is a guard rather than a
    workaround - it keeps a future 'Sega Mega Drive: 32X' from silently
    truncating the burn-in."""
    return s.replace("\\", "\\\\").replace(":", "\\:").replace("'", "\\'")


def make_card(src, caption, dst, font, index=None, total=None, crop=None):
    """One 640x400 card: the screenshot fitted onto black, a caption bar and
    the platform name. `fit` never crops - a console frame is 256x224 and a
    pc64 desktop is 1280x800, and cropping either to fill would be a lie about
    what the machine draws.

    A 256x224 console frame and a 1280x800 desktop are both real frames and
    both have to stay real, so both are letterboxed rather than filled."""
    bar_h = 46
    inner_h = VID_H - bar_h
    vf = (
        ("crop=%s," % crop if crop else "") +
        "scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,"
        "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x0B0E14,"
        "pad=%d:%d:0:0:color=0x0B0E14,setsar=1,"
        "drawbox=x=0:y=%d:w=%d:h=%d:color=0x11161F@1:t=fill,"
        "drawbox=x=0:y=%d:w=%d:h=2:color=0x3C82F6@1:t=fill,"
        "drawtext=fontfile='%s':text='%s':fontcolor=0xFFFFFF:fontsize=26:"
        "x=24:y=%d"
        % (VID_W, inner_h, VID_W, inner_h, VID_W, VID_H,
           inner_h, VID_W, bar_h,
           inner_h, VID_W,
           font, esc(caption), inner_h + 10)
    )
    if index is not None:
        vf += (",drawtext=fontfile='%s':text='%s':fontcolor=0x8A93A6:"
               "fontsize=18:x=w-tw-24:y=%d"
               % (font, esc("%d / %d" % (index, total)), inner_h + 15))
    r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", src,
                        "-vf", vf, "-frames:v", "1", dst])
    if r.returncode != 0:
        raise RuntimeError("card build failed for %s" % src)
    return dst


def make_endcard(dst, font):
    vf = (
        "drawtext=fontfile='%s':text='%s':fontcolor=0xFFFFFF:fontsize=64:"
        "x=(w-tw)/2:y=118,"
        "drawbox=x=(iw-160)/2:y=196:w=160:h=2:color=0x3C82F6@1:t=fill,"
        "drawtext=fontfile='%s':text='%s':fontcolor=0xC7CEDB:fontsize=21:"
        "x=(w-tw)/2:y=228,"
        "drawtext=fontfile='%s':text='%s':fontcolor=0xC7CEDB:fontsize=21:"
        "x=(w-tw)/2:y=258"
        % (font, esc(CARD_TITLE), font, esc(CARD_LINE), font, esc(CARD_LINE2))
    )
    r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error",
                        "-f", "lavfi", "-i", "color=c=0x0B0E14:s=%dx%d" % (VID_W, VID_H),
                        "-vf", vf, "-frames:v", "1", dst])
    if r.returncode != 0:
        raise RuntimeError("end card build failed")
    return dst


def build(hold, xfade, card_hold, outfile, beats):
    font = font_path()
    work = os.path.join(OUT, "s11_cards")
    os.makedirs(work, exist_ok=True)

    cards = []
    missing = []
    n = len(PLATFORMS)
    for i, (name, rel, crop) in enumerate(PLATFORMS):
        src = os.path.join(REPO, rel.replace("/", os.sep))
        if not os.path.exists(src):
            missing.append(rel)
            continue
        dst = os.path.join(work, "c%02d.png" % i)
        make_card(src, name, dst, font, index=len(cards) + 1, total=n, crop=crop)
        cards.append((name, dst))
    if missing:
        raise SystemExit("missing source screenshots: %s" % missing)
    end = make_endcard(os.path.join(work, "zz_end.png"), font)

    # xfade chain. Each input is a still looped for `hold`; the transition
    # eats `xfade` seconds of overlap, so a card is on screen alone for
    # hold - xfade and the sequence advances every (hold - xfade).
    inputs = []
    for _, p in cards:
        inputs += ["-loop", "1", "-t", str(hold), "-i", p]
    inputs += ["-loop", "1", "-t", str(card_hold), "-i", end]

    step = hold - xfade
    fc = []
    prev = "0:v"
    off = step
    for i in range(1, len(cards) + 1):
        lab = "x%d" % i
        fc.append("[%s][%d:v]xfade=transition=fade:duration=%s:offset=%s[%s]"
                  % (prev, i, xfade, round(off, 3), lab))
        prev = lab
        off += step
    fc.append("[%s]format=yuv420p,fps=%d[v]" % (prev, FPS))
    cmd = (["ffmpeg", "-y", "-loglevel", "error"] + inputs +
           ["-filter_complex", ";".join(fc), "-map", "[v]",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "18",
            "-pix_fmt", "yuv420p", outfile])
    r = subprocess.run(cmd)
    if r.returncode != 0:
        raise RuntimeError("montage encode failed")

    # Beats are DERIVED from the timeline we just built, so the sidecar and the
    # video cannot disagree: card k is fully up at k*(hold-xfade).
    t0 = time.time()
    for i, (name, _) in enumerate(cards):
        beats.mark("platform-" + name.lower().replace(" ", "-").replace("/", "-"),
                   t=t0 + i * step)
    beats.mark("end-card", t=t0 + len(cards) * step)
    return outfile


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--hold", type=float, default=1.25,
                    help="seconds each card is held (incl. its crossfade)")
    ap.add_argument("--xfade", type=float, default=0.35)
    ap.add_argument("--card-hold", type=float, default=4.2)
    ap.add_argument("--check", action="store_true",
                    help="verify every source image exists, build nothing")
    a = ap.parse_args(argv)

    if a.check:
        bad = [r for _, r, _c in PLATFORMS
               if not os.path.exists(os.path.join(REPO, r.replace("/", os.sep)))]
        print("%d platform(s), %d missing" % (len(PLATFORMS), len(bad)))
        for b in bad:
            print("  MISSING " + b)
        return 1 if bad else 0

    os.makedirs(OUT, exist_ok=True)
    base = os.path.join(OUT, "s11")
    clean_outputs(base)
    beats = Beats(base + ".beats.jsonl")
    try:
        build(a.hold, a.xfade, a.card_hold, base + ".mp4", beats)
    finally:
        beats.close()
    info = probe(base + ".mp4")
    st = {"scene": "s11", "platforms": len(PLATFORMS), "hold": a.hold,
          "xfade": a.xfade, "mp4": base + ".mp4", "mp4_bytes": info.get("bytes"),
          "dur": info.get("dur"), "w": info.get("w"), "h": info.get("h"),
          "fps": info.get("rate")}
    with open(base + ".stats.json", "w") as f:
        json.dump(st, f, indent=2)
    print(json.dumps(st))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
