# unodevices - implementation progress

> **SUPERSEDED IN PART, 2026-07-30 (branch `detach-bcd`): phases 2 and 4 are
> BUILT and QEMU-green.** The sections below describing them as "DESIGNED, not
> yet built" are of their moment. What actually landed, and where it differs:
>
> - **Phase 2 is on master's successor branch, not just designed.** The
>   `UNO_DRIVER` seam, specificity matching, probe-decline and the fixpoint
>   `devmgr_bind_all()` are in `uno_devmgr.c`; `UNO_DEVMGR_API` is 2. The
>   sample driver this doc describes as living inside `uno_devmgr.c` under
>   `UNO_DEBUG` does NOT exist in that form - the phase-4 work made it a real
>   loadable module instead, `\DRIVERS\SAMPLE.UNO`, which is a stronger proof
>   because it exercises the loader as well as the registry.
> - **Driver adoption happened here rather than per-lane**, for the built-in
>   PCI drivers (e1000, ahci, nvme, sdhci, hdaudio, ac97, r8169), with one
>   deviation from the recipe below: the legacy `pci_find` call was NOT deleted.
>   The registry is consulted first and the scan is the fallback. Deleting it
>   changes when a driver touches hardware on paths only metal can exercise.
>   See DEVICES.md 5.
> - **Phase 3 is still not built**, and phase 4's USB hotplug half depends on
>   it: `devmgr_rescan()` diffs the PCI tree and nothing calls it periodically.
>   The mechanism and the remove contract are in place for phase 3 to use.
>
> Verified by `tools/devmgr_qemu.py`, which now asserts real bindings per
> function rather than their absence.

> **Salvaged from the `unodevices` branch, 2026-07-30.** The branch is gone; the
> work reached master by a different route and master's `uno_devmgr.c` is well
> ahead of what this describes (641 lines to the branch's 433, including
> `devmgr_add_platform`). Kept because the engineering record below is not
> written down anywhere else: what was verified, how, and the reasoning behind
> the COFF grouped-section idiom, the match-specificity scoring and the guarded
> BAR sizing. Statements about the branch being unmerged or land-ready are of
> their moment and no longer true. The live contract is
> [pc64/DEVICES.md](../pc64/DEVICES.md), which is at API version 1.

Tracks what landed against [UNODEVICES-PLAN.md](UNODEVICES-PLAN.md).

## Phase 1, PCI enumerator + tree + introspection, DONE, QEMU-verified

- `pc64/uno_devmgr.c` / `.h`: static-pool device tree; recursive PCI scan
  following type-1 bridges; per-function capture (IDs, class/subclass/prog-if,
  subsystem, IRQ, MSI/MSI-X/PCIe cap offsets, all BARs with 64-bit pairing +
  guarded sizing); class-code name table; flat introspection; text dump.
- Guarded BAR sizing disables command decode around the size probe and skips
  the scanout device (display-class OR a BAR that contains the GOP
  framebuffer) so it never glitches the live framebuffer.
- `uefi_main.c`: `uno_devmgr_init()` right after `connect_all()`; the
  `CRASH\<machine>\DEVICES.TXT` dump (+ debugcon mirror) after `uno_fat_init()`,
  `UNO_DEBUG` only.
- `mod_uno.c` + `pc64_modload.c`: `uno.devices()` / `uno.pci()` bindings.
- `tools/devmgr_qemu.py`: q35 acceptance. PASS, host/isa/sata/ethernet/xhci
  enumerated, boot byte-identical.

## Phase 2, driver registry + fixpoint binding, INFRA DONE, QEMU-verified

Landed and QEMU-verified, entirely within the `unodevices` lane
(`uno_devmgr.*`) plus append-only edits to the shared registration seams:

- **Registry** (`uno_devmgr.h/.c`): `uno_driver` + `uno_match`, the
  `UNO_DRIVER()` COFF grouped-section macro (`.unodrv$a/$m/$z` markers, no
  constructors under `-nostdlib`), match-specificity scoring (exact id >
  class/subclass/progif > class), probe-decline, and the multi-pass fixpoint
  `uno_devmgr_bind_all()`. It is a **self-registering seam** (AGENTS.md §2): a
  driver opts in by adding a `UNO_DRIVER()` line in ITS OWN file; nothing in a
  driver's lane is edited from here.
