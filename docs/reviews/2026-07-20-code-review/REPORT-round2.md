# UnoDOS deep code review - round 2 report (2026-07-20)

Round 2 targets the concrete bare-metal symptoms arin reported after round 1: a
USB-Ethernet freeze, a Studio Python hard crash, wacky X1 trackpad acceleration, a
floaty Surface pointer, fuzzy Notepad text, and "several other hard crashes I did not
record." Five focused investigations ran; every root cause below was verified
first-hand by reading the cited code. Per-symptom detail is in the `FINDINGS-r2-*.md`
files. No code was changed.

The throughline: **almost every one of these is a real-hardware-only bug that QEMU
cannot reproduce** (Win64-vs-SysV ABI, firmware NX, USB3 link-state events, panel
resolution, wide-desktop integer overflow). The QEMU harness is necessary but is
systematically blind to this whole class, which is why they all shipped.

---

## The six symptoms, root-caused

### 1. USB-Ethernet adapter freezes the system (hard hang)
**Cause (confirmed):** two unbounded `for(;;)` event-wait loops in the xHCI driver,
`run_command` (`xhci.c:229`) and `poll_xfer` (`xhci.c:304`). Their only non-match exit
is `poll_event` timing out, which happens **only when the USB event ring goes empty**.
A real USB3 SuperSpeed adapter continuously emits Port-Status-Change / U1-U2 link-power
events after the port reset; those are dequeued, never match the awaited completion,
and keep the ring non-empty forever. The driver binds at boot (`pc64_uui.c:1881`), so
the machine hangs bringing the adapter up. QEMU models no link-power churn, so the ring
drains and the timeout path works, which is why it always passed. `reset_port:261`
makes it worse by acking only the PRC change bit and leaving CSC/PLC to keep re-firing.
**Fix:** cap total stray events / add a wall-clock deadline to both loops, and
write-1-to-clear CSC/PLC (and/or disable U1/U2 LPM) during port bring-up.
**Secondary:** the network test targets QEMU-SLIRP-only `10.0.2.x` addresses that
black-hole on a real network, spinning 4-12M iterations in `tls.c`; and
`ax88179.c:169` reports link "up" permanently. See FINDINGS-r2-usbnet.md.

### 2. Studio hard-crashes shortly after a sample .py is built and run
**Cause (confirmed):** a GC use-after-free from an ABI-incomplete register scan. The
MicroPython GC root scan (`pc64_upy_stubs.c:16-27`) saves only the six SysV
callee-saved registers (rbx, rbp, r12-r15), but PYRT is built with mingw (Win64 ABI),
where **RDI and RSI are also callee-saved**. A live Python object held in RDI/RSI
across a `gc_collect()` is never traced, gets freed, and is then used after free ->
heap corruption -> triple fault -> reset. `MICROPY_GC_ALLOC_THRESHOLD 1` forces a GC on
nearly every allocation and the sample boxes floats every tick, so it dies within
seconds. It is a timing-dependent heisenbug, so it can survive QEMU and die on metal.
Upstream MicroPython's own NLR code saves RDI/RSI on Win64; only the GC helper missed
it. **Fix:** `#define MICROPY_GCREGS_SETJMP (1)` in `mpconfigport.h` (uses jmp_buf,
which saves the full Win64 non-volatile set).
**Latent, not yet triggered:** the GC heap is NX kernel BSS, so any future
`@micropython.viper`/`native` app will fault on real firmware (`pc64_uui.c:154` +
`--nxcompat`), and the native emitter emits SysV-ABI calls into a Win64 runtime. No
shipped `.py` uses viper today, but both must be fixed before one does. See
FINDINGS-r2-studiopy.md.

### 3. X1 Carbon trackpad: acceleration completely wacky
**Cause (confirmed):** a signed 32-bit integer overflow in the pointer scaler,
`accx += dx * uno_fb_w * g_ptr_speed / 100` (`uefi_main.c:901`). All operands are
`int`, so the product is 32-bit: `4000 * 1920 * 300 = 2,304,000,000 > INT_MAX`. Fast
flicks wrap it negative and fling the cursor backward. It is machine-specific: only the
X1's 1920-wide desktop hits the FB_MAX cap that overflows; the Surface's narrower
desktop stays under the limit. A fixed +-4000 delta clamp (`i2c_hid.c:559`) also drops
legitimate fast swipes entirely. **Fix:** do the multiply in 64-bit
(`(long long)dx * uno_fb_w * g_ptr_speed / 100`); make the clamp rate-aware.
See FINDINGS-r2-pointer.md.

### 4. Surface Laptop Go: pointer unusably floaty
**Cause (confirmed):** the Surface's pad comes through the firmware
EFI_ABSOLUTE_POINTER path, which maps a relative touchpad's absolute position straight
onto the screen (`uefi_main.c:927-930`) and then runs it through a 0.75 EMA lag filter
(`:940-943`). Absolute-mapping a relative pad is hypersensitive and teleports the
cursor; the EMA adds glide/overshoot. The native path already solves this via
`rel_from_abs()` but the firmware-Abs path never got that treatment. The author's own
in-code comment attributes "floaty on the Surface" to this filter. **Fix:** convert the
firmware-Abs pad to relative deltas like `rel_from_abs()` and drop the EMA, or make
native I2C-HID bind on the Surface. (Confirm on metal that `gAbs` is the touchpad, not
the touchscreen.) See FINDINGS-r2-pointer.md.

