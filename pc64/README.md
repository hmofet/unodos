# UnoDOS / pc64 — modern PC (x86-64, UEFI)

The **first modern-PC world**: the portable C core (the same `unodos.c` /
`mac_compat` / `fb` stack the Mac, PS2, and Dreamcast ports run) booting
**bare-metal on any 64-bit UEFI PC** — which is to say, essentially every
x86 machine made since ~2007, the hardware the 16-bit real-mode reference
kernel can never reach (no BIOS, no INT 13h/10h/15h, peripherals that only
exist above the 1MB line).

**Firmware-as-BIOS, the family pattern.** The Pi asks the VideoCore firmware
for its framebuffer; the PowerPC Mac boots as an Open Firmware client and
keeps the CI alive; pc64 does the identical thing with UEFI: GOP hands the
kernel a linear 32bpp framebuffer, Simple Text Input (Ex) hands it the
keyboard with modifier state, the Simple/Absolute Pointer protocols hand it
a mouse where the firmware binds one, and **boot services stay alive** — UEFI
plays exactly the role INT 10h/13h/15h play for the x86 reference kernel.
`ExitBootServices` + native drivers (xHCI, NVMe/AHCI, e1000) are the
*driver tail*, not a bring-up requirement — and they are the point at which
`unobus`/`unonet` get their first real PC hardware backends.

## Status — M1–M3 QEMU+OVMF-verified (scripted, headless) + ✅ **validated on real hardware**

**Boots and runs on a Lenovo ThinkPad X1 Carbon Gen 8** (2026-07-15, USB-stick
ESP boot): desktop, keyboard nav, SysInfo — the first modern-PC silicon under
any UnoDOS. The metal pass caught three real-hardware-only bugs the emulator
can never show, each now baked into the layer (see Design notes): the
640×480 SetMode eDP black-panel trap, an SMM-trapped debug port, and
phantom-keystroke firmware.

| Milestone | Evidence |
|---|---|
| **M1 boot → desktop**: OVMF loads `BOOTX64.EFI` from the ESP; GOP mode-scan picks native 640×480; splash → full 11-icon desktop | `shots/m1_desktop.png` |
| **M2 windows + input**: keyboard nav (arrows/Enter/Esc over PS/2-compat conin), SysInfo / Clock / Files windows, Notepad typing with live Ln/Co/bytes | `shots/m2_sysinfo.png`, `m2_clock.png`, `m2_notepad.png` |
| **M2 storage**: Ctrl-S (Text Input Ex reports Ctrl → the Mac `cmdKey`) saves NOTES.TXT to the FAT12 volume; Files lists it (111 B) after refresh | `shots/m2_files_saved.png` |
| **M3 game + audio**: Dostris playing (scripted drops, score on the board); Music/Tracker/Dostris voice PIT channel-2 PC-speaker square waves — the same audio the x86 reference kernel makes | `shots/m3_dostris.png` |
| **M4 networking** (native e1000 + a from-scratch TCP/IP stack): the Network app runs a live self-test — MAC read, link up, **DHCP lease**, **ICMP ping**, a **UDP** round-trip (TFTP RRQ→DATA), and a **TCP** connect+echo — all green against QEMU SLIRP | `shots/net_2.png` |
| **M4 TLS** (BearSSL): a **TLS 1.2** handshake to a pinned-key echo server, negotiating **ECDHE-ECDSA-AES128-GCM-SHA256** (`cs=0xC02B`), seeded from **RDRAND**, encrypted app-data round-trip — the Network app's 7th line | `shots/tls_1.png` |
| **M4 3D** (uno3d ported): the write-once "UnoDOS Runner" game — the same one the PS2 (GS) and Dreamcast (PVR) ports run — rendering full-screen through the software rasteriser into the GOP framebuffer, perspective + z-buffer + gouraud, score HUD | `shots/runner_2.png` |

All screenshots are captured by `harness.py` — QEMU is natively scriptable
(QMP `send-key` + `screendump`), so unlike the console ports this world needs
no ROM-free CPU-core harness: the *real* image boots the *real* firmware
headlessly on every run.

## What is reused vs. new

- **Reused verbatim from the Dreamcast port** (which reuses the PS2 port):
  `unodos.c` (core: WM, desktop, scheduler, FAT12, module loader),
  `mac_compat.*` (Toolbox shim), `fb.*` (software framebuffer), `app_loader.c`,
  `uno_app.h`, the 11 family apps. Core edits: three `UNO_PC64` hook lines in
  `main()`, the RAM-FAT12 fallback enabled, runtime fb dimensions
  (`FB_W/FB_H` resolve to variables on pc64), and the `uno_screen_changed()`
  resolution hook.
