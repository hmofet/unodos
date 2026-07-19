# UnoDOS/pc64 native storage

pc64 owns its filesystem. Partition scanning and FAT16/32 read+write are done by
UnoDOS code over its own block layer; the UEFI firmware no longer parses the
filesystem — while attached it only moves sectors, and once the OS detaches
(a later milestone) even that becomes native. The practical result: **UnoDOS
runs from any FAT32 partition on any disk, not just the ESP the firmware
happened to expose.**

## Layers

```
 apps / shell            uno_fs_read / uno_fs_write / uno_fs_list  (pc64_fs.c)
        │
 fat.c                   native FAT16/32: dir walk, cluster chains, R/W/create/
        │                delete; GPT + MBR + superfloppy partition discovery;
        │                mounts EVERY FAT16/32 partition (ESP or basic-data)
        │
 blkdev.c                512 B/sector block-device registry
    ┌───┴───────────────────────────────────────────────┐
 ahci.c (native)                    EFI Block IO backend (blkdev.c)
 PCI 01/06, polled DMA,             wraps firmware whole-disk handles;
 no firmware, no IRQ                the sector transport while attached
    └── used when DETACHED ──┘      └── used while ATTACHED ──┘
```

`pc64_fs.c` presents one ordered volume list: RAM disk (0) → native FAT mounts →
any remaining firmware Simple-File-System volumes (e.g. the boot USB behind
xHCI, which has no native block driver), de-duplicated by FAT serial. The RAM
disk and native FAT volumes are writable (`uno_fs_write`); firmware SFS is
read-only here.

## One driver per controller (why the native drivers are gated)

While UEFI boot services are live, the **firmware** owns the AHCI/NVMe
controllers. If a native driver reprograms one (stops a port, repoints the
command list/queues to its own buffers, restarts), it breaks the firmware's own
Block IO — which corrupted an installer disk clone mid-write. So:

- **Attached** (boot services live): `blkdev` uses the **EFI Block IO backend**
  for every disk. Our FAT code still does all partition + filesystem work; only
  the sector transport is firmware.
- **Detached** (after `ExitBootServices` — M3, SHIPPED): the native drivers
  (`ahci.c` for SATA, `nvme.c` for PCIe SSDs) are the only things on the bus
  and firmware Block IO is gone.

The gate is `uno_pc64_detached()` (in `uefi_main.c`).  The detach itself is
automatic at the end of boot when the native stack covers the machine: linear
framebuffer + a native keyboard + a calibrated TSC + **a UnoDOS system volume
on a controller a native driver binds — AHCI (01/06) or NVMe (01/08)**
(`uno_fat_native_eligible()` — a foreign FAT partition alone must not count,
or a USB-booted system would detach away its own boot volume and the
firmware-dependent Install app).  The sequence is `uno_fat_sync()` (flush the
write-back cache over the dying transport) → `GetMemoryMap`+`ExitBootServices`
→ `uno_blk_detach()` (drop fw devices, `uno_ahci_init()` + `uno_nvme_init()`)
→ `uno_fat_remount()` → `uno_fs_remap()`.  Build with `-DUNO_NO_DETACH` to
keep a build permanently attached.

`nvme.c` is the same polled/bounded shape as `ahci.c`: admin + one I/O queue
pair in static 4 KB-aligned buffers, PRP1/PRP2 (+ a static PRP list page for
64 KB chunks), completion by phase-bit poll, every 512-byte-LBA namespace
registered as `nvme0..`. Namespaces formatted with 4 KB LBAs are skipped
(fat.c speaks 512-byte sectors). Verify with `python3 tools/nvme_test.py`:
boots the packed GPT image as an **NVMe-only** machine, asserts the detach,
opens a `.UNO` off the NVMe ESP (read path), saves in the Editor and pulls
`NOTES.TXT` back out of the raw image with mtools (write path).

**The dirty-line lesson (found by that test):** `uno_fat_write`'s create path
once wrote the new 8.3 name into a cache line *without* marking it dirty;
`fat_alloc`'s free-scan then churned the 8-line cache, evicting the clean line
and silently discarding the name — the final entry-fill stamped cluster/size
into a slot whose name was zeroed, i.e. an invisible file + leaked chain, on
every post-detach save on ANY native transport. If you touch a cache line's
bytes, `cache_put()` it in the same breath.

## Sector-buffer alignment (a footgun)

EFI Block IO honours `Media->IoAlign`, and native AHCI DMAs straight out of the
buffer. An **unaligned** sector buffer silently returns `0xFF`-filled garbage
(not an error) on some controllers/firmware. `fat.c`'s sector cache therefore
aligns its line buffers (`uint8_t buf[512] __attribute__((aligned(128)))`).
Symptom if you ever regress it: a raw `bdev->read(lba)` returns correct data but
the cached read of the same LBA returns `0xFF` — mount succeeds (label comes
from the BPB via a stack buffer) but every file read fails.

## Verifying (headless)

`python3 tools/storage_test.py` (WSL/Linux; `UNO_KVM=1` to use KVM,
`UNO_PREBUILT=1` to run an already-built self-test image without rebuilding)
boots the USB image plus a plain FAT32 **basic-data** (type 0700, *not* an ESP)
disk on the q35 AHCI controller. It confirms:

- **read** — a `MARKER.TXT` placed offline survives and is readable;
- **write + delete** — a gated boot-time self-test (`uno_fat_selftest()`, built
  only with `-DUNO_STORTEST`, inert unless a `WRTEST.REQ` file is present) turns
  `WRTEST.REQ` into `WRTEST.OK` and removes the request; mtools confirms offline.

The test rebuilds a clean production image (no self-test flag) afterward. Build
`-DUNO_DBGCON` to mirror `dbg_puts` to the QEMU debugcon log for driver
debugging.

## Files

| File | Role |
|---|---|
| `blkdev.{c,h}` | block-device registry + firmware-Block-IO backend + native gate |
| `ahci.c` | native AHCI (SATA) driver (detached-mode transport) |
| `nvme.c` | native NVMe driver (detached-mode transport) |
| `fat.{c,h}` | native FAT16/32 R/W + partition discovery + `uno_fat_selftest` |
| `pc64_fs.{c,h}` | unified volume list + `uno_fs_read/write/writable` |
| `tools/storage_test.py` | headless QEMU/KVM read+write+delete verification |
| `tools/nvme_test.py` | NVMe-only boot: detach + `.UNO` read + raw-verified write |
