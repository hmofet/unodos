# UnoDOS Audit & Stabilization — Handoff Document

**Date:** 2026-06-11 · **State:** v3.25.0, Build 403, branch `master`
**Audience:** the next development session/agent picking this work up cold.

---

## 1. What happened

The user reported: (1) apps crashing when launching apps / running many apps,
(2) visual anomalies, (3) keyboard & mouse input issues, and asked for a
window-manager create/destroy/z-order verification, performance tuning, and
8088/640KB compatibility.

A 116-agent audit ran against Build 400: 8 static subsystem auditors + 3
dynamic QEMU testers, with every static finding adversarially verified by an
independent agent. **140 findings: 97 confirmed, 25 observed live in QEMU, 8
refuted, 10 low/unverified.** Five waves of fix agents then applied the
verified patches. Every wave was build- and boot-verified in QEMU.

### Key documents

| File | Contents |
|------|----------|
| `docs/audit-2026-06-digest.md` | **Every finding**, sorted by verdict/severity, each with description, independent verification analysis, and an exact code-level FIX block. ~5300 lines. This is the primary backlog for remaining work. |
| `tools/qemu_test.sh` | Headless QEMU test driver (see §6). |
| `tools/to8086.py`, `kernel/cpu8086.inc` | Ready-made tooling for the pending 8088 pass (see §5). |
| `CHANGELOG.md` [3.25.0] | User-facing summary of the fixes. |

---

## 2. Root causes found for the reported symptoms

1. **win_create grip-pixel kernel corruption** (fixed): `win_draw_stub`'s
   resize-grip block called `plot_pixel_color` with the caller's ES (often
   the kernel segment 0x1000, set by win_create's title copy) instead of the
   video segment — the 4 grip dots read-modify-wrote **live kernel
   instructions** at CGA-equivalent offsets. A 160×100 window at the default
   position patched `gfx_draw_string_stub`'s code bytes. Apps calling API 22
   directly sprayed their own segments. This single bug plausibly explains
   most "random" crashes and corrupted-handle reports.
2. **Heap inside the kernel image** (fixed in Build 401/402, separate
   session): heap was at 0x1400:0 — 16KB into the 44KB kernel. Now at
   dedicated segment 0x8000 (60KB); **user app pool reduced to 5 slots**
   (0x3000–0x7000).
3. **Z-order drift** (fixed): create/focus demoted every window, destroy
   never renormalized → after ~7 launch/close cycles all background windows
   collided at z=0; painting, hit-testing and promotion then disagreed.
   Matches the observed "click destroys window frame", "desktop repaints
   over windows", "no click-to-raise".
4. **Event-queue head-of-line blocking + post race** (fixed): one
   undelivered event at the global queue head stalled input for every task;
   un-cli'd tail updates lost events whenever IRQ posts raced task posts.
   Matches the intermittent input loss.
5. **fat12_read stack corruption** (fixed): 7 pops for 6 pushes on any read
   from position != 0 → return into garbage. Apps doing seek+read or
   sequential reads crashed.
6. **Kernel load at exactly 100% of stage2's 88-sector limit** (fixed):
   any growth silently truncated the kernel tail. Area expanded to 104
   sectors; build now fails if exceeded.
7. **Font advance 12px on the 8x8 font** (fixed): cause of the boot-visible
   overlapping icon labels, plus 50% extra per-character draw cost.

---

## 3. Current memory & disk layout (post-fix, authoritative)

```
0x0800:0000  Stage 2                    2 KB
0x1000:0000  Kernel                     52 KB area (104 sectors; image padded,
                                        build fails if exceeded)
0x2000:0000  Shell/Launcher (fixed)     64 KB
0x3000-0x7000 User app slots 0-4        5 × 64 KB  (was 6 — 0x8000 is now heap)
0x8000:0000  Kernel heap (API 7/8)      60 KB
0x9000:0000  Scratch: clipboard 0-0xFFF, file-dialog 0x1000+,
             VESA ModeInfoBlock buffer at 0x2000 (moved from 0x0000)
```

Floppy (1.44MB): boot LBA 0 · stage2 LBA 1–4 · kernel LBA 5–108 ·
FAT12 reserved-sector count **110**. These constants are quintuplicated —
keep in sync: `boot/stage2.asm KERNEL_SECTORS`, `kernel/kernel.asm` end-pad,
`boot/boot.asm bpb_rsvd`, `tools/add_floppy_fs.py FS_START_SECTOR`,
`apps/mkboot.asm FLOPPY_KERNEL_SECTORS/FLOPPY_FS_START`. **The kernel FAT12
driver does NOT read the BPB** — `fat12_mount` hardcodes fat_start=111 /
root_dir_start=129 / data_area_start=143.

