# pc64 — bare-metal test checklist

Things that **can only be verified on real hardware** (the Lenovo X1 Carbon
Gen 8), because QEMU+OVMF can't exercise them. Everything here is already
QEMU-verified as far as QEMU allows (boots, renders, keyboard-drives); this list
is the on-metal follow-up to run **next time a stick write + boot is possible**.

Newest at the top. Check items off as they're confirmed on the X1.

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
