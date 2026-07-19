#!/usr/bin/env python3
"""UnoDOS/pc64 audio verification - QEMU + OVMF, fully headless.

Boots the unoui shell with a wav-capture audiodev and a sound device, lets
the startup chime play (pre-detach), then opens the Music app through the
Start menu and plays the song (post-detach - the DMA ring must survive
ExitBootServices). Finally parses the captured wav and asserts real tone
content: enough loud 100 ms windows, with a plausible dominant frequency.

  python3 tools/audio_test.py hda      -device intel-hda -device hda-output
  python3 tools/audio_test.py ac97     -device AC97
  python3 tools/audio_test.py both     the two above, in sequence
"""
import os, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
sys.path.insert(0, HERE)
from harness import Qmp, keys, QMP_SOCK, OVMF_CODE, OVMF_VARS


def boot_and_play(mode):
    wav = "build/audio_%s.wav" % mode
    if os.path.exists(wav):
        os.remove(wav)
    subprocess.run(["cp", OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", "format=vvfat,file=fat:rw:build/esp",
        "-device", "qemu-xhci", "-device", "usb-tablet",
        "-nic", "none",
        "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-audiodev", "wav,id=snd0,path=" + wav,
        "-debugcon", "file:build/audio_dbg.log",
        "-global", "isa-debugcon.iobase=0x402",
    ]
    if mode == "hda":
        argv += ["-device", "intel-hda", "-device", "hda-output,audiodev=snd0"]
    else:
        argv += ["-device", "AC97,audiodev=snd0"]
    qemu = subprocess.Popen(argv)
    try:
        q = Qmp(QMP_SOCK)
        print("[%s] qemu up; waiting for OVMF -> UnoDOS boot (chime)..." % mode)
        time.sleep(18)
        # Music is a .UNO bridge app: launcher index 10 (7 native + 3).
        q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"},
                                {"type": "qcode", "data": "esc"}])
        time.sleep(0.8)
        keys(q, *(["down"] * 10))
        keys(q, "ret")
        time.sleep(1.5)
        keys(q, "spc")                 # play - post-detach DMA streaming
        time.sleep(4.0)
        keys(q, "spc")                 # stop
        time.sleep(0.5)
    finally:
        try:
            q.cmd("quit")              # clean exit finalises the wav header
        except Exception:
            qemu.kill()
        qemu.wait(timeout=10)
    return wav


def analyze(wav):
    """Return (loud_windows, seconds, dom_hz of the loudest window)."""
    with open(wav, "rb") as f:
        data = f.read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "not a wav"
    pos, rate, chans, samples = 12, 44100, 2, b""
    while pos + 8 <= len(data):
        cid, sz = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + sz]
        if cid == b"fmt ":
            chans, rate = struct.unpack("<HI", body[2:8])
        elif cid == b"data":
            samples = body
        pos += 8 + sz + (sz & 1)
    n = len(samples) // 2
    mono = [struct.unpack_from("<h", samples, i * 2 * chans)[0]
            for i in range(n // chans)]
    win = rate // 10                              # 100 ms windows
    loud, best, best_rms = 0, None, 0
    for w in range(len(mono) // win):
        seg = mono[w * win:(w + 1) * win]
        rms = (sum(s * s for s in seg) / win) ** 0.5
        if rms > 500:
            loud += 1
            if rms > best_rms:
                best_rms, best = rms, seg
    dom = 0
    if best:
        zc = sum(1 for i in range(1, len(best))
                 if (best[i - 1] < 0) != (best[i] < 0))
        dom = zc * 10 / 2                          # crossings/100ms -> Hz
    return loud, len(mono) / rate, dom


def run(mode):
    wav = boot_and_play(mode)
    loud, secs, dom = analyze(wav)
    ok = loud >= 8 and 100 <= dom <= 2500
    print("[%s] %s: %.1fs captured, %d loud 100ms windows, dominant ~%d Hz"
          % (mode, "PASS" if ok else "FAIL", secs, loud, dom))
    return ok


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "both"
    modes = ["hda", "ac97"] if mode == "both" else [mode]
    if not all([run(m) for m in modes]):
        sys.exit(1)
