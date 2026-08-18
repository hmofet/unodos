#!/usr/bin/env python3
"""demo_common - the pieces s01 / s06 / s11 need that scenes.py does not have.

scenes.py owns s02-s10 and is being edited in another lane; this file is
DELIBERATELY NOT a refactor of it. It carries only what the three extra
scenes need, and mirrors its conventions so the outputs stitch:

  - everything lands in tools/demo/out/ as sNN.mp4 (+ .beats.jsonl,
    .timing.jsonl, .stats.json) exactly like scenes.py's record();
  - the cut is 1280x800 @ 30 fps, NATIVELY (see "One resolution" below);
  - beats are wall-clock lines in sNN.beats.jsonl.

Three capture paths live here:

  QMP screendump  - s01 only. The shell (and therefore unostream) does not
                    exist until late in boot, so a cold boot HAS to be
                    photographed from outside. screendump settles slowly and
                    is not synchronous in QEMU 8.2, so shot() polls the PPM
                    until it is complete (docs_shots.py:170-192 does the same
                    thing for the same reason).
  audiodev wav    - s06 only. The gates this borrows from (tools/audio_test.py,
                    tools/music_test.py) exist to prove sound; the DEMO
                    harnesses all pass `-audiodev none`, which is why s06 has
                    its own boot function rather than reusing remote_qemu's.
  ffmpeg stills   - s11 only. No emulator: committed screenshots.

Isolation matters: another scenes.py run may be live on 127.0.0.1:5399 with
/tmp/remote_disk.img. Nothing here touches those names or ports.
"""
import json, math, os, socket, struct, subprocess, time

HERE  = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
PC64  = os.path.dirname(TOOLS)
REPO  = os.path.dirname(PC64)
OUT   = os.path.join(HERE, "out")
PROBE = os.path.join(OUT, "probe")
ESP   = os.path.join(PC64, "build", "esp")

# ---------------------------------------------------------------------------
# ONE RESOLUTION FOR THE WHOLE VIDEO - 1280x800, and every scene reaches it
# natively rather than by being blown up at stitch time.
#
# The desktop is not the panel. uefi_main.c never calls SetMode; it takes the
# GOP mode the firmware left, and its default desktop is deliberately HALF of
# it (`apply_desktop(gModeW / 2, gModeH / 2)`, uefi_main.c:641), presented back
# up to the panel at a whole-number zoom. So OVMF's own 1280x800 gives a
# 640x400 desktop, and that - not the container, not the encoder - is why the
# first cut of s01/s06 was 640x400.
#
# The fix is therefore at the panel, not at the encoder: hand QEMU's VGA an
# EDID twice the size and the halving lands exactly on 1280x800.
#
#     -vga none -device VGA,edid=on,xres=2560,yres=1600,vgamem_mb=64
#
# Verified (2026-08-08) against a booted image: OVMF adopts the EDID's
# preferred mode, the Control Panel's Display tab reads "1280x800", and the
# desktop draws at native 1280x800. vgamem_mb has to be raised because
# 2560*1600*4 = 16.4 MB is just over the 16 MB default and the mode would not
# be offered at all.
#
# NOTHING IS UPSCALED ANYWHERE. The panel is an exact integer 2x of the
# desktop, so a 2:1 `area` downscale averages four copies of one guest pixel
# and returns that pixel. Proved rather than argued: area-halving a captured
# 2560x1600 screendump and then nearest-doubling it back is BYTE-IDENTICAL to
# the original PPM.
VID_W, VID_H, FPS = 1280, 800, 30
GOP_W, GOP_H = VID_W * 2, VID_H * 2

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"


def vga_args(gop_w=GOP_W, gop_h=GOP_H):
    """QEMU args for a panel whose half is the desktop we want to film.

    `-vga none` first, because q35 already gives you a std VGA and adding a
    second one leaves the firmware talking to the wrong card. Pass gop_w=0 to
    keep the machine's default panel (and therefore the 640x400 desktop).
    """
    if not gop_w or not gop_h:
        return []
    return ["-vga", "none",
            "-device", "VGA,edid=on,xres=%d,yres=%d,vgamem_mb=64"
                       % (gop_w, gop_h)]

