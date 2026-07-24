# DEVICES.md — the `unodevices` subsystem contract

**Owner:** unodevices (branch `unodevices`, `uno_devmgr.{c,h}`).
**API version:** `UNO_DEVMGR_API 1` — Phase 1 has landed, so the enumeration and
registry surface (§2, §3, §7) is `[STABLE]`; the driver/binding surface (§5, §6)
is still `[EXPERIMENTAL]` design. Breaking changes bump this number and get a
dated changelog entry at the bottom (AGENTS.md §6).

`unodevices` is the PCI/USB **device tree + driver registry**: it enumerates every
device on the machine once, then matches and binds drivers to them. It owns the
*discovery and binding* mechanism; it does **not** own any driver, bus-access
primitive, or capability seam — those it consumes.

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

**Capacity.** `UNO_DEV_MAX` (128) is a static table — nothing allocates this early.
A scan that fills it sets `devmgr_overflow()`, so a truncated listing is detectable
instead of silently short.

**Why `devmgr_info()` exists.** PYRT.UNO is built separately from the kernel and
resolves imports by name, so handing it a `uno_device *` would pin this struct's
layout into a module that ships independently. The flat unsigned row is the
module-safe view; keep its column order append-only.

---

## 3. PCI enumeration  `[STABLE]` (as landed)

In `uno_devmgr.c`, **consuming** `pc64_pci.c`'s existing accessors
(`pci_cfg_read32/16`, `pci_cfg_write32`) — the shared PCI file is **not** edited.

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
  functions 1..7 — probing them unconditionally duplicates parts that alias
  function 0 across all eight.

### BAR sizing is opt-in, and deliberately not part of a scan

Sizing a BAR means writing all-ones and reading back the mask, which parks a bogus
address in the BAR for a few config cycles. Every OS does this at boot — while IT
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

Factor the descriptor walk out of `xhci.c` (consumed, not restructured — a request to
its owner if a hook is needed): for each root-hub port → reset/address → read
**device + config + interface descriptors**, recurse through hubs. Emit one
`uno_device` per interface keyed on `idVendor:idProduct` + the
`bInterfaceClass/SubClass/Protocol` triple (HID vs mass-storage vs CDC-ethernet are
distinct claimants).

---

## 5. Driver model + binding  `[EXPERIMENTAL, Phase 2]`

```c
typedef struct uno_match {            /* wildcard: 0xFFFF (ids) / 0xFF (class) */
    u16 ven, dev; u8 cls, sub, prog; u16 flags;
} uno_match;

typedef struct uno_driver {
    const char *name; uno_bustype bus;
    const uno_match *match;           /* NULL-terminated table               */
    int  (*probe)(uno_device *);      /* claim + init; publish a capability   */
    void (*remove)(uno_device *);
    u8   priority;                    /* tie-break; higher wins               */
} uno_driver;
```

- **Self-registration via a linker set** (AGENTS.md §2 names this seam): `UNO_DRIVER(x)`
  drops `&x` into a `.uno_drivers` section the manager walks — no central list to edit
  when a driver is added.
- **Match precedence:** exact `ven:dev` > `cls/sub/prog` > `cls`-only; ties by
  `priority`. First successful `probe()` binds.
- **Pipeline** (`devmgr_bind_all()`): order by dependency (bridges/controllers before
  children; xHCI before USB devices) → for each `DEV_UNBOUND`, best match → `probe()`
  → `DEV_BOUND` (+ capability published) or stays `DEV_UNBOUND`/`DEV_FAILED`, **visibly**.
- **Existing drivers migrate additively:** each gains a `match[]` + a `probe()` wrapper
  around its current init and a `UNO_DRIVER(...)` line. `pci_find` stays as a compat
  shim so nothing breaks mid-migration; the driver's own file is the only edit.

## 6. Loadable `.UNO` drivers  `[EXPERIMENTAL, Phase 4]`

