# UnoDOS/Genesis — storage architecture

Four storage tiers for the Genesis port, in implementation order. The
Genesis has no ADC and no disk hardware; everything here is either
cartridge-resident memory or a 1-bit/serial interface on the control
ports, in the same passive-adapter spirit as the PS/2 wiring
(docs/GENESIS-PORT.md).

| Tier | Medium | Status | Hardware needed |
|---|---|---|---|
| 1 | Cartridge SRAM (8KB, battery) | **shipped** (M4) | none — emulators + flashcarts |
| 2 | Tape / WAV over audio (AFSK) | **shipped** (M4.5) | comparator on port 2 pin 1 (read); none to write |
| 3 | Sega CD backup RAM (Mode 1) | **shipped** (M5, `genesis/bram.i`) | a Sega/Mega CD attachment |
| 4 | SD card over bit-banged SPI | spec below — deferred | level-shifted SD breakout on a control port |

---

## Tier 1 — cartridge SRAM (IMPLEMENTED, `genesis/sram.i`)

8KB of battery-backed SRAM on the odd byte lane, declared in the ROM
header (`"RA" $F8 $20`, `$200001-$203FFF`). Byte *n* of the store is
at `$200001 + 2n`. `$A130F1` is written `1` once at boot and never
touched again — with a 64KB ROM there is no address overlap, and
toggling the register per-access breaks the mapping under BlastEm.

Mini-filesystem (**USV1**): magic[4] + count.w + heaptop.w at offset 0,
eight 16-byte directory entries (name[12], size.w, off.w) at offset 16,
and a byte heap from offset 144. Save-by-name overwrites
(delete-compact + append); delete compacts the heap and renumbers.
All fields big-endian; SRAM is private, not interchange media.

Apps: **Files** (proc 7) lists the store (Enter opens into Notepad,
`d` deletes), **Notepad F1** saves the buffer under its current name
(`UNTITLED.TXT` for new buffers, the source name for opened files).
Verified end-to-end in BlastEm (AUTOTEST_SRAM: F1-save, buffer wipe,
reopen from the listing).

## Tier 2 — tape / WAV over audio (IMPLEMENTED, `genesis/tape.i` + `genesis/mktape.py`)

The classic 1-bit tape interface. The console has no ADC, so reads go
through a comparator — exactly like the ZX Spectrum's EAR input:

- **Write — zero hardware.** The PSG generates the FSK; the Model 1
  headphone jack (or any console's line audio) records to a cassette
  deck or to a PC as WAV. Interrupts are masked during the write
  (~20s for a full 2KB Notepad buffer at 1200 baud).
- **Read — one comparator.** Tape/WAV playback → LM393 (or a single
  transistor + Schmitt) squaring the ~1V audio to 5V TTL → control
  port 2 pin 1 (D0), ground on pin 8, +5V for the comparator on pin 5.
  The read loop is its own timebase: it counts poll iterations
  (~5.7µs each) between input edges — no free-running timer, no HV
  counter quirks, and PAL is within tolerance automatically.

**Format** (Kansas City Standard at 1200 baud): `0` = one cycle of
1200 Hz, `1` = two cycles of 2400 Hz; byte = start(0) + 8 data
LSB-first + 2 stop(1); block = ~1.5s 2400 Hz leader + `UT01` +
name[12] + len.w + data + additive sum.w. 2KB ≈ 20 seconds of audio.

The decoder (`tape_feed_half`) is an injectable pure routine — the
AUTOTEST_TAPE build clocks a synthetic block through it in the
emulator, and `genesis/mktape.py` implements the same state machine in
Python: `encode` renders a file to a playable 44.1kHz WAV, `decode`
recovers a file from a recorded WAV, `selftest` round-trips in memory.
The PC is the tape deck: play the WAV into the adapter to load;
record the console to WAV (then `decode`) to save. UI: Files `w`
writes the Notepad buffer to tape, `r` reads a block back.

Real-hardware checklist: comparator polarity/threshold, the
TAPE_THRESH constant against a real deck's wow/flutter (the
SHORT/LONG decision point sits at ~310µs between nominal 208µs and
417µs halves — generous), and azimuth on well-worn cassettes.