Other structural facts:
- The kernel API table is pinned by a mid-file `times 0x3400 - ($-$$)` pad
  (bumped from 0x3320 — it has a history of needing bumps). Code added
  *before* it can blow the build; position-independent functions can be
  relocated after the table (done for `gfx_blit_rect`).
- Kernel binary: exactly 53248 bytes, ~7-8KB of true slack at Build 403.
- Event queue: 32 entries × 3 bytes, tombstone type 0xFF, forward-scan
  delivery, mouse-event coalescing at post time.
- Task context now includes ES (initial frame: ES at 0xFFEE, SP starts
  0xFFDE — see `app_start_stub` comment block).

---

## 4. Fixes applied (all build- and QEMU-verified)

**Wave 1 — foundations:** fat12_read stack bug + EOF path · post_event
pushf/cli · dispatcher one-shot flags · ES in task context (5 sites) ·
event_wait/kbd_wait yield (with boot-context guard) · event-queue forward
scan · event_get CX/DX preservation · VESA clipboard clobber · disk layout
88→104 sectors (6 files) · desktop icon table 40 + NUL-termination + label
truncation (launcher & kernel) + dirty-rect sizing + boot selection.

**Wave 2 — WM & input:** z-order renormalization (focus + destroy) ·
win_resize/win_focus repaint · occlusion-aware resize hit-test · z-clipped
WIN_REDRAW discard · dispatcher z-clip stale DX/SI + API 50 CX fix · title
truncation · destroy_task_windows batching · **grip-pixel kernel corruption
fix** · scancode bounds · XT 8255 keyboard ack · NumLock-aware numpad
arrows (XT/84-key) · IRQ12 AUX-bit fix · mouse desync self-healing · mouse
event coalescing. (Mouse clamping was already fixed in the Build 402 work.)

**Wave 3 — graphics:** CGA scroll→VESA fall-through · VESA scroll bank
straddling (fast + per-byte paths) · font advance 12→8 · draw_char /
draw_char_inverted clip enforcement · blit overlap direction (CGA + VGA) ·
read_pixel for VESA/mode-12h (fixes blit black fills) · CGA fill/clear
bounds clamps · vesa_fill_rect DI=0 bank skip · vesa_set_bank granularity ·
CGA scroll edge-pixel masking.

**Wave 4 — performance (PARTIALLY APPLIED — interrupted):** the
gfx_fill_color CGA hybrid fast path **landed** (`.gfc_hybrid`). The
dispatcher movzx/bt → 8086 rewrite did **NOT** land. Status of stosb→stosw,
draw_char row-base hoisting, floppy multi-sector reads, and the cursor
save/restore optimization is **unverified** — diff `kernel/kernel.asm`
against the digest entries at lines ~4554/4698/4813 to see what's present.
The current state builds and passes boot/launch/type/restore tests.

---

## 5. REMAINING WORK (prioritized backlog for the next session)

### A. 8088 compatibility — NOT STARTED in code (tooling ready)
The audit's hard verdict: **the OS cannot run on an 8088 today** — 1153
non-8086 instruction sites across 21 files (kernel 357, pacmanv 154, pacman
121, notepad 88, launcher 61, stage2_hd 56, outlastv 54, browser 49, tetris
46, outlast 44, tetrisv 44, …, stage2 3, vbr 1; `boot/boot.asm` and the
font files are clean). On 8088, PUSHA/POPA execute as JO/JNO — silent
control-flow corruption; the first INT 0x80 dispatch dies at `movzx`/`bt`.
Plan (tooling already in repo):
1. Run `python3 tools/to8086.py` over kernel/boot(stage2,vbr)/apps — it
   rewrites pusha/popa/shift-imm/movzx mechanically, inserts
   `%include "kernel/cpu8086.inc"`, and prints `MANUAL` lines for what it
   won't touch (imul-imm, push-imm, bt, 32-bit ops, flag-sensitive movzx).
2. Fix the MANUAL sites: dispatcher movzx+bt ×2 (exact 8086 replacement in
   digest lines 337-355), push-imm ×3, imul ×10 (KMENU_ITEM_H — mul via CX),
   mkboot dword stores ×9 (word pairs).
