# HWWATCHDOG.md — the PCH TCO hardware-watchdog primitive

**Owner:** unodevices (registry row: `uno_devmgr.*`; this primitive is
`uno_hw_wdt.{c,h}`).
**API version:** `UNO_HW_WDT_API 1`. The four-call surface (§2) is `[STABLE]`;
the chipset coverage (§4) is `[EXPERIMENTAL]` and grows per-generation. Breaking
changes bump the number and get a dated changelog entry (AGENTS.md §6).

`uno_hw_wdt` is the **hardware backstop for the software guard**. The
host-attested guard in `uno_debug.c` (unoautomate's lane — see REMOTE.md "The
guard") resets a wedged box from whichever context is still alive: the main-loop
heartbeat, the detached LAPIC-timer ISR, or the attached firmware timer +
`SetWatchdogTimer`. All three need the CPU still taking interrupts or still
cycling TPL. The one wedge class none of them catch is a **tight spin with
interrupts disabled** (`cli; for(;;){}`) or a true bus hang: no ISR fires, no
loop runs, no TPL cycles, nothing resets. Only separate silicon that ignores CPU
state can rescue that — the **Intel PCH TCO watchdog** — and that is this file.

---

## 1. Why it lives in unodevices, not unoautomate

The TCO is a PCH device reached through the LPC/SMBus function and the RCBA/PMC
register windows — chipset enumeration, which is exactly what `uno_devmgr`
already does. unoautomate *consumes* this primitive the same way it consumes any
device capability; it does not own the silicon. Correspondingly, `uno_hw_wdt`
does not own the guard: it exposes four calls and unoautomate wires them into the
guard lifecycle (§5).

## 2. The surface `[STABLE]`  (`uno_hw_wdt.h`)

```c
int  uno_hw_wdt_present(void);          /* 1 iff a usable TCO was found + is arm-able */
void uno_hw_wdt_arm(unsigned seconds);  /* start/reset the countdown to ~seconds       */
void uno_hw_wdt_pet(void);              /* reload (kick) the countdown                  */
void uno_hw_wdt_disarm(void);           /* halt it                                      */
int  uno_hw_wdt_status(char *buf, int cap);  /* one-line introspection (no side effect) */
```

Everything is `#ifdef UNO_DEBUG`; in a production build each call is a no-op
macro, so the shipped OS is untouched (the object compiles to ~0 bytes — the
build gate checks this). Discovery is lazy and cached: the first call to any of
`present`/`arm`/`status` runs it once.

### The honesty contract (the load-bearing rule)

`uno_hw_wdt_present()` returns 1 **only after** a usable TCO was located AND its
chipset NO_REBOOT bit was cleared *and read back clear*. A firmware that locks
NO_REBOOT, or a chipset whose NO_REBOOT home this driver does not yet know how to
reach, yields `present()==0`. The driver never programs a timer it cannot prove
will actually reset the board — a watchdog that silently never fires is worse
than none, because the operator trusts it. `arm`/`pet`/`disarm` are all no-ops
while `!present()`.

## 3. How it resets the box (the make-or-break details)

Reference: Linux `drivers/watchdog/iTCO_wdt.c` + `drivers/mfd/lpc_ich.c`, and the
Intel ICH9 / 5-series-PCH datasheets.

1. **NO_REBOOT.** Firmware normally sets the chipset's NO_REBOOT bit so a TCO
   timeout counts down but never resets. We MUST clear it; its home is
   generation-specific (§4). We clear it and **read it back** — a locked or
   unreachable bit fails the check and drops us to `present()==0`.
2. **TCOBASE.** The TCO I/O block base. On the v1/v2 parts it is the LPC ACPI
   base (LPC config `0x40`, a.k.a. PMBASE/ABASE, bit 0 = decode enable) `+ 0x60`.
   Found via the enumerated device tree — `devmgr_find_class(0x06,0x01)` locates
   the PCH LPC/ISA-bridge function; a direct class scan is the fallback.
3. **v1 vs v2 register layout differs** and we branch on it. TCO regs, relative
   to TCOBASE: `TCO_RLD` 0x00, `TCOv1_TMR` 0x01 (6-bit), `TCO1_STS` 0x04
   (bit 3 = TIMEOUT), `TCO2_STS` 0x06 (bit 1 = SECOND_TO, bit 0 = BOOT),
   `TCO1_CNT` 0x08 (bit 11 = TCO_TMR_HLT), `TCOv2_TMR` 0x12 (10-bit).
4. **Two-timeout behaviour.** A classic TCO reboots only on the SECOND uncleared
   timeout; the first just latches status (and, if TCO SMI is enabled in
   firmware, raises an SMI notification — which does *not* gate the reboot). So
   `arm(seconds)` programs each single-timeout period near `seconds/2`, so two
   expiries land close to the requested backstop. We deliberately leave the
   firmware's TCO SMI routing (`SMI_EN.TCO_EN`) untouched: the second-timeout
   reboot does not depend on it, and disturbing firmware SMIs is riskier than
   sizing for two timeouts.
5. **Reload / halt.** Program the period into `TCO_TMR`, write `TCO_RLD` to load
   the down-counter, clear `TCO1_CNT.TCO_TMR_HLT` to run. `pet` clears the
   first-timeout latch and rewrites `TCO_RLD`. `disarm` sets `TCO_TMR_HLT`. Tick
   granularity is ~0.6 s; `seconds → ticks = seconds·5 / (2·3)`, clamped to the
   generation's 6-/10-bit range.
6. **Firmware take-over.** Firmware may be using the TCO for its own watchdog
   while attached. Discovery halts it, clears stale status, and reprograms it
   cleanly. Post-detach it belongs entirely to the OS.

## 4. Coverage `[EXPERIMENTAL]` — what `present()` returns where

| Gen | TCOBASE | NO_REBOOT home | Supported |
|---|---|---|---|
| **v2** — ICH6 … ~6-series PCH (incl. QEMU q35 `ich9-lpc`) | LPC ACPI+0x60 | **RCBA GCS** (RCBA+0x3410, bit 5 / mask 0x20) — MMIO, read-back verifiable | **yes** |
| **v3** — Skylake … **Comet Lake** PCH-LP (400-series) | LPC ACPI+0x60 | **PMC `GEN_PMCON_A`** (PWRMBASE+0x1020, bit 1 / mask 0x02) — MMIO, read-back verifiable | **yes** |
| v1 — pre-ICH6 (no RCBA) | LPC ACPI+0x60 | no clean NR bit | no → `present()==0` |
| SoC parts (Apollo/Gemini Lake) | SMBus/PMC GCR | PMC GCR `PMC_CFG` (bit 4) — not yet implemented | no → `present()==0` |

Both the **v2 / RCBA-GCS** path (ICH6…6-series PCH, QEMU `ich9-lpc`) and the
**v3 / PMC GEN_PMCON_A** path (Skylake…Comet Lake PCH-LP) are fully implemented
and read-back-verified. The v3 register locations are: `GEN_PMCON_A` at
`PWRMBASE + 0x1020`, where PWRMBASE is the PMC function's (00:14.2, `8086:02ef`
on the CML-U Yoga; class 05/00) BAR0 if it reads back a sane high window, else
the 400-series fixed base `0xFE000000`; the no-reboot bit is bit 1 (mask 0x02),
matching Linux `iTCO_wdt`'s value for this memory-mapped PCH class. The driver
**requires an Intel PMC function to be enumerated** before touching any PWRM
address, so it never pokes a fixed MMIO on a chipset that isn't this family.

