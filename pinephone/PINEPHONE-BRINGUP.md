# PinePhone bare-metal bring-up — investigation notes

A standalone reference for bringing up the **original PinePhone** (Allwinner A64) from
bare metal, written while debugging why UnoDOS booted but showed nothing. Findings are
tagged **[HW]** (verified on real hardware this session), **[CFG]** (read from the actual
U-Boot config/source), or **[REF]** (external reference, not yet personally verified).
Intended to be reusable for a separate from-scratch PinePhone project.

Device under test: PinePhone (A64), running postmarketOS on eMMC. Host flashing box:
"devbuntu" (Linux). No serial cable yet — all debugging done with no serial console.

---

## 1. SoC & display hardware

- **SoC:** Allwinner **A64** — 4× ARM Cortex-A53 (AArch64). DRAM base `0x40000000`.
- **Panel:** Xingbangda **XBD599**, 5.99", **720×1440**, MIPI-DSI, 4 lanes. Controller =
  **Sitronix ST7703**. [REF]
- **Display path:** Framebuffer in DRAM → **DE2** (Display Engine 2.0, "mixer") does
  DMA/overlay/scale/blend → **TCON0** (timing controller) → **MIPI-DSI host** → **D-PHY**
  → ST7703 panel. [REF]
- **PMIC:** X-Powers **AXP803**, reached over the A64 **RSB** (Reduced Serial Bus, an
  enhanced 2-wire) — supplies the panel/DSI rails and gates backlight power. [REF]
