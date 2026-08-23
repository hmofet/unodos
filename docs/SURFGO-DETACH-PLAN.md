# Surface Laptop Go: full firmware detach and native xHCI, once and for all

Lane: **usb stack** (`xhci.*`, `usbio.*`, `usbhid.*`, `usbmsc.*`, `usbboot.*`,
contract `pc64/USB.md`), branch `surfgo-xhci`, worktree `unodos-surfgo`.
Everything outside that row is consumed; changes to the detach gate,
`uefi_main.c` boot wiring and the debug harness are filed as requests in
`pc64/UNOAUTOMATE-REQUESTS.md` (claim dated 2026-08-23) and kept additive.

Written 2026-08-23 from three fresh boots of build `debug-local-20260823-0326`
(master `be1cf7a4`) on the machine, the July metal archive, and a read of the
whole detach path. Supersedes the open question in `docs/SURFACE-KEYBOARD.md`.

## 1. What the evidence says (read this before touching code)

### 1.1 The keyboard question is closed: it is USB

The firmware's own UsbIo enumeration on this machine, identical in July and
today:

```
usb[0] 04f3:0c5b class 03/01      ELAN keyboard (HID boot interface, kbd)
usb[1] 04f3:0c5b class 03/01      ELAN touchpad  (HID boot interface, ptr)
usb[2] 05c8:03e2 class 0e/01      camera
usb[3] 05c8:03e2 class 0e/02
usb[4] 8087:0026 class e0/01      Bluetooth
usb[5] 8087:0026 class e0/01
usb[6] 18a5:0250 class 08/06      the Verbatim boot stick (BOT)
```

and the gate agrees: `detach gate: fw inst=1/3 ptr=usb kbd=usb  survives
ptr=1 kbd=1 strand=0`, `usb-hid preflight: kbd=1 ptr=1`,
`usbboot: usb=1 bot=1 matched=1 ok=1 (path matched)`.

Not SAM, not the I2C-HID device at `\_SB.PCI0.I2C2` slave 0x34 (that one
never answers and is a separate, non-blocking F4 item). **Every device a
detached Surface needs is behind one controller: the Ice Lake-LP xHCI,
`8086:34ed`.** The SAM sizing section of `SURFACE-KEYBOARD.md` is refuted by
this and should be marked so.

### 1.2 The machine detaches, then dies after the splash says "starting desktop"

Three boots today:

| boot | DEBUG.CFG / DETACH.CFG | result |
|---|---|---|
| 1 | `vm-selftest` armed | hangs on "starting desktop" |
| 2 | `vm-selftest` removed | hangs on "starting desktop", identical |
| 3 | `DETACH.CFG: off` | desktop at 8 s, trackpad drags, keyboard works (firmware path) |

"starting desktop" is painted **after** `try_detach()` returns
(`uefi_main.c:1387-1392`), and a stranded stick paints "detach failed - power
off" instead. So on boots 1 and 2: `ExitBootServices` succeeded, `xhci.c`
took the controller, `usbmsc` bound the stick well enough to read LBA 0 with
a `55 AA` signature, `uno_fat_native_eligible()` said the system volume was
back, `uno_usb_hid_init()` returned, and THEN the machine stopped. The
telemetry on disk is the pre-detach copy; the post-detach write never landed.

The window it died in (`uefi_main.c:1394-1433`, debug build), in order:

1. `uno_dbg_envblock()` - reads xhci status, harmless
2. `uno_dbg_on_detach()` - IDT, mask PICs, LAPIC enable + 50 ms calibration, watchdog
3. `uno_dbg_check("init:done")`, `uno_dbg_log(...)`
4. `uno_dbg_write_bootenv()` / `uno_dbg_write_bootlog()` - **the first native
   `WRITE(10)` ever issued on this controller**, through `usbmsc` over `xhci.c`
5. `uno_hw_wdt_boot_selftest()` - no-op without its key
6. `splash_stage(4, "security (accounts / RBAC)")` - never seen

Two live hypotheses, both testable in one boot with the Phase 0 tracer:

- **H1: the USB write path.** `msc_rw` write -> `bot_cmd` -> bulk-out + CSW.
  `poll_xfer` is bounded per iteration but loops on "stray" events up to 4096
  times at 5 s each; a SuperSpeed stick (`18a5:0250` is USB 3) that storms
  Port-Status-Change events, or a halted bulk-out endpoint whose recovery
  loops, looks exactly like a brick (`xhci.c:413-425`, `:849-852`,
  `usbmsc.c:184-202`). Reads worked (LBA 0 proof read); writes are
  unexercised on this silicon.
