# UnoDOS 3.1 — the Contract-Driven Architecture

**Status:** partially implemented. The Contract (`unodef/`) and the 3.1 window ABI now
ship across the reachable asm ports + x86 and the `pc64/` world (all generate from or are
checked against `unodef/`). The broader models described here — the runtime driver/bus,
the tiered concurrency/scheduler spanning a 6502 to the PS3 Cell, and the multi-surface
display model — remain the forward design being built out (`pc64/` is furthest along:
real `unobus`/`unonet` drivers, a NIC + TCP/IP stack, TLS, and 3D).
**Scope:** a redesign of how UnoDOS is *authored, generated, and verified* across
its many worlds; a rethink of the asm/C split; a write-once subsystem model that
still lets each machine exploit its custom hardware; a **tiered concurrency /
scheduler** model spanning a single 6502 to the PS3 Cell; a runtime
**driver/bus** model; a **multi-surface display** model; and Contract-declared
**capability profiles** so the OS scales from a 2 KB NES to a 256 MB PS3. Ships as
the **UnoDOS 3.1** ABI — a versioned clean break to the *call ABIs and the app
container*, while the name stays **UnoDOS 3** (uno · dos · tres).

Builds on: [PORT-SPEC.md](PORT-SPEC.md), [PLATFORM-COMPARISON.md](PLATFORM-COMPARISON.md),
[ARCHITECTURE.md](ARCHITECTURE.md), [UNO3D.md](UNO3D.md), [UNOUI.md](UNOUI.md).

---

## 1. The problem

UnoDOS today is **one prose contract, several worlds, many hand-maintained
realizations.**

[PORT-SPEC.md](PORT-SPEC.md) is the platform-independent law, but it is *prose
extracted from the x86 assembly* — "where this file and the x86 source disagree,
the source wins." That sentence is the whole problem in miniature: the contract
is a **secondary artifact** that drifts from an implementation, reconciled only by
a human noticing and editing markdown.

