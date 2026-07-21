# pc64 — proposed fixes and optimizations

Analysis of the metal findings in [METAL-FINDINGS.md](METAL-FINDINGS.md), scoped
to the **Surface Laptop Go, X1 Carbon Gen 8 and X13 Yoga Gen 3** (the Latitude
boot-selection question and the MacBook F9 are excluded). Written for a
dedicated fix session. P0.1-P0.3 are now IMPLEMENTED on this branch as a
measurable experiment (see the P0 status note); everything else is proposal only.

Evidence base: 4 clean metal runs, 4 machines' boot-environment blocks, and the
first real render-vs-present frame timings.

---

## 0. The one measurement that should drive everything

From the X13 Yoga, per-pass:

| pass | render_avg | present_avg | present/render |
|------|-----------|-------------|----------------|
| 0 | 2 924 us | 238 576 us | **82x** |
| 2 | 3 186 us | 262 518 us | **82x** |

Drawing the entire scene costs **~3 ms**. Pushing it to VRAM costs **~250 ms**.
Corroborated independently by the bandwidth bench: 1920x1080x4 = 8100 KB at
27 236 KB/s = **297 ms**.

**Therefore: ~99 % of a frame is the uncached framebuffer write.** Optimising
the rasteriser, the alpha blender, uno3d, or the scene painter is worth
approximately nothing. Only two things can help:

1. make VRAM writes faster (change the memory type), or
2. write fewer bytes to VRAM.

Everything in P0/P3 below is one of those two. **Do not start with the
renderer** — that is the intuitive move and the measurements say it is wasted.

---

## P0 — Write fewer bytes to VRAM (safe, large, do first)

> **STATUS: P0.1-P0.3 IMPLEMENTED and MEASURED ON METAL. P0.4 dropped (see
> below). Same machine, same deterministic 3-pass workload, X13 Yoga:**
>
> | pass | present BEFORE | present AFTER | gain | fps |
> |------|---------------|---------------|------|-----|
> | 0 | 238 576 us | **107 825 us** | **2.21x** | 5 -> 7 |
> | 1 | 98 653 us | **29 160 us** | **3.38x** | 16 -> 24 |
> | 2 | 262 518 us | **121 732 us** | **2.16x** | 4 -> 8 |
>
> **Whole-run wall clock: 121.3 s -> 70.4 s (1.72x) for identical work.**
> `render_avg` is unchanged (2924->2893, 75762->75548, 3186->3154), exactly as
> designed — these changes touch only bytes-written, not render cost. Zero
> crashes; the model holds.
>
> **Honest read on the shortfall:** I estimated 5-20x for P0.1 and we got ~2.2x.
> The stress workload opens and closes windows constantly, so most frames dirty
> large areas and the span tracking has little to trim — pass 1, the pass with
> the fewest dirty rows, is also the one that gained most (3.38x). Real
> interactive use (typing, caret blink, hover) should do better than this
> synthetic run shows, and a good chunk of the measured 2.2x is likely P0.3's
> wider stores rather than the spans. Worth separating if it ever matters.

No MTRR games, no firmware assumptions, contained changes in `uno_pc64_present`.

### P0.1 Dirty **spans**, not dirty rows  ·  est. 5-20x on interactive frames
`uefi_main.c` tracks dirtiness per *row* (`gDirtyRow[]`) and then writes the
**entire output row width** for any row that changed. A blinking caret, a hover
highlight, or a clock tick therefore repaints 1920 px per touched row.

The comparison pass already walks every pixel to decide dirtiness:

```c
for (x = 0; x < fbw; x++) if (src[x] != sh[x]) { dirty = 1; break; }
```

Record the **first and last** changed column while doing it (`xmin`/`xmax` per
row) instead of breaking early, then write only `[xmin..xmax]` scaled into the
output row. Cost: the compare loop no longer early-exits (it already reads the
whole row on the common no-change path, so this is close to free). Benefit: a
caret repaint drops from 1920 px to ~10 px.

Note the scaling: source column c maps to output columns via `gColMap`, so the
output span is `[first oy where gColMap[ox] >= xmin .. last <= xmax]`. Simplest
correct approach is to invert with the same 16.16 factor used to build the map.

### P0.2 Stop upscaling full-screen low-res content  ·  est. **16x for Runner3D**
`uno_pc64_lowres()` drops the desktop to 1/4 x 1/4 so 3D renders ~1/16 the
pixels — but `uno_pc64_present()` then scales it back up and writes the **full
panel area** to VRAM. The optimisation targets render cost, which we now know is
3 ms, and leaves the 250 ms untouched. It is aimed at the wrong half of the frame.