- **Status LED:** green LED on **PD18** (GPIO bank D pin 18 = sunxi GPIO #114). [HW]

---

## 2. Boot chain & BROM boot order

A64 BROM tries, in order: **SD (MMC0) → eMMC (MMC2) → SPI → FEL (USB-OTG)**. The original
PinePhone gives **SD priority over eMMC** (unlike the PinePhone *Pro*, where eMMC wins). So
a validly-flashed SD always boots first; you cannot lock yourself out. [REF]

SPL format = sunxi **eGON** image: at byte 0 a branch insn, `eGON.BT0` magic at +0x04,
`check_sum` (u32) at +0x0C, `length` (u32) at +0x10, then `SPL\x02`. The BROM loads
`length` bytes from **sector 16 (byte 8192)** of the SD and verifies the checksum
(sum of all u32 words with the checksum field replaced by the stamp `0x5F0A6C39`). A bad
checksum → BROM silently skips to the next medium. [CFG/HW]

`u-boot-sunxi-with-spl.bin` is written at the 8 KB offset (`dd seek=8 bs=1k`); the SPL +
U-Boot FIT occupy the 8 KB–1 MiB gap; the FAT/boot partition starts at 1 MiB. [HW]

**FEL mode** = USB-OTG recovery; entered when no valid boot medium is found, or
button-free by flashing `fel-sdboot.sunxi` (an 8 KB eGON SPL that drops straight to FEL)
into the SD's SPL slot. In FEL the screen stays dark on purpose; `sunxi-fel version` over
USB confirms the SoC is alive, and `sunxi-fel uboot <img>` runs a U-Boot from RAM. [REF]

---

## 3. THE KEY FINDING — nothing in the mainline boot chain lights the DSI panel

**Mainline U-Boot (checked: 2026.07) has no sunxi/sun6i MIPI-DSI driver at all.** [CFG]
- `grep` of the whole tree: only `stm32`, `tegra`, `exynos`, `dw_mipi_dsi` DSI drivers
  exist — **no sun6i / sun50i DSI host**. `drivers/video/sunxi/` has only `lcdc.c` +
  `simplefb_common.c`.
- `pinephone_defconfig` doesn't configure video at all; `# CONFIG_VIDEO_MIPI_DSI is not set`.
  `CONFIG_VIDEO=y`/`PANEL=y`/`BACKLIGHT=y`/`SUNXI_DE2=y` come from defaults but are inert
  for the panel without a DSI host.

Consequence: with mainline U-Boot the panel is **never initialised** — no U-Boot logo, no
`vidconsole` (console falls back to **serial only**), and any payload that assumes "the
boot chain already lit the panel" shows nothing. Only the **Linux kernel** (postmarketOS),
with `sun6i-mipi-dsi` + `panel-sitronix-st7703`, lights the screen.

This breaks the assumption the UnoDOS PinePhone port inherited from the **Raspberry Pi**
port. On the Pi, VideoCore firmware brings up HDMI before the kernel, so the payload only
has to program the scanout overlay. **The PinePhone has no equivalent** — there is no
firmware/bootloader stage that lights the DSI panel unless you provide one.

**So: whoever wants pixels must do the full DSI panel bring-up. The bootloader does not
have to — an OS/payload can do it itself (the Linux kernel does, from cold).**

---

## 4. Debugging with no serial cable

Three channels, in order of usefulness found here:

1. **The LED (PD18 / GPIO114).** [HW] The SPL lights this green LED
   (`CONFIG_SPL_SUNXI_LED_STATUS_GPIO=114`), so **green LED on = our SPL ran**. A payload
   can drive it via the A64 PIO controller and blink it — a 1-bit progress channel that
   needs neither serial nor display. Registers:
   - `PD_CFG2 = 0x01C20874` — PD pins 16–23 mode; **PD18 = bits [11:8]**, `0b001` = output.
   - `PD_DAT  = 0x01C2087C` — **PD18 = bit 18** (1<<18 = 0x40000); set/clear = on/off.
   - (PIO base `0x01C20800`; port block = base + port*0x24, PD = port 3 = +0x6C;
     CFG0/1/2/3 at +0x00/04/08/0C, DAT at +0x10.)
   Verified: an infinite blink loop made the green LED blink on hardware → payload runs
   end-to-end. Use **blink-count codes** to mark bring-up stages.
2. **FEL over USB.** [REF] `sunxi-fel version` (SoC alive?), `sunxi-fel uboot <img>` (run a
   U-Boot from RAM, bypassing SD/eMMC — does *that* U-Boot light the panel?).
3. **U-Boot `vidconsole`** — only useful if the panel is actually up (it isn't, with
   mainline U-Boot; see §3). The sunxi default env is `stdout=serial,vidconsole`, so once a
   DSI-capable U-Boot lights the panel you'd get console text + the `VIDEO_LOGO` on screen.

### Bisecting "screen dark" without serial — the decisive experiment [HW]
Flash two SD cards that differ **only** in `boot.scr`:
- a normal one (`if fatload ...; then ... go; fi`, which **falls through** to the next
  boot target on failure), and
- a "diagnostic" one whose script **halts in an infinite loop** instead of falling through.

If the normal card boots eMMC/pmOS but the diagnostic card does **not** (screen stays
blank, no pmOS), then your SD SPL+U-Boot+`boot.scr` are all running (if the BROM were
skipping the SD, both cards would boot eMMC identically). This is how we proved the boot
chain works and isolated the fault to "panel never initialised," with no serial.

---

## 5. The display bring-up stack (what an OS/payload must do)

Order roughly follows the Linux drivers and lupyuen's NuttX bare-metal port [REF]:

1. **Clocks (CCU @ 0x01C20000):** ungate + de-reset DE, TCON0, and the MIPI-DSI block;
   configure **PLL-MIPI** (the DSI/D-PHY pixel clock). Mixer/TCON clock dividers.
2. **PMIC rails (AXP803 over RSB @ 0x01F03400):** enable the panel/DSI regulators
   (e.g. DLDO1 for panel VCC, the DSI I/O rails) and the backlight power. Needs an RSB
   driver (init RSB, set AXP803 runtime address, read/write PMIC registers).
3. **Panel reset GPIO:** drive the ST7703 reset line (a PD/PH GPIO) low→high with the
   required delays.
4. **TCON0:** program for the 720×1440 panel mode (timings/porches) in DSI command/video
   mode; route it to DE2.
5. **MIPI-DSI host + D-PHY:** enable DSI, enable D-PHY, configure lanes/format.
6. **ST7703 init:** send the XBD599 DCS/manufacturer command sequence over DSI in low-power
   mode (long, panel-specific — see `panel-sitronix-st7703.c` / lupyuen).
7. **Backlight:** enable the backlight regulator/enable-GPIO + PWM brightness (PWM in the
   R_PWM block). NOTE: on an IPS panel an uninitialised panel reads black even with the
   backlight on, so "backlight only" is **not** a reliable visible milestone — use the LED.
8. **DE2 mixer (@ 0x01100000):** point the UI overlay layer at your XRGB framebuffer and
   enable scanout (this is the only part the UnoDOS port already does). Key regs used:
   `GLB_CTL 0x01100000`, `GLB_SIZE 0x0110000C`, blender `0x01101000/08/8C`, UI overlay
   `OVL_ATTR 0x01103000` (`0xFF000405` = glob-alpha|XRGB8888|enable), `OVL_MBSIZE/SIZE/
   COORD/PITCH/TOPADD 0x01103004/88/08/0C/10`. The mixer composites over the panel TCON0 is
   already driving — so it only produces visible output **after** steps 1–7.

---

## 6. Paths to a lit panel

**There is NO off-the-shelf U-Boot that lights the A64 PinePhone DSI panel.** [CFG]
Checked mainline 2026.07 **and** Megi's fork (`codeberg.org/megi/u-boot`, `megi-v2025.04`
and others): both ship only the legacy sunxi video drivers (`lcdc`, `sunxi_de2`,
`sunxi_dw_hdmi`, `sunxi_lcd`, `tve` — A10/A20-era LCD/HDMI), with **no sun6i MIPI-DSI host,
no ST7703 panel, no D-PHY** in the tree, and an empty display section in
`pinephone_defconfig`. **Tow-Boot also has no PinePhone display output** [REF]. So the
"just use a DSI-capable U-Boot and adopt its framebuffer" plan (the Raspberry-Pi model) is
**not available** on this device — nothing but the Linux kernel (and p-boot) brings up the
panel. This is the core hard truth of bare-metal PinePhone graphics.

| Path | Effort | Notes |
|---|---|---|
| **A. Full DSI bring-up in the payload** | High | The purist bare-metal route (§5), and the only fully self-contained one. Best reference = **lupyuen's NuttX PinePhone articles** — a proven, *register-level, non-Linux* recipe (NuttX is an RTOS that lights the panel from cold, exactly our situation). Realistically needs a serial console to iterate; the LED channel localizes stages blind. **This is what a from-scratch PinePhone project will need regardless.** |
| **C. p-boot** | Medium | Megi's **separate** PinePhone bootloader (NOT U-Boot) — the one that actually shows graphics/a boot menu on the A64 PinePhone. It lights the panel + sets up a DE2 framebuffer, then boots an ARM64 "kernel". A payload carrying an ARM64 Image header could be booted by it with the panel already up, and our adopt-framebuffer code would then show pixels — fastest route to pixels, but the project becomes "p-boot + payload" (less self-contained), and p-boot's config/image format must be learned. |
| ~~B. DSI-capable U-Boot~~ | — | **Ruled out — does not exist for the A64 PinePhone** (see above). |

---

## 7. Flashing / tooling gotchas (host side) [HW]

