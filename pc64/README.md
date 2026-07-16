# UnoDOS / pc64 — modern PC (x86-64, UEFI)

The **first modern-PC world**: UnoDOS booting **bare-metal on any 64-bit UEFI
PC** — essentially every x86 machine since ~2007, the hardware the 16-bit
real-mode reference kernel can never reach (no BIOS, no INT 13h/10h/15h,
peripherals that only exist above the 1 MB line). The default UI is the
cross-platform **unoui** widget toolkit as a full themed desktop shell; a
`legacy` build keeps the earlier `unodos.c` core + the 14 family apps.

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

## Status

**`./build.sh` boots straight into the unoui shell** — a themed widget desktop
that defaults to **Aurora**, a modern flat/rounded look (soft shadows, an accent
title underline, a frosted taskbar), with 9 other live-switchable themes
(Aurora Light/Dark + the 8 retro looks) — a splash screen, window manager,
retained-mode toolkit, taskbar, desktop icons, a scrollable Start menu and a
system-tray clock. It carries the whole app roster: the native widget apps, the
migrated creative tools + games, Runner3D, the Network self-test, and the web
browser (which now loads pages over HTTP) — plus the net/TLS/3D drivers. The
default image is ~657 KB (most of it BearSSL + uno3d); the pure shell + toolkit
is a small fraction of that. **Validated on a
Lenovo ThinkPad X1 Carbon Gen 8** (USB-stick ESP boot): boots, TrackPoint +
keyboard (and a smoothed firmware-pointer path) drive it, live theme and
resolution switching. The metal pass hardened three emulator-invisible firmware
traps into the layer (see Design notes): the 640×480 SetMode eDP black-panel
trap, an SMM-trapped debug port, and phantom-keystroke firmware.

Everything below the shell is verified headlessly by `harness.py` — QEMU is
natively scriptable (QMP `send-key` + `screendump`), so the *real* image boots
the *real* firmware on every run; no ROM-free CPU-core harness needed.

`./build.sh legacy` builds the **previous** shell — the `unodos.c` core + an
icon desktop + the 14 hand-drawn family apps + the drivers below (networking,
TLS, 3D). It was the source for the app migration into unoui windows, which is
now complete (the games, creative tools, Network and Runner3D all run in the
default shell). The two builds share the platform layer, `fb`, the RAM-disk FS,
and all the drivers; only the UI layer differs.

## Layout: the platform, the two shells, the drivers

- **Platform (both builds)** — `uefi.h` / `uefi_main.c` (handwritten minimal
  UEFI surface, no gnu-efi/EDK2: GOP present, conin/pointer poll + smoothing, PC
  speaker, fill-scaling, plus the UEFI **runtime services** it now uses — the
  real-time clock `GetTime`/`SetTime`, `ResetSystem` for shut down / restart —
  and the **Simple File System** protocol for FAT volumes), `fb.*` (software
  framebuffer), `pc64_libc.c` / `pc64_math.c` (freestanding libc + float math,
  own `include/*.h`, no host libc), `pc64_io.c` (RAM-disk File Manager +
  PC-speaker Sound Manager), `pc64_fs.c` (unified RAM + FAT namespace),
  `pc64_pci.c` (PCI config scan).
- **Default shell (unoui)** — `pc64_uui.c` (desktop/taskbar/Start menu/tray) +
  the cross-platform `../unoui/` toolkit + 10 themes (Aurora Light/Dark +
  `theme_aurora.c`, plus 8 retro looks), with `pc64_uui_apps.c` (the
  `mac_compat` bridge for the creative tools + Network), `pc64_games.c` (native
  Dostris/Pacman/Outlast + Runner3D canvases), `pc64_browser.c` + `pc64_fs.c` +
  `js.c` + `pc64_http.c` (the web browser, file system, JS engine and HTTP),
  `pc64_icons.c` (procedural icons) and `../unosound/unosound_seq.c` (audio).
  See "unoui shell" and "Web browser" below.
- **Legacy shell** — `unodos.c` (WM + icon desktop + module dispatch) +
  `mac_compat.c` (Mac-Toolbox/QuickDraw shim) + `app_loader.c` +
  `pc64_modload.c` + `apps/*.c` (14 apps incl. `settings.c`, `network.c`,
  `runner.c`). Built only by `./build.sh legacy`.
- **Drivers (linked by the build that needs them)** — `e1000.c` (native NIC),
  `net.c` (TCP/IP stack), `tls.c` + `bearssl/` (TLS), `../uno3d/*` (3D),
  `i2c_hid.c` (native trackpad, opt-in). Documented in their own sections.