- **H2: LAPIC bring-up on this firmware.** `uno_dbg_on_detach()` runs `cli`,
  `lidt`, masks the PICs, flips MSR 0x1B, calibrates against the TSC. Proven
  on Apollo Lake and Comet Lake; never on Ice Lake with whatever interrupt
  state this firmware leaves behind.

Either way, the takeover itself (H0: "xHCI cannot be driven on Ice Lake")
is **already partly refuted**: the controller reset, ran, enumerated the
stick and read from it. What is unproven is writes, HID input, and stability.

### 1.3 What the stack is missing that matters here

From `pc64/xhci.c` as of `be1cf7a4` (all confirmed by reading, not recalled):

- **No USBLEGSUP handoff.** No extended-capability walk, no OS-owned
  semaphore, no SMI disable (`USBLEGCTLSTS`). `METAL-CHECKLIST.md:323-325`
  already names this as the prime suspect for "QEMU works, metal does not".
  With BIOS SMIs still enabled on the controller, every port change, every
  command completion or an OS-ownership request can trap into SMM, and a
  pending-SMI controller is one plausible source of a machine that simply
  stops. The two machines that detach today (Apollo Lake ZimaBlade, Comet
  Lake Yoga) happened not to need it.
- **No PCI power state check** (PMCSR D3 -> D0). A controller left in D3hot
  reads `0xFFFFFFFF`; `xhci_bringup` then burns 5 x ~5 M spins silently.
- **Controller selection is "first class 0C/03/30 in bus/dev/fn order"**
  (`find_xhci`, `xhci.c:222-236`). Ice Lake-U parts carry a CPU-side
  Thunderbolt xHCI (`8086:8a13` at `00:0d.0`) that enumerates BEFORE the PCH
  controller (`00:14.0`). On this machine the stick came back, so either
  `8a13` is absent here or the order happened to work, but the boot path's
  own PCI node is known to `usbboot` and should be what `xhci.c` opens.
- **Untimed waits.** CNR / HCRST / HCH / port reset are spin counts
  (`xhci.c:1104-1145`, `:354`), the exact bug `USB.md:225-232` already fixed
  once for transfers. `scsi_ready()` can take tens of minutes in the worst
  case (`usbmsc.c:155-165`).
- **Zero `uno_dbg_log()` calls in `xhci.c`.** All 26 diagnostics go to
  `outb(0x402)`, which is compiled out on metal and SMM-trapped on this class
  of firmware. The single most useful post-mortem signal (`hubport[N]
  CONNECTED BUT NOT ENUMERATED`) only reaches disk if the disk came back.
- **No IDT and no watchdog during the takeover.** `uno_dbg_on_detach()` runs
  after `try_detach()` returns; the whole xHCI/usbmsc/usbhid bring-up is in
  the blind window where a fault triple-faults with no report
  (`uefi_main.c:1401-1412`).
- `IMAN.IE` set but `USBCMD.INTE` never; `USBSTS.EINT` never acked. Harmless
  while polled, a trap the moment anything enables interrupts.

### 1.4 The diagnostic channel this machine actually has

No serial. WiFi (AX201 hw_rev 0x332) has never reached ALIVE here, so URC is
out. The 0x402 debugcon is unsafe on Lenovo/Surface firmware. What remains:

- **The framebuffer.** Linear, at `0x40_0000_0000`, still ours after EBS.
  `splash_stage()` already draws text on it post-detach; that is the serial
  port.
- **The RAM stash at `0x01F00000`** (`stash: ram @1f00000` on every Surface
  boot). `uno_dbg_log()` writes every line into it; after an **unclean warm
  reset** the next boot flushes the previous boot's tail as
  `CRASH\SURFGO\RS###` / `HG###` (`uno_debug.c:1657-1708`). A cold
  power-button cycle loses it, which is why boots 1 and 2 left nothing. So
  the machine has to reset **itself**: the LAPIC watchdog's `HG` path does
  exactly that (`hang report -> stash -> CF9`), provided it is armed before
  the hang and CF9 actually resets this box (F14, reopened, metal-pending).
- **`\LOGS\LOG.CFG` with `level=debug`** turns the System Log app into an
  on-screen kernel-log viewer once the desktop is up. Useful for everything
  after the first successful detach, useless for the hang itself.

### 1.5 The iteration cost, and why every boot must over-report