---

## Tier 3 — Sega CD backup RAM (IMPLEMENTED, `genesis/bram.i`)

For consoles with a Sega/Mega CD attached, its battery-backed backup
RAM (8KB internal, up to 512KB on a Backup RAM cartridge) is real,
format-documented storage — files saved there are visible to the
console's own Backup RAM manager and other CD software.

**Architecture (Mode 1):** the Genesis cartridge stays the booted
program; the CD attachment is a peripheral. The Sub-CPU (the CD's own
68000) is the only processor with backup-RAM access, so the kernel:

1. **Detects** the attachment: probe `$400100` for the "SEGA" BIOS
   signature region / gate array at `$A12000` (no CD → all tiers
   above still work; the Files app simply doesn't list the BRAM
   volume).
2. **Boots the Sub-CPU**: write the gate-array reset/bus-request
   registers (`$A12000/$A12002`), copy a small Sub-CPU stub program
   into Program RAM through the 2Mbit window, point the Sub-CPU
   vector table at it, and release reset. The stub is ~1KB of 68000
   built with the same vasm.
3. **Speaks through the mailbox registers**: the gate array's
   communication flags/command words (`$A1200E-$A1202F` main side)
   carry a tiny RPC: `LIST / READ name / WRITE name len / DELETE
   name`, with data staged through the Word RAM (2Mbit mode swap).
4. The Sub-CPU stub calls the **BIOS BURAM traps** (`_BURAM`,
   function codes BRMINIT / BRMSTAT / BRMSERCH / BRMREAD / BRMWRITE /
   BRMDEL) so the on-disk format is the standard Sega directory —
   interchangeable, fsck'd by the console's own manager, and the size
   accounting ("blocks free") matches what users see elsewhere.

**File mapping:** BRAM names are 11 characters (the BIOS pads with
spaces); UnoDOS names map 1:1 with the dot dropped (`DEMO.TXT` →
`DEMO_TXT`-style normalization, recorded in the file's first block so
round trips restore the original name). Block size is 64 bytes plus
directory overhead; a 2KB Notepad file costs ~33 blocks of the
internal 125.

**Files app integration:** a volume toggle (`v`) cycles SRAM → BRAM
(when detected) → SRAM; the rest of the UI (Enter/d/F1 semantics) is
identical. The mailbox RPC is synchronous with a vblank-bounded
timeout so a wedged Sub-CPU can't hang the desktop.

**Emulator story:** BlastEm's CD support is limited; Genesis Plus GX
and Ares model Mode 1 + BRAM well. The RPC layer gets an injectable
transport (same pattern as PS/2/tape) so the protocol logic is
CI-testable without any CD emulation; the BIOS-trap stub needs a
CD-capable emulator or real hardware.

**Risks:** Mode-1 bring-up is the documented-but-fiddly part (gate
array handshake ordering); BIOS version differences (JP/US/EU model 1
vs 2 vs CDX) are absorbed by calling through the official trap table
rather than fixed addresses.

**Implementation notes (M5, shipped 2026-06-12):** as spec'd above,
with these refinements learned in bring-up:

- The Sub-CPU BIOS is Kosinski-compressed inside the main BIOS ROM;
  the loader checks the standard candidates ($415800, $416000,
  $41AD00 WonderMega/LaserActive, $40D500) for the "SEGA"/"WOND"
  signature at +$6D, clears PRG-RAM bank 0 first (the LaserActive
  hangs on dirty RAM), and write-protects the decompressed BIOS
  (byte write to $A12002 — a word write would clobber the bank/DMNA
  bits at $A12003).
- **Bus-error recovery around the probe**: with no attachment,
  $400000+ reads return open bus on real hardware but raise a 68000
  bus error under BlastEm. The probe records an unwind SP and arms
  vector 2; a fault during detection resumes at the no-CD exit
  instead of crashing the boot. The $A10001 DISK bit is checked
  first but not trusted (not all emulators model it).
