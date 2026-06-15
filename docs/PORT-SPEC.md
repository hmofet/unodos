# UnoDOS Port Specification

**Purpose:** the platform-independent contract a UnoDOS port must honor.
Extracted from the x86 reference implementation (v3.26.0, Build 405) per
docs/M68K-PORT-FEASIBILITY.md Phase 0. Where this file and the x86 source
disagree, the source wins — report the discrepancy and update this file.

Companion references: `docs/API_REFERENCE.md` (per-call semantics),
`docs/ARCHITECTURE.md`, `docs/MEMORY_LAYOUT.md` (x86-specific),
`docs/AUDIT-HANDOFF-2026-06.md` + `docs/audit-2026-06-digest.md`
(invariants and their rationale).

---

## 1. Identity & UX contract

- GUI-first: the machine boots directly into a windowed desktop. No shell,
  no DOS layer.
- Default visual: 320×200, 4 colors — palette index 0 = blue desktop
  (#0000AA), 1 = cyan (#00AAAA), 2 = magenta (#AA00AA), 3 = white
  (#FFFFFF). Ports with palettes match these RGB values; monochrome ports
  re-theme (white-on-black + dither) but keep metrics.
- **Palette extension (2026-06-11):** indices 0–3 remain the themed UI
  colors (see Theming below); ports may extend the palette to their
  platform maximum for app/game content (x86 VGA: DAC via API 105;
  Amiga: 5 bitplanes / 32 colors, entries 4–31 fixed game palette,
  17–19 shared with the cursor sprite; Mac color: direct RGB). UI
  chrome must keep using the themed indices so theme presets restyle
  every port identically. Caution: transparent text over a non-zero
  backdrop ORs color bits — use opaque text on extended-color surfaces.
- **Theming:** every color-capable port ships the same 8 preset
  palettes (Classic VGA, Midnight, Forest, Sunset, Ocean, Slate, Candy,
  Amber) for UI colors 0–3, plus per-channel custom editing where the
  hardware allows (Theme app on 68K, Settings on x86).
- **Splash:** boot shows a platform-themed "UnoDOS 3" splash (~2 s
  minimum hold) with platform-identity artwork before the desktop.
- Desktop: menu-bar title row (y 0–11 reserved — drag clamp protects it),
  icon grid (80 px column pitch, icons at col·80+32, labels at icon_x−8,
  label width clipped to the cell), version string bottom-left, build
  bottom-right.
- Default font: 8×8, **advance = 8** (the 12-px advance was a bug; see
  audit). Small font 4×6 (advance = width+2). Title bars render text
  inverted. Fonts are data — export the glyph tables byte-identical.
- Mouse cursor: 8×14 arrow, drawn above everything (hardware sprite where
  available; XOR/save-under otherwise).

## 2. Window manager

- Fixed table of **16 windows**, 32-byte entries: state (FREE/VISIBLE),
  owner task, x, y, width, height, flags (TITLE=bit0, BORDER=bit1,
  FRAMELESS_FULLSCREEN=bit2), z-order, title (truncated to fit), content
  scale.
- **Z-order is 0–15, topmost = 15.** Invariants (audit-paid, mandatory):
  - Raising a window demotes only windows with z **above** its old z by
    exactly 1; the raised window takes z=15. Never demote below-z windows
    (this leaked z-levels until everything collided at 0).
  - Destroy renormalizes survivors and promotes a new topmost; focus
    follows topmost; `focused_task` = owner of topmost window or NONE.
  - A topmost-window bounds cache backs the draw clipper.
- Clipping model: only the topmost window draws its content; non-topmost
  windows' draw calls are discarded (z-clip) and their content repaints on
  promotion via a redraw event. Frames of all windows are always drawn.
- Title-bar drag: XOR outline during drag, move applied on release.
  Clamp: window's right edge stays on-screen (close button reachable),
  title bar never leaves the screen, never covers desktop rows y<12.
- Close button: rightmost 12 px of the title bar. Kills the owning task:
  free its slot/memory, destroy its windows, **close its file handles**,
  silence audio.
- Click-to-raise: a press on the title bar OR body of a non-topmost
  window raises it (z-aware hit test — topmost match wins, not first
  match). Pressing the topmost window's body is a no-op for the WM.

## 3. Events & input

- Single global queue, **32 entries × 3 bytes**: type byte + 16-bit data.
  Forward-scan delivery with tombstones (type 0xFF) so one task's
  undelivered event never head-blocks others; head advances lazily over
  tombstones.
- Types: 1 = KEY_PRESS (data low = ASCII; data high = focus stamp, see
  below), 4 = MOUSE (data low = button mask), 6 = WIN_REDRAW (data low =
  window handle), 0xFF = consumed.
- **Key routing**: the keyboard ISR stamps the *focused task at press
  time* into the event. Delivery: stamp==consumer's task → deliver;
  stamp's task no longer focused → discard (stale); stamp == NONE and
  still nothing focused → deliver to any poller (desktop/launcher path).
- **Mouse**: ISR updates position (clamped to screen) and, **only on a
  button-state change**, posts a MOUSE event (motion is pollable state,
  never events). Mouse events are focus-routed like keys. The ISR also
  latches *press-time* X/Y, a press sequence counter, and the rising-edge
  button mask; the mouse-state API returns live X/Y/buttons **plus** the
  latch. Consumers hit-test the latch, not live position. A consumer
  seeing the sequence counter advance by ≥2 in one poll missed a press
  (fast double-click) and must process both.
- **No drawing or device I/O in interrupt context.** ISRs set dirty
  flags; cursor redraw and drag processing happen at poll points
  (event_get / mouse_get_state equivalents). The deferred cursor sync
  must NOT consume its dirty flag while the cursor lock is held.
- Cursor protection: hide-cursor + lock-increment is one atomic
  operation (interrupts masked across the pair). Unlock (decrement +
  show) needs no masking.
- Ticks: monotonic tick counter **relative to boot** (latch the platform
  counter at kernel entry). x86 rate is 18.2 Hz; ports may differ (50/60
  Hz vblank) but must expose the rate to apps or normalize.

## 4. Tasking & app model

- Cooperative scheduler, round-robin over a 16-slot app table (x86 uses
  5 loadable user slots + 1 fixed shell/launcher; ports size this to
  their memory). Context switch only at yield/event-wait.
- Per-task state restored on switch: draw context (window handle or
  NONE), clip rect derived from it, caller segment registers (x86) /
  base pointers (ports), font.
- App loading: validate image size against the slot (reject 0-byte and
  too-large), verify actual bytes read == file size, zero/initialize the
  entry frame, reap file handles and windows on every kill path.
- **.BIN header** (port-independent container):
  - 0x00: 0xEB ·· 'U' 'I' (magic; first byte doubles as x86 jmp)
  - 0x04: 12-byte NUL-padded display name
  - 0x10: 64-byte icon, 16×16 @ 2bpp packed chunky, row-major,
    leftmost pixel in the two MSBs of each byte
  - 0x50: code entry
  68K ports keep the header (little-endian fields stay little-endian for
  disk interchange) and place position-independent 68K code at 0x50.
- Syscall ABI: x86 = INT 0x80, function number in AH, args AL/BX/CX/DX/
  SI/DI, CF = error + AX = code. 68K = TRAP #0, function in D0 high byte
  (or D7 — fix at port time and document), args D1–D4/A0–A1, error in
  CCR carry + D0. 105 functions; semantics per API_REFERENCE.md.

## 5. Filesystem

- FAT12, 512-byte sectors, 1 sector/cluster on the boot floppy.
- x86 1.44 MB floppy layout: boot LBA 0 · stage2 1–4 · kernel 5–108 ·
  BPB reserved sectors = 110 · FAT at 111 · root dir at 129 (224
  entries) · data at 143. These constants exist in FIVE places on x86 —
  ports must define them **once**.
- All multi-byte on-disk fields are **little-endian**; big-endian ports
  use byte-order accessors at the FS boundary only.
- 16 file handles, 32-byte entries; byte 24 = owner task (kernel = 0xFF);
  owner-based reaping on task death is mandatory.
- Read path batches physically-consecutive clusters into multi-sector
  device reads (huge win on real floppies); bounce-buffer only the
  partial tail.

## 6. Design rules (the audit tax — do not relearn these)

1. Atomic cursor hide+lock (cursor_protect_begin pattern).
2. ISRs set flags; task context draws. Deferred sync keeps its dirty
   flag when the cursor lock blocks it.
3. Focus stamped at event-source time; stale events discarded at
   delivery.
4. Press-time coordinate latch + sequence counter; never hit-test live
   mouse position for clicks; handle seq jumps of ≥2.
5. Edge-only mouse event posting.
6. Z renormalization on focus AND destroy; one source of truth for
   topmost.
7. Validate loaded images (size, short reads); reap handles + windows on
   every kill path (there are always more kill paths than you think:
   self-exit, close-button, load-time eviction).
8. Centralize disk geometry; read sector runs, not single sectors.
9. Keep the API table address/IDs stable; additions append.
10. Every queue/table is fixed-size — design the "full" behavior
    explicitly (drop policy, error code) rather than discovering it.


---

## Implementations

| Implementation | Location | Notes |
|---|---|---|
| x86 (reference) | `kernel/`, `apps/`, `boot/` | v3.26.0+, 8086-clean |
| Amiga 68000 | `amiga/` | bare-metal; milestone 3 |
| Mac System 7 (color) | `mac/` (`UnoDOS7`) | Toolbox-based; milestone 2 |
| Mac System 1–6 (mono) | `mac/` (`UnoDOSClassic`) | Toolbox-based; milestone 2 |
| Sega Genesis 68000 | `genesis/` | bare-metal, VDP cell desktop; milestone 1 |
| Sony PS2 (R5900) | `ps2/` | portable C core over a software FB → GS; milestone 2 |
| Sega Dreamcast (SH-4) | `dreamcast/` | portable C core over a software FB → DC framebuffer (KallistiOS); milestone 1, host-verified |
| Apple IIGS (65C816) | `iigs/` | bare-metal Super Hi-Res; full app parity (M0–M3), ROM-free Python-65816 harness |

Deviations to reconcile in later milestones: the Mac and Genesis ports
use a single cooperative event loop (the Amiga has the milestone-3
scheduler), the Genesis has no storage yet (SRAM saves planned; its
Notepad F1-save is a no-op) and quantizes windows to 8 px cells, and
the 68K Notepads cap their edit buffers (2–4 KB) below the x86 app's.
