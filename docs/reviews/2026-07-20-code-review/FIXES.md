# Fixes applied - review follow-up (branch review-fixes-2026-07-20)

Status as of the fix pass. Build-verified centrally after all edits.

## Security (all CONFIRMED findings fixed)
- STG-C1 pc64_modload.c: cap mem_size (64 MB) + 64-bit page math. OOB memset closed.
- STG-C2 pc64_modload.c: 64-bit reloc + import RVA bounds. OOB write closed.
- STG-C3 unofs_core.c: bound fat_get/fat_set cluster index vs fat_bytes.
- NET-C1 wifi_wpa.c: bound kdlen vs sizeof kd + aes_unwrap output-capacity arg.
- NET-H1 mrvlwifi.c / NET-H2 iwlwifi.c: RX length bounds vs DMA'd capacity.
- MEDIA-H1 um_midi.c: mask Program Change byte & 0x7F (kFamily OOB read).
- ARCH/STUDIO studio_ai.c: removed cfg_insecure MITM switch (always tls_connect_ca);
  cfg_host kept but forced through validated SNI; null-check every alloc.
- STG-H1 installer.c: validate GPT esz range + 64-bit num*esz (OOB read).
- STG-H2 fat.c: reject TotalSectors underflow + implausible cluster count (hang).

## Crashes / correctness (Round 2)
- Studio-python crash: MICROPY_GCREGS_SETJMP(1) (GC now scans RDI/RSI on Win64).
- USB-eth freeze: xhci run_command/poll_xfer stray-event caps + reset_port clears
  all change bits; ax_link reads real PHY link; tls.c spins time-bounded.
- X1 pointer overflow: 64-bit multiply in the accel scaler (uefi_main.c).
- Surface pointer: firmware-Abs path converted to relative deltas (dropped EMA).
- Notepad fuzz: integer-only present upscale (uefi_main.c apply_desktop).
- TextWidth: measures real glyph widths (caret/wrap drift).
- Files head[64] overflow: bounded copy + [160]. Photos np[120] overflow: ph_join +
  refuse-to-descend. Runner NULL name: 4th string. Photos partial-OOM + gigapixel/
  div0 guards.
- ucc.c recursion depth guard (200) + INT64_MIN/-1 fold. tracker note clamp on load.
- pc64_math sinf/cosf non-finite/huge guard. pc64_http num[] bound. g_nat[160].

## Performance (safe wins landed)
- Pointer-move no longer full-repaints: present-only when hover unchanged (pc64_uui).
- Present builds each scaled row once per source row (gRow reuse, uefi_main).
- Duum: repaint gated on tick() re-render (kills the idle 100% CPU busy loop);
  cv.wall_col clips once + blits per column (was per-pixel fb_pixel).
- uno3d: incremental rasterizer / near-clip (see agent result; verified via host).
- um_pnm binary sample clamp.

## Deliberately DEFERRED (documented, not silently dropped)
- native/viper on MicroPython: NOT enabled. Confirmed unsafe on this mingw/Win64
  build (native code executes from an NX GC heap and uses SysV ABI against a Win64
  runtime). Needs an RWX EfiLoaderCode GC heap + a Win64 JIT ABI first. mpconfigport
  comment corrected to warn.
- Present WC framebuffer (MTRR/PAT): the biggest single perf unknown, but programming
  memory types on live firmware is high-risk without on-metal measurement. Needs a
  hardware read of the current mapping first.
- gfx damage-rect per-row column extents + gUseBlt multi-row batch + icon-drag
  rubber-band: moderate present.c refactors, deferred to a focused perf pass.
- STG-M1..M5 (cluster-vs-clusters bound, 2 TiB LBA width, fat_alloc O(n^2), storage
  controller reset-after-timeout, modload path-length guard), NET-M1 (TLS entropy
  hard-fail vs the current warn): MED severity, deferred.