- **Proof**: a debug-only **sample driver** (in `uno_devmgr.c`, `UNO_DEBUG`
  only) claims the SMBus function, exercising the whole path - linker-set
  iteration, match, probe, BOUND state, `uno.devices()` name introspection.
  `tools/devmgr_qemu.py` asserts `smbus -> sample` bound, all real-hardware
  lanes UNCLAIMED. In the production OS the sample is compiled out, so the
  shipped registry binds nothing until a real driver opts in.
- Bind loop runs at init right after enumeration.

### Driver adoption is per-owner (AGENTS.md lane rule)

The `unodevices` lane owns the *mechanism*. Each hardware driver
(`e1000*`, `ahci`, `nvme`, `iwlwifi`, ...) lives in its own lane, so **its
owner** adopts the registry - we do not edit those files from here. The
adoption recipe to hand them (or file as a request):

1. `#include "uno_devmgr.h"`.
2. Add `static pci_dev g_dev; static int g_claimed;` (or reuse the driver's
   existing `g_pci`).
3. Write `X_probe(uno_device *d)` recording `d->id.pci.{bus,dev,fn,...}`,
   returning 1. **NICs**: record only - bring-up stays lazy in
   `pc64_net_up()`. **Storage**: see the constraint below.
4. Add a `uno_match[]` table + `uno_driver` + `UNO_DRIVER(x)` (the self-
   registering seam - no central edit).
5. Delete the driver's own `pci_find*` scan; drive bring-up from the recorded
   device. Legacy scan and registry match must never both be live (double-claim).

### Per-lane adoption notes (constraints to pass along)

- **NICs `e1000`/`e1000e`/`igb`/`r8169`**, lazy-NIC shape; `e1000` matches
  exact ids, the others match class 02:00 + a vendor/device-id list so their
  probes check the list and decline (probe-decline resolves the overlap). Only
  `e1000` is QEMU-verifiable; the rest are metal (ZimaBlade r8169, Yoga/X1).
- **WiFi `iwlwifi`/`rtwifi`/`mrvlwifi`**, record-only probe; bring-up stays in
  `pc64_net_up()` (plan: do NOT eagerly init WiFi). Metal only.
- **Storage `ahci`/`nvme`/`sdhci`**, **CRITICAL**: while firmware-attached the
  firmware owns these controllers and reprogramming them corrupts its Block IO
  (it once corrupted an installer clone mid-write, see `blkdev.c`). Native
  bring-up is deferred to **detach** (`uno_blk_detach()`, post-EBS). Adoption:
  register a match whose `X_probe()` gates on `uno_pc64_detached()` and declines
  while attached; `uno_blk_detach()` re-runs `uno_devmgr_bind_all()` so they
  bind only once firmware is gone. Verify on QEMU (all backends) AND ZimaBlade.
- **Audio `hdaudio`/`ac97`, `xhci`**, QEMU-verifiable; `xhci` additionally
  becomes a child-creating controller in phase 3.

The `devices` URC verb (unoautomate reading the tree remotely) belongs to the
unoautomate lane, FILE A REQUEST, do not edit its core.

## Phase 3, USB into the same tree, DESIGNED, not yet built

Key facts that shape the work:

- The xHCI stack (`xhci.c`) is gated behind `-DUNO_XHCI` and compiles to inert
  stubs by default, so the shipped build is byte-identical and there is no risk
  of fighting the firmware's USB while attached. Phase 3 is therefore a
  `UNO_XHCI` build feature with its own harness (see `tools/usbtest.py`).
- The descriptor walk already exists: `uno_xhci_dev_count()` / `uno_xhci_dev(i)`
  return `uno_usb_dev` (slot, port, speed, vendor, product, dev class triple),
  and `uno_usb_get_config()` yields the config descriptor usbhid already parses
  per interface.
- Production USB enumeration happens at DETACH (firmware owns xHCI until then);
  under `-DUNO_USBHID_TEST` it runs eagerly in QEMU. Both call sites must
  publish into the tree.

Design (drivers create children, plan decision 2):