- **Flash on a stable Linux box, not a laptop:** a laptop whose USB card-reader shares a
  USB controller with a USB-Ethernet adapter will **drop the network during heavy SD
  writes**; laptops also sleep mid-write. (Carbon exhibited both; devbuntu was reliable.)
- **Windows:** `Set-Disk -IsOffline` **fails on removable media** ("Removable media cannot
  be set to offline"); you must `FSCTL_LOCK_VOLUME` + `FSCTL_DISMOUNT_VOLUME` the volume
  (P/Invoke) and hold the handle open while writing `\\.\PhysicalDriveN`. A guarded
  PowerShell raw-writer doing this is in `flash-win.ps1`.
- **`/dev/sdX` letters are NOT stable** across reinserts/readers — resolve the target by
  **model string** (`lsblk -dno MODEL` == "SD Transcend"), never a fixed letter. In this
  rig there are also a Ugreen "MassStorageClass" reader and an unrelated RPi card.
- devbuntu has a **udev USB write-blocker** (`ro=1` on all USB `sd*`): `blockdev --setrw`
  to write, re-arm `--setro` via a trap. Time-limited passwordless sudo via
  `sudo passwordless <dur>` (reverts on a timer / reboot; the bootstrap itself needs an
  interactive password once the window lapses).
- A 960 MB image flashes fine to a larger card (e.g. 64 GB) — the new MBR defines the only
  partition the phone reads; trailing space is ignored.

---

## 8. Status log

- Boot chain (SPL → U-Boot → `boot.scr` → payload `go`) **runs on hardware** [HW]
  (green-LED bisect + the diag-card no-fall-through test).
- Screen dark **root cause = no DSI bring-up anywhere** in the mainline chain [CFG] (§3).
- Payload runs end-to-end; LED debug channel works [HW] (§4).
- Path B (DSI-capable U-Boot) **ruled out** — no such U-Boot exists for the A64 PinePhone
  (mainline + Megi both lack a sun6i DSI host) [CFG].
- **Decision: Path A taken** (self-contained, matches every other from-scratch UnoDOS
  port). **Implemented** in `panel.inc.s`: the payload now does the full bring-up itself
  — CCU PLLs/gates, RSB + AXP803 PMIC rails (DLDO2 = MIPI power), MIPI-DSI host, D-PHY +
  analog LDOs, the ~20-command ST7703 init (precomputed DSI packets, ECC+CRC done in
  `mkdata.py`), DSI high-speed start, TCON0 720×1440, and PWM backlight — then the DE2
  mixer UI overlay (`fb_init`) scans out the 480×640 framebuffer at the panel's top-left.
  Register values distilled from Apache NuttX's PinePhone port (lupyuen) + the Linux
  sun50i-a64 drivers; the packet framing is validated against lupyuen's worked example
  (SETEXTC → `39 04 00 2C B9 F1 12 83 84 5D`, ECC/CRC exact).
- **What's verified:** the bring-up runs end-to-end in the Unicorn harness (peripheral
  MMIO sunk, status polls satisfied) and all M1–M3 milestones still render — i.e. the
  code is well-formed, encodes correctly, and never hangs (all polls bounded). The
  harness has no panel/DSI/clock model, so **whether the panel actually lights is
  hardware-only**. Build `./build.sh paneldbg` for the staged PD18-LED beacon (blink
  count 1–6 marks each block) and run it flash-free over FEL: `./fel.sh run`.
- **Serial console UP + payload instrumented [HW].** The headphone-jack UART (3.3 V TTL,
  115200 8N1, **DIP switch 6 = OFF** = UART) now works via a home-made TRRS cable: jack
  **Tip = phone RX, Ring = phone TX, Sleeve = GND**; on the USB-TTL adapter **TXD→Tip,
  RXD→Ring, GND→Sleeve** (the RX-on-Ring crossover is the bit that matters for *reading*
  the console). 3-pole plug is fine (4th ring unused); a multimeter loopback (adapter
  TXD↔RXD jumper) proves the host side independently. U-Boot already inits UART0, so the
  payload just polls `LSR.THRE` (0x01C28014 bit5) and writes `THR` (0x01C28000) — added
  `uart_putc/puts/hex/print_reg` (PANELDBG) to trace the bring-up live.
