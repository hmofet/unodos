# The ZimaBlade: always-on UnoDOS pc64 metal test box

Archived verbatim on 2026-09-08 from the Claude Code memory file `unodos-zimablade-test-box.md`. This is dated session history kept for reference. Later sections supersede earlier ones, so read bottom-up for the current state. The durable facts now live in the memory file itself and in the repo docs.

---

## Its eMMC does not fail writes - it leaves a PLAUSIBLE ARTEFACT (2026-08-21)

**The dead eMMC (volume 1, "NO NAME") carries a ZERO-BYTE `SHELL.CFG`.** That is
the real shape of "accepts a create and never completes the write": a valid
directory entry of length zero, which every is-it-there test in the tree answers
yes to. It cost the 2026-08 conformance run a wrong diagnosis - session restore
was blamed on app-index resolution when the actual cause was `prefs_read`
scanning from volume 0 and stopping on the first read that was not NEGATIVE, and
zero is not negative. So the box parsed an empty config on **every boot** and had
been silently discarding every Control Panel preference as well. Fixed
2026-08-21; when reading anything off this box, `uno.size(v, name)` per volume
first - **volume 2 `UNODOS` (usb0) is the boot stick and the only volume whose
contents mean anything**.

**This is the SAME fingerprint already recorded for a zero-byte `UNOSEC.DB` on
2026-08-18 (further down this file).** It is the device's signature, not a
one-off, and the 2026-08 lesson is that the fingerprint was known while the CODE
still had the matching bug: knowing "size 0 means a dead volume" is worthless
until every reader in the tree tests `> 0` rather than `>= 0`.

## Updating it remotely actually works: the bridge's `push` verb

`push <vol> <path> <localfile>` in `~/urc-multi/zima/cmd.txt` runs the A/B OS
update (`UnoAutoLink.push_file`: chunked `put` + verify). Measured 2026-08-21:
**45-145 KB/s**, 5.7 MB (kernel + 5 `.UNO` modules) in about a minute, every
transfer byte-verified, three reboots in one session with no physical access.
`put` stages in an 8 MiB `.bss` buffer and only touches disk at `done`, so an
interrupted push writes nothing. **Push only to volume 2** - a write to volume 1
hangs the box.

