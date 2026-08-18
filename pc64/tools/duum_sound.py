#!/usr/bin/env python3
"""duum_sound.py - host sound for duum_host.py's play mode (Windows, ctypes).

Two engines, no dependencies beyond winmm.dll:

- Music: the level's MUS lump (D_E1M1...) is parsed and sequenced live to the
  Windows MIDI mapper (Microsoft GS synth) via midiOutShortMsg.  MUS is the
  DMX format: 140Hz ticks, MUS channel 15 = percussion, controller 0 = program
  change.
- Effects: DS* lumps (DMX format 3: 8-bit unsigned PCM, usually 11025Hz) are
  mixed in a small waveOut stream (16-bit mono), up to 8 voices.  The app's
  uno.beep(note, ticks) codes map to the matching Doom sound lump via BEEPMAP.

LICENSING: everything here is read from the LOCAL WAD in memory at runtime -
nothing is extracted, converted, or written to disk, and no game asset enters
the repo (wads/ is gitignored) or any published artifact.  id's shareware /
registered WAD music stays personal-use-only on this machine; published
images bundle Freedoom, whose music and sounds are BSD-licensed.
"""
import ctypes, struct, threading, time

winmm = ctypes.WinDLL("winmm")
WAVE_MAPPER = ctypes.c_uint(-1)
WHDR_DONE = 1


class WAVEFORMATEX(ctypes.Structure):
    _fields_ = [("wFormatTag", ctypes.c_ushort),
                ("nChannels", ctypes.c_ushort),
                ("nSamplesPerSec", ctypes.c_uint),
                ("nAvgBytesPerSec", ctypes.c_uint),
                ("nBlockAlign", ctypes.c_ushort),
                ("wBitsPerSample", ctypes.c_ushort),
                ("cbSize", ctypes.c_ushort)]


class WAVEHDR(ctypes.Structure):
    _fields_ = [("lpData", ctypes.c_void_p),
                ("dwBufferLength", ctypes.c_uint),
                ("dwBytesRecorded", ctypes.c_uint),
                ("dwUser", ctypes.c_void_p),
                ("dwFlags", ctypes.c_uint),
                ("dwLoops", ctypes.c_uint),
                ("lpNext", ctypes.c_void_p),
                ("reserved", ctypes.c_void_p)]


# uno.beep(note, ticks) codes in DUUM.PY -> Doom sound lump.  (note, ticks)
# pairs first (collisions), then note alone.  None = deliberately silent.
BEEPMAP_EXACT = {
    (20, 6): b"DSBAREXP",              # barrel goes up
    (38, 4): b"DSRLAUNC",              # rocket leaves the tube
}
BEEPMAP = {
    20: b"DSPODTH1", 21: b"DSPODTH2", 22: b"DSBAREXP", 23: b"DSBGDTH1",
    24: b"DSPSTART", 25: b"DSPODTH3", 26: b"DSSGTATK", 27: b"DSBGDTH2",
    28: b"DSDOROPN", 30: b"DSPUNCH", 33: b"DSPLPAIN", 35: None,
    36: b"DSPOSIT1", 37: b"DSPOSIT2", 38: b"DSBGSIT1", 39: b"DSPOSIT3",
    40: b"DSOOF", 41: b"DSBGSIT2", 42: b"DSFIRSHT", 43: b"DSSGTSIT",
    44: b"DSPOPAIN", 45: b"DSOOF", 48: b"DSSHOTGN", 50: b"DSSWTCHN",
    52: b"DSPISTOL", 55: b"DSPISTOL", 60: b"DSSWTCHX", 66: b"DSPLASMA",
    70: b"DSTELEPT", 80: b"DSITEMUP",
}
# monster death beeps are 20 + (rnd & 7): cover the whole range
BEEPMAP.setdefault(29, b"DSSGTDTH")
# monster sight beeps are 36 + (rnd & 7)
BEEPMAP.setdefault(46, b"DSCACSIT")

RATE = 11025
CHUNK = 1024                           # ~93ms of mix per buffer
NBUF = 3
MAXVOICE = 8

# MUS controller number -> MIDI CC (0 is program change, handled apart)
MUS_CC = {1: 0, 2: 1, 3: 7, 4: 10, 5: 11, 6: 91, 7: 93, 8: 64, 9: 67}
MUS_SYS = {10: 120, 11: 123, 12: 126, 13: 127, 14: 121}