## Networking (native e1000 + hand-rolled TCP/IP)

The e1000 driver finds the card by PCI scan (8086:100E/100F/10D3), turns on
bus-mastering, reads the MAC from EEPROM, and drives 16-deep RX/TX legacy
descriptor rings by MMIO — publishing the family's `uno_nic_t`
(`send`/`recv`/`link`). `net.c` sits on top: ARP with a small cache, IPv4
with checksums, ICMP echo (both directions), UDP with a few bound ports, a
DHCP client, and a minimal TCP (active open, one in-flight segment,
retransmit, close). Static config matches QEMU SLIRP (10.0.2.15/24, gw
10.0.2.2); DHCP overrides it.

The **Network** self-test app (legacy build) drives it; `nettest.py` verifies
the whole stack headlessly against SLIRP: DHCP
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

## unoui shell — the DEFAULT UI (`./build.sh`)

`pc64_uui.c` is the shell: it makes the cross-platform **unoui** widget
toolkit the entire UnoDOS UI — a themed desktop + window manager +
retained-mode widgets (dropdowns, checkboxes, sliders, spinners, buttons,
menubars, multi-line text areas, fields, lists, scrollbars, tabs), rendered
into `fb` and scaled to the panel. It replaces the ad-hoc per-app drawing and
key-combo reliance — **every control is reachable by pointer OR keyboard**
(Tab focus, arrows, Enter), so it needs no mouse.

- **Desktop furniture**: a **Start button** opens a **scrollable Start menu**
  (hover the up/down chevrons to scroll a roster larger than the screen; it
  ends with **Restart** and **Shut Down**, which call UEFI `ResetSystem`); a
  **taskbar** along the bottom shows a chip per open window (click to focus /
  raise) and a **system-tray clock** (real wall-clock time from the UEFI
  runtime `GetTime`, not just uptime); **desktop icons** launch apps directly.
  Keyboard-only stays first-class: **Ctrl-Esc** opens the Start menu,
  **Ctrl-W** closes the focused window, **F2** / **Ctrl-Tab** cycles windows.
  Each window carries a **title-bar close box**, and dragging a window moves a
  light outline (no full-window redraw storm).
- **The app roster**: six native widget apps — a **Control Panel** (live theme
  dropdown → re-skins the desktop across all 10 themes; a **Dark mode** toggle
  that swaps Aurora Light↔Dark; live resolution dropdown; **date/time spinners**
  that set the UEFI clock; checkboxes/slider),
  an **Editor** (File/Edit menubar, multi-line editing, Save/Open/New wired to
  the RAM-disk File Manager), a **Files** list, a **System** panel, a **Clock**
  and a **Canvas** demo — plus the migrated **creative tools** (Paint, Music,
  Tracker), the native **games** (Dostris, Pacman, Outlast), **Runner3D**, the
  **Network** self-test, and the **web browser**. Windows are sized to the theme
  metrics so no widget overflows the frame.
- **Startup**: a **splash screen** with a loading bar paints while the shell and
  drivers come up, then a short **startup chime** (UnoSound) plays as the
  desktop appears.
- **Aurora — the modern default theme** (`themes/theme_aurora.c`, light + dark):
  a unique design language drawing from Windows 11 (rounded, mica-ish), macOS
  (clean surfaces, left close, translucency) and Material (tonal surfaces, one
  strong accent). It's a **graphics theme** — a custom painter vtable built on
  new `fb` primitives (alpha blend, vertical gradient, anti-aliased rounded
  rects — all additive and clip-safe, so the retro themes are byte-identical):
  soft drop shadows, AA-rounded windows, flat widgets, an **accent underline
  beneath the active title bar** (the signature), a subtle aurora-gradient
  desktop, and a **frosted rounded taskbar** with an accent Start pill. The
  Control Panel's **Dark mode** toggle swaps Light↔Dark
  (`shots/aurora_light.png`, `shots/aurora_dark.png`).
- **Canvas + fullscreen** (toolkit features, benefit every port): `UI_CANVAS`
  is an app-drawn widget — the toolkit does the chrome/focus/drag, the app
  owns the pixels inside the rect (games / paint / tracker / browser), with a
  `draw` and an `event` callback. `unoui_fullscreen(ui, win)` makes a window's
  canvas fill the whole screen with no chrome and routes all input to it (Esc
  to return); the games and Runner3D use it. Both verified in QEMU
  (`shots/canvas_win.png`, `shots/canvas_full.png`).
