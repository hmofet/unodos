# UnoAmp: a Winamp 2.x-shaped media player for pc64

Directive, 2026-07-30: make the pc64 Player a complete clone of Winamp 2.x -
skins, plugins including visualizers, codec support rewritten as plugins, and
encoders so the app can transcode between formats.

This supersedes [MUSIC-PLAYER-PLAN.md](MUSIC-PLAYER-PLAN.md) Track B **for pc64
only**. Track A (built-in tune parity on the asm ports) and Track B for the
retro ports are untouched by this and still stand.

---

## 1. The one thing that does not work as stated, and what replaces it

**Binary Winamp plugins cannot be loaded.** A real `in_mp3.dll` is a Win32 DLL
that imports `kernel32` (heap, threads, events, files), `user32` (windows,
messages, dialogs), `gdi32` (drawing) and `winmm` (`waveOut`). pc64 has a
PE/COFF loader already - `.UNO` modules are PE, so the *loading* is solved -
but it has none of those libraries, and no window/message system to host a
plugin's config dialog. Supporting stock binaries means writing a Win32
subsystem, which is a larger project than the player and than the OS it would
sit in.

**What replaces it: source-level compatibility.** The UnoAmp plugin ABI mirrors
Winamp 2's `In_Module` / `Out_Module` / `winampVisModule` field-for-field in
order and semantics wherever the field means something here, so porting a
plugin is recompiling it against `unoamp.h` and deleting its Win32 bits. Two
deliberate substitutions:

| Winamp | UnoAmp | why |
|---|---|---|
| `HWND hMainWindow`, `hwndParent` | `void *host_window` (a `unoui_window *`) | there are no HWNDs; a plugin that wants to draw gets a canvas |
| `HINSTANCE hDllInstance` | `void *module` (the `.UNO` handle) | same idea, our loader's handle |
| `Config(HWND)`, `About(HWND)` | `Config(void *host)`, `About(void *host)` | dialogs become unoui windows |

Everything else - `Play`/`Pause`/`Stop`/`GetLength`/`SetOutputTime`, the
`SAAddPCMData`/`VSAAddPCMData` visualisation feed, `outMod` chaining, the
`"MP3\0MPEG Audio Files\0"` extension-list convention - keeps Winamp's shape.

**Skins are a different story and DO work as stated.** A `.wsz` is a ZIP of
BMPs plus small text files. pc64 already has `um_inflate` (callback-driven raw
inflate, which is what ZIP stores) and `um_bmp`. Stock skins are loadable as
data. We ship our own default artwork rather than Winamp's, the same line
Audacious and XMMS took.

---

## 2. Architecture

```
  .UNO plugin modules in \PLUGINS\
    IN_*.UNO   decoders      -> In_Module    (one per format family)
    OUT_*.UNO  sinks         -> Out_Module   (HDA, AC'97, speaker, file/WAV)
    VIS_*.UNO  visualizers   -> Vis_Module   (spectrum, scope, ...)
    DSP_*.UNO  effects       -> Dsp_Module   (EQ lives here)
    ENC_*.UNO  encoders      -> Enc_Module   (transcode targets)
                     │
  UnoAmp core ───────┴──── skin engine (.wsz) ──── unoui window(s)
```

- **Input** plugins decode to PCM and push it to the current **output** plugin,
  exactly as Winamp does, so an input plugin never knows what it is playing on.
- **Transcoding** falls out of this: point an input plugin at a file and route
  its PCM into an **encoder** plugin instead of an output plugin. Winamp needed
  a separate tool for this; here it is the same graph with a different sink.
- **Caps** on each output plugin (`UNOAMP_CAP_PCM`, `_SQUARE`, `_FILE`) let the
  core refuse a format the machine cannot play, rather than running the
  transport into silence - which is what pc64 does today, see §4.

