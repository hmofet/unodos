# pc64 audio — HD Audio + AC'97 PCM (with PC-speaker fallback)

Modern PCs have no PC speaker, so the PIT-channel-2 square wave the Sound
Manager used to gate was silent on exactly the machines pc64 targets. This
layer gives the same one-voice square-wave world a real DAC:

```
Sound Manager / UnoSound  (noteCmd / quietCmd, unchanged)
        │
uno_pc64_snd_note/quiet  (uefi_main.c)
        │  PCM device up?            no ──► PIT ch2 + speaker gate (as before)
        ▼ yes
snd_pcm.c   one synthesised square voice, 48 kHz s16 stereo,
            short attack/release ramp (no clicks), Volume slider gain
        │   writes ~125 ms ahead of the hardware read position
        ▼
hdaudio.c   Intel HD Audio (PCI class 04/03)     ── probed first
ac97.c      AC'97 / Intel ICH (PCI class 04/01)  ── fallback
```

Both drivers follow the port's silicon-driver shape (ahci/xhci/e1000): find
by PCI class, static identity-mapped DMA buffers, **polled** (no interrupts,
no firmware services, no allocation), every wait iteration-bounded so a dead
device fails probe instead of hanging boot.

## The looping-ring trick

Both drivers program their DMA engine as an **endless loop over one ring**
(HDA: CBL = the whole ring, 2 BDL entries; AC'97: 32 descriptors tiling the
ring, with LVI kept one lap behind CIV by `uno_ac97_kick`). The PCM layer
just keeps writing ahead of the hardware position (HDA `LPIB`, AC'97
`CIV/PICB`), pumped from `uno_pc64_poll()` once per shell frame.

Underruns are benign *by construction*: a ring holding a periodic wave
replays seamlessly, and silence replays silence. A long blocking operation
(file load, TLS handshake) sustains the current note instead of glitching,
and the next poll rewrites the future. The startup chime pre-dates the main
loop, so `uno_pc64_chime` pumps the ring itself in 11 ms slices.

## HD Audio bring-up (hdaudio.c)

