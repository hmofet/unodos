/* unodevices - PCI/USB device tree + driver registry.  See DEVICES.md.
 *
 * Phase 1 (this file): the machine's PCI functions enumerated ONCE into a
 * registry of uno_device nodes - parent-linked, with class triple, subsystem
 * ids, capability list and BAR bases - plus text/tuple introspection so
 * "what hardware is here, and what has no driver?" is answerable on-device
 * and over URC.  Read-only: enumeration performs no config-space WRITES at
 * all (see devmgr_size_bars for the one opt-in exception).
 *
 * Owns discovery; CONSUMES pc64_pci.c's config accessors and does not edit
 * that shared file.  Nothing binds yet - every device reports UNCLAIMED until
 * phase 2 lands the driver registry. */
#ifndef UNO_DEVMGR_H
#define UNO_DEVMGR_H

/* Bumped on any breaking change to the surface below, with a dated DEVICES.md
 * changelog entry (AGENTS.md §6).  1 = the phase-1 registry, 2 = the driver
 * registry + fixpoint binding (phase 2) and loadable drivers (phase 4).
 *
 * The bump is ADDITIVE: every phase-1 entry point keeps its signature and
 * meaning, so a phase-1 consumer needs no change.  It is a bump rather than a
 * silent addition because `state` can now be BOUND and `drv` non-NULL, and the
 * phase-1 contract promised those never happened. */
#define UNO_DEVMGR_API 2

/* Table capacity.  A dense server board is ~60 functions; the X1 is ~30. */
#define UNO_DEV_MAX      128
#define UNO_DEV_NOPARENT (-1)

typedef enum { UNO_BUS_PCI = 0, UNO_BUS_USB = 1, UNO_BUS_PLATFORM = 2 } uno_bustype;

/* Binding state.  Phase 1 only ever produces UNBOUND. */
enum { UNO_DEV_UNBOUND = 0, UNO_DEV_BOUND = 1, UNO_DEV_FAILED = 2, UNO_DEV_GONE = 3 };

/* caps bitset: which capability structures the config-space walk found */
#define UNO_DEVCAP_PM    0x0001   /* 0x01 power management       */
#define UNO_DEVCAP_MSI   0x0002   /* 0x05 MSI                    */
#define UNO_DEVCAP_VNDR  0x0004   /* 0x09 vendor-specific        */
#define UNO_DEVCAP_PCIE  0x0008   /* 0x10 PCI Express            */
#define UNO_DEVCAP_MSIX  0x0010   /* 0x11 MSI-X                  */

/* bar_flags[] */
#define UNO_BAR_PRESENT  0x01
#define UNO_BAR_IO       0x02
#define UNO_BAR_MEM64    0x04
#define UNO_BAR_PREFETCH 0x08
#define UNO_BAR_SIZED    0x10     /* bar_sz[] is valid (devmgr_size_bars ran) */

typedef struct uno_device {
    unsigned char  bus_type;              /* uno_bustype                          */
    short          parent;                /* registry index of the bridge above,  */
                                          /* UNO_DEV_NOPARENT at the root          */
    union {
        struct { unsigned char bus, dev, fn; } pci;
        struct { unsigned char path[6], depth; } usb;   /* phase 3 */
    } addr;
    unsigned short vendor, device;
    unsigned short subsys_vendor, subsys_id;
    unsigned char  cls, subcls, prog_if, revision;
    unsigned char  hdr_type;              /* 0 = endpoint, 1 = PCI-PCI bridge      */
    unsigned char  irq_line, irq_pin;
    unsigned char  sec_bus;               /* bridges: secondary bus number, else 0 */
    unsigned short caps;                  /* UNO_DEVCAP_* bitset                   */
    unsigned char  cap_msi, cap_msix, cap_pcie;  /* config offsets, 0 = absent     */
    unsigned char  bar_flags[6];          /* UNO_BAR_*                             */
    unsigned long long bar[6];            /* base address, low flag bits masked    */
    unsigned long long bar_sz[6];         /* valid only with UNO_BAR_SIZED         */
    unsigned char  state;                 /* UNO_DEV_*                             */
    const char    *drv;                   /* bound driver name, NULL while unbound */
    void          *drvdata;
} uno_device;