3. FAT16/IDE region (~146 32-bit sites, kernel lines ~11955-14310 pre-drift)
   is a genuine 16-bit DX:AX rewrite — OR wrap it in `cpu 386`/`cpu 8086`
   directives and gate FAT16 mount at runtime with an 8086 CPU check
   (FLAGS bits 12-15 always set on 8086) so floppy-only 8088 boots work.
   `boot/mbr.asm`/`boot/stage2_hd.asm` (HD boot) similarly stay 386+.
4. Add `cpu 8086` to each converted file, then iterate on NASM "short jump
   out of range" errors (invert-condition + jmp rewrite). A prior agent
   estimated ~220 such sites in the kernel.
5. Verify: clean assembly under `cpu 8086`, byte-identical behavior in QEMU,
   then real-8088 testing needs 86Box/PCem (QEMU cannot emulate an 8088).
6. Note: the README example app uses pusha/popa — update it too
   (`apps/hello.asm` likewise).

### B. Cursor hide/lock race — NOT APPLIED (digest lines 1863-1890)
IRQ12 can redraw the cursor between `call mouse_cursor_hide` and
`inc byte [cursor_locked]` → XOR droppings / stale save-under rectangles.
Fix is mechanical: add the `cursor_protect_begin` helper from the digest and
replace all ~35 two-line pairs (`call mouse_cursor_hide` + `inc byte
[cursor_locked]`) — do NOT touch inc sites without a preceding hide call.
Deferred because every wave was editing those regions concurrently.

### C. Finish the interrupted performance wave
Audit digest entries: gfx_fill_color hybrid (done — verify edge cases),
draw_char row-base MUL hoisting (~4554/4092), floppy multi-sector reads
(~4698), stosb→stosw row fills (~4813), mouse-cursor draw MUL (~4602),
dispatcher movzx/bt (also part of 8088 work). Re-run the typing screenshot
test after any draw_char change.

### D. Confirmed findings not yet fixed (digest line refs)
- app_load_stub: no file-size validation vs segment/stack area (~2709)
- Task kill leaks open file handles → file_table exhaustion (~4392)
- Keyboard focus evaluated at consume time — keys leak across focus
  changes (~3552)
- EVENT_MOUSE carries no coordinates/edge — click position raced (~3475;
  design change, 3-byte queue entry limits this)
- IRQ12 draws cursor via VESA BIOS bank switches inside the ISR (~3415)
- Default `make` 360KB floppy target cannot boot — stage2/kernel hardcode
  1.44MB geometry (~2578); consider deleting the target or fixing geometry
- FAT16 INT 13h AH=42h used without AH=41h presence check (~4499)
- KBC fallback pokes 0x60/0x64 blindly on XT (~4459)
- Launcher key-stealing during fullscreen apps; SysInfo uptime wrong
  (reads RTC, not tick delta — observed ~5178); Notepad status bar stale
  while typing (~5153); window drag can move title bar off-screen (~5173).

### E. Dynamic re-verification
Re-run the audit's dynamic scenarios against Build 403+ (they were run
against Build 400): launch-stress (now 5 user slots + shell — verify the
6th launch fails gracefully), window open/close/z-order cycles, input tests.
The 25 "observed" findings in the digest list exact reproduction steps.

---

## 6. Build & test quickstart (Windows host)

```bash
# Build (WSL: nasm, make, qemu-system-x86, python3, socat, netpbm installed)
wsl -e bash -c "cd /mnt/c/Users/arin/Documents/Github/unodos && make floppy144"

# Headless QEMU test (driver: tools/qemu_test.sh; one fresh boot per run)
wsl -e bash -c "cd /mnt/c/Users/arin/Documents/Github/unodos && \
  printf 'wait 15\nshot desktop\nkeys down down right ret\nwait 5\nshot app\nquit\n' \
  | bash tools/qemu_test.sh build/unodos-144.img test-artifacts demo"
# Commands: wait N | key X | keys a b c | type text | mousemove DX DY |
#           click [btn] | btn N | dblclick | shot NAME | quit
# Mouse: 'mousemove -2000 -2000' saturates to top-left, then move absolute-ish
# in 320x200 guest coords. Screenshots are 640x400 (2x). QEMU runs -snapshot.
```

Desktop keyboard nav: arrows move selection (icon 0 pre-selected), Enter
launches, ESC exits most apps. Boot takes ~15s (floppy I/O).

Full audit machine-readable output (140 findings JSON):
`C:\Users\arin\AppData\Local\Temp\claude\C--Users-arin-Claude\ebca5af6-d681-4dec-93ab-c9df3e1d51d9\tasks\w0g4rduib.output`
(temp dir — the digest in docs/ contains everything important).
