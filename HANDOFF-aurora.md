# Aurora rollout — handoff / continuation prompt

Continue bringing the **unoui Aurora theme** to the UnoDOS ports. Repo:
`github.com/hmofet/unodos`, branch **`pc64-usb-flasher`**. **Read
[`PLAN-aurora-rollout.md`](PLAN-aurora-rollout.md) first** — it's the
authoritative plan (per-port tiers, architecture, phases). This doc is the
current state + how to execute on a machine with enough RAM to build the heavy
cross-toolchains that were blocked before.

**Rules:** install toolchains as needed; **verify each port by rendering in its
emulator (screendump), not just building**; commit per port; push to
`pc64-usb-flasher`.

## Already done — do NOT redo (see `git log`)

- **Phase 0** (`7bcd7ec`): the compositing foundation.
  - `UNO_BG_CACHE` capability (was a pc64 hardcode) in `unoui/unoui.c` — the
    cached desktop background is now opt-in per port.
  - `unoui_aurora_lite` flag in `unoui/themes/theme_aurora.c`: `0` = full (soft
    drop shadow + AA rounded corners); `1` = lite/static (**zero per-frame
    alpha** — `soft_shadow` skipped, corners squared).
  - "Aurora lite" checkbox in the pc64 Control panel. Render-verified in QEMU.
- **Phase 1 ps2** (`9679da6`): `./build.sh ee uui` builds unoui + Aurora on the
  EE. Build+link verified (`unodos-ps2-uui.elf`, MIPS N32). Rebuilt + PCSX2
  **execution**-verified (the EE ELF loads + runs the render loop; emulog shows
  entry `0x00101570` executing, NTSC vsync, no EE exceptions). A live GS
  **screendump** is still pending: PCSX2 v2.x presents every renderer (incl.
  software) through a GPU swapchain, and this box's RDP session is currently
  **disconnected**, so `CopyFromScreen` returns "handle invalid" and PrintWindow
  can't read a GPU surface. `ps2/tools/run_pcsx2_windowed.ps1` + the existing
  `run_pcsx2.ps1` will both capture once RDP is **connected** (or the installed-
  but-inactive virtual display driver is enabled). The Aurora *content* is
  render-verified via the identical host software path (see below).
- **Phase 1 dreamcast** (this branch): `./build.sh dc uui` builds the SH4 KOS
  ELF `unodos-dc-uui.elf` + a bootable image (`.iso`/`.cdi`). Files:
  `dreamcast/fb_aa.c` (portable ops), `dreamcast/fb.h` (unoui decls),
  `dreamcast/dc_uui.c` (KOS shell: 640×480 RGB565 present, maple d-pad nav).
  **KallistiOS is now fully built** (sh-elf gcc 15.2.0 at `/opt/toolchains/dc/sh-elf`
  + `libkallisti.a`; `source /opt/toolchains/dc/kos/environ.sh` before `dc uui`).
  Aurora **content** render-verified via `./build.sh uui-host` →
  `dreamcast/shots/aurora.png` (the SAME unoui+theme_aurora+fb_aa software path
  the DC runs; only the present differs, fb→565). Live emulator screendump
  pending the same capture constraint as ps2 (no DC emulator installed; all are
  GPU-present). The DC 565 present mirrors the already-verified legacy
  `dc_main.c` path.

## The reusable per-port pattern (established by ps2)

1. The port drives a chunky ARGB/XRGB `fb.c`, but its `fb.c` usually lacks the
   alpha/gradient/rounded primitives → add `<port>/fb_aa.c` (copy
   `ps2/fb_aa.c` — portable software ops) and declare in the port's `fb.h`:
   `FB_CORNER_*`, `FB_BUF_PIX`, and `fb_pixel` / `fb_blend_pixel` /
   `fb_blend_rect` / `fb_grad_v` / `fb_round_rect_a` / `fb_set_clip` /
   `fb_reset_clip`.
2. Write `<port>_uui.c` (copy `ps2/ee_uui.c`):
   `unoui_ui_init(&UI, &theme_aurora_light, FB_W, FB_H)`; build a couple of
   windows; loop `{ input → unoui_handle; feed UI_EV_TICK; unoui_render_ui(&UI);
   <port>_present(); }`.
