# Deep code review 2026-07-20 - checkpoint / resume doc

Requested by arin: thorough review of the whole codebase for performance,
security, bugs, and functionality gaps, plus an adversarial pass on
implementation and architecture. Known symptoms reported by the user:

- Window dragging / graphics redraw is very slow (pc64 primarily).
- uno3d is very slow.
- Duum (Python Doom on pc64) is very slow.

Prior art already in the repo (do NOT duplicate): `AUDIT-*.md` per retro port,
`AUDIT-INDEX.md`, and `HANDOFF-perf.md` (retro-port damage-rect program, largely
done). This review targets fresh ground: pc64 graphics stack, uno3d, Duum/upy,
security of parsers + network + loaders, and cross-cutting architecture.

## Phase status

- [x] Phase 0 - scope recon (repo at master 925224f, dirty: pc64/upy/MICROPY_VERSION)
- [x] Phase 1 - all 7 subsystem reviews DONE + persisted (gfx, uno3d, duum, media,
      net, storage, arch). Each FINDINGS-<key>.md is written and em-dash-free.
- [x] Phase 2 - adversarial verification. All 4 CRITICALs CONFIRMED first-hand:
      storage C1 (modload mem_size 32-bit overflow, fields unsigned int at
      modload.c:214, np=0 -> OOB memset/memcpy), storage C2 (reloc rel[i]+8u wrap
      defeats bound -> OOB write), storage C3 (unofs fat[4608] but fat_get/set index
      cl+cl/2 ~6130 for legal cl<0xFF8 -> OOB r/w), net C1 (aes_unwrap writes n*8<=400
      into kd[256] -> 144B stack smash, MIC-gated). Also confirmed media HIGH-1 (MIDI
      kFamily[16] indexed 0..31) and arch studio_ai cfg_insecure MITM+key-exfil.
- [x] Phase 3 - REPORT.md consolidated summary written.

## Phase 1 agent roster (write each result as FINDINGS-<key>.md here)

1. gfx      - pc64/fb.c, pc64_uui.c, pc64_uui_apps.c, unoui/ - drag/redraw perf
2. uno3d    - uno3d/* incl. intel + soft backends, pc64 usage
3. duum     - pc64/apps/DUUM.PY, pyrt.c, pc64/upy config, duum_host.py
4. media    - unomedia/* memory-safety vs malicious files, pc64_media.c
5. net      - pc64/net.c, tls.c, wifi_wpa.c, NIC RX paths, pc64_http.c
6. storage  - pc64/fat.c, blkdev, ahci/nvme/sdhci, app_loader, modload, installer, unofs
7. arch     - adversarial: contract/codegen, module ABI, build system, studio_ai,
              kernel-wide patterns

## Lead's own verified findings (present path, read first-hand)

pc64 DOES have a RAM backbuffer (`fb[]` in fb.c) - drawing is not per-pixel MMIO.
The cost is in `uno_pc64_present()` (uefi_main.c:1040):

- G-LEAD-1 (P2, interactive/idle floor): present **pass 1** rescans the ENTIRE
  source fb every call (uefi_main.c:1050-1058) - full W*H read + per-pixel compare
  vs gShadow + cursor composite per row - just to *discover* dirty rows. At zoom 1
  the desktop is native res (up to FB_MAX_H 1200 tall), so this is ~1-2M
  compares every present even when nothing moved. A damage-region API (draw prims
  record dirty rows) would delete pass 1 entirely.
- G-LEAD-2 (P1 for full-screen redraw = uno3d/Duum/animation): present **pass 2**
  on the `gUseBlt` path issues one `gGop->Blt()` per output row (uefi_main.c:1082)
  - a mostly-dirty frame = hundreds/thousands of separate firmware Blt calls.
  Batch contiguous dirty rows into a single multi-row Blt. This directly taxes
  every heavy-redraw app.
- Good, keep: dirty-row shadow, precomputed gColMap/gRowMap scale tables, drag
  scene save/restore (rubber-band), div255 multiply-not-divide blend.

(The gfx agent covers this area too; reconcile/merge with its FINDINGS-gfx.md.)

## Resume instructions

If interrupted: check which FINDINGS-*.md exist in this directory; relaunch only
the missing agents (prompts summarized above, full context = this file plus the
user request). Then do Phase 2 (adversarial verify: for each P1/security finding,
a skeptic pass that tries to refute it by reading the code) and Phase 3
(REPORT.md executive summary + per-area detail already in FINDINGS files).
Do not commit anything without asking arin. No em dashes in report prose.