- **On-hardware serial trace — clocks/PMIC/DSI-host/D-PHY/TCON0 ALL good; fault is the
  2nd ST7703 DCS command** [HW]. Trace:
  `panel_init → PLL_DE=91001701 PLL_VIDEO0=91006207 PLL_MIPI=90c0071a` (**all bit28 =
  LOCKED on real silicon** — PLL_MIPI, the DSI/panel clock, is good), `TCON0_GCTL=80000000`
  (enabled) → `[pmic] ok → [dsi_host] ok → [dphy] ok → [ystage] ok → [reset_hi] ok →
  "[st7703] begin: .."` (two dots) → **FAULT** (solid-white status LED = a *persistent*
  CPU exception trapped by the payload's own vector). So the whole clock/power/DSI-host/
  D-PHY/TCON0 chain succeeds; the payload dies sending the **2nd** ST7703 DCS command —
  **SETMIPI, the first *large* packet** (35 B / 9 FIFO words; cmd 0 SETEXTC = 10 B / 3
  words transmits fine).
- **Reference-diffs applied (from p-boot + NuttX), none fixed the cmd-1 fault:** (a) moved
  `tcon0_init` to *before* the DSI block (TCON0 is the timing master), (b) `dsi_start` now
  clears the LP11 `INST_FUNC` `LANE_CEN` bit (bit4 @ `DSI+0x20`) between HSC and HSD for a
  continuous HS clock, (c) added the ~160 ms DE2-settle delay after `fb_init`, (d) fixed
  `dcs_send`'s clear-flags `0x04000200 → 0x06000200` (was missing `RX_FLAG` bit25). Packet
  framing is **correct** (pre-computed in `mkdata.py`: header+ECC+payload+CRC; SETEXTC
  validated `39 04 00 2C B9 F1 12 83 84 5D`).
- **Leading hypothesis for the cmd-1 fault:** command 0's low-power DCS transmit never
  actually *completes* — `INSTRU_EN` (`DSI_BASIC_CTL0` `0x01CA0010` bit0) doesn't self-clear,
  so the bounded poll in `dcs_send` times out and returns with the DSI host wedged
  mid-instruction; command 1's trigger on the wedged engine then faults. The size
  correlation is incidental (cmd 0 is just the one that leaves it stuck).
- **Next steps:** (1) instrument `dcs_send` to read back `DSI_BASIC_CTL0`/status *after*
  command 0 — confirm whether `INSTRU_EN` clears and whether the LP escape actually
  transmitted. (2) Compare `dcs_send`'s exact trigger/wait sequence line-by-line against
  NuttX `a64_mipi_dsi_write` (+ `a64_disable/enable_dsi_processing`, `a64_wait_dsi_transmit`)
  and Linux `sun6i_dsi_dcs_write_long` — focus on the LP-escape handshake / how the FIFO is
  drained, not the framing. (3) Re-test on hardware via the serial markers (`[st7703] begin: `
  + one dot per command) — success = dots run to all 20, `[st7703] ok`, and the panel lights.

**Debug build:** `./build.sh paneldbg` adds the UART trace markers, the RGB status-LED beacon
(BLUE=clocks, RED=PMIC, GREEN=DSI/D-PHY, YELLOW=ST7703, MAGENTA=dsi_start; **fast WHITE
flutter = a CPU fault**, and the LED is RGB: red=PD19, green=PD18, blue=PD20, active-high),
the per-DCS-command dots, and an AArch64 exception vector that flutters the LED instead of
letting a fault silently reboot.

### 2026-06-19 — every block fixed + verified vs NuttX; PANEL PROVEN ALIVE; wall = DE2/TCON0/DSI **video scanout** [HW]

A long serial-debug session (run from a separate dev box over SSH; see "Rig/workflow" below)
took the panel from **fully-dark → backlit → confirmed-alive-but-no-image**. Net state:
**the entire bring-up is now byte-for-byte faithful to the working NuttX/lupyuen reference,
the panel is electrically alive and reports display-ON, yet no video pixels reach it.**

**SEVEN bugs found & fixed** (all confirmed by the on-hardware serial trace / the lit panel):
1. **Alignment fault walking the ST7703 init blob.** SETMIPI (cmd 1) is 35 bytes (odd), so
   after `x19 += 4 + len` the blob pointer is odd, and the next `ldrh` length-read takes a
   data-abort (ESR EC=0x25 / DFSC=0x21 = alignment; `FAR` = odd addr). **NOT** the originally-
   hypothesised "INSTRU_EN stuck / DSI wedged" — both DCS writes transmit fine, `BASIC_CTL0`
   bit0 self-clears every time. Fix: read len/delay as **`ldrb` byte-pairs** (alignment-agnostic).
   → all 20 DCS commands now send, `[st7703] ok`, payload reaches mainloop.
2. **Backlight enable on the wrong GPIO.** `backlight_on` drove PH10 via `PIO+0xA0`/`0xCC`
   (= PE_DAT / PF region). PH CFG1/DAT are **`PIO+0x100`/`+0x10C`** (0x01C20900/0x01C2090C).
   → panel went from fully-dark to **backlit**.
3. **DE2 MIXER writes silently dropped.** `clk_init` enabled the DE *bus* clock but never the
   DE block's *internal* gates/reset at DE base 0x01000000 (`SCLK_GATE +0x00`, `HCLK_GATE +0x04`,
   `AHB_RESET +0x08`, all bit0; `DE2TCON_MUX +0x10`=0). Symptom: post-`fb_init` readback
   `GLB_CTL`=0 / `OVL_TOPADD`=0. Fix: write those 4 at the top of `fb_init`. → readback flips
   to `GLB_CTL=1` / `OVL_TOPADD=0x40400000`.
4. **Blender configured for 3 channels with 1 layer.** `BLD_FILL_COLOR_CTL`(0x1101000)=0x701
   (enable pipes 0,1,2) + `BLD_CH_RTCTL`(0x1101080)=0x321 (3-channel route). Fix (NuttX
   single-channel): `0x101` (P0_EN|P0_FCEN) + `0x1` (pipe0←ch1), + `BLD_FILL_COLOR`(0x1101004)=
   0xFF000000.
5. **DE2 init incomplete.** Added NuttX `a64_de_init`'s **MIXER0 clear** (zero
   0x01100000..+0x6000) + **disable all 11 enhancement/scaler blocks**: VS 0x01120000,
   UNDOC 0x01130000, UIS1 0x01140000, UIS2 0x01150000, FCE 0x011A0000, BWS 0x011A2000,
   LTI 0x011A4000, PEAKING 0x011A6000, ASE 0x011A8000, FCC 0x011AA000, DRC 0x011B0000.
6. **`dsi_start` ordering.** The continuous-HS-clock enable (clear `LANE_CEN`, DSI+0x20 bit4)
   must come **before** the 1 ms settle, not after (clock must stabilise before HS data).
