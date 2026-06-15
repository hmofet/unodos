# UnoDOS — storage architecture (all platforms)

How every UnoDOS target persists files. The OS presents one storage
contract to apps — the Files browser and Notepad's save/load — but each
platform reaches it differently, because the underlying media range from
a 1.44 MB floppy to 8 KB of battery-backed cartridge SRAM to a console
memory card. This document is the cross-platform reference; the deep
per-tier detail for the Genesis (which has the richest storage story) is
at the end.

## The three storage strategies

Every port falls into one of three families, chosen by what the hardware
actually offers:

1. **Interchange filesystems (FAT12 / FAT16).** Where the medium is a
   real removable disk a PC could also read, the port runs the genuine
   filesystem so the disks are interchangeable. This is the x86
   reference (FAT12 floppy + FAT16 hard disk/CF/USB), the Amiga (FAT12
   on the DF1 drive), the Mac ports (PC FAT12 floppies alongside HFS),
   MacPlus (FAT12 on the .Sony disk), and the IIGS (FAT12 over the
   SmartPort block driver).

2. **USV1 mini-filesystem.** Where storage is a small private blob —
   battery SRAM or a track/sector region too small for FAT — the port
   runs **USV1**, UnoDOS's own fixed mini-FS (see format below). This is
   the SNES (battery SRAM), the Genesis (cartridge SRAM, tier 1), the
   C64 (a byte-heap variant on disk), and the Apple II (a track/sector
   mini-FS reached through its own GCR RWTS, because FAT12 doesn't fit
   the 5.25″ format under the space budget).

3. **Native platform stores.** Where the console provides a first-class
   save device with its own format, the port uses it so saves are
   visible to the system's own file manager. This is the PS2 memory card
   (via `libmc`), the Dreamcast VMU (via the KallistiOS VFS), and — on
   the Genesis — the optional Sega CD backup RAM (tier 3).

A handful of ports add **extra media** on top: the x86 reference
persists `SETTINGS.CFG`; the Genesis can additionally read/write **tape
/ WAV** (1-bit AFSK through the PSG) and, with a Sega CD attached, its
**backup RAM**; the Dreamcast keeps a flush-on-close buffer to the VMU.

## Cross-platform summary

| Port | Primary filesystem | Format | Extra media | Status |
|---|---|---|---|---|
| **x86 PC** | FAT12 floppy + FAT16 HD/CF/USB | FAT, full R/W | `SETTINGS.CFG` persistence | shipped |
| **Amiga** | FAT12 on DF1 (PC-interchangeable) | FAT12, full R/W | — | shipped (M3) |
| **Mac System 7** | HFS + PC FAT12 floppy | FAT12 + HFS | subdir navigation | shipped (M3) |
| **Mac System 1–6** | HFS + PC FAT12 floppy | FAT12 + HFS | subdir navigation | shipped (M3) |
| **MacPlus (OS)** | FAT12 on .Sony disk | FAT12 + disk-loaded apps | — | shipped (M3) |
| **Genesis** | USV1 in 8 KB battery SRAM | USV1 mini-FS | tape/WAV; Sega CD backup RAM; SD-over-SPI (deferred) | shipped (M4–M5) |
| **Apple II** | track/sector mini-FS | mini-FS over GCR RWTS | — | shipped (M2) |
| **Apple IIGS** | FAT12 over SmartPort blocks | FAT12, persistent | — | shipped |
| **SNES** | USV1 in battery SRAM | USV1 mini-FS | — | shipped |
| **C64** | byte-heap mini-FS on disk | USV1 variant | — | shipped (M2) |
| **PS2** | memory card | `libmc` (Sony format) | — | shipped (M2) |
| **Dreamcast** | VMU | KOS VFS | flush-on-close buffer | shipped |

Cartridge consoles aside, every storage-equipped port also **loads its
apps from this same storage** rather than baking them into the kernel —
see [MODULAR_APPS.md](MODULAR_APPS.md) (the shared C core) and the C64 /
Apple II / IIGS / Amiga / MacPlus READMEs for the per-port app-loader
contract. The two cartridge consoles (SNES, Genesis) keep their apps in
ROM because cartridge ROM is the correct delivery medium and there is no
writable code storage to load from.

## Shared format 1 — USV1 mini-filesystem

USV1 is the fixed mini-FS used wherever the store is a small private blob
(Genesis/SNES SRAM, C64 disk heap). Layout:

- **Header** at offset 0: `magic[4]` + `count.w` + `heaptop.w`.
- **Directory**: eight 16-byte entries at offset 16 — `name[12]`,
  `size.w`, `off.w`.
- **Heap**: a byte heap from offset 144.