- **Present-on-change**: the frame is redrawn/presented only when something
  changed (input or the ~2 Hz caret blink); idle frames touch no VRAM, so the
  desktop is steady and a drag only rewrites the moving window's rows. A
  fullscreen canvas redraws every frame (for animation).
- **Clip-to-window**: `fb` grew a clip window (`fb_set_clip`/`fb_reset_clip`);
  the renderer confines each window's widgets (and a canvas's own drawing) to
  its content rect, so an over-sized widget or a too-long label is cut at the
  frame instead of bleeding onto the desktop — overflow is impossible by
  construction, not by per-window hand-tuning. Chrome (frame/titlebar/shadow)
  draws unclipped since those painters are authored not to overflow.
- The only pc64-specific code is a ~40-line event adapter (UEFI input →
  `unoui_event`, gated so the legacy event queue is compiled out) + the window
  tree + action handlers; unoui owns everything else. Verified in QEMU: boots
  straight to the desktop, live keyboard theme switching (UnoDOS → Windows 3.1
  → …) re-skins the whole desktop.

The app migration is **complete**: every family app runs in the default shell.
The creative tools and Network are custom-render canvases (`pc64_uui_apps.c`
bridges them to a `mac_compat` Toolbox over `fb`); the **games are a native
rewrite** (`pc64_games.c`) — Dostris/Pacman/Outlast draw into whatever rect
they're handed, so the *same* canvas fills a window or the full screen with no
letterboxing, and Runner3D drives `uno3d` directly. **All audio is unified on
UnoSound** (`unosound_seq.c`, a single-voice PC-speaker sequencer): the games,
Music and Tracker all emit through `uno_seq_beep/play/stop`, so there is one
mixer path instead of per-app speaker banging. With the migration done,
`./build.sh legacy` (and its source) is now only kept as reference and could be
deleted. High-spec ports (PS2, Dreamcast, the ARM/PPC boards) can adopt the same
unoui shell; only the tiny targets (NES 2 KB, Game Boy 8 KB, C64) keep the
minimal legacy path — the toolkit won't fit.

## Web browser + file system (`pc64_browser.c`, `pc64_fs.c`, `js.c`)

A native unoui canvas app renders **HTML + Markdown + basic CSS** with a single
immediate-mode flow (word-wrap, heading scale, `<b>/<i>/<code>/<a>`, lists,
`<pre>`, rules, entity decode). It runs **JavaScript** too: a small tree-walking
interpreter (`js.c`) executes each `<script>` block, splicing its
`document.write` output into the page and collecting `console.log` into a
"console" panel at the foot of the document.