The **X13 Yoga** (Comet Lake-U) is now a supported v3 target: `present()==1` and
the guard arms the TCO. The **ZimaBlade** (SoC PMC GCR) is a distinct method
still on the follow-up list → `present()==0` there (honest, per §2). The
`status`/`hwwdt status` line dumps the raw `GEN_PMCON_A` value (`fw=0x…`) so a
new chipset's firmware NO_REBOOT bit is diagnosable rather than silent.

## 5. Integration with the guard `[WIRED]`

`uno_hw_wdt` ships as a **strong symbol**; `uno_debug.c` carries a local
**weak-stub declaration** plus the call sites, the same weak-symbol seam used for
`r8169_dbg_cmd` and `devmgr_list_str` — so the tree links green with or without
the module. Wired into the guard lifecycle (`uno_debug.c`):

- `uno_dbg_guard_arm(t_ms)`  → `guard_hw_wdt_arm(t_ms)` → `uno_hw_wdt_arm(t_ms/1000 + 8)` **iff `uno_hw_wdt_present()`**
- `uno_dbg_guard_pet()`      → `uno_hw_wdt_pet()`
- `uno_dbg_guard_clear()`    → `uno_hw_wdt_disarm()`

The TCO window is the **guard timeout + an 8 s margin** (`GUARD_HW_WDT_MARGIN_S`),
so the three software firing paths (main-loop heartbeat / LAPIC ISR / UEFI
`SetWatchdogTimer`) always fire first and the TCO only catches what they
structurally cannot — the IRQs-off spin. The guard arms the TCO only when a
usable one is present, so on a box where `present()==0` the guard behaves exactly
as before. This is unoautomate territory (`uno_debug.c`); the two lanes landed
together per the request in `UNOAUTOMATE-REQUESTS.md`.

## 6. The TCO in the device tree

On successful discovery the driver registers the TCO as a `UNO_BUS_PLATFORM`
node under its backing LPC function via `devmgr_add_platform()` (a sticky
registration that survives a re-scan). It lists as:

```
00:1f.0 8086:xxxx 08/80 system tco-wdt
```

