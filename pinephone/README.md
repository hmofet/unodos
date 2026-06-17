# UnoDOS / PinePhone (Allwinner A64, AArch64)

The **tenth** fresh contract-driven port — and the **second AArch64 world**. It
**reuses the Raspberry Pi AArch64 core** (the same GNU-as/GAS dialect, the same
software-framebuffer primitives, the same Dostris and app logic), retargeted to the
**Allwinner A64** SoC (4× Cortex-A53) and a **portrait** phone panel (480×640).
MINIMAL profile (CONTRACT-ARCH §9): a 4-column icon launcher, one full-screen app
at a time, directional nav.

Two things differ from the Pi, both honest to the silicon:

- **Display.** The A64 has no GPU mailbox. Exactly as the Pi relies on the VideoCore
  firmware to bring up HDMI, this port assumes the boot chain (boot ROM → SPL →
  U-Boot) has already initialised DRAM and the panel clock path (TCON0 + MIPI-DSI +
  the panel), then programs the **Display Engine 2.0 (DE2)** mixer **UI layer** to
  scan out our XRGB8888 framebuffer in DRAM (`PINE_FB = 0x40400000`).
- **Timing.** Per-frame pacing reads the **ARM architectural generic timer**
  (`cntpct_el0`) directly via `mrs` — no MMIO at all, real-hardware-correct.

## Status — M1 · M2 · M3 all shipped ✅

Verified headlessly on a **Unicorn Cortex-A core** (`pinephone/harness.py`) running
the real payload. Because the kernel uses a fixed DRAM framebuffer and the generic
timer (which Unicorn advances on its own), the harness is even simpler than the Pi's:
it maps DRAM + a RAM sink over the DE2 register block, runs the budget so the
AUTOTEST pad plays out, and renders the DE2 framebuffer to a PNG. Nothing is faked
(`pinephone/shots/*.png`):

- **M1 — launcher** (`shots/m1_boot.png`): boot → DE2 UI-layer scanout → the
  inverted "UnoDOS 3 – PinePhone (Allwinner A64)" title bar and a 4-column, 11-icon
  colour grid, in portrait.
- **M2 — navigation** (`shots/m2_nav.png`): a generic-timer-paced loop, a d-pad
  selection highlight (the selected label inverts), **A** launches / **B** returns.
- **M3 — apps** (`shots/m3_*.png`): SysInfo, live **Clock** (HH:MM:SS), Notepad,
  Files, **Theme** (cycles the 32-bit palette), **Music** (UI + note timeline), and
  **Dostris** — the falling-blocks game in a centred portrait well.

> **Real input + audio.** The interactive (non-AUTOTEST) build reads a real
> **A64 UART0 serial console** (16550, on the headphone jack): `WASD` = d-pad,
> `Enter`/`Space` = A, `Backspace` = B. The harness emulates the 16550 RX, so this
> path is verified end-to-end (`shots/live_nav.png`, `shots/live_notepad.png` —
> navigation + app launch from injected serial input). The capacitive touch panel is
> a heavier future driver. The Music app **synthesises square-wave PCM in software**
> (a phase accumulator) and feeds the samples to the A64 **I2S0 TX FIFO**; the harness
> captures those writes as the actual audio stream and confirms it plays "Ode to Joy"
> (`shots/music.wav`). The I2S clock + AC200/AC100 codec bring-up (to reach the real
> speaker) is best-effort / by-ear; the PCM synthesis is verified.

## Hardware brought up

| Part | Detail |
|---|---|
| CPU | **Allwinner A64** — 4× ARM Cortex-A53 (**AArch64**); secondary cores parked via `mpidr_el1` |
| Video | **Display Engine 2.0** mixer 0 UI layer (`0x01100000`): global enable + size, blender pipe, UI layer attribute (XRGB8888), pitch, top address = `PINE_FB`; assumes SPL brought up TCON0/MIPI-DSI |
| Surface | 480×640 **portrait**, 32bpp XRGB8888 linear framebuffer in DRAM at `0x40400000` |
| Colour | 16-entry 32-bit palette in RAM; pixels store the looked-up XRGB word |
| Timing | ARM generic timer `cntpct_el0` (24 MHz) via `mrs`; `wait_vblank` busy-waits one `FRAME_TICKS` (~60 Hz) |
| Audio | **software square-wave PCM** -> A64 **I2S0 TX FIFO** (`0x01C22020`); harness captures the samples to a WAV ("Ode to Joy"). Codec/I2S clock delivery to the speaker = best-effort/by-ear |
| Input | **A64 UART0** serial console (`0x01C28000`, 16550); poll `LSR.DR`, read `RBR`; WASD+Enter+Backspace → pad (touch panel = future) |
| RAM | DRAM at `0x40000000`: payload `0x40080000`, stack `→0x40200000`, vars `0x40300000`, fb info `0x40320000`, framebuffer `0x40400000` |
| Boot | flat AArch64 payload at `0x40080000` (the U-Boot `kernel_addr_r`); `_start` parks cores, sets SP, programs DE2 |