- The RPC sequence byte skips 0 (idle) and the live ack value, so a
  wrapped counter can never alias the stub's 'R' ready flag.
- File payloads carry a 14-byte header (original 12-char name +
  byte length) so round trips restore exact names/lengths; foreign
  saves fail the header sanity check and open raw. Listings cap at 8
  entries (one BRMDIR call); paging is a follow-up.
- The fake transport implements the same four ops over a RAM block
  store with 64-byte blocks and delete-compaction; AUTOTEST_BRAM
  drives the full Notepad-F1 → wipe → Files → reopen round trip
  through it in BlastEm. The `_BURAM` stub follows the documented
  ABI (megadev's register contracts; BRMWRITE with d1=0 per Tech
  Bulletin #1) and awaits a CD-capable emulator / real hardware run.

## Tier 4 — SD card over bit-banged SPI (SPEC — deferred)

Real removable FAT storage on a control port; the endgame that gives
the Genesis the same "PC-interchangeable media" story as the Amiga
port's DF1 disks.

**Wiring (port 2, same connector convention as the other adapters):**

| DE-9 pin | MD signal | SPI signal | Notes |
|---|---|---|---|
| 1 | D0 | MISO (card → console) | input |
| 2 | D1 | MOSI (console → card) | output, 5V→3.3V divider |
| 3 | D2 | SCLK | output, divider |
| 4 | D3 | CS | output, divider |
| 5 | +5V | — | feeds a 3.3V LDO for the card |
| 8 | GND | GND | common |

Dividers (1.8k/3.3k) suffice for the three console→card lines at
bit-bang speeds; MISO's 3.3V high reads as TTL high directly. TH/TL
stay free (TH could clock a future interrupt-driven design).

**Driver stack:**

1. `spi.i` — bit-banged SPI mode 0: set MOSI, pulse SCLK, sample
   MISO; ~15-20 CPU cycles per bit ≈ 50-60 KB/s raw, far above need.
   Init clocks 80 cycles with CS high at "≤400kHz" (trivially
   satisfied), then runs flat out.
2. `sd.i` — SD/SDHC init (CMD0 → CMD8 → ACMD41 loop → CMD58 → CMD16),
   single-block CMD17 reads / CMD24 writes with CRC off (CMD59),
   512-byte blocks, byte-addressed vs block-addressed handled from
   the CMD8/OCR responses. Bounded retry counters everywhere — a
   missing card fails out in milliseconds.
3. `fat16.i` — the portable FAT core: the Amiga port's `fat12.i`
   already proves the shape (BPB parse, root dir, cluster chains,
   alloc/flush); this is its FAT16 generalization over a 512-byte
   block device interface (`blk_read(lba, buf)` / `blk_write`).
   8.3 names map directly onto the existing Files/Notepad semantics.
4. Files app: third volume in the `v` cycle; directory listing pages
   beyond 8 entries.

**RAM budget:** one 512-byte sector buffer + FAT cache sector + BPB
fields ≈ 1.2KB of work RAM — fits alongside everything else.

**Emulator story:** none model SPI-on-control-port. Like PS/2: the
SD layer runs against the injectable block-device interface
(`AUTOTEST_SD` serves a tiny FAT16 image from ROM), so the entire
filesystem stack is emulator-verified; only `spi.i`'s pin wiggling is
real-hardware-only.

**Why deferred:** needs the most adapter hardware (regulator + level
shifting + card socket), and SRAM/tape/BRAM already cover
persistence. It lands best together with a real adapter PCB that
also carries the PS/2 sockets and the tape comparator — one "UnoDOS
Genesis I/O adapter" for the real-hardware milestone.

---

## The unifying pattern

Every tier follows the port's standing rule: **protocol engines are
injectable pure routines, emulator-verified through synthetic inputs;
only the physical pin layer waits for real hardware.** SRAM needed no
split (emulators model it); tape/CD/SD each expose their decoder /
RPC / block layer to AUTOTEST builds, so CI keeps covering them as
the kernel evolves.