# Ports and image paths that CANNOT collide with a concurrent scenes.py /
# remote_qemu.py run (5399 + /tmp/remote_*.img are theirs).
S01_QMP   = "/tmp/unodos-demo-s01-qmp.sock"
S01_DISK  = "/tmp/demo_s01_disk.img"
S01_FAT   = "/tmp/demo_s01_fat.img"
S01_VARS  = "/tmp/demo_s01_vars.fd"
S06_URC   = 5471
S06_STREAM = 5481
S06_DISK  = "/tmp/demo_s06_disk.img"
S06_FAT   = "/tmp/demo_s06_fat.img"
S06_VARS  = "/tmp/demo_s06_vars.fd"
# s12 (games, with sound) gets its OWN port pair and image paths so it can be
# recorded while s06 - or a scenes.py run - is live on the same box. Nothing
# below may collide with 5399/5471/5481 or /tmp/remote_*.img, /tmp/demo_s0*.
S12_URC   = 5473
S12_STREAM = 5483
S12_DISK  = "/tmp/demo_s12_disk.img"
S12_FAT   = "/tmp/demo_s12_fat.img"
S12_VARS  = "/tmp/demo_s12_vars.fd"

SECTOR, MIB = 512, 1 << 20

# A font that exists on THIS box. Segoe UI Bold is the Windows one; DejaVu is
# the WSL fallback so the outro still builds on a machine without /mnt/c.
# UNO_DEMO_FONT wins when set. The outro renders wherever there is an ffmpeg,
# and the box that builds it is not always the box that has Segoe: rendering it
# with the DejaVu fallback instead re-letters the WHOLE montage, which is a
# visible change to every platform caption, not just the card being edited.
# Carry the real font to the render host and name it here instead.
FONTS = [p for p in [os.environ.get("UNO_DEMO_FONT")] if p] + [
         "/mnt/c/Windows/Fonts/segoeuib.ttf",
         "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"]


def font_path():
    for f in FONTS:
        if os.path.exists(f):
            return f
    raise SystemExit("no usable font found (tried %s)" % FONTS)


def sh(argv, **kw):
    return subprocess.run(argv, **kw)


def quiet(argv, **kw):
    return subprocess.run(argv, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL, **kw)


