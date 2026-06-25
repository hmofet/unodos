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
- **Result on HW:** p-boot lights the panel; the **UnoDOS launcher renders**. Trace:
  `DE2_OVL_TOPADD=0x48000000` (p-boot's FB = what we adopted), `GLB_CTL=1`, `[unodos] mainloop`.
- **Centering:** the adopt path clears the whole 720×1440 FB to black (wiping p-boot's splashscreen)
  and offsets `fb_base` so the 480×640 content sits **centered** (offset 120×400 px), not top-left.
  Aspect mismatch (UnoDOS 3:4 vs panel 1:2) means it's centered-on-black, not full-screen; scaling
  to fill (1.5× width / letterbox, or stretch) is a documented future option, deferred.

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

### 2026-06-20 (cont.) — p-boot register cross-check COMPLETE; 3 environmental fixes + TCON0-scan diagnostic staged [code-only, awaiting HW]

Returned to the native DSI video wall (§8 above). Did the §8-candidate-#2 work fully — a
**register-by-register cross-check of megi's p-boot `display.c`** (davidwed/p-boot, the SAME
bootloader that lit *this exact phone* on 2026-06-20, so it is ground-truth for our hardware,
not just a paper reference). Read its real source (not a paraphrase) and hand-decoded the
bitfields. **Result: our bring-up matches p-boot on every register that matters:**
- **`tcon0_init`** — DCLK (MIPI_PLL/6), CTL (enable|IF_8080), `timing_active`=720×1440, ECC_FIFO_EN,
  CPU_IF (MODE_DSI|TRI_FIFO_FLUSH|TRI_FIFO_EN|TRI_EN = our `0x10010005`), TRI0/TRI1/TRI2,
  SAFE_PERIOD(NUM=3000,MODE=3 = our `0x0BB80003`), io_tristate `0xe0000000`, final
  `TCON_ENABLE` (GCTL bit31) — **identical**.
- **DE2 blender/UI layer** (p-boot does this in `display_commit`, we in `fb_init`) — route=`0x1`,
  fcolor_ctl=`0x101`, pipe0 fcolor=`0xff000000`, bld_mode=`0x03010301`, UI attr=`0xFF000405`,
  layer size/pitch/coord, `GLB_DBUFFER` commit — **identical**. DE-clock bring-up (gate/rst/bus/sel
  at DE base 0x01000000, mixer0→TCON0 mux) — **identical**.
- **`dsi_start`** — HSC `JUMP_SEL`=`0x00000F02`, then clear `LANE_CEN` (INST_FUNC0 bit4), 1 ms,
  HSD `JUMP_SEL`=`0x63F07006` — **identical** (decoded p-boot's `DSI_INST_ID_*` bitfields by hand;
  they equal our literals exactly).

So our registers are now faithful to **TWO** independent known-good A64 implementations (NuttX
*and* p-boot). That hardens the conclusion: **the wall is environmental** — state the U-Boot `go`
handoff leaves that a cold BROM boot (NuttX/p-boot) does not. Three concrete divergences from the
cold-boot path were found and fixed (all in the **non-debug** bring-up, harness-verified — the
default build still renders the launcher byte-clean, 4922-B PNG):
1. **Full RESET CYCLE of DE/TCON0/DSI** (`clk_init`): we previously only *de-asserted* the CCU
   resets; now we **assert then de-assert** (BUS_SOFT_RST1 bit12=DE, bit3=TCON0; BUS_SOFT_RST0
   bit1=DSI) so each block starts from its cold default regardless of what U-Boot left. Plus a
   reset *cycle* of the DE-internal mixer0 reset (`0x01000008`) in `fb_init`.
2. **DE clock DIVIDER explicitly cleared** (`clk_init`, DE_CLK_REG `CCU+0x104` bits[3:0]): a
   non-zero divider left by U-Boot would slow the DE clock below the pixel rate and starve TCON0.
   Cold boot leaves it 0 (/1); we now force that. (Both p-boot and NuttX leave it 0 implicitly.)
3. **SRAM_CTRL1 RMW** (`clk_init`): changed the SRAM-C→DE mapping from a whole-register zero to a
   **clear-bit24-only** RMW (matching p-boot/NuttX) so it can't disturb other SRAM-C masters.

**Diagnostic added** (§8 candidate #1, PANELDBG, the decisive split): after `[unodos] mainloop`,
sample **`TCON0_GINT0`** (`0x01C0C004`) six times ~50 ms apart over UART (and re-print GCTL).
**Changing across samples ⇒ TCON0 IS scanning** (stall is downstream: DSI video xfer or panel);
**static ⇒ TCON0's frame trigger never fires** (stall is TCON0). This is the next read to take.

**Staged, AWAITING HARDWARE:** `build.sh paneldbg` rebuilt (30184 B) and scp'd to
`devbuntu:~/pine-uboot/unodos_paneldbg.bin` (md5 `1d78fd44…`). **The SD is still the p-boot card
from the previous entry** — to test the native path it must become a **U-Boot card** again (so our
`panel_init` actually runs; under p-boot it's skipped). Recipe when the card is in the devbuntu
SD-Transcend reader (confirm first): (a) `blockdev --setrw /dev/sdX` then `dd if=~/pine-uboot/
pine-fresh.img of=/dev/sdX bs=4M conv=fsync` (restores SPL+U-Boot+ATF+boot.scr+FAT — the
write-blocker re-arms RO, so `--setrw` immediately before); (b) `bash ~/pine-uboot/flash-paneldbg.sh`
(swaps the freshly-staged payload into the FAT at offset 1 MiB); (c) card → phone; (d) serial:
`sudo stty -F /dev/ttyUSB0 115200 cs8 raw -echo; sudo timeout 120 cat /dev/ttyUSB0 >/tmp/log` in
the background, THEN power-cycle. Expect in the trace: PLL locks, `[st7703] ok`, the
`dcs_read_dbg` RXCTL/RXDAT (panel alive), `DSI_BASIC_CTL0=00030001`, `DE2_GLB_CTL=1`,
`DE2_OVL_TOPADD=40400000`, then the new `TCON0_GCTL` + six `TCON0_GINT0` samples — read whether
GINT0 moves. If the reset-cycle/divider fixes lit the panel, the launcher appears; else GINT0
tells us which side of TCON0 to chase next.

### 2026-06-21 — TCON0 confirmed scanning; FULL register dump = every write STUCK; not a value bug [HW]

Ran the staged paneldbg on HW. Reset-cycle + DE-divider fixes did **not** light it (still backlit-
black; the RED `BLD_BK_COLOR` diagnostic never showed). But the diagnostics were decisive:
- **TCON0 IS scanning continuously.** GINT0 sampled with a clear-between-reads: `0x0A00 → (clear)
  → 0x800 → 0x800 …` — bit 11 (the frame-done latch, = p-boot's `display_frame_done`) **re-sets
  every ~50 ms**. So TCON0 is triggering a new frame transfer continuously; it is **not** the stall.
  `DSI_BASIC_CTL0=0x00030001` steady (video INSTRU_EN running). `DE2_GLB_STATUS=0x10` (static).
- **D-PHY analog cross-checked vs p-boot** (the last un-compared block): `dphy_enable`'s ANA3 LDOR/C/D
  → VTTC/VTTD → DIV, ANA2 CK_CPU, ANA1 VTTMODE, ANA2 P2S_CPU — same registers/bits/order as our
  `dphy_init`. So **every** DE2/TCON0/DSI/D-PHY/CCU register now matches p-boot, not just NuttX.
- **FULL on-HW register READBACK dump** (`dump_regs`/`dump_tbl`, 35 registers): **every single one
  reads back exactly what we wrote** — TCON0 (GCTL=80000000, CTL=81000000, DCLK=80000006, BASIC0=
  02cf059f, CPU_IF=10010005, TRI0=002f02cf, TRI2=1bc2000a, SAFE_PERIOD=0bb80003), DSI (BASIC_CTL0=
  00030001, CTL1=00005bc7, SIZE0/1, JUMP_SEL=63f07006, TCON_DRQ=10000007, PIXEL_PH=1308703e), DE2
  (GLB_CTL=1, GLB_SIZE=059f02cf, BLD_FILL=101, **BLD_BKCOL=ffff0000=RED**, OVL_ATTR=ff000405,
  OVL_TOPADD=40400000), DE clocks (gates/reset/TCON_MUX=0), CCU (DE_CLK=81000000, TCON0_CLK=
  80000000, DSI_DPHY_CLK=00008203). Only "differences" are HW status bits: TCON0 TRI1 high half =
  `0x00C9` (a read-only current-block counter — itself further proof TCON0 is scanning) and DSI_CTL
  reads `0x01010001` (status bits above our enable). **Conclusion: this is NOT a dropped/wrong write
  — the silicon faithfully holds our entire config, RED backdrop included, yet emits black.** The
  whole "register value is wrong / didn't stick" hypothesis class is eliminated.

**Where the pixels are lost is now binary:** either DE2 is configured-but-not-clocking-pixels-out,
or the DSI **HS data lanes** aren't physically transmitting (LP works — panel answered a DCS read —
but HS is a separate analog path). **Next decisive test (staged):** `all_pixels_on` (DCS **0x23**)
appended to the ST7703 init in `mkdata.py` — drives every panel pixel white from the panel's OWN
logic. Panel→WHITE = DSI HS video timing reaches the panel + panel works ⇒ the black is the pixel
DATA (DE2 output); panel→BLACK = no video signal reaching the panel (DSI HS / D-PHY). (REMOVE the
`([0x23],20)` diagnostic from mkdata.py after.) Build staged (`unodos_paneldbg.bin` md5 c5287a71).

**WORKFLOW BREAKTHROUGH — shuffle-free + card-free hardware iteration:**
- **XMODEM-into-RAM:** our U-Boot has `CONFIG_CMD_LOADB=y` (→ `loadx`/`loady`). A hand-rolled
  pyserial XMODEM-CRC sender (`/tmp/xload.py`, `/tmp/iload.py` on devbuntu) drives `loadx 0x40080000`,
  streams the 30 KB payload, then `dcache flush; dcache off; icache off; go 0x40080000`. The payload
  runs straight from RAM — **no card write, no card removal**. `iload.py` also auto-interrupts the 2 s
  autoboot ("Hit any key" → spam space → `=>`), so one command + one power-cycle = load+run+capture.
- **U-Boot `ums` rebuilt but NOT working yet:** rebuilt U-Boot with `USB_MUSB_SUNXI`/`USB_MUSB_GADGET`/
  `USB_FUNCTION_MASS_STORAGE`/`CMD_USB_MASS_STORAGE` (installed to the card's 8 KiB SPL region via
  `dd seek=8`, MBR+FAT preserved). `ums 0 mmc 0` is recognised but the gadget controller fails:
  `Controller uninitialized / g_dnl_register failed, error -6`. Needs more sunxi-MUSB-gadget config
  (try `CONFIG_DM_USB_GADGET=y` + `USB_MUSB_PIO_ONLY`). Moot for now — XMODEM-into-RAM is simpler and
  works, so `ums` is deprioritised. (Serial is dialout-group accessible on devbuntu → no sudo needed
  for capture; only the card-FAT flash path needs sudo.)
- **Bare-metal power-cycle:** a SHORT power press does nothing (our payload has no power-button
  handler) — must **long-press ~12 s** (AXP803 PMIC hardware force-off) then short-press to boot.
- **Serial gotcha:** the headphone-jack TRRS plug is fragile; it dropped mid-session (0 bytes even
  though the FTDI still enumerated). Reseat the plug if serial goes silent after working.

### 2026-06-21 (cont.) — THREE real panel-DCS transcription bugs found + fixed; still black; full p-boot parity reached [HW]

The §8-candidate-#2 cross-check finally went all the way: a **byte-for-byte diff of our ENTIRE
ST7703 panel-init DCS sequence against p-boot's `dsi_panel_init_seq`** (decoded from its TX-FIFO
words; script `cmp_st7703.py`). The doc's old "byte-perfect vs pinephone_lcd.c" claim was **wrong in
THREE places** — all "missing 0x00" transcription errors that shifted every following parameter:
- **SETMIPI** (0xBA): had **29** bytes, should be **28** (8 zeros between 0x20 and 0x44, not 7).
- **SETGIP1** (0xE9): had **60** bytes, should be **64** (4 trailing 0x00 missing).
- **SETGIP2** (0xEA): had **60** bytes, should be **62** (2 0x00 missing).
SETMIPI configures the panel's MIPI/DSI interface; SETGIP1/2 configure its gate-driver (row-scan)
timing — exactly the commands whose corruption yields "display-ON but black." All three FIXED in
`mkdata.py`; `cmp_st7703.py` now reports **ALL 20 commands MATCH p-boot**. Also re-derived and
checked the **DSI video-timing block** (0xB0–0xEC, BASIC_SIZE0/1, BASIC_CTL1) by computing p-boot's
values from the panel constants (HFP=30/HSYNC=28/HBP=30, VFP=18/VSA=10/VBP=17, clk=72MHz, 4-lane,
non-burst): hsa=74, hbp=84, hfp=74, hblk=2330, vblk=0, video-start-delay=1468 — **all match our
hardcoded table**. And p-boot's `display_board_init` PMIC+reset == our `pmic_init` exactly.

**RESULT: still backlit-black.** After fixing all 3 DCS bugs, on HW: panel still **alive + display-ON**
(dcs_read RXDAT=…1c, all 20 commands send), TCON0 still scanning, DSI running — but no image, and the
RED `BLD_BK_COLOR` diagnostic still never shows. Also tried (still black): **per-frame `GLB_DBUFFER`
commit** in the mainloop — the hypothesis that the DE2 mixer's double-buffered config never latched
into the active set (the register dump reads the SHADOW, not the active scanout config, so "registers
correct" doesn't prove the latch). No change.

**STATE: we have reached byte-for-byte parity with p-boot across EVERYTHING comparable** — all
DE2/TCON0/DSI/D-PHY/CCU registers (verified stuck via on-HW readback), the full ST7703 DCS sequence,
the DSI video timing, the PMIC rails, and the reset sequence — yet p-boot lights this exact panel and
we don't. The remaining difference is therefore something source/register comparison **cannot** see:
a runtime timing/sequencing nuance, the DE2-produce-vs-DSI-HS-transmit split (still not definitively
resolved — `all_pixels_on`/0x23 stayed black but the ST7703 may not honor it in video mode), or
something p-boot establishes in its **cold BROM-up boot** (DRAM/clock cascade) that the U-Boot handoff
leaves subtly different (U-Boot video already RULED OUT — disabling `CONFIG_VIDEO_DE2` changed nothing).

**THE definitive next step:** get **p-boot's ACTUAL RUNTIME register dump** and diff against ours.
p-boot has `dump_de2_registers()` / `dump_dsi_registers()` (commented out in `display_init`). Rebuild
p-boot from source (github.com/davidwed/p-boot — we only have the prebuilt `dist/` binary on devbuntu)
with those enabled, boot it (it lights the panel), capture its working DE2/TCON/DSI state over serial,
and compare value-by-value. Any difference is the bug — this is the one thing that reveals a divergence
our source-level comparison can't. (Lower-value alternates: try p-boot's exact `udelay` microsecond
timings instead of our `delay_ms`; or instrument a DE2-output-vs-DSI-HS splitter.)

**The 3 DCS fixes are real corrections** (matched p-boot AND Linux canonical) and worth keeping even
though they didn't light the panel. NEW TOOLING this session (all on devbuntu): no-video U-Boot
(`CONFIG_VIDEO` disabled, installed on the SD via `dd seek=8`); **XMODEM-into-RAM serial loader**
(`/tmp/iload.py` — interrupt 2 s autoboot, `loadx 0x40080000`, stream payload, `dcache off; go`) for
shuffle-free iteration; `cmp_st7703.py` (panel-DCS differ) at `C:\Users\arin\cmp_st7703.py`;
`pboot_display.c` saved at `C:\Users\arin\pboot_display.c` for reference.

### 2026-06-22 — p-boot runtime register dump captured + diffed; REGISTER HYPOTHESIS DEAD, bug is runtime timing/sequencing [HW]

Executed §8's "definitive next step" — but via a **better, unblocked route** than rebuilding p-boot from
source (megous git has broken TLS; p-boot also needs a stripped modified U-Boot tree). Instead we read
p-boot's **live registers from our own payload running under p-boot**: in `PBOOT` mode `panel_init` is
skipped and `fb_init` only *adopts* the framebuffer, so at handoff p-boot's DE2/TCON0/DSI/D-PHY are live and
untouched. Added an **early `[pboot-ref]` dump** (pre-`fb_init`, pbootdbg only) plus an **expanded `dump_tbl`
(35→~90 regs)**, FIFO-safe — now covering the previously-unsampled suspects: the CCU PLLs / DE divider / bus
gates+resets, the **full DSI video-timing block (0xB0–0xE4)**, `INST_FUNC0–5`, the **D-PHY analog (ANA0–4)**,
and the DE2 blender/overlay detail. Captured both states on real hardware (`pp_regdiff.py` diffs them):
**p-boot card** (rebuilt with the pbootdbg Image) → `pboot-regs.log`; **native U-Boot card** → `native-regs.log`.

**Result: 78 / 89 registers identical.** The 11 differences are all benign:
- 8 **expected** — framebuffer geometry/address (native `027f01df`/`40400000`/pitch `780` vs p-boot
  `059f02cf`/`48000000`/`b40`), the left-in RED `BLD_BK_COLOR` diagnostic, and volatile status/counters
  (`TCON0_GINT0`, the `TRI1` block counter).
- 3 "suspect", all identified (via Linux `ccu-sun50i-a64.c`) as **non-display red herrings**:
  `0x064`/`0x2c4` **bit 21 = `bus-msgbox`** (p-boot enables it for the ATF/SCP CPU↔AR100 mailbox),
  `0x2c0` **bit 9 = `RST_BUS_MMC1`** (native has MMC1 out of reset because U-Boot loaded the payload from SD).

Also **killed the `INST_FUNC0` lead** (LANE_CEN bit 4 reads `0x0F` on native too — `dsi_start`'s clear *does*
stick once the engine is live) and **verified offline** the 7 un-dumped DSI instruction-engine registers
(`INST_LOOP_SEL=0x30000002`, `INST_LOOP_NUM0/1=0x00310031` for non-burst `delay=49`, `INST_JUMP_CFG=0x00560001`)
all match p-boot's source.

**Conclusion:** every display-path register — CCU display clocks/PLLs, DE gates, the DE2 mixer/blender/overlay,
TCON0 (all timing), the DSI host (CTL, BASIC, **all** INST + LOOP + JUMP, the full video SYNC/BLK timing block,
PIXEL), and the D-PHY (GCTL, TX timing, **all ANA**) — is **byte-identical between working-p-boot and
broken-native**. Identical static config + alive panel + scanning TCON0 + running DSI + enabled DE2, yet no
pixels (not even the RED backdrop). **So the divergence is NOT a register value — it is runtime
timing/sequencing**, the one thing register snapshots cannot capture.

**NEXT (recommended):** port p-boot's **exact `dsi_init` sequence** — its `udelay` *microsecond* timings (we use
coarse `delay_ms`) and its operation **order** (p-boot `display_init` = `tcon0_init` → `dsi_init` → `de2_init`,
then `display_commit`; we interleave differently and call `fb_init`/DE2 separately after a 160 ms delay). p-boot
source is now on devbuntu at `~/p-boot-src` (its `src/display.c` is ground truth; the build toolchain
gcc-aarch64-linux-gnu + php + ninja is installed, still missing only the modified U-Boot at `src/uboot`).
Lower-value: dump the few non-canonical sub-registers not yet sampled.

**Tooling note:** the devbuntu udev write-blocker re-arms RO mid-write (it bit `p-boot-conf`); the card-build
scripts (`~/pboot/build-pboot-card.sh`, `~/pine-uboot/make-native-card.sh`) now temporarily disable
`/etc/udev/rules.d/99-usb-storage-readonly.rules` with a trap that guarantees restore + re-arm.

**Confirmed by a second, fuller sweep (`dump_all`).** Added `dump_range` (walks a whole contiguous config
block, catching even unnamed/reserved sub-registers) and swept each block end-to-end — DE-top, DE2 global +
blender (`0x1000–0x10A0`, all pipes) + UI overlay, the full DSI host (`0x00–0xEC`), the full D-PHY
(`0x00–0x60`) — plus a curated TCON0+CCU list. Re-captured both cards (`pboot-regs2.log` / `native-regs2.log`,
~183 regs each): **168 match.** The only *additional* diffs the wider sweep found are DE2 blender **pipe 1**
(`FCOLOR 0x1101014` / `INSIZE 0x1101018` / `COORD 0x110101c` / `MODE 0x1101094`) — p-boot composites its GUI
with **two planes**, we use one — but **pipe 1 is disabled in both** (`PIPE_CTL 0x1101000 = 0x101`, bit 9
clear), so it's inert; our single UI layer routes channel 1 → pipe 0, which is byte-identical. Everything else
is the same FB-geometry / RED-diagnostic / volatile / MSGBOX / MMC1 set. **Net: zero display-relevant register
difference across the entire sweep — the register hypothesis is exhaustively closed.**

### 2026-06-22 (cont.) — bring-up REORDERED to p-boot's exact `display_init()` operation order [code-only, awaiting HW]

With the register *values* proven byte-identical to working p-boot, the only residual a snapshot cannot
capture is the **runtime bring-up ORDER**. p-boot's `display_init()` (ground truth, `src/display.c`, mirror at
`C:\Users\arin\pboot_display.c`) brings each block's clocks up **immediately before using that block**:

```
tcon0_init()    : PLL_VIDEO0 → PLL_MIPI(LDO→udelay(100)→full) → TCON0 src clk → LCD0 gate/reset → TCON0 regs
dsi_init_fast() : DSI bus gate/reset → DSI host regs → display_board_init(reset-low+PMIC+15ms)
                  → dphy_enable(CCU_MIPI_DSI_CLK first → D-PHY regs) → reset-high+15ms → panel DCS → HSC→1ms→HSD
de2_init()      : SRAM-C→DE → PLL_DE → DE clock → DE gate/reset → clear mixer → enable mixer   (LAST)
display_commit(): blender + UI layer + dbuff=1                                                  (per frame)
```

We had **front-loaded every clock** in one `clk_init` (plus an assert→deassert reset-cycle p-boot never does),
then run the blocks. Same end-state registers, different bring-up order. **Refactored to match p-boot exactly:**
`clk_init` is split into four per-block helpers — `tcon0_clk_init`, `dsi_bus_clk_on`, `dphy_clk_on`, `de_clk_init`
— each called at the p-boot-equivalent point in `panel_init` (so PLL_DE/DE clock now come up **last**, after the
DSI is in HS video mode, right before `fb_init`'s mixer config, exactly as `de2_init` runs after `sun6i_dsi_start(HSD)`).
Register **values are unchanged** (the snapshot-verified set); only *when* each clock comes up changed, and resets
are now **deassert-only** (the assert-cycle — our old "environmental" guess, which never lit it — is dropped, so
this is a clean test of *exact p-boot order*). All status polls (PLL_DE lock @`0x48`, RSB, DSI INSTRU_EN) keep the
same addresses, so the harness is unaffected. **Regression green: default + paneldbg both render the byte-identical
4922-B launcher PNG end-to-end (no fault/hang).** Built `unodos_paneldbg.bin` (30704 B, md5 `07f0d035…`) → staged
`devbuntu:~/pine-uboot/unodos_paneldbg.bin`. **NEXT (needs user + phone):** native U-Boot card
(`~/pine-uboot/make-native-card.sh`), serial bg-cat, power-cycle; read whether the panel now lights. If still
backlit-black, "exact p-boot order" is disproven too → next wedge is a **DE2-output-vs-DSI-HS-transmit splitter**
(does the DE2 mixer emit pixels at all, vs do the HS lanes carry them) or the handful of un-sampled sub-registers.

**HW RESULT (2026-06-22): STILL BACKLIT-BLACK. "Exact p-boot order" is DISPROVEN.** Native card, full serial
trace: LED beacon walked all stages clean; `SCTLR=30c50830` (MMU/caches off); **all three PLLs locked**
(`PLL_VIDEO0=91006207`, `PLL_MIPI=90c0071a`, and crucially `PLL_DE=91001701` — locks fine even brought up LAST);
`[dsi_host]/[pmic]/[dphy]/[st7703] ok`; panel ALIVE+display-ON (`RXCTL=02060003` RX_FLAG set, `RXDAT=…011c`
power-mode 0x1c); `DSI_BASIC_CTL0=00030001` (HS video); `DE2_GLB_CTL=1`, `OVL_TOPADD=40400000`; `TCON0_GINT0`
`a00→800→800` (bit11 re-sets = TCON0 scanning); `[unodos] mainloop`, no fault. **Identical end-state to every prior
run — the reorder changed nothing.** So with register VALUES *and* operation ORDER *and* the DCS init seq all
byte-identical to the p-boot that lights this exact panel, and the panel proven alive, the residual is provably
NOT in the DE2/TCON0/DSI/DPHY/CCU register+sequence space at all.

**KEY REFRAME (important): ST7703/XBD599 is a VIDEO-MODE panel with NO GRAM** — it has no framebuffer of its own and
displays the incoming MIPI video stream in real time. So "display-ON + backlit-black" means **no valid RED video is
arriving at the panel's MIPI input** (this also explains why the earlier `all_pixels_on` 0x23 test stayed black:
with no GRAM there's nothing to "turn on"; the panel only ever shows the live stream). The RED blender backdrop is
generated *inside* the DE2 pipe (no DRAM/MBUS fetch needed), so its absence rules OUT a framebuffer-DMA/MBUS cause
and narrows the wall to: **DE2 mixer not emitting pixels at all, OR the DSI HS lane/D-PHY analog not transmitting
them** (LP works — panel answered a DCS read — but HS is a separate analog path).

**NEXT — the DIFFERENTIAL PERTURBATION test (genuinely new; supersedes static register diffing).** Static
comparison is exhausted. Use p-boot's *live working* display as the reference substrate and perturb ONE block at a
time with OUR code: boot under PBOOT (p-boot lights the panel), then re-run exactly one of our blocks on top and see
if the screen goes black. First + highest-value instance: **PBOOT + re-run our native `fb_init` DE2 programming**
(GLB/blender/RED-backdrop/UI-layer/dbuff) on top of p-boot's known-good TCON0/DSI/DPHY/clocks. If the panel goes
black → OUR DE2 mixer config is the bug. If it shows RED / our launcher → our DE2 config is CORRECT and the bug is
in our TCON0/DSI/DPHY/clock bring-up (p-boot's versions work; ours are subtly wrong in a way registers don't show —
a clock at the wrong *rate* despite the right register bits, or an analog enable that doesn't actually engage).
That one cycle bisects the entire suspect space. Code NOT committed (`panel.inc.s` clock refactor + `panel_init`
reorder, `kernel.s` comment, this entry).

**HW RESULT (2026-06-22): ✅ DE2 EXONERATED — the differential test worked.** `build.sh de2test`
(PBOOT+PANELDBG+DE2TEST; `fb_init` wraps the adopt path in `.ifndef DE2TEST` so it falls through to native DE2 over
p-boot's live pipe). On HW: p-boot lit the panel → our payload → **a white rectangle top-left, then the UnoDOS
launcher rendered.** Trace corroborates: our overlay is live (`01103000=ff000405` ATTR, `01103004=027f01df` =
480×640, `01103010=40400000` = **our** FB, not p-boot's `0x48000000`) scanning out through p-boot's TCON0
(`GINT0 a00→800`) and DSI (`BASIC_CTL0=00030001`). **So our native DE2 mixer config is CORRECT when fed p-boot's
known-good TCON0/DSI/D-PHY/clocks.** The native-only black is therefore **upstream of DE2**: our `panel_init`
{clocks, TCON0, DSI-host, D-PHY, dsi_start} (ST7703/panel already exonerated — it answers DCS and is alive). Leading
theory: a wrong clock *rate* despite identical register bits, or a D-PHY HS analog enable that doesn't actually
engage (LP works, HS is a separate analog path).

**NEXT (staged, awaiting HW): `build.sh pbootnative` (PBOOT+PANELDBG+DE2TEST+NATIVEPANEL).** Re-runs our FULL native
`panel_init` on top of p-boot's warm state, then native DE2. The big fork: **launcher shows** → our `panel_init`
code is functionally correct and the native-cold failure is **environmental** (U-Boot's clock/DRAM cascade vs
p-boot's SPL) → pivot to comparing the *non-display* clock tree, stop perturbing `panel.inc.s`; **black** → our
`panel_init` actively breaks a working pipe → bisect its blocks (clocks → +TCON0 → +DSI/D-PHY). Tooling gotcha hit +
recorded: `build-pboot-card.sh`'s per-write `blockdev --setrw` **races** udev re-arming RO after the `sfdisk`
re-partition (`p-boot-conf` → "Operation not permitted"); robust fix = `sudo mv` the `99-usb-storage-readonly.rules`
aside + `udevadm control --reload` for the whole card build, then restore (the dd-only `make-native-card.sh` path
doesn't trip it).

**HW RESULT (2026-06-23): `pbootnative` = BLACK, then `pbootdsi` = BLACK → bug PINNED to our DSI link bring-up.**
`pbootnative` (re-run full `panel_init` on p-boot's warm state) went black → the bug is in our `panel_init` *code*,
not the environment (this also rules out PLL_PERIPH0/MBUS/DRAM — they were in p-boot's live state and our bring-up
still failed). Then `build.sh pbootdsi` (PBOOT+PANELDBG+DE2TEST+NATIVEPANEL+DSIONLY → kernel.s calls the new
`panel_dsi_only`, which re-inits ONLY the DSI link — `dsi_host_init` + `dphy_init` + panel reset/DCS + `dsi_start` —
on top of p-boot's clocks AND TCON0) ALSO went black. Trace: panel STILL answers DCS after our re-init
(`RXCTL=02060003`, `RXDAT=…011c` = display-ON), `DSI_BASIC_CTL0=00030001` (HS), p-boot's `TCON0 GINT0 a00→800` still
scanning — yet black. **The discriminator is exact:** `de2test` (p-boot's DSI + our DE2) = launcher; `pbootdsi`
(OUR DSI + our DE2) = black; the ONLY delta is our DSI link re-init. **So the bug is in our DSI-host / D-PHY /
dsi_start — the HS video path.** LP signalling works (panel replies DCS); HS video delivers no pixels = a classic
HS-transmit failure. Registers in those routines are all verified byte-identical to p-boot, so it is a *runtime /
analog* difference, not a value. (Caveat: re-running `dphy_init` on p-boot's already-powered D-PHY could be partly a
re-power artifact — `dphy_tbl` writes `ANA2=0x2`, clearing CK_CPU/P2S_CPU, then rebuilds — but cold-native is also
black and the DSI link is the consistent discriminator, so it remains the strong lead.)

**NEXT (next session):** cleanest sub-bisect — re-run **only `dsi_start`** (the HS handoff: JUMP_SEL HSC `0x00000F02`
→ clear `INST_FUNC0` LANE_CEN bit4 → 1 ms → JUMP_SEL HSD `0x63F07006`) on p-boot's fully-working DSI; this is
low-artifact (no panel reset, no D-PHY re-power). Black → our `dsi_start` HS handoff is the bug; survives → it's
`dsi_host_init` or `dphy_init`. Then scrutinize the HS **clock-lane continuous mode** (D-PHY `TX_CTL` HS_TX_CLK_CONT
bit28 + `dsi_start`'s LANE_CEN clear) — the HS-clock-lane control is the likeliest runtime culprit. Bisection tooling
left in tree: `build.sh {de2test,pbootnative,pbootdsi}`, flags DE2TEST/NATIVEPANEL/DSIONLY in `kernel.s`,
`panel_dsi_only` in `panel.inc.s`. All code still uncommitted on `parity-push-fresh-ports`.

**HW RESULTS (2026-06-23) — full bisection of the DSI link → bug is our PANEL RESET + `st7703_init`, NOT dphy.**
Sub-tests (each: re-run only the named block(s) on p-boot's live pipe, with native DE2):

| build | block(s) re-run by us | screen |
|---|---|---|
| `de2test` | DE2 | launcher ✓ |
| `pbootdsistart` | `dsi_start` | launcher ✓ |
| `pbootdsihost` | `dsi_host_init` + `dsi_start` | launcher ✓ |
| `pbootnodphy` | `dsi_host` + **panel reset + `st7703_init`** + `dsi_start` (p-boot's D-PHY) | **black** |
| `pbootdsi` | full DSI link incl dphy | black |

The clean pair `pbootdsihost` (lit) vs `pbootnodphy` (black) differ by **exactly `panel_reset_low/high` + `st7703_init`**.
So **our panel reset + ST7703 DCS init is the bug** — `dphy_init` is exonerated (removing it didn't help). The earlier
"panel is alive/exonerated" reasoning was wrong: the panel *answers* DCS (`RXDAT=…011c`, display-ON) even after our
re-init, but the XBD599 is **video-mode (no GRAM)**, so a subtly-wrong SETMIPI / SETGIP gate-driver / interface config
leaves it display-ON yet black. This also explains native cold boot, which *must* reset+init the panel itself.

**NEXT (next session) — re-scrutinize the panel init with fresh eyes** (data was "byte-verified" before, so look past the
bytes): (1) exact **reset pulse** timing/width vs p-boot (`PD23` low → 15 ms → [dphy] → high → 15 ms → DCS); (2) **delay
placement** — the post-SLPOUT 120 ms and any per-command waits, relative to p-boot's `panel_dcs_seq_initlist` (21 entries
= 20 DCS + 1 sleep); (3) **`dcs_send` framing/mode** for the LONG packets (SETGIP1 = 64 B, SETGIP2 = 62 B, SETGAMMA);
(4) any command/order difference. Can't cheaply split reset-vs-st7703 on HW (reset-alone leaves the panel blank).
Bisection flags now also DSISTARTONLY/DSIHOSTONLY/NODPHY (`panel_{dsistart,dsihost,nodphy}_only`). Default regression
still byte-identical 4922-B.

**HW RESULTS (2026-06-23, cont.) — `st7703` and `dphy` both EXONERATED; bug down to our clocks or TCON0.** Two more
low-artifact cuts: `pbootst7703` (re-send our DCS on p-boot's lit panel **without** a reset) = **launcher** → our ST7703
data *and* transmission are correct, and the earlier `pbootnodphy` black was the **panel-reset perturbation artifact**
(confirmed: `cmp_st7703.py` shows all 20 commands byte-identical to p-boot; framing/order/delays/FIFO-content all match).
`pbootdphy` (our `dphy_clk_on`+`dphy_init`+`dsi_start` on p-boot's everything, no reset) = **launcher** → our D-PHY analog
bring-up is correct even with the 1 ms delays. **Five blocks now cleanly proven correct: DE2, dsi_host_init, dsi_start,
st7703, dphy_init.** Every working test ran on **p-boot's clocks + TCON0**; native runs **ours** — so the bug is in our
**clock setup or TCON0**, plus possibly panel-reset+cold-init (the one block never cleanly isolated, though our reset+PMIC
order matches p-boot exactly). All of clocks/TCON0 are register-verified identical to p-boot and *functioning* (PLLs lock,
TCON0 scans), so the remaining fault is a subtle **rate / sequencing** issue a snapshot can't capture.

**NEXT (next session):** (a) a clk+TCON warm cut — reprogram our clocks + `tcon0_init` on p-boot's DSI/dphy/panel
(launcher → clocks/TCON0 fine, bug is panel-cold-reset; black → clocks/TCON0, modulo live-reprogram disruption); (b) rate
analysis of PLL_VIDEO0 → PLL_MIPI → TCON0 DCLK vs p-boot (registers match, so hunt a derived-rate or lock-sequence
subtlety); (c) cold-native A/B of candidate clock/TCON0 tweaks. Full bisection-flag set
(DE2TEST/NATIVEPANEL/DSIONLY/DSISTARTONLY/DSIHOSTONLY/NODPHY/ST7703ONLY/DPHYONLY) + `panel_*_only` routines in tree.
~9 HW cycles; default regression byte-identical throughout.

### NEW STRATEGY (the warm-perturbation method is exhausted for the last mile)

The differential-perturbation method (re-run one of our blocks on p-boot's live pipe) narrowed the bug from the whole
pipeline to **our clocks or TCON0** — but it *cannot* cleanly isolate those two: reprogramming a live PLL or the timing
master mid-scanout glitches the pipe regardless of correctness, so any black result is ambiguous. And every clock/TCON0
register is already byte-identical to p-boot. So the next phase needs a different lever — **measure behaviour, not
registers**, and **widen the comparison beyond the display block**:

1. **On-chip rate measurement (the key new idea).** The ARM generic timer (`cntpct_el0`) is a fixed 24 MHz reference.
   Time the **TCON0 frame period** precisely against it — count `cntpct` ticks between successive `GINT0` bit11 (vblank)
   latches — in BOTH native and PBOOT(=p-boot) mode, and compare. This directly tests the "rate differs despite matching
   registers" hypothesis with NO disruption. If native's frame period ≠ p-boot's, a clock divider/PLL is at the wrong
   *rate* (trace PLL_VIDEO0→PLL_MIPI→TCON0 DCLK); if they're identical, the bug is **not** a clock rate → redirect to
   TCON0's video/CPU-IF logic or a DSI↔TCON0 sync detail. (Can also gate other observable clocks the same way.)
2. **Full CCU sweep.** `dump_all` compares only a *curated* CCU list. Dump the COMPLETE CCU register space
   (`0x01C20000`–`0x01C202FF`, every PLL/gate/divider/mux) p-boot vs native and diff — catch a clock register we never
   compared (a sub-block gate, a source mux, PLL hidden bits) that diverges.
3. **Recompute the derived timing values.** p-boot *computes* `sun6i_dsi_get_video_start_delay()`, the TCON0
   `start_delay`, and `BASIC_CTL1` from the panel constants at runtime; we hardcoded them. Re-derive p-boot's formulas for
   the XBD599 numbers and confirm our literals match exactly — a one-off math slip here would pass the register dump (we'd
   be comparing our wrong value to our wrong value) yet desync the video.
4. **Bigger pivot if 1–3 are inconclusive:** rebuild p-boot from source with its `dump_de2/dsi_registers()` +
   clock-state dumps enabled to capture its EXACT runtime clock tree (the original blocked plan — megous TLS was dead;
   retry via a mirror, or lift the clock setup from a known-good pmOS/Tow-Boot build). Compare its live PLL/divider
   readbacks against ours value-by-value.

Priority: **#1 then #2** — both are cheap (extend the existing PANELDBG dump/measure code), non-disruptive, and directly
attack the clocks-vs-TCON0 fork that perturbation can't.

### 2026-06-25 — strategy #3 DONE (computed-timing eliminated); #1+#2 diagnostics IMPLEMENTED + staged [code-only, awaiting HW]

Executed the new strategy. **#3 (re-derive the computed timing values) is COMPLETE and ELIMINATED** —
done analytically, no hardware needed. p-boot *computes* four timing values from the XBD599 panel
constants at runtime (`src/display.c`, mirror `C:\Users\arin\pboot_display.c`); we hardcoded them, so a
one-off math slip would pass every register dump (wrong-vs-wrong) yet desync the video. Re-derived each
of p-boot's formulas for the panel numbers (720×1440; HTOT=808, VTOT=1485, pixel clock 72000 kHz,
4 lanes, 24bpp, **PANEL_BURST=0** → the burst/DRQ-edge/line_num path is dead code, never written) and
confirmed **every one is byte-identical to our literal**:

| value | p-boot formula | computed | our literal |
|---|---|---|---|
| DSI `BASIC_CTL1` video_start_delay | `VTOT-(VSS-VDISP)+1 = 1468`, `<<4 \| 0x7` | `0x00005BC7` | `0x00005BC7` ✓ |
| TCON0 `TRI2` start_delay | `(VTOT-VDISP-11)*HTOT*149/(CLK/1000)/8 = 7106` | `0x1BC2000A` | `0x1BC2000A` ✓ |
| DSI `TCON_DRQ` | burst=0,(HSS-HDISP)>20 → `((30-20)*24/32)=7 \| EN` | `0x10000007` | `0x10000007` ✓ |
| DSI `INST_LOOP_NUM0/1` | burst=0 → N0=N1=49 | `0x00310031` | `0x00310031` ✓ |
| DSI `INST_LOOP_SEL` | `2<<(4·LP11) \| 3<<(4·DLY)` | `0x30000002` | `0x30000002` ✓ |

So the **derived constants are not the bug** — the whole "computed-value math slip" hypothesis class is
out. (Cross-check arithmetic in the commit; the chained integer divisions on start_delay were the
error-prone one — 4,093,328 / 72 / 8 = 7106, exact.)

**Bonus — an ABSOLUTE prediction for #1.** The same constants give the *expected* TCON0 frame period:
HTOT·VTOT / pixel_clock = 808·1485 / 72 MHz = **60.006 Hz** → at the 24 MHz cntpct reference,
**≈399,960 ticks/frame (0x61A58)**, ≈23,997,600 over 60 frames. So #1 now has a target, not just a
native-vs-pboot diff: native reads ≈400k ⇒ DCLK rate is **correct** ⇒ redirect to TCON0 video/CPU-IF
logic or DSI↔TCON0 sync (NOT a rate bug); native reads materially different ⇒ a divider/PLL is at the
wrong rate (chase PLL_VIDEO0→PLL_MIPI→TCON0 DCLK ÷6).

**#1 + #2 IMPLEMENTED (PANELDBG, harness-safe):**
- **#1 `measure_frame_period`** (`panel.inc.s`): `wait_frame_edge` spins on TCON0 `GINT0` bit11
  (frame-done latch), clears it, bounded ~50 ms by cntpct so a stalled TCON0 can't hang it; the driver
  times 60 frames against `cntpct_el0` and prints `FRAME_TICKS_60` + `FRAME_TICKS_1` (`udiv`/60), or
  `FRAME_PERIOD=STALL`. Wired into the mainloop PANELDBG block (`kernel.s`) right after the GINT0 scan.
  Runs in BOTH native (`paneldbg` = our TCON0) and PBOOT (`pbootdbg` = p-boot's TCON0) — compare the two,
  and both against the 399,960 prediction.
- **#2 `dump_ccu_full`** (`panel.inc.s`): blind-sweeps the COMPLETE CCU `0x01C20000–0x01C202FF` (192
  words, no read side effects) instead of the curated `dump_tbl` subset; appended to `dump_all` so it
  appears in both the `pbootdbg` pboot-ref dump (p-boot's CCU) and the `paneldbg` mainloop dump (ours).
  Offline-diff the two to catch a clock register we never compared.

**Verified:** all builds assemble clean (`paneldbg` 31744 B, `pbootdbg` 31752 B, default 26088 B); the
**default regression renders the byte-identical 4922-B launcher** (`cmp` vs `shots/m1_boot.png`) and
`paneldbg` runs end-to-end in the Unicorn harness with no hang/fault (TCON0 reads 0 in the sink → the
new measure routine hits its bounded timeout once and prints STALL, exactly as designed; UART TX is a
harness no-op so the trace is hardware-only). Diagnostics are PANELDBG-only and don't touch the render
path.

**NEXT (needs user + phone + serial rig):** flash `unodos_paneldbg.bin` (native U-Boot card) AND
`unodos_pbootdbg.bin` (p-boot card) — or XMODEM-into-RAM both via `iload.py` — and read the trace:
(a) the `[frameperiod]` `FRAME_TICKS_1` in each (native vs p-boot vs the 399,960 prediction) → tells us
rate-vs-not; (b) diff the two `[ccu-full]` sweeps for any divergent CCU register. That binary outcome
picks the final wedge: clock-rate fault (chase the divider) vs TCON0-video/DSI-sync logic (a behaviour a
snapshot can't see).

### 2026-06-25 (cont.) — HW RESULTS: rate IDENTICAL + full CCU clean → clock hypothesis DEAD; bug is non-register [HW]

Ran both builds on hardware (native U-Boot card `paneldbg`, then p-boot card `pbootdbg`; serial via the
`at`-detached capture below). Traces saved: `shots/trace-native-frameperiod.log`,
`shots/trace-pboot-frameperiod.log`.

**#1 — frame rate is IDENTICAL (the decisive result):**
- native `FRAME_TICKS_1 = 0x6095E = 395,614` ticks/frame → **60.665 Hz**
- p-boot `FRAME_TICKS_1 = 0x60603 = 394,755` ticks/frame → **60.797 Hz**
- Δ = 859 ticks over 60 frames ≈ 14 ticks/frame = **0.2 %** — pure measurement jitter (the two runs enter
  `measure_frame_period` at different GINT0 phases after the 3×50 ms `tcon_scan`). The TCON0 frame period
  is the same in the failing-native and the working-p-boot pipe. **The "wrong clock rate despite matching
  register bits" hypothesis is ELIMINATED.** (The naive 399,960 video-mode prediction was ~1 % off both —
  expected, since the panel runs TCON0 in 8080/TRI mode, not plain video timing; the native-vs-p-boot
  equality is the valid test, and they match.)

**#2 — full CCU sweep + every swept block, native vs p-boot pristine (`[pboot-ref]`): display tree IDENTICAL.**
Comprehensive `reg=val` diff over all dumped ranges (362 shared registers):

| block | diffs | verdict |
|---|---|---|
| DSI (full 0x00–0xEC) | 0 | byte-identical |
| D-PHY (full 0x00–0x60) | 0 | byte-identical |
| DE-top (gates/reset/mux) | 0 | byte-identical |
| TCON0 (curated config) | 0 config | the 2 "diffs" are live status — `GINT0` frame-latch + `TRI1` hi-half running block-counter |
| CCU (full 192 words) | 9 | **all non-display:** CPU PLL (0x00), **disabled** PLL_PERIPH1 (0x50, bit31=0), MMC0/1/2 gates+clocks+resets (0x60/0x64/0x8c/0x90/0x2c0/0x2c4 — U-Boot uses MMC1, p-boot doesn't), one PLL bias reg (0x250). Confirmed from p-boot's own `display.c`: its display init touches only the TCON0 (bit3) + DE (bit12) gates, which native ALSO has set. |
| DE2 (glb/blender/UI) | 9 | expected — our 480×640 RED-backdrop UI vs p-boot's 720×1440 splash |

**CONCLUSION — the entire clock/register hypothesis class is dead.** Across the lit-p-boot and the
black-native boots, **every display-path register (DSI, D-PHY, DE-top, TCON0 config, all display CCU
clocks/gates/dividers/muxes) is byte-identical, and the measured TCON0 frame rate is identical.** Plus #3
(computed timing) already matched. The perturbation method had narrowed the bug to "our clocks or TCON0";
we have now proven our clocks and our TCON0 config + rate are indistinguishable from the working p-boot's.
So the fault is **NOT a register value, NOT a clock rate, NOT a derived-timing slip, NOT an unsampled CCU
clock.** It is **non-register state** — something a snapshot cannot hold.

**WHAT'S LEFT (next experiments, in priority):**
1. **Full TCON0 sweep** — TCON0 is the ONE block we only dump *curated* (15 regs; we skip 0x64–0x6c CPU-IF
   data ports). Sweep the rest (e.g. 0x00–0x60, 0x70–0x158, 0x160–0x230, avoiding the data ports) native
   vs p-boot. If an UNDUMPED TCON0 register (BASIC1/2/3 @0x4c/0x50/0x54, HV/CTL sub-fields, FILL, gamma…)
   differs, that's the bug. This is the last register surface never fully compared.
2. **Dump the AXP803 PMIC rails** (DLDO1/DLDO2/GPIO0LDO voltages, over RSB) native vs p-boot — panel power
   (DLDO2) at a subtly different voltage would light the backlight yet leave the panel unable to lock the
   HS video stream, and it's invisible to every MMIO dump we've done.
3. If both match: the residual is **analog/calibration state** the U-Boot `go` handoff leaves differently
   than p-boot's cold SPL — most likely **D-PHY HS-lane calibration/bias** (LP signalling works — panel
   answers DCS — but the HS data lanes are a separate analog path that p-boot's cold bring-up may train
   differently). At that point the wedge is to capture p-boot's D-PHY ANA* state immediately post-`dsi_start`
   and compare, or to add an explicit D-PHY calibration step.

**Workflow note (LAN robustness):** the amanuensis→devbuntu SSH link drops frequently mid-command (exit
255), killing any foreground or `&`-backgrounded capture and even truncating multi-step commands. The
robust pattern that worked: write a self-contained script on devbuntu, launch it with **`at -f <script> now`**
(atd fully decouples it from the SSH session), and poll a marker file / the log with short separate SSH
calls. Used this for both the serial capture (`/tmp/cap*.sh` → `/tmp/pine-*.log`) and the p-boot card build
(`/tmp/mk-pboot.sh`, which also mv's the udev write-blocker rule aside for the whole build then restores it
via trap — the per-write `blockdev --setrw` races udev re-arming RO after `sfdisk`).

### 2026-06-25 (cont.) — full TCON0 sweep = IDENTICAL; EVERY display register now matches → bug is non-register/analog/sequencing [HW]

Added `dump_tcon0_full` (sweeps TCON0 0x00–0x60 + 0x70–0x230, skipping the CPU-IF data ports
0x64/0x6c) and ran it on both cards. Traces: `shots/trace-native-tcon0full.log`,
`shots/trace-pboot-tcon0full.log`. **Full TCON0 diff (138 regs each): 2 differences, BOTH live status**
— `GINT0` (frame-done latch, sampled mid-flight) and `TRI1` hi-half (the running block counter). **Every
TCON0 config register is byte-identical.**

**So every register in the entire display pipeline is now proven identical native(black) vs p-boot(lit):**
DSI (full 0x00–0xEC) ✓, D-PHY (full) ✓, DE-top ✓, DE2 ✓ (modulo our UI content), **TCON0 (FULL 138-reg
sweep) ✓**, CCU (full 192) ✓ (only non-display CPU/MMC/bias diffs), frame rate ✓ (0.2 %), computed timing
✓. There is **no register surface left to compare** — the snapshot hypothesis class is fully exhausted.

Also re-audited p-boot's `dphy_enable` for a non-register step we might omit: it has **no lock/status poll**
(pure register writes + `udelay(5)` then `udelay(1)×4` between the ANA2/ANA3/ANA1 analog-LDO enables). Our
`dphy_init` matches its values + order; our only delta is `delay_ms(1)` where p-boot uses `udelay(1–5)` —
i.e. ~1000× *longer* settling, which is safe (not too short). So D-PHY is faithful, poll included (there is
none).

**CONCLUSION (hardened): the fault is NOT in any MMIO register, clock rate, or bring-up value.** Recall the
perturbation result that `pbootnative` (re-running our FULL `panel_init` on p-boot's warm, lit pipe) goes
**black**, while every *individual* block of ours re-run on p-boot's pipe stays lit. So the bug is something
our **full clocks+TCON0 bring-up does as a sequence** — a transient or ordering effect, or a non-MMIO analog
state — that corrupts the pipe, and it is invisible to any register dump (every end-state register matches).

**WHAT'S LEFT (register dumping is done — these are behavioural/analog probes):**
1. **The clk+TCON0 warm cut** (the one perturbation never cleanly done): re-run ONLY our `*_clk_init` +
   `tcon0_init` on p-boot's otherwise-live DSI/D-PHY/panel. Black → our clock/TCON0 *bring-up sequence*
   disrupts a working pipe (transient/ordering — chase what our PLL_MIPI re-program or TCON0 enable does to
   the already-running DSI/DE); survives → clocks/TCON0 fine, isolate the panel-cold-reset path. (Caveat:
   live-reprogramming the timing master self-disturbs — a black is ambiguous, but survival is decisive.)
2. **AXP803 PMIC rail dump** (DLDO1/DLDO2=panel power/GPIO0LDO, over RSB) native vs p-boot — the one
   subsystem with NO register dump yet; a panel-rail voltage delta lights the backlight but can stop the
   panel locking the HS stream, invisible to all MMIO.
3. **D-PHY HS-lane analog from cold**: every "works" test re-ran our D-PHY on p-boot's *already-trained* PHY
   (proves we don't BREAK a locked link, not that we LOCK from cold). Probe the D-PHY HS state right after
   `dsi_start` cold vs the warm case.

### 2026-06-25 (cont.) — probes #1+#2 run: clk+TCON0 SAFE, PMIC identical → bug RELOCATED to the cold panel-reset+ST7703-init [HW]

Ran the two behavioural probes on hardware.

**Probe #1 — clk+TCON0 WARM CUT = LIT (`pbootclktcon0`).** Re-ran ONLY our clock setup
(`tcon0_clk_init` incl. the PLL_MIPI LDO→settle→full re-lock, `dsi_bus_clk_on`, `dphy_clk_on`,
`de_clk_init`) + `tcon0_init` on p-boot's live, lit DSI/D-PHY/panel pipe → **panel stayed lit, UnoDOS
launcher rendered.** So our clock + TCON0 bring-up sequence does NOT disrupt a working pipe — it even
re-locks PLL_MIPI mid-scanout without breaking the DSI/panel. **Our clocks + TCON0 are exonerated** (the
last block group the warm-perturbation method could cleanly test). Survival is decisive here (the caveat
was that a *black* would be ambiguous due to live-reprogram self-disturbance; LIT has no such caveat).

**Probe #2 — AXP803 PMIC rails = IDENTICAL.** native vs p-boot (`dump_pmic`, over RSB):

| rail | native | p-boot |
|---|---|---|
| 03 chip-id | 0x51 | 0x51 |
| 00 pwr-src | 0xf7 | 0xf7 |
| 12 LDO on/off | 0xf9 | 0x19 (only bits 5-7 = non-display DLDO3/4 that U-Boot leaves on; **both have DLDO1 bit3 + DLDO2 bit4**) |
| 15 DLDO1 V | 0x1a (3.3V) | 0x1a |
| **16 DLDO2 V** (MIPI/panel) | **0x0b (1.8V)** | **0x0b** |
| 90 GPIO0 mode | 0x03 | 0x03 |
| 91 GPIO0LDO V | 0x1a | 0x1a |

The panel/MIPI power rail (DLDO2) is the **same voltage and enabled** in both. **PMIC exonerated.**

**THE BUG IS NOW PINNED (by total elimination) to the COLD panel-reset + ST7703 cold-init sequence.**
Everything else is proven identical or safe: all display registers (DSI/DPHY/DE-top/DE2/TCON0-full/CCU-full),
frame rate, computed timing, PMIC rails, and now clocks+TCON0. **Every individual block of ours, warm-tested
on p-boot's pipe, lights the panel** — INCLUDING clk+TCON0. The ONLY operation the warm method *cannot*
cleanly test is the **panel GPIO reset + ST7703 init from a cold/reset state**, because resetting p-boot's
live, scanning panel is inherently destructive (this is why `pbootnative`/`pbootnodphy` go black — a
reset-perturbation artifact, NOT a clk/TCON0/dphy fault). `pbootst7703` only ever proved our DCS *data +
transmission* are correct when re-sent to an **already-initialised, running** panel (and `cmp_st7703.py`
confirms all 20 DCS commands are byte-identical to p-boot) — it never exercised the panel transitioning
**reset → configured**. That transition is the last untested thing, and cold-native (which must do it) is
the only boot that fails.

**Since the DCS command DATA matches p-boot byte-for-byte, the suspect is NOT the command bytes but the
cold-init TIMING / SEQUENCING around them:** (1) the **reset pulse** width + the reset-low→dphy→reset-high
ordering + the post-reset settle before the first DCS; (2) the **post-SLPOUT 120 ms** and any per-command
delays vs p-boot's `panel_dcs_seq` timing; (3) whether the cold panel needs the DCS stream in a specific
LP-escape timing the warm panel tolerates. **NEXT: re-derive p-boot's exact panel reset/delay/order
(`display_board_init` + `panel_dcs_seq` timing) and diff against our `panel_reset_*` + `st7703_init` delays
line-by-line** (data already matches — look only at the µs/ms delays and the reset-relative-to-dphy order).
Then a cold-boot A/B of any delay/order fix. Traces: `shots/trace-native-pmic.log`,
`shots/trace-pboot-clktcon0.log`.

### 2026-06-25 (cont.) — cold panel-init reset/timing/transmission re-derived from p-boot: ALL faithful [analysis]

Re-derived p-boot's exact cold panel bring-up (`display_init`/`display_board_init`/`panel_init`/
`mipi_dsi_dcs_write` in `pboot_display.c`) and diffed against ours line-by-line — NOT the DCS data
(`cmp_st7703.py` already proved that identical) but the **reset sequence, delays, order, and FIFO
transmission**:

- **Reset order/timing.** p-boot: reset-LOW + PMIC + 15 ms (in `display_board_init`) → … → `dphy_enable`
  → reset-HIGH → 15 ms → DCS → HSC → 1 ms → HSD. Ours: …dsi_host → reset-LOW + PMIC + 15 ms → dphy →
  reset-HIGH → 15 ms → DCS → HSC → 1 ms → HSD. The **load-bearing order matches**: power applied with reset
  held low ≥15 ms (ST7703 needs ≥10 ms post-power), reset deasserted AFTER dphy, 15 ms settle before DCS.
  (p-boot asserts reset a bit earlier/holds it longer — through tcon0+dsi setup — but the panel is unpowered
  until PMIC in both, so the power-on-reset window is equivalent; a prior session already reordered to
  p-boot's exact order with no change.)
- **DCS delays.** p-boot: all 18 config commands delay-0, then SLPOUT, **120 ms**, DISPON. Ours: identical,
  + a harmless extra 20 ms after DISPON. Order of all 20 commands identical.
- **Long-packet transmission.** p-boot `mipi_dsi_dcs_write` (len>2): header→`CMD_TX_REG(0)`, data+CRC→
  `CMD_TX_REG(1+i)`, `CMD_CTL = total_bytes-1`, `DSI_START_LPTX`, wait-for-completion. Our `dcs_send` pushes
  the contiguous precomputed `[header(4)][payload][CRC(2)]` as words from `TX_REG(0)` (== header in word0,
  data in word1+) with `CMD_CTL = pkt_len-1` — **identical FIFO layout + length**. The 64 B/62 B SETGIP
  packets (18/17 FIFO words) fit the TX FIFO; no truncation.

**So the cold panel-init path is faithful to p-boot in data, order, delays, AND transmission — yet cold boot
is black.** Static/source analysis is now FULLY exhausted across the entire pipeline. The bug survives every
comparison we can make. The remaining unknown is the **panel's actual internal response** to our cold init
vs p-boot's — which a register dump of the *SoC* side cannot see.

**NEXT (requires new HW instrumentation, not more analysis):** probe the PANEL itself during/after cold init
via DCS **reads** — extend `dcs_read_dbg` to read back ST7703 config (e.g. read-back of the gate-driver /
SETGIP / address-mode / pixel-format registers the panel exposes, and the RDDPM/RDDSDR status) AFTER
`st7703_init` on a COLD boot, and compare to the same reads on p-boot's working panel. If a panel register
reads different cold vs p-boot-warm, that command didn't take effect on the cold panel despite identical
SoC-side transmission (points at a panel-state/timing dependency); if all panel reads match yet it's still
black, the fault is in the HS **video** path the panel receives after DISPON (DE2→TCON0→DSI HS pixel
delivery) under cold conditions, not the DCS config. Either way the probe is on the panel side, where every
SoC-side avenue has now been exhausted.

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