i.e. a device bound to the `tco-wdt` driver, its 32-byte TCO I/O block recorded
as an I/O BAR, parented to the LPC bridge. This is the first `UNO_DEV_BOUND`
node the registry produces (phase-1 PCI enumeration binds nothing); see
DEVICES.md §2. It appears only when a usable TCO was found — never for an
absent/unsupported one.

## 7. Gates

- **`tools/hwwdt_test.sh` / `tools/hwwdt_test.c`** — host gate: `uno_hw_wdt.c`
  linked against a synthetic south bridge (fake PCI config + TCO I/O block + GCS
  and GEN_PMCON_A MMIO + a CML PMC function), run natively. Asserts the NO_REBOOT
  clear+read-back on **both** the v2 (RCBA-GCS) and v3 (PMC GEN_PMCON_A) paths,
  the seconds→ticks two-timeout halving, the HLT run/halt/reload sequencing, the
  device-tree registration, and the honest-absent paths (locked NO_REBOOT on v2
  *and* v3, non-Intel LPC, ACPI decode off, no-RCBA-no-PMC, no LPC). Eight
  scenarios, seconds, no QEMU. Run after every edit to `uno_hw_wdt.c`.
- **`tools/hwwdt_qemu.py`** — smoke test on QEMU q35's `ich9-lpc` (a v2 TCO).
  Arms a short TCO with `-no-reboot` and asserts QEMU exits (a TCO reset), with a
  no-key control that stays up. Exercises the v2 path end to end.
- **Metal (v3 / CML) — the real gate for the Yoga.** An IRQs-off wedge
  (`cli; for(;;){}`) on the live Comet Lake-U X13 Yoga that defeats the software
  guard, reset by the TCO. Repro (URC): `iwl rerun`, `guard 40 reboot`,
  `iwl mvm 1..8,a,b`, then `iwl mvm c` (the ADD_STA IRQs-off wedge) → the box
  should TCO-reset and re-dial URC on its own. The `hwwdt status` line's
  `fw=0x…` dump confirms `present()==1` and the firmware NO_REBOOT bit before the
  wedge; see the changelog for the run status.

## 8. Territory (AGENTS.md §1–2)

Own: `uno_hw_wdt.{c,h}`, `HWWATCHDOG.md`, `tools/hwwdt_test.{c,sh}`,
`tools/hwwdt_qemu.py`, and the additive `devmgr_add_platform` in
`uno_devmgr.{c,h}`. Consume unchanged: `pc64_pci.c` (config accessors), the
`uno_devmgr` tree. Additive-only seam touch: the `build.sh` file list (one line
in the DEBUG block), the boot-selftest call in `uefi_main.c`. The guard call
sites in `uno_debug.c` (the weak-symbol seam + the three lifecycle calls) are
**unoautomate's lane** — edited here only because this task was assigned both
lanes; see `UNOAUTOMATE-REQUESTS.md`.

---

## Changelog

- **2026-07-24 — v3 / Comet Lake PMC path + guard wiring (no API bump).** Added
  the **v3** generation: NO_REBOOT in the PMC `GEN_PMCON_A` register
  (`PWRMBASE + 0x1020`, bit 1 / mask 0x02), PWRMBASE from the enumerated Intel
  PMC function's BAR0 or the 400-series fixed `0xFE000000`, gated on an actual
  PMC being present. This makes the **Comet Lake-U X13 Yoga** a supported target
  (`present()==1`). The guard now arms/pets/disarms the TCO via a weak-symbol
  seam in `uno_debug.c` (guard window + 8 s), so the IRQs-off wedge the software
  guard misses is caught by hardware. Host gate grew v3 `cml` / `cml-locked`
  scenarios (8 total, all green). `status` now dumps the raw `GEN_PMCON_A`
  firmware value for on-chip diagnosis. Metal validation on the live Yoga: see
  the run note below / `UNOAUTOMATE-REQUESTS.md`.
- **2026-07-24 — UNO_HW_WDT_API 1 (initial):** the TCO primitive lands. Four-call
  surface (`present`/`arm`/`pet`/`disarm`) + `status`; UNO_DEBUG-gated, prod
  no-op. Fully-implemented and read-back-verified NO_REBOOT clear on the
  **v2 / RCBA-GCS** generation (ICH6 … 6-series PCH, and QEMU `ich9-lpc`);
  honest `present()==0` on v1 (no RCBA) and on the Skylake+/SoC **PMC** parts
  whose NO_REBOOT home is not yet implemented (the Yoga and ZimaBlade targets —
  PMC path is the next slice). Registers the TCO as a `UNO_BUS_PLATFORM` node in
  the `uno_devmgr` tree via the new additive `devmgr_add_platform()`. Host gate
  `tools/hwwdt_test.*`; QEMU smoke `tools/hwwdt_qemu.py`.