# ---------------------------------------------------------------------------
# disk staging (our own paths - remote_qemu.build_disk hardcodes /tmp/remote_*)
# ---------------------------------------------------------------------------
def build_fat_disk(disk, fat, debug_cfg, extra=(), skip=(), ordered_dir=None,
                   label="UNODOS", mib=96, esp=None, mkdirs=()):
    """GPT + one ESP FAT32 carrying an ESP tree, the same recipe
    remote_qemu.build_disk() uses, with three knobs it does not have:

      debug_cfg    the DEBUG.CFG text to write (ours carries `nohud`, which
                   remote_qemu's does not - a red perf HUD over every frame
                   makes a debug build unfilmable, see pc64/DEBUG.md).
      extra        [(host path, "::/NAME")] copied after the tree.
      skip         ESP-relative paths (forward slashes) NOT copied.
      ordered_dir  ("PICTURES", [names]) - that directory is created and
                   filled in THIS order instead of os.walk order. FAT has no
                   sort: a directory lists in creation order, and Photos steps
                   through the listing, so the order files are written IS the
                   order the scene walks.
      esp          which ESP tree to author (default pc64/build/esp). s01 and
                   s06 both point this at their own worktree build: several
                   lanes share pc64/build/esp and rebuild it under each other
                   (it has been both a debug and a production tree inside one
                   afternoon), so a scene that needs a known build brings one.
      mkdirs       directories to create before `extra` copies into them.
    """
    esp = esp or ESP
    disk_sectors = mib * 2048
    with open(disk, "wb") as f:
        f.truncate(disk_sectors * SECTOR)
    quiet(["sgdisk", "--zap-all", disk])
    quiet(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:" + label, disk])
    part_start = 2048
    part_sectors = disk_sectors - part_start - 2048
    with open(fat, "wb") as f:
        f.truncate(part_sectors * SECTOR)
    quiet(["mformat", "-i", fat, "-F", "-T", str(part_sectors), "::"])

    skip = set(skip)
    od_name = ordered_dir[0] if ordered_dir else None
    for root, dirs, files in os.walk(esp):
        rel = os.path.relpath(root, esp).replace(os.sep, "/")
        if od_name and (rel == od_name or rel.startswith(od_name + "/")):
            continue
        if rel != ".":
            quiet(["mmd", "-i", fat, "::/" + rel])
        for fn in files:
            r = fn if rel == "." else rel + "/" + fn
            if r in skip:
                continue
            quiet(["mcopy", "-i", fat, "-o", os.path.join(root, fn), "::/" + r])
    if ordered_dir:
        name, names = ordered_dir
        quiet(["mmd", "-i", fat, "::/" + name])
        for n in names:
            src = os.path.join(esp, name, n)
            if os.path.exists(src):
                quiet(["mcopy", "-i", fat, "-o", src, "::/%s/%s" % (name, n)])
    for d in mkdirs:                           # directories `extra` copies into
        quiet(["mmd", "-i", fat, "::/" + d])
    for src, dst in extra:
        if os.path.exists(src):
            quiet(["mcopy", "-i", fat, "-o", src, dst])

    cfgp = fat + ".cfg"
    with open(cfgp, "w", newline="\r\n") as f:
        f.write(debug_cfg)
    quiet(["mcopy", "-i", fat, "-o", cfgp, "::/DEBUG.CFG"])

    with open(fat, "rb") as pf, open(disk, "r+b") as df:
        df.seek(part_start * SECTOR)
        while True:
            b = pf.read(MIB)
            if not b:
                break
            df.write(b)
    return disk


# ---------------------------------------------------------------------------
# QMP (self-contained: docs_shots.py os.chdir()s at import, harness.py wants a
# build tree - neither is importable from here without side effects)
# ---------------------------------------------------------------------------
class Qmp(object):
    def __init__(self, path, timeout=60):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.buf = b""
        self.recv()
        self.cmd("qmp_capabilities")

    def recv(self):
        while b"\n" not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError("QMP closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.s.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            r = self.recv()
            if "return" in r or "error" in r:
                return r

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def ppm_read(path):
    """(w, h, pixels) of a COMPLETE binary PPM, or None if it is short.

    QEMU 8.2's screendump is not synchronous over QMP: the command returns and
    the file is filled in afterwards, so a naive read gets a header with half a
    frame behind it. Every caller polls this until it stops returning None -
    which is exactly what docs_shots.py:170-192 does, and why.
    """
    try:
        with open(path, "rb") as f:
            if f.readline().strip() != b"P6":
                return None
            line = f.readline()
            while line.startswith(b"#"):
                line = f.readline()
            w, h = (int(t) for t in line.split())
            f.readline()                       # maxval
            px = f.read()
        if len(px) < w * h * 3:
            return None
        return w, h, px
    except Exception:                          # noqa: BLE001 - mid-write file
        return None


def send_key(q, *qcodes):
    """One chord over QMP: send-key with every qcode pressed together.

    This is the ONLY input channel a production build has. URC needs a token
    typed at the console when UNO_DEBUG=0 (see pc64/docs_shots.py's header),
    which is exactly why the manual's figures are driven this way too - and
    unlike URC's `key` verb, a QMP chord CAN carry Alt.
    """
    q.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes])


def screendump(q, path, tries=12, gap=0.06):
    """One settled screendump. Returns (w, h, px, t) or None.

    `t` is stamped when the frame was seen COMPLETE, which is the only wall
    clock we can honestly attach to it.
    """
    try:
        q.cmd("screendump", filename=path)
    except Exception:                          # noqa: BLE001
        return None
    for _ in range(tries):
        r = ppm_read(path)
        if r:
            return r[0], r[1], r[2], time.time()
        time.sleep(gap)
    return None


# ---------------------------------------------------------------------------
# frame maths (all post-hoc, on the real captured frames)
# ---------------------------------------------------------------------------
def frame_stats(px, w, h, step=7):
    """(mean luma, and a coarse signature) of a PPM frame, subsampled."""
    n = 0
    tot = 0
    sig = []
    for y in range(0, h, step):
        ro = y * w * 3
        row = 0
        for x in range(0, w, step):
            o = ro + x * 3
            v = px[o] + px[o + 1] + px[o + 2]
            tot += v
            row += v
            n += 1
        sig.append(row)
    return (tot / float(n * 3) if n else 0.0), sig