A metal boot costs a human round trip: stick from carbon to the Surface,
boot, watch, hold power, stick back to carbon. Nothing in QEMU can stand in
for an Ice Lake xHCI or this firmware's SMM. The plan is therefore built so
that **one boot answers several questions at once**, and so that a hung boot
still reports.

## 2. The plan

### Phase 0: make a hung boot talk (no behaviour change; one boot to validate)

All additive, all debug-build-only where it touches shared files.

0a. **Post-detach screen tracer.** A `splash_stage(4, ...)` line before each
    step in the "starting desktop" window (`envblock`, `watchdog/IDT`,
    `telemetry write`, `security`) so the frozen splash names the step. One
    more line painted right after `try_detach()` returns with the takeover's
    verdict: `xhci 00:14.0 8086:34ed present=1 err=0 ports=N devs=N
    msc=ok|<why> hid kbd=N ptr=N`. Owner of the text: this lane (the string
    comes from `uno_xhci_status` + `uno_usbmsc_why` + `uno_usb_hid_*`); the
    two `splash_stage` calls are an append in `uefi_main.c` (request filed).

0b. **Arm the IDT + watchdog right after `ExitBootServices`**, before
    `uno_blk_detach()`, instead of after `try_detach()` returns. Then a fault
    during the takeover is a `CR` report with a RIP, a spin is an `HG` report
    with the log tail, and the watchdog's CF9 reset carries both to the next
    boot. `uno_dbg_on_detach()` needs only the TSC and the LAPIC, both of
    which are already native at that point. The `vm-selftest` ordering
    comment (`uefi_main.c:1401-1412`) is satisfied either way. Debug-harness
    lane: request filed; this plan does not edit `uno_debug.c`.

0c. **`uno_dbg_log()` at the six decision points of `xhci.c`** (controller
    chosen: bdf, vid:did, BAR, PMCSR; bring-up stage + USBSTS on every
    failure; per root port: PORTSC; per device: vid:pid speed tier slot cc;
    per hub port; bulk error: cc epstate) and the three of `usbmsc.c`
    (bind target, scsi_ready outcome, first write outcome). These ride the
    stash. Also ack `USBSTS.EINT` so the register read in the log is honest.

0d. **Pre-detach PCI inventory line in the env block**: every class 0C/03
    function with vid:did, bdf, prog-if, BAR0, PMCSR.D and whether it is the
    one in the boot path. Answers the `8a13` question without a detach.

