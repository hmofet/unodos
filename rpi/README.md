# UnoDOS / Raspberry Pi (ARM Cortex-A, AArch64)

The **ninth** fresh contract-driven port — and the **first AArch64 (64-bit)
world**. The GBA port already proved the 32-bit ARM path (ARM7TDMI / ARMv4T); the
Pi moves UnoDOS to the **64-bit A-profile** — `x0..x30` registers, no conditional
execution (so the GBA's `movne`/`addeq` predication becomes `csel` + branches),
`stp`/`ldp` frames, and 64-bit framebuffer pointers — on the same GNU-as (GAS)
dialect. MINIMAL profile (CONTRACT-ARCH §9): a 4-column icon launcher, one
full-screen app at a time, directional nav.

Unlike the GBA (fixed VRAM + memory-mapped I/O registers), the Pi has **no fixed
framebuffer**. At boot the kernel asks the **VideoCore firmware**, over the
**mailbox property channel** (`0x3F00B880`), for a 640×480 32bpp (XRGB8888) linear
surface and draws into the base address the GPU hands back. There are no hardware
tiles — the kernel plots an 8×8 font and 16×16 icons pixel by pixel, each pixel's
palette **index** looked up in a 16-entry 32-bit table in RAM, so the Theme app
recolours the whole screen by swapping the table. Per-frame pacing comes from the
**BCM system timer** (`0x3F003004`, the free-running 1 MHz counter).

## Status — M1 · M2 · M3 all shipped ✅

Verified headlessly on a **Unicorn Cortex-A core** (`rpi/harness.py`) running the
real `kernel8.img` — exactly as the GBA port is verified on a Unicorn ARM7TDMI and
the MacPlus port on a Unicorn 68K. A real Pi renders to an HDMI surface no headless
RDP grab can read, so the harness emulates the **two MMIO channels the kernel
actually touches** — it answers the mailbox's *allocate-framebuffer* + *get-pitch*
tags (handing back a fixed base) and advances the system-timer counter so
`wait_vblank` paces one frame per loop — then renders the framebuffer to a PNG. The
AUTOTEST images drive the pad through the same input path; nothing is faked
(`rpi/shots/*.png`):

- **M1 — launcher** (`shots/m1_boot.png`): boot → mailbox FB bring-up → the
  inverted "UnoDOS 3 – Raspberry Pi (AArch64)" title bar and a 4-column, 11-icon
  colour grid.
- **M2 — navigation** (`shots/m2_nav.png`): a system-timer-paced loop, a d-pad
  selection highlight (the selected label inverts), **A** launches the app
  full-screen / **B** returns.
- **M3 — apps** (`shots/m3_*.png`): SysInfo, live **Clock** (HH:MM:SS), Notepad,
  Files, **Theme** (cycles the 32-bit palette → recolours the desktop), **Music**
  (the PWM headphone-jack tone path + a progress bar), and **Dostris** — the
  falling-blocks game with a 16px-cell well. Tracker / OutLast / Pac-Man / Paint
  open framed placeholders.

> **Real input + audio.** The interactive (non-AUTOTEST) build reads a real
> **PL011 UART serial console** (GPIO14/15): `WASD` = d-pad, `Enter`/`Space` = A,
> `Backspace` = B — so the launcher is fully drivable from a serial terminal. The
> harness emulates the PL011 RX, so this path is verified end-to-end too
> (`shots/live_nav.png`, `shots/live_notepad.png` — navigation + app launch from
> injected serial input). USB-HID keyboard is a heavier future driver. The Music
> app drives the **PWM headphone jack in mark/space mode** (a real, audible
> square-wave tone on Pi 3); the harness reconstructs that output from the PWM
> register writes to a **WAV** and confirms the note sequence is "Ode to Joy"
> (`shots/music.wav`). Everything emulator-verifiable is verified.

## Hardware brought up (from scratch)

| Part | Detail |
|---|---|
| CPU | **ARM Cortex-A (AArch64 / ARMv8-A)** — secondary cores parked via `mpidr_el1`, core 0 runs UnoDOS |
| Video | firmware-allocated **mailbox framebuffer**, flat 640×480 **32bpp XRGB8888** linear surface |
| FB setup | mailbox property channel (`0x3F00B880/98/A0`, ch 8): set phys/virt size + depth 32 + RGB order, allocate buffer, get pitch; GPU bus addr → ARM physical via `& 0x3FFFFFFF` |
| Colour | 16-entry 32-bit palette in RAM; pixels store the looked-up XRGB word |
| Timing | BCM system timer low word `0x3F003004` (1 MHz); `wait_vblank` = busy-wait one `FRAME_US` (~60 Hz) |
| Audio | **PWM0/1 mark/space** on the headphone jack: square wave at `PWM_CLK / RNG1`, `DAT1 = RNG1/2` (clock `0x3F1010A0`, PWM `0x3F20C000`). Harness reconstructs it to a WAV (note sequence = "Ode to Joy") |
| Input | **PL011 UART** serial console (`0x3F201000`); poll `FR.RXFE`, read `DR`; WASD+Enter+Backspace → pad (USB-HID = future) |
| RAM | fixed layout: stack `→0x200000`, vars `0x300000`, mailbox buffer `0x310000`, fb base/pitch `0x320000` |
| Boot | `kernel8.img` flat at `0x80000` (the `arm_64bit=1` entry); `_start` parks cores, sets SP, brings up the FB |

## Build & run

```sh
sh rpi/build.sh                                        # -> rpi/build/kernel8.img
sh rpi/build.sh nav|app|clock|theme|music|dostris      # AUTOTEST builds
python rpi/harness.py rpi/build/kernel8.img rpi/shots/m1_boot.png
```

Toolchain: `aarch64-linux-gnu-{as,ld,objcopy}` (binutils 2.42, via WSL) +
`python` with `unicorn` 2.x. On a real Pi 3/4: drop `kernel8.img` on a FAT32 card
with `config.txt` containing `arm_64bit=1` and the firmware (`bootcode.bin`,
`start.elf`).

## Contract

Screen geometry comes from **unogen** (`[world.rpi]` → `unodef/gen/rpi/sys_gen.inc`,
the `aarch64` GAS dialect). The 32-bit register-width step and the mailbox
framebuffer are the port's own; the cell geometry is the genuine Contract overlap.