def sig_diff(a, b):
    if a is None or b is None or len(a) != len(b):
        return 1e9
    tot = sum(abs(x - y) for x, y in zip(a, b))
    base = sum(abs(x) for x in a) + 1
    return tot / float(base)


# ---------------------------------------------------------------------------
# ffmpeg
# ---------------------------------------------------------------------------
# `area`, not lanczos. The only real downscale this does is the exact 2:1 from
# the 2560x1600 panel back to the 1280x800 desktop, and at exactly 2:1 area
# averages the 2x2 block - which, the panel being a nearest 2x of the desktop,
# is four copies of one guest pixel. That makes the step lossless (proved by
# round-trip: area-half then nearest-double == the original PPM, byte for
# byte). lanczos would ring on the same pixels for no gain.
FIT = ("scale=%d:%d:force_original_aspect_ratio=decrease:flags=area,"
       "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1"
       % (VID_W, VID_H, VID_W, VID_H))


def encode_concat(listfile, out, fps=FPS, extra_vf=""):
    """Encode a concat-demuxer list (per-entry `duration` = REAL inter-frame
    time) into a CFR mp4. The timing in the file is the timing that happened;
    -r only resamples it into a 30 fps container so it stitches with s02-s10.
    """
    vf = FIT + ("," + extra_vf if extra_vf else "")
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
           "-i", listfile, "-vf", vf, "-r", str(fps), "-fps_mode", "cfr",
           "-c:v", "libx264", "-preset", "veryfast", "-crf", "18",
           "-pix_fmt", "yuv420p", out]
    r = subprocess.run(cmd)
    if r.returncode != 0:
        raise RuntimeError("ffmpeg failed on %s" % listfile)
    return out


def probe(path):
    r = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries",
         "stream=width,height,nb_frames,avg_frame_rate",
         "-show_entries", "format=duration,size", "-of", "json", path],
        stdout=subprocess.PIPE)
    try:
        j = json.loads(r.stdout.decode())
    except Exception:                          # noqa: BLE001
        return {}
    st = (j.get("streams") or [{}])[0]
    fm = j.get("format") or {}
    return {"w": st.get("width"), "h": st.get("height"),
            "nb_frames": st.get("nb_frames"), "rate": st.get("avg_frame_rate"),
            "dur": float(fm.get("duration", 0) or 0),
            "bytes": int(fm.get("size", 0) or 0)}


