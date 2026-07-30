# DEVICES.md, the `unodevices` subsystem contract

**Owner:** unodevices (branch `unodevices`, `uno_devmgr.{c,h}`).
**API version:** `UNO_DEVMGR_API 1`: Phase 1 has landed, so the enumeration and
registry surface (§2, §3, §7) is `[STABLE]`; the driver/binding surface (§5, §6)
is still `[EXPERIMENTAL]` design. Breaking changes bump this number and get a
dated changelog entry at the bottom (AGENTS.md §6).

`unodevices` is the PCI/USB **device tree + driver registry**: it enumerates every
device on the machine once, then matches and binds drivers to them. It owns the
*discovery and binding* mechanism; it does **not** own any driver, bus-access
primitive, or capability seam, those it consumes.

---

## 1. Why (the model shift)

Today UnoDOS is **driver-pull**: each driver calls `pci_find(ven,dev)` for its own
IDs at boot (`pc64_net_up` walks a hard-coded NIC list; storage probes AHCI/NVMe/
SDHCI; `xhci.c` scans for the controller). Consequences:

- **Driver-less hardware is invisible.** Nothing lists what is actually on the bus,
  so "what does this box have that we can't drive yet?" is unanswerable on-device.
- **Adding a device means editing a probe list** in the middle of a subsystem.
- **No unified device→driver→capability view** for diagnostics.

`unodevices` inverts this to **bus-push**: enumerate all buses → build a device
registry → match each device against a driver registry → bind (probe) → the driver
publishes into its existing capability seam. Unclaimed devices stay in the registry,
visibly, with the IDs you need to go write a driver.

This does not replace the seams (`uno_nic_t`, `blkdev`, input, audio, GOP). Drivers
still publish into exactly those. `unodevices` only changes *how a driver finds its
device*: the manager hands it one, instead of the driver searching.

---

## 2. Device model  `[STABLE]` (as landed)

`uno_devmgr.h` is the authoritative declaration; this is the shape of it. Every
node carries a `parent` index, so the registry is a TREE, not a flat list.

```c
typedef enum { UNO_BUS_PCI, UNO_BUS_USB, UNO_BUS_PLATFORM } uno_bustype;

typedef struct uno_device {
    unsigned char bus_type;
    short       parent;                  /* registry index of the bridge above,
                                            UNO_DEV_NOPARENT at a root          */
    union {                              /* physical location */
        struct { u8 bus, dev, fn; } pci;
        struct { u8 path[6], depth; } usb;   /* hub-port path from root */
    } addr;
    u16 vendor, device;                  /* PCI ven:dev  | USB idVendor:idProduct */
    u16 subsys_vendor, subsys_id;        /* PCI SSID (0 for USB)                    */
    u8  cls, subcls, prog_if, revision;  /* class triple (+ USB iface class triple) */
    u8  hdr_type, irq_line, irq_pin, sec_bus;   /* sec_bus: bridges only            */
    u16 caps;                            /* UNO_DEVCAP_* bitset (PM/MSI/MSI-X/PCIe) */
    u8  cap_msi, cap_msix, cap_pcie;     /* their config offsets, 0 = absent         */
    u8  bar_flags[6];                    /* UNO_BAR_ present/io/mem64/prefetch/sized */
    u64 bar[6], bar_sz[6];               /* PCI resources (0 for USB)               */
    u8  state;                           /* UNO_DEV_ UNBOUND/BOUND/FAILED/GONE      */
    const char *drv; void *drvdata;      /* bound driver's name + private state     */
} uno_device;
```

Registry API (all in `uno_devmgr.h`):

```c
int          devmgr_enumerate(void);              /* (re)scan all buses; returns count   */
int          devmgr_count(void);                  /* enumerates on first use             */
uno_device  *devmgr_get(int idx);
uno_device  *devmgr_find(u16 ven, u16 dev);       /* first match, else NULL              */
uno_device  *devmgr_find_class(u8 cls, u8 sub);
const char  *devmgr_class_name(u8 cls, u8 sub);   /* "ethernet","sata","vga",...        */
const char  *devmgr_driver_name(int idx);         /* NULL while unbound                  */
int          devmgr_overflow(void);               /* last scan hit UNO_DEV_MAX           */
int          devmgr_size_bars(int idx);           /* OPT-IN write probe - see §3         */
int          devmgr_list_str(char *buf, int cap); /* the whole machine, one line each     */
int          devmgr_detail_str(int idx, char *b, int cap);
int          devmgr_info(int idx, unsigned *out, int nmax);  /* flat row, module-safe    */
```

