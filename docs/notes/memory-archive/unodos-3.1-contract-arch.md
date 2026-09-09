# UnoDOS 3.1 "Contract-Driven Architecture" redesign: the agreed design doc, locked decisions, and the phased plan whose next step is Phase 0 (author UNODEF)

Archived verbatim on 2026-09-08 from the Claude Code memory file `unodos-3.1-contract-arch.md`. This is dated session history kept for reference. Later sections supersede earlier ones, so read bottom-up for the current state. The durable facts now live in the memory file itself and in the repo docs.

---


Major architecture redesign for UnoDOS, agreed at design level (2026-06-15), doc
at `docs/CONTRACT-ARCH.md`. Ships as the **UnoDOS 3.1** ABI — a versioned clean
break to the *call ABIs and `.UNO` container* only (on-disk DATA formats stay
byte-identical); name stays "UnoDOS 3" (uno-dos-tres pun).

Core idea: invert PORT-SPEC from "prose extracted from x86" into a single
machine-readable Contract (**UNODEF**); a host tool (**unogen**) emits per-world
stubs/tables/struct-equates/constants/docs/conformance from it. Four layers:
L0 Contract (orthogonal) · L1 mechanism+drivers · L2 portable policy · L3 apps;
L0.5 bus/driver layer on bus-rich targets. The universal boundary is **tall**
(software floor + optional high-altitude hardware overrides) — the uno3d/unoui
vtable pattern generalized, now with an **asm realization** (generated jump table,
e.g. Amiga $77000) not just C fn-ptrs. Unit of "world" = the SUBSYSTEM not the
port: generated tables everywhere, shared-C policy only where a CPU compiler + RAM
fit (Amiga 68K yes, NES/GB no).

Subsystems on that boundary: uno2d, unoui(exists), uno3d(exists), unosound
(voice/score floor NOT pcm), input/game-input, networking, unofs (the worked
example — storage, chosen over audio), and **unosched** = tiered concurrency
(COOP floor → PREEMPT → SMP → SPU/DSP OFFLOAD); sync primitives free on COOP, real
on SMP; jobs = uno3d-pattern-for-compute. Plus: runtime driver/bus model
(enumerate→bind→register, drivers implement the service vtables), multi-surface
display (DS dual-screen, Virtual Boy stereo), and 3 Contract profiles
(windowed/single_app/minimal) scaling 2KB NES → 256MB PS3.

LOCKED decisions: x86 DEMOTED (definition→first-consumer+conformance-oracle);
clean break = 3.1; drivers STATIC-LINKED first (loadable deferred); concurrency
pilots = **Saturn dual-SH-2 (SMP)** + **PS3 SPUs (OFFLOAD)**; worked example =
unofs; verification stays host-first incl. a pthreads+ThreadSanitizer host SMP
oracle for parallel correctness.

Targets in scope (existing + proposed): the 8 shipped ports plus GameCube, NES
(+FDS+Family BASIC kbd), VIC-20, PowerPC Mac, PS3, Xbox-original, Saturn, SMS,
GB/GBC, GBA, DS, PSP, Vita, Virtual Boy. Most new ones are C-world via SDK; only
NEW asm family added is Z80 (SMS/GB).

