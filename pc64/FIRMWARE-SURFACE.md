# The firmware surface, every UEFI call pc64 makes and why

Phase C of [docs/DETACH-COMPLETION-PLAN.md](../docs/DETACH-COMPLETION-PLAN.md).

"Traditional OS parity" does not mean zero firmware calls. Every UEFI OS uses
the firmware to boot and keeps runtime services afterwards; Windows and Linux
both do. It means the list is **short and intentional**, and that nothing on it
is there because nobody looked. This file is that list, and it is meant to be
re-derived, not trusted:

```bash
grep -n 'gBS->\|gST->\|rts()->' pc64/*.c
```

Counted 2026-07-30 against the phase-C changes: **26 call sites**, in four
categories. Every one is bootloader-phase, attached-only, a runtime service, or
the detach itself.

## 1. Bootloader phase, before the machine is ours (13 sites)

These run once during `efi_main` / `uno_pc64_init` and never again. The
firmware is by definition the only thing that can answer them.

| Site | Call | Why it is legitimate |
|---|---|---|
| `efi_main` | `SetWatchdogTimer(0,...)` | disarm the 5-minute boot watchdog before it resets us mid-init |
| `con_puts` | `ConOut->OutputString` | text console, used only before the framebuffer exists |
| `uno_pc64_init` | `Stall(50000)` | **the one remaining timing call**, see §5 |
| `uno_pc64_init` | `LocateProtocol(GOP)` | find the framebuffer; there is no other way to get it |
| `connect_all` | `LocateHandleBuffer` + `ConnectController` + `FreePool` | make the firmware bind its own drivers so their descriptors exist to read |
| `collect` | `LocateHandleBuffer` + `HandleProtocol` + `FreePool` | enumerate firmware pointer instances |
| `uno_pc64_init` | `ConIn->Reset`, `HandleProtocol(TextInputEx)` | keyboard bring-up while attached |
| `uno_pc64_boot_dp` | `HandleProtocol(LoadedImage, DevicePath)` | which volume we booted from; guarded by `gDetached` |

`detachgate.c` and `usbboot.c` also call `LocateHandleBuffer` / `HandleProtocol`
to read device paths. Those are descriptor reads, they touch no hardware, and
both files latch their verdict while boot services live precisely so nothing
re-enters the firmware later.

## 2. Attached-only services, dead the moment we detach (5 sites)

A machine that detaches never reaches these again; a machine that stays
attached uses them for its whole life, which is the point of staying attached.

| Site | Call | Notes |
|---|---|---|
| `poll_keys` | `ConIn->ReadKeyStroke` | firmware keyboard. The native path (i8042 / I2C-HID / USB HID) replaces it at detach |
| `efifs_scan`, `fs_root` | `LocateHandleBuffer(SimpleFS)`, `HandleProtocol`, `FreePool` | firmware file system, superseded by the native FAT stack |
| `uno_pc64_pci_disconnect` | `DisconnectController` | returns 0 when detached, since post-EBS this is freed memory (it announced itself as a `#UD` at a stale RIP) |

Pointer polling reads the `EFI_SIMPLE_POINTER` / `EFI_ABSOLUTE_POINTER`
instances collected at init; those protocol pointers are dead after EBS and
`try_detach()` clears `gNPtr` / `gNAbs` so nothing dereferences them.

## 3. Runtime services, legal on both sides of ExitBootServices (6 sites)

Runtime services survive EBS by specification, as long as we stay in physical
addressing (we do; there is no `SetVirtualAddressMap` call anywhere).

| Site | Call | Notes |
|---|---|---|
| `power_down` | `ResetSystem` | the single power policy, §4 |
| `uno_pc64_set_bootnext` | `SetVariable` | debug-only; refuses when detached |
| `write_boot_entry` | `SetVariable` / `GetVariable` | installer `Boot####`; refuses when detached |
| `installer.c` boot-order read | `GetVariable` x2 | same |

**The known limit lives here.** This port declines runtime variable access
after EBS (`uno_pc64_set_bootnext` returns 0 when `gDetached`), so neither the
URC `install <disk>` verb nor the GUI installer can write an NVRAM `Boot####`
entry from a detached machine. Whole-disk installs boot through the
removable-media fallback path instead; an ESP install alongside another OS
needs a boot-menu pick. That is a deliberate policy, not an oversight, and
revisiting it is its own decision.

## 4. The detach itself (2 sites)

`GetMemoryMap` and `ExitBootServices` in `try_detach()`. One retry after a
fresh map, per the spec's key-invalidation rule.

## 5. What phase C actually changed

Three things were using the firmware for no reason:

- **Timing.** `uno_pc64_delay_ms` routed to firmware `Stall` while attached and
  to the TSC only once detached, because the TSC was calibrated near the END of
  init. Everything before that point - the splash, the bring-up stripes, the
  startup chime - therefore rode `Stall`. Calibration only ever needed `gBS`,
  which exists from `efi_main`, so it moved to the top of `uno_pc64_init()`.
  Firmware Stall now has a window of exactly one call: its own calibration.

- **The clock.** `uno_pc64_time` read firmware `GetTime` while attached and the
  CMOS RTC once detached. Same silicon, but the firmware applies timezone and
  daylight-saving fields on top and we do not, so the clock could STEP at the
  moment of detach - and a machine whose clock jumps mid-boot is one whose logs
  cannot be ordered. It is the CMOS RTC on both sides now. That removes a
  runtime-service dependency as a side effect.

- **Power.** Shutdown and restart disagreed about ordering and about which
  fallback they had: restart only tried CF9 when already detached, shutdown
  only tried ACPI S5 after `ResetSystem` returned. They are one `power_down()`
  with one rule now: **attached prefers the firmware, detached prefers our own
  registers, and either way the other one is tried before halting.**

  Note this is deliberately NOT the plan's "prefer native CF9" as written.
  While the firmware is alive, `ResetSystem` is also its chance to flush a
  pending NVRAM write and run whatever the platform does on the way down; CF9
  is a hard platform reset that skips all of it. Post-EBS the argument reverses
  and native goes first. One policy, expressed once, which is what the plan was
  asking for.

Also removed: the no-GOP panic loop span `for(;;) gBS->Stall(1000000)`, which
burned firmware calls forever on a machine that was already dead. It halts.

## 6. What is deliberately NOT on the roadmap

**A native display driver.** Post-detach we keep scanning out of the
GOP-negotiated linear framebuffer, which is hardware state, not a firmware
service. This is exactly what Windows' Basic Display Adapter does on UEFI. A
native modeset driver means per-generation PLL / transcoder / DDI programming
per vendor, and the only things it would buy are resolution switching after
detach and support for BltOnly firmware (rare on x86, none known in the fleet).
Out of scope, and `gUseBlt` machines stay gated for that reason.