- **`apps/settings.c` — the family's first Settings app** (12th icon):
  display-resolution picker over a `display_res_*` tail added to the
  KernelApi (NULL on other ports), the C-core sibling of the x86 reference
  port's video-mode selector.
- **New platform + subsystems**:
  - `uefi.h` / `uefi_main.c` — handwritten minimal UEFI surface (no
    gnu-efi/EDK2) + GOP present, conin/pointer poll, PC speaker
  - `pc64_libc.c` / `pc64_math.c` — freestanding mem/str + first-fit malloc
    + the float math uno3d needs (`sinf/cosf/tanf/floorf/ceilf/sqrtf`); own
    `include/` headers, no host libc at all
  - `pc64_io.c` — File Manager over a RAM disk + Sound Manager → speaker
  - `pc64_modload.c` — static app registry (`-DUNO_APP_SYM=...`)
  - `pc64_pci.c` — PCI config-space scan (ports 0xCF8/0xCFC)
  - `e1000.c` — **native Intel e1000 (82540EM) NIC driver** — the family's
    first real `nic`-service hardware backend (MMIO descriptor rings; DMA is
    trivial while UEFI keeps the machine identity-mapped)
  - `net.c` — **a from-scratch TCP/IP stack**: Ethernet / ARP / IPv4 /
    ICMP-echo / UDP / a minimal single-connection TCP + a DHCP client
  - `apps/network.c` — the Network self-test app (13th icon)
  - `apps/runner.c` — the **uno3d "UnoDOS Runner" 3D game** (14th icon),
    linking `../uno3d/{uno3d,uno3d_soft,uno3d_intel,uno3d_game}.c` — the
    write-once game the PS2/DC ports run, here on the software rasteriser
    into the GOP framebuffer

## Networking (native e1000 + hand-rolled TCP/IP)

The e1000 driver finds the card by PCI scan (8086:100E/100F/10D3), turns on
bus-mastering, reads the MAC from EEPROM, and drives 16-deep RX/TX legacy
descriptor rings by MMIO — publishing the family's `uno_nic_t`
(`send`/`recv`/`link`). `net.c` sits on top: ARP with a small cache, IPv4
with checksums, ICMP echo (both directions), UDP with a few bound ports, a
DHCP client, and a minimal TCP (active open, one in-flight segment,
retransmit, close). Static config matches QEMU SLIRP (10.0.2.15/24, gw
10.0.2.2); DHCP overrides it.

`harness.py net` verifies the whole stack headlessly against SLIRP: DHCP
lease, ping the gateway, a **UDP** round-trip via SLIRP's built-in TFTP
server (RRQ → DATA), and a **TCP** connect+echo to a `guestfwd` host `cat`.
A packet-capture (`filter-dump`) pass confirmed the wire traffic during
bring-up — which is how the LLP64 footgun below was caught.

> **The bring-up bug worth remembering:** mingw is **LLP64** — `long` is
> **32-bit**. The descriptor struct used `unsigned long` for the 64-bit DMA
> address, so each descriptor was 12 bytes not 16; the NIC read the ring
> completely misaligned and transmitted multi-KB zero frames. Fixed by using
> `unsigned long long` / `uintptr_t` for every DMA address. On real hardware
> loaded above 4 GB the same truncation would have been fatal, not just
> corrupting — the 32-bit cast is now gone everywhere.

## unoui shell — the toolkit as the whole UI (`./build.sh uui`)

`pc64_uui.c` is an alternative shell that makes the cross-platform **unoui**
widget toolkit the entire UnoDOS UI: a themed desktop + window manager +
retained-mode widgets (dropdowns, checkboxes, sliders, spinners, buttons,
menubars, multi-line text areas, fields, lists, scrollbars, tabs), rendered
into `fb` and scaled to the panel. It replaces the ad-hoc per-app drawing and
the key-combo reliance — **every control is reachable by pointer OR keyboard**
(Tab focus, arrows, Enter), so it needs no mouse.

- Build: `./build.sh uui` — a lean 80 KB image (platform + fb + RAM-disk FS +
  unoui + 8 themes; no legacy core/apps/net/3D). The default `./build.sh`
  still produces the full legacy build.