Fix: when the desktop is in low-res mode, present **1:1 centred/letterboxed**
rather than upscaled. 480x270 written to VRAM instead of 1920x1080 is a direct
16x cut in the only cost that matters. Visually a smaller image on a black
field; at 3 fps -> ~30 fps that is an easy trade, and it can be a toggle
(`Settings > fill screen` for machines where present is cheap).

### P0.3 Wider stores  ·  est. 1.5-2x, ~10 lines
The inner loop is 32-bit:

```c
for (x = 0; x < gOutW; x++) dst[x] = gRow[x];
```

On UC memory every store is its own bus transaction, so transaction *count*
dominates. Writing 64 bits (2 px) per store roughly halves them; 128-bit SSE
stores (`movdqu`, the buffer is 16-byte alignable) may do better. Measure with
the existing `fb bench` harness — it already reports KB/s, so this is directly
verifiable per machine.

Do **not** expect non-temporal stores to help: on a UC region the NT hint is
ignored, they do not write-combine.

### P0.4 ~~Don't repaint the whole screen for fullscreen apps~~ — DROPPED
On a closer read this was wrong. `if (UI.full) g_dirty = 1;` forces a re-RENDER,
but the shadow comparison downstream still rejects every unchanged row, so it
costs the 3 ms half of the frame and **not** the 250 ms half. Removing it would
have risked breaking animation in fullscreen apps that rely on it, for no VRAM
saving. Left alone.

**Expected combined P0 effect:** interactive/desktop frames become 5-20x
cheaper; Runner3D ~16x. That is the difference between 3 fps and playable,
without touching a single MTRR.

---

## P1 — Unblock input, and a crash fix (small, high value)

### P1.1 F4: drive I2C-HID discovery from ACPI, not PCI guessing
Present state across machines:

| machine | ctrls | present | addr | desc_parsed |
|---------|-------|---------|------|-------------|
| Surface | 3 | 0 | 0 | 0 |
| X1 Carbon | 2 | 0 | 0 | 0 |
| X13 Yoga (touch!) | **0** | 0 | 0 | 0 |

Two distinct failures, and the table separates them cleanly:
- Where controllers **are** found, no device binds and `addr=0` — we never
  discover the device's I2C slave address, so nothing can bind.
- On the Yoga we find **zero controllers**, on a machine that certainly has I2C
  touch — so the controller enumeration itself is wrong there, not just binding.

Both have the same correct answer: **enumerate from ACPI**. An I2C-HID device is
declared in the ACPI namespace with `_HID` = `PNP0C50` (or `ACPI0C50`) and a
`_CRS` containing an `I2cSerialBus` descriptor that gives the **controller path
and the slave address**; the HID descriptor register comes from `_DSM`. That is
the address we are currently missing, and the controller path also fixes the
Yoga case (newer chipsets present the controller through ACPI rather than at the
PCI IDs we scan for).

We already ship a working AML interpreter (`unoacpi`, used for battery/lid), so
this is an extension of existing machinery, not new infrastructure.

**Knock-on:** fixing this also removes the Surface's firmware OSK (a native
keyboard binds, so firmware ConIn is dropped) and dissolves F6's detach block.

### P1.2 F7: `text_pen()` shifts a negative int  ·  one line + an audit
`pc64_font.c:307`:

```c
int pen26 = x << 6;      /* UB when x < 0 */
```

Fix: `int pen26 = x * 64;` — well-defined for negatives. Then **clip**: skip
glyphs entirely left of the surface and partially clip the first visible one,
so negative x is not merely defined but correct.

Audit the callers that can go negative — the centred idiom
`fb_text(cx - fb_text_w(s)/2, ...)` used by the splash and dialogs is negative
whenever the string is wider than the surface. `prov_glyph()` has the same
`x << 6`. In a release build this is *silent* UB, not a trap.

---

## P2 — Correctness and safety (small)

### P2.1 F8: make the detach gate boot-volume aware
`uno_fat_native_eligible()` asks "is *some* UnoDOS-bearing volume natively
reachable?", which is the wrong question. It should ask **"can the native stack
still reach the volume we booted from?"** The boot device is already known:
`uno_pc64_image_handle()` -> `LoadedImage->DeviceHandle` -> device path. If that
device is not AHCI/NVMe/SDHCI-backed (i.e. it is USB), refuse to detach.

Without this, any user who installs UnoDOS and later boots a USB stick loses
their boot volume mid-session — S1, and it also silently kills all app/module
loading, not just telemetry.