def parse_mus(d):
    """MUS lump -> list of (midi_msgs, delay_ticks) at 140Hz.  Clean-room
    from the public DMX MUS spec."""
    if len(d) < 16 or d[:4] != b"MUS\x1a":
        return None
    scorelen, scorestart, nch = struct.unpack_from("<HHH", d, 4)
    p = scorestart
    end = min(len(d), scorestart + scorelen)
    chanvol = [100] * 16
    chmap = {}                          # MUS channel -> MIDI channel
    nextch = [0]

    def midi_ch(mc):
        if mc == 15:
            return 9                    # percussion
        c = chmap.get(mc)
        if c is None:
            c = nextch[0]
            nextch[0] += 1
            if nextch[0] == 9:
                nextch[0] += 1          # skip the percussion channel
            c &= 15
            chmap[mc] = c
        return c

    out = []
    msgs = []
    while p < end:
        b = d[p]; p += 1
        last = b & 0x80
        typ = (b >> 4) & 7
        ch = midi_ch(b & 15)
        if typ == 0:                    # release
            note = d[p] & 127; p += 1
            msgs.append(0x80 | ch | (note << 8))
        elif typ == 1:                  # play
            v = d[p]; p += 1
            note = v & 127
            if v & 0x80:
                chanvol[ch] = d[p] & 127; p += 1
            msgs.append(0x90 | ch | (note << 8) | (chanvol[ch] << 16))
        elif typ == 2:                  # pitch bend, 0..255 -> 14 bit
            bend = d[p] << 6; p += 1
            msgs.append(0xE0 | ch | ((bend & 127) << 8) |
                        ((bend >> 7) << 16))
        elif typ == 3:                  # system event
            n = d[p] & 127; p += 1
            cc = MUS_SYS.get(n)
            if cc is not None:
                msgs.append(0xB0 | ch | (cc << 8))
        elif typ == 4:                  # controller
            n = d[p] & 127; val = d[p + 1] & 127; p += 2
            if n == 0:
                msgs.append(0xC0 | ch | (val << 8))
            else:
                cc = MUS_CC.get(n)
                if cc is not None:
                    msgs.append(0xB0 | ch | (cc << 8) | (val << 16))
        elif typ == 6:                  # finish
            break
        elif typ == 5:                  # end of measure
            pass
        else:                           # unused
            p += 1
        if last:
            delay = 0
            while p < end:
                c = d[p]; p += 1
                delay = (delay << 7) | (c & 127)
                if not (c & 0x80):
                    break
            out.append((msgs, delay))
            msgs = []
    if msgs:
        out.append((msgs, 0))
    return out


def decode_ds(d):
    """DMX sound lump -> (rate, centered sample list).  Format 3 header:
    u16 format, u16 rate, u32 length (incl. 16-byte pads either side in
    1.9-era lumps).  Returns None if it isn't PCM."""
    if len(d) < 12:
        return None
    fmt, rate, ln = struct.unpack_from("<HHI", d, 0)
    if fmt != 3 or rate == 0:
        return None
    body = d[8:8 + ln]
    if ln > 48:                         # strip the DMX padding when present
        body = body[16:-16]
    return rate, [(s - 128) << 6 for s in body]     # 8-bit -> ~14-bit