The built-in decoders (`unomedia`'s WAV/MIDI/MP3/AAC) become the first input
plugins. They stay in-tree and statically registered; the ABI is the same, so
a dropped-in `IN_*.UNO` is not a second-class citizen.

---

## 3. Phases

Each is a landable slice with its own gate.

1. **Plugin ABI + output plugins.** `unoamp.h`; HDA, AC'97 and PC-speaker as
   `Out_Module`s behind a probe order; caps; the player refuses honestly.
   *This is also the sink-vtable spine MUSIC-PLAYER-PLAN Track B asked for.*
2. **Input plugins.** Wrap `um_audio` as `IN_UNOMEDIA`; the core dispatches by
   `IsOurFile`/extension list rather than by a hardcoded probe.
3. **Skin engine.** `.wsz` reader (ZIP + BMP), sprite-sheet atlas, the classic
   275x116 main-window layout, `viscolor.txt` / `pledit.txt` parsing. Our own
   default skin.
4. **The three windows.** Main, Equalizer, Playlist Editor, with shade modes
   and the snapping/docking behaviour.
5. **Visualizers.** The `SAAddPCMData`/`VSAAddPCMData` feed, an FFT, and
   built-in spectrum analyser + oscilloscope as `VIS_*.UNO`.
6. **DSP + EQ.** The 10-band EQ as a DSP plugin on the Winamp chain.
7. **Encoders + transcode.** `ENC_WAV` first (trivial, proves the graph), then
   the harder targets. **MP3/AAC *encoding* is a large piece of work in its own
   right** - a psychoacoustic model is not a weekend - and should be scoped
   separately rather than assumed.
8. **MOD + VGM** as input plugins (MUSIC-PLAYER-PLAN phase 4, which pc64 can
   host as software mixers).

## 4. The bug this starts from

`uno_snd_stream_begin()` does not check that a PCM device exists. On a machine
where `uno_snd_init()` found neither HDA nor AC'97, `g_ring` is NULL and
`uno_snd_active()` is 0, but the player still starts the transport, runs the
level meter and reports progress - into nothing. Phase 1 fixes that by
construction: with no `Out_Module` advertising `UNOAMP_CAP_PCM`, the core will
not offer the file at all.

## 5. Status, 2026-07-31: phases 1-8 landed

All eight phases are implemented and build. What follows is what was actually
built versus what section 3 planned, including the two places the plan was
wrong and the three limits that are real.

| Phase | File | State |
|---|---|---|
| 1 Output plugins | `pc64/unoamp_out.c` | PCM sink over `snd_pcm`, PC speaker as `CAP_SQUARE` only |
| 2 Input plugins | `pc64/unoamp_in.c` | `IN_UNOMEDIA` (WAV/MIDI/MP3/AAC); content sniff beats extension |
| 3 Skin engine | `pc64/unoamp_skin.c` | `.wsz` ZIP walk, 13 sheets, `viscolor.txt`, `pledit.txt` |
| 4 The three windows | `pc64/unoamp_ui.c` | Main + EQ + playlist, shade, drag, dock, integer scale |
| 5 Visualisers | `pc64/unoamp_vis.c` | Fixed-point FFT, spectrum + oscilloscope as `winampVisModule`s |
| 6 DSP + EQ | `pc64/unoamp_dsp.c` | Ten RBJ peaking biquads in Q20, on the Winamp DSP chain |
| 7 Encoders | `pc64/unoamp_enc.c` | WAV, AIFF, raw PCM + a transcoder over the playback graph |
| 8 MOD + VGM | `pc64/unoamp_mod.c` | ProTracker 4/6/8ch, VGM driving an SN76489 |
| - The player | `pc64/unoamp_app.c` | Playlist, transport, mixer, the per-frame pull |

### Where the plan was wrong

**Skin BMPs do not go through unomedia.** Section 3 assumed the image half was
available to the kernel. It is not: `build.sh` links the image decoders only
under `BROWSER_ENGINE=uw`, and `um_image`'s decoder roster is all-or-nothing -
taking BMP means taking PNG, JPEG, GIF, WebP and VP8 as well. `um_image` is
also a singleton the browser holds while decoding `<img>`. So `unoamp_skin.c`
reads BMP itself (about sixty lines, uncompressed only) and only `um_inflate`
is shared. That one is standalone and is exactly what ZIP method 8 needs.

**The framebuffer word is `0xAABBGGRR`, not `0xAARRGGBB`.** `FB_RGB` in `fb.h`
puts blue at bits 16-23. A BMP stores B,G,R, so BMP byte order maps straight
across with no swap - but `viscolor.txt` ("r,g,b") and `pledit.txt`
(`#RRGGBB`) both need one. Getting this wrong renders every skin with red and
blue exchanged, and it is invisible in greyscale test material.

### The three real limits

**Conversions are capped at about two minutes.** `uno_fs_write` replaces a
file entire and the fs has no append or write-at-offset, so `unoamp_enc.c`
assembles the whole output in memory and writes once. The cap is 12 MB against
a 32 MB kernel heap shared with Studio. Lifting it means adding a streaming
append to `fat.c`; that was deliberately not bodged around, because a
conversion that silently produced a truncated file is worse than one that says
it ran out of room.

**MP3 and AAC encoding are not implemented, on purpose.** A psychoacoustic
encoder is more work than every decoder in unomedia put together, and a bad
one is worse than none - it would produce files that sound wrong and be blamed
on the player. The encoder registry takes one the day it exists with no change
to `unoamp_enc.c`, which is the half of the work that was actually asked for.

**VGM covers the SN76489 only.** Most VGMs in the wild are YM2612 (Genesis FM)
or YM2151. Those are refused BY NAME at open rather than played as silence,
because silence looks like a bug in the player rather than a missing chip.

### Not yet verified on hardware

Everything above builds and is wired into `uno_pc64_init`. None of it has been
run on the ZimaBlade or any other metal. Specifically unproven: that a real
`.wsz` decodes (the ZIP walk and the BMP reader have not met a genuine skin),
that the FFT's fixed-point scaling produces sensible bar heights against real
music, that the EQ biquads are stable at the extremes of the sliders, and that
MOD playback is in tune. Those are ear-and-eye checks, not gates a QEMU run
can settle.

### Winamp fidelity: what is faithful and what is not

Faithful: the plugin ABIs (`In_Module`, `Out_Module`, `winampVisModule`,
`winampDSPModule` shapes), the 576-sample vis window, the NUL-separated
extension lists, the 275x116 sprite offsets, the input opening the output, and
`ModifySamples` being allowed to change the frame count.

Deliberately not: control is INVERTED. Winamp's input plugin ran its own
thread and pushed into `outMod`; pc64's shell is a cooperative frame loop with
no threads, so the host pulls via `Decode()`. Same graph, and it is why a slow
decode costs latency rather than stalling the desktop. Binary Win32 plugin
DLLs cannot load - that finding is in section 2 - so a plugin is a `.UNO`
built against `unoamp.h`.