/* (Re)scan every PCI bus into the registry; returns the device count.
 * Idempotent: drv/drvdata survive a re-scan for a device still present at the
 * same address.  Config-space READS only. */
int devmgr_enumerate(void);

/* Device count, enumerating on first use. */
int devmgr_count(void);

/* Registry accessors; NULL when idx is out of range / no match. */
uno_device *devmgr_get(int idx);
uno_device *devmgr_find(unsigned short ven, unsigned short dev);
uno_device *devmgr_find_class(unsigned char cls, unsigned char sub);

/* Short, single-token class name ("ethernet", "vga", "sata", "host-bridge"...).
 * Single-token is contractual: the URC `devices` host parser splits on
 * whitespace and takes the last token as the driver column. */
const char *devmgr_class_name(unsigned char cls, unsigned char sub);

/* Bound driver name, or NULL while unbound (always NULL in phase 1). */
const char *devmgr_driver_name(int idx);

/* Non-zero if the last scan hit UNO_DEV_MAX and stopped adding devices, i.e.
 * every listing is truncated and the registry is not the whole machine. */
int devmgr_overflow(void);

/* The whole machine, one line per PCI function (always NUL-terminated):
 *
 *   "bb:dd.f VVVV:DDDD cc/ss <class-name> <driver|UNCLAIMED>"
 *
 * Returns the length written, excluding the NUL, truncated to fit cap.  This
 * is the format the URC `devices` verb forwards verbatim. */
int devmgr_list_str(char *buf, int cap);

/* One device in detail (location, ids, subsystem, revision, class triple,
 * capabilities, BARs, parent, state) as several short lines. */
int devmgr_detail_str(int idx, char *buf, int cap);

/* Flat, struct-free row for callers across a module boundary (the pc64-python
 * uno.pci() binding): writes bus, dev, fn, vendor, device, cls, subcls,
 * prog_if, revision, subsys_vendor, subsys_id, caps, state, parent, irq_line
 * into out[] and returns the count written (DEVMGR_ROW_N), or -1. */
#define DEVMGR_ROW_N 15
int devmgr_info(int idx, unsigned int *out, int nmax);

/* OPT-IN BAR sizing for one device: the write-0xFFFFFFFF probe, with memory +
 * I/O decode disabled around it and restored after.  NOT part of enumeration -
 * while UnoDOS is still attached, firmware drivers own their devices and a
 * decode gap on a live controller can wedge real hardware.  Refuses (returns
 * 0) on display-class devices and on any device holding the active GOP
 * framebuffer.  Returns the number of BARs sized. */
int devmgr_size_bars(int idx);

/* Register a synthetic PLATFORM device: a logical block that lives INSIDE an
 * already-enumerated PCI function rather than being its own PCI function - the
 * canonical case is the PCH TCO watchdog, which is decoded by the LPC/SMBus
 * function but is not itself enumerable.  `backing` is the registry index of
 * that PCI function; the platform node inherits its bb:dd.f location and ven:dev
 * for the listing, links to it as parent, records [io_base, io_base+io_len) as
 * an I/O BAR, and lists as BOUND to `drv`.  cls/sub give it a class-name token.
 *
 * The registration is STICKY: it is re-applied at the end of every
 * devmgr_enumerate(), so a re-scan (which rebuilds the PCI table) does not drop
 * it - matching the "enumerate is idempotent" contract (DEVICES.md §2).
 * Returns the new registry index, or -1 (bad backing index / table full).
 *
 * Additive to UNO_DEVMGR_API 1: no struct or ABI change (UNO_BUS_PLATFORM was
 * always in the bus-type enum); platform nodes are the first devices that can
 * report a bound driver.  See DEVICES.md §2 + changelog. */