- CRST reset, codec presence from `STATESTS`, verbs over **CORB/RIRB** with
  an automatic fallback to the **Immediate Command** registers if the CORB
  never answers (the same two-tier approach as Linux's `single_cmd` mode).
- Codec walk: first Audio Function Group → best output pin
  (speaker > line-out > headphone, physically-connected only) → DFS down the
  connection lists to a DAC (depth ≤ 4, through mixers/selectors). The path
  gets D0 power, out-amp (and mixer in-amp) unmute at the 0 dB offset,
  connection selects, pin output-enable (+ HP bit), EAPD where capable.
- Stream: the first **output** stream descriptor (index = ISS count from
  GCAP), stream tag 1, format `0x0011` (48 kHz / 16-bit / stereo).
- `TCSEL` is cleared to traffic class 0 (Intel quirk; harmless elsewhere).

## AC'97 bring-up (ac97.c)

- BAR0 = mixer (NAM), BAR1 = bus master (NABM) — both I/O BARs; the driver
  also sets PCI command bit 0 (I/O decode).
- Cold-reset deassert, wait for primary-codec-ready, mixer reset, master +
  PCM-out to 0 dB unmuted. No VRA: the 48 kHz power-on default is exactly
  the PCM layer's rate.
- BDL entry lengths are in **samples**, not frames or bytes.

## The sample stream — playing files, not just notes

The square voice above is a *synthesiser*: one note at a time, generated
straight into the ring. Playing a file needs the opposite — arbitrary sample
data pushed in from outside — so `snd_pcm.c` grew a second source that takes
the ring while it is open (the square voice is muted for the duration):

```
Music app (pc64_music.c)
   │  uno_snd_stream_begin(rate, channels)
   ▼
decoder ── s16 frames at the DECODER's rate ──► uno_snd_stream_write()
                                                   │  16.16 linear resampler
                                                   ▼
                                           32768-frame FIFO (~0.68 s)
                                                   │  uno_snd_poll()
                                                   ▼
                                             the 48 kHz DMA ring
```

Two properties make this safe in a cooperative single-threaded shell:

- **The decode is bounded by buffer space, not by the file.** The app asks
  `uno_snd_stream_space()` how many input frames fit and decodes at most that
  many per tick, so a big file costs latency, never a stalled frame.
- **Underrun is silence, not a glitch.** A starved FIFO writes zeros and the
  decode catches up next tick, which is the same bargain the square voice
  already makes with its looping ring.

Files stream from disk rather than being loaded: `uno_fs_read_at` (and the
`uno_fat_read_at` / `uno_efifs_read_at` / `uno_ramfs_read_at` behind it) reads
from an arbitrary offset, so resident memory is a 64 KB sliding window in
`pc64_media.c` regardless of how long the song is.

## Decoders (moved to unomedia - the shared media foundation)

Since phase 2 of unomedia (2026-07-20) the decoders live in `../unomedia/`
behind its `um_adecoder` vtable, shared by every port; `pc64_media.c` is now
just this port's adapter (the 64 KB sliding-window source + one open call)
and the Music app drives `um_audio_*` directly. Same code, new home - the
move was a rename because the surfaces were designed to match.

| Format | File | Notes |
|---|---|---|
| **WAV** | `unomedia/um_wav.c` | RIFF chunk walk (order-tolerant); PCM 8/16/24/32, IEEE float 32/64; any rate; >2 channels folded to the front pair; `WAVE_FORMAT_EXTENSIBLE` resolved through the SubFormat GUID |
| **MIDI** | `unomedia/um_midi.c` | SMF type 0/1/2 + `.RMI`; live multi-track merge (no flattening), 16.16 tick→sample accumulation so tempo changes don't drift; 48-voice synth, GM families mapped to 2-oscillator patches with ADSR, channel 10 synthesised as drums |
| **MP3** | `unomedia/um_mp3.c` | MPEG-1 Layer III, 32/44.1/48 kHz, mono / stereo / joint (M-S + intensity) / dual. ID3v2 skip, Xing/VBRI frame counts, bit reservoir, full Huffman + requantise + reorder + alias reduction + IMDCT + polyphase filterbank |
| **AAC** | `unomedia/um_aac.c` | AAC-LC (ADTS + MP4/M4A), SCE/CPE, M/S + intensity stereo, TNS, PNS, sine + KBD windows, long/short blocks. See the licensing note below |

MP3 needs ISO constant data no decoder can invent - the Huffman codebooks, the
512-tap synthesis window, the scalefactor-band boundaries. Those live in the
generated `unomedia/mp3_tables.h`; `unomedia/tools/mkmp3tables.py` documents
where they come from (a public-domain reference) and recovers the canonical
codebooks rather than copying another program's data structure. Everything
else in `um_mp3.c` is written here.

**Not decoded:** MPEG-2 / 2.5 (the 8-24 kHz half- and quarter-rate
extensions) use different scalefactor tables and a different intensity-stereo
rule; they are detected and refused rather than turned into noise.

## AAC: decoded (the 2026-07-20 licensing reversal)

Earlier revisions of this file recorded a deliberate decision NOT to decode
AAC: the ISO constant tables no decoder can invent (the 11 spectral Huffman
codebooks, the scalefactor codebook, the scalefactor-band offsets) only exist
under real licenses - the permissive option being Apache-2.0 (OpenCORE) - and
the project declined to take on a third-party notice. **That decision has been
reversed**: the tables in `unomedia/aac_tables.h` derive from OpenCORE aacdec
(Apache-2.0), extracted by the documented `unomedia/tools/mkaactables.py`, and
the required notice now ships with every image - `DOCS\LICENSES.MD`, reachable
from **System > View licenses**. Everything AROUND the tables - the bitstream
parse, Huffman decode, TNS, M/S, intensity, PNS, the filterbank - is this
project's own code, like every other decoder here.

Patents were never the obstacle (AAC-LC claims lapsed around 2017-2018, which
is why Fedora, Debian and Wikimedia all ship decoders); the notice was, and
the notice is now simply carried. HE-AAC's SBR/PS extensions are not decoded:
a plain LC decoder legitimately plays the LC core those files contain.

**The media layer carries the reason.** `um_error()` lets a decoder that
RECOGNISES a file but cannot play it say so; the Music app shows that instead
of a generic failure, because "no decoder in this build" and "malformed" are
very different things to tell someone.

MIDI carries no audio, so its "decoder" is really a synthesiser — there is no
soundfont (a GM sample set dwarfs the 1 MB boot image), so instruments are
approximated per GM family. Seeking replays the event stream with rendering
suppressed, because programs/controllers/tempo are all stateful.

## Verifying the media chain

```
python3 tools/music_test.py both     # WAV + MIDI + MP3, end to end in QEMU
```

Boots with `-audiodev wav`, opens Music, switches to the ESP volume, plays a
staged file and asserts the captured audio: a steady tone is judged on its
median frequency, a melody on the *spread* of pitches (a stuck note would pass
a "was it loud" check). Keyboard-driven — the machine detaches under QEMU, so
the firmware AbsolutePointer is gone and only a relative PS/2 mouse survives.

The decoders also build natively for host-side unit testing; that is the fast
path when changing one, since it isolates codec bugs from OS plumbing.

## Volume

The Control Panel slider (ID_VOL) drives `uno_snd_volume(0..100)` — linear
gain on the synth amplitude, live even while a note is sounding. The System
window shows which backend is active (`Audio: HD Audio (PCM 48k s16 stereo)`
/ `AC'97` / `PC speaker (PIT ch2)`).

## Detach (M3)

`uno_snd_init` runs pre-detach, but neither driver touches firmware services
afterwards, so the ring keeps streaming across `ExitBootServices` untouched
— verified: the audio test plays the Music app *after* the detach line
appears on the debug console.

## Verification

```
python3 tools/audio_test.py both      # hda + ac97, or one of them
```

Boots the image with `-audiodev wav` capture and `-device intel-hda -device
hda-output` (or `-device AC97`), lets the chime play, opens Music through
the Start menu (post-detach) and plays ~4 s, then parses the wav and asserts
≥ 8 loud 100 ms windows with a plausible dominant frequency. Both modes PASS
(QEMU 8.2, OVMF). `harness.py boot` with no audio device covers the
PC-speaker fallback path.

## Metal-pending

- Real HDA codec bring-up (a laptop Realtek/Conexant path is deeper than
  QEMU's 2-widget codec — the DFS + amp-unmute is written for it, but only
  QEMU-verified so far). If a machine stays silent, check the System window
  first: probe failure falls back to the PC speaker line.
- AC'97 on a real ICH board (pre-2007 hardware).
- LPIB accuracy on non-Intel HDA controllers (VIA/AMD sometimes need the
  DMA position buffer instead; not implemented — the symptom would be a
  wobbling write-ahead, i.e. crackle).