# ---------------------------------------------------------------------------
# wav measurement (s06's proof that the guest really made a noise)
# ---------------------------------------------------------------------------
def wav_read(path):
    """(rate, channels, bits, samples-as-int16-list-of-frames) for a PCM wav."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise RuntimeError("%s is not a RIFF/WAVE file" % path)
    pos, rate, chans, bits, body = 12, 44100, 2, 16, b""
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        sz = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        blob = data[pos + 8:pos + 8 + sz]
        if cid == b"fmt ":
            chans, rate = struct.unpack("<HI", blob[2:8])
            bits = struct.unpack("<H", blob[14:16])[0]
        elif cid == b"data":
            # QEMU's wav backend patches the size in on clean exit; if it was
            # killed the header says 0 and the real payload is the tail.
            body = blob if sz else data[pos + 8:]
        pos += 8 + sz + (sz & 1)
        if sz == 0 and cid == b"data":
            break
    return rate, chans, bits, body


def wav_measure(path, win_ms=50):
    """Duration / rate / peak / RMS, plus the per-window RMS envelope.

    Returned in dBFS as well as raw, because "the wav exists" and "the wav has
    audio in it" are different claims and only the second one is the check.
    """
    rate, chans, bits, body = wav_read(path)
    assert bits == 16, "expected s16 wav, got %d-bit" % bits
    nf = len(body) // (2 * chans)
    peak = 0
    sq = 0
    win = max(1, int(rate * win_ms / 1000.0))
    env = []
    wsq = 0
    wn = 0
    for i in range(nf):
        v = struct.unpack_from("<h", body, i * 2 * chans)[0]
        a = -v if v < 0 else v
        if a > peak:
            peak = a
        sq += v * v
        wsq += v * v
        wn += 1
        if wn == win:
            env.append((wsq / float(wn)) ** 0.5)
            wsq = 0
            wn = 0
    if wn:
        env.append((wsq / float(wn)) ** 0.5)
    rms = (sq / float(nf)) ** 0.5 if nf else 0.0

    def db(x):
        return -999.0 if x <= 0 else round(20.0 * math.log10(x / 32768.0), 2)

    return {"path": path, "rate": rate, "channels": chans, "bits": bits,
            "frames": nf, "seconds": round(nf / float(rate), 2) if rate else 0,
            "peak": peak, "peak_dbfs": db(peak),
            "rms": round(rms, 1), "rms_dbfs": db(rms),
            "win_ms": win_ms, "env": env}


def volumedetect(path, ss=None, t=None):
    """ffmpeg's own `volumedetect` over a wav, optionally over one slice.

    wav_measure() above already computes peak and RMS from the samples, so this
    is not new information - it is INDEPENDENT information, from a tool nobody
    here wrote, which is the point when the claim being made is "the guest
    really made a noise". Reported per game window as well as whole-file:
    a scene can be loud in aggregate and silent where it matters.

    Returns {mean_db, max_db, n_samples, seconds} - mean_db None if ffmpeg
    printed nothing (an empty slice), which the caller must treat as silence
    rather than as success.
    """
    cmd = ["ffmpeg", "-v", "info", "-hide_banner"]
    if ss is not None:
        cmd += ["-ss", "%.3f" % max(0.0, ss)]
    if t is not None:
        cmd += ["-t", "%.3f" % max(0.0, t)]
    cmd += ["-i", path, "-af", "volumedetect", "-f", "null", "-"]
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    out = {"mean_db": None, "max_db": None, "n_samples": None,
           "start_s": round(ss, 2) if ss is not None else 0.0,
           "seconds": round(t, 2) if t is not None else None}
    for line in r.stderr.decode("utf-8", "replace").splitlines():
        if "mean_volume:" in line:
            out["mean_db"] = float(line.split("mean_volume:")[1].split("dB")[0])
        elif "max_volume:" in line:
            out["max_db"] = float(line.split("max_volume:")[1].split("dB")[0])
        elif "n_samples:" in line:
            try:
                out["n_samples"] = int(line.split("n_samples:")[1].split()[0])
            except (ValueError, IndexError):
                pass
    return out


def sustained_onset(env, win_ms, thresh, hold_ms=400):
    """Start time (s) of the first run of `hold_ms` of windows above `thresh`.

    Used to find where MUSIC starts in the wav, which is what pins the wav
    clock to the video clock: the two are independent (QEMU's wav sink starts
    with the machine; the stream starts when the guest dials the receiver), so
    the offset has to be MEASURED against something audible.
    """
    need = max(1, int(hold_ms / float(win_ms)))
    run = 0
    for i, v in enumerate(env):
        if v >= thresh:
            run += 1
            if run >= need:
                return (i - run + 1) * win_ms / 1000.0
        else:
            run = 0
    return None


# ---------------------------------------------------------------------------
# beats
# ---------------------------------------------------------------------------
class Beats(object):
    """scenes.py's beat log, standalone (its version is a Demo method)."""

    def __init__(self, path, verbose=True):
        self.f = open(path, "w")
        self.verbose = verbose
        self.marks = []

    def mark(self, name, t=None):
        t = time.time() if t is None else t
        self.f.write(json.dumps({"beat": name, "t": t}) + "\n")
        self.f.flush()
        self.marks.append((name, t))
        if self.verbose:
            print("  beat: %s" % name, flush=True)
        return t

    def close(self):
        self.f.close()


def clean_outputs(base, exts=(".mp4", ".timing.jsonl", ".beats.jsonl", ".png",
                             ".stats.json", ".wav")):
    for e in exts:
        try:
            os.unlink(base + e)
        except OSError:
            pass
