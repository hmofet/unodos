# DEVICES.md — the `unodevices` subsystem contract

**Owner:** unodevices (branch `unodevices`, `uno_devmgr.{c,h}`).
**API version:** `UNO_DEVMGR_API 0` (pre-1: everything here is `[EXPERIMENTAL]` until
Phase 1 lands, then the enumeration/registry surface goes `[STABLE]`). Breaking
changes bump this number and get a dated changelog entry at the bottom (AGENTS.md §6).

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

## 2. Device model  `[EXPERIMENTAL]`

```c
typedef enum { BUS_PCI, BUS_USB, BUS_PLATFORM } uno_bustype;

typedef struct uno_device {
    uno_bustype bus;
    union {                              /* physical location */
        struct { u8 bus, dev, fn; } pci;
        struct { u8 path[6], depth; } usb;   /* hub-port path from root */
    } addr;
    u16 vendor, device;                  /* PCI ven:dev  | USB idVendor:idProduct */
    u8  cls, subcls, prog_if, revision;  /* class triple (+ USB iface class triple) */
    u16 subsys_vendor, subsys_id;        /* PCI SSID (0 for USB)                    */
    u64 bar[6]; u32 bar_sz[6];           /* PCI resources (0 for USB)               */
    u8  irq; u16 caps;                   /* MSI/MSI-X/PCIe capability bitset         */
    enum { DEV_UNBOUND, DEV_BOUND, DEV_FAILED } state;
    const struct uno_driver *drv; void *drvdata;
} uno_device;
```

Registry API (all in `uno_devmgr.h`):

```c
void         devmgr_enumerate(void);              /* (re)scan all buses, fill the table */
int          devmgr_count(void);
uno_device  *devmgr_get(int idx);
uno_device  *devmgr_find(u16 ven, u16 dev);       /* first match, else NULL              */
const char  *devmgr_class_name(u8 cls, u8 sub);   /* "display","network","storage",...  */
```

`devmgr_enumerate()` is idempotent: it rebuilds the table, preserving `drv`/`drvdata`
for devices still present at the same address (so a re-scan does not tear down bound
drivers).

---

## 3. PCI enumeration  `[EXPERIMENTAL]`

In `uno_devmgr.c`, **consuming** `pc64_pci.c`'s existing accessors
(`pci_cfg_read32/16`, `pci_cfg_write32`, `pci_bar`) — the shared PCI file is **not**
edited.

- **Recursive** by bridge: start at bus 0; for each type-1 header (a PCI-PCI bridge,
  class `0x06/0x04`) follow `SecondaryBus` and recurse. (The current flat 0..255 loop
  misses anything behind a bridge — e.g. cards in the ZimaBlade's PCIe x4 slot.)
- Per function: read `ven:dev` (skip `0xFFFF`), class triple, revision, subsystem IDs,
  header type (multi-function bit), the 6 **BARs with size probing** (save, write
  `0xFFFFFFFF`, read mask, restore; handle 64-bit BAR pairs), and walk the
  **capability list** (`0x34` → chained) recording MSI/MSI-X/PCIe presence.

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

## 7. Introspection — the point of Phase 1  `[EXPERIMENTAL → STABLE]`

- **`uno.pci()` / `uno.usb()` / `uno.devices()`** — Python bindings appended to the
  `mod_uno.c` module table (additive seam). Each returns a list of
  `(loc, ven, dev, cls, sub, driver_or_None)`.
- **URC `devices` verb** — one line per device `loc ven:dev class driver|UNCLAIMED`,
  mirroring `disks`. This is a **request to unoautomate** (its dispatch), filed when
  Phase 1 lands; until then `py import uno; print(uno.devices())` over the `py` verb.
- **A "Device Manager" unoui app** — a tree of devices, each bound driver or
  `no driver — 8086:5A85 class 0x03 (display)` spelled out. Later phase.

---

## 8. Rollout (each phase lands small and green — AGENTS.md §3)

| Phase | Slice | Payoff |
|---|---|---|
| **1** | `uno_devmgr.{c,h}` PCI enumerator + registry + `uno.devices()` (read-only) | The real driver-less map of the ZimaBlade — answers the immediate question |
| **2** | `uno_driver` + `UNO_DRIVER` linker set; migrate r8169/e1000 to match tables | New PCI driver = a match table, not a probe-list edit |
| **3** | USB enumerator into the same registry; migrate HID / AX88179 | USB devices first-class + visible |
| **4** | Loadable `\DRIVERS\*.UNO` + hotplug re-scan | Ship/third-party drivers; PCIe/USB hotplug |

## 9. Territory (AGENTS.md §1–2)

Own: `uno_devmgr.{c,h}`, `DEVICES.md`. Consume unchanged: `pc64_pci.c`, `xhci.c`, the
`uno_nic_t`/`blkdev`/input/audio/GOP seams. Additive-only seam touches: `mod_uno.c`
table, `build.sh` file list (append `uno_devmgr.c`), the `.uno_drivers` linker set.
The URC `devices` verb is unoautomate's to add (request).

---

## Changelog

- **2026-07-23 — UNO_DEVMGR_API 0 (initial design):** subsystem claimed, contract
  written. No code yet; Phase 1 (`uno_devmgr` PCI enumerator + `uno.devices()`) next.