7. **TCON0 `SAFE_PERIOD` typo.** Was `0x00BB8003`; correct is **`0x0BB80003`** (FIFO_NUM
   3000=0xBB8 belongs at bits[28:16], i.e. `<<16`). Gates the frame-transfer trigger.

**Verified byte-exact vs authoritative source** (WebFetched NuttX `arch/arm64/src/a64/*` +
Linux `sun6i_mipi_dsi.c`, **not** just the [REF] blueprint) — do NOT re-audit these on return:
`clk_init` (all 3 PLLs lock on HW: PLL_DE=91001701, PLL_VIDEO0=91006207, **PLL_MIPI=90c0071a**);
`pmic_init` == `pinephone_pmic_init` (DLDO1 3.3V / GPIO0LDO 3.3V / **DLDO2 1.8V = the XBD599
panel power**; NuttX lights the panel with exactly these 3 rails); `dsi_host_tbl` == every value
in `a64_mipi_dsi_enable` incl. all video-mode timing (BASIC_CTL1 0x5BC7, BASIC_SIZE0/1, PIXEL_PH
0x1308703E, the SYNC/HSA/HBP/HFP/HBLK/VBLK block, INST_FUNC table); `dphy_init` == `a64_mipi_dphy_enable`;
`dcs_send` trigger/wait == `a64_mipi_dsi_write` (JUMP_SEL 0x000F0004, clear-flags 0x06000200);
`dcs_packet` framing correct (1B→DT0x05, 2B→0x15, 3+B→0x39); `tcon0_init` **order (before the
DSI block) AND values match `up_fbinitialize` exactly** (8080 IF, CTL 0x81000000, DCLK 0x80000006,
BASIC0 0x02CF059F, CPU_IF 0x10010005, the TRI regs) — the tcon0-first order is CORRECT, do not
"fix" it. `fb_init` == `a64_de_init` + `a64_de_blender_init` + `a64_de_ui_channel_init` + `a64_de_enable`.

**THE DECISIVE DIAGNOSTICS (this is what to build on when we return):**
- **Panel is ALIVE and received our init.** A **DCS READ** of get_power_mode (0x0A) in LP mode
  (`dcs_read_dbg`, runs after `st7703_init`, before `dsi_start`; LPRX instruction chain
  JUMP_SEL=**0x100700F4** = LP11→LPDT→DLY→TBA→END; response read from DSI+0x240) returns
  `RXCTL=02060003` (**RX_FLAG bit25 SET** = the panel answered, no overflow) and
  `RXDAT=...1c` → **power-mode byte 0x1C = Display-ON + Normal-mode + Sleep-OUT**. So the DSI
  LP link works AND the panel processed SLPOUT/DISPON. The panel is *not* the problem.
- **Cache coherency RULED OUT.** Added an SCTLR readback to the trace: **`SCTLR=0x30c50830`**
  → bit0 M=0 (MMU **off**), bit2 C=0 (D$ off), bit12 I=0 (I$ off). U-Boot `go` hands off
  **MMU-OFF** (so `_start`'s "keep MMU on" comment is moot). MMU-off ⇒ data accesses are Device
  memory ⇒ FB writes hit DRAM directly ⇒ DE2 DMA is coherent. (A `dc cvac` flush added to
  `post_fill` changed nothing — it's a no-op with caches off.)
- **DE2 is NOT scanning out.** Decisive test: set the blender **background colour to RED**
  (`BLD_BK_COLOR`=0xFFFF0000) — the backdrop DE2 emits regardless of any layer. Result on HW:
  **still black.** And solid POST fills written straight to the FB (0x40400000, where the DE2
  layer points) never appear either. So nothing is reaching the panel from DE2/TCON0.

**CONCLUSION / the wall:** panel alive + display-ON + DSI in HS (`DSI_BASIC_CTL0=00030001`,
INSTRU_EN running) + DE2 enabled & pointed at the FB + TCON0 enabled & fully-correct config —
yet **the DE2→TCON0→DSI *video* path delivers no pixels.** Every register matches NuttX, so the
difference is almost certainly **environmental**: NuttX boots natively and sets up *all* clock
state; we run as a U-Boot `go` payload and inherit whatever clock/peripheral state U-Boot left,
which has **no DSI/DE/TCON driver** — so some clock divider, reset, or enable that NuttX's runtime
establishes may differ from what we (and U-Boot) leave. The Unicorn harness cannot model any of
this (it sinks all the MMIO).

**WHEN WE RETURN — candidate next experiments (in rough priority):**
1. Prove whether **TCON0 is actually scanning**: read `TCON0_GINT0` (+0x04) frame/vblank flags
   (and/or a line-counter) twice ~50 ms apart in the mainloop — changing = scanning, static =
   stalled trigger. Likewise check whether the DSI video instruction loop is actually consuming
   from TCON0.
2. **Cross-check megi's p-boot** (a *different* known-good A64 PinePhone bring-up that lights the
   panel) register-by-register against ours for the DE/TCON/DSI-video startup — most likely to
   surface the one non-obvious step NuttX and our derivation both assume the environment provides.
3. Re-examine the **DE/TCON clock dividers & the DE↔TCON0↔DSI rate relationship** (PLLs lock, but
   the *dividers*/mux that NuttX programs from cold may differ post-U-Boot).
4. Match NuttX's **exact ordering**: `de_init` → wait 160 ms → (`blender_init`+`ui_channel_init`+
   `de_enable`+`GLB_DBUFFER`), instead of doing all of `fb_init` then waiting.
5. Consider whether the panel needs **DSI VIDEO mode actually started by TCON0 first frame** /
   an initial trigger kick in the 8080-IF-feeding-video-mode hybrid lupyuen uses.