1. Migrate `xhci` to the registry: match PCI class 0C:03 prog-if 30 (xHCI); its
   `probe()` records the controller node. Keep `uno_xhci_init()` as the
   bring-up, invoked as today (detach / test path), NOT eagerly in probe.
2. New devmgr API (create children under the xHCI node):
   `uno_device *uno_devmgr_add_usb_dev(uno_device *parent, const uno_usb_id*)`
   and `uno_devmgr_add_usb_if(uno_device *dev, const uno_usb_id*)`. The device
   node owns address+configuration; interface nodes never issue SET_CONFIG.
3. After `uno_xhci_init()` enumerates, a bridge iterates `uno_xhci_dev_count()`
   creating a `UNO_BUS_USB` device node per device (parented to the xHCI node)
   and one interface node per interface parsed from the config descriptor, then
   calls `uno_devmgr_bind_all()` so interface drivers bind.
4. Migrate `usbhid` to a `UNO_MATCH_USB_IF` table (HID 03/01/01 boot kbd,
   03/01/02 boot mouse); its probe replaces the `uno_xhci_dev_count()` walk with
   the passed interface node. Same for `ax88179` (USB NIC, lazy like the PCI
   NICs) and `rtl8152`.
5. A hub is an ordinary registry driver (class 09) whose probe creates a child
   node per downstream port, no recursion hardcoded in the enumerator.

Acceptance: `-DUNO_XHCI -DUNO_USBHID_TEST` QEMU with `-device usb-kbd usb-mouse
usb-hub usb-storage` shows all of them in `uno.devices()` (usb-storage
UNCLAIMED until the detach-plan MSC driver lands); HID input + the USB NIC keep
working. Metal: X1 (usb-eth), any machine with a USB keyboard.

## Phase 4, loadable .UNO drivers + hotplug, DESIGNED, not yet built

- Driver-as-`.UNO` in `\DRIVERS\`: a manifest header (name, bus, match table,
  api-version, probe/remove offsets), scanned for UNCLAIMED devices after the
  built-ins bind. Reuse `pc64_modload` (the `.UNO` PE loader + KX import table).
- ABI = a **versioned services struct** passed into `probe()`: MMIO map,
  DMA-safe alloc, MSI/MSI-X setup, timer/delay, log, and the USB
  control/bulk ops for USB drivers. NO dynamic symbol resolution; the manifest
  api-version versions this struct. The devmgr owns MSI setup as a service so
  drivers never touch `_PRT`/INTx routing (our platforms have a history of
  unusable legacy IRQs, prefer MSI everywhere).
- Remove/hotplug contract (defined before hotplug ships): the manager sets a
  `gone` flag, calls `remove()`, and the driver must not touch MMIO after it
  returns (precedent: the trackpad detach-gate pointer bug). Hotplug = a
  periodic/triggered rescan diffing the tree (`uno_devmgr_init` is already
  idempotent); USB first (the xHCI port-status events), PCIe later. The node
  states `UNBOUND -> BOUND -> GONE` already model this.

Acceptance: a trivial `\DRIVERS\SAMPLE.UNO` binds to a QEMU device; USB
unplug/replug of a HID device survives BOUND -> GONE -> re-enumerated.

## Not done in this pass, for the record

- Per-lane driver adoption of the registry (NIC/storage/WiFi/audio), each is
  the owner's call and mostly metal-gated; hand them the recipe above or file a
  request.
- Phases 3 and 4 (designed above).
- Flasher deploy is opt-in now (CLAUDE.md 2026-07-23: no longer mandatory after
  a build, since the OS updates over the URC link). Not relevant on an unmerged
  branch anyway.
- The `devices` URC verb request to the unoautomate lane (bundle with the
  already-filed `install <disk>` verb).

## Merge gate status (AGENTS.md §3)

1. rebased on `origin/master`, yes.
2. builds `UNO_DEBUG=0` and `UNO_DEBUG=1`, yes.
3. QEMU gate (`tools/devmgr_qemu.py`) green, yes.
4. no central-dispatch edit bypassing a registration seam, the registry is a
   self-registering seam; all shared-file edits are append-only.

Branch is land-ready for the `unodevices` slice (mechanism only; driver
adoption lands separately in each driver's lane).
