#!/usr/bin/env python3
"""game_audio_test - does a shipped GAME actually make sound?

    UNO_DEBUG=1 ./build.sh && python3 tools/game_audio_test.py

duum_audio_test.py covers the sampled-effect + score path a Python game uses
(uno.sfx_* / uno.mus_*).  Nothing covered the path the .UNO games use: the
KernelApi's gm_start / music_note_on, i.e. UnoSound's single square voice,
advanced by uno_seq_tick() in the shell loop and realised by HD Audio, AC'97
or the PC speaker.  That path had no gate at all, which is how Pac-Man
shipped at digital silence for weeks and how a conformance run could kill the
voice for the rest of a session without anything noticing.

Each game is launched over URC BY ID - not by launcher index, which drifts
every time an app is added - started with its own new-game key, and left
running while the emulated DAC is captured to a wav.  The assertion is about
the samples the DAC consumed, not about the calls that were made: a gm_start
that returns cleanly into a dead backend sounds exactly like silence.

  dostris   'n' starts a game -> Korobeiniki loops       (gm_start)
  outlast   'n' starts a game -> Sunset Drive loops      (gm_start)
  pacman    'n' starts a game -> fanfare, then the siren (gm_start), with
                                 the waka blips borrowing the voice on top
                                 (music_note_on)
"""
import os, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import remote_qemu as RQ                                    # noqa: E402
from unoauto_remote import UnoAutoLink                      # noqa: E402

BUILD = os.path.join(os.path.dirname(HERE), "build")
WAV = os.path.join(BUILD, "game_audio.wav")
GAMES = [("dostris", "n"), ("outlast", "n"), ("pacman", "n")]
HOLD = 9.0                      # seconds of play captured per game
fails = []


def check(cond, what, detail=""):
    print(("  ok   " if cond else "  FAIL ") + what + (("   " + detail) if detail else ""))
    if not cond:
        fails.append(what)


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "not a wav"
    pos, rate, chans, body = 12, 44100, 2, b""
    while pos + 8 <= len(data):
        cid, sz = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        chunk = data[pos + 8:pos + 8 + sz]
        if cid == b"fmt ":
            chans, rate = struct.unpack("<HI", chunk[2:8])
        elif cid == b"data":
            body = chunk
        pos += 8 + sz + (sz & 1)
    step = 2 * chans
    return rate, [struct.unpack_from("<h", body, i * step)[0]
                  for i in range(len(body) // step)]


def loud_runs(rate, mono, ms=100, floor=500, bridge=0.6):
    """Sustained stretches above `floor`, as (start_s, end_s, [pitch Hz]).

    Windows are BRIDGED across gaps of up to `bridge` seconds.  A score is not
    continuous sound - kKoro ends on a rest, OutLast has several - and a
    strictly contiguous run therefore chops one nine-second tune into four
    four-second fragments and reads "one game played" as "two games did not".
    Anything longer than the bridge is a real silence between stages.

    NOT bracketed by host wall-clock: QEMU's wav sink only advances while the
    DAC is running, so recording time and host time drift apart by however
    long the guest took to open the stream.  Every earlier version of this
    test read a working game as silent for exactly that reason.  What the
    stages are told apart by instead is their own shape - a game's score is a
    multi-second run carrying several distinct pitches, and the boot chime is
    the short one at the start.
    """
    n = rate * ms // 1000
    runs = []
    for w in range(len(mono) // n):
        seg = mono[w * n:(w + 1) * n]
        r = (sum(s * s for s in seg) / n) ** 0.5
        if r <= floor:
            continue
        t0, t1 = w * ms / 1000.0, (w + 1) * ms / 1000.0
        zc = sum(1 for i in range(1, len(seg)) if (seg[i - 1] < 0) != (seg[i] < 0))
        if runs and t0 - runs[-1][1] <= bridge:
            runs[-1][1] = t1
            runs[-1][2].append(int(zc * 10 / 2))
        else:
            runs.append([t0, t1, [int(zc * 10 / 2)]])
    return runs


def boot(wav):
    """RQ.boot_qemu()'s command line plus a sound device and a capture sink.

    Not a change to RQ: that file is unoautomate's harness, and every other
    gate that boots through it wants no audio device at all."""
    subprocess.run(["cp", RQ.OVMF_VARS, RQ.VARS])
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + RQ.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + RQ.VARS,
        "-drive", "format=raw,file=" + RQ.DISK,
        "-drive", "format=raw,file=" + RQ.DISK2,
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-display", "none",
        "-audiodev", "wav,id=snd0,path=" + wav,
        "-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
    ]
    if os.environ.get("URC_DBGCON"):
        cmd += ["-debugcon", "file:" + os.environ["URC_DBGCON"],
                "-global", "isa-debugcon.iobase=0x402"]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def main():
    if os.path.exists(WAV):
        os.remove(WAV)
    link = UnoAutoLink("127.0.0.1", RQ.PORT)
    link.listen()
    RQ.build_disk()
    q = boot(WAV)
    played = 0
    try:
        if not link.wait_connected(120):
            print("FAIL: the guest never dialled in - is this the DEBUG build?")
            return 1
        time.sleep(6.0)                  # let the chime finish and fall silent
        for game, startkey in GAMES:
            try:
                link.launch(game, timeout=15)
            except Exception as e:                       # noqa: BLE001
                check(False, "%s: launch" % game, str(e))
                continue
            time.sleep(1.5)
            link.key(0, ord(startkey), 0, timeout=10)    # 'n' = new game
            time.sleep(HOLD)
            played += 1
            link.close_top(timeout=10)                   # stops its score
            time.sleep(2.0)
    finally:
        try:
            link.poweroff(timeout=5)
            q.wait(timeout=20)
        except Exception:                                # noqa: BLE001
            q.kill()
            q.wait(timeout=10)
        link.close()

    if not os.path.exists(WAV):
        print("FAIL: no wav captured")
        return 1
    rate, mono = read_wav(WAV)
    runs = [r for r in loud_runs(rate, mono) if r[1] - r[0] >= HOLD * 0.5]
    print("captured %.1fs, %d sustained run(s)" % (len(mono) / rate, len(runs)))
    for a, b, pit in runs:
        print("    %6.1f - %6.1f s   %d distinct pitch bands"
              % (a, b, len(set(p // 20 for p in pit))))
    check(len(runs) >= played,
          "every game's score reached the DAC",
          "%d run(s) for %d game(s)" % (len(runs), played))
    for a, b, pit in runs:
        check(len(set(p // 20 for p in pit)) >= 3,
              "the run at %.1fs is a tune, not one held note" % a,
              "%d pitch bands" % len(set(p // 20 for p in pit)))
    print("FAILS:", fails if fails else "none")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
