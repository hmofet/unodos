# UnoDOS PinePhone port (Allwinner A64 / AArch64 bare metal)

Archived verbatim on 2026-09-08 from the Claude Code memory file `unodos-pinephone-port.md`. This is dated session history kept for reference. Later sections supersede earlier ones, so read bottom-up for the current state. The durable facts now live in the memory file itself and in the repo docs.

---

UnoDOS 3 **PinePhone** port — the **10th fresh contract-driven port** and the
**SECOND AArch64 world**. Part of the ARM+PowerPC bare-metal round (after
[[unodos-rpi-port]]; before [[unodos-ppcmac-port]] the hard PowerPC one). **REUSES
the rpi AArch64 core verbatim** (same GAS dialect, same framebuffer primitives,
same apps.inc.s / dostris.inc.s logic) — the project ethos is each port its own
assembly, so it's a self-contained copy, not a shared lib.

**Shipped: M1–M3 + Dostris, ALL harness-verified** (7 screenshots
`pinephone/shots/*.png`). Committed + pushed to origin/master.

**2026-06-18 (session 2): PATH A DSI PANEL BRING-UP IMPLEMENTED** (`pinephone/panel.inc.s`,
~600 lines, harness-verified end-to-end; NOT yet hardware-tested). The payload now lights
the DSI panel ITSELF (nothing in the boot chain does — the founding "boot chain lit the
panel" assumption was false). `panel_init` (called from `_start` before `fb_init`) runs:
clk_init (CCU PLL_DE/VIDEO0/MIPI two-step + DE/TCON0/DSI/D-PHY gates+resets, SRAM-C1→DE) →
rsb_init+pmic_init (RSB driver → AXP803 rt-addr 0x2D: DLDO1 3.3V, DLDO2 1.8V=MIPI power,
GPIO0LDO) → panel_reset_low+15ms → dsi_host_init (table of ~40 pokes to 0x01CA0000) +
dphy_init (table + ANA3/ANA2/ANA1 LDO RMW sequence w/ 1ms delays, 0x01CA1000) →
panel_reset_high+15ms → st7703_init (walks the precomputed `dsi_init_seq` blob, 20 DCS
packets via dcs_send, SLPOUT+120ms+DISPON) → tcon0_init (table, 720×1440, IF=8080/DSI) →
dsi_start (HS) → backlight_on (PWM PL10 0x04AF0437 + PH10 high). Then `fb_init` programs
DE2: GLB/BLD/TCON all PANEL_SZ=0x059F02CF (720×1440), our 480×640 fb is the UI overlay
LAYER (LAYER_SZ=0x027F01DF) at COORD 0 (top-left) — the old "adopt U-Boot's FB" path is
GONE (U-Boot leaves no panel). **Register values** distilled from NuttX/lupyuen + Linux
sun50i-a64 (a64_rsb/de/tcon0/mipi_dsi/mipi_dphy.c, pinephone_pmic/lcd.c) via WebFetch.
**DSI packet framing (ECC over header + CRC-16 reflected-0x8408 over payload) precomputed
in `mkdata.py`** (dcs_packet/mipi_ecc/mipi_crc) → `dsi_init_seq` blob (.hword len,delay +
bytes; terminator len=0); VALIDATED against lupyuen's worked example SETEXTC →
`39 04 00 2C B9 F1 12 83 84 5D` (ECC 0x2C, CRC 0x5D84 — exact match). ST7703 arrays
byte-perfect from raw pinephone_lcd.c (SETGIP1/2 = 60B each, SETGAMMA = 35B).
**KILLER BUG fixed:** panel_reset_low/high called `bl mmio_clrbits/setbits` then `ret`
WITHOUT saving x30 → ret jumped back to itself = infinite loop (harness froze at the
`ret`, PC stuck, black screen). Fix = inline the RMW so they stay leaf. (Audit all
non-leaf asm for LR-save; the table-tail-call routines use `b apply_pokes` which is safe.)
**Harness extended** (harness.py): mmio_map the 8 new peripheral pages (SRAM/TCON0/CCU+PIO/
DSI/DPHY/R_PRCM/R_PIO/RSB+PWM) as sinks; ccu_read returns 0x10000000@0x48 (PLL lock),
rsb_read returns 1@0x40C (RSB_STAT TRANS_OVER), DSI BASIC_CTL0 reads 0 (INSTRU_EN clears) —
so all status polls pass and the bring-up runs to the launcher. ALL M1-M3 milestones STILL
RENDER (regression green) — proves the bring-up is well-formed, encodes, never hangs (every
poll bounded ~0x100000). Harness CANNOT prove the panel lights (no panel/DSI/clock model) —
that's hardware-only. **`build.sh paneldbg`** (--defsym PANELDBG=1) blinks PD18 a stage count
(1=clocks..6=done) between blocks for blind hardware bisect. **`fel.sh run [payload]`** = the
fast flash-free dev loop: `sunxi-fel spl <fw>` (DRAM init) → write payload @0x40080000 → exec
(panel self-lit, no U-Boot). NEXT (hardware, user connects phone to devbuntu): confirm the
panel lights; tune the harness-unverifiable bits — DE2 layer/blender sizing over 720×1440,
DSI video-mode H/V-blank constants, cache/EL on FEL-SPL vs U-Boot handoff, DSI/TCON/DE2 vs
HS-start ordering. Serial console would make DSI-timing tuning far easier; LED beacon
localizes blind until then. NOT yet committed.

**2026-06-18 (session 2) ON-HARDWARE TEST — bring-up RUNS CLEAN, panel still dark.** Drove the
real phone via devbuntu. FEL path was a dead end (USB bulk_send wedges + `sunxi-fel spl` doesn't
start the ARM generic-timer system counter, so our cntpct delays hang → LED stuck solid). Pivoted
to a DIRECT-BOOT card (pine-fresh.img = U-Boot SPL+ATF+boot.scr; ATF enables the counter; swap
unodos.bin on the FAT via offset-mount, card resolved by model "SD  Transcend" w/ DOUBLE space,
write-blocker `blockdev --setrw`+trap). Added an RGB-LED colour beacon (PinePhone LED: red=PD19
green=PD18 blue=PD20, active-high; led_rgb) + an exception-vector (VBAR_EL2/EL1) fault handler
that fast-flutters WHITE so a crash can't reboot-loop and hide. RESULT: the colour beacon reaches
the end (RED clocks→YELLOW pmic→GREEN dsi/dphy→CYAN st7703→BLUE tcon/backlight→MAGENTA done, then
post-panel GREEN/RED/BLUE→steady CYAN=mainloop) **STEADY, no white flutter = panel_init + fb_init
+ fs_init + draw_launcher + mainloop ALL run with NO fault/hang on real silicon.** But **screen
stays dark** — the bring-up executes yet doesn't light the XBD599. This is the blind DSI-timing
tuning wall the bring-up doc predicted. Confirmed via WebFetch that U-Boot maps 0x0-0x40000000 as
device (so DE2 0x01100000 IS mapped — no translation-fault theory). **SERIAL IS DEAD: /dev/ttyUSB0
(FT232 on the headphone jack) reads 0 BYTES even mid-boot = no signal = the PinePhone internal UART
DIP switch (#6) is in audio mode, not console.** NEXT (recommended): flip DIP switch 6 (open back
cover) to get U-Boot's serial log + add payload UART0 debug prints — turns blind tuning into real
debugging. Alt: blind experiments (reorder dsi_start vs TCON0/DE2 — lupyuen starts DSI-HS before
DE/TCON, we do TCON0 before dsi_start; add PLL_MIPI lock poll; DE2 480x640-layer-over-720x1440
sizing; backlight). Build with `build.sh paneldbg` (PANELDBG: RGB beacon + fault vectors + post
markers). All staged on devbuntu ~/pine-uboot/ (unodos_paneldbg.bin + the direct-boot card).
SERIAL CABLE BUILD (user soldering one from a Waveshare USB-to-TTL): PinePhone headphone-jack
UART = 3.3V TTL, 115200 8N1, **DIP switch 6 OFF = UART** (ON = audio). Jack pinout (Pine64 wiki):
Tip=phone RX, Ring=phone TX, Sleeve=GND. Crossover to adapter: Tip→TXD, Ring→RXD, Sleeve→GND.
USER'S CABLE COLOURS: **white=Tip(→TXD), red=Ring(→RXD), bare=Sleeve(→GND)**. 3.3V jumper, do
NOT connect VCC. If no/garbage output: swap white↔red (TX/RX). The FT232 (0403:6001) seen on
devbuntu earlier was an UNRELATED adapter, not the phone — phone is on USB-C. Once serial works:
capture U-Boot boot log @115200 during power-on, then ADD UART0 (0x01C28000) TX debug prints to
the payload to see why the panel stays dark (panel_init runs clean but XBD599 doesn't light).
On the Waveshare USB-TTL: **yellow→TXD** (=white/Tip/phone-RX), **orange→RXD** (=red/Ring/phone-TX),
bare→GND; the FT232 0403:6001 on /dev/ttyUSB0 IS the user's Waveshare. Loopback (TXD↔RXD jumper)
verified adapter+host. Capture reliably via a 120s BACKGROUND `cat` + one power-cycle off→on (U-Boot
prints only during boot). Card readers: letter UNSTABLE (sdc/sdf) + a SECOND unrelated
MassStorageClass card exists → resolve ONLY by model "SD  Transcend" (double space). WAIT for user
to confirm card-in-reader / phone-off before flashing/capturing.

**SERIAL-DEBUGGED — fault PINNED to 2nd ST7703 DCS command.** Live serial trace: panel_init →
PLL_DE=91001701 PLL_VIDEO0=91006207 PLL_MIPI=90c0071a (**all bit28=LOCKED on silicon**) TCON0_GCTL=
80000000 → [pmic]ok [dsi_host]ok [dphy]ok [ystage]ok [reset_hi]ok → "[st7703] begin: .." (2 dots) →
**FAULT (solid-white LED = persistent exception)**. So clocks/PMIC/DSI-host/D-PHY/TCON0 ALL succeed;
dies sending the **2nd** ST7703 DCS cmd = SETMIPI (first LARGE packet 35B/9 FIFO words; cmd0 SETEXTC
10B/3 words transmits OK). Applied p-boot/NuttX reference-diffs (none fixed it): tcon0_init moved
BEFORE dsi block; dsi_start clears LP11 LANE_CEN bit4 @DSI+0x20; 160ms DE2 settle; dcs_send clear-
flags 0x04000200→0x06000200 (+RX_FLAG bit25). LIKELY ROOT CAUSE to chase next: cmd0's LP DCS xmit
never completes (INSTRU_EN @DSI+0x10 bit0 doesn't self-clear → bounded poll times out, dcs_send
returns, DSI left wedged → cmd1 trigger faults). NEXT: instrument dcs_send to read back DSI BASIC_
CTL0/status after cmd0; compare dcs_send trigger/wait to NuttX a64_mipi_dsi_write + Linux
sun6i_dsi_dcs_write_long (LP-escape/FIFO). Framing is CORRECT (pre-framed in mkdata.py). build.sh
paneldbg = UART markers + RGB beacon (BLUE=clk RED=pmic GREEN=dsi/dphy YELLOW=st7703 MAGENTA=
dsi_start, WHITE-flutter=fault) + exception vectors + per-DCS-cmd dots. Code NOT committed.

**2026-06-19 SERIAL-DEBUG SESSION (run from amanuensis, NOT the phone box) — 4 BUGS FOUND, panel
now LIT + bring-up runs clean to mainloop; chasing "backlit black".** RIG: this session ran on
**amanuensis**; the USB-TTL serial adapter + the boot SD card are on a SEPARATE Windows laptop
**Carbon** (hostname carbon = 192.168.2.19, reachable by `ssh carbon` w/ KEY auth, Windows shell).
WORKFLOW that works: edit+build+harness on amanuensis (pinephone source IS here at
`C:\Users\arin\Documents\Github\unodos\pinephone`); drive Carbon's serial over SSH by sending
PowerShell as **`powershell -NoProfile -EncodedCommand <b64-UTF16LE>`** (avoids all nested-quote
hell through git-bash→ssh→Win shell); the adapter is **COM3**; capture = a `System.IO.Ports.SerialPort`
@115200 8N1 reader looping `ReadExisting()` to `%USERPROFILE%\pine-serial.log` for ~120s, run via
the Bash tool with run_in_background while the user power-cycles. FLASH from Carbon (Windows) is
TRIVIAL vs devbuntu: the card's FAT32 partition (label UNODOS) auto-mounts as **E:**, so flash =
`Copy-Item unodos_paneldbg.bin E:\unodos.bin` + `Write-VolumeCache E` (SPL/U-Boot in the 8K-1M gap
are not a partition, untouched). scp amanuensis→carbon to stage. Card ping-pongs E:(Carbon)↔phone
each iter; user confirms each move. INSTRUMENTATION added to panel.inc.s (PANELDBG): per-DCS-cmd
readback of DSI BASIC_CTL0(+0x10)/CMD_CTL(+0x200); and the exception vector table now stamps the
vec index (0-15) into w9 + a fault handler that prints **VEC/EL/ESR/FAR/ELR** over UART (EL-aware
mrs esr/far/elr_elN) instead of just the white flutter — THIS pinned bug #1 instantly. BUGS, all
hardware-confirmed by the serial trace / the lit panel: **(1) ALIGNMENT FAULT in st7703_init blob
walk** — SETMIPI (cmd1) is 35 bytes (ODD), so after `x19 += 4+len` the pointer is odd-aligned and
the next `ldrh w0,[x19]` alignment-faulted (ESR EC=0x25 DFSC=0x21, FAR=odd addr; the "INSTRU_EN
stuck / DSI wedged" hypothesis was WRONG — both DCS cmds transmit fine, BASIC_CTL0 bit0 self-clears).
FIX = read len/delay as `ldrb` byte-pairs (alignment-agnostic). After: all 20 cmds send, `[st7703]
ok`, mainloop reached. **(2) BACKLIGHT dead** = `backlight_on` drove PH10 via PIO+0xA0/0xCC = PE_DAT/
PF region (WRONG); PH CFG1/DAT are PIO+0x100/+0x10C (0x01C20900/0c). FIX → panel now BACKLIT.
**(3) DE2 MIXER writes dropped** = clk_init enables the DE *bus* clock but never the DE block's
INTERNAL gates/reset at DE base 0x01000000 (SCLK_GATE +0x00, HCLK_GATE +0x04, AHB_RESET +0x08 all
bit0; DE2TCON_MUX +0x10 = 0). Symptom: post-fb_init readback GLB_CTL=0/OVL_TOPADD=0. FIX = write
those 4 at the top of fb_init → readback flipped to GLB_CTL=1 / TOPADD=40400000. **(4) BLENDER
3-channel config on a 1-channel layer** (IN FLIGHT, not yet HW-confirmed) = fb_init wrote
BLD_FILL_COLOR_CTL(0x1100000)=0x701 (enable pipes 0,1,2) + BLD_CH_RTCTL(0x1101080)=0x321 (3-channel
route); we only have channel 1 → phantom pipes 1,2 composite black OVER our UI layer. FIX (per NuttX
a64_de.c single-channel) = FILL_CTL=0x101 (P0_EN|P0_FCEN), RTCTL=0x1 (pipe0<-ch1), + BLD_FILL_COLOR
(0x1101004)=0xFF000000. ALL fb_init/panel changes are NON-ifdef but harness-safe (harness reads FB
directly, regression PNG byte-identical to shots/m1_boot.png). Diagnostic build `build.sh panelpost`
= PANELDBG+POST (serial markers + on-screen RED/GREEN/BLUE fills via post_fill, palette-independent).
VERIFIED-FAITHFUL vs authoritative sources (WebFetched, NOT just the [REF] blueprint): dcs_send
trigger/wait == NuttX a64_mipi_dsi_write (JUMP_SEL 0x000F0004, clear-flags 0x06000200); dphy_init,
dsi_host_tbl (ALL video-mode timing values: BASIC_CTL1 0x5BC7, BASIC_SIZE0/1, PIXEL_PH 0x1308703E,
SYNC/HSA/HBP/HFP/HBLK/VBLK), INST_FUNC table ALL match a64_mipi_dsi_enable EXACTLY; tcon0_init order
(BEFORE the DSI block) + TCON0 values match NuttX up_fbinitialize EXACTLY (do NOT "fix" the
tcon0-first order — verified correct); pmic_init == pinephone_pmic_init EXACTLY (DLDO1 3.3V / GPIO0LDO
3.3V / DLDO2 1.8V=panel power, NuttX lights the panel with exactly these 3 rails); dcs_packet framing
correct (1B->DT0x05, 2B->0x15, 3+B->0x39, SLPOUT/DISPON proper short writes).

**STATE AT END OF 2026-06-19 SESSION: ALL 6 bugs/diffs fixed, the ENTIRE display pipeline is now
byte-faithful to NuttX, yet panel is BACKLIT-BLACK (lit, no image).** Bugs 5 & 6 (beyond the 4 above):
**(5) DE2 incomplete** — fb_init only set GLB/blender/overlay; added NuttX a64_de_init's MIXER0-clear
(zero 0x01100000..+0x6000) + disable all 11 enhancement/scaler blocks (VS 0x01120000, UNDOC 0x01130000,
UIS1 0x01140000, UIS2 0x01150000, FCE 0x011A0000, BWS 0x011A2000, LTI 0x011A4000, PEAKING 0x011A6000,
ASE 0x011A8000, FCC 0x011AA000, DRC 0x011B0000). **(6) dsi_start ordering** — clear LANE_CEN (DSI+0x20
bit4) must come BEFORE the 1ms settle, not after (continuous HS clock must stabilise before HS data);
was swapped. **COHERENCY DEFINITIVELY RULED OUT**: added SCTLR readback to the trace = **0x30c50830**
-> M(bit0)=0 MMU OFF, C(bit2)=0 D$ OFF, I(bit12)=0 I$ OFF. With MMU off, AArch64 data accesses are
Device memory -> FB writes hit DRAM directly -> DE2 DMA coherent. (U-Boot `go` handed off MMU-OFF, NOT
on as _start's comment assumed; _start's cache-clear is moot.) Also added a dc-cvac FB flush in
post_fill (no-op with caches off) + a `build.sh panelpost` (PANELDBG+POST) diag. On-HW trace EVERY
build: PLLs lock, all 20 DCS send (BCTL0=00030000 each), [st7703] ok, DSI_BASIC_CTL0=00030001 (HS),
DE2_GLB_CTL=00000001, DE2_OVL_TOPADD=40400000, [unodos] mainloop — clean, no fault. **Yet POST solid
RED/GREEN/BLUE fills (written straight to FB @0x40400000, DE2 layer points there) DO NOT show.**
RIG NOTE: by session end the card reader AND the FTDI serial adapter are BOTH BACK ON DEVBUNTU
(/dev/ttyUSB0 = FT232 0403:6001; flash via flash-paneldbg.sh, capture via `sudo stty -F /dev/ttyUSB0
115200 cs8 raw; sudo timeout 120 cat /dev/ttyUSB0`). Carbon (192.168.2.19) ssh still works but its COM
port was unplugged. Bug #7 also found+fixed: **TCON0 SAFE_PERIOD typo 0x00BB8003 -> 0x0BB80003**
(FIFO_NUM 3000=0xBB8 belongs <<16; gates the frame trigger; I'd missed it in the "tcon0 matches" pass).

**DECISIVE RESULTS (2026-06-19, the durable conclusions — full writeup in PINEPHONE-BRINGUP.md §8):**
(1) **PANEL IS ALIVE + DISPLAY-ON.** Implemented a DCS READ (`dcs_read_dbg`, LP, get_power_mode 0x0A,
LPRX JUMP_SEL=0x100700F4 LP11->LPDT->DLY->TBA->END, read DSI+0x240): trace = **RXCTL=02060003 (RX_FLAG
bit25 SET = panel answered) + RXDAT=...1c (power-mode 0x1C = Display-ON+Normal+SleepOut)**. So the DSI
LP link works and the panel processed our SLPOUT/DISPON. Panel is NOT the problem. (2) **COHERENCY RULED
OUT**: SCTLR=0x30c50830 (M=0 MMU off, C=0 D$ off, I=0 I$ off; U-Boot `go` hands off MMU-OFF -> data =
Device mem -> DE2 DMA coherent). (3) **DE2 IS NOT SCANNING OUT**: set blender BLD_BK_COLOR=RED
(0xFFFF0000, the backdrop DE2 emits regardless of layer) -> screen STILL BLACK; POST solid fills to the
FB (0x40400000 where the layer points) also never show. **THE WALL = the DE2->TCON0->DSI *video* path
delivers no pixels to an alive, display-ON panel, despite EVERY register matching NuttX byte-for-byte.**
Almost certainly ENVIRONMENTAL: NuttX boots natively + sets all clock state; we run as a U-Boot `go`
payload inheriting U-Boot's state (U-Boot has NO DSI/DE/TCON driver), so some divider/reset/enable NuttX
establishes from cold may differ. Harness can't model any of it. RETURN-TO-FIX candidates (see doc §8):
read TCON0_GINT0(+0x04) twice to see if TCON0 is scanning; cross-check megi's p-boot DE/TCON/DSI startup;
re-examine DE/TCON clock dividers; match NuttX exact order (de_init->160ms->blender/ui/enable); maybe an
initial TCON0 trigger kick. Diagnostic toolbox left in tree (PANELDBG): SCTLR print, fault VEC/ESR/FAR/ELR
vectors, per-DCS BCTL0/CMDCTL, dcs_read_dbg, post_fill dc-cvac; `build.sh panelpost`=PANELDBG+POST
(on-screen RED/GREEN/BLUE). NOTE: red BLD_BK_COLOR is a left-in diagnostic -> revert to 0xFF000000.
**DECISION (user): document everything (DONE), then try a DIFFERENT BOOTLOADER (megi's p-boot, which DOES
light the A64 panel + hands a framebuffer) to load UnoDOS via the ADOPT-FB path (skip our panel_init),
get pixels, THEN return to fix the from-scratch DSI video path.** Panel HW is fine (pmOS lights it).

**✅ 2026-06-20: UnoDOS BOOTS + DISPLAYS ON REAL PINEPHONE HARDWARE via p-boot.** Pivoted to megi's
**p-boot** (the bootloader that lights the A64 DSI panel itself + hands off a live framebuffer). UnoDOS
runs as p-boot's "Linux kernel": added an **ARM64 Image header** to kernel.s `_start` (code0=`b _entry`,
**text_offset=0x80000** so p-boot's LINUX_IMAGE_PA 0x40000000 + 0x80000 = our link addr 0x40080000;
magic 0x644d5241 @ off 56; header is transparent to U-Boot `go`+harness which just hit code0). New build
flags: **PBOOT** (skip panel_init; `fb_init` ADOPTS p-boot's FB = read DE2 OVL_TOPADD/OVL_PITCH, draw
there, do NOT reprogram DE2) and **pbootdbg** (PBOOT+PANELDBG). On HW: **p-boot lit the panel + UnoDOS
launcher rendered** (small, top-left — our 480x640 content drawn pitch-relative into p-boot's NATIVE
720x1440 FB). Trace: **DE2_OVL_TOPADD=0x48000000** (p-boot's FB, = what we adopted), GLB_CTL=1,
[unodos] mainloop. So the from-scratch-DSI video wall (DE2/TCON0 not scanning, see above) is BYPASSED.
**HOW (all on devbuntu, ~/pboot/):** prebuilt p-boot pieces from github.com/davidwed/p-boot dist/
(p-boot.bin 32K GUI+display, fw.bin=ATF+SCP, p-boot-conf-native x86-64); PinePhone-1.2 DTB from
~/pine-uboot/u-boot/arch/arm/dts/; conf/ = boot.conf (no=0 name=UnoDOS dtb/atf/linux + bootargs) +
board.dtb + fw.bin + Image(=unodos_pbootdbg.bin) + files/{off,pboot2}.argb (GUI splashscreens, REQUIRED
or p-boot-conf errors). Card build (`~/pboot/build-pboot-card.sh`): MBR, one 0x83 partition (bootable)
from sector 2048; `dd p-boot.bin seek=8 bs=1024` (8KiB boot sector); `p-boot-conf-native conf /dev/sdc1`.
GOTCHA: the devbuntu udev write-blocker RE-ARMS ro on partition-table re-read → `blockdev --setrw` BOTH
/dev/sdc AND /dev/sdc1 immediately before EACH raw write (the dd AND the p-boot-conf). p-boot-conf also
needs name= in boot.conf + a files/ dir w/ the .argb splashscreens. LAYOUT: the PBOOT adopt path now CLEARS the full
720x1440 FB to black (wipes p-boot's splashscreen) + CENTERS the 480x640 (offset 120x400). Aspect
mismatch (UnoDOS 3:4 vs panel 1:2) => centered-on-black, NOT full-screen; scaling to fill (1.5x
width-fill/letterbox, or stretch-distort) is a documented deferred option. Clean build = unodos_pboot.bin
(no debug); diag = unodos_pbootdbg.bin. This p-boot card REPLACED the U-Boot card on the SD Transcend
(U-Boot card rebuildable via mksd.sh). **COMMITTED** to branch parity-push-fresh-ports: 5452495 (the
7 DSI fixes + instrumentation + p-boot Image-header/PBOOT support + PINEPHONE-BRINGUP.md + tooling),
plus a follow-up commit for the centering. Session WRAPPED 2026-06-20. Eventual RETURN: fix the native
DSI video scanout (doc §8 candidates), then optionally scale the p-boot display + wire real input.

**2026-06-20 (cont., from amanuensis) — p-boot REGISTER CROSS-CHECK DONE (everything matches); 3
environmental fixes + TCON0-scan diagnostic staged; AWAITING HW.** Did §8 candidate #2 fully: pulled
megi's p-boot **src/display.c** (davidwed/p-boot, the SAME loader that lit THIS phone, so it's
ground-truth not paper) to /tmp + C:\Users\arin\pboot_display.c, read the real source + hand-decoded
the bitfields. **Our native bring-up matches p-boot register-for-register on EVERY scanout reg:**
tcon0_init (DCLK MIPI_PLL/6, CTL enable|IF_8080, timing_active 720x1440, ECC_FIFO, CPU_IF
0x10010005, TRI0/1/2, SAFE_PERIOD 0x0BB80003, io_tri 0xe0000000, TCON_ENABLE) == p-boot tcon0_init;
DE2 blender/UI-layer (route 0x1, fcolor_ctl 0x101, fcolor 0xff000000, bld_mode 0x03010301, UI attr
0xFF000405) == p-boot display_commit; dsi_start HSC JUMP_SEL 0x00000F02 + clear LANE_CEN bit4 + 1ms +
HSD JUMP_SEL 0x63F07006 == p-boot sun6i_dsi_start (decoded DSI_INST_ID_* fields by hand, equal our
literals). So our regs are faithful to TWO independent known-good impls (NuttX AND p-boot) =>
**the wall is ENVIRONMENTAL** (U-Boot `go` handoff state a cold BROM boot doesn't have). THREE cold-boot
divergences found+fixed (ALL in NON-debug bring-up, harness-clean — default build still renders the
launcher, 4922-B PNG): **(1) full RESET CYCLE of DE/TCON0/DSI** — clk_init now ASSERTS then de-asserts
CCU resets (BUS_SOFT_RST1 0x2C4 bit12=DE/bit3=TCON0, BUS_SOFT_RST0 0x2C0 bit1=DSI) instead of
de-assert-only; + DE-internal mixer0 reset (0x01000008) cycled in fb_init. **(2) DE clock DIVIDER
cleared** — clk_init DE_CLK_REG (0x104) BIC now 0x8300000F (was 0x83000000) so a U-Boot-left divider
can't starve TCON0 below pixel rate. **(3) SRAM_CTRL1 RMW** — clear bit24 only (was whole-reg zero),
matching p-boot/NuttX. **DIAGNOSTIC (§8 cand #1, PANELDBG):** after [unodos] mainloop, sample
TCON0_GINT0 (0x01C0C004) 6x ~50ms apart over UART (+ re-print GCTL) — CHANGING=TCON0 scanning (stall
downstream), STATIC=trigger never fires (stall=TCON0). **STAGED:** build.sh paneldbg rebuilt (30184B,
md5 1d78fd44) → scp'd devbuntu:~/pine-uboot/unodos_paneldbg.bin. **NEXT NEEDS USER+PHONE:** SD is STILL
the p-boot card → must become a U-Boot card so panel_init RUNS (p-boot skips it). Recipe (card in
devbuntu SD-Transcend reader, confirm first): blockdev --setrw /dev/sdX; dd if=~/pine-uboot/pine-fresh.img
of=/dev/sdX bs=4M conv=fsync (restores SPL+U-Boot+ATF+boot.scr+FAT); bash ~/pine-uboot/flash-paneldbg.sh
(swaps new payload into FAT @1MiB); card→phone; serial bg-cat + power-cycle. Read whether GINT0 moves.
flash-paneldbg.sh mounts FAT @offset 1048576 so it ONLY works AFTER the card is a U-Boot card. Code
changes NOT yet committed (working tree on parity-push-fresh-ports: kernel.s, panel.inc.s, BRINGUP.md).

**2026-06-21 (HW session, from amanuensis) — TCON0 CONFIRMED SCANNING; every register STUCK; not a
value bug. + SHUFFLE-FREE XMODEM workflow.** Ran on HW (full writeup PINEPHONE-BRINGUP.md §8 2026-06-21).
Reset-cycle+DE-divider fixes did NOT light it (still backlit-black, RED BLD_BK_COLOR diag never shows).
DECISIVE diagnostics: **(1) TCON0 IS scanning** — GINT0 cleared-between-reads goes 0xA00→0x800→0x800…,
bit11 (frame-done latch) RE-SETS every 50ms = continuous frame transfer; DSI_BASIC_CTL0=00030001 steady
(video running). **(2) D-PHY analog cross-checked vs p-boot too** (ANA3 LDOR/C/D→VTTC/VTTD→DIV, ANA2
CK_CPU, ANA1 VTTMODE, ANA2 P2S_CPU) = matches → EVERY DE2/TCON0/DSI/DPHY/CCU reg now matches p-boot, not
just NuttX. **(3) FULL on-HW register READBACK dump (dump_regs/dump_tbl, 35 regs): ALL read back exactly
what we wrote**, incl BLD_BKCOL=ffff0000 RED — so NOT a dropped/wrong write; silicon holds our whole
config + RED backdrop yet emits black. (Only deltas = HW status bits: TCON0 TRI1 hi=0x00C9 read-only
block-counter=more proof it's scanning; DSI_CTL reads 01010001.) So the whole "value wrong/didn't stick"
class is ELIMINATED. **Remaining question is binary: DE2-not-clocking-pixels-out vs DSI-HS-lanes-not-
transmitting** (LP works—panel answered DCS read—but HS is separate analog). **NEXT decisive test STAGED:
all_pixels_on (DCS 0x23) appended to mkdata.py ST7703 seq** — drives panel white from its OWN logic:
WHITE=>HS video reaches panel, black is the pixel DATA (DE2 output); BLACK=>no video signal (DSI HS/DPHY).
REMOVE the ([0x23],20) from mkdata.py after. **WORKFLOW BREAKTHROUGH — shuffle-free+card-free HW iter:**
our U-Boot has CONFIG_CMD_LOADB=y (loadx/loady). Hand-rolled pyserial XMODEM-CRC sender (devbuntu
/tmp/xload.py + /tmp/iload.py) does loadx 0x40080000 → stream 30KB payload → dcache flush;dcache off;icache
off;go — runs from RAM, NO card write/removal. iload.py also auto-interrupts the 2s autoboot (watch "Hit
any key"→spam space→"=>"). So 1 cmd + 1 power-cycle = load+run+capture. **U-Boot ums REBUILT but FAILS:**
added USB_MUSB_SUNXI/MUSB_GADGET/FUNCTION_MASS_STORAGE/CMD_USB_MASS_STORAGE (installed via dd seek=8,
MBR+FAT kept), `ums 0 mmc 0` recognized but "Controller uninitialized / g_dnl_register failed error -6"
→ needs more cfg (try DM_USB_GADGET + USB_MUSB_PIO_ONLY); deprioritized since XMODEM works. GOTCHAS:
arin IS in dialout group on devbuntu → serial capture needs NO sudo (only card-FAT flash needs sudo);
**bare-metal power-cycle = LONG-press ~12s (AXP803 PMIC force-off) then short-press** (short press alone
does nothing, no power-btn handler); **the headphone-jack TRRS plug is FRAGILE — dropped mid-session
(0 bytes though FTDI still enumerated), reseat if serial goes silent.** Code STILL not committed
(kernel.s diagnostics, panel.inc.s reset-cycle/dump_regs, mkdata.py 0x23 diag, BRINGUP.md).

**2026-06-21 (cont.) — THREE real panel-DCS transcription bugs FOUND+FIXED; full p-boot parity reached;
STILL backlit-black.** Did the complete byte-for-byte diff of our ENTIRE ST7703 DCS init seq vs p-boot's
dsi_panel_init_seq (script C:\Users\arin\cmp_st7703.py decodes p-boot's TX-FIFO words). **The doc's old
"byte-perfect vs pinephone_lcd.c" claim was WRONG 3x — all "missing 0x00" errors that shift every later
param: SETMIPI(0xBA) had 29 bytes should be 28; SETGIP1(0xE9) had 60 should be 64; SETGIP2(0xEA) had 60
should be 62.** SETMIPI = panel's MIPI/DSI interface cfg, SETGIP1/2 = gate-driver row-scan timing =
exactly what corrupts to "display-ON but black." Fixed in mkdata.py; cmp now says ALL 20 cmds MATCH.
Also verified DSI video-timing block (0xB0-EC, BASIC_SIZE0/1, BASIC_CTL1) by COMPUTING p-boot's values
from panel constants (HFP30/HSYNC28/HBP30, VFP18/VSA10/VBP17, 72MHz, 4-lane non-burst → hsa74 hbp84
hfp74 hblk2330 vblk0, vstart-delay1468) = all match our table. p-boot display_board_init PMIC+reset ==
our pmic_init exactly. **RESULT: STILL black** after all 3 fixes (panel still alive+display-ON, all 20
cmds send, TCON scanning, DSI running, RED backdrop never shows). Also tried per-frame GLB_DBUFFER
re-commit in mainloop (hypothesis: DE2 double-buffered cfg never latched — dump reads SHADOW not active
set) → still black. **NOW AT BYTE-FOR-BYTE p-boot PARITY across EVERYTHING comparable** (all regs stuck
on HW, full DCS seq, video timing, PMIC, reset) yet p-boot lights this panel + we don't. Remaining diff
is un-seeable by source/register compare: runtime timing nuance, DE2-produce-vs-DSI-HS-transmit (unsplit
— all_pixels_on/0x23 stayed black but ST7703 may not honor it in video mode), or p-boot's cold-boot
DRAM/clock cascade (U-Boot video RULED OUT — disabling CONFIG_VIDEO_DE2 changed nothing). **DEFINITIVE
NEXT STEP: get p-boot's ACTUAL RUNTIME register dump** — p-boot has dump_de2_registers()/dump_dsi_
registers() (commented in display_init); rebuild p-boot from source (github.com/davidwed/p-boot — only
prebuilt dist/ is on devbuntu) with them enabled, boot it (lights panel), capture its WORKING DE2/TCON/
DSI state, diff value-by-value — any diff IS the bug. 3 DCS fixes are real (match p-boot+Linux), keep
them. NEW SHUFFLE-FREE TOOLING (devbuntu): no-video U-Boot (CONFIG_VIDEO off, on SD via dd seek=8);
XMODEM-into-RAM loader /tmp/iload.py (interrupt 2s autoboot→loadx 0x40080000→stream→dcache off;go);
USER PREFERS the card reader for writes over in-phone XMODEM. Code STILL uncommitted (kernel.s, panel.inc.s,
mkdata.py 3 DCS fixes + per-frame dbuff, BRINGUP.md). Pause point: panel HW is FINE (p-boot lights it);
the from-scratch DSI is a genuine deep wall after exhaustive parity.

**2026-06-22 — THE p-boot RUNTIME REGISTER DUMP DONE (the definitive experiment) → REGISTER HYPOTHESIS
COMPREHENSIVELY DEAD; bug is RUNTIME TIMING/SEQUENCING, not a register value.** Did NOT rebuild p-boot from
source (the handoff's plan) — megous git (megous.com / git.xff.cz) has BROKEN TLS, all fetch routes dead
(https/git/SNI-override/HTTP1.1/cgit-snapshot/GitHub); p-boot needs a stripped *modified* U-Boot v2020.04 at
src/uboot anyway. **BETTER UNBLOCKED ROUTE (use this):** read p-boot's live registers FROM OUR OWN PAYLOAD
running under p-boot — in PBOOT mode panel_init is skipped + fb_init only ADOPTS the FB, so p-boot's DE2/
TCON0/DSI/DPHY are live+untouched at handoff. Added (kernel.s/panel.inc.s, PANELDBG, harness-safe, default
render byte-identical 4922-B PNG): an **early `[pboot-ref]` dump** (pre-fb_init, pbootdbg only) + **expanded
`dump_tbl` 35→~90 regs** FIFO-safe (added CCU PLLs/dividers/bus-gates/resets, full DSI video-timing block
0xB0-E4, INST_FUNC0-5, DPHY ANA0-4, DE2 blender/overlay details). Diff tool `C:\Users\arin\pp_regdiff.py`.
Captured BOTH on real HW (full card shuffle each): **p-boot card** (rebuilt via ~/pboot/build-pboot-card.sh
w/ conf/Image=pbootdbg) → `C:\Users\arin\pboot-regs.log` (+devbuntu ~/pine-uboot/pboot-regs.log); **native
U-Boot card** (~/pine-uboot/make-native-card.sh = dd pine-fresh.img + flash-paneldbg.sh) → native-regs.log.
**RESULT of the value-by-value diff: 78/89 regs IDENTICAL.** The 11 diffs: 8 EXPECTED (FB geom/addr — native
027f01df/40400000/pitch780 vs p-boot 059f02cf/48000000/b40; RED BLD_BKCOL diag; volatile TCON0 GINT0/block-
counter). 3 "suspect" = CCU bus gate/reset, ALL identified as **NON-display red herrings**: 0x064/0x2c4
bit21 = **bus-msgbox** (p-boot has it on for ATF/SCP mailbox), 0x2c0 bit9 = **RST_BUS_MMC1** (native has MMC1
out-of-reset because U-Boot loaded from SD). Confirmed bit IDs via Linux ccu-sun50i-a64.c (WebFetch). Also
killed the INST_FUNC0 lead (LANE_CEN bit4: native=0x0F=p-boot, our dsi_start's clrbits DOES stick) and
verified offline the 7 un-dumped DSI inst-engine regs (INST_LOOP_SEL 0x30000002, LOOP_NUM0/1 0x00310031 since
non-burst delay=49, JUMP_CFG 0x00560001) ALL match p-boot source. **=> EVERY display-path register (CCU
display clocks/PLLs, DE gates, DE2 mixer/blender/overlay, TCON0 all-timing, DSI host incl full video timing +
all INST regs, DPHY incl ANA) is BYTE-IDENTICAL working-p-boot vs broken-native. The divergence is NOT a
register value.** Identical static config + panel alive (DCS 0x1c) + TCON0 scanning + DSI INSTRU_EN + DE2
enabled, yet no pixels incl RED backdrop → **the bug is RUNTIME TIMING / SEQUENCING** (the one thing register
snapshots can't see). **NEXT (recommended): port p-boot's EXACT dsi_init sequence — its udelay MICROSECOND
timings (we use coarse delay_ms) + its operation ORDER** (p-boot display_init = tcon0_init→dsi_init→de2_init
then display_commit; we interleave differently + call fb_init/DE2 separately after a 160ms delay). p-boot src
NOW on devbuntu ~/p-boot-src (cloned; src/display.c is ground truth; build needs gcc-aarch64-linux-gnu+php+
ninja = INSTALLED, + the missing modified u-boot at src/uboot). Lower-value: dump the few non-canonical sub-
regs not yet sampled. Tooling all staged on devbuntu; toolchain installed; dpkg repaired (old stuck-apt gone).
GOTCHA: the udev write-blocker re-arms RO mid-write (bit p-boot-conf) — the build scripts now temporarily
disable /etc/udev/rules.d/99-usb-storage-readonly.rules w/ trap-restore.
**CONFIRMED via a SECOND fuller sweep (dump_all: dump_range walks each block's whole config range — DE/DE2-
glb/blender0x1000-10A0/overlay/DSI0x00-EC/DPHY0x00-60 + curated TCON0+CCU; ~183 regs; logs pboot-regs2.log/
native-regs2.log):** 168/183 match. The ONLY extra diffs the wider sweep revealed = DE2 blender **pipe1**
(FCOLOR 0x1101014 / INSIZE 0x1101018 / COORD 0x110101c / MODE 0x1101094) which p-boot sets (it composites its
GUI with 2 planes) but we don't — **INERT: pipe1 is DISABLED in both (PIPE_CTL 0x1101000=0x101, bit9 clear),
our single UI layer routes ch1->pipe0 which is byte-identical**. Plus the same red herrings (MMC1 gate/rst bit9
@0x60/0x2c0; MSGBOX gate/rst bit21 @0x64/0x2c4) + FB geom/RED-diag/volatile. **=> ZERO display-relevant
register diff across the FULL sweep. Register hypothesis EXHAUSTIVELY dead; bug = runtime timing/sequencing.**
Committed b06cdc1 (first finding) + the dump_all tooling. NEXT = port p-boot's exact dsi_init udelay timings +
op order (see above).

**2026-06-22 (cont.) — DID the directed experiment: REORDERED bring-up to p-boot's exact display_init() op order
(code-only, AWAITING HW).** Established first that our delays are all LONGER than p-boot's udelay µs (safe — timing
is NOT the bug) and values are byte-identical, so the only residual is RUNTIME ORDER. p-boot brings each block's
clocks up JUST BEFORE using it; we had front-loaded everything in one clk_init + an assert→deassert reset-cycle
p-boot never does. **Refactored panel.inc.s:** split clk_init into 4 per-block helpers — tcon0_clk_init (PLL_VIDEO0/
PLL_MIPI LDO→settle→full / TCON0 src+gate+reset), dsi_bus_clk_on (DSI bus gate/reset before host regs), dphy_clk_on
(CCU_MIPI_DSI_CLK = dphy_enable's first act), de_clk_init (SRAM-C→DE / PLL_DE+lock / DE clock+gate+reset) — each
called at p-boot's equivalent point in panel_init. KEY: **PLL_DE/DE clock now come up LAST** (after dsi HSD, right
before fb_init's mixer), matching de2_init order, instead of early. Resets are now **deassert-only** (dropped the
assert-cycle = our old environmental guess that never lit it → clean test of exact p-boot order). Register VALUES
unchanged; only WHEN each clock comes up + reset style differ. All polls (PLL_DE lock @0x48, RSB, DSI INSTRU_EN)
keep same addrs → harness unaffected. **Regression GREEN: default + paneldbg both render byte-identical 4922-B PNG
end-to-end, no fault/hang** (paneldbg needs ~1500M-instr budget to clear its LED-beacon delays). Built
unodos_paneldbg.bin 30704B md5 07f0d035… → STAGED devbuntu:~/pine-uboot/unodos_paneldbg.bin. **NEXT NEEDS USER+PHONE:**
make-native-card.sh (dd pine-fresh.img + swap payload), serial bg-cat, LONG-press power-cycle; read if panel LIGHTS.
If still backlit-black, "exact p-boot order" is disproven too → next wedge = **DE2-output-vs-DSI-HS-transmit splitter**
(does the DE2 mixer emit pixels vs do the HS lanes carry them) or the few un-sampled sub-regs. Full writeup in
PINEPHONE-BRINGUP.md §8 (2026-06-22 cont.). Code NOT committed (panel.inc.s clock refactor + panel_init reorder,
kernel.s comment, BRINGUP.md).
**HW RESULT (2026-06-22): STILL BACKLIT-BLACK — "exact p-boot order" DISPROVEN.** Native card serial trace: LED
beacon clean all stages; ALL 3 PLLs locked (incl PLL_DE=91001701 brought up LAST); panel ALIVE+display-ON
(RXCTL=02060003 RX_FLAG, RXDAT=...011c pm=0x1c); DSI_BASIC_CTL0=00030001 (HS); DE2_GLB_CTL=1 OVL_TOPADD=40400000;
TCON0_GINT0 a00→800→800 (scanning); mainloop, no fault. IDENTICAL to every prior run — reorder changed nothing. So
register VALUES + ORDER + DCS-seq ALL byte-identical to working-p-boot, panel proven alive, yet no video incl RED
backdrop → residual is provably NOT in the DE2/TCON0/DSI/DPHY/CCU register+sequence space. **KEY REFRAME: ST7703 is
a VIDEO-MODE panel w/ NO GRAM** — shows the live MIPI stream in real time, so backlit-black = no valid RED video
arriving (also why all_pixels_on 0x23 stayed black: nothing to "turn on"). RED backdrop is DE2-pipe-internal (no
DRAM/MBUS fetch) so its absence RULES OUT framebuffer-DMA/MBUS → wall narrows to **DE2-not-emitting-pixels vs
DSI-HS-lane/DPHY-analog-not-transmitting** (LP works, HS is separate analog). **NEXT = DIFFERENTIAL PERTURBATION
test** (supersedes static diffing): boot PBOOT (p-boot lights panel), re-run ONE of our blocks on top, see if screen
goes black. FIRST: PBOOT + re-run our native fb_init DE2 programming on p-boot's known-good TCON0/DSI/DPHY/clocks —
black ⇒ our DE2 config is the bug; shows RED/launcher ⇒ our DE2 is fine, bug is in our TCON0/DSI/DPHY/clock bring-up
(wrong clock RATE despite right register bits, or an analog enable that doesn't engage). One cycle bisects the whole
suspect space.
**HW RESULT (2026-06-22): DE2 EXONERATED — the differential test WORKED.** Built `de2test` (build.sh de2test =
PBOOT+PANELDBG+DE2TEST; kernel.s fb_init: `.ifndef DE2TEST` skips the adopt path so it falls through to native DE2
on top of p-boot's live pipe). On HW: p-boot lit panel → our payload → **WHITE rect top-left then the UnoDOS LAUNCHER
RENDERED.** Trace: our overlay live (011030xx ATTR ff000405, SIZE 027f01df=480x640, OVL_TOPADD=40400000=OUR FB not
p-boot's 48000000) scanning out via p-boot's TCON0 (GINT0 a00→800) + DSI (BASIC_CTL0=00030001). So **our native DE2
mixer config is CORRECT** when fed p-boot's known-good TCON0/DSI/DPHY/clocks. **=> bug is UPSTREAM: our native
panel_init {clocks, TCON0, DSI-host, DPHY, dsi_start}** (st7703/panel already exonerated — panel answers DCS + alive).
Likely a wrong clock RATE despite identical register bits, or a D-PHY HS analog enable that doesn't actually engage
(LP works, HS is a separate analog path). de2test card = p-boot card w/ conf/Image=de2test (473bd851). NEXT differential
= add our native panel_init (or just DSI+DPHY, the leading HS-transmit suspect) on top of p-boot, see which block
breaks the working display. GOTCHA HIT: build-pboot-card.sh's per-write `blockdev --setrw` RACES udev re-arming RO
(p-boot-conf "Operation not permitted") — FIX = `sudo mv /etc/udev/rules.d/99-usb-storage-readonly.rules` aside +
`udevadm control --reload` for the whole build, then restore (make-native-card.sh's dd-only path doesn't trip it; only
the sfdisk re-partition in build-pboot-card.sh does).
**HW RESULT 2 (2026-06-22): pbootnative = BLACK → bug CONFIRMED in our panel_init code (not environment).** Built
`pbootnative` (build.sh = PBOOT+PANELDBG+DE2TEST+NATIVEPANEL; kernel.s NATIVEPANEL re-runs full panel_init on top of
p-boot's warm state, then native DE2). On HW: p-boot lit panel → our panel_init ran (LED beacon) → **screen BLACK.** So
our full panel_init tears down p-boot's working pipe and rebuilds it broken ⇒ **bug is in our panel_init {clocks/TCON0/
DSI-host/DPHY/dsi_start}, definitively our CODE.** Bonus: since p-boot's PLL_PERIPH0/MBUS/DRAM were the live env and our
panel_init STILL failed, those inherited-environment items are RULED OUT — it's what panel_init itself programs. NEXT
bisection STAGED: `pbootdsi` (build.sh = +DSIONLY; kernel.s calls new `panel_dsi_only` instead of panel_init). panel_dsi_only
(panel.inc.s) re-inits ONLY the DSI link (dsi_bus_clk_on+dsi_host_init+panel_reset+dphy+st7703+dsi_start) on p-boot's
clocks+TCON0 — does NOT touch PLLs/TCON0/DE. **black ⇒ bug in our DSI-host/DPHY/dsi_start (leading HS-transmit theory);
launcher ⇒ DSI link fine, bug in our clocks/TCON0.** pbootdsi.bin 30856B. The pN (clk-reprogram on live PLL) branch is
deferred — DSI cut is cleaner (no live-PLL disruption).
**HW RESULT 3 (2026-06-23 ~00:30): pbootdsi = BLACK → BUG PINNED to our DSI LINK bring-up (DSI-host/DPHY/dsi_start).**
After 2 false starts (jack loose → 0 bytes; then SD not seated → BROM booted eMMC postmarketOS U-Boot 2021.01 instead of
our card — watch for p-boot splash not "U-Boot SPL" to confirm the card took), the good run: LED settled CYAN (mainloop
reached), SCREEN BLACK. Trace: [dsi_host] ok, [dphy] ok, [st7703] ok, RXCTL=02060003 + RXDAT=0a00011c (panel STILL
answers DCS, display-ON, AFTER our DSI re-init), DSI_BASIC_CTL0=00030001 (HS), TCON0 GINT0 a00→800 (p-boot's TCON0 still
scanning), mainloop. **DECISIVE: de2test (p-boot's DSI + our DE2)=LAUNCHER vs pbootdsi (OUR DSI + our DE2)=BLACK — the
ONLY delta is our DSI link re-init (dsi_host_init+dphy_init+st7703+dsi_start on p-boot's clocks+TCON0). So the bug is our
DSI/DPHY/dsi_start HS VIDEO path.** LP works (panel replies DCS), HS doesn't deliver pixels → classic HS-transmit failure.
Registers all verified-matching p-boot → it's a RUNTIME/ANALOG diff in those 3 routines, NOT a value. CAVEAT: re-running
dphy_init on p-boot's already-powered DPHY could be a re-power-glitch artifact (dphy_tbl writes ANA2=0x2 clearing CK_CPU/
P2S_CPU then rebuilds) — but cold-native is ALSO black, and the DSI link is the consistent discriminator, so it's the
strong lead. **NEXT (next session): cleanest sub-bisect = re-run ONLY dsi_start (HS handoff: JUMP_SEL HSC 0x00000F02 →
clear LANE_CEN bit4 → 1ms → JUMP_SEL HSD 0x63F07006) on p-boot's fully-working DSI — low-artifact (no panel reset, no
DPHY re-power). black ⇒ our dsi_start HS handoff is the bug; survives ⇒ it's dsi_host_init or dphy_init.** Then scrutinize
the HS clock-lane continuous mode (DPHY TX_CTL HS_TX_CLK_CONT bit28 + dsi_start's LANE_CEN clear) — the HS-clock-lane
control is the likeliest runtime culprit. Diagnostic builds: build.sh {de2test,pbootnative,pbootdsi}; flags DE2TEST/
NATIVEPANEL/DSIONLY in kernel.s, panel_dsi_only in panel.inc.s. ALL code still uncommitted (working tree). It is now
2026-06-23 past midnight — MANY HW cycles done; good wrap point with bug localized to ~3 routines.
**HW RESULT 4 (2026-06-23): pbootdsistart = LAUNCHER → dsi_start EXONERATED. Bug now = dsi_host_init OR dphy_init.**
`pbootdsistart` (build.sh = +DSISTARTONLY → panel_dsistart_only: re-runs ONLY dsi_start = JUMP_SEL HSC→clear LANE_CEN→
1ms→JUMP_SEL HSD, on p-boot's fully-working DSI, NO panel reset / NO dphy re-power / NO dsi_host rewrite) → white-screen-
then-LAUNCHER. So our dsi_start HS handoff is FINE. Remaining 2 suspects: dsi_host_init, dphy_init. **NEXT STAGED:
pbootdsihost** (build.sh = +DSIHOSTONLY → panel_dsihost_only: dsi_bus_clk_on + dsi_host_init + dsi_start, leaving p-boot's
DPHY untouched; low-artifact config rewrite). **black ⇒ dsi_host_init is the bug; launcher ⇒ dphy_init is the bug BY
ELIMINATION** (which sidesteps the dphy re-power-glitch artifact a direct dphy test would have). pbootdsihost.bin 30984B
md5 6ac4786b staged → conf/Image. Flags now DSISTARTONLY/DSIHOSTONLY in kernel.s, panel_dsistart_only/panel_dsihost_only
in panel.inc.s. Strong prior: dphy_init (analog HS path) given LP-works/HS-fails + dsi_host values verified-matching.
**HW RESULT 5 (2026-06-23): pbootdsihost = LAUNCHER → dsi_host_init EXONERATED. dphy_init is the bug (pending airtight
confirm).** So now CLEARED individually: DE2, dsi_start, dsi_host_init. pbootdsi (full DSI link incl dphy)=black. By
elimination dphy_init is THE bug. CAVEAT: pbootdsi also did panel-reset + st7703 re-DCS that pbootdsihost skipped, so to
be airtight built **pbootnodphy** (build.sh = +NODPHY → panel_nodphy_only: the WHOLE DSI link MINUS dphy = dsi_host +
reset + st7703 + dsi_start, keeping p-boot's D-PHY). **launcher ⇒ dphy_init DEFINITIVELY the bug; black ⇒ it's our panel-
reset/st7703 path instead.** pbootnodphy.bin 31120B md5 e96a4c49 staged → conf/Image. Default regression STILL byte-ident
4922B (all diag flags isolated). **THE FIX LEAD once dphy confirmed:** dphy_init values+order+ are byte-identical to
p-boot dphy_enable; the SOLE difference is our delays = delay_ms(1)=1000us where p-boot uses udelay(5) then udelay(1)×5.
So first fix to try = add a delay_us (cntpct: us*24 ticks) + make dphy_init use p-boot's EXACT udelay(5)/udelay(1)
timings (the analog LDO/bias ramp may be timing-sensitive in a way 1000×-too-long delays break). Flags now incl NODPHY,
panel_nodphy_only in panel.inc.s. ~6 HW cycles done 2026-06-23; bug ~1 routine from solved.
**HW RESULT 6 (2026-06-23): pbootnodphy = BLACK → REDIRECT: dphy is NOT the bug; the bug is our PANEL RESET +
st7703_init (DCS panel init).** Clean pair: pbootdsihost (dsi_host+dsi_start, NO panel touch)=LAUNCHER vs pbootnodphy
(dsi_host + panel_reset + st7703 + dsi_start, NO dphy, on p-boot's D-PHY/clocks/TCON0)=BLACK. The ONLY delta =
panel_reset_low/high + st7703_init. **So OUR panel re-init breaks the display** — every test that leaves the panel's
init alone (de2test/dsihost/dsistart) works; every test where WE reset+re-DCS it (pbootdsi/pbootnodphy) goes black, even
on p-boot's known-good dphy/clocks/tcon0/dsi-host. Explains NATIVE perfectly (cold boot MUST reset+init the panel
itself). **The "panel is alive/exonerated" assumption was WRONG**: panel answers DCS (0x1c display-ON) but answering ≠
correctly configured for video; XBD599 is video-mode (no GRAM) so a subtly-wrong SETMIPI/SETGIP/gate-timing leaves it
display-ON but BLACK. So the bug is in {panel_reset_low/high timing, st7703_init / the dsi_init_seq DCS blob, dcs_send
transmission}. NOTE: prior sessions "byte-verified st7703 vs p-boot's dsi_panel_init_seq + fixed 3 transcription bugs"
and dcs_send was verified — so re-scrutinize with FRESH eyes: exact reset pulse timing/width vs p-boot (gpio PD23 low→
15ms→[dphy]→high→15ms→DCS), per-command + post-SLPOUT 120ms delay PLACEMENT, dcs_send mode/framing for the LONG packets
(SETGIP1=64B/SETGIP2=62B/SETGAMMA), and whether any command/order differs from p-boot panel_dcs_seq_initlist (21 entries:
20 DCS + 1 sleep). Can't cheaply split reset-vs-st7703 on HW (reset-alone = blank panel). **~7 HW cycles done, past
midmnight 2026-06-23 — STRONG stop point: bug localized from "everything" to the panel reset + st7703 DCS init (2
routines), a surprise redirect off the dphy/HS-analog theory. ALL diagnostic code uncommitted (working tree).**
**ANALYSIS (post-commit e3f760d): st7703 is PROVABLY CORRECT → pbootnodphy black is likely a panel-RESET artifact.**
Re-ran cmp_st7703.py (C:\Users\arin\) = ALL 20 cmds byte-identical to p-boot's pre-framed dsi_panel_init_seq (incl SETGIP1
64B/SETGIP2 62B gate tables). Also verified dcs_packet framing (1B→0x05,2B→0x15,3+→0x39+CRC), command ORDER, DELAYS
(120ms post-SLPOUT), and dcs_send FIFO content ALL match p-boot; the only diffs are benign (dcs_send CMD_CTL RMW→0x06000209
vs p-boot plain 0x09 — equivalent since status bits are W1C; +20ms after DISPON). Reset timing also matches (PD23 low 15ms→
high 15ms). **So our panel init genuinely matches p-boot → pbootnodphy=black is most likely a panel-RESET PERTURBATION
ARTIFACT (tearing down the panel mid-running-warm-HS-pipe), NOT a real st7703 bug.** This SOFTENS HW RESULT 6: cleanly
exonerated (low-artifact) = DE2, dsi_host_init, dsi_start, st7703-DATA; artifact-prone/untested = clocks, TCON0, dphy_init,
panel-reset-timing. **NEXT STAGED: pbootst7703** (build.sh = +ST7703ONLY → panel_st7703_only: re-send st7703 DCS on
p-boot's lit panel WITHOUT a reset; low-artifact). **launcher ⇒ st7703 fine, pbootnodphy was the reset artifact → bug is
reset-timing or back in clocks/TCON0/dphy; black ⇒ dcs_send transmission corrupts the panel despite correct data.**
pbootst7703.bin 31208B md5 78b269a8. Flags now incl ST7703ONLY, panel_st7703_only in panel.inc.s (uncommitted since e3f760d).
**HW RESULT 7 (2026-06-23): pbootst7703 = LAUNCHER → st7703 EXONERATED (data+TX); pbootnodphy black WAS the panel-reset
artifact, confirmed.** Re-sending our DCS on p-boot's lit panel WITHOUT reset = launcher. So CLEANLY PROVEN CORRECT now:
DE2, dsi_host_init, dsi_start, st7703 (data+transmission). The panel path is fully cleared. **Refined suspect set = our
{clocks, TCON0, dphy_init}** — every working test used p-boot's versions of these; native uses ours. clocks LOCK + TCON0
SCANS (both likely fine) → **dphy_init (D-PHY analog) is the prime suspect AND the only block never isolated.** NEXT STAGED:
**pbootdphy** (build.sh = +DPHYONLY → panel_dphy_only: our dphy_clk_on+dphy_init+dsi_start on p-boot's clocks/TCON0/dsi-
host/panel, no reset). **launcher ⇒ our dphy fine → bug is clocks/TCON0 (rate issue); black ⇒ dphy_init is the bug**
(modulo re-power glitch: dphy_tbl writes ANA2=0x2 briefly clearing CK_CPU/P2S_CPU then rebuilds). If dphy implicated, the
fix lever = its delays (delay_ms(1)=1000us vs p-boot udelay(5)/udelay(1)) — add delay_us + match p-boot exactly. pbootdphy.bin
31288B md5 126c5887. Flags incl DPHYONLY, panel_dphy_only. ~8 HW cycles 2026-06-23. NOTE: devbuntu passwordless sudo
expired mid-session → user re-armed via `sudo passwordless 1h` (needed for card builds).
**HW RESULT 8 (2026-06-23): pbootdphy = LAUNCHER → our dphy_init EXONERATED (even with the 1ms delays).** So FIVE blocks
now cleanly proven correct: DE2, dsi_host_init, dsi_start, st7703(data+TX), dphy_init. **Every working test used p-boot's
CLOCKS + TCON0; native uses ours → the bug is in our {clocks, TCON0} (+ possibly panel-RESET+cold-init, the one thing
never cleanly isolated — pbootnodphy black is reset-on-warm OR genuine, ambiguous; but our reset+pmic ORDER matches p-boot
exactly: PD23 low→pmic→15ms→dphy→PD23 high→15ms→DCS).** clocks LOCK (PLLs 91xxxxxx) + TCON0 SCANS (GINT0 toggles) + all
their regs verified byte-identical to p-boot, yet one is the bug → a subtle RATE/sequencing thing registers can't show
(e.g. PLL_MIPI actual freq, or TCON0 pixel timing). **MASSIVE narrowing: from the whole pipeline to clocks-or-TCON0.**
The dphy-timing-fix lead is now MOOT (dphy exonerated). NEXT (next session, both options disruption-aware since reprogram-
ming live PLL/TCON0 glitches the pipe): (a) clk+TCON warm test (reprogram OUR clocks+tcon0 on p-boot's dsi/dphy/panel →
launcher = clocks/TCON0 fine, bug is panel-cold-reset; black = clocks/TCON0, modulo disruption); (b) careful RATE analysis
of PLL_VIDEO0→PLL_MIPI→TCON0-DCLK vs p-boot (all regs match, so look for a derived-rate or lock-sequence subtlety); (c)
cold-native A/B of candidate clock/TCON0 tweaks. ~9 HW cycles, very late 2026-06-23 — deep-stop with bug down to 1-2
routines (clocks/TCON0). All diag flags (DE2TEST/NATIVEPANEL/DSIONLY/DSISTARTONLY/DSIHOSTONLY/NODPHY/ST7703ONLY/DPHYONLY +
panel_{dsi,dsistart,dsihost,nodphy,st7703,dphy}_only) COMMITTED 2026-06-23: e3f760d (reorder + first cuts) + eac8bb9
(st7703/dphy cuts → clocks/TCON0). Working tree clean.
**NEW STRATEGY (warm-perturbation exhausted — it CAN'T cleanly isolate clocks/TCON0: live PLL/timing-master reprogram
glitches the pipe regardless; all their regs already match p-boot). Switch to MEASURE-BEHAVIOUR + WIDEN-COMPARISON
(full writeup PINEPHONE-BRINGUP.md §8 "NEW STRATEGY"):** (1) **On-chip RATE measurement** — count cntpct_el0 (fixed
24MHz) ticks between TCON0 GINT0 bit11 vblank latches = the actual frame period, in native vs PBOOT(p-boot); ≠ ⇒ a
clock is at the wrong RATE despite matching regs (trace PLL_VIDEO0→PLL_MIPI→TCON0 DCLK), = ⇒ NOT a rate bug, redirect to
TCON0 video/CPU-IF logic or DSI↔TCON0 sync. Non-disruptive. (2) **FULL CCU sweep** 0x01C20000–0x202FF (every PLL/gate/
divider/mux) p-boot vs native — dump_all only does a curated CCU subset; catch a missed clock-reg divergence. (3)
**Recompute derived timing** — p-boot computes video_start_delay/TCON0 start_delay/BASIC_CTL1 from panel constants at
runtime; we HARDCODED them → re-derive p-boot's formulas for XBD599 + verify our literals (a math slip passes the dump:
wrong-vs-wrong). (4) Pivot: rebuild p-boot from source w/ dump_de2/dsi + clock-state dumps for its EXACT runtime clock
tree. PRIORITY #1 then #2 (cheap, extend existing PANELDBG dump/measure). Bug is ~1-2 routines (clocks or TCON0) from
solved.

**2026-06-25 — NEW STRATEGY EXECUTED on HW + two more probes → EVERY SoC-side avenue EXHAUSTED; bug RELOCATED to the
COLD panel-init/panel-response (the one thing warm-perturbation can't test).** Full writeup PINEPHONE-BRINGUP.md §8
(2026-06-25 entries); 6 commits 33d3279→5aec9d3 on parity-push-fresh-ports (this session's work IS committed). Implemented
`measure_frame_period`(cntpct ticks between TCON0 GINT0 bit11 vblank latches, bounded), `dump_ccu_full`(0x01C20000-0x202FF
192 words), `dump_tcon0_full`(0x00-0x60+0x70-0x230, skips CPU-IF data ports 0x64-0x6c), `dump_pmic`(AXP803 rails over RSB),
all appended to dump_all/mainloop, PANELDBG-only, harness-clean (default render byte-identical 4922-B). New probe routine
`panel_clktcon0_only` + flag CLKTCON0ONLY (build.sh pbootclktcon0) + `panel_dphy_only` etc. already existed.
**RESULTS (native paneldbg card vs p-boot pbootdbg/pbootclktcon0 card, ~4 HW boots):**
- **#1 RATE: IDENTICAL.** native FRAME_TICKS_1≈0x6095E=395,614 (60.665Hz) vs p-boot≈0x60603=394,755 (60.797Hz) = 0.2%
  jitter. → NOT a clock-rate bug. (Naive HTOT·VTOT·dotclk=399,960/60.006Hz was ~1% off both — expected, panel runs TCON0
  in 8080/TRI mode not plain video; native-vs-pboot equality is the valid test, they match.)
- **#2 FULL CCU + every swept block: display tree IDENTICAL.** Comprehensive native-vs-pboot diff (362 shared regs): DSI
  (full 0x00-0xEC)=0, DPHY(full)=0, DE-top=0, TCON0 config=0 (only live GINT0/TRI1 differ). CCU full=9 diffs ALL
  non-display (CPU PLL 0x00, DISABLED PLL_PERIPH1 0x50, MMC0/1/2 gates/clocks/resets 0x60-bit9/0x64-bit21/0x8c/0x90/
  0x2c0/0x2c4, one PLL bias 0x250). **Full TCON0 sweep (138 regs): only live status differs.** → no register surface left.
- **#3 COMPUTED TIMING (analytic): all match.** Re-derived p-boot's formulas for XBD599 (HTOT=808,VTOT=1485,72MHz,4lane,
  BURST=0): BASIC_CTL1 vsd=1468→0x5BC7 ✓, TCON0 TRI2 start_delay=(VTOT-VDISP-11)·HTOT·149/(CLK/1000)/8=7106→0x1BC2000A ✓,
  TCON_DRQ=((HSS-HDISP-20)·24/32)=7→0x10000007 ✓, INST_LOOP_NUM=0x310031 ✓, INST_LOOP_SEL=0x30000002 ✓. → no math slip.
- **PROBE clk+TCON0 WARM CUT (pbootclktcon0): LIT.** Re-ran our tcon0_clk_init(incl PLL_MIPI re-lock)+tcon0_init+
  dsi_bus_clk_on+dphy_clk_on+de_clk_init on p-boot's live pipe → launcher stayed up (even re-locking PLL_MIPI mid-scanout).
  → our clocks+TCON0 sequence is SAFE (survival is decisive; the last block-group warm-testable). **This OVERTURNS the
  earlier "bug is in clocks or TCON0" elimination** — that was reached because warm-perturbation never tested clk+TCON0;
  now it has, and they're fine.
- **PROBE PMIC (dump_pmic): display rails IDENTICAL.** native vs p-boot: DLDO2(MIPI/panel)=0x0b=1.8V both, DLDO1=0x1a both,
  GPIO0LDO=0x1a both, both DLDO1+DLDO2 enabled. Only diff = reg 0x12 bits5-7 (non-display DLDO3/4 U-Boot leaves on). → power
  exonerated.
- **COLD panel-init re-derived (analytic): order/delays/transmission ALL faithful.** p-boot panel_init = 18 cfg DCS delay-0,
  SLPOUT, 120ms, DISPON; reset-LOW+PMIC+15ms→…→dphy→reset-HIGH→15ms→DCS→HSC→1ms→HSD; long-pkt mipi_dsi_dcs_write =
  header@TX_REG(0)+data+CRC@TX_REG(1+i), CMD_CTL=total-1. OURS matches all of it (our dcs_send pushes contiguous
  [header][payload][CRC] from TX_REG(0), CMD_CTL=pkt_len-1; cmp_st7703.py already proved the 20 cmds byte-identical).
**CONCLUSION: EVERY SoC-side thing (all display regs, rate, computed timing, PMIC rails, clk+TCON0 safe, cold-init
reset/delays/order/FIFO-transmission) matches working-p-boot, yet cold boot is backlit-black.** By total elimination the bug
is the **panel's actual RESPONSE to our COLD init** — the ONE op warm-perturbation can't test (resetting p-boot's live
scanning panel is destructive; pbootnative/pbootnodphy black = reset-perturbation ARTIFACT, not a clk/TCON0/dphy fault).
pbootst7703(lit) only proved DCS data+TX on an ALREADY-RUNNING panel; the reset→configured transition is untested and
cold-native (which must do it) is the only failing boot. **NEXT (needs NEW HW instrumentation, not analysis): extend
dcs_read_dbg to READ BACK ST7703 panel config/status (RDDPM 0x0A, RDDSDR, gate-driver/SETGIP/address-mode/pixel-format
readable regs) AFTER cold st7703_init, compare to p-boot's warm panel — a panel reg reading DIFFERENT ⇒ that DCS cmd didn't
take effect cold (panel-state/timing dep); ALL panel reads matching yet black ⇒ fault is the HS VIDEO delivery
(DE2→TCON0→DSI HS pixels) post-DISPON, not the DCS config.** Traces saved pinephone/shots/trace-{native,pboot}-*.log;
builds staged devbuntu:~/pine-uboot/{unodos_paneldbg,unodos_pbootdbg,unodos_pbootclktcon0}.bin + ~/pboot p-boot card tooling.
RIG NOTE: amanuensis→devbuntu SSH drops mid-command (exit 255) — robust pattern = write a script on devbuntu, launch via
`at -f <script> now` (decouples from SSH), poll a marker/log with short SSH calls; used for serial capture (/tmp/cap*.sh→
/tmp/pine-*.log) AND card builds (/tmp/mk-{native,pboot}.sh, which mv the udev write-blocker rule aside for the whole build
then restore via trap).

**2026-06-25 (cont.) — PANEL DCS-READBACK PROBE IMPLEMENTED + staged (code-only, AWAITING HW).** Built the panel-side
probe the prior entry called for (read the PANEL's own state cold-vs-warm — every SoC-side avenue is exhausted).
**mkdata.py** emits `dsi_read_seq` = a table of (reg, read-request-header word) pairs for the ST7703's STANDARD MIPI DCS
readable regs: RDDPM 0x0A, RDDMADCTL/address-mode 0x0B, RDDCOLMOD/pixel-format 0x0C, RDDIM 0x0D, RDDSM/signal-mode 0x0E,
RDDSDR/self-diag 0x0F, + RDID1/2/3 0xDA/DB/DC (link-sanity baseline). Header={DI=0x06,reg,0x00,ECC} reuses `mipi_ecc`
(emitted 0x0A hdr=0x3F000A06 == the old hardcoded literal → validates ECC). **KEY LIMITATION:** the panel's manufacturer
command-set regs (SETMIPI 0xBA, SETGIP1/2 0xE9/0xEA…) are WRITE-ONLY/not DCS-readable, so the gate-driver/SETGIP config
can't be read back directly — RDDSM/RDDPM/RDDSDR are the proxies (reflect whether the panel believes it has a valid
display/signal post cold-init). **panel.inc.s:** factored the read mechanics out of `dcs_read_dbg` into **`dcs_read_raw`**
(in: header word; out: w0=RXCTL, w1=RXDAT; bounded poll) — dcs_read_dbg's single-0x0A liveness output UNCHANGED, 4 callers
unaffected; added **`dcs_panel_dump`** (walks dsi_read_seq, prints `PANEL_<reg>=<RXCTL>,<RXDAT>`). Wired BOTH sides of the
A/B: native cold path calls it after st7703_init (LP, before dsi_start) in panel_init; warm p-boot path calls it in the
`[pboot-ref]` block in kernel.s (after dump_all, before fb_init). **Verified:** default 26208B / paneldbg 32728B /
pbootdbg 32736B all assemble clean; **default regression renders byte-identical 4922-B launcher** (cmp vs shots/m1_boot.png
✓); paneldbg AND pbootdbg both run end-to-end in Unicorn harness no hang/fault (new reads hit the bounded poll timeout once
in the MMIO sink; UART is a harness no-op → trace is HW-only). **NEXT (needs user+phone+serial; fresh native+p-boot cards,
card is in the phone): flash unodos_paneldbg.bin (native U-Boot card) + unodos_pbootdbg.bin (p-boot card), capture both
serial traces, diff the `[paneldump]` PANEL_xx= lines cold-vs-warm.** Reg DIFFERENT cold-vs-warm ⇒ that DCS config didn't
take on the cold panel (the bug) → chase its cold-init timing; ALL panel reads MATCH yet native black ⇒ DCS config fine,
fault is post-DISPON HS VIDEO delivery (DE2→TCON0→DSI HS pixels) under cold conditions → pivot to instrumenting that. Code
uncommitted on parity-push-fresh-ports (mkdata.py, panel.inc.s, kernel.s, PINEPHONE-BRINGUP.md §8).

**REAL-HARDWARE: DOES NOT BOOT — SCREEN STAYS FULLY DARK (user-confirmed 2026-06-18).**
"Fully dark" (no backlight) is the key clue: U-Boot's pinephone_defconfig lights the
panel ITSELF, so dark = the fault is UPSTREAM of our payload (U-Boot/SPL/power), NOT in
kernel.s. On the original PinePhone the **microSD has boot priority over eMMC** (SD→eMMC
→SPI→FEL; this is a PinePhone *Pro* problem, not ours), so eMMC shadowing is ruled out.

**2026-06-18 debug session (no serial cable available) — what was VALIDATED + BUILT:**
- **Boot artifacts are STRUCTURALLY VALID** (so the BROM would accept the card; rejection
  is NOT the cause): in `~/pine-uboot/pine.img` the SPL `eGON.BT0` header is present, its
  **eGON checksum VERIFIES** (computed==stored, STAMP=0x5F0A6C39, len 0x8000), the MBR is
  one FAT32-LBA partition at 1 MiB (sector 0x800), SPL at sector 16 (byte 8196), boot.scr
  magic 0x27051956 OK. So fully-dark ≠ bad header/layout.
- Remaining live suspects: **drained battery** (documented PinePhone no-boot cause),
  **U-Boot hangs before its video init** (DRAM/PMIC), **stale U-Boot on the card** (the
  written image at 16:50 predates a u-boot rebuild at 17:42), or a **bad write / wrong card**.
- **The only no-serial signal channel = FEL mode over USB-OTG.** Built the full FEL kit on
  [[devbuntu-image-writing]]: `sunxi-fel` BUILT FROM SOURCE into `~/.local/bin` (apt was
  unusable — a hung `apt upgrade` has held the dpkg lock for 2.5 DAYS, blocking all apt;
  flagged to user, not killed; built via gcc + a static libfdt.a compiled from the U-Boot
  tree's scripts/dtc/libfdt since no libfdt-dev). `fel-sdboot.sunxi` staged (button-free FEL
  trigger: dd to SPL slot → BROM enters FEL; loads exactly 0x2000 bytes so leftover U-Boot
  FIT is harmless). `pinephone/fel.sh` (probe|uboot|payload|felcard) committed to repo +
  staged at `~/pine-uboot/fel.sh`. Two ready images on devbuntu: **pine-fresh.img** (current
  U-Boot + hardened payload, for a clean normal-boot retry) and **pine-felcard.img**
  (fel-sdboot SPL + POST payload, FAT intact). PATH: added ~/.local/bin to ~/.bashrc.
- **Bisect plan (in README "No-serial bring-up debugging"):** 0) battery/power; 1) re-flash
  pine-fresh.img (kills stale-U-Boot + bad-write); 2) FEL: `fel.sh probe` (SoC alive?) →
  `fel.sh uboot` (panel lights ⇒ U-Boot good, fault is BROM→SD→SPL; dark ⇒ U-Boot can't
  light panel) → `fel.sh payload` (whole chain minus BROM→SPL, shows POST beacon).
- **kernel.s HARDENED (3 changes, all harness-verified, default+POST builds render):**
  (1) **`_start` now disables MMU+D/I-cache itself** (EL-aware via CurrentEL; A64 hands off
  in EL2 after ATF; mask 0x1005 = bits M|C|I), removing the dependence on boot.cmd's
  unverified `dcache off`. (2) **`fb_init` now ADOPTS U-Boot's live framebuffer** — reads
  back OVL_TOPADD+OVL_PITCH U-Boot already programmed (its console is on-screen) and draws
  into that, instead of reprogramming DE2 output size to 480x640 while the TCON clocks the
  native **720x1440** XBD599 panel (a prime blank-screen suspect); falls back to the old DE2
  bring-up if OVL_TOPADD reads 0 (which is what the harness sink returns → fallback stays
  verified). Primitives are pitch-relative so adopting just works (content lands top-left).
  (3) **POST beacon** (`./build.sh post`, `--defsym POST=1`): paints RED(after fb_init)→
  GREEN(after fs_init)→BLUE(pre-launcher)→desktop, ~0.5s each via cntpct_el0, so a freeze
  shows the last stage; pure R/G/B also reveals an R↔B swap (same bug class as the Pi brown-
  bg). Helpers post_fill/post_delay gated by .ifdef POST. Verified RED+GREEN beacons + final
  launcher in pinephone/harness.py at staged instr budgets.
- NEXT (needs the phone + a USB-C cable to devbuntu): run the bisect above. A real **serial
  console** (headphone-jack UART0, 115200 8N1) would still be the gold standard if FEL is
  inconclusive. The cache-coherency caveat below is now mitigated in-payload (change 1).

**2026-06-18 ON-HARDWARE BISECT — ROOT CAUSE FOUND + PAYLOAD CONFIRMED RUNNING.** Flashed
fresh cards (on devbuntu; Carbon flashing FAILED — its USB card-reader + USB-net share a
controller so heavy writes drop the network, plus the laptop sleeps; ALSO Windows Set-Disk
-IsOffline fails on removable media → must FSCTL_LOCK/DISMOUNT the volume; device letters are
UNSTABLE so resolve the reader by MODEL "SD Transcend", never a fixed /dev/sdX — there are also
Ugreen "MassStorageClass" + the RPi UNODOS card in the rig). Findings, in order:
  1. **pine-fresh card → boots postmarketOS (from eMMC).** Original "fully dark" was likely a
     stale/old card; the panel + phone are FINE (pmOS lights the screen).
  2. **Self-diagnosing card (diag boot.scr: loud banner, 5s pauses, HALT loop instead of the
     fall-through `if fatload..;then..go;fi`) → green LED on, screen BLANK, NO pmOS.** Since the
     ONLY diff from pine-fresh is boot.scr, and diag does NOT reach pmOS, our **SD SPL+U-Boot+
     boot.scr ALL run** (if BROM skipped SD, both cards would boot eMMC identically — they don't).
  3. **ROOT CAUSE: mainline U-Boot 2026.07 has NO sunxi/sun6i MIPI-DSI driver** (searched whole
     tree: only stm32/tegra/exynos/dw DSI exist; `# CONFIG_VIDEO_MIPI_DSI is not set`; pinephone_
     defconfig doesn't configure video at all). So U-Boot NEVER lights the DSI panel (no logo, no
     vidconsole — console is serial-only). The port's founding assumption ("boot chain brought up
     the panel, we just program DE2") is **FALSE for PinePhone** — unlike the Pi (VideoCore inits
     HDMI), nothing inits the panel before our payload. Only pmOS's kernel (sun6i-mipi-dsi +
     panel ST7703/XBD599) lights it. THIS is why the screen is dark; CPU/DRAM/boot/payload are fine.
  4. **PAYLOAD CONFIRMED RUNNING on metal via a NEW LED debug channel.** `defconfig` has
     `CONFIG_SPL_SUNXI_LED_STATUS_GPIO=114` = PD18 = the green LED the SPL lights (so green LED =
     SPL ran). Added an `LEDTEST` build (`build.sh led`): payload drives PD18 via A64 PIO
     (PD_CFG2=0x01C20874 pin-18 mode bits[11:8]=001; PD_DAT=0x01C2087C bit18) in an infinite
     ~0.8s blink loop. ON HARDWARE: solid green (SPL) → **BLINKING green** = payload runs all the
     way through. So we have a working NO-serial/NO-display 1-bit debug channel (the LED).
NEXT = **Step 2: bring up the DSI panel IN THE PAYLOAD** (since nothing else will): CCU clocks
(DE2/TCON0/DSI + PLL-MIPI/DPHY), AXP803 PMIC regulators over RSB (panel power + backlight),
panel reset GPIO, TCON0 timing (720x1440), sun6i MIPI-DSI host + DPHY, the ST7703/XBD599 DCS
init sequence, backlight enable — port from Linux sun6i_mipi_dsi.c + panel-sitronix-st7703.c +
sun50i-a64 CCU + AXP803. Big + hardware-iterative; instrument with LED blink-count beacons per
stage; SUCCESS signal = panel lights. The incoming UART0 serial cable would make this MUCH more
tractable. Faster ALTERNATIVE if blind DSI stalls: **p-boot** (PinePhone bootloader that DOES
light the panel + hands a framebuffer) → our adopt-FB reads it (kernel-free pixels). Build kit
staged on devbuntu ~/pine-uboot: pine-{fresh,diag,led}.img, boot-diag.cmd/.scr, unodos-{hardened,
post,led}.bin, fel.sh + sunxi-fel (built from source into ~/.local/bin). Repo: pinephone/
{boot-diag.cmd,fel.sh,flash-win.ps1,verify-win.ps1,md5-disk.ps1}; kernel.s LEDTEST+POST builds.
**PATH B (DSI-capable U-Boot) RULED OUT** — neither mainline nor Megi's U-Boot fork has a
sun6i/A64 MIPI-DSI host driver (only legacy lcdc/hdmi); Tow-Boot has no PinePhone display. So
ONLY the Linux kernel or **p-boot** light the A64 panel. DECISION (user): do **path A**
(bare-metal DSI bring-up IN the payload) in a NEW session. The full register-level blueprint is
saved at **`pinephone/PINEPHONE-BRINGUP.md`** (self-contained bring-up reference for a separate
PinePhone project: BROM/boot order, FEL, LED-PD18 debug channel, the no-DSI root cause, AND
Appendix A = lupyuen-sourced [REF] register recipe: CCU/PLL-MIPI, AXP803-over-RSB rails,
backlight PL10/PH10, panel-reset PD23, MIPI-DSI host 0x01CA0000, D-PHY 0x01CA1000, ST7703 DCS
init, TCON0 0x01C0C000, DE2/MIXER0 0x01100000). Next session: implement Appendix A, LED-beacon
each stage, ideally with the (incoming) UART0 serial cable.

**What differs from rpi (both honest to A64 silicon):**
- **Portrait** 480×640 (vs Pi 640×480). icon_x=(i%4)*112+16, icon_y=(i/4)*120+48;
  Dostris BORG_X=160,BORG_Y=80 (centred well). sys_gen.inc from `[world.pinephone]`.
- **No GPU mailbox.** A64 framebuffer comes from programming the **Display Engine
  2.0 (DE2) mixer 0 UI layer** directly: GLB_CTL(0x01100000)=enable, GLB_SIZE,
  BLD_SIZE/CH_ISZ/FILL, OVL_ATTR(0x01103000)=0xFF000405 (glob-alpha|fmt XRGB8888
  4<<8|LAY_EN), OVL_MBSIZE/SIZE/COORD/PITCH, OVL_TOPADD=PINE_FB. Best-effort
  bring-up that ASSUMES SPL/U-Boot already did DRAM + TCON0/MIPI-DSI panel clocks
  (exactly as the Pi assumes VideoCore firmware did HDMI). FB at fixed DRAM
  0x40400000 (no negotiation — like simplefb); harness knows it. Real-HW exact DE2
  correctness = by-test.
- **Timing via `mrs cntpct_el0`** (ARM architectural generic timer, 24MHz, NO MMIO)
  — wait_vblank target = now + FRAME_TICKS(400000). KEY FINDING: **Unicorn advances
  cntpct_el0 ~2 ticks/instruction on its own**, so the harness needs NO timer hook
  (verified with a probe). This makes the PinePhone harness simpler than the Pi's
  (no mailbox + no timer MMIO).
- **Input = REAL A64 UART0 serial console** (16550, on the headphone jack):
  UART0_LSR 0x01C28014 (poll DR bit0), UART0_RBR 0x01C28000; WASD/Enter/Backspace →
  pad, same as rpi. Harness adds a UART mmio_map at 0x01C28000 (outside DRAM + DE2
  sink, no overlap) + `--keys=` injector. Verified live: live_nav.png + live_notepad.png.
  Touch panel = future. (Pi used PL011; A64 is 16550 — different regs, same idea.)
- **Audio = software square-wave PCM synth** (music_gen, phase accumulator): per frame
  generate AUD_PERF=133 samples at AUD_RATE=8000, square via csel(phase<4000?+amp:-amp),
  phase+=freq wrap at 8000, write 16-bit (strh) to I2S_TXFIFO 0x01C22020 each frame
  (called from music_tick after the m_play check). m_phase/m_freq vars (VARS+176/180);
  music_load reads freq (ldrh) + resets phase. Harness mmio_maps I2S page 0x01C22000,
  sound_write captures samples (sign-extend 16-bit), `--audio=out.wav` writes WAV +
  analyze_pcm (50ms-window zero-crossing → note, drop <2-window boundary blips).
  VERIFIED Ode to Joy (pinephone/shots/music.wav). Real I2S clock + AC200 codec to the
  speaker = best-effort/by-ear; the PCM synthesis is what's verified. (Pi uses HW PWM;
  PinePhone/PPC have no HW tone gen → software PCM.)
- dostris_init seed: `mrs x1, cntpct_el0` (vs rpi's SYS_TIMER_CLO MMIO read).

**Boot/layout** (DRAM @ 0x40000000): payload `0x40080000` (U-Boot kernel_addr_r),
stack `→0x40200000`, VARS `0x40300000`, FBINFO `0x40320000`, FB `0x40400000`. Build
output `unodos.bin` flat (`go 0x40080000` from U-Boot). App indices identical to
rpi/GBA so AUTOTEST scripts port verbatim.

**Harness** (`pinephone/harness.py`, Unicorn UC_ARCH_ARM64): map DRAM 0x40000000
+16MB, DE2 RAM sink 0x01000000+0x200000 (layer pokes land here); set SP=0x40200000,
PC=0x40080000; run budget ~160M instr (each frame ~200k instr at FRAME_TICKS=400000),
read FB 32bpp @ 0x40400000 → PNG. No MMIO hooks at all.

Toolchain: same `aarch64-linux-gnu-*` binutils 2.42 via WSL as rpi. Contract:
`[world.pinephone]`/`[port.pinephone]` (cpu=aarch64, GASA64), `gen/pinephone/
sys_gen.inc`. Build: `bash pinephone/build.sh [nav|app|clock|theme|music|dostris]`.

**SELF-BOOTING microSD recipe (built on [[devbuntu-image-writing]] 2026-06-17, card
`/dev/sdc` 960 MB).** The port is NOT a bootable image — `unodos.bin` is a flat payload
that needs a real boot chain to bring up DRAM + the DSI panel first, then be loaded at
`0x40080000`. So a self-booting card = mainline **U-Boot + ATF** for the A64 + a
`boot.scr` that loads + `go`s the payload. All built in `~/pine-uboot` on devbuntu:
- **Cross toolchain WITHOUT sudo:** kernel.org crosstool `x86_64-gcc-14.2.0-nolibc-aarch64-linux`
  (43 MB, from mirrors.edge.kernel.org/pub/tools/crosstool) — nolibc is perfect for
  bare-metal U-Boot/ATF. devbuntu has `mkimage`+`dtc` but NO apt cross-gcc.
- **ATF:** `make PLAT=sun50i_a64 DEBUG=0 bl31` → `build/sun50i_a64/release/bl31.bin` (41 KB).
- **U-Boot:** `make pinephone_defconfig` (resolves `CONFIG_VIDEO/PANEL/BACKLIGHT=y` — it
  DOES light the DSI panel, that's the whole point), then `make BL31=<bl31> SCP=/dev/null`.
  Build needs `swig`+`pyelftools` → install in a **venv** (PEP-668 blocks system pip; no
  sudo needed). Two host-tool snags fixed without sudo: disable `TOOLS_MKEFICAPSULE`
  +`EFI_CAPSULE_*` (wants gnutls headers); enable `CMD_CACHE` (see below). Output
  `u-boot-sunxi-with-spl.bin` (~838 KB) → `dd ... seek=8 bs=1k` (8 KB offset; SPL magic
  `eGON.BT0` lands at file offset 8196; ends ~826 KB, clears the 1 MiB partition start).
- **boot.scr:** this U-Boot uses `distro_bootcmd` (`bootcmd=run distro_bootcmd`), which
  scans each device for `boot.scr` (`boot_scripts=boot.scr.uimg boot.scr`) at the FAT
  root and runs it with `${devtype}/${devnum}/${distro_bootpart}` set. `boot.cmd` →
  `mkimage -A arm64 -O u-boot -T script -C none`: `fatload ${devtype} ${devnum}:${distro_bootpart}
  0x40080000 unodos.bin` (fallback `fatload mmc 0:1`), then **`dcache flush; dcache off;
  icache off; go 0x40080000`**.
- **CACHE-COHERENCY CAVEAT (the key real-HW risk):** U-Boot `go` does NOT flush/disable
  caches (unlike `booti`). The payload runs with U-Boot's MMU+caches still on, never
  re-enables/flushes, and DE2 scans out the FB via DMA → cached FB writes = stale DRAM =
  garbage on screen. Mitigation baked into boot.scr: `dcache off; icache off` BEFORE `go`
  (needs `CONFIG_CMD_CACHE=y`, hence the rebuild) so the payload runs cache-off and DE2
  sees coherent DRAM. UNVERIFIED on real hardware (harness has no caches/U-Boot) — this
  is the thing most likely to need a tweak after a first boot on the actual phone.
- Card layout: MBR, single FAT32 (label `UNODOS`, 1 MiB→end), holding `unodos.bin` +
  `boot.scr`; SPL/U-Boot in the 8 KB–1 MiB gap. Build tree kept in `~/pine-uboot` for rebuilds.
- **CODIFIED IN THE REPO (commit cbcda23, master):** `pinephone/mksd.sh`
  (`fw`|`image`|`write /dev/sdX`|`all`) automates the whole recipe above + `pinephone/
  boot.cmd` (the boot script source) + a "Self-booting microSD" README section. Run
  `mksd.sh` ON A LINUX BOX (not WSL). NOTE: tracked `pinephone/build/unodos.bin` was
  STALE vs committed source (11344 vs the verified 11496-byte payload) — left for the
  user to refresh; `build.sh` rebuilds it from source anyway.

Related: [[unodos-rpi-port]] (the shared core), [[unodos-gba-port]], [[unodos-3.1-contract-arch]],
[[devbuntu-image-writing]] (the flashing box + write-blocker/loop-image/`/tmp`-quota gotchas).
