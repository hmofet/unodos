# Round 2 - Studio "build+run .py" hard crash: root cause (2026-07-20)

## Executive summary
The shipped sample (`sdk/SAMPLE.PY`, the "Bouncer") and `DUUM.PY` are pure-bytecode
Python (neither uses `@micropython.viper`/`native`; Duum's hot loop is the C helper
`cv.wall_col`). So the native-emitter theories (NX heap, SysV/MS ABI) are real but
NOT triggered by any shipped app. The crash on the actual sample is a **GC
use-after-free** from an ABI-incomplete register scan (#1). Verified first-hand.

## 1. CONFIRMED - THE crash: GC root scan omits RDI/RSI (Win64 callee-saved) -> live mp_obj_t collected -> use-after-free
- `pc64/upy_port/pc64_upy_stubs.c:16-27` - `gc_helper_get_regs_and_sp` saves only 6
  registers: `rbx, rbp, r12, r13, r14, r15` (the SysV callee-saved set). Verified;
  the stub's OWN comment even acknowledges the Win64 ABI for the arg register (rcx)
  yet still saves only the SysV six.
- `pc64/upy/shared/runtime/gchelper.h:37` - `typedef uintptr_t gc_helper_regs_t[6];`
  for `__x86_64__`.
- `MICROPY_GCREGS_SETJMP` is NOT set (verified: no override in upy_port/ or build.sh)
  -> the native-asm path above is used, not setjmp.
- `pc64/build.sh:14` - PYRT is compiled/linked with `x86_64-w64-mingw32-gcc` = Win64
  MS x64 ABI.

**Mechanism.** Under Win64 the callee-saved integer registers are RBX, RBP, **RDI,
RSI**, R12-R15 (eight). Under SysV they are only the six the stub saves; RDI/RSI are
caller-saved there. Because mingw treats RDI/RSI as call-preserved, the MicroPython
VM can keep a live `mp_obj_t` heap pointer purely in RDI or RSI across a call that
triggers `gc_collect()`, spilling it nowhere on the reachable stack. The GC's
conservative scan (registers-via-stub + C stack) never sees that reference; if
nothing else holds the object, GC frees it, the block is recycled on the next
allocation, and the VM then dereferences the dangling pointer -> heap corruption ->
with no IDT, #PF triple-faults -> hard reset.

**Why "shortly after run", and why QEMU can survive it.**
`MICROPY_GC_ALLOC_THRESHOLD 1` (mpconfigport.h:22) forces a full GC on essentially
every allocation, and the Bouncer boxes several floats per tick/draw (REPR_A boxes
all floats), so the register scan runs many times per second; within seconds a live
float pointer sits in RDI/RSI at a collect. It is a heisenbug: whether the freed slot
is reused before the dangling deref depends on timing, so the same binary can survive
QEMU's TCG timing and die on real hardware.

**Note:** MicroPython's own `nlrx64.c:72-73,109-110` DOES save/restore RDI/RSI on
Win64 - proof the ABI difference is known upstream - yet the GC helper was left
SysV-only. That asymmetry is the bug.

**Fix (either):**
- Set `#define MICROPY_GCREGS_SETJMP (1)` in `mpconfigport.h`. `setjmp`/`jmp_buf` on
  mingw saves the full Win64 non-volatile set (incl. RDI/RSI and XMM6-15); this is
  what upstream `ports/windows` does and makes the stub unnecessary. Lowest-risk.
- Or extend `gc_helper_regs_t` to `[8]` and add `mov %rdi,48(%rcx)` /
  `mov %rsi,56(%rcx)` to the stub (the array is currently sized [6], so it must grow
  too).

## 2. CONFIRMED (latent) - GC heap is NX kernel BSS; any viper/native app faults on real firmware
- `pc64/pc64_uui.c:154` - `static char g_pyrt_gc[16*1024*1024];` (GC heap) in kernel
  BSS.
- `pc64/build.sh:92,356` - kernel linked `--nxcompat` -> firmware that honors it marks
  DATA/BSS NX.
- `mpconfigport.h:35` - `MICROPY_EMIT_X64 (1)`: viper/native code is emitted into the
  GC heap and executed.
- `uefi_main.c` builds no page tables of its own (no CR3/EFER/NX code), so attributes
  come from firmware. The module loader deliberately uses `EfiLoaderCode` to stay
  executable post-EBS (`pc64_modload.c:246-249`), but the GC heap gets none of that.

Executing JIT-emitted native code from RW+NX pages = #PF -> triple fault. QEMU/OVMF
does not enforce this. NOT triggered today (no shipped `.py` uses viper/native) but
would be THE crash the moment a viper app runs on metal. **Fix:** carve the GC heap
from an `AllocatePages(EfiLoaderCode)` reservation (mirror `uno_modload_reserve`), or
clear NX via `EFI_MEMORY_ATTRIBUTE_PROTOCOL`, or set `MICROPY_EMIT_X64 (0)` if viper
is unwanted.

## 3. CONFIRMED (latent) - native emitter uses SysV ABI; runtime is Win64
`MICROPY_EMIT_X64` -> `asmx64.c` emits SysV-convention calls (args RDI/RSI/..., no
shadow space), but every C runtime function it calls is mingw/Win64 (RCX/RDX/R8/R9).
Same non-trigger as #2, same fix.

## 4. SUSPECTED (lower) - one-shot `stack_top` makes the C-stack scan range fragile
`gchelper_native.c:44` computes `(stack_top - sp)` with no clamp; `stack_top` is
captured once deep in the first run. If a later callback's `gc_collect` runs at a
shallower C frame (`sp > stack_top`), the length underflows to ~2^60 and the scan
runs off the stack. Probably safe for the shallow Bouncer, but brittle. Fix: capture
`stack_top` at the outermost shell frame, or clamp the range.

## 5. SUSPECTED (low) - `___chkstk_ms` stubbed to `ret`
`pc64_upy_stubs.c:5-9` no-ops the stack probe; combined with
`mp_stack_set_limit(64*1024)` a deep VM recursion could silently overflow into
unmapped memory on metal. Latent hazard for recursive apps, not the Bouncer.

## Ruled out
- **modload integer-overflow OOB (round-1 STG-C1/C2):** not on the Studio-py path. A
  PYAPP is never instantiated - `uno_mod_load_pyapp` (`pc64_modload.c:449-463`) only
  validates and hands raw source to PYRT; its size check is 64-bit and safe. The
  `mod_instantiate` OOB writes fire only for real code modules; PYRT.UNO is a trusted
  build artifact.
- **Missing GC root for bound app callbacks:** already fixed -
  `mod_uno.c:234 MP_REGISTER_ROOT_POINTER(uno_app_roots[8])`.
- **Native NLR ABI:** `nlrx64.c` is Win64-aware. Only the GC helper (#1) missed it.

**Bottom line:** fix #1 (`MICROPY_GCREGS_SETJMP (1)`) to stop the sample crashing; fix
#2/#3 before shipping any viper/native `.py` on real hardware.