A driver may ship as a `.UNO` in `\DRIVERS\` with a manifest header (name, bus, match
table, api-version, `probe`/`remove` offsets), reusing the existing module loader. For
an unclaimed device the manager scans `\DRIVERS\` manifests, loads the matching module,
and calls `probe()`. Enables shipping/updating drivers out of band (pairs with the A/B
`put` flow) and third-party drivers.

---

## 7. Introspection — the point of Phase 1  `[STABLE]` (as landed)

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

> **What `UNCLAIMED` means in phase 1.** It means *the manager has bound nothing* —
> which is true of every device, because binding does not exist until phase 2. It
> does **not** mean "UnoDOS has no driver for this part": the legacy pull-drivers
> still find their own hardware by `pci_find()` and are unaffected. Read a phase-1
> dump as the machine's inventory, not as a coverage report.

- **`uno.devices()`** — the listing above as a string. **`uno.pci()`** — the same
  registry parsed: a list of `(loc, ven, dev, cls, sub, progif, driver_or_None)`.
  Both appended to the `mod_uno.c` module table (additive seam). `uno.usb()` arrives
  with phase 3.
- **URC `devices` verb** — landed on master 2026-07-23 (unoautomate), a weak-symbol
  pass-through to `devmgr_list_str`, so it upgraded itself from the "unodevices
  pending" stub to real rows the moment this branch's strong definition linked in.
- **A "Device Manager" unoui app** — a tree of devices, each bound driver or
  `no driver — 8086:5A85 class 0x03 (display)` spelled out. Later phase.

### Gates

- `tools/devmgr_test.sh` — the enumerator linked against a **synthetic** config
  space and run natively (seconds, no QEMU): topology, multi-function handling,
  64-bit BAR pairs, capability walks, listing format, truncation safety, re-scan
  idempotence, and that enumeration writes nothing. Run it after every edit.
- `tools/devmgr_qemu.py` — the same enumerator against QEMU's q35 built with a
  PCIe root port and a controller behind it, driven over URC.

---

## 8. Rollout (each phase lands small and green — AGENTS.md §3)

| Phase | Slice | Payoff |
|---|---|---|
| **1 ✅ DONE 2026-07-23** | `uno_devmgr.{c,h}` PCI enumerator + registry + `uno.devices()`/`uno.pci()` (read-only) | The real driver-less map of the ZimaBlade — answers the immediate question |
| **2** | `uno_driver` + `UNO_DRIVER` linker set; migrate r8169/e1000 to match tables | New PCI driver = a match table, not a probe-list edit |
| **3** | USB enumerator into the same registry; migrate HID / AX88179 | USB devices first-class + visible |
| **4** | Loadable `\DRIVERS\*.UNO` + hotplug re-scan | Ship/third-party drivers; PCIe/USB hotplug |

## 9. Territory (AGENTS.md §1–2)

Own: `uno_devmgr.{c,h}`, `DEVICES.md`, `tools/devmgr_test.c`, `tools/devmgr_test.sh`,
`tools/devmgr_qemu.py`. Consume unchanged: `pc64_pci.c`, `xhci.c`,
`tools/unoauto_remote.py` + `tools/remote_qemu.py` (the QEMU gate imports them), the
`uno_nic_t`/`blkdev`/input/audio/GOP seams. Additive-only seam touches so far:
`build.sh` file list, `pc64_modload.c` `KX()` exports, `mod_uno.c` module table, and
one read-only accessor appended to `uefi_main.c` (`uno_pc64_fb_phys`). Phase 2 adds
the `.uno_drivers` linker set. The URC `devices` verb is unoautomate's (landed).

---

## Changelog

- **2026-07-23 — UNO_DEVMGR_API 1 (Phase 1 landed):** the registry is real.
  `uno_device` gained `parent`, `hdr_type`, `sec_bus`, IRQ line/pin, capability
  offsets and `bar_flags[]`; `devmgr_enumerate()` returns the count; added
  `devmgr_find_class`, `devmgr_driver_name`, `devmgr_overflow`, `devmgr_detail_str`,
  `devmgr_info`, `devmgr_size_bars`. Two deliberate departures from the original
  design above, both now documented in place: enumeration is recursive **plus** a
  flat sweep (multi-root machines and unconfigured bridges), and **BAR sizing is
  opt-in per device rather than part of a scan** — phase 1 runs while firmware still
  drives the hardware, so a scan writes nothing at all. The listing gained the
  driver column (`UNCLAIMED` throughout phase 1; see §7 for exactly what that
  claims). §2, §3 and §7 are `[STABLE]`; §5 and §6 remain design.
- **2026-07-23 — UNO_DEVMGR_API 0 (initial design):** subsystem claimed, contract
  written. No code yet; Phase 1 (`uno_devmgr` PCI enumerator + `uno.devices()`) next.
