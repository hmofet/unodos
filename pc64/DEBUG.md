# UnoDOS/pc64 debug + stress build

Branch **`pc64-debug-stress`** (based on `review-fixes-2026-07-20`). This build
turns the silent metal failures the deep review catalogued into readable
reports on the boot volume. It exists to be *driven hard on real hardware* -
the X1 Carbon, the Surface Laptop Go, and the Latitude are the three most
different targets - and to leave behind a `\CRASH` folder you copy to
amanuensis for a Claude Code agent to read.

Everything here compiles away without `-DUNO_DEBUG`; the plain OS is unchanged.

## What it adds

| Piece | What it catches | Where it lands |
|-------|-----------------|----------------|
| **IDT fault handler** | every CPU exception (#PF/#GP/#DE/#UD/#DF...) that used to triple-fault and silently reboot | `\CRASH\CR###.TXT` |
| **Watchdog** | hard freezes (the main loop going silent) - firmware timer while attached, LAPIC timer once detached | `\CRASH\HG###.TXT` |
| **Residue flush** | a boot that died with *no* report at all (triple fault, power event) - salvages the RAM-stashed log tail on the next boot | `\CRASH\RS###.TXT` |
| **Panic / assert / stack canary** | software-detected fatals (`__stack_chk_fail`, `abort`) | `\CRASH\PN###.TXT` |
| **Boot environment block** | the machine-specific state almost every confirmed bug depends on | `\BOOTENV.TXT` + stamped into every report |
| **UBSan traps** | signed-overflow / OOB index / bad shift / null deref, as a `#UD` with the exact RIP | a `CR###` report reading `UBSAN TRAP` |
| **Perf HUD + counters** | slow render vs slow present, idle-CPU %, and the **framebuffer cache attribute** (the biggest unmeasured perf unknown) | on-screen HUD + `BOOTENV.TXT` |
| **Stress driver** | drives every app / input path / FS depth / malformed file to *provoke* the above | armed by `\STRESS.CFG` |
| **Symbol table** | self-symbolized backtraces (`name+0xoff`) | baked into the image + `\DOCS\SYMBOLS.TXT` |

Reports are plain ASCII. The kernel log is a RAM ring written continuously and
mirrored into a warm-reset-surviving stash, so even a triple fault (which
writes no report) leaves the last log lines for the next boot to salvage.

## Building

WSL, mingw toolchain (same as the normal build):

```sh
cd pc64
./build.sh                 # debug build is the DEFAULT on this branch
UNO_DEBUG=0 ./build.sh      # ...build the plain OS instead
UNO_UBSAN=0 ./build.sh      # debug, but no sanitizer traps
UNO_DBGCON=1 ./build.sh     # ALSO mirror log+reports to QEMU debugcon (port
                            # 0x402) - QEMU verification ONLY, METAL-UNSAFE
                            # (0x402 can be SMM-trapped on real laptops)
```

The link is two-pass: it links once, reads the `.text` symbol RVAs out of that
image, bakes them into `build/dbg_syms.c`, and relinks. The symbol table is
const data (PE `.rdata`, after `.text`), so the RVAs stay valid - `build.sh`
asserts they didn't move.

## Flashing (one flasher, the debug build)

Per the standing rule this branch ships a **single** flasher, embedding the
debug OS, formatting the **whole disk as one FAT32 volume** (so UnoDOS can
write reports to it):

```powershell
pc64\flash\build-flasher.ps1        # build/UnoDosFlasher.exe (debug OS embedded)
pc64\flash\deploy-to-share.ps1      # + publish to \\behemoth\...\unodos\pc64\
```

Flash a stick, boot the target with **Secure Boot off**, pick the USB from the
firmware boot menu. `\CRASH`, `\STRESS.CFG`, the fuzz corpus and
`\DOCS\SYMBOLS.TXT` are already on the stick.

## Running a stress session on the three machines

The stress driver is **armed by the presence of `\STRESS.CFG`** (any contents).
A freshly flashed stick has a safe default one (`mode=continuous`, no
self-crash). Config keys (whitespace/newline separated, substring-matched):

- `once` - stop after a single full pass (good for a quick smoke run)
- `fast` / `slow` - action cadence (default: one action every 4 frames)
- `allow-force` - **opt in** to a forced `#PF` on pass 1 that proves the whole
  crash pipeline end to end. Off by default so an armed stick never
  self-crashes.

For each of the X1 / Surface / Latitude:

1. Flash the stick, boot the machine to the UnoDOS desktop.
2. Let the stress driver run (it opens every app, sweeps the pointer including
   the wide-desktop overflow-fling case, types storms into editors, builds a
   deep long-named directory tree, churns the window cap, injects OOM, and
   feeds the malformed corpus through the parsers). The HUD (top-right) shows
   render/present ms, fps, idle %, heap, and the live crash count.
3. Drive it by hand too - drag windows, plug a USB-ethernet dongle, run a `.py`
   in Studio, open Photos on the `PICTURES\BAD_*` files, play the
   `MEDIA\BAD_*` clips. The classes that need a real machine (framebuffer
   cache attribute, USB3 event storms, real HID descriptors) only show here.
4. Power down cleanly when done (**Shut Down** in the menu - that marks the
   boot clean so the next boot doesn't file a spurious residue report).
5. Reboot once more so any report captured on the *reset path* gets flushed to
   `\CRASH` (a crash resets the machine; the report is stashed in RAM and
   written on the next boot if the in-fault disk write was skipped).

If a machine **freezes** instead of resetting, the watchdog files an `HG###`
report naming the last checkpoint (`init:storage`, `stress:corpus`, ...) - that
tag is the code region that stalled. If it **resets and loops**, that is itself
the signal: the `\CRASH` reports from each boot accumulate, and after 8 the
stress driver throttles itself so the machine stays usable enough to collect
them.

## Collecting + reading the reports

Copy the whole `\CRASH` folder (and `\BOOTENV.TXT`, `\BUILD.TXT`) off the stick
to amanuensis. Reports are already self-symbolized. To re-resolve raw RVAs (an
older report, or against a different build) use the host tool with the matching
`SYMBOLS.TXT`:

```sh
python3 pc64/tools/crashsym.py DOCS/SYMBOLS.TXT CRASH/CR001.TXT
```

Tie a report to its exact image via the `build:` line (matches `\BUILD.TXT`).

## A crash report at a glance

```
==== UNODOS PC64 CRASH REPORT ====
build: debug-<sha>-<date>
clock / uptime / detached / image_base / heap / last_checkpoint
exception: vector 14 (#PF ...)  err=...  CR2=... (not-present write)
RIP: ...  poll_pointer+0x1a4 [rva 0x...]
   registers (RAX..R15, CS/SS/RFLAGS/CR0/CR3/CR4)
---- backtrace (rbp chain) ----   #00 ... #01 ...  (self-symbolized)
---- raw stack (in-image qwords symbolized) ----  (catches smashed frames)
---- boot environment ----   gop/mtrr/fb-bench/pointer/i2c-hid/ps2/usb/volumes
---- kernel log (tail) ----   the last N log lines + checkpoints before the fault
```

The `fb bench` line in the environment block measures VRAM vs RAM write
bandwidth directly - if VRAM is uncached-class it flags `SLOW PRESENT`, which is
the "slideshow UI" perf bug the review flagged as the biggest unknown.

## Verifying the pipeline in QEMU

```sh
UNO_DBGCON=1 ./build.sh
python3 tools/dbg_crash_test.py     # boots a writable FAT32 disk, forces a
                                    # #PF, asserts the report hits debugcon AND
                                    # is persisted to \CRASH on the volume
```

## Notes / limits

- The RAM stash uses a fixed physical address that survives a warm (CF9) reset
  on most machines; where it can't be claimed it falls back to `.bss` (no
  cross-reset survival - the env block says which).
- While **attached** (USB-booted, e.g. the Surface) the in-fault disk write is
  skipped if the fault interrupted firmware code; the report is stashed and
  written on the next boot. While **detached** (native AHCI/NVMe, e.g. the
  Latitude) it is written immediately by our own driver.
- UBSan traps are applied to first-party pc64 code only, not bearssl / uACPI /
  MicroPython (which do defined unsigned wraparound). A boot-path UBSan trap
  still produces a readable report before resetting.