### 5. Notepad text looks fuzzy (low-res, poorly upscaled)
**Cause (confirmed):** not a Notepad bug. The desktop framebuffer is upscaled to the
panel with a **fractional** 16.16 scale (`apply_desktop`, `uefi_main.c:330`) turned
into nearest-neighbour source maps (`:340-347`). A non-integer scale duplicates some
glyph columns/rows and not others = broken-looking antialiasing. Notepad shows it worst
because it is a wall of small body text. The boot default (half-panel = exactly 2x) is
sharp, so the user most likely selected a non-default resolution (any Settings pick can
produce 1.2x/1.333x/1.5x, and 4K panels are capped to FB_MAX = ~1.8x). **Fix (two
lines):** floor the scale to an integer when >= 1.0 (`if (sc >= (1<<16)) sc &=
~0xFFFF;`); centering and clearing already exist, so the only change is small borders
instead of fuzz. **Bonus bug:** `TextWidth` returns `count*8` but `DrawText` uses real
glyph widths, so Notepad's caret and line-wrap drift on any non-8px font
(`mac_compat.c:225`). See FINDINGS-r2-fontscale.md.

### 6. "Several other hard crashes I did not record"
**Most likely causes (confirmed):** two stack-buffer overflows triggered by ordinary
folder navigation, both from hand-rolled unbounded copy loops sitting next to safe
helpers that should have been used:
- **Files:** `draw_pane` builds `VOLUME\PATH` into `char head[64]` via the unbounded
  `fcat`, but the pane path is `char[120]` and legitimately fills as you descend
  directories. A path past ~52 chars smashes the stack frame on the next repaint
  (`pc64_files.c:262`).
- **Photos:** `ph_activate` builds a new directory path into `char np[120]` with a raw
  unbounded copy; ~9 levels of nested folders overrun it (`apps/photos.c:580`).
Also confirmed: `pc64_game_name(GAME_RUNNER)` returns NULL because the name array has 4
slots but 3 initializers, so anything rendering the Runner's name derefs NULL
(`pc64_games.c:588`); and the in-OS UnoC compiler (`ucc.c`) has no recursion-depth
guard, so deeply nested source overflows the ring0 stack. A handful of lower-severity
latent items (a `sinf`/`cosf` infinite loop on Inf input, tracker-file note validation,
Photos OOM/gigapixel handling) are listed in FINDINGS-r2-crashsweep.md. Much of the app
layer, the allocator, and the window-manager lifecycle were checked and found clean.

---

## Cross-cutting takeaways

1. **The test harness is blind to real hardware by construction.** Every confirmed
   bug here needs a real machine: Win64 register liveness, firmware NX, USB3
   link-power events, a specific panel width, a wide-desktop integer overflow. A
   metal smoke-test matrix (boot + open every app + plug USB-eth + run a .py + drag
   windows on each of the target laptops) would have caught all six. The QEMU harness
   cannot substitute for it.
2. **The unbounded-copy idiom is a systemic hazard.** `fcat` and raw
   `while(*s)*p++=*s++` into a fixed local recur precisely where a bounded helper
   (`join_path`, `ph_join`) already exists a few lines away. A tree-wide sweep for
   this idiom is worth doing; the two found are unlikely to be the only ones.
3. **ABI discipline for the mingw/Win64 target is incomplete.** The Studio crash and
   the two latent viper/native faults all stem from code that assumes SysV on a Win64
   build. The NLR path got it right; the GC helper and the JIT path did not. Any
   hand-written asm or ABI-sensitive glue in this port should be audited against Win64
   specifically.

---

## Recommended fix order (round 2)

1. **Studio crash:** `MICROPY_GCREGS_SETJMP (1)` (one line, stops the sample crashing).
2. **Files + Photos path overflows** (`pc64_files.c:262`, `apps/photos.c:580`) - the
   likely unrecorded crashes, hit by normal navigation.
3. **X1 pointer overflow** (64-bit multiply) and **Surface pointer** (relative deltas +
   drop the EMA) - two machines currently unusable.
4. **USB-Ethernet xHCI loops** (deadline caps) + quiet the PSC source.
5. **Notepad fuzz** (integer-only upscale, two lines) + the `TextWidth` drift.
6. **Runner NULL name** (one line) and the **ucc recursion guard**.
7. Before shipping any viper/native .py: the **NX GC heap** and **SysV/Win64 JIT ABI**.
8. Tier-3 hardening from FINDINGS-r2-crashsweep.md (sinf loop, tracker load, etc.).

## Open questions (need metal to settle)
- Confirm the USB-eth freeze is the PSC storm (instrument the event ring) vs a
  different xHCI stall.
- Confirm which device `gAbs` is on the Surface (touchpad vs touchscreen), which
  decides whether the Surface fix is B1 (relative conversion) or B2 (tip-switch parse).
- Confirm the exact resolution the user ran when Notepad looked fuzzy (validates the
  fractional-scale diagnosis vs the subpixel-AA aggravator).