`devmgr_enumerate()` is idempotent: it rebuilds the table, preserving `drv`/`drvdata`
for devices still present at the same address (so a re-scan does not tear down bound
drivers). Everything else is re-read from hardware, so a re-scan also invalidates any
BAR sizes previously probed.

**Platform devices.** `devmgr_add_platform(backing, cls, sub, io_base, io_len, drv)`
registers a `UNO_BUS_PLATFORM` node for a logical block that lives *inside* a PCI
function rather than being its own function, the first user is the PCH TCO watchdog
(`uno_hw_wdt`, HWWATCHDOG.md), which is decoded by the LPC function but is not itself
enumerable. The node inherits the backing function's `bb:dd.f`/`ven:dev`, links to it
as parent, records `[io_base, io_base+io_len)` as an I/O BAR, and lists as **BOUND** to
`drv`: so it is the first device the registry reports with a real driver (phase-1 PCI
enumeration binds nothing; §7). The registration is **sticky**: re-applied at the end of
each `devmgr_enumerate()`, so a re-scan does not drop it, and idempotent (re-registering
the same backing+driver does not duplicate). Additive to `UNO_DEVMGR_API 1`: no struct or
ABI change (`UNO_BUS_PLATFORM` was always in the bus-type enum).

**Capacity.** `UNO_DEV_MAX` (128) is a static table, nothing allocates this early.
A scan that fills it sets `devmgr_overflow()`, so a truncated listing is detectable
instead of silently short.

**Why `devmgr_info()` exists.** PYRT.UNO is built separately from the kernel and
resolves imports by name, so handing it a `uno_device *` would pin this struct's
layout into a module that ships independently. The flat unsigned row is the
module-safe view; keep its column order append-only.

---

## 3. PCI enumeration  `[STABLE]` (as landed)

In `uno_devmgr.c`, **consuming** `pc64_pci.c`'s existing accessors
(`pci_cfg_read32/16`, `pci_cfg_write32`), the shared PCI file is **not** edited.

- **Recursive** by bridge: start at bus 0; for each type-1 header follow its
  secondary bus number and recurse, recording the parent link. A bus-number bitmap
  makes the walk loop-proof.
- **Then a flat sweep** of every bus the walk never reached. This is the safety net
  that keeps the registry a strict superset of the old `pci_find()` scan: a
  multi-root machine has top-level busses with no bridge above them, and a bridge
  left unconfigured by firmware (secondary = 0) hides its children from the walk.
  Devices found this way are recorded as roots, which is what they are.
- Per function: `ven:dev` (skip `0xFFFF`), class triple, revision, subsystem IDs,
  header type, IRQ line/pin, the **capability list** (`0x34` → chained, bounded
  guard) recording PM/MSI/MSI-X/PCIe and their offsets, and the **BAR bases** with
  their kind (I/O, 64-bit pair, prefetchable). The multi-function bit gates
  functions 1..7, probing them unconditionally duplicates parts that alias
  function 0 across all eight.

### BAR sizing is opt-in, and deliberately not part of a scan

Sizing a BAR means writing all-ones and reading back the mask, which parks a bogus
address in the BAR for a few config cycles. Every OS does this at boot, while IT
owns the hardware. Phase 1 runs **attached**: UEFI boot services are alive and
firmware drivers are still driving these controllers, so a decode gap on an
in-flight AHCI or xHCI is exactly the class of thing that wedges a real machine.
So `devmgr_enumerate()` performs **config-space reads only** (the host gate asserts
this), and sizing is a separate per-device call:

`devmgr_size_bars(idx)` disables memory + I/O decode around the probe and restores
it after, always writes the original BAR values back, sizes a 64-bit pair as one
BAR, and **refuses** display-class devices and any device whose BAR holds the live
GOP framebuffer (scanout reads that aperture continuously). When writing back the
command register it zeroes the status half rather than echoing what it read: those
bits are write-1-to-clear, and echoing them would silently wipe the errors the
firmware had recorded.

## 4. USB enumeration  `[EXPERIMENTAL, Phase 3]`

