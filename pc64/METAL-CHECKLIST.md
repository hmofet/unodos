# pc64 — bare-metal test checklist

Things that **can only be verified on real hardware** (the Lenovo X1 Carbon
Gen 8), because QEMU+OVMF can't exercise them. Everything here is already
QEMU-verified as far as QEMU allows (boots, renders, keyboard-drives); this list
is the on-metal follow-up to run **next time a stick write + boot is possible**.

Newest at the top. Check items off as they're confirmed on the X1.

## Newest — M3 firmware detach (ExitBootServices) on metal

QEMU-verified end-to-end (auto-detach, PS/2 input, native-AHCI module loads,
CMOS clock). Metal is where the gate's conservatism actually matters:

- [ ] **A machine WITH an i8042 + AHCI system disk** (older desktop/laptop,
      UnoDOS *installed* to the internal disk): boot → System app should say
      `DETACHED (native): ahci ...`; keyboard + (PS/2) mouse still work, the
      clock ticks (CMOS), apps open (native AHCI `.UNO` loads), Restart works
      (CF9). Music/games audio still fine (PIT speaker was already native).
- [ ] **USB-stick boot on the same machine**: must STAY attached (the boot
      volume is USB) — System says `Native FS: fw-sect ...`, Install works.
- [ ] **The Surface / X1 (no i8042 or NVMe-only)**: must stay attached and
      behave exactly as before (the detach gate declines quietly; check
      `detach:` lines with a `-DUNO_DBGCON` build if curious).
- [ ] If a machine misbehaves detached, `-DUNO_NO_DETACH` is the escape hatch
      (and file what broke — likely NX page tables or an AHCI quirk).

## Newest — xHCI USB stack + AX88179 USB Ethernet (networking for the X1)

The USB host-controller driver + transfer API + the AX88179 USB Gigabit
Ethernet driver. **Gated behind `-DUNO_XHCI` (OFF by default)** — build a test
image with it, since taking over the USB controller can conflict with the
firmware. In QEMU the host stack + device **enumeration** are verified
(controller inits; a USB-net gadget and a tablet both complete Address Device +
GET_DESCRIPTOR over the control-transfer path). QEMU has no ASIX chip, so
**everything AX88179-specific is metal-first**.

- [ ] **Build with USB on:** `UNO_EXTRA="-DUNO_XHCI" ./build.sh`, write the stick.
- [ ] **Plug in the ASIX AX88179 USB Ethernet adapter**, boot, open the **System
      app**. Expect: `USB xHCI: up, N port, M dev  0xNNNN:0xNNNN`, plus an
      `ASIX 0x0b95:0xNNNN  bound  link up/down` line once the driver binds. VID
      must be **0x0b95** (ASIX). If it reads `0bda:...` it's a Realtek batch
      (different driver); `0525:...` is a phone/gadget.
- [ ] **Network over USB:** open the browser and load an `http://` page (or the
      Network app). `pc64_net_up()` tries e1000 then the AX88179; a successful
      DHCP lease + page load is the real end-to-end win. The RX aggregation
      trailer + 8-byte TX header are unverified against silicon — if the link is
      up but no packets flow, that framing is the first suspect.
- [ ] **Keyboard/trackpad should survive** — the X1's built-in keyboard + pad are
      PS/2 / I2C, not USB, so the xHCI takeover shouldn't kill them. If input
      dies, that's the firmware-USB conflict (note it and we add the USBLEGSUP
      BIOS→OS handoff).
- [ ] **If `init failed at stage N` or `0 dev`:** report the second System line
      (`sl=… ad=… de=… sts=0x…`): `sl` = Enable-Slot result (`-N` = a completion
      code), `ad` = Address-Device cc (1 = ok), `de` = device-descriptor
      byte (1 = ok, `-1` = the control transfer failed), `sts` = USBSTS
      (`0x1000` = HC error; the bring-up retry usually recovers it — QEMU needed
      it). For a full per-port trace (`slot`/`addr_cc`/`desc_cc`/`residual`/
      VID:PID) rebuild with `-DUNO_XHCI -DUNO_DBGCON` — **but 0x402 debugcon is
      SMM-trapped on this X1 and hung it once**, so only use DBGCON for QEMU.