3. Build target: compile `unoui/*.c` + `unoui/themes/*.c` + the port platform +
   `fb_aa` + shell, with `-DUNO_UUI -DUNO_BG_CACHE` and
   `-ffunction-sections -fdata-sections -Wl,--gc-sections` (drops legacy
   coupling). See the `ee uui` path in `ps2/build.sh`.
4. Build → **render-verify in the emulator.**

## Capture note (read before "verify in the emulator")

Live emulator screendumps are currently blocked by the **dev box's session
state**, not the ports: this machine is driven over RDP and the session is
presently **disconnected**, so a full-screen/GDI `CopyFromScreen` fails ("handle
invalid"), and modern console emulators (PCSX2 v2.x, flycast, redream) present
through a **GPU swapchain** that `PrintWindow` can't read either. The verified
escape hatches: (a) reconnect RDP, then the `run_pcsx2*.ps1` capture scripts
work; (b) enable the **virtual display driver** (winget package
`VirtualDrivers.Virtual-Display-Driver` is installed but no virtual monitor is
active — a one-time admin step per `~/.claude/CLAUDE.md`). Until then, each new
port is verified by: real cross-toolchain ELF build + **host-identical Aurora
render** (`uui-host`, byte-identical software path) + emulog "executes" evidence.

## Remaining work, in order

- **Phase 2 — rpi → pinephone → ppcmac** (bigger lift): these are bare-metal
  **assembly** today, but each already stands up a 640×480 **32-bit XRGB8888**
  framebuffer in its `kernel.s`. Bring up a freestanding C world (reuse pc64's
  `pc64_libc`/heap pattern), point an `fb.c` at the framebuffer base/pitch the
  asm establishes, add a platform shim (timer + serial/USB input), then unoui +
  Aurora FULL. Keep the asm kernel as default/fallback. (rpi = VideoCore
  mailbox, testable in QEMU raspi.)
- **Phase 4 mac (dithered)**: install **Retro68**; add a QuickDraw `fb` backend
  (unoui chunky fb → `CopyBits` at 8-bit); Aurora at `DEPTH_8` dithered on a
  256-color Mac. Toggle.
- **Phase 3 gba / x86 (STATIC toggle, no live composite):**
  - **gba**: a **baked static Aurora wallpaper** — precompute the 240×160
    BGR555 gradient+blobs at build time → DMA to VRAM; opaque chrome. Likely a
    faux-Aurora wallpaper behind the existing asm UI (256 KB RAM won't hold full
    unoui). `arm-none-eabi` is available.
  - **x86**: needs a **VESA LFB** truecolor mode + the pc64-style freestanding
    C+unoui world (x86 is pure nasm today). Enabling Aurora = switch to VESA LFB
    + boot the C/unoui shell (reboot to apply). Biggest single lift. STATIC
    composite.
- **Phase 4 amiga** (research spike, conditional): C toolchain (vbcc/gcc-m68k) +
  chunky→planar `c2p` + AGA/020 target → `DEPTH_4` dithered Aurora. Impractical
  on stock OCS 68000/512 KB. **iigs: not via unoui** (65816 asm, ~256 KB RAM ≈ a
  chunky fb alone); a native-asm faux-aurora only if the look is wanted.

## Design invariants (keep these)

- **Compositing is a one-time bake**: the desktop gradient + blobs render once
  into the `UNO_BG_CACHE`'d background; per-frame = a row `memcpy`. The only live
  per-frame alpha is window chrome (soft shadow / rounded corners), gated by
  `unoui_aurora_lite`.
- **"Don't composite on weak hardware"** = that hardware runs lite/static + the
  baked bg (or, for gba/x86, a build-time-baked wallpaper).
- **Aurora is a boot-applied toggle** on marginal platforms (video-mode / memory
  / UI-swap changes want a reboot).
- **Every port keeps its existing UI as the default/fallback** — Aurora is
  additive + toggleable, never a replacement.

## Coordination

A separate **perf pass** (drag damage-rect; see `AUDIT-*.md` and the branch
history) may push changes to `unoui.c` / `pc64_uui.c` on this same branch —
rebase/reconcile if so. The 8 GB dev box could not run the perf-pass toolchain
builds and Aurora builds at once (WSL VM crashed under memory pressure); on a
bigger machine that's a non-issue.
