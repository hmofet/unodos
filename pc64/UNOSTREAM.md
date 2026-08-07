# unostream - live screen streaming for demo capture

`unostream.c/.h` (guest) + `tools/demo/` (host). Owner: the unostream lane.
Status: v1 `[STABLE]` wire protocol below; guest verb surface `[STABLE]`.

## What it is

The third capture path, complementing the two in `unoauto_screen.c`:

| Path | Transport | Pacing | Bounded by |
|---|---|---|---|
| `screen grab [delta]` | URC RSP/base64, host-polled | host poll rate | link round-trips |
| `screen record` | RAM ring, pulled after the fact | device tick | 4 MB ring |
| **`stream`** (this) | **its own outbound TCP socket, binary** | device tick | host disk |

The guest dials OUT to `tools/demo/stream_recv.py`, which decodes frames and
pipes raw RGBA into ffmpeg -> `out.mp4` + a per-frame `out.timing.jsonl`.
The URC 512 B/tick TX pump is *bypassed on purpose* (a video stream would
starve the command channel); the stream rides a second netsock socket and
paces itself.

## Guest verb (URC; OBSERVE power, like `screen`)

```
stream start <ip4> <port> [fps] [scale]   dial + stream (fps 1..60 def 30;
                                          scale >=1 nearest-neighbour, def 1)
stream stop                               close the stream
stream status                             ok on=<0|1> fps=<n> sent=<frames>
                                             bytes=<n> drops=<n>
```

`drops` counts frames skipped while a previous frame was still draining,
encode overflows, and stream teardowns (stall/close/connect-fail).

## Wire protocol v1 (all little-endian)

On connect the guest sends one 16-byte hello:

| off | size | field |
|---|---|---|
| 0 | 4 | magic `"UNSM"` |
| 4 | 1 | version = 1 |
| 5 | 1 | pad = 0 |
| 6 | 1 | fps |
| 7 | 1 | scale |
| 8 | 2 | w (u16le, emitted = FB_W/scale) |
| 10 | 2 | h (u16le, emitted = FB_H/scale) |
| 12 | 4 | reserved = 0 |

Then frames, each `8-byte header + payload`:

| off | size | field |
|---|---|---|
| 0 | 1 | type: 0 = keyframe, 1 = delta |
| 1 | 1 | pad = 0 |
| 2 | 2 | reserved = 0 (u16le) |
| 4 | 4 | payload_len (u32le) |

- **keyframe** payload: one full-frame QOI image (w x h, RGBA, the
  `unoauto_screen.c` encoder's byte stream). Forced every 120 frames and after
  every hello.
- **delta** payload: `[QOI strip][manifest]`, the exact strip+manifest shape of
  `screen grab delta`: the strip is a 32 x (nch*32) QOI image of the changed
  32x32 tiles, the manifest is `nch` u16le row-major tile indices. The
  receiver derives `nch` from the strip's own QOI height (height/32), so the
  manifest is the trailing `2*nch` bytes; `payload_len == 0` is a valid
  "nothing changed" delta.
- **resolution change** mid-stream: the guest emits a fresh hello (first byte
  0x55, unambiguous against frame types 0/1) followed by a keyframe. A
  receiver treats a mid-stream hello as a stream reset (stream_recv.py closes
  the current mp4 segment and starts `<name>-2.mp4`).

The cursor (the same 9x15 arrow glyph the present path composites) is blended
into every frame at capture time - the pixels on the wire are what the console
user sees, pointer included. Nothing is written into the live `fb[]`.

## Guarantees / limits

- Never blocks the shell frame loop: at most ONE frame encode per tick;
  socket sends are non-blocking with a partial-send offset carried across
  ticks. A frame due while the previous is still draining is dropped (counted).
- Connect timeout 10 s; a stalled peer (no byte accepted for 5 s) or a remote
  close stands the stream down (counted in `drops`; `status` shows `on=0`).
- Capture pauses (stream stays up) while a security dialog is modal at the
  console - the `screen` verb's privacy rule.
- Ships in production; the GATE[] row (OBSERVE) is the privilege boundary.
- The cursor is drawn unconditionally (uefi_main.c does not export its
  `g_have_pointer`); on a box that has never seen a pointer the arrow sits at
  the default centre position.

## Host side (`tools/demo/`)

- `stream_recv.py` - stdlib-only receiver: listens, accepts ONE connection,
  parses hello + frames, maintains the RGBA canvas (QOI decode reused from
  `tools/unoauto_remote.py`), pipes raw frames to ffmpeg
  (`-f rawvideo -pixel_format rgba ... -c:v libx264 -crf 18`), writes
  `out.timing.jsonl` (one JSON line per frame: index, wall clock, bytes,
  type) and a final `out.png` canvas snapshot + `out.stats.json`.
- `stream_gate_qemu.py` - the QEMU merge gate: boots the DEBUG image like
  `tools/remote_qemu.py`, starts a receiver on a second host port, issues
  `stream start 10.0.2.2 <port> 30`, drives visible activity via URC, and
  asserts: mp4 exists with > 100 frames, >= 15 fps average arrival, deltas
  present (not all keyframes), zero receiver decode errors, and the cursor
  glyph visible at a commanded pointer position in the PNG snapshot.