Save-by-name overwrites (delete-compact then append); delete compacts the
heap and renumbers. All fields are big-endian on the 68000/65816 ports.
USV1 is private storage, **not** interchange media — it is not meant to
be read on another machine. The Files app lists the store (Enter opens a
file into Notepad, `d` deletes); Notepad's save key writes the buffer
under its current name (`UNTITLED.*` for a new buffer, the source name
for an opened file). The C64 port uses a byte-heap variant of the same
structure sized to its disk loader.

## Shared format 2 — FAT12 / FAT16

The interchange filesystem family. FAT12 covers 1.44 MB floppies (12-bit
entries, dual FAT copies, full mount/open/read/write/create/delete/
rename/seek/readdir); FAT16 covers the x86 hard-disk/CF path (16-bit
entries, MBR + partition table, multi-sector clusters, LBA with CHS
fallback). The same driver API routes by mount handle, so apps don't know
which one is mounted. Because these are real FAT volumes, the disks are
**PC-interchangeable** — a key goal on the Amiga (DF1) and Mac (data
floppies) ports.

The portable C core's FAT12 stack (`fat12_mount`/`list`/`read`/`write`)
is exposed to apps through the `KernelApi` table — see
[MODULAR_APPS.md](MODULAR_APPS.md). The Amiga's `fat12.i` proves the
native-assembly shape (BPB parse, root dir, cluster chains, alloc/flush)
that the Genesis tier-4 SD spec generalizes to FAT16 over a 512-byte
block device.

## Native platform stores

- **PS2 — memory card (`libmc`).** Files/Notepad persist to the memory
  card via the PS2 File Manager. The card is not a POSIX filesystem;
  files are created with `mcOpen` + `sceMcFileCreateFile`. The storage
  read path is real on hardware; see [ps2/README.md](../ps2/README.md).
- **Dreamcast — VMU (KOS VFS).** Files persist to the VMU through the
  KallistiOS virtual filesystem, with a flush-on-close buffer. The
  Notepad save→reload round trip is captured in Flycast
  (`dreamcast/shots/dc_vmu.png`); see
  [dreamcast/README.md](../dreamcast/README.md).
- **Apple IIGS — FAT12 over SmartPort.** Files persist across reboot on a
  FAT12 volume reached through the firmware SmartPort block driver; see
  [iigs/README.md](../iigs/README.md).

---

# Genesis storage — the four tiers

The Genesis has the richest storage story because it has no disk hardware
and no ADC: everything is either cartridge-resident memory or a
1-bit/serial interface on the control ports, in the same passive-adapter
spirit as the port's PS/2 wiring ([genesis/README.md](../genesis/README.md)).
Four tiers, in implementation order:

| Tier | Medium | Status | Hardware needed |
|---|---|---|---|
| 1 | Cartridge SRAM (8KB, battery) | **shipped** (M4) | none — emulators + flashcarts |
| 2 | Tape / WAV over audio (AFSK) | **shipped** (M4.5) | comparator on port 2 pin 1 (read); none to write |
| 3 | Sega CD backup RAM (Mode 1) | **shipped** (M5, `genesis/bram.i`) | a Sega/Mega CD attachment |
| 4 | SD card over bit-banged SPI | spec below — deferred | level-shifted SD breakout on a control port |

## Tier 1 — cartridge SRAM (`genesis/sram.i`)

8KB of battery-backed SRAM on the odd byte lane, declared in the ROM
header (`"RA" $F8 $20`, `$200001-$203FFF`). Byte *n* of the store is at
`$200001 + 2n`. `$A130F1` is written `1` once at boot and never touched
again — with a 64KB ROM there is no address overlap, and toggling the
register per-access breaks the mapping under BlastEm. The store holds a
**USV1** mini-filesystem (see the shared format above).

Apps: **Files** (proc 7) lists the store, **Notepad F1** saves the buffer
under its current name. Verified end-to-end in BlastEm (AUTOTEST_SRAM:
F1-save, buffer wipe, reopen from the listing). Traps: index SRAM with
`(a0,d0.w)` (a stale high word in a `.l` index reaches random buses), and
write `$A130F1 = 1` once at boot (the per-access on/off dance unmapped
SRAM for good under BlastEm).

## Tier 2 — tape / WAV over audio (`genesis/tape.i` + `genesis/mktape.py`)

The classic 1-bit tape interface. The console has no ADC, so reads go
through a comparator — exactly like the ZX Spectrum's EAR input:

- **Write — zero hardware.** The PSG generates the FSK; the Model 1
  headphone jack (or any console's line audio) records to a cassette deck
  or to a PC as WAV. Interrupts are masked during the write (~20s for a
  full 2KB Notepad buffer at 1200 baud).
- **Read — one comparator.** Tape/WAV playback → LM393 (or a single
  transistor + Schmitt) squaring the ~1V audio to 5V TTL → control port 2
  pin 1 (D0), ground on pin 8, +5V for the comparator on pin 5. The read
  loop is its own timebase: it counts poll iterations (~5.7µs each)
  between input edges — no free-running timer, no HV counter quirks, and
  PAL is within tolerance automatically.

**Format** (Kansas City Standard at 1200 baud): `0` = one cycle of
1200 Hz, `1` = two cycles of 2400 Hz; byte = start(0) + 8 data LSB-first
+ 2 stop(1); block = ~1.5s 2400 Hz leader + `UT01` + name[12] + len.w +
data + additive sum.w. 2KB ≈ 20 seconds of audio.

The decoder (`tape_feed_half`) is an injectable pure routine — the
AUTOTEST_TAPE build clocks a synthetic block through it in the emulator,
and `genesis/mktape.py` implements the same state machine in Python:
`encode` renders a file to a playable 44.1kHz WAV, `decode` recovers a
file from a recorded WAV, `selftest` round-trips in memory. The PC is the
tape deck: play the WAV into the adapter to load; record the console to
WAV (then `decode`) to save. UI: Files `w` writes the Notepad buffer to
tape, `r` reads a block back.

Real-hardware checklist: comparator polarity/threshold, the TAPE_THRESH
constant against a real deck's wow/flutter (the SHORT/LONG decision point
sits at ~310µs between nominal 208µs and 417µs halves — generous), and
azimuth on well-worn cassettes.

## Tier 3 — Sega CD backup RAM (`genesis/bram.i`)

For consoles with a Sega/Mega CD attached, its battery-backed backup RAM
(8KB internal, up to 512KB on a Backup RAM cartridge) is real,
format-documented storage — files saved there are visible to the
console's own Backup RAM manager and other CD software.

**Architecture (Mode 1):** the Genesis cartridge stays the booted
program; the CD attachment is a peripheral. The Sub-CPU (the CD's own
68000) is the only processor with backup-RAM access, so the kernel:

1. **Detects** the attachment: probe `$400100` for the "SEGA" BIOS
   signature region / gate array at `$A12000` (no CD → all tiers above
   still work; the Files app simply doesn't list the BRAM volume).
2. **Boots the Sub-CPU**: write the gate-array reset/bus-request
   registers (`$A12000/$A12002`), copy a small Sub-CPU stub program into
   Program RAM through the 2Mbit window, point the Sub-CPU vector table at
   it, and release reset. The stub is ~1KB of 68000 built with the same
   vasm.
3. **Speaks through the mailbox registers**: the gate array's
   communication flags/command words (`$A1200E-$A1202F` main side) carry
   a tiny RPC: `LIST / READ name / WRITE name len / DELETE name`, with
   data staged through the Word RAM (2Mbit mode swap).
4. The Sub-CPU stub calls the **BIOS BURAM traps** (`_BURAM`, function
   codes BRMINIT / BRMSTAT / BRMSERCH / BRMREAD / BRMWRITE / BRMDEL) so
   the on-disk format is the standard Sega directory — interchangeable,
   fsck'd by the console's own manager, and the size accounting ("blocks
   free") matches what users see elsewhere.

**File mapping:** BRAM names are 11 characters (the BIOS pads with
spaces); UnoDOS names map 1:1 with the dot dropped, recorded in the
file's first block so round trips restore the original name. Block size
is 64 bytes plus directory overhead; a 2KB Notepad file costs ~33 blocks
of the internal 125.

**Files app integration:** a volume toggle (`v`) cycles SRAM → BRAM (when
detected) → SRAM; the rest of the UI (Enter/d/F1 semantics) is identical.
The mailbox RPC is synchronous with a vblank-bounded timeout so a wedged
Sub-CPU can't hang the desktop.

**Emulator story:** BlastEm's CD support is limited; Genesis Plus GX and
Ares model Mode 1 + BRAM well. The RPC layer gets an injectable transport
(same pattern as PS/2/tape) so the protocol logic is CI-testable without
any CD emulation; the BIOS-trap stub needs a CD-capable emulator or real
hardware.

**Risks:** Mode-1 bring-up is the documented-but-fiddly part (gate array
handshake ordering); BIOS version differences (JP/US/EU model 1 vs 2 vs
CDX) are absorbed by calling through the official trap table rather than
fixed addresses.

**Implementation notes (M5, shipped 2026-06-12):** as spec'd above, with
these refinements learned in bring-up:

- The Sub-CPU BIOS is Kosinski-compressed inside the main BIOS ROM; the
  loader checks the standard candidates ($415800, $416000, $41AD00
  WonderMega/LaserActive, $40D500) for the "SEGA"/"WOND" signature at
  +$6D, clears PRG-RAM bank 0 first (the LaserActive hangs on dirty RAM),
  and write-protects the decompressed BIOS (byte write to $A12002 — a
  word write would clobber the bank/DMNA bits at $A12003).
- **Bus-error recovery around the probe**: with no attachment, $400000+
  reads return open bus on real hardware but raise a 68000 bus error
  under BlastEm. The probe records an unwind SP and arms vector 2; a
  fault during detection resumes at the no-CD exit instead of crashing
  the boot. The $A10001 DISK bit is checked first but not trusted (not
  all emulators model it).
- The RPC sequence byte skips 0 (idle) and the live ack value, so a
  wrapped counter can never alias the stub's 'R' ready flag.
- File payloads carry a 14-byte header (original 12-char name + byte
  length) so round trips restore exact names/lengths; foreign saves fail
  the header sanity check and open raw. Listings cap at 8 entries (one
  BRMDIR call); paging is a follow-up.
- The fake transport implements the same four ops over a RAM block store
  with 64-byte blocks and delete-compaction; AUTOTEST_BRAM drives the
  full Notepad-F1 → wipe → Files → reopen round trip through it in
  BlastEm. The `_BURAM` stub follows the documented ABI (megadev's
  register contracts; BRMWRITE with d1=0 per Tech Bulletin #1) and awaits
  a CD-capable emulator / real hardware run.

## Tier 4 — SD card over bit-banged SPI (SPEC — deferred)

Real removable FAT storage on a control port; the endgame that gives the
Genesis the same "PC-interchangeable media" story as the Amiga port's DF1
disks.

**Wiring (port 2, same connector convention as the other adapters):**

| DE-9 pin | MD signal | SPI signal | Notes |
|---|---|---|---|
| 1 | D0 | MISO (card → console) | input |
| 2 | D1 | MOSI (console → card) | output, 5V→3.3V divider |
| 3 | D2 | SCLK | output, divider |
| 4 | D3 | CS | output, divider |
| 5 | +5V | — | feeds a 3.3V LDO for the card |
| 8 | GND | GND | common |

Dividers (1.8k/3.3k) suffice for the three console→card lines at bit-bang
speeds; MISO's 3.3V high reads as TTL high directly. TH/TL stay free (TH
could clock a future interrupt-driven design).

**Driver stack:**

1. `spi.i` — bit-banged SPI mode 0: set MOSI, pulse SCLK, sample MISO;
   ~15-20 CPU cycles per bit ≈ 50-60 KB/s raw, far above need. Init
   clocks 80 cycles with CS high at "≤400kHz" (trivially satisfied), then
   runs flat out.
2. `sd.i` — SD/SDHC init (CMD0 → CMD8 → ACMD41 loop → CMD58 → CMD16),
   single-block CMD17 reads / CMD24 writes with CRC off (CMD59), 512-byte
   blocks, byte-addressed vs block-addressed handled from the CMD8/OCR
   responses. Bounded retry counters everywhere — a missing card fails out
   in milliseconds.
3. `fat16.i` — the portable FAT core: the Amiga port's `fat12.i` already
   proves the shape (BPB parse, root dir, cluster chains, alloc/flush);
   this is its FAT16 generalization over a 512-byte block device interface
   (`blk_read(lba, buf)` / `blk_write`). 8.3 names map directly onto the
   existing Files/Notepad semantics.
4. Files app: third volume in the `v` cycle; directory listing pages
   beyond 8 entries.

**RAM budget:** one 512-byte sector buffer + FAT cache sector + BPB fields
≈ 1.2KB of work RAM — fits alongside everything else.

**Emulator story:** none model SPI-on-control-port. Like PS/2: the SD
layer runs against the injectable block-device interface (`AUTOTEST_SD`
serves a tiny FAT16 image from ROM), so the entire filesystem stack is
emulator-verified; only `spi.i`'s pin wiggling is real-hardware-only.

**Why deferred:** needs the most adapter hardware (regulator + level
shifting + card socket), and SRAM/tape/BRAM already cover persistence. It
lands best together with a real adapter PCB that also carries the PS/2
sockets and the tape comparator — one "UnoDOS Genesis I/O adapter" for the
real-hardware milestone.

---

## The unifying pattern

Across the whole family, two principles keep storage testable:

- **Injectable protocol engines.** Wherever a port talks to hardware
  through a custom protocol (Genesis tape decoder, Sega CD mailbox RPC,
  SD/SPI block layer, the PS/2 wiring), the protocol engine is an
  injectable pure routine, emulator-verified through synthetic inputs;
  only the physical pin layer waits for real hardware. SRAM and the
  console memory cards/VMU need no split (emulators model them); tape/CD/
  SD each expose their decoder / RPC / block layer to AUTOTEST builds, so
  CI keeps covering them as the kernel evolves.
- **One Files/Notepad contract.** Apps see the same save/load/list
  semantics on every platform regardless of whether the backend is FAT12,
  USV1, a memory card, or a VMU — the platform difference lives entirely
  below the `KernelApi` storage calls.