Factor the descriptor walk out of `xhci.c` (consumed, not restructured, a request to
its owner if a hook is needed): for each root-hub port → reset/address → read
**device + config + interface descriptors**, recurse through hubs. Emit one
`uno_device` per interface keyed on `idVendor:idProduct` + the
`bInterfaceClass/SubClass/Protocol` triple (HID vs mass-storage vs CDC-ethernet are
distinct claimants).

---

## 5. Driver model + binding  `[STABLE, Phase 2]` (as landed)

```c
typedef struct uno_match {
    unsigned char  kind;                 /* UNO_MATCH_PCI_ID | _PCI_CLASS     */
    unsigned short vendor, device;       /* _PCI_ID                            */
    unsigned char  cls, subcls, prog_if; /* _PCI_CLASS                         */
    unsigned char  have_progif;          /* 0 = class/subclass only            */
} uno_match;

typedef struct uno_driver {
    const char      *name;               /* single token: it IS the listing column */
    unsigned char    bus;                /* uno_bustype                        */
    unsigned short   api;                /* UNO_DEVMGR_API it was built to     */
    const uno_match *match;              /* UNO_MATCH_END-terminated           */
    int  (*probe)(uno_device *);         /* 1 = claimed, 0 = "not mine"        */
    void (*remove)(uno_device *);        /* may be NULL                        */
} uno_driver;
```

**There is no priority field, and that is a decision rather than an omission.**
The earlier draft of this section had one. Priority numbers are a coordination
problem between files that do not know about each other: every new driver has
to guess a number relative to drivers it has never seen, and the number means
nothing locally. Specificity plus probe-decline expresses the same thing with
only local knowledge, which is what plan decision 3 locked.

- **Self-registration** through the `UNO_DRIVER(x)` seam (AGENTS.md §2): a
  driver opts in with one line in its own file, and no central list is edited.
  The idiom is COFF grouped sections (`.unodrv$a` / `$m` / `$z`), NOT the ELF
  "custom section + `KEEP()`" advice, because this kernel is PE/COFF via mingw
  ld with `-nostdlib` and no linker script. Constructor registration is not
  available either: there is no CRT, so `__attribute__((constructor))` never
  runs. The manager iterates between the markers and SKIPS NULL slots, since
  the linker may pad between contributions.
- **Match precedence:** exact `vendor:device` (3) > `class/subclass/prog-if`
  (2) > `class/subclass` (1). The best entry in a driver's own table wins, so a
  driver may list an exact id AND a class fallback without the fallback
  weakening it.
- **Probe-decline is the tie-break.** `devmgr_bind_all()` offers a device to
  each matching driver most-specific-first until one returns 1. A declining
  probe is normal, not an error, and must be side-effect-free. This is how
  drivers sharing a class sort themselves out.
- **The bind loop runs to a FIXPOINT**, not in dependency order: a pass that
  binds nothing ends it. Binding a controller may create children (plan
  decision 2) whose drivers deserve a pass of their own. It is idempotent and
  re-runnable, which is what lets `uno_blk_detach()` re-run it.

### Probes must not touch hardware they do not yet own

The two shapes that matter, both load-bearing:

- **Lazy devices (NICs, WiFi).** The probe RECORDS the node and returns 1;
  bring-up stays in `pc64_net_up()`. Adoption must not make a radio eager.
- **Storage (`ahci`, `nvme`, `sdhci`).** The probe DECLINES while
  `uno_pc64_detached()` is false. While attached the firmware owns those
  controllers and is moving sectors through them; reprogramming one underneath
  it is not theoretical damage, it once corrupted an installer clone mid-write
  (see `blkdev.c`). An UNCLAIMED listing while attached is the honest answer,
  and `uno_blk_detach()` re-runs `devmgr_bind_all()` past ExitBootServices so
  they bind exactly when the hardware becomes ours.

### What adoption did NOT do, and why

The plan said each driver's legacy `pci_find` call should be deleted in the
same commit as its match table. That has not been done, and the reason is
scope of proof rather than tidiness: deleting the scan changes WHEN a driver
touches hardware, on paths only metal can exercise (r8169 on the ZimaBlade,
e1000e/igb and the WiFi parts on laptops). The registry is now consulted
FIRST and the scan remains as the fallback, so a machine where a bind pass has
run uses the registry and one where it has not behaves exactly as before.
Finishing the deletion is a per-lane, metal-gated step.

## 6. Loadable `.UNO` drivers  `[STABLE, Phase 4]` (as landed)

