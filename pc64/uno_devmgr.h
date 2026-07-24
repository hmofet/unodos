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
 * changelog entry (AGENTS.md §6).  1 = the phase-1 registry. */
#define UNO_DEVMGR_API 1

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

#endif /* UNO_DEVMGR_H */
