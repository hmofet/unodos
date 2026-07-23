# unodevices — Device Manager implementation plan (pc64)

Status: PLANNED, run this BEFORE the detach-completion plan
(docs/DETACH-COMPLETION-PLAN.md, which builds its drivers on this registry).

## Goal

Replace driver-pull (each driver calls `pci_find()` for its own IDs) with
bus-push PnP: enumerate the whole machine once into a device tree, match
drivers to devices from a registry, and make unclaimed hardware visible.
The introspection output is a first-class deliverable: it is what tells us,
per machine, which device is keeping that machine attached to firmware.

## Current state (verified 2026-07-23)

- `pc64/pc64_pci.c` — flat scan of buses 0..255 in `pci_find()` /
  `pci_find_class()`. NOTE: on UEFI-booted machines this flat scan already
  reaches devices behind bridges (firmware assigns secondary bus numbers), so
  do not expect the recursive enumerator to reveal new devices on our fleet.
  Recursion is still required: it builds the parent/child tree and handles
  unconfigured bridges + future hotplug.
- `pc64/xhci.c` — finds the controller itself, enumerates ports, fetches
  descriptors, and already has control transfers, SET_CONFIGURATION, and bulk
  rings (`uno_usb_setup_bulk`, `uno_usb_bulk_in/out`, async bulk-IN).
- Native drivers today (each with its own hardcoded probe): ahci, nvme, sdhci,
  e1000, e1000e, igb, r8169, iwlwifi, mrvlwifi, rtwifi, ax88179 (USB),
  rtl8152 (USB), usbhid, i2c_hid, hdaudio, ac97.
- Build: `pc64/build.sh`, mingw-clang, output is a PE/COFF `BOOTX64.EFI`,
  linked `-nostdlib`. There is NO ELF linker script. See "Toolchain traps".

## Locked design decisions

These were reviewed and agreed; do not re-litigate them mid-implementation.

1. **The device TREE is the core abstraction**, not two flat lists.
   `uno_device` is a node: parent pointer, bus tag + per-bus id union,
   class info, state (UNBOUND / BOUND / GONE), bound-driver pointer.
   Bus-agnostic from day 1 (bus = PCI | USB | future ACPI/platform; we
   already vendor uACPI, and the X1 trackpad is an ACPI-described I2C-HID
   child, so the schema must be able to represent non-PCI/USB devices even
   though no ACPI enumerator ships in this plan).
2. **Drivers can create child devices.** The xHCI driver spawns USB device
   nodes; a USB hub driver spawns its children; a USB device node spawns one
   child per interface. This one rule replaces special-cased "USB enumerator
   phases", makes hubs ordinary drivers, and gives composite devices a
   correct owner for SET_CONFIGURATION (the device node owns configuration;
   interface nodes never issue it).
3. **Match specificity + probe-decline. NO priority field.**
   Precedence: exact vendor:device > class/subclass/prog-if > class-only
   (USB: exact idVendor:idProduct > interface class/subclass/protocol
   triple > class). A driver's `probe()` returning failure means "not mine,
   try the next candidate" and the manager continues down the match list.
   Two drivers claiming the same exact ID is a bug to fix in the tables.
4. **Multi-pass binding to a fixpoint**, not topological ordering: loop over
   all UNBOUND devices binding what matches; binding a controller creates
   children; repeat until a pass binds nothing.
5. **Phase 1 is strictly read-only** (enumerate + report, bind nothing).
   `pci_find()` stays as a compat shim throughout. When a driver migrates to
   the registry (phase 2), its legacy init call is deleted IN THE SAME
   COMMIT, never left running in parallel (double-claim hazard).

## Phase 1 — PCI enumerator + tree + introspection (read-only)

New files `pc64/uno_devmgr.c` / `uno_devmgr.h`; extend `pc64_pci.c`.

- Recursive scan from bus 0 following type-1 bridges (secondary bus). Check
  the header-type multi-function bit before probing functions 1..7.
- Per function, capture: vendor/device, class/subclass/prog-if, revision,
  subsystem IDs, header type, IRQ line/pin, capability list walk (record
  MSI / MSI-X / PCIe presence and offsets), all BARs with sizes.
- **BAR sizing safety (metal trap, do not skip):** clear the COMMAND
  register's memory+IO decode bits around the write-0xFFFFFFFF size probe
  and restore afterwards. SKIP sizing entirely on the device whose BAR
  contains the active GOP framebuffer (compare against
  `gGop->Mode->FrameBufferBase`); scanout reads it continuously and a decode
  gap can glitch or hang real hardware. 64-bit BARs (type bits 0b10) consume
  two slots; size the pair as one and skip the upper half.
- Class-code decode table (small, static: the ~20 class/subclass names we
  actually meet) so listings read `display` not `0x03`.
- Introspection:
  - MicroPython bindings in the pc64 upy port (`uno.devices()`,
    `uno.pci()`): list of tuples
    `(loc, ven, dev, class, subclass, progif, driver_or_None)`.
  - A plain-text dump routine callable from the debug harness
    (`UNO_DEBUG=1`) printing one line per device:
    `01:00.0 8086:5A85 display UNCLAIMED`.
  - The `devices` URC verb belongs to the unoautomate agent's territory:
    FILE A REQUEST (bundle with the already-filed `install <disk>` verb),
    do not edit the frozen unoautomate core yourself.
  - A read-only "Device Manager" unoui app (tree view, device -> driver or
    "no driver") can land here or in phase 2; do not block phase 1 on it.