- The shell ships four functional windows: a **Control Panel** (live theme
  dropdown → re-skins the whole desktop across all 8 themes; live resolution
  dropdown → `uno_pc64_res_set`; checkboxes/slider/spinner), an **Editor**
  (File/Edit menubar, real multi-line editing, filename field, format
  dropdown, Save/Open/New wired to the RAM-disk File Manager), a **Files**
  list, and a **System** panel.
- The only pc64-specific code is the ~40-line event adapter (UEFI input →
  `unoui_event`) + the window tree + action handlers; unoui owns everything
  else. Verified in QEMU: boots, renders the full widget set, and
  **keyboard-drives live theme switching** (UnoDOS → Windows 3.1 →
  Mac Plus → …) — `shots/uui_desktop.png`, `shots/uui_theme2.png`.

Folding the games / Network / Runner3D into unoui windows and making the
unoui shell the default is the continued migration.

## Native I2C-HID trackpad (foundation — needs hardware bring-up)

Modern laptop trackpads (2015+, incl. the X1 Carbon Gen 8) are **I2C-HID**
devices behind an Intel LPSS DesignWare I2C controller, described in ACPI. A
native driver replaces the firmware's (jerky) Absolute Pointer with direct
control. `i2c_hid.c` has the two reusable, spec-defined layers — the
**DesignWare I2C master** and **HID-over-I2C** (descriptor / SET_POWER / RESET
/ input-report read) — plus a first-pass report parser and a raw-dump hook.

**Honest status**: this is *unverifiable in QEMU* (no emulated I2C touchpad),
so it is **gated behind `-DUNO_I2C_TRACKPAD` and OFF by default** — the shipped
build is byte-for-byte unaffected (it compiles to inert stubs; `present()`
returns 0 and the firmware-pointer path is used). Bring-up needs two device
facts that normally come from ACPI/AML (not parsed yet):

1. From Linux on the same machine: the **LPSS I2C controller** (auto-found if
   PCI-visible; else set `I2C_HID_BASE` from `dmesg | grep i2c_designware`),
   the **slave address** (`i2cdetect` — Synaptics ~0x2c, Elan ~0x15/0x2a), and
   the **HID descriptor register** (almost always 0x0020).
2. Build `UNO_EXTRA='-DUNO_I2C_TRACKPAD -DI2C_HID_ADDR=0x2c ...' ./build.sh`,
   boot, and capture real report bytes via `uno_i2c_hid_dump()` — the report
   layout is device-specific (defined by the HID report descriptor), so the
   parser gets refined from those bytes.