### P2.2 `crash_vol()` negative caching
It caches only on success, so on a machine where nothing qualifies **every call
re-scans every volume** (`uno_fat_list_ex` + `uno_fat_mkdir`). The debug build
calls it from a 30 s heartbeat. Cache the failure or bound the retries.

### P2.3 F10 cosmetics
- The `fb bench` paints the bottom 64 rows of the panel; when the desktop is
  letterboxed those rows are never repainted and the test pattern stays on
  screen all session. It reads as framebuffer corruption in photos — bad, since
  corruption is what we are hunting. Clear the rows after benching, or bench
  into an offscreen/soon-overdrawn region.
- The splash `DEBUG / STRESS BUILD` banner collides with the loading bar.

---

## P3 — The 100x fix: a write-combining framebuffer (high value, real risk)

This is the *correct* answer to F3 and subsumes most of P0, but it is the one
place where getting it wrong destabilises the machine, so it should come after
the safe wins are banked.

Confirmed constraint from four machines: the framebuffer is covered by a
**variable MTRR of type UC**, and that same range also covers device MMIO.

| machine | MTRR over fb | fb base |
|---------|--------------|---------|
| Surface | `mtrr6 base=4000000000 mask=4000000800` (coarse, >4 GB) | `4000000000` |
| X1 Carbon | `mtrr0 base=80000000 mask=7f80000800` | `c0000000` |
| X13 Yoga | `mtrr0 base=c0000000 mask=7fc0000800` | `d0000000` |

Two approaches that do **not** work, already established:
- Adding a WC MTRR over just the fb: on a UC∩WC overlap **UC wins** (SDM).
- PAT: `PAT=WC` combined with `MTRR=UC` still resolves to **UC**.

So the UC range must stop covering the framebuffer. Sketch:

1. Enumerate real device MMIO precisely — we already have `pc64_pci.c`, so walk
   every function's BARs and build the set of ranges that genuinely require UC.
2. Rebuild the variable MTRR set: UC only over those BAR ranges, WC over the
   framebuffer extent (`GOP FrameBufferBase/Size`), default WB. MTRR ranges are
   power-of-2 aligned/sized, so carving a hole costs several entries — check
   `IA32_MTRRCAP.VCNT` (usually 8-10) for headroom before committing.
3. Apply with the documented sequence: `cli`; `CR0.CD=1, NW=0`; `WBINVD`; clear
   `IA32_MTRR_DEF_TYPE.E`; reprogram pairs; `WBINVD`; set `E`; `CR0.CD=0`; `sti`.
4. Re-run the existing `fb bench` immediately and log before/after — we already
   have the instrument, so success is measurable on the spot.

**Ship it opt-in first** (flag file / `STRESS.CFG` key, default off) so a bad
attempt never affects a normal run, and with the operator at the machine to
power-cycle. Expected: 14-43 MB/s -> GB/s, i.e. present stops being the
bottleneck at all.

Cheaper thing to try first, ~free: **time `GOP Blt()` against direct writes on a
UC machine.** The firmware blitter may DMA. The `gUseBlt` path already exists, so
this is a measurement, not an implementation — and if Blt wins it is a large,
zero-risk gain.

---

## P4 — Enabler: USB mass storage over xHCI

`xhci.c` currently serves HID only. A bulk-only-transport MSC driver would:
- let a USB-booted system survive detach (the real fix behind P2.1),
- unblock F6 so the native stack finally runs on metal,
- and remove the "every test boot is from USB, so we cannot test detach" bind
  the harness is in today.

Largest item here; sequence it after P0-P2.

---

## Suggested order

1. **P0.1 + P0.2 + P0.3** — safe, measurable, and they attack the only thing
   that costs time. Verify each with `fb bench` + the PF perf line.
2. **P1.2** (one line, prevents crashes), then **P1.1** (ACPI I2C-HID — the
   single change that fixes input on every machine and dissolves F6).
3. **P2.1/P2.2/P2.3**.
4. **P3**, opt-in, operator present. Try the `Blt()` measurement first.
5. **P4** when the above are banked.

## How to verify any of it

The harness already measures everything needed — no new instrumentation:
- `BOOTENV.TXT` -> `fb bench` KB/s (P0.3, P3)
- `CRASH\PF###.TXT` -> `render_avg` / `present_avg` / `fps` (P0.1, P0.2, P0.4)
- `BOOTENV.TXT` -> `i2c-hid ctrls/present/addr/desc_parsed` (P1.1)
- UBSan traps -> `CRASH\CR###.TXT` (P1.2 regressions)
- `detached:` + `volumes:` in the env block (P2.1)

A pass is ~30 s; `passes=3` is a ~2 minute regression run per machine.
