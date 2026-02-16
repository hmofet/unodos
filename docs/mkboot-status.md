# MkBoot Floppy Writer — Status & Next Steps

## What's been done across builds

- **Build 202-203**: Rewrote MkBoot with floppy-to-floppy copy — pre-reads apps to RAM, prompts for disk swap, writes boot sector + stage2 + kernel + apps via FAT12 write APIs
- **Build 204**: Refined the pre-read/swap/write flow
- **Build 209**: Fixed `fs_readdir_stub` mount handle routing (was checking wrong register)
- **Build 210**: Fixed `fs_open_stub` BX→BL comparison — callers set only BL but the stub compared full 16-bit BX, causing silent open failures when BH was dirty

## What's still broken

### 1. "Copied 0 files"
Despite fixing both readdir and open routing, apps still aren't being copied. The fixes were correct (real inconsistencies), but there's likely another issue deeper in the preread→write→count flow. Possible causes:
- Another register getting trashed between API calls in the copy loop
- The preread phase succeeds but the write phase counter doesn't increment
- A different code path where BH or another register causes silent failure

### 2. Window disappearing
Affects both MkBoot and Settings. The win_draw redraw added in Build 210 didn't help. This is likely a cross-cutting issue, possibly related to the `draw_bg_color` changes from Build 209 affecting `win_draw_stub`.

## Recommended next steps

- Add diagnostic prints in MkBoot (show n_apps after preread, BX value before fs_open calls, return codes)
- Single-step trace through the preread→write flow in QEMU debugger
- Investigate the window disappearing as a separate kernel-level drawing bug (affects multiple apps)

## Key code paths

- `apps/mkboot.asm` — main app logic, preread/write/count flow
- `kernel/kernel.asm: fs_open_stub` — mount handle routing (fixed BX→BL in Build 210)
- `kernel/kernel.asm: fs_readdir_stub` — directory listing (fixed in Build 209)
- `kernel/kernel.asm: fat12_write_file` — actual FAT12 write implementation
- `kernel/kernel.asm: win_draw_stub` — window frame drawing

*Last updated: Build 210 (2026-02-16)*