class Sound:
    """SFX mixer + MUS music player over winmm.  All lump data comes from
    the caller's Wad object at runtime; nothing touches the filesystem."""

    def __init__(self, wad):
        self.wad = wad
        self._sfx_cache = {}
        self._voices = []               # [samples, pos]
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._level = None
        self._mus_thread = None
        self._mus_stop = None

        # --- waveOut stream (effects).  An RDP session without "remote
        # audio playback" has ZERO wave devices - degrade to silent SFX
        # rather than fail, so the game window still runs.
        self._hwo = None
        if winmm.waveOutGetNumDevs() > 0:
            wfx = WAVEFORMATEX(1, 1, RATE, RATE * 2, 2, 16, 0)
            hwo = ctypes.c_void_p()
            rc = winmm.waveOutOpen(ctypes.byref(hwo), WAVE_MAPPER,
                                   ctypes.byref(wfx), 0, 0, 0)
            if rc == 0:
                self._hwo = hwo
                self._bufs = []
                for _ in range(NBUF):
                    data = (ctypes.c_short * CHUNK)()
                    hdr = WAVEHDR()
                    hdr.lpData = ctypes.cast(data, ctypes.c_void_p)
                    hdr.dwBufferLength = CHUNK * 2
                    hdr.dwFlags = 0
                    winmm.waveOutPrepareHeader(self._hwo, ctypes.byref(hdr),
                                               ctypes.sizeof(hdr))
                    hdr.dwFlags |= WHDR_DONE    # ready for first fill
                    self._bufs.append((data, hdr))
                self._mix_thread = threading.Thread(target=self._mix_loop,
                                                    daemon=True)
                self._mix_thread.start()

        # --- MIDI out (music), independent of the wave path ---
        self._hmo = ctypes.c_void_p()
        rc = winmm.midiOutOpen(ctypes.byref(self._hmo), WAVE_MAPPER, 0, 0, 0)
        if rc != 0:
            self._hmo = None
        if self._hwo is None and self._hmo is None:
            raise OSError("no audio devices in this session (RDP without "
                          "remote-audio playback?)")
        if self._hwo is None:
            print("duum_sound: no wave device - music only "
                  "(enable RDP remote audio playback for effects)")

    # ---- effects --------------------------------------------------------
    def _sfx(self, name):
        s = self._sfx_cache.get(name)
        if s is None and name not in self._sfx_cache:
            s = decode_ds(self.wad.lump(name))
            if s is not None and s[0] != RATE:
                rate, smp = s           # nearest-neighbour resample
                n = int(len(smp) * RATE / rate)
                smp = [smp[int(i * rate / RATE)] for i in range(n)]
                s = (RATE, smp)
            self._sfx_cache[name] = s
        return s

    def beep(self, note, ticks):
        if self._hwo is None:
            return
        name = BEEPMAP_EXACT.get((note, ticks), BEEPMAP.get(note))
        if name is None:
            return
        s = self._sfx(name)
        if s is None:
            return
        with self._lock:
            if len(self._voices) >= MAXVOICE:
                self._voices.pop(0)
            self._voices.append([s[1], 0])

    def _mix_loop(self):
        while not self._stop.is_set():
            idle = True
            for (data, hdr) in self._bufs:
                if not (hdr.dwFlags & WHDR_DONE):
                    continue
                idle = False
                with self._lock:
                    voices = self._voices
                    for i in range(CHUNK):
                        acc = 0
                        for v in voices:
                            smp, pos = v
                            if pos < len(smp):
                                acc += smp[pos]
                                v[1] = pos + 1
                        if acc > 32767: acc = 32767
                        elif acc < -32768: acc = -32768
                        data[i] = acc
                    self._voices = [v for v in voices if v[1] < len(v[0])]
                hdr.dwFlags &= ~WHDR_DONE
                winmm.waveOutWrite(self._hwo, ctypes.byref(hdr),
                                   ctypes.sizeof(hdr))
            time.sleep(0.004 if idle else 0.02)

    # ---- music ----------------------------------------------------------
    def want_music(self, level):
        """Call per frame; (re)starts D_<level> when the level changes."""
        if level == self._level:
            return
        self._level = level
        self.play_music(("D_" + level).encode())

    def play_music(self, lumpname):
        if self._hmo is None:
            return
        if self._mus_stop is not None:
            self._mus_stop.set()
            self._mus_thread.join(timeout=1.0)
        seq = parse_mus(self.wad.lump(lumpname))
        if not seq:
            return
        stop = threading.Event()
        self._mus_stop = stop

        def run():
            while not stop.is_set():
                t_next = time.monotonic()
                for (msgs, delay) in seq:
                    if stop.is_set():
                        break
                    for m in msgs:
                        winmm.midiOutShortMsg(self._hmo, m)
                    if delay:
                        t_next += delay / 140.0
                        dt = t_next - time.monotonic()
                        if dt > 0 and stop.wait(dt):
                            break
            for ch in range(16):        # silence on the way out
                winmm.midiOutShortMsg(self._hmo, 0xB0 | ch | (123 << 8))

        self._mus_thread = threading.Thread(target=run, daemon=True)
        self._mus_thread.start()

    # ---- teardown -------------------------------------------------------
    def stop(self):
        self._stop.set()
        if self._mus_stop is not None:
            self._mus_stop.set()
        time.sleep(0.05)
        try:
            if self._hwo is not None:
                winmm.waveOutReset(self._hwo)
                winmm.waveOutClose(self._hwo)
            if self._hmo is not None:
                winmm.midiOutReset(self._hmo)
                winmm.midiOutClose(self._hmo)
        except Exception:
            pass