The cost is already visible where the docs admit it: the syscall API ("105
functions" in PORT-SPEC vs "106" in ARCHITECTURE.md — *they already disagree*),
the disk geometry ("these constants exist in FIVE places on x86"), and the `.BIN`
header are each duplicated by hand, per world, times every target. The font
advance is **contradictory in our own docs** (ARCHITECTURE.md "12 px"; PORT-SPEC
"advance = 8, the 12-px advance was a bug"). The event record, window-table entry,
and file-handle entry are specified as *English about byte offsets*, then
re-encoded by hand in six assembly kernels and the C core.

We already have the cure in miniature, twice: **uno3d** and **unoui** prove the
"portable core + swappable vtable" pattern — but both are *C-only* and explicitly
**do not serve the asm world**. The pattern stops where most targets live. And the
*tasking* model — cooperative round-robin — was sized for a single CPU; it has no
answer for the Saturn's two SH-2s, the PS3's Cell, or "run this machine as a
server."

The redesign must serve six co-equal goals:

1. **Single source of truth** for the ABI, structs, constants, geometry.
2. **Reduce the per-CPU asm duplication** — share whatever can honestly be shared.
3. **Extend write-once OS-wide**, into the asm world — *without* capping any
   machine at software-only performance (the Amiga must still use its blitter,
   copper, and Paula; the SID must still synthesize).
4. **Lower the cost of a new port** to a small, specified, *generated* surface.
5. **Scale across a ~100,000× hardware spread** — from a 2 KB NES to a 256 MB
   PS3 — and across dynamic, expandable hardware (PCI/NuBus/SCSI/USB).
6. **Exploit parallel hardware** — real threading/scheduling for games and for
   server use, on Saturn/PS3/Xbox/DS/multi-core, *without* breaking the tiny
   single-core ports.

## 2. Goals and non-goals

**Goals**

- Make the contract the **primary artifact**: one machine-readable definition
  (`UNODEF`) from which tables, stubs, struct offsets, constants, docs, and
  conformance tests of *every* world are generated or checked. **x86 is demoted**
  from "the definition" to "first consumer + conformance oracle."
- Reframe the asm/C split so the **unit of "world" is the subsystem, not the
  port** — "write asm only where it is required *or where it pays*."
- Generalize the uno3d/unoui vtable into one **tall** universal boundary (the
  *Primitive Vtable*): a mandatory software floor + optional high-altitude
  hardware overrides, with a C realization (fn-ptrs) and an asm realization
  (generated jump table).
- Define the full **subsystem catalog** (2D, 3D, audio, input, game-input,
  widgets, networking, storage, **scheduler**) on that boundary, each with
  explicit acceleration tiers.
- Provide a **tiered concurrency model** (cooperative floor → preemptive → SMP →
  heterogeneous offload) with a portable task/sync/job API correct on every tier.
- Add a runtime **driver / bus / service-registry** model so bus-rich machines
  discover and bind hardware — reusing the subsystem vtables as the driver ABI.
- Add a **multi-surface display** model (dual screens, stereo, widescreen,
  multi-monitor) and a **headless** capability for server builds.
- Formalize **capability profiles** (`windowed` / `single-app` / `minimal`) so
  scaling down to 2–8 KB machines is *declared and generated*, not ad hoc.
- Pay off ABI debt as the versioned **UnoDOS 3.1** break.
- Keep the **host-first verification culture** as the target count explodes —
  including a host oracle for *parallel* correctness.

**Non-goals**

- **Not** moving whole platforms between worlds. A 64 KB 6502 can't host the
  52 KB C core; we move *subsystems*, not ports.
- **Not** breaking on-disk **data** formats (Tracker songs, Theme presets, FAT12
  data interchange). The clean break is to *code ABIs and the container*.
- **Not** a lowest-common-denominator floor that caps hardware. The portable core
  is the *floor and fallback*, **never the ceiling** (§5).
- **Not** *forcing* threads or preemption anywhere. Cooperative scheduling stays
  the floor; the tiny ports pay nothing for the parallel tiers (§10).
- **Not** a heavyweight dynamic driver stack on fixed-hardware machines, and **not
  loadable drivers in 3.1** — drivers are **statically linked** first (§7); the
  model degenerates to "hard-bound services" at zero cost where there is no bus.
- **Not** rewriting the asm kernels in C. We shrink incidental asm (tables,
  equates), not essential asm (boot, ISRs, blitting, context switch).

## 3. The core abstraction: the Contract, and four layers

UnoDOS 3.1 inverts the relationship. Instead of *prose extracted from x86*, there
is one **machine-readable Contract** (`UNODEF`); every world — **including x86,
now demoted to first consumer + conformance oracle** — is generated from it or
checked against it.

Layer 0 is *orthogonal*: it doesn't sit in the runtime stack, it **defines the
boundaries** of the other three.

```
  Layer 3   APPLICATIONS  — .UNO modules; name no platform; couple ONLY to the
            ─────────────    Contract's call gate.
                  ▲   call gate (generated per world): INT 0x80 / TRAP / jump-table / fn-ptr
  Layer 2   POLICY  — portable logic: window manager, events, SCHEDULER, FS,
            ──────    UI toolkit, 2D, 3D pipeline, audio mix, bus enumeration.
                  ▲   Primitive Vtable + subsystem backends  (TALL: floor + optional accel)
  Layer 1   MECHANISM  — platform primitives & DRIVERS: framebuffer/present,
            ─────────    input source, block r/w, ticks, sound sink, NIC,
                         context switch / timer / core-start / barrier, boot.
                  ▲   (bus-rich targets only) bus enumerate → bind → service registry
  Layer 0.5 BUSES & DRIVERS  — PCI / NuBus / SCSI / USB / IDE enumerators; drivers
            ───────────────    bind device nodes and PUBLISH Layer-1 services.
  ════════════════════════════════════════════════════════════════════════════
  Layer 0   THE CONTRACT (UNODEF)  — the single machine-readable definition.
            ───────────  unogen ─► stubs · tables · struct/const equates (asm+C)
                                   · regenerated docs · conformance vectors · profiles
```

- **L3/L2 boundary = the call gate** (the syscall ABI apps use). Shape generated
  per world; numbering/semantics in `UNODEF`.
- **L2/L1 boundary = the Primitive Vtable + subsystem backends.** Layout generated
  from `UNODEF`; entries written (or bound) per platform.
- **L0.5** exists only on bus-rich targets; it *populates* L1 at runtime instead
  of L1 being hard-coded.

uno3d and unoui are Layer-2 policy modules; their internal vtables (rasteriser
backend, theme) are sub-vtables. UnoDOS 3.1 doesn't replace them — it explains
them as the first two instances of one pattern and gives that pattern an asm-world
realization.

### 3.1 What `UNODEF` contains (illustrative — not the implementation)

```
contract  { name = "UnoDOS", abi_major = 3, abi_minor = 1 }

syscall gfx.fill_rect {                     # the call gate, per call
    ordinal=0x12  category="gfx"
    params=[x:u16,y:u16,w:u16,h:u16,color:u8]  returns=void  errors=[CLIP]
    # per-world binding DERIVED: x86 AH=0x12 ; 68K D0.hi=0x12 ; C table slot 0x12
}

struct win_entry size=32 { state:u8@0 owner:u8@1 x:u16@2 ... z:u8@11 title:char[16]@12 }
struct event     size=3  { type:u8@0 data:u16@1 le }
struct dirent    size=32 { ... fat12 dir entry, fields LE ... }
struct uno_header size=80 { magic@0 abi_ver@? arch@? name:char[12]@4 icon@16 entry@80 }

const palette=[0x0000AA,0x00AAAA,0xAA00AA,0xFFFFFF]
const font.default={w=8,h=8,advance=8}      # the bug is fixed IN THE CONTRACT
const fat12={boot_lba=0,fat_lba=...,root_lba=...,data_lba=...,spc=1}

enum errno { OK=0, CLIP, NOMEM, BADHANDLE, ENOENT, EFULL, ENODEV, EAGAIN, ... }

service block { read(lba,n,buf) write(lba,n,buf) geometry() }   # the driver ABIs
service input { poll(event*) }
service fb    { present(surface*) | caps DIRECT_DRAW + px/fill/blit }
service nic   { send(pkt) recv(pkt) link() }
service audio { voice(ch,wave,freq,env) | mix_submit(pcm) }
service sched { ctx_switch(a,b) timer_hz() core_start(id,fn) barrier() yield() }

capability { SMP, PREEMPT, OFFLOAD, NET, RTC, HEADLESS, TOUCH, POINTER, STEREO }

profile windowed   { requires=[wm, mouse_or_dirnav, fs] }
profile single_app { requires=[fs?] one_app=true }
profile minimal    { requires=[] one_app=true no_fs_ok=true dirnav=true }
```

### 3.2 What `unogen` emits

| Output | x86 asm | per-CPU asm (6502/65816/68000/**Z80**) | C core |
|---|---|---|---|
| Call-gate **app stubs** | `INT 0x80` macro/call | jump-table call stub | inline wrapper / fn-ptr |
| Call-gate **dispatch skeleton** | `AH→handler` table | ordinal→vector table | table of fn-ptrs |
| **Struct offsets / constants** | `equ` / `%define` | assembler-specific `equ` | header `struct` / `#define` |
| **Primitive Vtable + service layout** | fixed-addr jump table | fixed-addr jump table | `struct` of fn-ptrs |
| **Profile + capability manifest** | which syscalls/services/caps this port must provide |||
| **Docs / conformance vectors** | regenerated tables · shared test data every world runs |||

`unogen` **never** emits syscall *bodies*, drivers, context-switch code, or CPU
logic — only the *shape* of the boundary (the mechanical, duplicated, drift-prone
parts). The meat stays hand-written (asm or shared C), per §4. This is the literal
cure: the "five places" become one constant; "105 vs 106" becomes `len(syscalls)`;
PORT-SPEC's tables regenerate; the font advance is fixed once.

## 4. The asm/C split, rethought: the subsystem is the unit

A *port* is no longer wholly one world. For each (subsystem × CPU) pick the
*lightest realization that fits*:

```
Layer-1 mechanism (pixels, sectors, ISRs, boot, hot blit, a sound chip's voices,
                   a context switch)?
   → hand-write it (asm on bare metal; C backend on hosted). Always.
Layer-2 policy (WM rules, event routing, FAT walk, layout, bus enumeration,
                scheduler run-queue)?
   ├─ usable C compiler for this CPU  AND  fits the RAM/segment budget?
   │     → compile the SHARED C policy for this CPU, link to the hand-asm vtable.
   └─ otherwise
         → hand-write policy in asm, but against GENERATED equates + struct
           offsets, verified by the SAME conformance vectors.
```

The win is **graduated and honest**:

| World / CPU | Generated tables & consts | Shared-C policy | Hand-asm |
|---|---|---|---|
| x86 (reference) | ✅ all | n/a | bodies, ISRs, boot |
| 68K — Amiga / MacPlus | ✅ all | ✅ where it fits (vbcc/gcc) | boot, blitter, copper, Paula, ISRs |
| 65C816 — IIGS | ✅ all | ⚠️ pilot (cc65/llvm-mos) | boot, SHR, device |
| 65816/6502 — SNES / Apple II / C64 / **NES / VIC-20** | ✅ all | ❌ RAM too tight | most policy + mechanism |
| 68K — Genesis | ✅ all | ❌ 64 KB total | most policy + mechanism |
| **Z80 — SMS / Game Boy** | ✅ all | ❌ tiny + new family | most policy + mechanism |
| C core — Mac/PS2/DC/**GC/PS3/Xbox/Saturn/PSP/Vita/DS/GBA/VB/PPC-Mac** | ✅ all | ✅ default | platform backend only |

So codegen pushes shared *shape* into every port (goal 2), shared *C policy*
migrates whole subsystems into the C world wherever they fit (goals 1 & 3), and
the only genuinely **new asm family** the huge target list adds is **Z80**
(SMS/GB) — everything else big is C-world via its SDK.

## 5. The Primitive Vtable — one *tall* boundary (floor + fallback)

The central design principle, and the answer to "write-once vs. maximum
hardware":

> **The portable core is the floor and the fallback — never the ceiling.** Every
> operation has a complete portable software implementation over the lowest
> primitive, so a new port runs *everything* by implementing only the bottom
> rung. Above that, a port overrides operations at the **highest altitude it can
> accelerate**, advertised by caps / NULL-fallback. The core calls the highest
> rung the backend offers and synthesizes the rest from lower rungs.

Both halves already exist: uno3d swaps at the *"rasterise this screen-space
triangle"* altitude (not "put pixel") with `u3d_backend_caps()` — that is what
lets GS/PVR run it in hardware while `soft` runs it in CPU; unoui themes have
**NULL-fallback painters**. UnoDOS 3.1 names this and makes it universal — *and it
applies to concurrency too* (§10): the cooperative scheduler is the floor, real
threads/SMP are the accel overrides.

Consequence for the asm/C split: a bare-metal **Amiga** port is *not* "thin
mechanism + generated tables." It is a **heavy backend** that overrides many
high-altitude ops in hand-tuned asm — blitter fills/blits, copper scroll &
per-scanline palette, Paula 4-channel DMA audio, hardware-sprite cursor — while
falling back to shared policy for everything it doesn't accelerate. "Asm where
required" gains its second meaning: **asm where it pays.**

Two realizations, one abstraction:
- **C**: a `struct` of function pointers — literally what `fb.h` + the unoui input
  adapter already are.
- **asm**: a **fixed-address jump table** whose slot layout `unogen` emits. This
  *is* the Amiga `$77000` table, the IIGS JMP-vectors, the C64 `$DE00` port, and
  x86's `INT 0x80` — recognized as one construct.

## 6. Subsystems: the catalog and acceleration tiers

Every subsystem = portable policy (L2) + a tall backend over the Primitive Vtable.
The mandatory **floor** guarantees write-once; the **accel overrides** are where
custom chips earn their keep (Amiga examples in **bold**).

| Subsystem | Mandatory floor (write-once) | Optional accel overrides | Status |
|---|---|---|---|
| **2D** `uno2d` (generalizes `fb.h`) | `present(surface)` / put-pixel | `fill/blit/copy` → **blitter**; `hscroll/vscroll` → **copper**; HW cursor sprite; tile/sprite engines | new — **do first** |
| **Widgets** `unoui` | portable painters over `uno2d` | per-theme native chrome (NULL-fallback) | ✅ exists — retarget onto `uno2d` |
| **3D** `uno3d` | `soft` CPU rasteriser | screen-space tri → GX/RSX/D3D8/GS/PVR/GE/SGX/VDP1 | ✅ exists — the reference |
| **Audio** `unosound` | **voice/score** model, SW-synth to PCM | voices → **Paula** / SID / DOC / SPC700 / PSG / SPU2 / AICA | new |
| **Input** (pointer/keys) | native → Contract `event`; **directional-focus** when no pointer | HW cursor sprite | ✅ partial |
| **Game input** | abstract digital `{l,r,u,d,fire,start}` + player index | analog axes, gyro/touch/rear-touch, rumble | partial |
| **Networking** | **absent by default** — a queried capability | per-link `nic` driver | last / optional |
| **Storage** `unofs` | `block` service + FAT12/mini-FS policy | DMA block, save-data slot model | worked example (§11) |
| **Scheduler** `unosched` | **cooperative round-robin** (today's model) | **PREEMPT** (timer) → **SMP** (N cores) → **OFFLOAD** (SPU/DSP jobs) | new (§10) |

**The audio-altitude trap.** A PCM floor (`mix → play`) is *wrong* for audio:
SID, Paula, Ensoniq DOC, SPC700, PSG are *synthesis* chips, not PCM DACs. So
audio's floor sits at the **voice/note/instrument** altitude; chiptune chips
realize it natively, PCM platforms software-synthesize into a mix buffer. The
**Tracker song format is already byte-identical everywhere** — the natural
write-once input that proves the altitude is right.

## 7. Drivers & buses: enumerate → bind → register (static-link first)

**A driver is a backend bound at runtime instead of compile time, implementing one
of the §6 service interfaces.** A SCSI disk publishes `block`; a PCI GPU publishes
the `uno2d`/`uno3d` backend; a USB keyboard publishes `input`. We invent **no new
driver ABI**. The only new machinery:

```
  bus enumerator ─► device nodes {bus, vendor:device, class, resources}
        │                 (PCI/AGP, NuBus, SCSI, USB, IDE/SATA)
        ▼
  driver binding (claim by class/id) ─► PUBLISH service ─► service registry
        │                                (block / fb / input / nic / audio)
        ▼
  Primitive Vtable slots filled FROM the registry  (instead of hard-coded)
```

Each bus enumerator is floor/fallback: a portable walk + a tiny per-platform
primitive (PCI config-space read/write — x86 `0xCF8/0xCFC`, PowerPC Mac
memory-mapped). **3.1 ships static-linked drivers only** — a port links the
drivers it knows it needs and the registry binds them at boot; **loadable driver
modules are deferred** (a `.uno`-style service module is a post-3.1 option, noted
in §17).

**Capability-tiered, so it costs the tiny machines nothing:**

| Target kind | Vtable filled by | Bus/driver cost |
|---|---|---|
| Fixed hardware (Genesis, GB, SMS, NES core) | hard-coded at build | **zero** |
| Optional peripheral (NES + **FDS**/Family BASIC kbd, **VIC-20** RAM expansion) | soldered "present?" detect | tiny — same question as a bus walk, answered by a pin |
| Bus-rich (PowerPC Mac, PS3, Xbox, GameCube) | runtime enumerate→bind→register | full model, linked only here |

Two payoffs: **drivers are write-once** (one USB-HID keyboard driver serves DC,
PS2, PS3, Xbox, Vita, GameCube; one FAT-over-`block` driver serves SCSI, IDE, and
memory cards alike), and **the model scales down** — the Famicom Disk System
publishing `block` is the *same* binding question as a SCSI controller, answered
by a detect pin instead of a bus walk.

## 8. Display & input: surfaces and modalities

**Multi-surface display.** The single-`fb` assumption breaks, so the display model
becomes *a list of surfaces*:

```
display { surfaces: [ {id, w, h, depth, role} ... ]  ;  active_for_wm: id }
roles:  PRIMARY · SECONDARY (DS bottom) · STEREO_L / STEREO_R (Virtual Boy) ·
        MIRROR · (none → HEADLESS server build, §10)
```

- **DS** — top PRIMARY + bottom SECONDARY (app-assignable, e.g. touch keyboard).
- **Virtual Boy** — `STEREO_L`/`STEREO_R`: two views of one scene, red-mono
  (re-uses mono theming); a parallax offset is the only extra policy.
- **PSP/Vita widescreen, PowerPC Mac multi-monitor** — more surfaces with sizes;
  `uno2d`/`unoui` derive geometry from a surface rect, so they scale unchanged.

**Pointer-less directional input.** GB/GBC, GBA, NES, Virtual Boy have **no mouse
or touch**. unoui's Tab-traversal generalizes into a **directional-focus** mode
(d-pad moves focus; A/B = activate/cancel); the cursor becomes an optional
capability. DS/Vita **touch** feeds the same `event` stream (touch-down = MOUSE
button), so WM logic is unchanged.

## 9. Capability profiles

A port declares one **Contract profile** plus a set of **capabilities**; `unogen`
emits the manifest of what the port must provide, making "this machine can't
window" *generated and honest*:

| Profile | RAM tier | WM | Pointer | FS | Targets |
|---|---|---|---|---|---|
| `windowed` | roomy (≥256 KB) | full z-order WM | mouse **or** touch | yes | x86, Amiga, IIGS, Mac, PS2, DC, GC, PS3, Xbox, Saturn, PSP, Vita, DS, PPC-Mac |
| `single_app` | tight (~32–128 KB) | none — one full-screen app | optional | optional | C64, Apple II, SNES, Genesis, GBA |
| `minimal` | extreme (2–16 KB) | none | **directional only** | often none | **NES (2 KB), GB/GBC (8 KB), VIC-20 (~5 KB), SMS** |

Capabilities (`SMP`, `PREEMPT`, `OFFLOAD`, `NET`, `RTC`, `HEADLESS`, `TOUCH`,
`STEREO`, …) cut across profiles — e.g. a `windowed` PS3 build advertises
`SMP+OFFLOAD+NET`, while a `windowed` Dreamcast *server* build sets `HEADLESS+NET`
and zero display surfaces. The `minimal` profile is where the 2 KB NES, 8 KB Game
Boy, and 5 KB VIC-20 force honesty: one app, no WM, directional input,
cooperative-only scheduling, storage only if a peripheral is present.

## 10. Concurrency: the scheduler, sync, and the job model

Cooperative round-robin was sized for one CPU. The Saturn (2× SH-2), PS3 (Cell:
PPU + 6 SPUs), Xbox, DS (2× ARM), and multi-core hosts are parallel; games want to
offload physics/AI/audio; and "run the box as a server" wants concurrent I/O. So
UnoDOS 3.1 adds a **tiered concurrency model** — designed by the same floor/fallback
rule as §5, so it costs the NES and Game Boy nothing.

> **Portable code is written to the cooperative floor and is therefore correct on
> every machine. A scheduler backend may upgrade execution to preemptive or
> parallel transparently. Code that *wants* parallelism uses an explicit job + sync
> API that runs in parallel on capable machines and degrades to inline / no-op on
> the rest.**

Three pieces, each split portable-policy / per-platform-mechanism like everything
else:

**(A) Task scheduler — tiers selected by the `unosched` backend.** The scheduling
*policy* (run-queue, priorities, fairness, yield points) is portable Layer-2 code;
the *mechanism* (`ctx_switch`, `timer_hz`, `core_start`, `barrier`) is the
per-platform Primitive-Vtable service.

| Tier | What it is | Mechanism needed | Targets |
|---|---|---|---|
| `COOP` (floor) | round-robin; switch at yield/event-wait (today's model) | nothing | **every** target, incl. NES/GB |
| `PREEMPT` | timer-tick preemption, one core | timer ISR + saveable context + per-task stacks | x86, 68K roomy, most C single-core |
| `SMP` | N cores, per-core run-queue / work-stealing | `core_start` + `barrier` + atomics | **Saturn (2×SH-2)**, **PS3 PPU**, Xbox, DS, host |
| `OFFLOAD` | dispatch jobs to specialized processors | a job-kernel ABI per accelerator | **PS3 SPUs**, Saturn SCU-DSP / sound CPU |

**(B) Synchronization primitives** — one API (`uno_mutex`, `uno_sem`, `uno_cond`,
`uno_atomic` CAS/add). On `COOP` they compile to **near-nothing** (no preemption
⇒ no race inside a yield-bounded critical section; atomics are plain ops) — **zero
cost on the tiny ports**. On `PREEMPT`/`SMP` they become real
test-and-set / LL-SC (SH-2), `lwarx/stwcx` (PPC), `ldrex/strex` (ARM) with memory
barriers. *App/game code uses the same calls everywhere*: they vanish on a 6502
and become real atomics on the Saturn.

**(C) Jobs & parallel-for — data parallelism and heterogeneous offload.** A **job**
is a portable C kernel + data submitted to a pool; `uno_parallel_for(n, fn)`;
await. This is **uno3d's pattern applied to compute**:
- Floor: jobs run **inline, serially, on the calling core** — correct everywhere
  (a "parallel_for" on a 6502 is just a loop).
- `SMP`: jobs distributed across worker tasks on other cores.
- `OFFLOAD`: a job kernel may carry an *optional* per-platform accelerated
  implementation (an SPU/DSP version) selected by the backend; absent one, the
  portable C kernel runs on the main core. So the audio mixer or a physics step is
  written once in portable C (runs anywhere) and *may* have a hand-tuned SPU
  version on PS3 — same write-once-with-accel contract as a rasteriser backend.

**The correctness contract (the genuinely hard part, stated plainly).** Code
written/tested only on the cooperative floor can hide races that surface under
real preemption/SMP. The rules that make "write for cooperative, run on SMP" safe:
1. **The kernel guards its own shared structures** (WM tables, event queue, FS
   handles) with the sync primitives — *real on SMP, free on COOP* — so a
   preempted/parallel task can never corrupt them.
2. **Apps share nothing implicitly.** A task that only yields at defined points and
   touches no shared app state behaves **identically** under any backend.
3. **Parallelism is opt-in**: code that wants it uses the job + sync API, which is
   real on capable tiers — so that code is correct *because* it used the
   primitives, not by luck.

**Server / headless use** is then composition, not new mechanism: non-blocking
`block`/`nic` completion variants + the scheduler's event-wait generalized to "wait
on I/O completion" + a worker pool over the job system + the `HEADLESS` capability
(zero display surfaces, §8). A request server is a `windowed`-or-not port with
`HEADLESS+NET+SMP`.

**Mapping to worlds/profiles:**

| World / profile | Concurrency tier |
|---|---|
| `minimal` (NES/GB/VIC-20/SMS) | `COOP` only — one app; sync = no-ops; jobs inline. Cost ≈ 0 |
| `single_app` (C64/Apple II/SNES/Genesis/GBA) | `COOP`; `PREEMPT` if a timer + stacks fit |
| roomy single-core (x86, Amiga 68K, DC, PS2 main) | `COOP` + `PREEMPT` |
| parallel C-world (**Saturn**, **PS3**, Xbox, DS, host) | `SMP` (+ `OFFLOAD` on PS3/Saturn) |

**Host-first verification of parallelism.** The host gets an **`SMP` backend
(pthreads) run under ThreadSanitizer** — a cheap, deterministic oracle for the
Saturn/PS3 parallel correctness *before* touching that hardware, exactly as the
software rasteriser is the oracle for GS/PVR. The conformance suite gains a
**concurrency stress pass**: run the same job/sync vectors under `COOP` and under
host-`SMP`+TSan and require identical results + zero data races.

**Pilots (confirmed):** **Saturn dual-SH-2 is the `SMP` pilot** — two symmetric
SH-2s over shared work RAM is the cleanest real-SMP forcing function and de-risks
`ctx_switch` / `barrier` / `core_start`. **PS3 SPUs are the `OFFLOAD` pilot** — a
distinct job-kernel model that validates tier (C).

## 11. The ABI apps use (the UnoDOS 3.1 clean break)

Apps couple to Layer 2 only through the generated call gate, now with a version
handshake. The 3.1 break pays off:

- **Categorized syscall ordinals** (append-only within a major): `0x00 gfx ·
  0x20 text · 0x30 window · 0x40 event · 0x50 fs · 0x60 task/sched · 0x70 ui ·
  0x80 audio · 0x90 system · 0xA0 net · 0xB0 device/driver …`. Count is
  contract-defined; per-world binding (AH / D0.hi / slot) is derived.
- **`.UNO` container v2** — explicit `abi_version` + `arch` tag (loader rejects
  mismatched binaries), every multi-byte field explicitly little-endian, the
  "first magic byte = x86 `JMP`" trick demoted to an optional x86 nicety. A `.UNO`
  declares its required **profile** and **capabilities** (e.g. `SMP`), so a
  `minimal`-only NES build won't launch a windowed or SMP-requiring app.
- **Font-advance fix** — `font.default.advance = 8` in `UNODEF`; ARCHITECTURE.md's
  "12 px" regenerates from the contract; the contradiction can't recur.

## 12. Worked example: `unofs` (storage)

**Why storage, not audio.** It is the only subsystem **mandatory on every
target**, carries the **byte-identical on-disk invariant**, is the **worst current
duplication** (geometry in five places), and **forces the pattern into the asm
world**. Audio has none of those three.

- **Contract:** FAT12 geometry as one constant block; 32-byte dir entry & 32-byte
  handle entry (owner at byte 24, kernel `0xFF`) as LE structs; C64 USV1 byte-heap
  and Apple II GCR mini-FS described alongside so *data* stays interchangeable.
- **Policy (`unofs_core`, L2):** mount/open/read/write/close/readdir; cluster-chain
  walk; FAT parse; **owner-based handle reaping on every kill path** (PORT-SPEC
  §6.7); **consecutive-cluster batching** (PORT-SPEC §5).
- **Mechanism (`block` service, L1):** x86 `INT 13h`; Amiga trackdisk; IIGS
  SmartPort; PS2 libmc; DC VMU; Apple II RWTS; C64 byte-heap; **NES Famicom Disk
  System** (published via the §7 detect path); on bus-rich machines, *bound from a
  SCSI/IDE driver via the registry*.

| World | `unofs_core` policy | `block` mechanism |
|---|---|---|
| C ports (PS2/DC/Mac/GC/PS3/Xbox/…) | compiled once, shared | C fn-ptrs / driver-bound |
| Roomy asm (Amiga/MacPlus 68K) | **compiled from the same C source** (vbcc) | hand-asm trackdisk/.Sony |
| Tiny asm (Apple II/C64/SNES/NES/VIC-20) | hand-asm vs **generated** equates + conformance | hand-asm RWTS / byte-heap / FDS |

**Host-first:** `unofs_core` links a *file-backed* `block` (a `.img` is a host
file); a generated conformance run mounts a golden FAT12 image, reads/checks bytes,
writes/re-reads, exercises reaping, diffs against golden — all on PC before any
emulator.

## 13. Targets × worlds × endianness

Most new big targets are **C-world via an SDK**; the only new *asm* family is
**Z80**.

| Target | CPU | Endian | World | Profile | Notable forcing function |
|---|---|---|---|---|---|
| x86 PC / XT | 8086+ | LE | x86 asm | windowed/single | reference (now demoted to oracle) |
| Amiga | 68000 | BE | asm (heavy backend) | windowed | blitter/copper/Paula accel |
| Genesis | 68000 | BE | asm | single | VDP cells, 64 KB |
| MacPlus-OS | 68000 | BE | asm | windowed | 1-bit dither |
| IIGS | 65C816 | LE | asm (+C pilot) | windowed | SHR, SmartPort |
| SNES | 65816 | LE | asm | single | bank-0 tight, SPC700 |
| Apple II / C64 / **VIC-20** | 6502/6510 | LE | asm | single / **minimal** | RAM floor (VIC-20 ~5 KB) |
| **NES** | 6502 (2A03) | LE | asm | **minimal** | 2 KB RAM; **FDS + Family BASIC kbd** = §7 optional-peripheral |
| **SMS / Game Gear** | **Z80** | LE | **asm (new family)** | single/minimal | new CPU family; PSG/VDP |
| **Game Boy / Color** | **LR35902** | LE | **asm (new family)** | **minimal** | 8 KB RAM, no pointer |
| **GBA** | ARM7TDMI | LE | C (devkitARM) | single | tight EWRAM, no pointer |
| **Nintendo DS** | ARM9/ARM7 | LE | C (devkitARM) | windowed | **dual screen + touch**; dual-CPU |
| **PowerPC Mac** | PPC 601–G5 | **BE** | C | windowed | **driver/bus** (NuBus/PCI/AGP/SCSI) |
| PS2 | R5900 | LE (64) | C | windowed | GS, libmc |
| Dreamcast | SH-4 | LE | C | windowed | PVR, VMU, BBA net; HEADLESS server |
| **GameCube** | PPC Gekko | **BE** | C (devkitPPC) | windowed | GX backend |
| **PS3** | Cell (PPU64 + SPUs) | **BE** | C (PSL1GHT) | windowed | RSX; **SPU OFFLOAD pilot**; USB/BT/GigE |
| **Xbox (original)** | Pentium III | LE | C (nxdk) | windowed | x86 **protected-mode**; IDE/USB/NV2A; SMP |
| **Sega Saturn** | dual SH-2 | **BE** | C (libyaul) | windowed | **SMP pilot**; VDP1 quads |
| **PSP** | Allegrex MIPS | LE | C (pspsdk) | windowed | GE 3D, widescreen, wifi, suspend |
| **PS Vita** | Cortex-A9 | LE | C (vitasdk) | windowed | SGX, front+rear touch, gyro |
| **Virtual Boy** | NEC V810 | LE | C (gccvb) | single | **stereo display**, red mono |

Endianness now carries weight: BE (PPC GC/PS3/Mac, SH-2 Saturn, 68K) will instantly
expose any sloppy multi-byte field, so the Contract's per-field LE tags +
boundary-only accessors are load-bearing.

## 14. Memory & performance budget on the smallest target

Floor: **NES (2 KB)**, **Game Boy (8 KB)**, **VIC-20 (~5 KB)**. Commitments:

- **Generated tables/stubs/equates = the same bytes** hand-typed today; an `equ`
  block is **0 runtime bytes**; app-side stubs ~3–6 bytes/call site. Net RAM
  delta ≈ **0**.
- **The Primitive Vtable on an asm port is the jump table it already uses.**
- **Shared-C policy is OFF** here (§4); **drivers degenerate to hard-bound
  services** (§7); **the `minimal` profile omits WM + FS** (§9); **the scheduler is
  `COOP` and sync primitives compile to nothing** (§10). The smallest targets pay
  nothing for features they can't host, and gain generated correctness +
  conformance for free.
- For roomy ports that *do* pull shared-C policy or a parallel tier, the budget
  gate is explicit and host-measured ("measure host-first, then enable").

## 15. Verification, host-first

Stronger, because the spec becomes executable.

1. **Host reference build** — `unogen` on PC emits the C header + x86 equates; C
   policy links a *host* Primitive Vtable (file-as-disk, PPM framebuffer, scripted
   input, loopback `nic`, **pthreads `SMP` scheduler**). Evidence = PPM/PNG + byte
   diffs. Same shape uno3d/unoui already use.
2. **Generated conformance = PORT-SPEC, executable** — §6 audit-tax invariants
   become test vectors; every world runs the subset its **profile** requires;
   **a concurrency stress pass** runs the job/sync vectors under `COOP` and
   host-`SMP`+ThreadSanitizer, requiring identical results + zero races.
3. **Then emulator, then hardware** — MartyPC/WinUAE/PCSX2/Flycast/Mesen2/py65 plus
   the new SDK emulators (Dolphin, RPCS3, xemu, Mednafen/Yabause, PPSSPP, Vita3K,
   melonDS, mGBA).

The meta-risk — "the *generator* could be wrong" — is answered by the Phase-1
byte-identical x86 rebuild (§16): the generator is validated against the known-good
implementation before anything depends on it.

## 16. Phased build plan (each phase host-verifiable, incremental, reversible)

- **Phase 0 — Author `UNODEF`** for today's surface (format; syscalls/structs/
  consts/enums/services/profiles/caps; fill the §4/§13 matrices). No behavior
  change; host-only.
- **Phase 1 — `unogen` MVP + trust anchor.** Emit C header + NASM equates;
  regenerate x86's five-places geometry + the font-advance constant; **prove the
  x86 build is byte-identical.**
- **Phase 2 — Executable conformance (host)** for the PORT-SPEC §6 invariants.
- **Phase 3 — `unofs` worked example** — extract `unofs_core`, define `block`,
  host-verify against a golden FAT12 image, wire one C port.
- **Phase 4 — Asm consumption** — per-CPU equates/stubs; convert one asm port's FS
  geometry to generated equates (x86, then a 68K port); prove no behavior change.
- **Phase 5 — Hybrid policy pilot** — compile `unofs_core` via vbcc for Amiga 68K,
  link hand-asm trackdisk; verify on WinUAE.
- **Phase 6 — `uno2d`** — generalize `fb.h` into the tall vtable; software floor on
  host, then the **Amiga blitter/copper accel** as the first hardware-accel proof.
  Retarget `unoui`.
- **Phase 7 — Concurrency floor + host SMP oracle** — formalize `COOP` +
  the `uno_sched` service + the sync/job API (free on `COOP`); stand up the host
  **pthreads `SMP` backend under ThreadSanitizer** and the concurrency stress pass;
  add `PREEMPT` on x86.
- **Phase 8 — Display + profiles + directional input** — multi-surface model and
  the three profiles land together; validate `minimal`/directional on a tiny target
  (NES or GB) in emulator.
- **Phase 9 — `unosound`** — voice/score floor + a chiptune accel (SID or Paula) +
  a PCM backend, anchored on the Tracker format.
- **Phase 10 — SMP + OFFLOAD pilots** — **Saturn dual-SH-2 `SMP`** (real
  `ctx_switch`/`barrier`/`core_start`, checked against the Phase-7 host oracle);
  then **PS3 SPU `OFFLOAD`** for the job-kernel tier.
- **Phase 11 — Drivers & buses (static-link)** — service registry + one bus (USB on
  DC/PS2 *or* PCI on PowerPC Mac), one `block` + one `input` driver, proving
  enumerate→bind→register; the FDS detect path validates the scale-down.
- **Phase 12 — Ship UnoDOS 3.1 ABI** (categorized ordinals, `.UNO` v2 with
  arch/abi/profile/caps, font fix); re-issue x86 + C ports first, then port-by-port
  with a dual-build window.
- **Phase 13 — Roll out new targets** as backends (GC/PS3/Xbox/Saturn/PSP/Vita/DS/
  VB/PPC-Mac) + the **Z80 asm family** (SMS/GB); add **networking** and the
  **HEADLESS server** profile where a net-capable port wants it.

## 17. Risks & open questions

- **Parallel-correctness is the new sharpest edge.** Code written on the
  cooperative floor can hide races under SMP. Mitigation = the §10 correctness
  contract (kernel guards its own shared state; apps share nothing implicitly;
  parallelism is opt-in via real primitives) + the host `SMP`+TSan oracle as a
  gating conformance pass *before* Saturn/PS3. This must be enforced, not assumed.
- **Heterogeneous offload (SPU/DSP) is genuinely different from SMP.** A job kernel
  may need a hand-written accelerated variant; the portable C fallback must always
  exist. Open: how much of the job ABI to freeze in `UNODEF` vs leave per-backend.
- **Asm policy-sharing is bounded.** No shared asm bodies across instruction sets;
  for tiny ports, "reduce duplication" is met only at the tables/equates/conformance
  tier. Stated plainly.
- **New CPU family (Z80) + many new C SDKs** — each SDK's calling convention,
  headless-emulator story, and BE/LE quirks cost real integration. Pilot the
  riskiest early.
- **`unogen` backend sprawl** across assemblers — keep asm emit to flat constants +
  offsets + jump-table skeleton, never logic.
- **Endianness discipline** — BE PPC/Saturn/GC punish any untagged field; run a BE
  pass / byte-swap fuzz in host conformance early.
- **`UNODEF` format & ownership** — language, review, and keeping *it* from
  drifting; guardrails = its own conformance + the byte-identical x86 build.

**Resolved this round:** x86 is **demoted** to first-consumer + conformance oracle;
drivers are **static-linked** in 3.1 (loadable modules deferred); the concurrency
pilots are **Saturn (SMP)** + **PS3 (OFFLOAD)**.

## 18. Summary

UnoDOS 3.1 turns the contract from *prose extracted from x86* into a
**machine-readable definition every world is generated from and checked against**
(x86 demoted to oracle). It keeps the worlds but moves the boundary of "world" down
to the **subsystem**, and makes the universal boundary **tall** — a software floor
plus optional high-altitude hardware overrides — so the same write-once app uses
the Amiga blitter/copper/Paula, the SID's voices, a console GPU, *and multiple
CPU cores* without being capped at software speed or a single core. On that
boundary it defines the full **subsystem catalog**, a **tiered concurrency model**
(cooperative floor → preempt → SMP → SPU/DSP offload) that is free on a 6502 and
real on a Saturn, a static-linked **driver/bus model** (drivers are just
runtime-bound service backends), a **multi-surface display** model, and three
Contract **profiles** that scale honestly from a 2 KB NES to a 256 MB PS3. `unofs`
is the worked proof; a host `SMP`+ThreadSanitizer backend is the cheap oracle for
parallel correctness; and the byte-identical x86 rebuild is the anchor that makes
the generator trustworthy.

---

*The Contract core (`unodef/` + the 3.1 window ABI) is implemented and shipping across the
ports; the broader driver/bus, scheduler, and multi-surface models here are the forward
design, built per §16 and verified host-first at each phase.*