**Diagnostic toolbox now in the tree** (all `PANELDBG`, harness-safe): `SCTLR` readback;
exception handler prints **VEC/EL/ESR/FAR/ELR** (vector slot → sync-abort vs SError; FAR=fault
addr); per-DCS `BCTL0`/`CMDCTL` readback; **`dcs_read_dbg`** panel-liveness probe; `post_fill`
`dc cvac`. `build.sh panelpost` = `PANELDBG`+`POST` (serial markers **and** on-screen RED→GREEN→
BLUE fills via `post_fill`, palette-independent). NOTE: the red `BLD_BK_COLOR` is a left-in
diagnostic — revert to `0xFF000000` for the real build.

**Rig / workflow used (2026-06-19):** dev/edit/build/harness on **amanuensis** (the source lives
at `C:\Users\arin\Documents\Github\unodos\pinephone`). The phone, the SD card reader, **and** the
USB-TTL serial adapter ended the session all on **devbuntu**. Flash: `scp` the build to
`devbuntu:~/pine-uboot/unodos_paneldbg.bin`, then `bash ~/pine-uboot/flash-paneldbg.sh` (strict
"SD Transcend" slot resolver + write-blocker `--setrw`/trap + offset-mount of the FAT, copies over
`unodos.bin`). Serial capture on devbuntu: `sudo stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb
raw -echo; sudo timeout 120 cat /dev/ttyUSB0 > /tmp/pine-serial.log` (FTDI FT232 0403:6001). The
serial only streams during boot → start the capture, THEN power-cycle. (Earlier in the session the
adapter was on a Windows laptop **Carbon**, COM3, driven over `ssh carbon "powershell -NoProfile
-EncodedCommand <b64-UTF16LE>"` + a `System.IO.Ports.SerialPort` reader — base64-encoding the PS
sidesteps all nested-quote breakage through git-bash→ssh→Windows shell. Flashing from Carbon was
just `Copy-Item build E:\unodos.bin` + `Write-VolumeCache E` since the FAT auto-mounts as E:.)
Code is **NOT committed**.

### 2026-06-20 — ✅ UnoDOS BOOTS + DISPLAYS via p-boot (DSI wall bypassed) [HW]

While the from-scratch DSI video path is still unsolved (above), we proved UnoDOS runs on real
hardware by letting **megi's p-boot** (Path C in §6 — the bootloader that lights the A64 panel
itself and hands off a live framebuffer) do the panel bring-up, with UnoDOS adopting its FB:

- **UnoDOS as an "arm64 kernel":** added a 64-byte ARM64 Image header at `_start` (code0=`b _entry`;
  **text_offset=0x80000** → p-boot's `LINUX_IMAGE_PA` 0x40000000 + 0x80000 = our link addr
  0x40080000; magic 0x644d5241 at offset 56). Transparent to U-Boot `go` and the harness.
- **New build flags:** `PBOOT` (skip `panel_init`; `fb_init` instead **adopts** p-boot's framebuffer
  — reads DE2 `OVL_TOPADD`/`OVL_PITCH`, draws there, does NOT reprogram DE2) and `pbootdbg`
  (`PBOOT`+`PANELDBG`). `build.sh pboot` / `build.sh pbootdbg`.