int devmgr_add_platform(int backing, unsigned char cls, unsigned char sub,
                        unsigned long long io_base, unsigned long long io_len,
                        const char *drv);

/* ===========================================================================
 * PHASE 2 - the driver registry
 *
 * A driver declares what it can drive (a match table) and how to take it
 * (probe).  The manager matches, orders candidates by SPECIFICITY, and offers
 * each in turn until one accepts.  There is deliberately no priority field:
 * priority numbers are a coordination problem between files that do not know
 * about each other, and specificity plus probe-decline expresses the same
 * thing locally.  See DEVICES.md §5.
 * ======================================================================== */

/* Match entry kinds.  A table is terminated by a UNO_MATCH_END entry. */
enum {
    UNO_MATCH_END = 0,
    UNO_MATCH_PCI_ID,      /* vendor + device exact                        */
    UNO_MATCH_PCI_CLASS    /* cls + subcls, and prog_if when have_progif   */
};

typedef struct uno_match {
    unsigned char  kind;
    unsigned short vendor, device;      /* UNO_MATCH_PCI_ID                */
    unsigned char  cls, subcls, prog_if;/* UNO_MATCH_PCI_CLASS             */
    unsigned char  have_progif;
} uno_match;

/* Specificity, high wins.  Exactly the precedence the plan locked:
 * exact id > class/subclass/prog-if > class/subclass. */
#define UNO_SPEC_ID       3
#define UNO_SPEC_CLASSPI  2
#define UNO_SPEC_CLASS    1

struct uno_device;

typedef struct uno_driver {
    const char      *name;          /* single token: it is the listing column */
    unsigned char    bus;           /* uno_bustype                            */
    unsigned short   api;           /* UNO_DEVMGR_API this driver was built to */
    const uno_match *match;         /* UNO_MATCH_END-terminated                */
    /* Take the device, or decline.  1 = claimed (state becomes BOUND), 0 =
     * "not mine, try the next candidate".  A probe MUST be side-effect-free
     * when it declines, and must tolerate being called again on a later pass. */
    int  (*probe)(struct uno_device *d);
    /* Release it.  May be NULL.  After this returns the driver must not touch
     * the device's MMIO again - see the hotplug contract in DEVICES.md §7. */
    void (*remove)(struct uno_device *d);
} uno_driver;

/* Self-registration, the AGENTS.md §2 seam: a driver opts in by putting this
 * line in ITS OWN file, and nothing central is edited.
 *
 * The idiom is COFF grouped sections, NOT the ELF "custom section + KEEP()"
 * advice: this kernel is PE/COFF via mingw ld with -nostdlib and no linker
 * script.  The linker concatenates `.unodrv$a`, `.unodrv$m` and `.unodrv$z` in
 * `$`-suffix order, so the markers in uno_devmgr.c bracket the entries.
 * Constructor-based registration is NOT an option here: there is no CRT, so
 * __attribute__((constructor)) never runs. */
#define UNO_DRIVER(sym)                                                  \
    __attribute__((used, section(".unodrv$m")))                          \
    static const uno_driver *const sym##__reg = &sym

/* Bind every UNBOUND device that some driver claims, to a FIXPOINT: a pass
 * that binds nothing ends it.  Multi-pass rather than topologically ordered
 * because binding a controller can create children (plan decision 2), and a
 * child's driver must get its turn in a later pass.
 *
 * Idempotent and re-runnable: already-BOUND devices are skipped, so callers
 * re-run it whenever the world changes (the big one is uno_blk_detach(), where
 * storage controllers become ours for the first time).  Returns the number of
 * devices newly bound. */
int devmgr_bind_all(void);