0e. **`DETACH.CFG: nowrite`** (this lane's reader is `cfg_word`; request): a
    one-word switch that makes the post-detach telemetry writes skip, so H1
    (first write hangs) vs H2 (LAPIC) is separable in a single boot pair
    without a rebuild.

0f. **Collection sequence written on the stick** (`CRASH\README.TXT`): wait
    150 s on a frozen splash (watchdog grace is 120 s), let the box reset
    itself, boot once more with `DETACH.CFG: off`, then pull the stick. A
    machine that does NOT self-reset in 150 s is itself a finding (CF9 dead:
    F14), and is the one case where the screen line from 0a is all we get.

Exit criterion: a hung Surface boot produces either `HG###` with an xhci log
tail or a screen line naming the step. Verified by ONE boot.

### Phase 1: the takeover, done the way real hardware expects (xhci.c, own lane)

Gated on the Phase 0 readout, but all four are correct regardless and
cheap, so they ship together in the same stick as Phase 0's second boot:

1a. **USBLEGSUP handoff** (`HCCPARAMS1[31:16]` -> xECP -> cap id 1): set OS
    Owned (bit 24), wait up to 1 s for BIOS Owned (bit 16) to clear, proceed
    anyway on timeout (Linux does), then clear every SMI enable in
    `USBLEGCTLSTS` and write-1-clear its status bits. Before the halt in
    `xhci_bringup`. Spec: xHCI 1.2 §7.1. Linux reference:
    `drivers/usb/host/pci-quirks.c quirk_usb_handoff_xhci`.
1b. **PCI sanity before register waits**: read PMCSR, force D0, wait 10 ms;
    verify BAR0 decodes (CAPLENGTH not 0xFF); fail fast with `err=5`/`err=6`
    instead of burning five million-spin loops.
1c. **Open the controller the boot path names.** `usbboot` already walks the
    boot device path to a PCI node; export that bdf and have `uno_xhci_init`
    prefer it, falling back to the scan. Log which one was taken.
1d. **Deadlines are durations**: CNR 5 s, HCRST 1 s, HCH 16 ms, port reset
    100 ms, via `uno_pc64_delay_ms` (TSC), mirroring the 2026-07-29 fix for
    transfers. Budget `scsi_ready()` to 5 s wall total. Cap `poll_xfer`'s
    stray loop by time as well as count.

Also in scope here if Phase 0 points at it: bulk-out/CSW ordering on
SuperSpeed (H1), and `Stop Endpoint` before reset on a timed-out write.

Exit criterion: `telemetry: post-detach (final)` on the stick, written by
`usbmsc` over `xhci.c` on the Surface, with `xhci: present=1 ... devs>=4`.

### Phase 2: input on the detached machine (usbhid.c, own lane)

The ELAN part is ONE device with two HID boot interfaces (kbd protocol 1,
ptr protocol 2), exactly the wireless-combo shape `xhci.c:165-175` was fixed
for. Expect it to work; verify on the desktop: type in Notepad, drag a window,
Alt and Win chords (`METAL-CHECKLIST.md:52-62`), touchpad motion scale. If
the touchpad's boot-protocol report is a mouse report with an odd stride,
`usbhid.c:199` is the place. Also confirm the camera and Bluetooth interfaces
are left unclaimed and do not stall enumeration (`MAX_DEV` is 16; fine).

Exit criterion: typing and pointing on a detached Surface; `usb-hid: kbd=1
mouse=1` in the final env block.

### Phase 3: make it stick

3a. **F14**: Restart and Shut Down on a detached Surface (CF9 -> i8042 -> FADT
    path, `53d746ed`, metal-pending). Phase 0 already tells us whether CF9
    works; close the finding either way.
3b. **eMMC** (`sdhci.c`, unofs lane): with the USB stick detaching, the eMMC
    is no longer on the critical path. Read the System window STORAGE row
    once detached: `sdhci@XX.X` present means an internal install is a
    follow-up; absent means the Ice Lake eMMC is ACPI-only and gets its own
    request, not this plan.
3c. **Docs**: `SURFACE-KEYBOARD.md` marked answered (USB), `METAL-CHECKLIST`
    Surface rows ticked or re-worded, `USB.md` changelog entries for 1a-1d,
    `METAL-FINDINGS.md` F-entry for whatever H1/H2 turned out to be.
3d. **Regression gate**: QEMU `diskboot_test.py` (usb-storage over
    qemu-xhci) stays green through every commit; QEMU has no legacy
    ownership capability, so 1a is exercised only on metal and must be a
    no-op when the capability is absent.

### Phase 4: the thing the user actually wanted

Chromium in the appliance on the Surface. Once detached, `HV: eligible`
flips; remaining known risks, to be confirmed on the first detached boot and
NOT solved in this lane: the carve is **768 MB** on a 3.4 GB machine (the
harness ran the guest with 2 GB; Chromium in 768 MB is doubtful, unovirt
lane), `ROOTFS.IMG` is 911 MB read over `usbmsc` (fine if Phase 1 holds),
and the guest's network bridge wants a working NIC, which on this machine is
the AX201 that has never come ALIVE (F12 follow-up, NIC lane). Those are
three separate requests once Phases 0-3 land; this plan's job is to get the
machine to the point where they can be tried at all.

## 3. Boot budget

| boot | stick carries | question answered |
|---|---|---|
| A | Phase 0 (tracer, early watchdog, xhci logs, inventory) | which step hangs; does CF9 work; is `8a13` present; the takeover log |
| B | Phase 0 + Phase 1 | detached with telemetry written natively |
| C | + `nowrite` only if B still hangs | H1 vs H2 |
| D | Phase 2 | typing on a detached Surface |
| E | Phase 3a | Restart / Shut Down close F14 |

Five boots is the plan; three is the hope. Each boot's stick is prepared on
carbon with the in-place update (`tar` onto `D:\`, `DEBUG.CFG` rewritten,
`CRASH\SURFGO` cleared first so the reports are unambiguous), and each
boot's collected `CRASH\SURFGO\` is archived under
`~/unodos-metal-reports/surfgo-2026-08-<n>/` before the next.

## 4. Out of scope, deliberately

- A SAM / Surface Serial Hub driver (refuted by §1.1).
- The I2C-HID device at slave 0x34 (F4; a second pointer at best).
- AX201 firmware load on hw_rev 0x332 (F12 follow-up).
- Any change to the default detach policy for other machines.
