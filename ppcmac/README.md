# UnoDOS / PowerPC Macintosh (32-bit PowerPC, Open Firmware)

The **eleventh** fresh contract-driven port — and the **first PowerPC (big-endian
RISC) world**. A brand-new ISA: it needed a new GAS-PPC unogen dialect (`#` line
comments) and a from-scratch harness. It is the **first port to boot over Open
Firmware** (no Mac OS): OF loads this client program into RAM and enters `_start`
with the IEEE-1275 **client-interface entry in r5**. The kernel makes a few OF
client calls — `finddevice "screen"`, then `getprop` for `address` and
`linebytes` — to obtain the linear framebuffer, then draws into it directly.
MINIMAL profile (CONTRACT-ARCH §9): a 4-column icon launcher, one full-screen app
at a time, directional nav.

There are no hardware tiles — the kernel plots an 8×8 font and 16×16 icons pixel by
pixel into a 640×480 32bpp (XRGB8888) framebuffer, each pixel's palette **index**
looked up in a 16-entry table in RAM (so the Theme app recolours by swapping it).
PowerPC is **big-endian**, so a `stw` of `0xFFRRGGBB` lands as bytes `FF RR GG BB`
— the framebuffer stores XRGB byte-for-byte.

## Status — M1 · M2 · M3 all shipped ✅

Verified headlessly on a **Unicorn PPC32 big-endian core** (`ppcmac/harness.py`)
running the real payload — exactly as the GBA/Pi run on Unicorn ARM and the MacPlus
port on Unicorn 68K. The distinctive piece is the **Open Firmware emulation**: r5
points at a one-instruction `blr` trampoline; a code hook there reads the CI argument
array (r3), services `finddevice` / `getprop` (handing back a fixed framebuffer base
+ pitch), and returns — so `fb_init` gets a real surface. Then the harness runs an
instruction budget (`wait_vblank` is a spin loop, so frames advance and the AUTOTEST
pad plays out) and renders the framebuffer to a PNG. Nothing is faked
(`ppcmac/shots/*.png`):

- **M1 — launcher** (`shots/m1_boot.png`): boot → OF client calls → the inverted
  "UnoDOS 3 – PowerPC Mac (Open Firmware)" title bar and a 4-column, 11-icon grid.
- **M2 — navigation** (`shots/m2_nav.png`): a frame-paced loop, a d-pad selection
  highlight (the selected label inverts), **A** launches / **B** returns.
- **M3 — apps** (`shots/m3_*.png`): SysInfo, live **Clock** (HH:MM:SS), Notepad,
  Files, **Theme** (cycles the 32-bit palette), **Music** (UI + note timeline), and
  **Dostris** — the falling-blocks game with a 16px-cell well.

> **Real input + audio.** The interactive (non-AUTOTEST) build reads the keyboard
> through **Open Firmware** — `read(stdin, …)` on the `/chosen` `stdin` instance
> (reusing the OF client interface): `WASD` = d-pad, `Enter`/`Space` = A,
> `Backspace` = B. The harness emulates the OF `read` service, so this path is
> verified end-to-end (`shots/live_nav.png`, `shots/live_notepad.png` — navigation +
> app launch from injected console input). A native ADB-over-CUDA driver is a future
> refinement. The Music app **synthesises square-wave PCM in software** (a phase
> accumulator) and feeds the samples to the codec data port; the harness captures
> those writes as the actual audio stream and confirms it plays "Ode to Joy"
> (`shots/music.wav`). The codec + DBDMA bring-up (to reach the real output) is
> best-effort / by-ear; the PCM synthesis is verified.

## Hardware / firmware brought up

| Part | Detail |
|---|---|
| CPU | **32-bit PowerPC** (G3/G4-class), big-endian; SysV/EABI register usage (r1 SP, LR link, r14-r31 callee-saved) |
| Boot | **Open Firmware** client program; entered at `_start` with the IEEE-1275 client-interface entry in **r5** |
| Display | OF `finddevice "screen"` → `getprop "address"` (framebuffer base) + `getprop "linebytes"` (pitch); 640×480 32bpp XRGB linear surface |
| Colour | 16-entry 32-bit palette in RAM; pixels store the looked-up XRGB word (big-endian `FF RR GG BB`) |
| Timing | `wait_vblank` is a calibrated `bdnz` spin loop (~one frame); the PowerPC Time Base would pace this on real hardware |
| Audio | **software square-wave PCM** -> the codec data port (`0x80800000`); harness captures the samples to a WAV ("Ode to Joy"). Codec/DBDMA delivery = best-effort/by-ear |
| Input | **Open Firmware `read`** on `/chosen` stdin (the OF console keyboard); WASD+Enter+Backspace → pad (native ADB-over-CUDA = future) |
| RAM | payload `0x00100000`, stack `→0x00300000`, vars `0x00400000`, OF-entry + fb info `0x00420000`, CI buffer `0x00430000` |

## Build & run

```sh
sh ppcmac/build.sh                                      # -> ppcmac/build/unodos.bin
sh ppcmac/build.sh nav|app|clock|theme|music|dostris     # AUTOTEST builds
python ppcmac/harness.py ppcmac/build/unodos.bin ppcmac/shots/m1_boot.png
```

Toolchain: `powerpc-linux-gnu-{as,ld,objcopy}` (binutils 2.42, via WSL) + `python`
with `unicorn` 2.x. On real hardware: from the Open Firmware prompt, `load` the
payload and `go` (or boot it as an OF client program).

## Contract

Screen geometry comes from **unogen** (`[world.ppcmac]` →
`unodef/gen/ppcmac/sys_gen.inc`, the new `ppc` GAS dialect with `#` comments). The
PowerPC big-endian ISA and the Open Firmware client-interface boot are the port's
own; the cell geometry is the genuine Contract overlap.

> **A note on PowerPC's `r0`.** In load/store addressing, `r0` in the base position
> means *literal 0*, not the register — so the `LWZA`/`STWA` address-load macros
> must never target `r0`. (The one place that tripped this — loading the OF entry —
> was the only bug found in bring-up.)