**There is NO delete verb in URC, and the Files pane does not scroll**, so a file
pushed into `APPS\` cannot be removed remotely. Truncate it instead:
`py import uno; uno.write(2, "APPS"+chr(92)+"X.UNO", b"")` - a `.UNO` with no
48-byte header fails `uno_mod_desc_read`, so `rescan` drops it from the registry.
Use `chr(92)` for the backslash; it survives every layer of shell quoting.

The **`zgrab.py shot <png> 1`** scale argument matters: without it you get a
half-size image and every coordinate you read off it is wrong by 2x. This box's
desktop is 960x540.

The **`key` verb takes `<scan> <uni> <ctrl>`** - `key 23 27 0` is Esc,
`key 23 27 1` is Ctrl+Esc (the Start menu), `key 0 65 0` types "A". Arrow keys do
not step the Files list.

The **ZimaBlade** is our always-on desktop test system for UnoDOS pc64 development
(adopted 2026-07-23), driven live from **devbuntu** (dev PC, LAN `192.168.2.100`)
over the unoautomate remote channel (URC). See [[unodos-unoautomate]] for the
harness contract, [[unodos-pc64-wired-nics]] for the NIC it connects over.

## BACK UP as of 2026-08-18 10:51 — and what it can/cannot film

The power-cycle happened: the box re-dialed the bridge at **10:51 on
2026-08-18** and has held the link since. State as found, all read over the
bridge: build **`debug-c958b2e2-20260807-1552`** (11 days old), `screen info`
**960x540**, boot volume **2 `UNODOS`** on `usb0` (`disks` row ending `1 1`),
and only TWO disks enumerated again — both 500 GB drives absent, as on
2026-08-05.

**It cannot shoot the demo film, and the reason is resolution, not speed.**
The cut is assembled at **1280x800 with nothing upscaled** (SCENES.md), and the
desktop is **half the firmware GOP mode**, so 960x540 means the sink is handing
it 1920x1080. Reaching a 1280x800 desktop needs a **2560x1600** GOP mode, which
a 1080p EDID never offers - so this is a **physical fix at the box** (a
different monitor or EDID emulator), not something a push can change. There is
no `screen modes` verb (`screen` takes only `info|grab|read|record`), so the
mode list can only be enumerated by driving the Display panel over `zgrab.py`.

Also missing for s08 regardless: **no `DOOM1.WAD` on the boot volume at all**
(`uno.size` → -1), and `APPS\DUUM.UNO` is the **old 17,437-byte walkable
prototype** - feature-complete Duum is 81,722 B. See [[unodos-demo-video]] for
why QEMU+KVM is the shooting platform.

## RESOLVED 2026-08-18: stick rewritten on devbuntu, Duum validated in QEMU first

arin pulled the Verbatim stick and put it in devbuntu, which is the escape hatch
the "no remote recovery path" note should always point at. It appears as
**`/dev/sdf`** — identify it by **model `STORE N GO` + 29.3 G + partition label
`UNODOS`**, never by letter. devbuntu's write-blocker leaves it `RO 1`: clear
with `blockdev --setrw` on **both** `/dev/sdf` and `/dev/sdf1` after
`udevadm settle`, then it mounts rw.

**Physical access removes the 8 MiB URC cap entirely** — the 11 MB WAD was
simply copied on. Restored to clean master `debug-9bb6e83e-20260818-1703`
(kernel + matching BUILD.TXT), with DOOM1.WAD 11,159,840, DUUM.UNO 81,770 and
PYRT.UNO 322,864 all md5-verified against source. The ESP is only 511 MB of the
29.3 G stick and sat at 47 MB used.

### Boot the stick in QEMU BEFORE carrying it back

`sudo qemu-system-x86_64 -machine q35 -m 4096 -cpu host -enable-kvm ... -drive
format=raw,file=/dev/sdf,snapshot=on -display none -serial file:...`.
**`snapshot=on` is what makes this safe** - writes go to a temp overlay and the
stick was byte-identical afterwards (md5 re-checked). One boot proves the kernel
runs and the asset set loads, which is exactly the thing a trip to the box would
otherwise be spent discovering.

Two traps in that VM session:
- Through slirp the dial-in arrives from the HOST's IP, so the bridge files it
  under **`~/urc-multi/192-168-2-100/`, not `zima/`** (`zgrab.py` hardcodes
  `~/urc-multi/zima` at line 18 - copy it and repoint `D`).
- **The volume index is 1 in the VM and 2 on metal** (the VM has no internal
  "NO NAME" disk). `uno.read(2,...)` returned `None` and every size `-1`, which
  looks exactly like missing files. Run `vols` first, always.

**Duum VALIDATED (in QEMU, not on metal).** `uno.run_app(1,'APPS/DUUM.UNO')`
returns **0 = success**, and the window renders a real textured level with the
full STBAR. Interactivity proved by firing: the URC verb is
**`key <scan> <uni> <ctrl>`**, so fire is `key 0 102 0` ('f'), and three of them
took ammo **50 → 47** while the HUD frame counter moved f65 → f69. `key w` does
nothing - Duum reads arrows + `f` + space, not WASD. HUD read
`r3.8ms p1.2ms f65 idle95%` at a 640x400 desktop.

## THE FIX WORKS ON METAL — and then DUUM wedges the box (2026-08-18 14:56)

**Boot fix CONFIRMED on hardware.** With `...-1831` the box dialed in **10 s
after power-on**, served `vols`/`disks`/`py` cleanly, and reported
`disks: 0 emmc0 ... 1 0` / `1 usb0 ... 1 1`. The store landed where it should:
**`vol2` (boot stick) = 24784, `vol1` (eMMC) = 0**.

**That `0` is the smoking gun for the original hang**: a **zero-byte UNOSEC.DB**
on the eMMC - the old kernel created the file and the write never completed.
`uno.size` returning **0 rather than -1** is the fingerprint of a device that
accepts a create and then swallows the write; treat a 0-byte store as a dead
volume, not as an empty one.

**Then `uno.run_app(2,'APPS/DUUM.UNO')` killed it.** In QEMU that call returns 0
instantly; on metal it **never returned** - `! no response to 'py'` at 90 s, a
`screen grab` timed out behind it, the link dropped `silent 60s` at ~2 min, and
the box has not pinged or re-dialed since (watchdog did not recover it). So the
failure is in the LAUNCH, not the render loop - the box stopped servicing the
URC tick while loading, and never came back.

**Reproduced twice, and it kills the box in ~10 SECONDS** (tight-ping timing;
the 90 s figure from the first run was just how long the URC verb took to give
up). Debounce liveness polling - break on 3 consecutive failures, not 1, or a
transient miss reads as death.

**IT LEAVES NO TELEMETRY WHATSOEVER.** After both deaths the stick carries **no
CR report, no HG report** (only the unrelated `HG005` from 2026-08-05), and
`BOOTS.TXT` shows **no extra boot entry**, so the box did not reset either. The
last BOOTLOG predates the launch and describes a completely healthy machine:
`audit uid=0 sec.boot store loaded -> ALLOW`, `shell: main loop entered,
fb=960x540`, net test WIRED PASS, `remote: link up` at 28.6 s.

That signature - no caught fault, no watchdog fire, no reset, network gone
instantly - means the whole CPU is stopped, not just the shell. **A hang with
interrupts disabled (or a bus stall) is the shape**, because the LAPIC watchdog
at 20 s never got to run.

**The framebuffer is NOT the culprit - I was wrong to suspect it.** The metal
desktop measured `DBG r5.1ms p4.7ms f61 idle96%`: **61 fps, present 4.7 ms**.
The `UC - SLOW PRESENT` warning and the 1:125,081 vram:ram ratio are real but
the shell only presents dirty regions, so normal drawing is fine. Do not spend a
session on MTRR on this evidence.

**Nor is it the USB path or the WAD size**, which the QEMU control rules out: the
same kernel, the same physical stick over `usb-storage` on xHCI, the same 11 MB
WAD, runs Duum fine. The delta is real Apollo Lake silicon.

### STORAGE IS EXONERATED (read ladder, 2026-08-18)

`uno.read_at(2,'DOOM1.WAD',0,N)` on metal: **64 KB, 1 MB and 8 MB all return
instantly and the box stays up**. The full 11,159,840 raises a clean
`MemoryError: memory allocation failed` and **the box survives that too** - so
PYRT's single-allocation ceiling is between 8 and 11 MB, and hitting it is
graceful. Neither the USB read path nor the WAD size can be the killer.

### A LIVE LOG CHANNEL EXISTS - use it, a hang writes nothing to disk

`unolog` forwards **per record, immediately** (`syslog_emit` calls
`net_udp_send` inline; it is NOT batched until flush), so it survives right up to
the instant of death. `uno_dbg_log` taps into it at **severity 7**
(`UNO_DBGLOG_TAP_SEV`, uno_debug.c:195), so all ~96 kernel call sites stream out.

**BOTH levels must be raised or nothing arrives** - this cost several rounds:

```
py import uno; uno.log_level(7); uno.log_remote_level(7)
py import uno; print(uno.log_remote('192.168.2.100', 5514))   # returns 1
```

`log_remote_level` alone is not enough: `unolog()` drops the record against the
LOCAL `g_level` before `syslog_emit` ever sees it, and the defaults are
`g_remote_level = LOG_WARNING`. Receiver: any UDP socket on the port (a 6-line
Python `recvfrom` loop). Arg order is `uno.log(sev, fac, text)`; `<135>` in the
frame is severity 7.

### What the live channel showed: NOTHING

With the channel proven working (a `=== ABOUT TO RUN DUUM ===` marker arrived),
`run_app` was issued and the box died **11 s later having emitted not one
further line**. So the fatal path logs nothing at all - which is itself the
finding: it is not reaching any instrumented code.

## DUUM RUNS ON METAL (2026-08-18, kernel `debug-9bb6e83e-20260818-2055`)

**Confirmed on the ZimaBlade, not in QEMU.** Full launch trace completes
(`window open, granting caps` -> `DONE` -> `tick#0` -> `draw#0 ... ALL OPS DONE`
-> three full frames), the E1M1 view renders at a 520x380 canvas with the real
STBAR, and **firing takes ammo 50 -> 47**, so it is interactive, not a static
frame.