The firmware-pointer path was also fixed independently (latched buttons so
held clicks register; only the first pointer instance drives the cursor so a
touchpad and TrackPoint don't fight) — that alone may resolve the jerky-pad /
dead-button symptoms on machines where the firmware exposes the pad.

## TLS (BearSSL)

TLS is **BearSSL** (`bearssl/`), chosen for security over convenience: it is
constant-time by design, a small audited single-author codebase, and — decisive
for us — freestanding with **no dynamic allocation and no OS dependencies**, so
it drops into the `-ffreestanding -nostdlib` build where mbedTLS/OpenSSL would
not. We roll **none** of our own crypto.

- **Build**: a freestanding `bearssl/src/config.h` forces portable C only (no
  CPU intrinsics, no int128, no OS entropy, no clock); the 8 CPU-accel /
  OS-entropy files that pull `<intrin.h>`/`<unistd.h>` are excluded (their
  portable equivalents are built). ~286 BearSSL sources compile unmodified
  with the same mingw flags as the rest of the port.
- **Trust = pinned key** (`br_x509_knownkey`): the client pins the server's
  P-256 public key, so no CA store and **no system clock** are needed — a
  strong, simple model for a fixed endpoint, and a clean fit for a machine
  with no RTC wired yet.
- **Entropy**: `tls.c` seeds BearSSL's PRNG from **RDRAND** when the CPU
  advertises it (CPUID leaf 1, ECX bit 30), else a documented TSC-mix
  fallback (demo-grade — a real deployment wires a proper source here).
- **Transport**: `tls.c` drives the BearSSL record engine (`br_sslio`) over
  `net_tcp_send/recv`, fragmenting to the stack's one-segment-in-flight TCP.
- **Verified**: `nettest.py` runs a per-connection TLS echo server on the
  host (reachable via SLIRP `guestfwd`), and the Network app completes a full
  **TLS 1.2** handshake negotiating **ECDHE-ECDSA-AES128-GCM-SHA256**,
  validates the pinned key, and round-trips encrypted application data — with
  `-cpu max` exposing RDRAND. (`no-TLS-1.3` only because the test pins TLS
  1.2 to match a fixed suite; BearSSL itself supports 1.2.)

`tls_test/` holds the throwaway EC key/cert and the stdio-TLS test server;
regenerate with `gen.sh` and re-pin `PINNED_EC_Q` in `tls.c`.

## 3D — uno3d ported (software rasteriser; Intel backend scaffolded)

uno3d's portable pipeline (`uno3d.c`) + software rasteriser (`uno3d_soft.c`)
now build freestanding on pc64 (the z-buffer is sized to the runtime fb
ceiling; the math comes from `pc64_math.c`). The **write-once** UnoDOS Runner
game runs full-screen at the current desktop resolution — the *same*
`uno3d_game.c` the PS2 (GS) and Dreamcast (PVR) ports run, proving the
backend abstraction across two real-3D consoles and now bare-metal x86-64.

`uno3d_intel.c` is the **Intel-iGPU backend scaffold**: `u3d_backend_intel`
probes PCI for an Intel display device (8086, class 0x03) and today delegates
every draw to the software rasteriser (identical output, CPU-timed) — so
selecting "intel" always renders correctly. A real hardware path (Gen command
streamer: GTT/GEM buffers, a 3DPRIMITIVE batch, depth target, a display-engine
flip) is the same *native driver tail* as xHCI/NVMe; when it lands, only the
four backend hooks change — no app or pipeline edits. This is deliberately
honest: the vtable is the contract, the hardware path swaps in behind it.

## Toolchain & build

UEFI applications are PE32+ images in the MS x64 calling convention — the
mingw cross compiler's native output, so the whole port builds with stock
Ubuntu packages, no EDK2:

```
sudo apt install gcc-mingw-w64-x86-64 qemu-system-x86 ovmf
./build.sh            # -> build/BOOTX64.EFI + build/esp/EFI/BOOT/BOOTX64.EFI
./build.sh run        # boot it in QEMU+OVMF (VNC :0)
python3 harness.py    # the scripted M1-M3 verification pass -> shots/*.png
```

**Real hardware:** format a USB stick FAT32, copy `build/esp/*` onto it
(so it holds `EFI/BOOT/BOOTX64.EFI`), boot the target machine with Secure
Boot disabled. Any 64-bit UEFI PC is in scope; pending the first physical
pass.

## Design notes (each metal-hardened rule is marked ⚠)

- **⚠ Full resolution support without SetMode, FILL-scaled**: laptop eDP
  panels accept a 640×480 SetMode at the API level and then stop scanning out
  (black panel — the X1 Carbon did exactly this). So the GOP mode is KEPT and
  the desktop resolution changes instead: the **Settings app** offers the
  standard desktop-OS list, the framebuffer is runtime-sized (`uno_fb_w/h`,
  ceiling 1920×1200), and present **fractionally scales it to FILL the panel**
  — nearest-neighbour, aspect preserved, centred (16.16 fixed-point source
  maps, `gColMap`/`gRowMap`). So a *low* resolution fills the screen as a big
  chunky UI instead of sitting in a small letterboxed box. The core re-reads
  its screen rect via `uno_screen_changed()` and repaints, rescuing stranded
  windows. Boot default is ~half the panel (fills exactly at 2×). **F10**
  cycles GOP modes for external monitors. `uno_pc64_lowres()` drops the fb to
  ~¼ the panel for full-screen 3D (Runner), then the same scaler upscales it —
  ~16× fewer pixels for the software rasteriser, the difference between a
  slideshow and a playable game.
- **⚠ No legacy-port debug I/O on metal**: QEMU-debugcon writes (port
  0x402) hung the X1 Carbon — legacy-port I/O can be SMM-trapped and vendor
  SMI handlers mishandle unclaimed ports. Compiled out unless `-DUNO_DBGCON`.
- **⚠ Budgeted input drains**: firmware exists whose `ReadKeyStrokeEx`
  returns SUCCESS forever with phantom keystroke data; drain-until-NOT_READY
  is a hang. All drains are capped per frame.
- **Bring-up markers**: two stripe rows paint at the top of the screen as
  init and the first frame progress — the pc64 equivalent of the x86 boot
  chain's diagnostic letters. Row 1 red/green/cyan/white = platform init;
  row 2 gray/yellow/magenta/orange = core init + first polls; the gray
  segment turns light blue when the first frame presents.
- **Pixel formats**: the software fb is `0xAABBGGRR` (R low byte); present
  handles BGRX/RGBX linear framebuffers and falls back to `Blt()` rows when
  the GOP is BltOnly.
- **Keyboard**: `SimpleTextInputEx` on the ConIn handle when present (gives
  Ctrl/Shift state; Ctrl carries the Mac `cmdKey`, and Ctrl+letter control
  codes are normalized back to letters), plain `ReadKeyStroke` fallback.
- **Watchdog**: `SetWatchdogTimer(0,...)` first thing — UEFI otherwise
  resets the machine ~5 minutes into any app that hasn't exited.
- **⚠ Mouse (TrackPoint-validated on the X1)**: every Simple/Absolute
  Pointer *device* instance is polled, after a BDS-style `ConnectController`
  sweep — but the ConIn splitter aggregate is a fallback ONLY: it re-reports
  the same devices, and polling both applied every movement twice (the
  "jerky TrackPoint"). Relative motion uses sub-count accumulators (a plain
  delta/Resolution divide rounds slow motion to zero). Note QEMU can't
  exercise this leg: Ubuntu's OVMF ships no pointer driver.
- **⚠ Dirty-row present**: rows are diffed against a shadow buffer and only
  changed rows hit VRAM. Full-screen uncached-VRAM rewrites made the first
  metal build's frame loop — and with it the input poll rate — crawl; an
  idle desktop now presents nearly for free, which is most of what makes
  the pointer feel smooth.
- **RAM disk + RAM FAT12**: `pc64_io.c` serves the File Manager from memory
  (seeded README.TXT), and the core's formatted-in-RAM FAT12 volume (the
  Executor-test fallback, enabled for `UNO_PC64`) is the mountable PC
  volume Files/Notepad use. Both persist in-session only, by design, until
  the block driver lands.

