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
