# Deep code review round 2 - bare-metal bug reports (2026-07-20)

Round 1 (REPORT.md + FINDINGS-*.md) is complete. arin then ran a quick bare-metal
test and reported concrete symptoms. Round 2 hunts the exact root cause of each,
plus a broader crash sweep, because "there are no doubt other undiscovered issues."

## STATUS: COMPLETE. All 5 done + verified first-hand. REPORT-round2.md written.
- DONE studiopy -> FINDINGS-r2-studiopy.md. Verified: GC reg-scan
  (pc64_upy_stubs.c:16-27) saves only SysV 6, misses RDI/RSI (Win64 callee-saved,
  mingw build) -> live mp_obj_t UAF -> crash. Fix MICROPY_GCREGS_SETJMP(1). Plus NX
  GC heap + SysV/Win64 JIT ABI latent (no viper app ships yet). Stub comment
  self-incriminates. All 6 symptoms root-caused; every finding em-dash-free.
- DONE crashsweep -> FINDINGS-r2-crashsweep.md. Verified first-hand: Files head[64]
  overflow via fcat (pc64_files.c:262), Photos np[120] overflow (photos.c:580),
  Runner NULL name (pc64_games.c:588, n[4] has 3 inits). These two path overflows are
  the likely unrecorded crashes. Plus ucc recursion, sinf Inf-loop, tracker load, etc.
- DONE fontscale -> FINDINGS-r2-fontscale.md (fractional NN upscale; verified
  apply_desktop:308-361; 2-line integer-floor fix). Bonus: TextWidth caret/wrap drift.
- DONE pointer -> FINDINGS-r2-pointer.md (X1 = int32 overflow uefi_main.c:901,
  4000*1920*300 > INT_MAX, verified; Surface = absolute-map + EMA lag, verified).
- DONE usbnet -> FINDINGS-r2-usbnet.md (xhci unbounded event loops :229/:304 hang on
  a real USB3 PSC storm; verified both loops + reset_port:261 only acks PRC).

## Reported symptoms -> assigned investigation

1. **USB Ethernet card: system FROZE when running the network test.**
   -> agent `usbnet`: pc64/rtl8152.c, ax88179.c, xhci.c (USB host), net.c network
   test path, apps/network.c. Freeze = unbounded poll / link-up wait / no timeout.
2. **Hard CRASH shortly after the sample .py is built and run in Studio.**
   -> agent `studiopy`: apps/studio_py.c, apps/pyrt.c, apps/studio.c,
   pc64_modload.c pyapp path, executable-heap / ABI concern the duum pass flagged.
3. **X1 Carbon trackpad: mouse acceleration completely wacky.**
   **Surface Laptop Go: pointer unusably floaty.**
   -> agent `pointer`: pc64/i2c_hid.c, usbhid.c, uefi_main.c pointer accel + speed
   (g_ptr_speed), absolute vs relative handling.
4. **Notepad looks very fuzzy (low-res render then poor upscale).**
   -> agent `fontscale`: apps/notepad.c, pc64_font.c, uefi_main.c scale path
   (gColMap/gRowMap nearest-neighbour), why Notepad specifically.
5. **Other unrecorded hard crashes + "dig deeper."**
   -> agent `crashsweep`: robustness sweep of pc64 kernel/app paths for null-deref,
   unchecked alloc, stack overflow, unbounded loop, use-after-free.

Each writes FINDINGS-r2-<key>.md here. Lead verifies the crash/freeze root causes
first-hand before the round-2 report (REPORT-round2.md). No code changes without
arin's OK. No em dashes in prose.

## Lead first-hand pointer notes (read pending the `pointer` agent)

pc64/uefi_main.c poll_pointer (883-960) + pc64/i2c_hid.c rel_from_abs (546-564):
- i2c-hid delta math (uefi_main.c:901-907) is LINEAR/plausible (a full-pad swipe =
  ~1.5 screens); rel_from_abs correctly computes deltas and updates last_x/y. So the
  X1 "wacky accel" is likely NOT this arithmetic but (a) the report-descriptor parse
  (x_off/x_bits/contact-count/high-res fields misread on the Synaptics precision
  pad), (b) the i2c clock corruption noted in memory, or (c) the fling guard below.
- SUSPECT for X1 "wacky": i2c_hid.c:559 swallows the WHOLE delta when any axis moves
  >4000 pad-units in one poll. On a slow poll a legit FAST swipe exceeds that and is
  dropped entirely, so slow moves track but fast moves stick-then-jump = nonlinear/
  wacky feel. Threshold is a fixed constant, not rate-aware.
- Surface "floaty": absolute path uses a low-pass `g_cx=(g_cx+tx*3)/4` every update
  (uefi_main.c:941-943). For an ABSOLUTE precision touchpad that lag IS the
  floatiness; direct map (g_cx=tx) + small dead-zone would be crisp. Comment admits
  they retuned 2:1->3:1 but kept a lag filter.
- Path-selection question for the agent: does the Surface pad bind as native i2c-hid
  (relative path, "1:1 no filter") or firmware Absolute Pointer (filtered path)? The
  in-code comments contradict each other about which the Surface uses (937 vs 935).