Phase 0 STARTED (2026-06-15): authored the first UNODEF at **`unodef/unodef.toml`**
(+ `unodef/README.md`). Format chosen = **TOML** (not the §3.1 bespoke DSL) — Python
stdlib `tomllib` parses it, no hand-written parser to drift (kills a §17 risk); one
tiny field mini-grammar `"name : type @offset [le|be]"` for struct fields. Encodes
the CURRENT shipping x86 surface, no behavior change, transcribed from
kernel/kernel.asm > PORT-SPEC > API_REFERENCE. Contents: all **106** call-gate slots
(ordinals 0..105 — resolves the "105 vs 106": 105 was count-vs-last-ordinal off-by-
one; ALL 106 now verified=true with full register sigs — ords 91-100/102/104, which
API_REFERENCE leaves "pending", were transcribed from their kernel.asm handler-header
comments); structs win_entry(32)/event(3)/file_handle(32,
owner@24 — kernel's own data comment "12-31 reserved" is stale drift)/dirent(32)/
bin_header(80); const.fat12 (the 5-places geometry unified: fs_start=110, fat=111,
root=129, data=143); const.font (advance=8 fixes the 12px bug); palette; enums
event_type/win_state/win_flags/fs_errno; profiles + x86 port decl; and a
`[discrepancies]` ledger. File validated: parses, ordinals contiguous 0..105.

Phase 1 MVP STARTED (2026-06-15): **`unodef/unogen.py`** reads unodef.toml and emits
the per-world contract SHAPE (ordinals, struct offsets/sizes, FAT12 geom, font
metrics, palette, enums — never bodies/logic, per §3.2) into `unodef/gen/<world>/`
for the FIRST 5 WORLDS = the CPU families covering every shipped port: x86/NASM
(gen/x86/unodef.inc), C core (gen/c/unodef.h, packed structs + _Static_asserts),
68000/vasm (gen/m68k/unodef.i → amiga/genesis/macplus), 6502/dasm (gen/6502 →
apple2/c64), 65816/ca65 (gen/65816 → snes/iigs). Same symbols, 5 dialects.
TRUST ANCHOR: `unogen.py --check` asserts 22 generated x86 constants == kernel.asm
literals (struct sizes, five-places FAT12 geom, advance=8, count=106) — PASSES.
Then EXTENDED (commit aaf382c): +6th world **Z80/sjasmplus** (gen/z80, for planned
SMS/GB — §13's one new asm family); +call-gate **app stubs** (gen/<world>/unosys.*,
a macro per syscall) generated ONLY for worlds with a declared [callgate.binding.*]
= x86 (INT 0x80/AH) + m68k (TRAP#0/D0.hi); 6502/65816/z80 SKIPPED with a printed
note (their call ABI isn't in the Contract — inventing one would break single-
source); +world-neutral **gen/manifest.json** (conformance/doc snapshot, §3.2 last
row). Generated includes NOT yet wired into the ports (no behavior change yet).
PHASE-1 TRUST ANCHOR PROVEN (commit 02f44e9): kernel.asm now `%include`s
unodef/gen/x86/unodef.inc and DROPPED its hand-written equate blocks (EVENT_*,
FS_ERR_*, WIN_*/WIN_OFF_*/WIN_STATE_*/WIN_FLAG_*, FILE_MAX_HANDLES/FILE_ENTRY_SIZE,
FONT_COUNT/FONT_DESC_SIZE) + both in-kernel FAT12 literal sites (mount routine + BPB
defaults). All 38 colliding syms verified == generated before removal; FAT12
immediates → equal-valued Contract exprs (FAT12_NUM_FATS*FAT12_SECTORS_PER_FAT=18,
DIRENT_SIZE=32) that NASM folds to identical bytes. REBUILD BYTE-IDENTICAL: `nasm
-f bin -Ikernel/` (build_info.inc held constant) → SHA256 04b9709a..., == committed
build/kernel.bin. The generator is now the verified source of truth.
Build: nasm at ~/AppData/Local/bin/NASM/nasm.exe (only python is on PATH in shells);
build_info.inc embeds the build number so byte-identical tests must hold it constant
(call nasm directly, not `make`, which regenerates+bumps it).
PHASE 2 DONE (commit 787a6bf): **unodef/conformance/conformance.py** — PORT-SPEC §6
audit-tax invariants made executable, host/stdlib-only, 29/29 pass. Structural checks
(rules 8/9/10 vs Contract+gen+kernel, incl. re-asserting the trust anchor) + behavioral
reference models w/ golden vectors (rule 6 z-order, 3 focus-stamp/stale-discard, 10
fixed-size tombstone queue, 5 edge-only mouse, 7 owner-reaping). Each behavioral
invariant paired with its HISTORICAL BUG; runner asserts ref passes all + buggy fails
>=1 (vectors discriminate). Not host-testable: rule 1 / part of 2 (HW timing); rule 4
vectors TODO.

KEY FINDING (2026-06-16, while probing "wire all worlds"): the non-x86 ports ship
GENUINELY DIVERGENT ABIs, not just different names — ALL asm ports use WENT_SIZE=16
(window entry 16B, not the contract's 32B from x86), 4-byte event records (not 3B),
Amiga has only 25 API routines with totally different ordinals (draw_string=0,
fat_mount=14), IIGS DIRENT_SZ=16 + its own FAT12 geom (SPC=2/SPF=3/start=256). So
"wiring" other ports to the x86-derived contract = a behavior-CHANGING ABI migration
(Phase 12), NOT a byte-identical no-op; doing it wholesale breaks 7 shipped ports.
Toolchains ARE reachable for asm ports (vasmm68k_mot ~/amiga-tools, ca65/ld65
~/snes-tools/bin, dasm ~/apple2-tools) but NOT the C consoles (need WSL KOS/ee SDKs;
only generic WSL gcc present). Decision on multi-world-contract vs unify-now deferred
(user dismissed the question, chose to follow the original phase order instead).

PHASE 3 DONE (commit daedb48): **unofs/** — storage worked example. Portable C
Layer-2 policy `unofs_core` (mount/readdir/open/read/close/create/write/delete,
cluster-chain walk, 12-bit FAT parse, consecutive-cluster batching, owner-based
reaping) over the Layer-1 `block` service (unofs.h iface), driven entirely by the
generated Contract (unodef/gen/c/unodef.h) — no magic numbers. block_file.c = host
file-backed backend; unofs_test.c = harness. HOST-VERIFIED via `sh unofs/build.sh`
(WSL gcc): mounts the REAL shipping floppy build/unodos-144.img and reads
CLOCK(2)/SYSINFO(3)/TEXT(13)/PAINT(62-clusters,batched) out BYTE-IDENTICAL to their
build/*.bin; owner-reaping + create/write/remount/re-read/delete round-trip all PASS,
exit 0. build/unofs_test{,.exe}+scratch gitignored.
Build note: WSL gcc compiles it; `gcc -std=c11 -Wall -Wextra -I unodef/gen/c -I unofs`.

Commits: 55d5a7f (UNODEF), ae5c9e3 (all 106 sigs), ebb1ed9 (unogen+5 worlds),
aaf382c (z80+stubs+manifest), 02f44e9 (kernel byte-identical), 787a6bf (conformance),
daedb48 (unofs Phase 3); legacy restore point = tag **legacy-pre-3.1** (2f0f261).
ALL 13 PHASES ADDRESSED (2026-06-16, "finish all phases autonomously"). Status ledger
= **unodef/PHASES.md**. Full sweep green: gen trust anchor OK, conformance 38/38, x86
AND Amiga kernels BYTE-IDENTICAL, host C suites unofs/uno2d/unosound/unobus/unonet/
unosched all PASS.
- Fully host-proven (8): P0 UNODEF, P1 x86 byte-identical, P2 conformance, P3 unofs,
  P4 Amiga byte-identical via [world.amiga] (multi-world contract = "describe what
  ships"), P6 uno2d (accel==floor pixel-identical), P7 concurrency (COOP==SMP, TSan
  oracle via `setarch -R`), P9 unosound (A440 ±3%, WAV).
- Host core + blocked tail (5): P5 (unofs_core freestanding-clean; vbcc/WinUAE blocked),
  P8 (multi-surface+profile manifest+directional-focus; NES/GB emulator blocked),
  P10 (uno_job OFFLOAD floor/accel equiv; Saturn-SH2/PS3-SPU blocked), P11 (unobus
  registry+FDS detect-pin; PCI/USB blocked), P12 (3.1 categorized ordinals + .UNO v2
  header, additive; port re-issue future), P13 (nic loopback+HEADLESS server, Z80
  equates generated; console SDK backends blocked).
New top-level subsystems added: uno2d/, unosched/, unosound/, unobus/, unonet/, unofs/
(+ unodef/ contract). NOTE divergent-ABI finding stands: ports use 16B windows/4B
events/own ordinals — Phase 4 resolved it via per-world [world.*] equates; full port
migration to one ABI is Phase 12's future tail. Toolchains: nasm+vasm+ca65+dasm
reachable, WSL gcc+TSan(needs setarch -R), vbcc+console SDKs+emulators NOT.
Commits 55d5a7f..b219184 on master; legacy restore = tag legacy-pre-3.1 (2f0f261).

PHASE 4 BROADENED (2026-06-16, commit 939ebba on master): asm consumption now spans
**5 ports across 2 dialects** — vasm 68K = Amiga + **Genesis** + **MacPlus**, ca65 65816 =
SNES + **IIGS**. Added [world.genesis]/[world.macplus]/[world.iigs] to unodef.toml; each
port now `include`s its generated unodef/gen/<port>/ equate file (genesis/sysabi_gen.i,
macplus/sysabi_gen.i, iigs/sys_gen.inc) in place of its hand-written window/event block.
ALL byte-identical incl. disk apps + packed images (genesis unodos.gen; macplus kernel+
boot+9 apps+.dsk; iigs kernel+8 .APP+.po, harness reproduced same 180587-instr render).
MacPlus seam: the gen file is shared by BOTH kernel.asm AND sysequ.i (apps) — kills the
former duplicate; its build-variant SCRW/SCRH `ifnd` guards stay port-side (excluded from
contract). IIGS ALSO single-sources its DIVERGENT FAT12 geom (FS_START_BLOCK=256/SPC=2/
SPF=3/NFATS=2/ROOT@7/DATA@14) + DIRENT_SZ=16. Port UI policy (attrs/timers/icon counts/
cell screen dims) kept hand-written. Sweep stayed green: unogen --check OK, conformance
39/39, x86 kernel still 04b9709a, existing amiga/snes gen files byte-unchanged. Toolchains
used: vasm ~/amiga-tools, ca65/ld65 ~/snes-tools/bin (build via each port's build.sh from
its own dir; include path "../unodef/gen/<port>/..."). REMAINING asm tail = 6502 ports
(C64/Apple II) — no clean equate seam, deeper refactor deferred. Migration doc Next-
direction #1 marked DONE; #2 (decide canonical 3.1 ABI + pilot Phase 12 on one port) and
blocked-toolchain tails (#3 vbcc/console SDKs) remain. NOTE: stale uncommitted edits to
dreamcast/dc_modload.c + ps2/ee_modload.c sit in the working tree (pre-fork legacy-port
experiment, untouched, NOT part of 3.1).

6502 PORTS + LEGACY COMMIT (2026-06-16, commits 75d21b9 + a89744f on master; a9c46f8 on
unodos-3-legacy): asm consumption COMPLETE for ALL 7 reachable-toolchain asm ports (added
dasm 6502 = C64 + Apple II to the vasm/ca65 five). KEY FINDING: the feared "deeper refactor"
was the wrong frame — C64 (M3 full-screen launcher) + Apple II are architecturally single_app
(one disk-loaded app at a time, NO window-entry struct, NO event queue — keyboard polled
synchronously). Forcing the windowed WENT/EV_* ABI on them would FAKE an ABI they don't ship.
Done honestly: [world.c64]/[world.apple2] source ONLY the genuine overlap = cell screen
geometry (SCRCOLS/SCRROWS) into unodef/gen/{c64,apple2}/sys_gen.inc (dasm EQU dialect),
included by each port's sys.inc; [port.c64]/[port.apple2] declare profile=single_app+caps+
surface so the profile manifest GENERATES their no-WM shape; conformance rule 9 extended with
single_app honesty (one_app + requires-no-WM) → now 43/43 (was 39). BYTE-IDENTICAL: c64
kernel+10 apps+prg+d64; apple2 kernel+boot+8 apps+dsk. They use USV1 mini-FS (NOT FAT12) so FS
geom stays port-side. LEGACY: the stale dreamcast/ps2 working-tree edits (dc_modload.c cache-
coherency fix dcache_purge+icache_inval after elf reloc + debug-scaffold removal; ee_modload.c
printf cleanup; shots) were committed to the unodos-3-legacy branch (a9c46f8), NOT master.
GOTCHA fixed: repo has core.autocrlf=true and .gitattributes pinned only *.asm/*.sh/*.py to
eol=lf — the gen tree (.inc/.i/.h/.txt/.json) churned LF↔CRLF on every unogen run; fixed by
pinning `unodef/gen/** text eol=lf` (commit a89744f). master is now PUSHED to origin
(hmofet/unodos-3, HEAD 3e78a01, 2026-06-16; unodos-3-legacy may still be local-ahead).
Toolchains: dasm at ~/apple2-tools
(shared by both 6502 ports + the 68K? no — vasm ~/amiga-tools, ca65 ~/snes-tools/bin, dasm
~/apple2-tools). Window-entry ABI unification (32B vs 16B) still Phase 12's open call.

GREENFIELD WINDOW MODEL — the 3.1 window-ABI decision (2026-06-16, commits 22e4743 +
6761ce7 on master). User asked which window ABI is objectively cleanest across the 2KB→
256MB spectrum (vs flat-32B / flat-16B / tiered). ANSWER ADOPTED (RFC, not yet engine-
wide): no canonical struct — a LOGICAL model + per-platform DERIVED physical layout +
generated zero-cost accessors keyed by an integer HANDLE (the tall-vtable pattern applied
to DATA). Artifacts: unodef/unodef.toml [wmodel] (fields+kind+tier, z-order as a RELATION
not a field, invariants; NO offsets/size) + [wmodel.platform.*] descriptors; unodef/
wmgen.py (derives layout, emits storage+accessors; SoA floor = free 8-bit indexing + SIMD
ceiling, AoS = a layout knob); unodef/gen/wm/<plat>/; unodef/WMODEL.md (the RFC+verification
+legacy delta). VERIFIED: C SoA compiles/runs, -O2 = pure indexed loads (zero-cost boundary);
dasm assembles 6502 macros (single lda/sta, no stride mult); vasm assembles 68000 macros
(u16 index-double, u8 direct); C AoS _Static_assert checks gen offsets vs compiler. Conformance
+9 `rule wm` checks (reap+zorder-total run THROUGH accessors over the SoA realization, layout-
independent, w/ discrimination) → 52/52 (was 43). KEY FINDINGS: (1) SoA is the better FLOOR
than AoS (free indexing on multiply-poor 8-bit + SIMD on big). (2) clean model in AoS = 14B
vs legacy win_entry 32B (inline char[12] title was 12 of those bytes) — the COMPACT 16B ports
(macplus etc.) ARE the greenfield model in AoS form; x86's 32B inline-title is the outlier.
WIRED TO A REAL PORT byte-identical: macplus/kernel.asm window ADDRESSING (win_ptr_raw +
zwin_ptr) now uses a wmgen-generated `win_entry_ptr` macro ([wmodel.platform.macplus] AoS
ptr32 word-align → WIN_ENTRY_SIZE=16, offsets match [world.macplus]); kernel.bin c6824c71…
+ .dsk a479556a… UNCHANGED. So macplus's whole window boundary is now Contract-generated.
ENGINE-WIDE ADOPTION DONE (2026-06-16, commits 868af03 + ec5ecdf): ALL 5 windowing ports now
source window ADDRESSING from [wmodel] via wmgen, byte-identical (field offsets already came from
[world.*]; addressing was the last hand-written piece, so each port's whole window boundary is now
Contract-generated). vasm AoS emitter → win_entry_ptr macro (lsl#4/lea base): amiga+macplus use
wintab(pc), genesis uses absolute VARS+v_wintab — base mode is just a descriptor field
(entry_base+pc_relative). ca65 AoS emitter → win_index_to_x macro (asl×4+tax) for snes+iigs
(replaced ent_x/zent_x); pad_pow2 forces the 16B power-of-two stride; max_align handles 68000
word-alignment. Byte-identical verified: amiga.exe, genesis.gen, snes.sfc, iigs kernel+.po, macplus
kernel+.dsk. AMD64 ADDED ([wmodel.platform.amd64]) = the 64-bit-C SoA realization (= host + bigger
cap=256); it expands the host/C pattern NOT the 16-bit-realmode x86 (AoS-16) pattern — descriptor-
only, compiles+runs; the new work for a FULL amd64 port is L1 mechanism (long mode/64-bit call ABI/
paging/GOP), orthogonal to the window model; only new window surface = OPTIONAL SIMD-composite accel
behind the same accessors. 9 platforms now derived from one logical model (c64 66B u8 → amd64 5376B
u64). wmgen.py grew vasm-AoS + ca65-AoS emitters. Gitignore: added global __pycache__/.
FULL FIELD ACCESS + ARCH SURVEY + PUSH + SoA MEASUREMENT (2026-06-16, commits 5298817 + 0b342f9;
PUSHED). (1) Full field access: C worlds emit get/set for every field + z-relation + a WRITE-ONCE
policy lib (win_hit/move/resize/raise/topmost_at/reap) — compiles+runs (host); asm worlds emit
WIN_<FIELD> offset equates + win_ld_/win_st_ (vasm) + win_lda_/win_sta_ (ca65) + WIN_CAP — assemble;
wired ports keep offset-direct for fused ops (offset IS the natural asm accessor). All 5 ports still
byte-identical (new symbols unused). (2) ARCH SURVEY (unodef/gen/wm/ARCHITECTURES.txt): 17 platforms
from one logical model — added spec_only descriptors arm/arm64/sparc/alpha/mips/ppc/riscv/sh4 + an
`endian` field. Endian is METADATA only (in-mem access native; .UNO/on-disk stays LE, BE ports
byteswap at that boundary; portable policy never sees it); fixed m68k ports to BE. spec_only avoids
redundant near-identical 64-bit-C headers. KEY framing: per-arch NEW work = L1 mechanism (boot/call-
gate/MMU/framebuffer), NOT the window model. (3) PUSHED: master→origin (hmofet/unodos-3, now at
0b342f9), unodos-3-legacy→legacy (hmofet/unodos-3-legacy at a9c46f8). Both remotes up to date.
(4) SoA-vs-AoS MEASURED on snes (65816): AoS geom-read routine 29B/55cyc vs SoA 26B/49cyc — SoA saves
3 of 4 index-shifts (−3B, −6cyc ~11%/access); field reads identical; runtime emulator-blocked.
VERDICT: SoA win is MARGINAL on tiny micro tables (cheap pow2 AoS stride) — its real value is the
big-machine SIMD/cache ceiling + non-pow2 free indexing. So keep AoS for micros, SoA floor for C/big
targets — validates the derived-layout design (each platform gets its optimal shape from one model).
X86 FULL PORT ON NEW ARCH — QEMU-VERIFIED + REAL-HW IMAGE (2026-06-16, commits a5f3d0d + f59e724,
PUSHED). QEMU IS AVAILABLE: C:\Program Files\qemu\qemu-system-i386.exe (off PATH) + harness
tools/qemu_test.py (monitor-driven: wait/moveto/dblclick/shot/quit, 320x200 coord space, env QEMU
points to the exe; -M isapc -m 640K -snapshot). Baseline: current x86 boots to desktop + WM works
(opened Sys Info window, launched Clock) — build 425. Then WIRED x86 window index->address arithmetic
to the wmodel: added nasm AoS emitter (emit_nasm_aos) + [wmodel.platform.x86nasm] (entry_size=32
override → shift 5, entry_base=window_table, shift_macro=SHL_N) → win_entry_addr macro; replaced 38
inline `SHL_N reg,5 + add reg,window_table` sites in kernel/kernel.asm (%include after unodef.inc
line 17). BYTE-IDENTICAL proven by before/after build with same build_info.inc (e433e02b; can't
compare to old 04b9709a because `make` bumps build_info.inc — compare HEAD-vs-working instead).
QEMU re-verified post-wiring (desktop+sysinfo+clock identical). So x86 = FIRST FULL PORT on the new
architecture ready for real hardware: contract-driven constants/structs + wmodel window addressing,
byte-identical, booting in QEMU. Image=build/unodos-144.img (1.44MB floppy, `make floppy144`); CONFIRMED BOOTING ON REAL
HARDWARE by the user (2026-06-16) — the new-architecture x86 port is real-hw validated. Boot
guide docs/RUN-X86-REAL-HARDWARE.md (Gotek/real floppy/Rufus-DD/QEMU; needs 386+ real-mode, PS2 or
COM1 serial mouse). ALL 6 windowing ports now wmodel-driven for addressing. NOTE: x86 still uses the
shipping [struct] win_entry 32B LAYOUT (only ADDRESSING is wmodel-wired); the greenfield CLEAN layout
(pointer titles, z as relation) is the future 3.1 behavior-changing break, intentionally NOT shipped
(can't runtime-verify the clean break without real hw; the title inline->pointer + 21 zorder sites are
the deltas). Build gotcha: `make floppy144` needs nasm on PATH (~/AppData/Local/bin/NASM).
X86 3.1 CLEAN LAYOUT SHIPPED (2026-06-16, commits 7b0b977 + f297472 + 08c73e7, PUSHED). The real
3.1 window clean-break, live on x86, QEMU-verified, in 2 increments: (1) TITLE inline char[12] ->
near POINTER into a new kernel title pool (win_title_pool, 16B/handle slot @ seg 0x1000; win_create
copies there + stores ptr in entry.WIN_OFF_TITLE; both titlebar draw paths load the ptr). Removes
the 11-char limit. (2) COMPACT [struct] win_entry 32B -> 16B in the Contract: state@0 owner@1 x@2
y@4 w@6 h@8 title@10(u16 ptr) zorder@12 flags@13 content_scale@14 (vs legacy flags@1/zorder@10/
owner@11/title-char[12]@12/cscale@24). KERNEL.ASM NEEDED ZERO CODE EDITS — it uses WIN_OFF_* +
WIN_ENTRY_SIZE symbolically + the win_entry_addr macro, so changing the Contract struct + setting
[wmodel.platform.x86nasm] entry_size=16 (stride shl5->shl4) flips everything on regen. window_table
512B->256B. z-order KEPT IN-ENTRY (zorder@12) as a valid per-platform realization of the z relation
(entry pads to 16B for shl4 stride regardless, so side-list = no size win + high risk; deferred).
Trust-anchor KERNEL_LITERALS updated (WIN_ENTRY_SIZE 16, WIN_OFF_OWNER 1, WIN_OFF_CONTENT_SCALE 14).
VERIFIED: trust anchor OK, conformance 52/52 (count check unaffected), C _Static_assert(sizeof==16),
unofs compiles, ALL 5 other asm ports STILL BYTE-IDENTICAL (they use [world.*] not WIN_OFF_*, so the
shared struct change doesn't touch them); QEMU full WM (boot, open SysInfo, close, reopen Clock, DRAG
window, z-order Settings-over-SysInfo, 3 distinct pointer-titles). x86 = FIRST PORT shipping the 3.1
clean window layout. file_handle/dir entries stay 32B (only window entry changed). New image build/
unodos-144.img sent to user. KEY TECHNIQUE: because the kernel was already fully symbolic + macro-
addressed (from the earlier wiring), the layout break was a regenerate-only contract change + 4 title
edits — the contract-driven architecture made the ABI break nearly free. NEXT possible: z-order
side-list realization if desired; migrate other ports to clean layout; or C-world OS on amd64/arm64.

CGA CURSOR FLICKER FIXED (2026-06-16, commit cbb079c, pushed): user reported cursor flicker in CGA
mode on a Dell Latitude 7280 (modern laptop LCD emulating CGA in a framebuffer -> NOT real CGA snow,
which can't occur there). Root cause: CGA (video_mode 0x04) used an XOR cursor (cursor_xor_sprite)
while VGA(0x13)/VESA(0x01) use opaque save-under (cursor_save_and_draw_vga/vesa) -> XOR inverts the
bg, shimmering over the dithered desktop = "flicker". FIX: added cursor_save_and_draw_cga +
cursor_restore_cga (per-pixel save-under over CGA interlaced 2bpp via cga_pixel_calc which returns
DI=byte off + CL=shift; trashes AX/BX/DX; scratch vars _cga_csr_* near cursor_save_buf which is 448B/
reused). mouse_cursor_show/hide dispatch 0x04 -> save-under; mode12h(0x12) keeps XOR (planar, scoped
out). QEMU-verified clean opaque arrow + no trail + bg restored; flicker-gone confirmed by user on
real hw. KEY: 8086 has `shl reg,cl` (variable) but NOT `shl reg,imm8` (hence the SHL_N/SHR_N macros).
QEMU x86 testing: qemu-system-i386 at C:\Program Files\qemu (off PATH); tools/qemu_test.py <img>
<artdir> 0 reads stdin cmds (wait N / moveto X Y / dblclick / click / btn 1|0 / shot NAME / quit;
320x200 coord space); make floppy144 needs nasm on PATH (~/AppData/Local/bin/NASM); default video
mode is CGA 0x04. SendUserFile tool is intermittently available.

8088/XT VALIDATION (2026-06-16): the 8088 port = the SAME kernel.asm (x86 ref build), so the 3.1
clean layout + CGA cursor fix already apply to it. kernel.asm:7 has `cpu 8086` (nasm REJECTS 186+/
386+ opcodes) — so a successful `make floppy144` PROVES 8088 opcode-safety (the substantive 8088
risk). My cursor code is 8086-safe (shl reg,cl variable-shift is 8086; SHL_N/SHR_N macros exist for
immediate shifts which 8086 lacks). MartyPC (cycle-accurate 8088 + accurate CGA) is at ~/xt-tools/
martypc.exe with harness tools/xt/shot_xt.ps1, BUT its screenshot trigger is a Ctrl+F5 keystroke to
the FOCUSED GUI window → does NOT work in a non-interactive/RDP session (no PNG produced; also tends
to boot the bundled unodos-cf.vhd unless the floppy mount takes). So MartyPC visual capture is
blocked here (run it interactively for the screenshot); 8088-safety is instead proven statically via
cpu 8086. BlastEm (genesis) at ~/genesis-tools/blastem-win32-0.6.2. Z80 TOOLCHAIN INSTALLED (2026-06-16): sjasmplus 1.23.1 at C:\Users\arin\z80-tools\sjasmplus-1.23.1.win\
sjasmplus.exe (downloaded from github z00m128/sjasmplus releases; matches the contract's z80/sjasmplus
world). VERIFIED: assembles gen/z80/unodef.inc clean (0 errors; `sjasmplus --raw=out.bin in.asm`).
CPU-FIT NOTE for the planned Z80 port: SMS uses a TRUE Z80 -> sjasmplus + the existing z80 world fit
directly (recommend SMS as the first Z80 port). Game Boy uses the Sharp LR35902 (a Z80 *subset* +
GB-only opcodes like ldh) -> needs rgbds/rgbasm + a NEW "gbz80" dialect in unogen, NOT sjasmplus. Still
needed for an SMS port: an SMS emulator (winget has none obvious; BlastEm reportedly does SMS, or
Emulicious/MEKA/Genesis-Plus-GX) + from-scratch port engineering (SMS VDP ~TMS9918 256x192, Sega
mapper, memory map, controller) like the other ports. winget IS available (has AILZ80ASM + ez80 CEmu,
but wrong dialect for our z80 world).

RDP SCREENSHOT RELIABILITY (root cause + plan): capture fails under RDP when the harness uses GUI
FOCUS + SendKeys (e.g. MartyPC shot_xt.ps1 sends Ctrl+F5 to the focused window). RDP disconnect (or
minimize, or Windows foreground-lock blocking synthetic focus) tears down the rendering surface ->
keystroke never lands -> no PNG. QEMU works under RDP because qemu_test.py captures via the MONITOR
SOCKET (`screendump` to a file) — NO GUI. RELIABLE PATTERN = control-channel capture (socket/CLI/file),
never window focus. MartyPC has only the Ctrl+F5 GUI path (--headless does NOT auto-capture, verified),
so MartyPC captures need a LOCAL/active session (flagged via spawn_task task_5ef32ee4). General fixes
for GUI-only tools under RDP: stay connected (minimized-but-connected usually still renders), or install
a virtual-display driver so a rendering surface persists when disconnected. For routine RDP-safe x86
VISUAL checks use QEMU; reserve MartyPC for local cycle-accurate runs.

RDP CAPTURE SOLVED + 8088 VALIDATED (2026-06-16): built ~/.claude/tools/cc-capture.ps1 (focus-
independent GDI/PrintWindow capture: `-Out f.png` whole desktop, `-Out f.png -Window <substr>` one
window incl. background/unfocused via PW_RENDERFULLCONTENT — NO SetForegroundWindow/SendKeys).
Documented machine-wide in ~/.claude/CLAUDE.md (user-scope, loads in EVERY session). MartyPC capture
recipe that WORKS under RDP: Start-Process martypc.exe (windowed, NOT --headless/--full_screen which
both fail to render/capture) with `--machine-config-name unodos_xt --auto-poweron --no_sound -m
fd:0:"<img>"`; then ShowWindow(hwnd,9=SW_RESTORE) + SetWindowPos(...,SWP_NOACTIVATE 0x10) to un-
minimize+size the window WITHOUT focus-steal (martypc opens minimized/tiny under RDP otherwise; its
configured size is 1040x820 / toml [[emulator.window]] size); wait ~110s for cycle-accurate boot; then
cc-capture -Window marty. RESULT: build/xt/marty_boot.png shows the UnoDOS 3.1 desktop (clean 16B
layout + CGA cursor fix) rendering CORRECTLY on the cycle-accurate 8088 with accurate CGA — strongest
validation (vs 486-class QEMU). User installed a virtual display via winget VirtualDrivers.Virtual-
Display-Driver (for disconnected-RDP capture; may need its companion-app config to add the monitor).
Toolchains: gcc only in WSL (/mnt/c paths), not git-bash; dasm ~/apple2-tools, vasm ~/amiga-
tools, ca65 ~/snes-tools/bin, nasm ~/AppData/Local/bin/NASM.

FORKED INTO TWO REPOS (2026-06-16): the GitHub repo hmofet/unodos was RENAMED to
**hmofet/unodos-3-legacy** (PUBLIC; the shipped real-hardware-validated OS + ports,
frozen at 2f0f261; has branches master+unodos-3-legacy + tag legacy-pre-3.1; old
/unodos URL auto-redirects). A NEW repo **hmofet/unodos-3** (PUBLIC) holds the
forward contract-driven 3.1 line (full history + all 3.1 work, master HEAD 4265ecf).
The local working dir C:\Users\arin\Documents\Github\unodos now has remotes:
**origin = unodos-3** (forward), **legacy = unodos-3-legacy**. Handoff doc =
docs/UNODOS-3.1-MIGRATION.md (fork structure, status, open ABI-unification decision,
toolchain map, prioritized next directions, resume checklist). README has a two-lines
banner. NEXT SESSION resumes on origin/master (unodos-3); see the migration doc's
"Next directions" (broaden asm consumption to Genesis/MacPlus/IIGS; decide canonical
3.1 ABI then pilot Phase 12 on one port; reach blocked tails when toolchains avail).
NEXT = wire gen/x86/unodef.inc into kernel.asm (replace the hand-written equate
blocks + the five FAT12 literal sites) and prove a byte-identical x86 rebuild. Open question
still unsettled: how much of the job-kernel/OFFLOAD ABI to freeze in UNODEF vs leave
per-backend. See [[portage-test-infra]] for host build infra.