- **Result on HW:** p-boot lights the panel; the **UnoDOS launcher renders** — small, top-left,
  because our 480×640 content is drawn pitch-relative into p-boot's **native 720×1440** FB. Trace:
  `DE2_OVL_TOPADD=0x48000000` (p-boot's FB = what we adopted), `GLB_CTL=1`, `[unodos] mainloop`.

**p-boot card recipe (all on devbuntu, `~/pboot/`):** prebuilt pieces from
`github.com/davidwed/p-boot` `dist/` (`p-boot.bin` = 32 KiB GUI build *with display support*,
`fw.bin` = ATF+SCP, `p-boot-conf-native` = x86-64 builder); a PinePhone-1.2 DTB (from the U-Boot
tree `arch/arm/dts/sun50i-a64-pinephone-1.2.dtb`). `conf/` holds `boot.conf`
(`no=0` / `name=UnoDOS` / `dtb=` / `atf=` / `linux=` / `bootargs=`), `board.dtb`, `fw.bin`,
`Image` (= our `unodos_pbootdbg.bin`), and a **`files/` dir with `off.argb`+`pboot2.argb`**
(GUI splashscreens — REQUIRED, or `p-boot-conf` errors). Card layout: MBR, one **type-0x83
bootable** partition from sector 2048; **`dd p-boot.bin of=<dev> bs=1024 seek=8`** (8 KiB boot
sector); **`p-boot-conf-native conf/ <dev>1`** formats the boot-fs. GOTCHA: the devbuntu udev
write-blocker **re-arms `ro` on partition-table re-read**, so `blockdev --setrw` BOTH the device
**and** the partition immediately before EACH raw write (the `dd` and the `p-boot-conf`).
`p-boot-conf` honoured our header (`Kernel Image detected: text_offset=0x00080000`). This p-boot
card **replaced** the U-Boot card on the SD Transcend (the U-Boot card is rebuildable via
`mksd.sh`). Reassess next: center/scale the 480×640 in the 720×1440 FB; confirm UART input/nav;
ship a clean `unodos_pboot.bin`; and eventually return to fix the native DSI video path.

---

## 9. References

- lupyuen, "Understanding PinePhone's Display (MIPI DSI)" — https://lupyuen.org/articles/dsi
- lupyuen, "Rendering PinePhone's Display (DE and TCON0)" — https://lupyuen.github.io/articles/de
- lupyuen, "NuttX RTOS for PinePhone: LCD Panel" — https://lupyuen.github.io/articles/lcd
- lupyuen, "NuttX RTOS for PinePhone: MIPI Display Serial Interface" (D-PHY/DSI) — https://lupyuen.github.io/articles/dsi3 (Zig News mirror)
- xnux.eu PinePhone (Megi) — https://xnux.eu/devices/pine64-pinephone.html
- linux-sunxi.org: BROM, FEL, PinePhone boot priority
- Linux: `drivers/gpu/drm/sun4i/sun6i_mipi_dsi.c`, `drivers/gpu/drm/panel/panel-sitronix-st7703.c`, sun50i-a64 CCU, AXP803 regulators

---

## Appendix A — Path-A bare-metal DSI bring-up blueprint (register-level)

All values **[REF]** distilled from lupyuen's NuttX-on-PinePhone articles (de / dsi / lcd)
+ the Linux drivers. **Verify on hardware** — treat as the starting recipe, not gospel.
Panel = 720×1440 (HW writes use 1440×720 = `(1439<<16)|719 = 0x059F02CF` in several regs).

### A.0 Base addresses
| Block | Base |
|---|---|
| DE / Display Engine | `0x01000000` |
| MIXER0 | `0x01100000` (DE + 0x100000) |
| TCON0 | `0x01C0C000` |
| CCU | `0x01C20000` |
| PIO (ports B–H) | `0x01C20800` |
| MIPI-DSI host | `0x01CA0000` |
| MIPI D-PHY | `0x01CA1000` |
| R_PIO (port L) | `0x01F02C00` |
| R_PWM | `0x01F03800` |
| AXP803 PMIC | via **RSB** `0x01F03400`, runtime addr `0x2D` |

### A.1 Master sequence (order matters)
1. Backlight: configure PWM (PL10) + enable GPIO (PH10) — see A.2.
2. Panel reset **LOW** (PD23, active-low).
3. AXP803 rails on via RSB (A.3); **wait 15 ms**.
4. Enable MIPI-DSI + D-PHY clocks/blocks (A.5, A.6).
5. Panel reset **HIGH**; **wait 15 ms**.
6. Send ST7703 init DCS commands (A.7).
7. Start DSI bus in **HS** mode.
8. CCU clocks for DE + TCON0 (A.4), DE2/MIXER0 init + TCON0 timing (A.8, A.9).
9. Point UI overlay at the framebuffer + enable scanout (A.8 step "UI ch1").

### A.2 Backlight  [REF]
- PL10 → PWM function (R_PIO `+0x04` bits[10:8]=`0x2`); PH10 → output high
  (PIO `+0x100` bits[10:8]=`0x1`, PIO `+0x10C` bit10=1) — enables the AP3127.
- R_PWM: period `R_PWM+0x04` = `0x04AF0437` (1199 cyc, ~90% duty); ctrl `R_PWM+0x00` =
  `0x5F` (SCLK_CH0_GATING bit6 | PWM_CH0_EN bit4 | PRESCAL bits[3:0]=0xF).

### A.3 AXP803 PMIC rails (RSB, addr 0x2D)  [REF]
| Rail | V | Purpose | Set |
|---|---|---|---|
| DLDO1 | 3.3 V | sensors/USB/cam | DLDO1_VOLT=26 (2.6+0.7), OUTPUT_PWR_CTL2 bit3=1 |
| DLDO2 | 1.8 V | **MIPI-DSI power** | DLDO2_VOLT=11, OUTPUT_PWR_CTL2 bit4=1 |
| GPIO0LDO | 3.3 V | touch | GPIO0LDO_HIGH=26, GPIO0_CTRL bits[2:0]=0b11 (LDO) |

RSB driver: init RSB, set device runtime addr `0x2D`, byte read/write; `pmic_clrsetbits`.

### A.4 CCU clocks / PLLs  [REF]
| What | Reg (CCU+) | Value | Notes |
|---|---|---|---|
| PLL_DE | `0x0048` | `0x81001701` | 288 MHz; poll bit28 lock |
| DE clk | `0x0104` | `0x81000000` | gate + src=DE-PLL |
| DE bus reset | `0x02C4` bit12 | set | BUS_SOFT_RST1 |
| DE bus gate | `0x0064` bit12 | set | BUS_CLK_GATING1 |
| SRAM C1→DMA | `0x01C00004` | `0x0` | SRAM_CTRL_REG1 |
| PLL_VIDEO0 | `0x0010` | `0x81006207` | |
| PLL_MIPI | `0x0040` | `0x80C0071A` | enable AFTER its LDOs |
| TCON0 clk | `0x0118` | `0x80000000` | src = MIPI-PLL |
| TCON0 gate | `0x0064` bit3 | set | |
| TCON0 reset | `0x02C4` bit3 | set | |
| DSI gate | `0x0060` bit1 | set | BUS_CLK_GATING0 |
| DSI reset | `0x02C0` bit1 | set | BUS_SOFT_RST0 |
| DSI-DPHY clk | `0x0168` | `0x8203` | 150 MHz |

### A.5 MIPI-DSI host (base `0x01CA0000`, "DSI+off")  [REF]
- `+0x00` DSI_CTL=`0x1`; `+0x10` BASIC_CTL0=`0x30000` (ECC+CRC); `+0x60`=`0xA`; `+0x78`=`0x0`.
- Instr regs `+0x20..0x3C` = `0x1F, 0x10000001, 0x20000010, 0x2000000F, 0x30100001,
  0x40000010, 0x0F, 0x5000001F`; `+0x4C`=`0x560001`; `+0x2F8`=`0xFF`.
- `+0x14` BASIC_CTL1 = video-mode | frame-start | precision | Video_Start_Delay=1468.
- `+0x90` PIXEL_PH=`0x1308703E` (DT 0x3E 24-bit, WC 2160); `+0x98`=`0xFFFF`; `+0x9C`=
  `0xFFFFFFFF`; `+0x80` PIXEL_CTL0=`0x10008`; `+0x0C`=`0x0`.
- Sync pkts: `+0xB0`=`0x12000021`, `+0xB4`=`0x1000031`, `+0xB8`=`0x7000001`, `+0xBC`=`0x14000011`.
- Blanking: `+0x18`=`0x11000A` (VBP17,VSA10); `+0x1C`=`0x05CD05A0` (VT1485,VACT1440);
  H-blank regs `+0xC0..0xE4`, V-blank `+0xE8..0xEC`.
- Start HS: `+0x48`=`0xF02`, set `+0x10` bit0, wait 1 ms, `+0x48`=`0x63F07006`, set `+0x10` bit0.
- **DCS TX:** write packet (DI, WC, ECC, payload, CRC16) to `+0x300..0x3FC`; CMD_CTL `+0x200`:
  set bits[26:25] to clear RX, bits[7:0]=len-1. Long-write DT=`0x39`, short DT per DCS.

### A.6 MIPI D-PHY (base `0x01CA1000`)  [REF]
`+0x04`=`0x10000000`; timing `+0x10..0x20`; `+0x00`=`0x31` (enable); analog `+0x4C..0x5C`
(PWS/CSMPS/CKDV); enable LDOR, LDOC, LDOD, VTT, DIV sequentially w/ ~1 µs delays.

### A.7 ST7703 panel init  [REF]
~20 DCS commands (panel-sitronix-st7703.c order): SETEXTC(0xB9) → SETMIPI, SETPOWER_EXT,
SETGIP1/2, SETCOLMOD, SETESPL, SETSRCCTRL, SETDGC, SETRGBCYC, SETVOPN, SETVAKC,
SETGAMMA1/2, SETDGC_LUT, SETDISPLAY, SETEQ, SETPANEL, SETCYC → SLPOUT(0x11) **wait 120 ms**
→ DISPON(0x29). Send each via the DSI DCS-TX path (A.5).

### A.8 DE2 / MIXER0 (base `0x01100000`)  [REF]  — the UnoDOS payload already does most of this
- Clocks: SCLK_GATE DE`+0x00` bit0, HCLK_GATE DE`+0x04` bit0, AHB_RESET DE`+0x08` bit0;
  route mixer→TCON0: DE2TCON_MUX DE`+0x10` clear bit0.
- Clear MIXER0 `0x0000..0x5FFF` = 0; disable scalers/FCE/BWS/LTI/PEAKING/ASE/FCC/DRC.
- GLB: GLB_CTL `+0x0000`=`0x1`; GLB_SIZE `+0x000C`=`0x059F02CF`; GLB_DBUFFER `+0x0008`=1 (commit).
- Blender: BLD_FILL_COLOR_CTL `+0x1000`=`0x701`; BLD_CH_RTCTL `+0x1080`=`0x321`;
  BLD_SIZE `+0x108C`=`0x059F02CF`; BLD_BK_COLOR `+0x1088`=`0xFF000000`; per-pipe
  `+0x1004/14/24`, `+0x1008/18/28`, `+0x100C/1C/2C`, `+0x1090/94/98`=`0x03010301`.
- **UI ch1 (XRGB8888):** OVL_UI_ATTR `+0x3000`=`0xFF000405`; TOP_LADD `+0x3010`=fb addr;
  PITCH `+0x300C`=`720*4=0xB40`; MBSIZE `+0x3004`=`0x059F02CF`; SIZE `+0x3088`=`0x059F02CF`;
  COOR `+0x3008`=0. (This is the part `fb_init` already does — the rest of Appendix A is the
  missing panel bring-up that must run first.)

### A.9 TCON0 (base `0x01C0C000`)  [REF]
- Disable+clear: GCTL `+0x00`=0, GINT0 `+0x04`=0, GINT1 `+0x08`=0.
- CTL `+0x40`=`0x81000000` (En|IF=DSI|src=DE0); DCLK `+0x44`=`0x80000006` (En=8, div=6);
  BASIC0 `+0x48`=`0x02CF059F` (X=719,Y=1439).
- CPU-IF `+0x60`=`0x10010005` (24-bit DSI, FLUSH, trigger); ECC_FIFO `+0xF8`=`0x8`.
- Trigger: TRI0 `+0x160`=`0x002F02CF`, TRI1 `+0x164`=`0x059F`, TRI2 `+0x168`=`0x1BC2000A`;
  SAFE_PERIOD `+0x1F0`=`0x0BB80003`.
- IO_TRI `+0x8C`=`0xE0000000` (enable outputs, DMB); GCTL `+0x00` set bit31 (enable, DMB).

### A.10 Implementation notes for the payload
- Add a `gpio_cfg(port,pin,mode)` + `gpio_set` helper (PIO/R_PIO); we already have PD18 LED.
- Add an **RSB driver** (the biggest new sub-block) for the AXP803 rails.
- Reuse the existing **LED beacon** (PD18) to mark stages A.2→A.9 by blink count while blind.
- The DE2/UI-overlay code already in `kernel.s` (`fb_init`) is A.8's tail — keep it; the new
  work is everything before it (clocks, PMIC, panel reset, DSI host, D-PHY, ST7703, TCON0).
- Success = panel lights. Likely needs the **serial console** to debug DSI timing/DCS.
