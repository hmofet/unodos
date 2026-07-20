# UnoDOS - Adversarial Architecture & Code-Quality Review (2026-07-20)

Sampled ~284k lines C, ~130k asm, 30 `build.sh` scripts, ~30 ports.

## 1. Most questionable architectural decisions

**1.1 The Contract governs the frozen ports and skips the flagship.**
`unodef/unodef.toml` is billed as OS-wide single source of truth. It genuinely is
`%include`d by the *asm* kernels (kernel/amiga/genesis/snes/c64/iigs/macplus/nes/
sms). But the only C consumers of generated `unodef.h` are `uno2d/uno2d.c:6` and
`unofs/unofs.h:13`. **pc64 - the ~45k-line flagship under daily development -
consumes the Contract nowhere.** The trust anchor (`unodef/unogen.py:394`) checks
x86 kernel.asm, not pc64. Fix: onboard pc64's core structs to `unodef.h`, or stop
calling it OS-wide truth.

**1.2 The module ABI - the thing the theory says to generate - is hand-copied and
drifting.** The `KernelApi`/`AppInterface` ABI lives as hand-copied C headers:
`pc64/apps/uno_mod.h`=129 lines, `dreamcast/`=94, `mac/`=75, `ps2/`=54 - already
divergent. `uno_app.h` and `app_loader.c` are byte-identical across all four ports.
And the versioning is theater: `KernelApi.abi_version` is written by every loader
but **no app module ever reads it**; only the module *header* abi is checked
(`pc64/pc64_modload.c:332`). Fix: generate `uno_app.h` from the Contract, or
collapse the four identical copies to one.

**1.3 Thirty ports each reimplement the WM; the same bug is fixed N times.**
`HANDOFF-perf.md` documents one defect - "no damage-rect → full-scene repaint" -
found, fixed, and separately render-verified in genesis, sms, snes, amiga, c64,
iigs, dreamcast, ps2, mac, still open on rpi/pinephone/ppcmac. `dostris.c` is
byte-identical across dreamcast/mac/ps2 + a diverged 4th in pc64 + 7 asm rewrites.
Dominant structural maintenance cost. Fix: a shared `unowm` C policy for the four C
ports (the damage-rect fix would be one commit, not five).

**1.4 CONTRACT-ARCH.md is 660 lines of forward design, mostly unbuilt - and reality
contradicts its centerpiece.** Tiered concurrency, SMP+TSan oracle, SPU OFFLOAD,
multi-surface display, driver/bus registry - none exist as code. Meanwhile
`pc64/apps/studio_ai.c:269 do_request()` is a **fully blocking** TLS loop (line 329)
that freezes the entire cooperative desktop for a whole HTTP round-trip - the exact
failure §10 exists to prevent. Fix: split "what ships" from a "SPECULATIVE" section.

**1.5 "Executable conformance" tests a Python reference model, not the 30
implementations.** `unodef/conformance/conformance.py` reimplements the policy in
Python and checks golden vectors (with a clever discrimination test), but never runs
genesis asm or pc64 C. Fix: wire the models to `UNO_HOST` and run pc64's actual WM
against `vectors_zorder()`.

**1.6 The build system is 30 hand-written shell scripts with no dependency
tracking.** `pc64/build.sh` (374 lines) hardcodes its object list as a shell loop
(line 50), two divergent modes with copy-pasted BearSSL loops (84-90, 341-349),
duplicated QEMU blocks (315-322, 364-372), duplicated app loops that already differ
(149 vs 350). No incremental build. Import checking is a fragile `nm|awk|sort|grep`
pipeline (line 139). Fix: one parameterized driver with real prerequisites.

**1.7 Studio AI: plaintext keys + an app-writable MITM switch.** `AI.CFG` holds keys
plaintext (acknowledged) but *also* `insecure=1` → unvalidated `tls_connect`
(line 286) and a `host` override (283). Any app that can write the volume can flip
Studio to "trust any cert, connect to my IP" and exfiltrate the API key (sent at
316-321). Also mallocs at 398-402 null-check only `conv`; `resp_buf`/`req_buf`/
`reply_buf` used unconditionally → null-write on alloc failure. Fix: remove
`insecure`/`host` from on-disk config, null-check every alloc, yield in the read
loop.

## 2. Systemic code-quality issues
- **Duplication by copy is the house style**: 56 C app files as independent tracked
  copies; `uno_app.h`/`app_loader.c` x4 identical; `uno_mod.h` x4 divergent.
- **Reinvented parsers per subsystem** (HTTP chunked `studio_ai.c:224`, JSON,
  `parse_hostport`, TOML) even in the C world.
- **Inconsistent error handling**: alloc checks present then absent two lines later;
  19 hlt/while(1) sites in `uefi_main.c`.
- **Dead ABI negotiation** (§1.2). **Security toggles in plaintext config** (§1.7).

## 3. Tech-debt time-bombs
- **129 build artifacts committed to git** (`git ls-files | grep /build/`).
- **Status docs known-wrong**: CLAUDE.md flags `FEATURE-MATRIX.md` as stale.
- **`pc64-usb-flasher` branch**: 52 commits diverged, carries the pre-`unomedia`
  decoder architecture that would resurrect deleted code if merged.
- **Damage-rect campaign unfinished** (rpi/pinephone/ppcmac).

## 4. Done RIGHT (preserve/extend)
- The Contract genuinely collapsed the asm "five places" for the asm family.
- The shared C libraries (`unofs`/`uno2d`/`unoui`/`uno3d`/`unomedia`/`unoacpi`) are
  the correct portable-core+backend model and pc64 links them all - extend with a
  shared WM.
- The `.UNO` decoupling asserts (`build.sh:309` no app code in kernel; `:153`
  imports must be exported) are excellent mechanical invariants.
- conformance.py's discrimination test is sophisticated; host-first byte-identical
  render verification is disciplined.

## 5. Pushback on the "30 ports" strategy
Not "worst of both worlds" - it's "the cheap half of the right idea, sold as the
whole." Codegen shares *shape* (constants/offsets/enums), rarely the source of
*behavioral* bugs. The expensive, bug-generating surface is *logic* (WM, repaint,
app behavior), which cannot be shared across instruction sets. Codegen touches the
cheap-but-drift-prone 10% and cannot touch the wrong-90% - evidenced by one
damage-rect bug fixed in ~10 ports. Highest-leverage move: a shared `unowm`/
`unoapp` the four C ports link. For asm ports you pay full N-times cost regardless.