## Build & run

```sh
sh pinephone/build.sh                                   # -> pinephone/build/unodos.bin
sh pinephone/build.sh nav|app|clock|theme|music|dostris  # AUTOTEST builds
python pinephone/harness.py pinephone/build/unodos.bin pinephone/shots/m1_boot.png
```

Toolchain: `aarch64-linux-gnu-{as,ld,objcopy}` (binutils 2.42, via WSL) + `python`
with `unicorn` 2.x. On real hardware: load `unodos.bin` at `0x40080000` from
U-Boot (`go 0x40080000`) after the panel is up.

## Self-booting microSD (`mksd.sh`)

`unodos.bin` is a flat payload — it assumes DRAM and the DSI panel are already up and
just wants to run at `0x40080000`. To make a card that boots straight into UnoDOS on a
bare PinePhone, we add the rest of the boot chain:

```
BROM → SPL (8 KB offset) → U-Boot (brings up DRAM + DSI panel)
     → distro_bootcmd scans the card → runs boot.scr
     → fatload unodos.bin @0x40080000 → go
```

`mksd.sh` (run **on a Linux box** — it needs `parted`/`mkfs.vfat`/`losetup`/`mkimage`,
and `sudo` + a card reader to write; this is not a WSL script like `build.sh`) builds
mainline **U-Boot + ARM Trusted Firmware** for the A64 and assembles the image:

```sh
sh pinephone/build.sh              # first make build/unodos.bin
sh pinephone/mksd.sh fw            # build U-Boot + ATF  -> build/u-boot-sunxi-with-spl.bin
sh pinephone/mksd.sh image         # assemble bootable    -> build/pinephone-unodos.img
sh pinephone/mksd.sh write /dev/sdX   # dd onto a card (guards: removable/USB only)
# or in one go:  sh pinephone/mksd.sh all /dev/sdX
```

What it does, and why each piece:

- **Toolchain**: auto-fetches a userspace `aarch64` *nolibc* gcc from kernel.org
  crosstool (no `apt`/`sudo`); set `CROSS=` to use your own. `swig`+`pyelftools`
  (U-Boot host tools) go in a venv to dodge PEP-668.
- **ATF**: `PLAT=sun50i_a64 bl31` → the secure monitor U-Boot loads.
- **U-Boot**: `pinephone_defconfig` (which resolves `CONFIG_VIDEO/PANEL/BACKLIGHT=y` —
  it lights the XBD599 DSI panel, the whole reason we use it). Two tweaks: disable the
  `mkeficapsule` host tool (wants gnutls headers, unneeded) and **enable `CMD_CACHE`**
  (needed by `boot.scr`, below). The `u-boot-sunxi-with-spl.bin` SPL+FIT is written at
  the 8 KB offset; the `eGON.BT0` SPL magic lands at byte 8196 and it ends well before
  the 1 MiB partition start.
- **Card layout**: MBR, one FAT32 partition (label `UNODOS`, 1 MiB→end) holding
  `unodos.bin` + `boot.scr`; SPL/U-Boot occupy the 8 KB–1 MiB gap.
- **`boot.cmd`** (compiled to `boot.scr` by `mkimage`): `fatload`s the payload and
  jumps to it. This U-Boot uses `distro_bootcmd`, which scans each device for
  `boot.scr` at the FAT root and runs it with `${devtype}/${devnum}/${distro_bootpart}`
  pre-set.

### The cache-coherency caveat (the one real-hardware unknown)

U-Boot's `go` does **not** flush or disable caches (unlike `booti`). The payload then
runs with U-Boot's MMU + caches still on, never re-enables or flushes them, and the
DE2 engine scans the framebuffer out of DRAM via DMA — so cached FB writes could read
back as stale DRAM (garbage on the panel). `boot.cmd` therefore runs
`dcache flush; dcache off; icache off` right before `go`, so the payload runs
cache-coherent. This is the one thing the harness can't exercise (it has no caches and
no U-Boot); if a first boot shows a black/garbled screen, this handoff is the prime
suspect. A UART0 console on the headphone jack (115200 8N1) shows the U-Boot log and
pinpoints where it stops.

## Contract

Screen geometry comes from **unogen** (`[world.pinephone]` →
`unodef/gen/pinephone/sys_gen.inc`, the `aarch64` GAS dialect). The Allwinner DE2
display path and the portrait orientation are the port's own; the cell geometry is
the genuine Contract overlap. The AArch64 core is shared with [the Raspberry Pi
port](../rpi/README.md).