/* Release a device: calls the driver's remove() (if any), clears the binding,
 * and leaves the node UNBOUND.  Used by hotplug and by an operator forcing a
 * rebind.  Returns 1 if something was released. */
int devmgr_release(int idx);

/* Re-scan the bus and diff against the tree (phase 4 hotplug).  Devices that
 * vanished are marked UNO_DEV_GONE and their drivers get remove(); new devices
 * are added UNBOUND.  Then binds to a fixpoint.  Returns the number of changes
 * (arrivals + departures).  Cheap enough to call from a timer, but nothing
 * calls it periodically yet - see DEVICES.md §7. */
int devmgr_rescan(void);

/* How many drivers are registered (built-in + loaded), for introspection. */
int devmgr_driver_count(void);
const char *devmgr_driver_at(int i);

/* ===========================================================================
 * PHASE 4 - loadable drivers
 *
 * A driver shipped as \DRIVERS\<NAME>.UNO exports one symbol through the
 * module entry point: a pointer to its uno_drv_module.  It receives a
 * VERSIONED SERVICES STRUCT and resolves nothing dynamically, so a driver
 * built against api N either matches the running services struct or is
 * refused outright.
 * ======================================================================== */

#define UNO_DRVSVC_API 1

/* UnoModHdr.flags for a driver module, alongside UNO_MODF_UUI (0x0001) and the
 * Python tiers (0x0002 / 0x0004).  Build one with:
 *   python3 tools/mkuno.py convert foo.dll DRIVERS/FOO.UNO 8
 * The loader refuses a module without it, so an app dropped into \DRIVERS\
 * cannot be called through the driver entry signature. */
#define UNO_MODF_DRV 0x0008

typedef struct uno_drv_services {
    unsigned short api;                    /* UNO_DRVSVC_API                 */
    /* config space */
    unsigned int (*cfg_read32)(const struct uno_device *d, int off);
    void         (*cfg_write32)(const struct uno_device *d, int off, unsigned int v);
    /* MMIO: the BAR is already in the node; this hands back a usable pointer
     * and enables decode.  No mapping is required on this platform (identity),
     * so it is a decode-enable plus a validity check, but drivers must go
     * through it so a future paged port has one place to change. */
    void        *(*map_bar)(struct uno_device *d, int bar, unsigned long long *len);
    /* DMA-safe allocation: identity-mapped, 64-byte aligned, never freed.
     * (The BOT lesson from usbmsc.c: DMA must not target the stack.) */
    void        *(*dma_alloc)(unsigned long bytes);
    /* MSI/MSI-X.  The manager owns this so drivers never touch _PRT or INTx
     * routing: every platform in this fleet has a history of unusable legacy
     * IRQs, so the house style is MSI everywhere.  Returns 0 on failure. */
    int          (*msi_enable)(struct uno_device *d, int vector);
    /* time + log */
    void         (*delay_ms)(int ms);
    unsigned long long (*rdtsc)(void);
    void         (*log)(const char *s);
} uno_drv_services;

/* What a .UNO driver's entry point returns. */
typedef struct uno_drv_module {
    unsigned short   api;           /* UNO_DRVSVC_API it was built against   */
    const uno_driver *drv;
} uno_drv_module;

/* A loadable driver's entry point signature. */
typedef const uno_drv_module *(*UnoDrvEntry)(const uno_drv_services *svc);

/* Load every \DRIVERS\*.UNO that some UNCLAIMED device might want, register
 * them, and bind.  Safe to call more than once; a driver already registered
 * under the same name is not loaded twice.  Returns the number of drivers
 * registered.  Runs AFTER the built-ins have had their turn, so a shipped
 * driver always beats a dropped-in one for the same device. */
int devmgr_load_drivers(void);

/* Register a driver at runtime (what devmgr_load_drivers uses, and what a
 * test can use directly).  Returns 0 if the table is full or the api does not
 * match. */
int devmgr_register(const uno_driver *drv);

#endif /* UNO_DEVMGR_H */