## The driver tail (where unobus/unonet meet real PC hardware)

1. **e1000** (`nic`) — ✅ **done**: `unonet`'s first real PC link + a
   from-scratch TCP/IP stack (see Networking above). virtio-net is the next
   easy addition; a `HEADLESS+NET` build of this world is the family's
   server story.
2. **ExitBootServices** + own long-mode setup (GDT/IDT, APIC timer, PCIe
   ECAM via ACPI MCFG) — graduates the port from firmware-hosted to
   self-hosted (and makes DMA independent of the firmware identity map).
3. **xHCI + USB HID** (`input`) — keyboard/mouse without firmware help; also
   unblocks the mouse on every machine.
4. **NVMe, then AHCI** (`block`) — real persistence: the FAT12 code in the
   core (and `unofs`) gets a device; apps go storage-loaded like PS2/DC
   (`pc64_modload.c` shrinks to a FAT read).
5. **Intel iGPU 3D** (`uno3d_intel.c`) — the Gen command-streamer path behind
   the already-wired `u3d_backend_intel` vtable (see 3D above).
6. **Intel HDA** (`audio`) — beyond the PC speaker.

## Files

```
pc64/
├── build.sh            # mingw-w64 build -> BOOTX64.EFI + ESP tree
├── harness.py          # QEMU+OVMF QMP harness (boot / full pass / mouse)
├── uefi.h  uefi_main.c # the UEFI platform layer
├── pc64_libc.c pc64_math.c  # freestanding libc + float math (+ include/*.h)
├── pc64_io.c           # RAM-disk File Manager + PC-speaker Sound Manager
├── pc64_modload.c      # static app registry (14 apps)
├── pc64_pci.c          # PCI config-space scan
├── e1000.c e1000.h     # native Intel e1000 NIC driver
├── net.c net.h uno_nic.h  # Ethernet/ARP/IPv4/ICMP/UDP/TCP + DHCP
├── tls.c tls.h         # BearSSL TLS client (pinned key, RDRAND entropy)
├── bearssl/            # vendored BearSSL (inc/ src/ + freestanding config.h)
├── tls_test/           # throwaway EC cert + stdio-TLS test server
├── nettest.py          # net+TLS QEMU verification harness
├── fb.c fb.h mac_compat.c mac_compat.h unodos.c app_loader.c uno_app.h
│                       # the shared portable core (Dreamcast copies + UNO_PC64 gates)
├── apps/               # 14 apps + uno_mod.h (Settings/Network/Runner3D are pc64)
│                       # Runner3D links ../uno3d/{uno3d,uno3d_soft,uno3d_intel,uno3d_game}
└── shots/              # harness-captured evidence
```
