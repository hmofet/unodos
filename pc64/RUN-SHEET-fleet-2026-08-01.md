# Fleet metal run sheet, 2026-08-01

Three machines, one stick. Written for a session at the hardware; read the
preamble once, then the per-machine sheet.

---

## RESULTS OF THE RUN (added after the fact)

| Machine | Outcome |
|---|---|
| **X1 Carbon** | **Boots.** The bar-3 freeze regression is fixed on this machine. Sees WiFi networks; **cannot join SKYNET**. Trackpad/pointer readouts and the install/detach test NOT captured. |
| **X13 Yoga** | Boots, **joins SKYNET**. Detach regression check not formally captured. |
| **Eee PC 1005** | Loader narration and dots seen, then an unread flash, then black. **Inconclusive and confounded** - see below. |

**Two mistakes in this sheet's own premise, both mine, both fixed for next time:**

1. **The stick was written with `dd`, which ships `DEBUG.CFG` with `passes=3`.**
   That arms the fuzz driver and powers the machine off after three passes, and
   carries no URC key. It is the flasher's Developer options that normally write
   those keys, so bypassing the flasher bypassed them. The Eee PC's "black
   screen" may simply be that auto power-off.
2. **The sheet assumed the X1 could be driven over URC. It cannot.** Its only
   NIC is the Intel radio, and the radio is what is broken, so there is no link
   to dial out on. Use an AX88179 USB-Ethernet dongle, or carry
   `CRASH\<machine>\BOOTLOG.TXT` + NETLOG off on the stick.

Full write-up, including the BSS-selection hypothesis for the SKYNET failure and
the QEMU `-snapshot` trap, is the 2026-08-01 metal entry in
[UNOAUTOMATE-REQUESTS.md](UNOAUTOMATE-REQUESTS.md). The Eee PC analysis is in
[docs/BIOS-BOOT-PLAN.md](../docs/BIOS-BOOT-PLAN.md) under "Metal, 2026-08-01".

**Images staged on devbuntu for the next session:** `~/unodos-urc.img` (debug,
`nostress noshutdown mtrr-wc remote=192.168.2.100:5099`, currently on the Cruzer
Glide) and `~/unodos-eee.img` (production, verbose stage2, no `DEBUG.CFG`).

**The stick as written:** `unodos-fleet.img`, md5 `ac25494e130a604d2690e625208746c5`,
96 MiB hybrid (MBR, one FAT32 partition typed 0xEF at LBA 16384), on the
Verbatim STORE N GO, serial `FC090CB174377695`. Written from devbuntu and
verified byte-for-byte off the medium with caches dropped; devbuntu's USB
write-blocker is back at `ro=1`.