A driver ships as `\DRIVERS\<NAME>.UNO`, flagged `UNO_MODF_DRV` (0x0008) in
its module header, and is loaded after the built-ins have had their turn, so a
shipped driver always beats a dropped-in one for the same device.

```c
typedef const uno_drv_module *(*UnoDrvEntry)(const uno_drv_services *svc);
```

- **A versioned services struct, and NO dynamic symbol resolution.** The
  module receives `uno_drv_services` (config read/write, `map_bar`,
  `dma_alloc`, `msi_enable`, `delay_ms`, `rdtsc`, `log`) and resolves nothing
  by name. `UNO_DRVSVC_API` versions the struct.
- **Two independent version gates, and both earn their keep.** The MODULE
  declares which services struct it was built against; the DRIVER record
  declares which registry contract. A driver built against an older services
  struct would read function pointers at the wrong offsets, and that is not a
  failure that reports itself.
- **The manager owns MSI.** Drivers never touch `_PRT` or INTx routing: every
  machine in this fleet has a history of unusable legacy IRQs, so the house
  style is MSI everywhere and a driver that cannot get one should poll rather
  than trust a line it has no reason to trust.
- **`dma_alloc` is a bump arena, 64-byte aligned, never freed.** A driver's DMA
  buffers live as long as the driver. The alignment requirement is the BOT
  lesson from `usbmsc.c` restated as a service: DMA must never target the stack.

## 7. Hotplug and the remove contract  `[STABLE, Phase 4]` (as landed)

`devmgr_rescan()` re-enumerates and diffs against the tree. Devices still
present keep their bindings (`devmgr_enumerate()` has always carried them);
devices that have gone get their driver's `remove()` and are counted as
departures; arrivals bind on the way out.

**The contract for a driver, which the manager cannot enforce:** after
`remove()` returns, the driver must not touch that device's MMIO again. The
precedent is the trackpad detach-gate bug, where a pointer outlived the thing
it pointed at.

A departing driver is handed a synthetic node carrying its ADDRESS and nothing
else. That is deliberate: `drvdata` pointed into a table slot the re-scan has
already rebuilt, so passing it back would be passing back a dangling
reference. `remove()` may trust the address; it may trust nothing else.

**Nothing calls `devmgr_rescan()` periodically yet.** PCI hotplug on these
machines is rare enough that a timer would be pure overhead, and USB hotplug -
the case that actually happens - needs USB in the tree, which is phase 3. The
mechanism and the contract are here so phase 3 has something to plug into.

---

## 8. Introspection, the point of Phase 1  `[STABLE]` (as landed)

The **line format is unodevices'** to define; URC forwards it verbatim and does not
parse it. One line per PCI function:

```
bb:dd.f VVVV:DDDD cc/ss <class-name> <driver|UNCLAIMED>
00:03.0 1b36:000c 06/04 pci-bridge UNCLAIMED
01:00.0 8086:2922 01/06 sata UNCLAIMED
```

Two constraints on that format, both load-bearing: the class name is always a
**single token** (the URC host parser reads the last whitespace token as the driver
column), and the driver column is always present.

> **What `UNCLAIMED` means in phase 1.** It means *the manager has bound nothing* -
> which is true of every device, because binding does not exist until phase 2. It
> does **not** mean "UnoDOS has no driver for this part": the legacy pull-drivers
> still find their own hardware by `pci_find()` and are unaffected. Read a phase-1
> dump as the machine's inventory, not as a coverage report.

- **`uno.devices()`**: the listing above as a string. **`uno.pci()`**: the same
  registry parsed: a list of `(loc, ven, dev, cls, sub, progif, driver_or_None)`.
  Both appended to the `mod_uno.c` module table (additive seam). `uno.usb()` arrives
  with phase 3.
- **URC `devices` verb**: landed on master 2026-07-23 (unoautomate), a weak-symbol
  pass-through to `devmgr_list_str`, so it upgraded itself from the "unodevices
  pending" stub to real rows the moment this branch's strong definition linked in.
- **A "Device Manager" unoui app**: a tree of devices, each bound driver or
  `no driver, 8086:5A85 class 0x03 (display)` spelled out. Later phase.

### Gates

- `tools/devmgr_test.sh`: the enumerator linked against a **synthetic** config
  space and run natively (seconds, no QEMU): topology, multi-function handling,
  64-bit BAR pairs, capability walks, listing format, truncation safety, re-scan
  idempotence, and that enumeration writes nothing. Run it after every edit.
