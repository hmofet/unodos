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
  1. per platform, one full-size card PNG - the screenshot fitted (never cropped,
     never stretched) onto black, with a caption bar burned in;
  2. those cards, each held `--hold` seconds, chained through xfade.
Splitting it keeps every intermediate inspectable, which matters when the input
is twenty-odd images of wildly different sizes.
"""
import argparse, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from demo_common import (OUT, REPO, FPS, font_path, probe, Beats,  # noqa: E402
                         clean_outputs)

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
# The name used to appear a SECOND time under the sentence, on its own card
# crossfaded in after this one (added 2026-08-08, removed 2026-08-17). It was
# the third "UnoDOS" on a card that already carries it in 96pt, and it read as
# a stutter rather than a beat. make_endcard still takes `closer`, so putting
# it back is one argument; the card's hold below absorbs the time either way,
# which is why removing it does not shorten the outro.
CARD_CLOSER = None


def esc(s):
    """drawtext's text= is parsed twice; a colon or a quote in a caption ends
    the option. None of ours carry one today, so this is a guard rather than a
    workaround - it keeps a future 'Sega Mega Drive: 32X' from silently
    truncating the burn-in."""
    return s.replace("\\", "\\\\").replace(":", "\\:").replace("'", "\\'")


# RENDER SIZE. This scene is built from stills by ffmpeg, so unlike s01 and s06
# it has no native resolution of its own - it should be authored AT the size the
# final cut is assembled at, not authored small and scaled up. The cut is
# 1280x800, so that is the default; --width/--height re-target it. Every
# dimension below is expressed against a 640x400 reference and multiplied by
# SC = height/400, so one number moves the whole layout and the captions get
# real pixels instead of an upscale.
DEF_W, DEF_H = 1280, 800


def make_card(src, caption, dst, font, w, h, index=None, total=None, crop=None):
    """One card: the screenshot fitted onto the backdrop, a caption bar and the
    platform name.

    `fit` never crops. A 256x224 console frame and a 1280x800 desktop are both
    real frames and both have to stay real, so both are letterboxed rather than
    filled. Console frames are upscaled with NEAREST - a 224-line frame blown up
    to 800 with lanczos is a smear of the pixel art it is meant to show - while
    anything already at or above the target keeps lanczos for the downscale.
    """
    sc = h / 400.0
    bar_h = int(46 * sc)
    inner_h = h - bar_h
    src_h = probe(src).get("h") or 0
    flags = "neighbor" if src_h and src_h * 1.6 < inner_h else "lanczos"
    vf = (
        ("crop=%s," % crop if crop else "") +
        "scale=%d:%d:force_original_aspect_ratio=decrease:flags=%s,"
        "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x0B0E14,"
        "pad=%d:%d:0:0:color=0x0B0E14,setsar=1,"
        "drawbox=x=0:y=%d:w=%d:h=%d:color=0x11161F@1:t=fill,"
        "drawbox=x=0:y=%d:w=%d:h=%d:color=0x3C82F6@1:t=fill,"
        "drawtext=fontfile='%s':text='%s':fontcolor=0xFFFFFF:fontsize=%d:"
        "x=%d:y=%d"
        % (w, inner_h, flags, w, inner_h, w, h,
           inner_h, w, bar_h,
           inner_h, w, max(2, int(2 * sc)),
           font, esc(caption), int(26 * sc), int(24 * sc), inner_h + int(10 * sc))
    )
    if index is not None:
        vf += (",drawtext=fontfile='%s':text='%s':fontcolor=0x8A93A6:"
               "fontsize=%d:x=w-tw-%d:y=%d"
               % (font, esc("%d / %d" % (index, total)), int(18 * sc),
                  int(24 * sc), inner_h + int(15 * sc)))
    r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", src,
                        "-vf", vf, "-frames:v", "1", dst])
    if r.returncode != 0:
        raise RuntimeError("card build failed for %s" % src)
    return dst


def make_endcard(dst, font, w, h, closer=None):
    """The closing card; with `closer`, the same card plus the final word.

    Every dimension is against the same 400-line reference the platform cards
    use, so the two cards are laid out by one number (sc) and the crossfade
    between them cannot shift anything that is on both.
    """
    sc = h / 400.0
    vf = (
        "drawtext=fontfile='%s':text='%s':fontcolor=0xFFFFFF:fontsize=%d:"
        "x=(w-tw)/2:y=%d,"
        "drawbox=x=(iw-%d)/2:y=%d:w=%d:h=%d:color=0x3C82F6@1:t=fill,"
        "drawtext=fontfile='%s':text='%s':fontcolor=0xC7CEDB:fontsize=%d:"
        "x=(w-tw)/2:y=%d,"
        "drawtext=fontfile='%s':text='%s':fontcolor=0xC7CEDB:fontsize=%d:"
        "x=(w-tw)/2:y=%d"
        % (font, esc(CARD_TITLE), int(64 * sc), int(118 * sc),
           int(160 * sc), int(196 * sc), int(160 * sc), max(2, int(2 * sc)),
           font, esc(CARD_LINE), int(21 * sc), int(228 * sc),
           font, esc(CARD_LINE2), int(21 * sc), int(258 * sc))
    )
    if closer:
        # The same font and the title's white, at a size between the title and
        # the description - so it reads as the same voice signing off, not as a
        # second heading. Centred on the same axis as everything above it, with
        # a clear gap: it is meant to be alone on that part of the screen.
        vf += (",drawtext=fontfile='%s':text='%s':fontcolor=0xFFFFFF:"
               "fontsize=%d:x=(w-tw)/2:y=%d"
               % (font, esc(closer), int(30 * sc), int(312 * sc)))
    r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error",
                        "-f", "lavfi", "-i", "color=c=0x0B0E14:s=%dx%d" % (w, h),
                        "-vf", vf, "-frames:v", "1", dst])
    if r.returncode != 0:
        raise RuntimeError("end card build failed")
    return dst


# ---- the call-to-action cards (the outro since 2026-08-20) -----------------
# The montage of twenty-two ports is out. Where a viewer can GET this is worth
# more than another look at machines they have already been shown, and the
# roster is on the website, in the manual and in the download list anyway.
#
# Two cards, both true and both checkable: the browser build is live at the
# URL below, and the download list on the same site is a raw image you write
# to a stick and boot. Nothing here is a screenshot, so nothing here can go
# stale in the way a picture of an old desktop would.
CTA = [
    ("Try it in your browser",
     "The real image, running in a page:",
     "unodos.arinbakht.com"),
    ("Or run it on your own PC",
     "Write the image to a USB stick",
     "and boot from it."),
]


def make_cta_card(dst, font, w, h, title, line1, line2):
    """One call-to-action card, laid out on the SAME 400-line reference the
    end card uses, so the three of them crossfade without anything moving."""
    sc = h / 400.0
    vf = (
        "drawtext=fontfile='%s':text='%s':fontcolor=0xFFFFFF:fontsize=%d:"
        "x=(w-tw)/2:y=%d,"
        "drawbox=x=(iw-%d)/2:y=%d:w=%d:h=%d:color=0x3C82F6@1:t=fill,"
        "drawtext=fontfile='%s':text='%s':fontcolor=0xC7CEDB:fontsize=%d:"
        "x=(w-tw)/2:y=%d,"
        "drawtext=fontfile='%s':text='%s':fontcolor=0xFFFFFF:fontsize=%d:"
        "x=(w-tw)/2:y=%d"
        % (font, esc(title), int(46 * sc), int(130 * sc),
           int(160 * sc), int(196 * sc), int(160 * sc), max(2, int(2 * sc)),
           font, esc(line1), int(21 * sc), int(228 * sc),
           font, esc(line2), int(26 * sc), int(258 * sc))
    )
    r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error",
                        "-f", "lavfi", "-i", "color=c=0x0B0E14:s=%dx%d" % (w, h),
                        "-vf", vf, "-frames:v", "1", dst])
    if r.returncode != 0:
        raise RuntimeError("cta card build failed")
    return dst


def build_cta(hold, xfade, card_hold, outfile, beats, w, h):
    """The whole outro: two call-to-action cards, then the end card."""
    font = font_path()
    work = os.path.join(OUT, "s11_cards")
    os.makedirs(work, exist_ok=True)
    cards = []
    for i, (title, l1, l2) in enumerate(CTA):
        cards.append((make_cta_card(os.path.join(work, "cta%d.png" % i),
                                    font, w, h, title, l1, l2), hold))
    cards.append((make_endcard(os.path.join(work, "zz_end.png"), font, w, h),
                  card_hold))

    # One xfade chain, same as the montage: each still becomes a clip of its
    # own hold, and the clips dissolve into each other.
    inputs, filt, prev, t = [], [], None, 0.0
    for i, (png, hold_i) in enumerate(cards):
        inputs += ["-loop", "1", "-t", "%.3f" % hold_i, "-i", png]
        lbl = "c%d" % i
        filt.append("[%d:v]fps=%d,format=yuv420p,setsar=1[%s]" % (i, FPS, lbl))
        if prev is None:
            prev, t = lbl, hold_i
            continue
        off = t - xfade
        out = "x%d" % i
        filt.append("[%s][%s]xfade=transition=fade:duration=%.3f:offset=%.3f[%s]"
                    % (prev, lbl, xfade, off, out))
        prev, t = out, t + hold_i - xfade

    # Beats: the name each card is known by, at the moment it is ALONE on
    # screen (its fade-in done), so a narration cue anchored to it lands on a
    # readable card rather than on a dissolve.
    t0 = time.time()
    at = 0.0
    for i, name in enumerate(["try-in-browser", "try-on-hardware", "end-card"]):
        beats.mark(name, t=t0 + at + xfade)
        at += cards[i][1] - xfade

    r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error"] + inputs +
                       ["-filter_complex", ";".join(filt), "-map", "[%s]" % prev,
                        "-c:v", "libx264", "-preset", "medium", "-crf", "18",
                        "-pix_fmt", "yuv420p", outfile])
    if r.returncode != 0:
        raise RuntimeError("cta outro build failed")
    return outfile


def build(hold, xfade, card_hold, card_tail, outfile, beats, w, h):
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
        make_card(src, name, dst, font, w, h,
                  index=len(cards) + 1, total=n, crop=crop)
        cards.append((name, dst))
    if missing:
        raise SystemExit("missing source screenshots: %s" % missing)
    # TWO end cards: the card, then the card plus the closing word. See
    # CARD_CLOSER - the word gets its own beat by being its own segment.
    end = make_endcard(os.path.join(work, "zz_end.png"), font, w, h)
    end2 = (make_endcard(os.path.join(work, "zz_end2.png"), font, w, h,
                         closer=CARD_CLOSER) if CARD_CLOSER else None)

    # The timeline, as (label, still, seconds-on-screen-including-its-fade).
    # This used to assume every segment lasted `hold` and derive one `step`
    # from it, which was true only while the end card was last: an xfade offset
    # is the sum of the PREVIOUS segments' lengths, so a second end card of a
    # different length would have been composited into the middle of the first.
    segs = ([("platform-" + name.lower().replace(" ", "-").replace("/", "-"),
              p, hold) for name, p in cards] +
            [("end-card", end,
              # With no closing word there is no second segment and no fade
              # into it, so the one card holds for what the pair used to take
              # and the montage keeps its length and its pacing.
              card_hold if end2 else card_hold + card_tail - xfade)] +
            ([("closing-word", end2, card_tail)] if end2 else []))

    inputs = []
    for _, p, dur in segs:
        inputs += ["-loop", "1", "-t", str(dur), "-i", p]

    fc = []
    prev = "0:v"
    off = segs[0][2] - xfade
    starts = [0.0]
    for i in range(1, len(segs)):
        lab = "x%d" % i
        fc.append("[%s][%d:v]xfade=transition=fade:duration=%s:offset=%s[%s]"
                  % (prev, i, xfade, round(off, 3), lab))
        prev = lab
        starts.append(off)
        off += segs[i][2] - xfade
    fc.append("[%s]format=yuv420p,fps=%d[v]" % (prev, FPS))
    cmd = (["ffmpeg", "-y", "-loglevel", "error"] + inputs +
           ["-filter_complex", ";".join(fc), "-map", "[v]",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "18",
            "-pix_fmt", "yuv420p", outfile])
    r = subprocess.run(cmd)
    if r.returncode != 0:
        raise RuntimeError("montage encode failed")

    # Beats are DERIVED from the timeline just built, so the sidecar and the
    # video cannot disagree: segment k begins its fade-in at starts[k].
    t0 = time.time()
    for (name, _, _), s in zip(segs, starts):
        beats.mark(name, t=t0 + s)
    return outfile


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    # 22 platforms + the end card: 22*(hold-xfade) + card_hold.
    #
    # SLOWED 2026-08-08, on the user's note that the closer went past too fast
    # to see. Each card now holds 2.55 s and the crossfade is 0.55, so the
    # sequence advances every 2.0 s and a platform is on screen ALONE for two
    # full seconds - long enough to read the caption and look at the
    # screenshot, which was the point of the montage. 22*2.0 + 4.6 = ~48.6 s.
    # The roster does not shorten to pay for it: every port earns its frame.
    ap.add_argument("--hold", type=float, default=2.55,
                    help="seconds each card is held (incl. its crossfade)")
    ap.add_argument("--xfade", type=float, default=0.55)
    ap.add_argument("--card-hold", type=float, default=4.6)
    # The closing word's own beat. It costs card_tail - xfade = 2.65 s of extra
    # runtime, which is the price of the word landing on its own instead of
    # arriving inside the same still as the sentence above it.
    ap.add_argument("--card-tail", type=float, default=3.2,
                    help="seconds the card holds AFTER the closing word "
                         "appears (incl. its crossfade)")
    ap.add_argument("--cta", action="store_true",
                    help="build the call-to-action outro (two cards + the end "
                         "card) INSTEAD of the ports montage - what the cut "
                         "has used since 2026-08-20")
    ap.add_argument("--cta-hold", type=float, default=5.0,
                    help="seconds each call-to-action card holds (incl. its "
                         "crossfade): long enough to read a URL off it")
    ap.add_argument("--width", type=int, default=DEF_W,
                    help="render width (default %d - the final cut's size)" % DEF_W)
    ap.add_argument("--height", type=int, default=DEF_H)
    ap.add_argument("--out-dir", metavar="DIR", default="out",
                    help="where the artifacts land, absolute or relative to "
                         "tools/demo (default out)")
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

    out_dir = a.out_dir if os.path.isabs(a.out_dir) \
        else os.path.join(HERE, a.out_dir)
    os.makedirs(out_dir, exist_ok=True)
    base = os.path.join(out_dir, "s11")
    clean_outputs(base)
    beats = Beats(base + ".beats.jsonl")
    try:
        if a.cta:
            build_cta(a.cta_hold, a.xfade, a.card_hold, base + ".mp4", beats,
                      a.width, a.height)
        else:
            build(a.hold, a.xfade, a.card_hold, a.card_tail, base + ".mp4",
                  beats, a.width, a.height)
    finally:
        beats.close()
    info = probe(base + ".mp4")
    st = {"scene": "s11", "cta": bool(a.cta),
          "platforms": 0 if a.cta else len(PLATFORMS), "hold": a.hold,
          "xfade": a.xfade, "card_hold": a.card_hold, "card_tail": a.card_tail,
          "closing_word": CARD_CLOSER,
          "seconds_per_platform": round(a.hold - a.xfade, 2),
          "mp4": base + ".mp4", "mp4_bytes": info.get("bytes"),
          "dur": info.get("dur"), "w": info.get("w"), "h": info.get("h"),
          "fps": info.get("rate")}
    with open(base + ".stats.json", "w") as f:
        json.dump(st, f, indent=2)
    print(json.dumps(st))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