**What is in it:** `UNO_DEBUG=1` kernel (phase D asks for the debug build, and
the `CRASH\` boot log only exists there) plus a `UNO_BIOS_VERBOSE=1` stage2.
The verbose stage2 only ever executes on a **BIOS** boot, so it costs the two
ThinkPads nothing and it is what turns an Eee PC black screen into data.

**Verified before writing:** this exact image booted to the desktop under
SeaBIOS (1024x768) and under OVMF (1280x800). Both boots were run with QEMU
`-snapshot`, so the bytes on the stick are the bytes that were tested, not a
copy that a test run had written to.

**The volume is 88 MiB**, not 29 GB, because this is a raw hybrid image rather
than a flasher-authored whole-disk volume. There is ~80 MB free, which is
plenty for the `.WAV` / `.MID` the Music check wants.

---

## Preamble, all machines

- **Secure Boot must be off.** `BOOTX64.EFI` is unsigned. Both ThinkPads:
  firmware setup, Security → Secure Boot → Disabled.
- **Boot menu:** ThinkPad **F12**. Eee PC **Esc** at POST (**F2** for setup) —
  confirm on the machine, the Eee firmware varies a little by revision.
- **The escape hatch needs no rebuild.** A file `DETACH.CFG` at the root of the
  stick's FAT volume containing the word `off` refuses detach outright;
  `nousb` refuses only the USB arm. Drop it from any machine that can mount the
  stick. Worth creating *before* you start if you would rather a first boot not
  detach at all.
- **What to capture on every box**, per the phase D procedure:
  - the `detach gate:` line from the env block in `CRASH\<machine>\BOOTLOG.TXT`
  - `uno.devices()` (or the URC `devices` verb) saved to `docs/fleet/<machine>.txt`
  - `docs/fleet/` does not exist yet; create it on the first save.

---

## 1. X1 Carbon Gen 8 — do this one first

This is the machine that can **falsify** a claim already merged to master, so
it is worth more than the other two combined. Read [DETACH.md](DETACH.md) §5
before you start.

### 1.1 Does it boot at all

The first cut of the trackpad work hung **both** this machine and the Surface
at splash **bar 3** (`uno_i2c_hid_init()`). That fix has never been run on
metal. This is the first thing to check and everything below depends on it.

- **Boots** → carry on.
- **Freezes at bar 3** → stop. Say so and the SCL-timing sweep gets gated off
  by default. Do not chase anything else first.
- **Freezes somewhere else** → note the bar number. Because this is a
  `UNO_DEBUG=1` build, UBSan traps are compiled in as `ud2`, so a debug-only
  halt is possible where production is fine. Before concluding the regression
  is back, reflash with a production image and retry. (Say the word and I will
  build one; it is a five-minute turnaround.)

### 1.2 System window, the trackpad readout

Open **System**. Read the `Trackpad I2C:` lines verbatim.

| What you see | What it means |
|---|---|
| `HID device: UP addr 0x2c parsed  scl#1` | the 216 MHz Comet Lake timing was the fix |
| `... parsed  scl#0` | it bound on the historical pair after all, and the timing sweep was never needed |
| `no HID (bus ok, abrt 0x....)` | report the `abrt` value — it now distinguishes a NAK from a dead bus |
| `no HID (no ACK, abrt 0x....)` | same, report it |
| `no controller (ACPI-only?)` | the LPSS controllers were not found at all |

Nobody has called `scl#0` vs `scl#1` yet. Either answer is useful; the point is
that it is currently a coin flip.

### 1.3 System window, the pointer readout

Same window:

```
Pointer: fw simple N / abs N   (live)
  PS/2: kbd up, aux port ok, mouse streaming id 0
```

On a **USB-stick boot** the machine stays attached, so the TrackPoint runs
through firmware and the counts should be non-zero with `(live)`.

`aux port ok` + `mouse streaming id 0` is the interesting one: it means the
TrackPoint survives detach on PS/2 and the gate could be relaxed later.
`aux port none` means it cannot, and the gate is load-bearing.

### 1.4 The real test: install and boot from internal storage

> **Destructive-option warning.** The Install app lists two kinds of target.
> Pick the ESP:
>
> - `Volume "..."  ESP (has \EFI)  [keeps data]` — **non-destructive.** Copies
>   into `\EFI\UNODOS\` and adds a UEFI boot entry. Windows keeps booting.
> - `Disk ...  [ERASES ALL]` — **wipes the machine.** It asks twice and wants
>   `ERASE` typed. Do not pick this unless the Carbon is expendable.
>
> To undo afterwards: delete `\EFI\UNODOS\` from the ESP and remove the
> `UnoDOS` entry from the firmware boot menu (or `bcdedit /enum firmware` then
> `bcdedit /delete {id}` from Windows).

Install to the ESP, remove the stick, boot it. **This is the phase B test.**
One of three things happens:

1. **The pad binds and the machine detaches** → the prediction holds.
2. **The gate holds it attached**, System says `[attached to keep pointer]` →
   the prediction holds by the other branch. Also fine.
3. **It detaches and the pointer is dead** → the prediction failed. The System
   window should say `the i8042 aux mouse did not answer after detach`. That is
   the honest failure and it is exactly the result worth having. `DETACH.CFG:
   off` is the way back without a rebuild.

A detached machine with no pointer is a regression. Report it rather than
working around it.

### 1.5 While you are there

Cheap items from [METAL-CHECKLIST.md](METAL-CHECKLIST.md) that want this
machine and nothing else:

- **UI scale** 125% / 150% at native 1920x1200.
- **Editor at speed** — type fast, the layout cache and glyph banks should hold.
- **Files real ops** on the installed volume: new folder, rename, delete.
- **Music on real silicon** — copy a `.WAV` and a `.MID` onto the stick first.
  This is the first time the HDA path carries sampled audio on a real
  Realtek/Conexant codec rather than QEMU's two-widget model. Ear-check for
  crackle; check the seek slider tracks and that Pause/Stop respond.

---

## 2. X13 Yoga

A regression check, not a discovery run. It already detaches today, so the
question is only whether anything landed since broke it.

1. Boot the stick, confirm it still detaches, capture `CRASH\<machine>\BOOTLOG.TXT`
   and the device tree into `docs/fleet/`.
2. Keyboard, pointer, clock, restart — the same sweep the ZimaBlade passed.

**One correction to the checklist before you spend time on it.** METAL-CHECKLIST
says the Yoga is "the one machine that can exercise the PCH TCO watchdog metal
pass". **That line is stale** and the later findings contradict it twice over:

- The Yoga's firmware **locks the TCO** (`tco1_cnt_fw=0x1800` = TCO_LOCK+HLT),
  which the OS cannot un-halt.
- The Yoga is **PMC-class**, and only the v2/RCBA NO_REBOOT path is
  implemented; the PMC path is an unbuilt next slice.

So `uno_hw_wdt_present()` returns 0 there and it is *correct* to. There is no
TCO metal pass to run on this machine. Skip it.

**Worth doing instead, if the AX88179 dongle is to hand:** the DHCP
option-6 fix (request 1/3/6, fall back to the gateway as resolver when the
router omits option 6) is metal-pending on the next ethernet round. Bring the
link up, confirm DNS resolves, and the live checks `S-AI-01` / `S-AI-02` should
complete themselves over the real link. That closes NEXT-ITERATION.md phase 3.

---

## 3. Eee PC 1005 — BIOS boot

### 3.1 Check the CPU before you spend a boot

**The 1005 series splits across a hard exclusion, and the two halves look
identical.** pc64 is x86-64 only:

| Model | CPU | Long mode | Result |
|---|---|---|---|
| 1005**HA** / HAB / HAG | Atom **N270 / N280** (Diamondville) | **no** | cannot run pc64, ever |
| 1005**PE** / PEB / P / PR | Atom **N450 / N455** (Pineview) | **yes** | can run pc64 |

Check the label on the underside or the CPU in the firmware setup screen before
booting.

**If it is an N270 machine, boot it anyway — once.** Stage2 tests CPUID leaf
`0x80000001` bit 29 *before* it touches a page table, and prints:

```
This CPU has no long mode (no EM64T/x86-64). UnoDOS pc64 is 64-bit only
and cannot run here.
```

That refusal path has only ever been tested in QEMU against a simulated
Pentium 3. Seeing it on real silicon is a genuine result and it is the only
thing that machine can tell us. Photograph it and stop there.

### 3.2 Why this machine is a better BIOS target than the Revo

The Revo run stalled partly because of what that machine is. The Eee PC is
better on two counts and worse on one:

- **It has a real i8042.** The netbook's keyboard and touchpad are on the
  internal PS/2 controller, so unlike the Revo (USB-only input, no xHCI on that
  era, therefore no keyboard in UnoDOS even if it booted) this machine should be
  *drivable* if it comes up.
- **Its SATA is almost certainly in IDE compatibility mode** (NM10 / ICH7-M),
  which exercises `ide.c` — the PIO ATA driver that is currently only
  QEMU-verified against a PIIX3. That is the phase D metal validation the plan
  says is missing.
- **Its panel is 1024x600**, and the mode policy now prefers 1024x768. A card
  will advertise and successfully *set* a mode the panel cannot show, and the
  mode set returns success either way. So a black screen here is informative
  rather than surprising — it is the "panels whose best mode is 1024x768"
  quirk the plan names as needing metal.

### 3.3 The run

Boot the stick (Esc at POST). Stage2 is the verbose build, so it narrates:

```
UnoDOS pc64
pc64 stage2
a20 ok
.......
kernel loaded
e820 entries: N
vbe mode: 1024x768 lfb 0x???????? pitch 4096
continuing: ........      <- eight dots, one per second
```

**Photograph that screen**, whatever happens next. It is the whole diagnostic.

| Outcome | Meaning | Next |
|---|---|---|
| Desktop appears | phase D validated on real IDE-mode silicon. Check System reads `x86-64 legacy BIOS` and `DETACHED (native): ide.. 1 disk` | run the desktop, keyboard, apps |
| Narration then **black** | either the VBE mode is not reaching the panel, or long mode is failing | §3.4 |
| Narration stops early | note the last line printed | report it |
| Long-mode refusal | N270 machine, see §3.1 | stop |

### 3.4 If it goes black after the mode switch

This is the Revo symptom and it has two opposite causes, because the green
marker band is painted from 64-bit code. The `UNO_BIOS_NOVIDEO=1` probe settles
it: it skips VBE entirely, stays in text mode, enters long mode, and writes a
white-on-red banner to `0xB8000` from 64-bit code.

**That image is already built and staged on devbuntu** at
`~/unodos-novideo.img` (md5 `e708b07165d897bbfae961576887642a`). Swapping the
stick to it is one command:

```bash
ssh devbuntu 'sudo bash ~/write-stick.sh ~/unodos-novideo.img FC090CB174377695'
```

- **Banner appears** → long mode, page tables, GDT and SSE all work on this
  hardware and the fault is entirely the video mode. Next step is forcing
  800x600, then 640x480.
- **No banner** → the transition itself is failing and video was never the
  issue. Different search: page-table placement at 0x20000, the stack at
  0x90000 against this machine's EBDA, or the A20 path.

To go back to the fleet image, same command with `~/unodos-fleet.img`.

---

## Rebuilding any of this

```bash
cd pc64 && rm -rf build/esp && UNO_DEBUG=1 UNO_BIOS_VERBOSE=1 ./build.sh
```

produces `build/unodos-hybrid.img`. Verify it with QEMU **`-snapshot`** or the
guest writes its own boot log into the image you are about to ship.
