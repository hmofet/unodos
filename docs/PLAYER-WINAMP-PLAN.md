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