Measured from the shell HUD, un-instrumented build:

| state | HUD |
|---|---|
| desktop only | `r5.1ms p4.4ms f61 idle96%` |
| Duum running | `r29.9ms p4.4ms f42 idle89%` |

So Duum costs **~25 ms of render per frame** and the box still idles ~89%.
**Present stays at 4.4 ms even with Duum up** - the `UC - SLOW PRESENT` warning
never mattered, because the shell only presents dirty regions. This box is
nothing like the X13 Yoga's ~2.5 fps.

The control matters as much as the result: **a native app (`launch 4`, Clock)
opened first and the box survived**, which is what proves the fix rather than
some Duum-specific luck.

## SOLVED: it was NEVER DUUM. `session_save()` wrote to the dead eMMC

**Opening ANY app killed this box.** `open_app()` ends with `session_save()`,
which picks its volume with `session_vol()` (pc64_uui.c) - **a verbatim copy of
the old `unosecure.c pick_vol()`**, comment included ("Same order unosecure
picks"). It returns the lowest writable native volume = **vol1, the eMMC that
enumerates, reports writable, and never completes a transfer**. The
`uno_fs_write(v,"SHELL.CFG",...)` never returns and the machine stops dead.

So the two hangs were **one bug in two copies**: `pick_vol` hung it at BOOT,
and after I fixed only that copy, `session_vol` hung it on the FIRST APP OPEN -
which looked exactly like "Duum kills the box" and cost most of a day.

**The proof is in the QEMU stand-in volumes** (each run's non-boot 256 MB FAT
image, kept at `/tmp/nonameN.img` on devbuntu):

| kernel | what landed on the NON-BOOT volume |
|---|---|
| `-1703` no fixes | `UNOSEC.DB`, `UNOSEC.LOG` -> the boot hang |
| `-1831`/`-2031` pick_vol fixed only | **`SHELL.CFG`** -> the open-an-app hang |
| `-2055` both fixed | **empty** |

Fix: `session_vol()` prefers `uno_fs_is_boot()` first, same shape as pick_vol.

**LANDED**: commit `6ff23564` on branch **`fix-persist-to-boot-volume`**, pushed
to `hmofet/unodos` (based on `9bb6e83e`; master had moved 23 commits to
`0c9e0de9` by then but the merge is CLEAN - nothing upstream touches these
files). **The box runs the committed build `debug-9bb6e83e-20260818-2120`**,
pushed over URC, and both stores now sit on the boot stick.

Those 23 upstream commits are largely Duum perf work done in parallel
(`0c9e0de9 requests: the Duum frame rate is the present path, not the renderer`,
`6e880b69 duum: A/B a CONTROL app`, `d4773585 cv.seg_cols`,
`be366941 PYRT native/viper emitters crash the guest`). **Reconcile before
trusting them**: on this box present measured 4.4 ms with Duum up and 4.7 ms
idle, so the cost here is render, not present - and any measurement taken on
this box before the fix was taken on a machine about to hang.

### FOUR MORE COPIES OF THIS HEURISTIC ARE STILL UNFIXED

`grep -n 'uno_fs_kind(v) == 1 && uno_fs_writable(v)' *.c` finds it in
**`iwlwifi.c:3367`** (WiFi creds), **`unossh_store.c:90`** (SSH keys),
**`unoscript.c:241`**, and `installer.c:411` (that one is choosing an install
TARGET and must NOT prefer the boot volume). Each is the same latent hang on any
machine with a sick disk that enumerates before the boot medium. **This wants one
shared helper, not a sixth copy.**

### METHOD THAT CRACKED IT, after two wrong suspects

I blamed the framebuffer (wrong - desktop measured 61 fps, present 4.7 ms) and
then ed25519/entropy in `caps_begin` (wrong - the trace stopped BEFORE it). What
actually worked was bisecting by instrumented trace over the live log channel,
each round halving the window: init -> render loop -> before the first frame ->
inside `open_app` after `build()` -> `session_save`. **A hang that writes nothing
is still perfectly bisectable if you can stream a log off the box.**

### Duum's INIT is innocent - the earlier narrowing that led here

Traced by adding `uno.log(7,1,...)` through `apps/DUUM.PY` and `uno_dbg_log` to
`pc64_shell_run_python` (pc64_uui.c). **A traced DUUM.UNO needs no reboot and no
disk** - it is ~83 KB, so `push 2 APPS\DUUM.UNO <file>` over URC then `run_app`
re-loads it. Only a kernel change costs a reboot.

On metal `build()` **runs to completion in ~1 s**: wad -> textures -> palette ->
sprites -> new_game -> load_level -> geometry -> sky -> player_start ->
`build DONE`. Then the box dies ~10 s later. So the WAD, the textures, the level
and PYRT are all fine, and the fault is in the per-frame path
(`tick()` -> `draw()`), not in loading anything.

**`draw()` is a loop over `self.frame` ops calling NATIVE span renderers** -
`cv.wall_span` / `cv.mask_span` / `cv.flat_span` in `upy_port/mod_uno.c`, each
taking 13-14 args straight from Python. `cv_wall_span` validates `tw/th/count/
wpx` and clamps `sh`, but **never clips x/y0** - it relies on `fb_blit`, which
DOES clip (`fb.c`), so no obvious out-of-bounds by inspection.

**THE SIZE DIFFERENCE THAT MAKES QEMU USELESS AS A CONTROL:** the canvas is
**520x252 in QEMU but 520x380 on metal**, because the desktop is 640x400 there
and 960x540 here. Every span coordinate scales with that, so a height-dependent
renderer bug fires only on the box. Any future QEMU repro attempt must match the
metal desktop size first.

**QEMU reference trace saved at `devbuntu:~/zima-trace-qemu-reference.log`**:
3 complete frames, **2029 ops each - 868 `F`(flat), 671 `M`(mask), 490 `W`(wall)**,
each frame ending `draw#N ALL OPS DONE`.

### NARROWED AGAIN: it dies BEFORE the first frame, not during one

The metal trace ends at **`build DONE` and `tick#0 enter` NEVER APPEARS** - so it
never reaches the render loop either. The renderer and the span ops are innocent
too; the whole `draw()` per-op trace never got to run.

The QEMU reference gives the exact sequence that must follow `build DONE`:

```
runpy: module compiled, opening window (this calls build())
duum: build enter ... duum: build DONE
runpy: window open, granting caps      <- open_app() returned
runpy: DONE                            <- unoscript_app_caps_begin() returned
duum: tick#0 enter
```

So the fatal window is **after `build()` returns and before the first `tick()`**:
the remainder of `open_app(EX_PYAPP)`, then `unoscript_app_caps_begin(vol,path)`.
**`caps_begin` is the prime suspect** - it opens the app's SIGNED MANIFEST, and
`c487acbe` moved that to **ed25519** verification with salts drawn from the
**fail-closed `tls_entropy` source**. An entropy source that never satisfies on
this silicon would hang exactly here, with interrupts off and nothing logged.
(The CPU does advertise RDRAND - `cpuid1 ecx=47f8ebbf`, bit 30 - so if that is
the fault it is in the entropy plumbing, not a missing DRNG.)

Kernel `debug-9bb6e83e-20260818-2031` carries the `runpy:` trace and is
QEMU-verified. Push it (4,706,850 B, under the 8 MiB cap) + BUILD.TXT, `reboot`
over URC, and the next metal run splits it: `window open, granting caps` present
= the fault is in caps_begin; absent = it is in open_app's remainder.

## THE FIX: `pick_vol()` prefers the BOOT volume (built + QEMU-proved 2026-08-18)

`unosecure.c pick_vol()` now takes the first writable native-FAT volume **whose
backing device `is_boot`**, falling back to the old lowest-index order. Needed a
new accessor, `uno_fs_is_boot(int vol)` in `pc64_fs.[ch]` (reads
`uno_fat_dev(idx)->is_boot`; **`pc64_fs.c` must `#include "blkdev.h"`** or
`struct uno_bdev` is an incomplete type). Build `debug-9bb6e83e-20260818-1831`.
Uncommitted on devbuntu `~/unodos-build`; writer staged at
`devbuntu:~/write-zima-stick.sh` (finds the stick BY SERIAL FC093B2B7B604FB8).

**A/B proved in QEMU, same stick image, same 3-volume topology:**

| kernel | vol1 (non-boot) | vol2 UNODOS (boot) |
|---|---|---|
| `-1703` master | `UNOSEC.DB` 24784 | none |
| `-1831` fixed  | none | `UNOSEC.DB` 24784 |

**The test only works if the boot disk is presented over USB.** `is_boot` is set
in exactly two places - `blkdev.c` (firmware device-path prefix match) and
`usbmsc.c` (the reclaimed BOT device) - and **never by the native AHCI/NVMe
path**. A first attempt with the stick image on SATA showed `disks` is_boot=0 on
every row, so `pick_vol` fell through and the store landed on vol1 again: the
fix looked broken when the harness was simply unfaithful. Boot it as USB:
`-device qemu-xhci,id=xhci -drive if=none,id=stick,file=IMG -device
usb-storage,bus=xhci.0,drive=stick,bootindex=0`, and `disks` then reads
`usb0 ... 1 1` exactly as the metal box does.

**KNOWN LIMITATION:** on a machine that boots a native AHCI/NVMe disk, `is_boot`
is never set, so this silently falls back to the old behaviour. No regression,
but no fix either - those paths would need to carry a boot flag the way
`usbmsc.c` carries `want_boot`.

**When the stick's own store goes live, delete the stale `UNOSEC.DB`/`UNOSEC.LOG`
on it first** - under the fix the stick's copy IS the account store, and an old
one would gate the box. A missing store just re-seeds empty, no accounts, no gate.

## THE REAL FAULT: master hangs in `unosec_boot()` on THIS box's vol1 (2026-08-18)

**Current master does not reach the desktop on the ZimaBlade.** It stops at
**splash stage 4, whose label is literally `"security (accounts / RBAC)"`**
(`uefi_main.c`, `splash_stage(4, ...)` immediately before `unosec_boot()`), so a
box "stuck on the boot screen" and a box "stuck on security" are **the same
symptom** - the splash stays up until `build_desktop()` runs. arin read the
screen correctly; I talked him out of it because the splash also shows the build
id. **Trust the stage label.**

Telemetry from two separate boots is identical: `last_checkpoint: init:done @
~17.3 s`, kernel log ends at `init done ... shell starting`, nothing after.
`BOOTLOG` is written just BEFORE the security stage, which is why the log always
looks like a clean boot that simply stops.

**My `PUT_MAX` change is EXONERATED.** Clean master `...-1703` (the define
reverted) hangs in exactly the same place as my `...-1643`. The earlier claim
that I had "isolated" it was wrong because I isolated it **in QEMU**, where
master boots fine - it had never been proven to boot on this box.

**It is the DEVICE, not the code path.** `pick_vol()` (`unosecure.c`) takes the
**first writable native-FAT volume from index 1 up**, which on this box is
**vol1 `NO NAME` - the eMMC**, the disk whose `readsec` fails on every LBA. That
is where `unosec_boot()` binds its store, and it blocks there.
**Reproduced-by-elimination in QEMU**: booting the same stick with a second
256 MB FAT image attached gives the guest the box's exact topology
(`vol0 RAM / vol1 NO NAME / vol2 UNODOS`) and master **boots, dials in and runs
Duum**. A healthy vol1 is fine; this box's vol1 is not.

Ruled out along the way: the boot security stage is not new (`unosec_boot()` sits
at the same place in the Aug 7 kernel), `account_t` is byte-identical across the
163 commits, and every store-sizing constant (`MAX_USERS/ROLES/KEYS/USERROLES`,
`NAME_MAX`) is unchanged - so `sizeof(db_blob_t)` did not change and `db_load`
is not failing on size. **Still open: why the Aug 7 kernel got past the same
device.** The likeliest lead is `c487acbe` (unosecure: ed25519 manifests, login
lockout, fail-closed salts) changing the I/O pattern - a WRITE to a dead eMMC
hangs where a read might not.

**There is NO config-only escape.** The only DEBUG.CFG key the boot/security path
reads is `ui-unlock`; nothing steers or disables the store volume. Fixing this
needs a kernel change, and the obvious candidate is making `pick_vol()` prefer
**the boot volume** instead of the lowest index - which would also explain and
fix the Yoga's standing sign-in gate, where the live `UNOSEC.DB` is on the
internal disk while the stick boots. Same bug class, two machines.

## DOWN 2026-08-18 12:47 — the kernel I pushed (superseded by the section above)

**State: needs a physical trip.** It rebooted at 12:47 into a kernel I built and
has not answered **a single ping** since (9+ min; a healthy reboot re-dials in
~45 s). No ping means no IP stack, so no listen-mode, no URC, **no remote
recovery** - exactly as the 2026-08-07 wedge established.

**What I changed and why it was the wrong shape.** URC's `put` stages the whole
file in one static buffer, `PUT_MAX` in `pc64/unoauto_remote.c`, so **the cap is
the largest pushable file** - 8 MiB, sized for BOOTX64.EFI, and **smaller than
every real IWAD** (Freedoom 0.12 11 MB, 0.13 28.8 MB). An 11 MB WAD push dies at
~8.39 MB with `! bad-base64-or-too-big` and correctly leaves **no file** behind
(`uno.size` → -1). I raised it to 32 MiB, which grew `.bss` from **0x09af95f0
(~155 MB) to 0x0b2f95f0 (~179 MB)** with the PE file size unchanged at
4,706,287 - and that image does not boot.

**The stick's own telemetry says the kernel BOOTED - it hung later.** Read off
the pulled stick (preserved at `devbuntu:~/zima-telemetry-20260818-hang/`):
`BOOTLOG.TXT` carries `build: debug-9bb6e83e-20260818-1643` (mine), reaches
`init done: detached=1 volumes=3 - shell starting` at **17.266 s**, writes final
post-detach telemetry at uptime **37.9 s**, and is marked "written
unconditionally - this is NOT a crash". Beside it sits a **0-byte `HG006.TXT`** -
a hang report begun and never finished. `NETLOG.TXT` is **stale from the Aug 7
boot** (it still names `debug-c958b2e2-20260807-1552`), so the new kernel died
**before the network test**, which is why there was never an IP or a ping.

**So it is not "does not boot" - it boots, detaches, starts the shell, then
hangs.** Diagnose this class by reading the stick's `CRASH\DEFAULTS\` on another
machine; a silent box is not necessarily a dead kernel.

**And the cause IS my change**, isolated afterwards on the same stick: clean
master `...-1703` (identical but for reverting `PUT_MAX`) boots, dials in and
runs Duum. Only that one define differed.

**Likely mechanism, unconfirmed and worth a look before anyone retries:**
`BOOTLOG` reports `heap: used=320 free=33554048` — the kernel heap is ~**32 MiB**,
exactly the size I gave `g_put`. `.bss` went 0x09af95f0 → 0x0b2f95f0 (~155 → ~179
MB) with the PE file size unchanged at 4,706,287, so a 24 MiB `.bss` growth
plausibly ran into a fixed-address heap or stash (`stash: ram @1f00000`).
**Raising `PUT_MAX` needs the memory map checked first, not just the define.**

**The rule that would have prevented it: boot the unmodified baseline FIRST, then
layer one change.** A kernel push costs a reboot, and a reboot on this box is
unrecoverable remotely, so every push must be a single-variable experiment.

**Recovery** (physical, at the box): restore a known-booting kernel to
`EFI\BOOT\BOOTX64.EFI` on the `UNODOS` stick. Best candidate is
**`devbuntu:~/zb-boot.efi`** (4,502,645 B, 08-07 11:54) - the Aug 7 lineage that
was actually running. `devbuntu:~/zima-new.efi` (4,706,287 B) is clean master
1226 but has never booted this box. Mount the stick elsewhere and copy, or use
the firmware boot menu to boot another volume. Cannot be done over URC.

## How it looked while wedged, 2026-08-07 to 2026-08-18 (resolved, kept as the signature)

The box **pinged fine** (`192.168.2.118`, MAC `00:e0:4c:30:5b:d4` confirmed, net
stack + frame loop alive) but its URC channel has been dead since the bridge
logged `link dropped: silent 60s` at **2026-08-07 21:06** and it has **not
re-dialed in 11 days**. The dial-out state machine retries every ~5s when
healthy, so the box is stuck: either its TCP still believes it is ESTABLISHED to
the socket the bridge `shutdown()` on the FIN-less drop (RS_UP, empty tx, never
provokes a RST, never retries), or a security dialog is modal at the console
(URC walled off, link "up"). No listen-mode port is open (tried dial-in on
5099/5098/5097 — all time out), so **there is no remote recovery path**. It
needs a human to power-cycle the box; then it re-dials in ~45s.

**Feature-complete Duum is staged and ready to push** (2026-08-18): built
`UNO_DEBUG=1 UNO_DETACH=1` from master on devbuntu, artifacts at
`~/zima-new.efi`, `~/zima-duum.uno` (81770B), `~/zima-doom1.wad`, with helper
`~/zima-push-duum.sh` (re-checks `vols` first — the index moves every boot).
The bridge was restarted `python3 -u urc_bridge.py 5099,5098 ~/urc-multi` and is
listening. When the box comes back: run the helper, push DUUM.UNO + DOOM1.WAD to
the UNODOS boot volume + BOOTX64.EFI to the ESP, `reboot`, then launch Duum via
`uno.run_app(vol,'APPS/DUUM.UNO')` over the `py` verb (a PYAPP has no app-registry
row, so `launch` can't find it). Duum itself is already metal-equivalent verified
in QEMU (`tools/duum_urc.py` PASS).

**Identity / reach:** onboard Realtek NIC MAC `00:e0:4c:30:5b:d4` = `192.168.2.118`.
r8169 (dev 8168) up on metal at **gigabit**; this box metal-validated it. URC dials
OUT to the dev PC, so no TCP ports listen on the box.

## IT BOOTS THE INTERNAL DISK UNLESS YOU CATCH THE BOOT MENU (read first)

Boot order prefers the internal disk (`fw1`), so **inserting a USB stick does not
boot it** — you must hit the firmware boot menu and pick the stick explicitly. This
burned hours on 2026-07-27/28: a "stick-booted current-master build has no network"
report was actually the box running a **five-day-old internal build** the whole
time. Three separate root-cause theories were built on that false premise before
the giveaway surfaced (the running OS had no tabbed Control Panel, a feature that
landed 2026-07-26).

**Always confirm what is actually running before diagnosing anything:**
- `disks` over URC → `is_boot=1` names the disk it booted from.
- On screen: System window, or `BUILD.TXT` on the boot volume (`uno.read(v,"BUILD.TXT")`
  via the `py` verb) → the build id. **BUT a kernel-only push leaves BUILD.TXT
  STALE and it will lie** (2026-08-03: pushed a new BOOTX64.EFI, BUILD.TXT still
  named the five-day-old build). Push `build/esp/BUILD.TXT` alongside the kernel,
  or confirm from `BOOTENV.TXT`, which the running kernel rewrites every boot and
  which also carries the machine name, CPU and geometry.
- **`Native FS:` vs `DETACHED (native):` in the System window proves NOTHING about
  the boot medium on this box.** It has no PS/2, I2C-HID or USB-HID keyboard
  (`native_kbd_for_detach()` fails), so `try_detach()` always refuses and it is
  *always* firmware-attached, USB or internal.
  **SUPERSEDED 2026-08-03:** built `UNO_DEBUG=1 UNO_DETACH=1`, `BOOTENV.TXT` now
  reports `detached: 1` — it DOES detach. Re-check rather than assuming either way.

## Driving the UI over URC (screen + mouse), 2026-08-03

`~/zgrab.py` on devbuntu does screen-grab and click **through the bridge's
file interface** (the bridge owns the box's only link, so `UnoAutoLink`
directly would kill its session): `zgrab.py shot out.png [scale]`,
`zgrab.py click X Y`, `zgrab.py cmd "<verb>"`. It reuses
`unoauto_remote.qoi_decode` and writes PNG with a 40-line encoder (no PIL).

- Desktop is **400x300 scaled to 800x600** — `screen grab 1` returns 400x300
  and those ARE the framebuffer coordinates to click. (`screen info` can
  disagree; trust the grab.) CPU is a Celeron N3350, machine name `DEFAULTS`,
  so telemetry lands in `CRASH\DEFAULTS\`.
- **Every bridge log line is timestamped**, so a parser matching a bare
  `"\n   ok"` terminator never fires and reports a timeout on commands that
  actually succeeded.
- **Opening any security dialog (login/consent/Accounts/Remote control) used to
  freeze URC and reset the box in 20 s** — fixed on master, see
  [[unodos-urc-production-gate]]. The link now survives, but **synthetic input
  is deliberately locked out**, so the panel can be SEEN over URC and never
  DRIVEN. `reboot` is the escape hatch; arming still needs a human at the box.
- **Accounts used to brick this box — FIXED 2026-08-04 (`b73a7fd1`).**
  `pc64_login_gate()` runs before the frame loop that brings URC up, so once an
  account existed the next boot sat at a password prompt with no link, and this
  box has no keyboard. Master now brings the network + channel up FIRST when the
  gate is going to block, and you can sign in over URC (needs `ui-unlock`).
  **Verified on metal**: rebooted with an account, dialed home, signed in
  remotely, reached the desktop. The box is nonetheless left with **no
  accounts** — the rescue depends on running a kernel that has the fix, so a
  rollback or a production image would strand it again. Delete what you create.
- Its DEBUG.CFG now carries **`ui-unlock`** (plus `nostress`, `noshutdown`,
  `remote=192.168.2.100:5099`) so the harness can drive the security dialogs;
  without it the input lockout blocks all injection into them. Debug-only key.

As of **2026-07-28 the internal disk runs current master** (`debug-059a64f-20260728-0425`),
reinstalled over URC, so both boot paths finally agree.

## The URC bridges on devbuntu — ONE CLIENT EACH, MIND THE PORT

Two bridges run, one per box. Each assumes it is the only listener and its box the
only client: when a **second** ESTABLISHED connection appears it `shutdown()`s the
"stale" socket to recover from the box's FIN-less re-dial. So **dialing the wrong
port kills another lane's live session.**

| port | box | dir |
|---|---|---|
| `5099` | **ZimaBlade** — `urc_bridge_v2.py 5099 ~/urc-zima` | `~/urc-zima/` |
| `5098` | **X13 Yoga** (iwlwifi/AX201 lane) — `urc_bridge.py 5098` | `~/urc/` |

Drive by files: append a verb per line to `<dir>/cmd.txt`, read `<dir>/session.log`.
Neither is a systemd unit — they do not survive a devbuntu reboot. Start detached:
`setsid nohup python3 -u ~/urc_bridge_v2.py 5099 ~/urc-zima </dev/null >log 2>&1 &`
(a bare `nohup ... &` over ssh dies when the session exits).

`pkill -f urc_bridge` / `pkill -f qemu-system-x86_64` **self-match** the very shell
running them — use `pkill -f "[u]rc_bridge"`.

The bridge's response timeout is ~15 s: a slow verb (`mkfs`) logs
`! no response to '<verb>'` while still completing on the box. Check for a
`re-dial detected` line to tell a slow op from an actual reset.

## Storage — RE-IDENTIFY BY SIZE/CONTENT EVERY TIME, NEVER BY INDEX

Indices shift across reboots and with which devices are seated; firmware enumerates
BlockIO at boot only, so a hot-plugged stick needs a reboot. Map on **2026-07-28**
(4 disks, Kingston not seated):

- `fw0` 61,440,000 sec (31.5 GB) → the **Verbatim** boot stick when present
- `fw1` 976,773,168 sec (500 GB) → **the UnoDOS internal install**, ESP `UNO-ESP`
- `fw2` 976,773,168 sec (500 GB) → **live ZFS pool** (`zfs-4fa4285d86d53dcb`)
  — **DO NOT TOUCH, real data.** It sits one index from the install target.
- `fw3` 61,071,360 sec (31.3 GB) → eMMC; `readsec` **fails on every LBA**,
  enumerated but unreadable, leave alone.

Identify non-destructively with `readsec <disk> 2 1` → base64 of GPT partition
entry 1 (type GUID at +0, first/last LBA at +32/+40, UTF-16LE name at +56). `arm
<disk>` echoes the size and refuses the boot disk — the safe target-confirm.

## Remote install to a disk WORKS now (supersedes the 2026-07-23 "not safely doable")

The `mkdir` / `prepdisk` / `install` verbs landed, and a full remote reinstall was
done 2026-07-28. Two traps, both **fixed on master** the same day but worth knowing:

- `install <disk>` runs `prepdisk`, which lays the ESP across the **whole** disk.
  `uno_fat_mkfs` used to zero the FAT region one 512-byte sector per write, so a
  500 GB volume blocked past the 20 s freeze watchdog and reset the box **after the
  GPT rewrite but before the format** — old install erased, nothing bootable, and
  every retry died identically. Fixed by batching + a heartbeat pet.
- `install_dir()` in `tools/unoauto_remote.py` took `os.path.dirname` of an already
  backslashed path, so on **any Linux host** it created no directories and every
  nested push failed (silent on Windows). Fixed.

Manual geometry is the fallback if a format ever stalls again: `gptinit` →
`mkpart <d> <first> <last> esp <name>` → `mkfs <d> <first> <sectors> <label>` at a
modest size (31.5 GB formats in seconds; 500 GB is ~15x the FAT). Then push the
tree and `makeboot <disk>`. **`reboot`/`poweroff` flush the write-back FAT cache —
always end a remote write with one**, and `makeboot` with no index defaults to disk 0.

## Flashing a stick on devbuntu

`UNO_DEBUG=1 ./build.sh`, then either `tools/mkuefi.py <MiB>` + `dd`, or partition
the stick directly and rsync `build/esp/` onto it. Gotchas:
- devbuntu's **write-blocker** (`99-usb-storage-readonly.rules`) forces USB disks
  `ro=1` on attach **and on every change uevent** — `blockdev --setrw` *after*
  `partprobe`/`udevadm settle`, or `mkfs` fails mid-write.
- `rsync -a` fails on vfat (no chown): use `rsync -rlt`.
- Target by **serial**, never device letter — a USB re-enum once shuffled `/dev/sde`→`/dev/sdc`.
- **`DEBUG.CFG` used to be read as only the first 511 bytes**, which once pushed
  `remote=` past the cutoff so the box never dialed. **SUPERSEDED 2026-08-07:**
  `pc64_stress.c` now reads `CFG_MAX 4096` and warns if the file is longer. Keys
  first is still the right habit — `iwlwifi.c`'s `file_has_ssid()` still reads
  only **255 bytes**. `cfg_find` skips `#` lines, so a key named in a comment is
  safely ignored.

## Pushing a new build to it (worked out 2026-07-30, no physical trip needed)

The bridge on devbuntu is `~/urc_bridge.py 5099,5098 ~/urc-multi`, file-driven:
append a line to `~/urc-multi/zima/cmd.txt`, read `~/urc-multi/zima/session.log`.
`~/urc-multi/links.txt` says whether the box is connected.

The bridge has a **`push <vol> <path> <localfile>`** verb that does the whole
chunked base64 upload and verifies it. So a build lands with:

```
scp build/BOOTX64.EFI devbuntu:~/new.efi
ssh devbuntu 'printf "%s
" "push 2 EFI\BOOT\BOOTX64.EFI /home/arin/new.efi" >> ~/urc-multi/zima/cmd.txt'
ssh devbuntu 'echo reboot >> ~/urc-multi/zima/cmd.txt'
```

It re-dials in ~45 s. A 2 MB push takes ~15 s and reports `push VERIFIED`.

**Which volume: NEVER trust a remembered index — it moves.** Push to whichever
volume is labelled **`UNODOS`**; that is the stick the box boots. The index it
carries has already changed once:

| when | `UNODOS` (boot stick) | `NO NAME` (internal) |
|---|---|---|
| 2026-07-30 | volume 2 | volume 1 |
| 2026-08-05, before reboot | volume 1 | volume 2 |
| 2026-08-05, after ONE reboot | volume 2 | volume 1 |

**They swapped back across a single reboot on the same day** — so an index is
good for exactly one boot. Re-run `vols` after every reboot, before every push.

Following the old "push to volume 2" note on 2026-08-05 would have written the
kernel to the eMMC, which by then held **no BOOTX64.EFI and no BUILD.TXT at
all**. Confirm the target every single time, three ways:

- `disks` → the row ending `1 1` is `is_boot=1`. On 2026-08-05 only TWO disks
  enumerated (`fw0` 61,440,000 boot + `fw1` 61,071,360 eMMC); both 500 GB disks,
  the internal install AND the ZFS pool, were simply gone. Unexplained.
- `vols` → columns are `idx kind writable label` (kind 0=RAM, 1=firmware block,
  2=native USB MSC), NOT `idx disk part`.
- size probe → debug image is multi-MB (4.3 MB on 2026-08-05), production ~1 MB.
  Build the backslash path with `chr(92).join(["EFI","BOOT","BOOTX64.EFI"])` so
  no shell layer can eat the separators.

**`py` (PYRT) hangs when it READS A TELEMETRY FILE — reproduced 2026-08-18.**
Filed to the storage lane in `pc64/UNOAUTOMATE-REQUESTS.md`. Both occurrences
are the same shape: 2026-08-05 was `read_at` on `CRASH\DEFAULTS\BOOTLOG.TXT`,
2026-08-18 was `uno.read(2,'BOOTENV.TXT')`. Each timed out at **~90 s** while
`uno.size`, `uno.read` of `BUILD.TXT`, `vols` and `disks` all answered
instantly, before and after. So it is **not** a general `py` fault and the
2026-08-05 note that it "has NOT reproduced" was only true because nobody read
a log file again.

It is survivable: the bridge queues one verb at a time, so everything behind it
stalls for the full 90 s and then dispatches normally - the link stays up and
no reboot is needed. **Read telemetry with `screen grab` or off a mounted
stick, not through `py`.**

**The methodological lesson, which is the part worth keeping:** I concluded
"detach causes it" because `DETACH.CFG: off` + reboot made `py` work. That test
was CONFOUNDED — flipping DETACH.CFG requires a reboot, and the box is now
detached again with `py` working, so the reboot was the variable. **On this box
almost every config change costs a reboot, so a reboot is a confound in nearly
every experiment; change one thing and re-test the OLD setting too.** The only
property unique to the failing boot was that it was the first boot after a large
write-back-cached kernel push.

**`screen grab` via `zgrab.py` is the working fallback** when `py` is dead: the
rendered UI shows the build's features directly, which beats `BUILD.TXT` anyway.
The System window is the authority on storage posture (`DETACHED (native):` vs
`Native FS:`) — do not infer it from the volume count, though the count does
track it (detached = 3 vols, attached = 5, the USB ones come back).

A verb sent INTO the reboot window blocks the queue for its full ~90 s timeout
before anything behind it dispatches — wait for the `re-dial` line first.

**Quoting the `py` verb from Windows:** send it with a Bash-tool heredoc
(`ssh devbuntu "cat >> ...cmd.txt <<'EOF'"`). A PowerShell here-string piped to
`ssh` prepends a **UTF-8 BOM**, so the bridge reads the verb as `﻿py` and answers
`unknown-verb`; PowerShell backtick-escaped inner quotes mangle into `\\` and the
box answers `SyntaxError`. Both fail loudly and harmlessly, but they cost a
round trip each.

**Build it with `UNO_DEBUG=1 UNO_DETACH=1`.** The debug build sets
`-DUNO_NO_DETACH` by default (finding F8), so a plain `UNO_DEBUG=1` image will
NOT detach and you will be testing the wrong posture.

To also test loadable drivers, `mkdir 2 DRIVERS` then push `SAMPLE.UNO` the same
way — see [[unodos-unodevices]].

**2026-08-23: its framebuffer is UNCACHED, and that is the box's real ceiling.**
`mtrr: none covers fb base - default type 0 (UC - SLOW PRESENT!)` and
`fb bench: vram 30709 KB/s ram 3870362852 KB/s ratio 1:126033`. The desktop is
960x540 scaled to 1920x1080, so a full-screen present writes 8.3 MB at 30 MB/s
= **~270 ms, a ceiling near 4 fps whenever the whole screen dirties**. A normal
desktop only blits dirty rectangles so it is invisible; the Appliances
**Display view rescales the entire guest framebuffer every frame** and hits the
ceiling exactly - which reads as "starting Chromium made the mouse choppy" and
is not the guest's fault. A focused guest also gets 10 ms per frame instead of
4 ms (`SLICE_FOCUS_US`), but that is the smaller half.

**`mtrr-wc` does not fix it and its refusal MISLEADS.** It looks for a UC
*variable* MTRR covering fb_base so it can tile around it, finds none here, and
reports "fb may already be WB/WC (nothing to do)" - which is exactly wrong.
This box is UC by **default type**, a shape F3 never saw ("covered by a variable
MTRR of type UC on every machine tested"). It is also the EASY case: nothing
else covers the range, so one WC variable MTRR over the fb window would do it,
with no tiling. Unwritten as of 2026-08-24; `pc64_mtrr.c`'s own header warns of
bricking risk, so it is opt-in and operator-present by design.

Two more things that cost time on this box and are not faults:
- **`HG*.TXT` in `CRASH\<MACHINE>\` are NOT crashes** - "written unconditionally
  - this is NOT a crash" in their own header. They are boot-log reports.
- **A black Display view early in a guest's life is expected.** The appliance's
  DHCP loop is FOREGROUND by design (a late lease leaves Chromium caching "site
  can't be reached"): 40 tries x ~14 s ~ **9 minutes**, then two more probe
  loops, before the compositor is started at all.
- A `WIFI.CFG` on the stick makes the boot net test take the WiFi branch on this
  wired-only box and print `net test done: WIFI FAIL`, 0.8 s before the r8169
  comes up and leases normally. Cosmetic, but it reads like a failure.