Acceptance: QEMU q35 boot shows the expected topology (host bridge, ICH9,
e1000, xHCI, AHCI...) with correct BAR sizes; boot behavior is otherwise
byte-identical (no binding, no register writes beyond the guarded BAR
probe); capture the device list on the harness via `tools/qemu_test.py`.

## Phase 2 — driver registry + binding for built-in drivers

- `UNO_DRIVER(x)` linker-set macro (see "Toolchain traps" for the COFF
  idiom) emitting a `uno_driver` record: name, bus, match table pointer,
  `probe(uno_device*)`, `remove(uno_device*)` (may be NULL for now).
- Convert built-in PCI drivers one commit each: add a match table +
  `probe()` wrapper around the existing init, delete the legacy
  `pci_find`-based call in the same commit. Order (risk-ascending):
  r8169, e1000, e1000e, igb, ahci, nvme, sdhci, hdaudio/ac97, xhci,
  iwlwifi/mrvlwifi/rtwifi (these are lazy-bound on first net use today;
  preserve laziness by registering a match that records the device and
  defers hardware touch to `pc64_net_up()`, do NOT eagerly init WiFi).
- Manager runs the fixpoint bind loop at the same point in init where the
  drivers used to self-probe. Devices with no match stay UNBOUND and
  visible.

Acceptance: `SPECTEST` baseline unchanged (62 pass / 0 fail / 4 skip per the
debug-stress notes), networking up in QEMU (e1000) and on the ZimaBlade
(r8169 via the URC bridge), storage detach still works, and `uno.devices()`
now shows drivers bound against each function.

## Phase 3 — USB into the same tree

- Factor the descriptor walk out of `xhci.c` into the devmgr's USB
  enumeration path, invoked by the xHCI driver post-bind (drivers create
  children). One node per USB device (owns address + configuration), one
  child node per interface.
- Hub support = a hub class driver in the registry that creates child
  device nodes per port, not recursion hardcoded in the enumerator.
- Migrate usbhid, ax88179, rtl8152 to interface-triple match tables.
- Timing constraint: in production, the firmware owns xHCI until detach
  (see uefi_main.c try_detach), so USB enumeration into the tree happens at
  detach time (or eagerly under `UNO_USBHID_TEST` in QEMU, same as today).

Acceptance: QEMU with `-device usb-kbd -device usb-mouse -device usb-hub
-device usb-storage` shows all of them in the tree (usb-storage as
UNCLAIMED until the MSC driver from the detach plan lands); HID input and
the USB NIC keep working.

## Phase 4 — loadable .UNO drivers + hotplug

- Driver-as-.UNO in `\DRIVERS\`: manifest header (name, bus, match table,
  api-version, probe/remove offsets), scanned for UNCLAIMED devices after
  built-ins bind. Reuse pc64_modload.
- **ABI: a versioned services struct passed into probe()** (function
  pointers: MMIO map, DMA-safe alloc, MSI/MSI-X setup, timers, delay, log,
  bulk/control USB ops for USB drivers). NO dynamic symbol resolution. The
  manifest api-version versions this struct. The devmgr owns MSI setup as a
  service so drivers never touch _PRT/INTx routing (our platforms have a
  history of unusable legacy IRQs; prefer MSI everywhere).
- Remove/hotplug contract, defined here BEFORE hotplug ships: manager sets
  a `gone` flag, calls `remove()`, driver must not touch MMIO after
  returning (precedent: the trackpad detach-gate pointer bug). Hotplug =
  periodic/triggered rescan diffing the tree; USB first, PCIe later.

Acceptance: a sample trivial driver shipped as `\DRIVERS\SAMPLE.UNO` binds
to a QEMU device; USB unplug/replug of a HID device survives with the
device transitioning BOUND -> GONE -> re-enumerated.

## Toolchain traps (read before writing the registry macro)

- The kernel is PE/COFF via mingw ld, `-nostdlib`, no linker script. ELF
  linker-set advice (custom section + `KEEP()`) does NOT apply. Use the COFF
  grouped-section idiom: entries in `.unodrv$m`, a start marker variable in
  `.unodrv$a`, end marker in `.unodrv$z`, everything
  `__attribute__((used, section(...)))`; the linker sorts by `$` suffix and
  you iterate between the markers.
- `__attribute__((constructor))` does NOT run under `-nostdlib` (no CRT
  init); never use constructor-based registration.
- QEMU testing: use `tools/qemu_test.py` (monitor-socket screendumps, never
  focus/SendKeys per machine rules). Do not use QEMU vvfat for anything
  that writes multi-cluster files; build a real FAT image via mkuefi.
- After any meaningful pc64 build, rebuild + publish the flasher per the
  standing rule (`pc64\flash\deploy-to-share.ps1`); skip only if \\behemoth
  is offline.
- Commit per phase (small commits per driver in phase 2). Do NOT push
  without asking; master has unpushed work by policy.