- [ ] Real Intel xHCI may need the **USBLEGSUP** (xHCI extended-cap BIOS→OS
      ownership) handoff, which isn't implemented yet — if enumeration never
      works on metal but does in QEMU, that's the likely reason.

## Newest batch — HTTPS, trackpad probe, GUI controls, stub fixes

- [ ] **HTTPS** — the browser now does CA-validated TLS 1.2 (`https://`). Fully
      verified in QEMU (handshake + chain + RTC validity). On the X1 it needs a
      NIC (none wired), so it's inert there like the rest of networking. **Note:
      cert dates want the RTC in UTC** — if the X1's RTC is local time and far
      from UTC, real certs could read as not-yet-valid; set the clock to UTC via
      Control Panel if HTTPS reports `BearSSL err 54`.

- [ ] **Trackpad (I2C-HID)** — last X1 readout: "2 DW ctrl / 2 bars, HID device:
      not found on ctrl". Added an **Intel LPSS reset-release** (controllers were
      likely found-but-held-in-reset, so every transfer timed out), a broader
      address/descriptor probe, and a richer System readout. **Re-check the
      System app**: it now shows either `no HID (no ACK, abrt 0xNNNN)` (address
      NAK → wrong slave address) or `no HID (bus ok, abrt 0xNNNN)` (a device
      answered but the descriptor didn't validate), or `UP addr 0xNN parsed`.
      Report the new line — abrt `0x0000` with "no ACK" means the bus still isn't
      driving (deeper LPSS clock/power issue); `0x0080` means NAK (address).
- [ ] **GUI controls for every keypress action** — all mouse-reachable now:
      games' New Game/Retry/Drive/Restart buttons, Tracker's Play/Clear/Demo/
      Save/Load toolbar, Music's Play/Prev/Next, Paint's Clear/Save/Load,
      Network's Re-run. Confirm the clicks work (QEMU has no pointer).
- [ ] **Paint/Tracker file pickers** now list real files (fat12_list wired to
      uno_fs) — confirm save/open show the file list.

## Newest batch — TTF fonts, Paint fix, resizable windows, calendar picker

- [ ] **Paint draws under the cursor** — the drag bug (all pixels landed at the
      canvas top-left) is fixed: Paint's blocking `GetMouse` spin now samples the
      live firmware pointer. Confirm pencil/brush/shapes follow the cursor and
      the stroke shows live. **[HW — QEMU has no pointer]**
- [ ] **TTF fonts** — Control Panel → Font picker (System / Sans / Mono /
      Ubuntu); the whole UI re-skins to proportional subpixel-AA text. Editor →
      "Doc font" changes just the document face. Confirm the subpixel AA looks
      right on the real eDP (subpixel order assumes RGB stripes). Default stays
      the bitmap monospace until you're happy with TTF. **[QEMU✓ render]**
- [ ] **Resizable windows** — drag a window's bottom-right grip; content reflows.
      **[HW — needs pointer]**
- [ ] **Calendar date-picker** — Control Panel → "Pick date..." opens the month
      grid; the chevrons page months and clicking a day sets the clock.
      **[HW — needs pointer]** (renders correctly in QEMU)

## Newest batch — network loading + the Aurora visual overhaul

These shipped after the JavaScript build; all QEMU-verified. Re-flash the stick
to pick them up.

### Aurora theme (the new default look) — verify it reads well on the real eDP
- [ ] **Aurora Light** is the boot default — confirm the rounded windows, soft
      shadows, the accent underline under the active title, the frosted taskbar
      and the aurora-gradient desktop all render cleanly at the panel's native
      resolution (the AA rounding + blends are resolution-independent, but worth
      a look on real hardware / real colour).
- [ ] **Dark mode** — Control Panel → "Dark mode" checkbox should swap to Aurora
      Dark instantly (and back). Both are also in the theme dropdown.
- [ ] The **8 retro themes** should still look exactly as before (the Aurora
      primitives are additive; QEMU-confirmed against Windows 3.1).

### Browser network loading — expected to be inert on the X1 (no wired NIC)
- [ ] The browser **address bar** (Up from the file list) + a `http://…` URL:
      on the X1 this will report **"No e1000 NIC found"** because the machine
      has Intel Wi-Fi, not e1000 — expected until a Wi-Fi / USB-Ethernet driver
      lands. The whole path (DNS + TCP + HTTP + render) is QEMU-verified via
      SLIRP instead. Nothing to fix here; just confirm the error is graceful.

## Pending since the last metal test (2026-07-16, commit b2c8d04 build)

Everything below shipped after the last on-metal boot. The current stick already
carries the JavaScript build (commit dfc7d28); re-flash before testing to pick up
anything newer.

### Pointer / trackpad (QEMU has no OVMF pointer at all)
- [ ] **Trackpad smoothing** — the firmware Absolute-Pointer path now has a
      2-pole low-pass + dead-zone + axis-snap. Confirm the cursor tracks cleanly
      and isn't over-damped/laggy on the X1 pad.
- [ ] **Title-bar close box** — click the box on a window title bar; the window
      should close. (Wired via `UI_ACT_CLOSE`; never clicked on metal.)
- [ ] **Outline drag** — drag a window by its title bar; only a rubber-band
      outline should move, committing on release (no flicker).
- [ ] **Taskbar chips** — click a chip to focus/raise its window.
- [ ] **Desktop icons** — click an icon to launch its app.
- [ ] **Start menu** — click Start, click an entry; hover the up/down chevrons
      to scroll; click Restart / Shut Down.
- [ ] **Paint / creative tools** — mouse drawing + the on-screen tool/colour
      palettes (unclickable to verify in QEMU).
- [ ] **I2C-HID native trackpad** — still `-DUNO_I2C_TRACKPAD` (on by default but
      inert without the controller). Run the **System app** readout ("Trackpad
      I2C: N DW ctrl / M bars", "HID device: ...") and report it; use
      `uno_i2c_hid_dump()` to capture raw report bytes to tune the parser.

### Clock / power (needs the real firmware)
- [ ] **System-tray clock** — should show real wall-clock time (UEFI `GetTime`).
- [ ] **Settings → Set date & time** — the Y/Mo/D/H/Mi spinners + Apply Clock
      should write the RTC via UEFI `SetTime`; confirm the tray clock updates.
- [ ] **Start → Restart / Shut Down** — UEFI `ResetSystem`; confirm the machine
      actually restarts / powers off.
- [ ] **Startup chime** — a C-E-G-C arpeggio (PC speaker) should play once after
      the splash bar completes.

### Audio (PC speaker — QEMU doesn't sound it)
- [ ] **Game + app audio** — Dostris (Korobeiniki), OutLast (Sunset Drive),
      SFX (line-clear, waka, crash), Music/Tracker playback — all now on the one
      UnoSound voice; confirm tones actually sound.

### Networking (works in QEMU via SLIRP, but the X1 has no e1000)
- [ ] **Browser internet loading** — the X1 has **Intel Wi-Fi, no e1000**, so the
      browser's network fetch and the Network app will report "No e1000 NIC
      found" on metal. This is expected until a Wi-Fi / USB-Ethernet driver
      lands. Verified end-to-end in QEMU instead (SLIRP DNS + NAT).

## Confirmed working on metal (2026-07-16 and earlier)
- [x] Boots to desktop from USB ESP; keyboard + TrackPoint drive it.
- [x] Live theme + resolution switching.
- [x] Runner3D, Settings, Notepad run on metal.
- [x] eDP full-res fill-scaling (no 640×480 black-panel trap).
