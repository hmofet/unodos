# Aurora rollout plan — bring the unoui Aurora theme to the ports, down to x86

Status: **planned**. Phase 0 starts **after the cross-port performance pass finishes**
(it is landing the cached-desktop-background + damage-rect work that Phase 0 builds
on — see `unodos-perf-pass-pending`). Companion analysis of *which* ports can support
Aurora and why: this doc's tier table + the reasoning in the audits.

## Architecture — the three constraints, solved once

1. **Compositing is a one-time bake, not a per-frame cost.** Aurora's expensive part
   (full-screen gradient + two large alpha "blobs") renders **once** into the cached
   desktop background (`unoui_bg_invalidate()` / `g_bg_valid` already exist in
   `unoui/unoui.c`, currently pc64-guarded). Every frame after is a row-`memcpy`. So
   "don't composite on weak hardware" = *bake the bg once (or at build time), never
   live-blend.*
2. **Live alpha is a capability flag.** The only per-frame alpha in Aurora is window
   chrome (6-layer soft shadow, rounded-corner alpha), gated behind
   `AURORA_COMPOSITE`:
   - **FULL** → live alpha shadow/corners (pc64, ps2, dreamcast, pinephone, rpi, ppcmac).
   - **STATIC** → baked bg + **opaque** chrome (1px hard shadow, squared/pre-masked
     corners), zero per-frame blend — for marginal hardware.
3. **Toggle, applied at boot.** A persisted `aurora = on/off` setting. On marginal
   platforms, enabling may require a video-mode change (x86 VGA→VESA LFB), memory
   reallocation (bg cache), or switching from the asm UI to the C/unoui shell — all
   cleanest at boot, so the setting reads *"reboot to apply."*

## Per-port tier

| Tier | Ports | Aurora mode |
|------|-------|-------------|
| **FULL** | pc64 (done), ps2, dreamcast, pinephone, rpi, ppcmac | live alpha, 32-bit (dreamcast → RGB565 out) |
| **STATIC toggle** | gba, x86 | baked bg + opaque chrome, no live composite, off by default |
| **DITHERED** (low depth) | mac (viable), amiga (conditional) | Aurora palette at DEPTH_8/4 via `ui_shade()` |
| **Not via unoui** | iigs | native-asm faux-aurora only, if wanted |

## Phases

### Phase 0 — Foundation (M, linchpin)
- Un-gate the cached-bg path in `unoui.c` (currently `#ifdef` pc64) → portable default.
- Add the `AURORA_COMPOSITE = full|static` split: `theme_aurora` (full) vs a `static`
  variant (baked bg, opaque 1px shadow, squared corners, no per-frame blend).
- Add the per-port `UNO_AURORA` build flag + the runtime toggle plumbing (setting in
  the Theme/Control app, persisted, applied on next boot).
- **Build on the perf pass's cached-bg work; rebase after it merges.**

### Phase 1 — C ports that already have `fb.c` (M each — fast wins)
- **ps2**: add `unoui/*.c` as an alt EE target (`build.sh ee uui`) + `ee_uui_main`
  shim (pad input, EE timer, present `fb`→GS `CT32`, already 32-bit). Aurora FULL.
  Verify in PCSX2. Already composites through the same ARGB8888 `fb.c` unoui wants.
- **dreamcast**: add unoui to the KOS build + `dc_uui_main` (maple input, present
  `fb`→565 already exists). Aurora FULL internal, 16-bit out (slight gradient
  banding). Verify emu/hw.

### Phase 2 — Asm ports with a ready 32-bit framebuffer (L each)
Each needs a **freestanding C world** stood up beside the asm kernel (reuse pc64's
`pc64_libc`/heap pattern), `fb.c` pointed at the framebuffer their asm bring-up already
establishes (hand off base+pitch to C), a platform shim (timer/input), then unoui +
Aurora FULL. Keep the asm kernel as default/fallback + other themes.
- **rpi** first (VideoCore mailbox fb, QEMU raspi) → **pinephone** (A64 DE2.0, fb
  already brought up in asm) → **ppcmac** (OF fb, big-endian XRGB already handled).

### Phase 3 — Marginal: Aurora = STATIC toggle, no live composite
- **gba (S–XL)**: 16 MHz ARM7 can't composite. Aurora = a **baked static wallpaper**
  (precompute the 240×160 BGR555 gradient+blobs at *build* time → const → DMA to VRAM),
  opaque chrome. RAM is the wall (chunky 240×160×4 fb = 153 KB vs ~350 KB total +
  unoui + fonts) → realistically ship a **faux-Aurora wallpaper behind the existing
  asm UI**, not the full unoui toolkit. Toggle off by default.
- **x86 (XL)**: needs (a) a **VESA LFB** hicolor/truecolor mode and (b) the pc64-style
  **freestanding C + unoui world** (x86 is pure nasm today). Enabling Aurora sets VESA
  LFB + boots the C/unoui shell; default keeps the fast native VGA asm UI. Reboot
  required. STATIC composite. Biggest single lift.

### Phase 4 — Stripped-down at lower depth (the amiga/mac/iigs question)
Mechanism is ready (`ui_shade()` dithers gradient/shadows into a limited palette). The
blocker is whether the system can run the unoui C toolkit + hold a chunky fb:
- **mac — YES, easiest (M).** Hosted, already C (Retro68). Add a QuickDraw `fb` backend
  (unoui chunky buffer → `CopyBits` at the Mac's depth). Aurora at **DEPTH_8** with a
  dithered Aurora palette on a 256-color Mac (LC / Color Classic). Toggle.
- **amiga — CONDITIONAL (L + new toolchain).** Chunky fb (327 KB) fits a **≥1 MB**
  Amiga, but needs (1) a C toolchain not in-project (vbcc/gcc-m68k), (2) **chunky→planar
  `c2p`** every present (expensive on 68000), (3) AGA or 020+ for a decent DEPTH_4/8
  palette. Real on an **020/AGA** target as a spike; impractical on stock OCS 68000/512 KB.
- **iigs — NO via unoui.** A chunky fb alone (~256 KB) ≈ the whole machine's RAM, and
  it's 65816 asm with no C path. An "Aurora-like" IIGS desktop is only reachable as a
  **hand-written dithered 16-color SHR theme in the existing asm UI** — a native
  reimplementation, not the unoui toolkit. Skip unoui-Aurora; native faux-aurora (S)
  only if the look is wanted.

## Verification
Each port via its emulator + screendump (PCSX2, lxdream, QEMU raspi/virt/ppc, mGBA,
QEMU-x86), same baseline-vs-result render discipline as the perf pass.

## Suggested order
Phase 0 → ps2, dreamcast → rpi, pinephone, ppcmac → **mac dithered** (easy C win) →
gba wallpaper → x86 → amiga spike. iigs: native-asm only if desired.

## Dependencies / risks
- **Perf pass (blocking):** Phase 0's cached-bg overlaps what it's implementing for
  pc64 — start Phase 0 only after it lands, and rebase onto it.
- New toolchains: vbcc (amiga, if pursued); the freestanding-C bring-up for x86.
- Keep every port's existing UI as the default/fallback so Aurora stays additive +
  toggleable.