- `tools/devmgr_qemu.py`: the same enumerator against QEMU's q35 built with a
  PCIe root port and a controller behind it, driven over URC.

---

## 9. Rollout (each phase lands small and green, AGENTS.md §3)

| Phase | Slice | Payoff |
|---|---|---|
| **1 ✅ DONE 2026-07-23** | `uno_devmgr.{c,h}` PCI enumerator + registry + `uno.devices()`/`uno.pci()` (read-only) | The real driver-less map of the ZimaBlade, answers the immediate question |
| **2** | `uno_driver` + `UNO_DRIVER` linker set; migrate r8169/e1000 to match tables | New PCI driver = a match table, not a probe-list edit |
| **3** | USB enumerator into the same registry; migrate HID / AX88179 | USB devices first-class + visible |
| **4** | Loadable `\DRIVERS\*.UNO` + hotplug re-scan | Ship/third-party drivers; PCIe/USB hotplug |

## 10. Territory (AGENTS.md §1-2)

Own: `uno_devmgr.{c,h}`, `DEVICES.md`, `tools/devmgr_test.c`, `tools/devmgr_test.sh`,
`tools/devmgr_qemu.py`. Consume unchanged: `pc64_pci.c`, `xhci.c`,
`tools/unoauto_remote.py` + `tools/remote_qemu.py` (the QEMU gate imports them), the
`uno_nic_t`/`blkdev`/input/audio/GOP seams. Additive-only seam touches so far:
`build.sh` file list, `pc64_modload.c` `KX()` exports, `mod_uno.c` module table, and
one read-only accessor appended to `uefi_main.c` (`uno_pc64_fb_phys`). Phase 2 adds
the `.uno_drivers` linker set. The URC `devices` verb is unoautomate's (landed).

---

## Changelog

- **2026-07-30, `UNO_DEVMGR_API 2`.** Phases 2 and 4. The driver registry
  (`UNO_DRIVER` seam, specificity matching, probe-decline, fixpoint
  `devmgr_bind_all`), loadable `\DRIVERS\*.UNO` behind a versioned services
  struct, and `devmgr_rescan` with the remove contract. Additive: every
  phase-1 entry point keeps its signature. It is a BUMP rather than a silent
  addition because `state` can now be BOUND and `drv` non-NULL, which the
  phase-1 contract promised never happened. §5 also drops the `priority`
  field the earlier draft carried - see the note there.

- **2026-07-24, additive: `devmgr_add_platform()` (no API bump).** Platform
  devices (a logical block inside a PCI function) can now be registered as sticky
  `UNO_BUS_PLATFORM` nodes; the first user is the PCH TCO watchdog (`uno_hw_wdt`,
  HWWATCHDOG.md), which lists as `08/80 system tco-wdt` under the LPC function and
  is the registry's first `UNO_DEV_BOUND` node. Additive to `UNO_DEVMGR_API 1`:
  no struct/ABI change, existing PCI rows unchanged. Host gate `devmgr_test.c`
  covers creation, the inherited-location listing, re-scan stickiness, and
  no-duplicate re-registration.
- **2026-07-23, UNO_DEVMGR_API 1 (Phase 1 landed):** the registry is real.
  `uno_device` gained `parent`, `hdr_type`, `sec_bus`, IRQ line/pin, capability
  offsets and `bar_flags[]`; `devmgr_enumerate()` returns the count; added
  `devmgr_find_class`, `devmgr_driver_name`, `devmgr_overflow`, `devmgr_detail_str`,
  `devmgr_info`, `devmgr_size_bars`. Two deliberate departures from the original
  design above, both now documented in place: enumeration is recursive **plus** a
  flat sweep (multi-root machines and unconfigured bridges), and **BAR sizing is
  opt-in per device rather than part of a scan**: phase 1 runs while firmware still
  drives the hardware, so a scan writes nothing at all. The listing gained the
  driver column (`UNCLAIMED` throughout phase 1; see §7 for exactly what that
  claims). §2, §3 and §7 are `[STABLE]`; §5 and §6 remain design.
- **2026-07-23, UNO_DEVMGR_API 0 (initial design):** subsystem claimed, contract
  written. No code yet; Phase 1 (`uno_devmgr` PCI enumerator + `uno.devices()`) next.