- **js.c** — arena-allocated (no `malloc`), freestanding (only `string.h` from
  `pc64_libc`; `Math.sqrt` is Newton's method, `Math.random` an xorshift). A
  lexer + recursive-descent parser to an AST + tree-walking evaluator: numbers/
  strings/booleans, `var`, arithmetic/logical/comparison/`+=`/`++`, `if`/`for`/
  `while`/ternary, functions + recursion, arrays (`.length`/`.push`),
  `console.log`, `document.write`, `Math.*`, `String`/`Number`/`parseInt`. ⚠
  Function calls **mark/release** the arena on return (copying out simple return
  values), so deep recursion is O(depth) not O(calls) — `fib(24)` (46 k calls)
  runs in the 512 KB arena. It has a `-DJS_TEST` host harness for native
  testing before it ever touches the EFI image.
- **File system** (`pc64_fs.c`) — a unified read-only namespace. Volume 0 is the
  RAM disk (`uno_ramfs_*`); volumes 1.. are the FAT / FAT32 / local disks the
  firmware mounted, reached through the UEFI **Simple File System** protocol
  (the same firmware-as-BIOS approach used for GOP and the pointer). The browser
  lists RAM files and ESP/FAT files side by side; Enter opens, Backspace
  returns, arrows/PgUp/PgDn scroll.
- **Network** (`pc64_http.c`) — the browser loads pages over the wire, not just
  from disk. An **address bar** (Up from the file list focuses it) takes a
  `http://host[:port]/path` URL; `pc64_http_get` brings the link up on demand
  (`pc64_net_up`: e1000 + DHCP, shared with the Network app), resolves the host
  by **DNS** (`net_dns_query`, UDP/53; DHCP now learns the resolver + gateway
  from options 6/3), TCP-connects, sends a HTTP/1.0 GET and renders the reply —
  including any `<script>`. HTTPS isn't supported yet (it needs CA trust);
  https:// URLs report that.

Verified in QEMU: the **Script.html** demo runs its `<script>` (a Fibonacci
table + a sum/`sqrt` line via `document.write`, `console.log` in the console
panel), and a page fetched over the network (`http://…` via SLIRP) renders with
its embedded JavaScript running too (`shots/browser_network.png`). HTTPS and a
Wi-Fi/USB-Ethernet NIC for the X1 are the remaining network work.

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
touchpad and TrackPoint don't fight) and **smoothed** — a 2-pole low-pass over
the raw deltas plus a small dead-zone and axis snap, so it tracks cleanly on
most pads with no per-device tuning. That alone may resolve the jerky-pad /
dead-button symptoms on machines where the firmware exposes the pad; the native
I2C-HID driver above is the eventual replacement for hardware that needs it.

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
./build.sh            # unoui shell (default) -> build/esp/EFI/BOOT/BOOTX64.EFI
./build.sh run        # unoui shell, boot in QEMU+OVMF (VNC :0)
./build.sh legacy     # the old core + 14 apps + net/TLS/3D
./build.sh legacy run # legacy, boot in QEMU
python3 harness.py boot   # scripted boot -> shots/*.png (QMP send-key/screendump)
python3 nettest.py        # net + TLS verification (needs the legacy build)
```

**Real hardware:** format a USB stick FAT32, copy `build/esp/*` onto it
(so it holds `EFI/BOOT/BOOTX64.EFI`), boot the target with Secure Boot
disabled. Validated on the X1 Carbon Gen 8; any 64-bit UEFI PC is in scope.

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
- **Unified file system** (`pc64_fs.c`): a single read-only namespace over the
  RAM disk (volume 0) **and** the firmware-mounted FAT / FAT32 / local disks
  (volumes 1..), the latter reached through the UEFI **Simple File System**
  protocol — so the browser and Files can already *read* real on-disk files
  (e.g. the ESP) with no native block driver. Write/persistence still waits on
  the NVMe/AHCI tail below.

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
├── build.sh            # mingw-w64 build: default = unoui, "legacy" = old core
├── harness.py          # QEMU+OVMF QMP harness (boot / screenshots)
├── nettest.py          # net + TLS QEMU verification (legacy build)
│  --- platform (both builds) ---
├── uefi.h  uefi_main.c # UEFI surface: GOP + input(+smoothing) + fill-scaling
│                       # + runtime services (clock, ResetSystem) + Simple FS
├── fb.c fb.h           # software framebuffer (clip window, blend/gradient/round)
├── pc64_libc.c pc64_math.c include/*.h  # freestanding libc + float math
├── pc64_io.c           # RAM-disk File Manager + PC-speaker Sound Manager
├── pc64_fs.c pc64_fs.h # unified file system (RAM vol 0 + FAT vols via UEFI)
├── pc64_pci.c pc64_pci.h  # PCI config-space scan
│  --- default shell: unoui ---
├── pc64_uui.c          # the unoui shell: desktop, taskbar, Start menu, tray,
│                       # app windows + UEFI event adapter (links ../unoui/*)
├── pc64_uui_apps.c     # mac_compat bridge for creative tools + Network
├── pc64_games.c        # native games (Dostris/Pacman/Outlast) + Runner3D
├── pc64_browser.c      # web browser: HTML/Markdown/CSS canvas app
├── js.c js.h           # tree-walking JavaScript interpreter (for <script>)
├── pc64_http.c .h      # HTTP/1.0 GET for the browser (address bar -> net)
├── pc64_icons.c        # procedural app icons
│                       # + ../unosound/unosound_seq.c (unified audio)
│                       # + ../unoui/themes/theme_aurora.c (modern default look)
│  --- legacy shell (./build.sh legacy) ---
├── unodos.c mac_compat.* app_loader.c pc64_modload.c uno_app.h
├── apps/               # 14 apps + uno_mod.h (settings/network/runner are pc64)
│  --- drivers ---
├── e1000.c e1000.h     # native Intel e1000 NIC driver
├── net.c net.h uno_nic.h  # Ethernet/ARP/IPv4/ICMP/UDP/TCP + DHCP + DNS
├── tls.c tls.h bearssl/ tls_test/  # BearSSL TLS client (pinned key, RDRAND)
├── i2c_hid.c i2c_hid.h # native I2C-HID trackpad (opt-in, -DUNO_I2C_TRACKPAD)
└── shots/              # harness-captured evidence
```
