# UnoDOS Music — multi-song status + the new hardware-aware Player app

Analysis + design captured 2026-06-15. Two tracks:

- **Track A** — the existing **Music** app (built-in tunes): bring multi-song
  selection to parity across all platforms. *Partially done this session;
  pattern proven.*
- **Track B** — a new, full-featured **Player** app that plays **files**
  (MIDI / MP3 / WAV-AUD / console formats), routed to the **best available
  sound hardware** on each platform and scaled down gracefully. *Designed
  here; not yet built. Build after Track A is at parity.*

There is also a deferred UI item (GUI widget arrows instead of `<`/`>` text in
the Music app) — see [Deferred](#deferred).

---

## Track A — Music app multi-song parity

The Music app plays the built-in tune library with `<` / `>` (and number-key)
song selection + on-screen title. Common library (authored as note-name +
duration, auto-encoded per platform): **Canon in D, Ode to Joy, Twinkle
Twinkle, Greensleeves, Jingle Bells, When the Saints, Mary Had a Little Lamb,
Amazing Grace** (x86 also keeps Für Elise + Brahms' Lullaby → 10).

| Platform | Sound HW | Songs | Multi-song | Verified |
|---|---|---|---|---|
| x86 PC | PC speaker | 10 | ✅ | ✅ QEMU |
| Mac System 7 | Sound Manager | 8 | ✅ | ✅ host (C core) |
| Mac System 1–6 | Sound Manager | 8 | ✅ | ✅ (C core) |
| Sony PS2 | SPU2 | 8 | ✅ | ✅ host (C core) |
| Sega Dreamcast | AICA | 8 | ✅ | ✅ (C core) |
| Sega Genesis | PSG | 8 | ✅ | ✅ BlastEm |
| **Amiga** | Paula | 1 | ❌ TODO | — |
| **MacPlus** | PWM | 1 | ❌ TODO | — |
| **Apple II** | 1-bit spkr | 1 | ❌ TODO | — |
| **SNES** | SPC700 | 1 | ❌ TODO | — |
| **Apple IIGS** | Ensoniq DOC | 1 | ❌ TODO | — |
| **C64** | SID (3 voices) | 1 (Ode to Joy on SID voice 1) | ❌ TODO | — |

### Proven pattern (from the Genesis port — replicate for the rest)

1. **Data** (generator ports — Amiga/MacPlus via `amiga/mkdata.py`, Apple II via
   `apple2/mknotes.py`): author the 8 melodies as `(note, dur)` lists; emit a
   **song table** `{notes_ptr, count, title_ptr}` + per-song note arrays. The
   generators auto-compute the platform pitch encoding (Paula period / 1-bit
   half-period). SNES authors `(MIDI−36, dur)` pairs in `apps.inc`; IIGS authors
   DOC frequency words in `snd.i`.
2. **State**: add `v_mus_song` (current index) + cache the active song's
   `(base, count, title)` in vars set by a `mus_load_song` helper.
3. **Handlers**: data-drive `music_start` / `music_tick` / `music_draw` to read
   the cached active song instead of fixed labels; add `,` / `.` (prev/next) to
   `music_key`; draw the title from the table.
4. **Build + render**: vasm (Amiga/MacPlus/Genesis), dasm (Apple II), ca65
   (SNES/IIGS — `snes-tools/bin/`). Detailed per-port file/line recipes were
   gathered and live in the session transcript; Genesis (`genesis/apps.i`,
   `genesis/mkdata.py`, `genesis/kernel.asm`) is the reference implementation.

**C64** already has an **M3 single-song SID Music app** (`c64/music.i` —
"Ode to Joy" on SID voice 1, triangle) and an **M3 3-voice SID Tracker**, so it
needs the same multi-song treatment as the others (not a from-scratch app). The
existing SID voice/envelope/gate code is a head-start for Track B's SID sink.

---

## Track B — the Player app (plays files, hardware-scaled)

### Goal

A real media player: pick a file → detect format → decode → play on the **best
sink the machine has**, degrading gracefully. Per the directive:

- **MIDI** files where the hardware can synth (FM / wavetable / multi-voice).
- **MP3** on hardware with the CPU to decode it (PS2, Dreamcast).
- **WAV / AU(D)** PCM on *every* platform (quality scales with the sink).
- **Console-native formats** on consoles (the formats real games used).

### Architecture (mirror the Uno3D backend-vtable pattern)

```
  file ──► [format probe] ──► decoder ──► PCM frames / note events ──► sink
                                                                        │
        ┌───────────────────────────────────────────────────────────┘
        ▼  audio sink vtable (per platform / device):
   { open, close, set_rate, write_pcm(buf,len), note_on/off(voice,pitch,inst),
     caps }   ← caps tell the core which decoders can run
```

- **Portable core** (`player.c` for the C-core ports; `player.asm` per asm
  port): open file, sniff magic bytes, dispatch to a decoder, pump the sink.
- **Decoders** are independent and capability-gated: a platform advertises
  `CAP_PCM`, `CAP_FM`, `CAP_WAVETABLE`, `CAP_MP3` (needs CPU), and the core only
  offers formats the sink can play.
- **Sinks** are the per-device drivers below. Most platforms have one native
  sink; x86 has several (selected by probe order).

### Per-platform sound hardware + realistic envelope

| Platform | Native sink(s) | PCM | Multi-voice synth | MP3 | Native game formats |
|---|---|---|---|---|---|
| **x86 PC** | PC speaker; **AdLib OPL2/OPL3**; **SoundBlaster** (DSP+DMA, SB-OPL); **GUS** (GF1 wavetable) | spkr PWM (poor) → SB/GUS DMA (good) | OPL FM (2-op×9 / 4-op), GUS 32 wavetable voices | ✗ (8088 too slow) | — |
| **Amiga** | Paula | 4ch 8-bit DMA (native, good) | via samples | ✗ (68000) | **MOD** (Paula's reason for being) |
| **Genesis** | PSG (SN76489) **+ YM2612 FM** | DAC ch via YM2612 (low rate) | YM2612 6-ch FM (currently unused!) | ✗ | **VGM / GYM** |
| **SNES** | SPC700 + S-DSP | 8 BRR-ADPCM voices (native) | 8 voices | ✗ | **SPC** |
| **C64** | **SID** 6581/8581 | 4-bit volume PCM trick (poor) | 3 voices + filter | ✗ | **SID / PSID** |
| **Apple IIGS** | Ensoniq 5503 DOC | 32-osc wavetable (good) | 32 oscillators | ✗ | — |
| **Apple II** | 1-bit `$C030` | PWM (poor); Mockingboard AY add-on (optional) | — / AY 3-voice | ✗ | — |
| **MacPlus** | PWM sound buffer | 1-bit PWM (poor) | — | ✗ | — |
| **Mac 7 / 1–6** | Sound Manager | PCM (good) | square synth | ✗ (68020 marginal) | AIFF |
| **PS2** | SPU2 (48 ADPCM voices) + **R5900** | native PCM | 48 voices | **✓** | VAG / ADX |
| **Dreamcast** | AICA (64 ch) + **SH-4** | native PCM | 64 voices | **✓** | ADX |

### PC sound-card support (the named hardware)

All ISA, real-mode-friendly, and **QEMU-emulatable** (`-device adlib` /
`sb16` / `gus` — confirmed present in QEMU 11), so detection + register
programming are verifiable headlessly; the sound itself is an ear-check like
the other ports.

- **AdLib (OPL2, port 388h)** — detect via the timer-overflow test (reset
  timers, read status, set timer-1, wait, read status for 0xC0). Drives **MIDI
  / FM music**: program a 2-operator instrument per channel (up to 9), key-on by
  F-number + block. OPL3 (388h+, 4-op, 18 voices) on later cards.
- **SoundBlaster (DSP at 2x0h, default 220h)** — detect by DSP reset (port
  base+6: 1, delay, 0; read base+0Eh/0Ah for 0xAA), read version (cmd 0xE1).
  Drives **WAV / PCM** via single-cycle then auto-init **DMA** (8-bit ch1 / 16-bit
  ch5 on SB16); also carries an OPL clone for FM. The bread-and-butter WAV sink.
- **Gravis Ultrasound (GF1 at 2x0h, default 240h)** — detect via GF1 reset +
  DRAM peek/poke probe. Upload samples to on-card DRAM, play **wavetable**
  voices (up to 32) — best for **MOD** and multi-sample MIDI on PC.
- **Out of envelope:** **Aureal Vortex / AC97 / ES1370** are **PCI, late-90s**
  parts needing a PCI BIOS + protected-mode drivers — incompatible with a
  real-mode 8088/8086 target. Documented as not-feasible for this OS.

Probe order on PC: SB → GUS → AdLib → PC-speaker fallback. The chosen sink sets
`caps`; e.g. AdLib-only ⇒ MIDI/FM yes, WAV no (degrade to speaker PWM or
"can't play this file" message).

### Format feasibility + scaling

| Format | Decode cost | Plays well on | Degrades to | Notes |
|---|---|---|---|---|
| **WAV / AU(D)** | trivial (PCM) | Paula, SB, GUS, SPU2, AICA, DOC, Sound Mgr | PC-speaker / 1-bit **PWM** (Apple II, MacPlus, C64, x86-no-card) — low fidelity | universal target; the floppy-friendly low-rate (≤11 kHz, 8-bit mono) profile is the baseline |
| **MIDI (SMF)** | light (parser + scheduler) | OPL/GUS (PC), DOC (IIGS), SID (C64), SPC700 (SNES), YM2612 (Genesis), Paula | monophonic melody on 1-bit speaker | needs an SMF type-0/1 parser + a per-sink instrument map |
| **MP3** | **heavy** (large codec) | **PS2, Dreamcast only** | "unsupported on this hardware" | integrate an existing fixed-point decoder (e.g. minimp3/libmad-style) into the C core; do **not** hand-roll |
| **MOD** (Amiga) | moderate (sample mixer) | Amiga (native!), GUS, SPU2/AICA | — | Paula plays it almost natively; the canonical Amiga win |
| **VGM / GYM** | light (register log player) | Genesis (PSG+YM2612), x86-OPL (partial) | — | replays chip-register writes; exact-match to the chip |
| **SPC** | light (it *is* an SPC700 image) | SNES (load into the APU) | — | SNES can run the ripped driver directly |
| **SID / PSID** | light (6502 player + SID writes) | C64 (native), others via SID emu (expensive) | — | C64 runs the player routine on its own CPU |
| **VAG / ADX** | light (ADPCM) | PS2 / Dreamcast | — | console PCM-ADPCM, near-native |

### Phased roadmap (suggested build order, after Track A parity)

1. **Core + WAV everywhere.** Player app shell (file picker via the existing
   file dialog), WAV/AU parser, the audio-sink vtable. Real PCM on the
   PCM-capable sinks; 1-bit PWM fallback on speaker platforms. Verifiable: app
   loads a `.WAV`, parses the header, drives the sink (sound = ear-check).
2. **x86 sound cards.** AdLib OPL2 detect + FM (MIDI + the built-in library
   through real FM), SoundBlaster detect + DMA WAV, GUS detect + wavetable.
   Verify detection/register paths in QEMU with `-device adlib`/`sb16`/`gus`.
3. **MIDI (SMF) + the synth sinks.** SMF parser feeding OPL/GUS/DOC/SID/SPC700/
   YM2612; monophonic fallback elsewhere.
4. **Console-native formats.** MOD (Amiga/GUS), VGM (Genesis — *also lights up
   the unused YM2612 FM chip*), SPC (SNES), SID (C64), VAG/ADX (PS2/DC).
5. **MP3 on PS2 + Dreamcast.** Integrate a fixed-point MP3 decoder into the
   shared C core; gate behind `CAP_MP3`.

### Open decisions for when you resume

- **Two apps or one?** Keep the simple **Music** app (built-in tunes, every
  platform) and add **Player** (files, hardware-scaled) as a second app — or
  fold both into one. Recommendation: keep both (Music = always-works demo,
  Player = the feature app).
- **MP3 decoder source** — confirm integrating an existing permissively-licensed
  fixed-point decoder is acceptable (hand-rolling one is out of scope).
- **GUS depth** — full wavetable/DRAM upload, or detect-and-basic-PCM first.
- **Apple II Mockingboard** — support the AY add-on, or 1-bit speaker only.

---

## Deferred

- **GUI widget arrows** in the Music app (replace the `<` / `>` text chevrons
  with clickable arrow-button widgets). The x86 Music app already has
  Prev/Play/Next **button** widgets; the C-core footer and the title chevrons
  are still text. Picked up later, per direction.
