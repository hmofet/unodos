// ============================================================================
// UnoDOS / PinePhone (Allwinner A64, ARM Cortex-A53 / AArch64) — milestones 1-3.
// ============================================================================
// The TENTH fresh contract-driven port. It REUSES the Raspberry Pi AArch64 core
// (same GNU-as/GAS dialect, same software-framebuffer primitives, same Dostris and
// app logic) retargeted to the Allwinner A64 SoC and a PORTRAIT phone panel
// (480x640). MINIMAL profile (CONTRACT-ARCH §9): a 4-column icon launcher, one
// full-screen app at a time, directional nav.
//
// Display: like the Pi (which relies on the VideoCore firmware to bring up HDMI),
// this assumes the boot chain (boot ROM -> SPL -> U-Boot) has already brought up
// DRAM and the panel clock path (TCON0 + MIPI-DSI + the panel). The kernel then
// programs the A64 Display Engine 2.0 (DE2) mixer UI layer to scan out our
// XRGB8888 framebuffer in DRAM. Per-frame pacing reads the ARM architectural
// generic timer (cntpct_el0) directly — no MMIO, real-hardware-correct.
//
// M1: boot -> DE2 UI layer -> a rendered launcher (title bar + 4-col colour grid).
// M2: a generic-timer-paced loop, a d-pad selection highlight, A launches an app
//     full-screen, B returns. (Real touch / power-button input is a future driver;
//     the milestones are driven by the AUTOTEST scripted pad, like every port.)
// M3: full-screen apps — SysInfo, live Clock, Notepad, Files, Theme (palette),
//     Music (UI/timing; the AC200 codec path is a future driver), and Dostris.
//
// Contract-owned (Phase 4): the screen geometry comes from unogen
// ([world.pinephone] -> gen/pinephone/sys_gen.inc).
// ============================================================================

.include "../unodef/gen/pinephone/sys_gen.inc"   // SCRW/SCRH/SCRCOLS/SCRROWS
.include "build/gfxequ.inc"                       // NICONS/NTHEMES/MUSIC_COUNT

// ---- Allwinner A64 display engine 2.0 (mixer 0) ----------------------------
.equ GLB_CTL,    0x01100000      // mixer0 global control (enable)
.equ GLB_DBUF,   0x01100008      // mixer0 double-buffer commit
.equ GLB_SIZE,   0x0110000C      // mixer0 output size
.equ BLD_FILL,   0x01101000      // blender pipe enable / fill-colour control
.equ BLD_FILLCOL,0x01101004      // blender pipe0 fill colour (ARGB)
.equ BLD_CH_ISZ, 0x01101008      // blender ch0 input size
.equ BLD_CH_OFF, 0x0110100C      // blender ch0 input offset
.equ BLD_RTCTL,  0x01101080      // blender routing control
.equ BLD_BKCOL,  0x01101088      // blender background colour
.equ BLD_SIZE,   0x0110108C      // blender output size
.equ BLD_MODE,   0x01101090      // blender ch0 blend mode
.equ OVL_ATTR,   0x01103000      // UI overlay layer0 attribute control
.equ OVL_MBSIZE, 0x01103004      // UI overlay layer0 memory-block size
.equ OVL_COORD,  0x01103008      // UI overlay layer0 coordinate
.equ OVL_PITCH,  0x0110300C      // UI overlay layer0 pitch (bytes/row)
.equ OVL_TOPADD, 0x01103010      // UI overlay layer0 top framebuffer address
.equ OVL_SIZE,   0x01103088      // UI overlay window size
// DE2 top-level clock/reset (DE base 0x01000000). The CCU enables the DE *bus* clock,
// but the Display Engine gates+resets its sub-blocks internally: MIXER0 (0x01100000)
// registers are inaccessible (writes dropped, read back 0) until these are enabled.
.equ DE_SCLK_GATE, 0x01000000    // special (pixel) clock gate; bit0 = mixer0 (core0)
.equ DE_HCLK_GATE, 0x01000004    // AHB (bus) clock gate;        bit0 = mixer0 (core0)
.equ DE_AHB_RESET, 0x01000008    // module reset de-assert;      bit0 = mixer0 (core0)
.equ DE_TCON_MUX,  0x01000010    // mixer->TCON routing; bit0=0 routes mixer0 -> TCON0
// A64 UART0 (16550-compatible) — the serial console (on the headphone jack)
.equ UART0_RBR,  0x01C28000      // receive buffer register
.equ UART0_LSR,  0x01C28014      // line status register (bit0 = data ready)
// A64 I2S/PCM0 — TX FIFO for the audio codec PCM stream
.equ I2S_TXFIFO, 0x01C22020      // I2S0 TX data FIFO
// A64 PIO (port D) — the green status LED is on PD18 (GPIO114; SPL lights it)
.equ PD_CFG2,    0x01C20874      // PD config reg, pins 16-23 (PD18 = bits [11:8])
.equ PD_DAT,     0x01C2087C      // PD data reg (PD18 = bit 18)
.equ AUD_RATE,   8000            // PCM sample rate
.equ AUD_PERF,   133             // samples per ~60 Hz frame (8000/60)
.equ AUD_AMP,    6000            // square-wave amplitude

// ---- fixed DRAM layout (A64 DRAM starts at 0x40000000) ---------------------
.equ STACK_TOP, 0x40200000
.equ VARS,      0x40300000       // cleared at boot
.equ FBINFO,    0x40320000       // framebuffer base/pitch (NOT cleared)
.equ fb_base,   FBINFO+0         // 8 bytes
.equ fb_pitch,  FBINFO+8         // 4 bytes
.equ PINE_FB,   0x40400000       // the XRGB8888 framebuffer in DRAM
.equ PINE_PITCH, (SCRW*4)        // 480 * 4 = 1920 bytes/row
// The XBD599 panel is natively 720x1440. The DE2 mixer global/blender output and
// TCON0 are programmed to that native size; our 480x640 portrait content is a UI
// overlay LAYER positioned at the panel's top-left (the rest stays the blender's
// black background). PANEL_SZ / LAYER_SZ encode ((H-1)<<16)|(W-1) for each.
.equ PANEL_W,   720
.equ PANEL_H,   1440
.equ PANEL_SZ,  (((PANEL_H-1)<<16)|(PANEL_W-1))   // 0x059F02CF
.equ LAYER_SZ,  (((SCRH-1)<<16)|(SCRW-1))          // 0x027F01DF

// pad bits (active-high) — same layout as the Pi/GBA so AUTOTEST scripts match
.equ PAD_A,   0x01
.equ PAD_B,   0x02
.equ PAD_SEL, 0x04
.equ PAD_ST,  0x08
.equ PAD_R,   0x10
.equ PAD_L,   0x20
.equ PAD_U,   0x40
.equ PAD_D,   0x80

.equ FRAME_TICKS, 400000         // 24 MHz generic timer / 60 Hz

// Dostris geometry (board cells; 16px cells, centred in the portrait field)
.equ BW, 10
.equ BH, 14
.equ CELL, 16
.equ BORG_X, 160
.equ BORG_Y, 80
.equ FALLRATE, 30

// Paint geometry (portrait 480x640)
.equ PCW, 36
.equ PCH, 24
.equ PCELL, 12
.equ PCO_X, 16
.equ PCO_Y, 24
.equ NSWATCH, 8
.equ PSW_W, 26
.equ PSW_Y, (PCO_Y + PCH*PCELL + 10)
// Pac-Man geometry
.equ PM_COLS, 28
.equ PM_ROWS, 25
.equ PM_CELL, 16
.equ PMO_X, 16
.equ PMO_Y, 24
.equ GSIZE, 20
.equ FRIGHT_STEPS, 45
.equ PM_STEPFRAMES, 4
// OutLast geometry (480 wide -> 12px cols)
.equ OL_BANDS, 20
.equ OL_COLW, 12
.equ OL_BH, 20
.equ OLO_Y, 24
.equ OL_RATE, 4
// Tracker geometry
.equ NT_ROWS, 16
.equ NT_CH, 4
.equ TK_STEPF, 12
.equ TKO_X, 60
.equ TKO_Y, 44
.equ TK_RH, 22
.equ TK_CW, 80

.equ v_pad,    VARS+0
.equ v_padp,   VARS+4
.equ v_pade,   VARS+8
.equ v_inapp,  VARS+12
.equ v_sel,    VARS+16
.equ v_selp,   VARS+20
.equ v_app,    VARS+24
.equ v_dirty,  VARS+28
.equ v_frac,   VARS+32
.equ v_ss,     VARS+36
.equ v_mm,     VARS+40
.equ v_hh,     VARS+44
.equ v_theme,  VARS+48
.equ m_idx,    VARS+52
.equ m_timer,  VARS+56
.equ m_play,   VARS+60
.equ pf_hl,    VARS+64
.equ pf_clk,   VARS+68
.equ pf_pc,    VARS+72
.equ pf_score, VARS+76
.equ d_fg,     VARS+80
.equ d_bg,     VARS+84
.equ g_type,   VARS+88
.equ g_rot,    VARS+92
.equ g_px,     VARS+96
.equ g_py,     VARS+100
.equ g_state,  VARS+104
.equ g_fall,   VARS+108
.equ g_lines,  VARS+112
.equ g_seed,   VARS+116
.equ g_tx,     VARS+120
.equ g_ty,     VARS+124
.equ g_srot,   VARS+128
.equ g_row,    VARS+132
.equ g_lt,     VARS+136
.equ g_pt,     VARS+140
.equ mlo,      VARS+144
.equ g_oldpx,  VARS+148
.equ g_oldpy,  VARS+152
.equ g_oldrot, VARS+156
.equ a_idx,    VARS+160
.equ a_tmr,    VARS+164
.equ a_pad,    VARS+168
.equ a_gpause, VARS+172
.equ m_phase,  VARS+176          // PCM square-wave phase accumulator
.equ m_freq,   VARS+180          // current note frequency (Hz)

.equ p_cx,     VARS+184
.equ p_cy,     VARS+188
.equ p_col,    VARS+192
.equ pm_x,     VARS+196
.equ pm_y,     VARS+200
.equ pm_dir,   VARS+204
.equ pm_ndir,  VARS+208
.equ pm_score, VARS+212
.equ pm_lives, VARS+216
.equ pm_level, VARS+220
.equ pm_dots,  VARS+224
.equ pm_mode,  VARS+228
.equ pm_modet, VARS+232
.equ pm_fr,    VARS+236
.equ pm_kills, VARS+240
.equ pm_st,    VARS+244
.equ pm_sc,    VARS+248
.equ pm_tgx,   VARS+252
.equ pm_tgy,   VARS+256
.equ pm_ft,    VARS+260
.equ pm_gh,    VARS+264
.equ ol_carx,  VARS+324
.equ ol_scroll,VARS+328
.equ ol_dist,  VARS+332
.equ ol_over,  VARS+336
.equ ol_ctr,   VARS+340
.equ tk_crow,  VARS+344
.equ tk_cch,   VARS+348
.equ tk_prow,  VARS+352
.equ tk_ptmr,  VARS+356
.equ fl_sel,   VARS+360
.equ fl_view,  VARS+364
.equ np_saved, VARS+368
.equ palette,  VARS+0x200        // 16 XRGB words
.equ clk_str,  VARS+0x240        // 9 bytes
.equ numstr,   VARS+0x250        // 6 bytes
.equ g_board,  VARS+0x260        // BW*BH bytes

.equ pcanvas,  VARS+0x400
.equ pm_maze,  VARS+0x800
.equ tk_pat,   VARS+0xC00
.equ fbuf,     VARS+0x1000

.section .text
.global _start
// ARM64 Linux Image header (64 bytes). Lets a kernel-style loader (e.g. megi's p-boot,
// which lights the DSI panel itself and hands off a live framebuffer) load + enter us as
// an "arm64 kernel". code0 = a branch over the header to the real entry, so this is
// transparent to U-Boot `go` and the Unicorn harness (both jump to 0x40080000 = code0).
// text_offset 0x80000 -> p-boot's LINUX_IMAGE_PA(0x40000000)+0x80000 = 0x40080000 = our
// link address (so the non-PIC absolute code lands correctly). magic 0x644d5241 = "ARM\x64".
_start:
    b     _entry                           // code0
    .word 0                                // code1
    .quad 0x00080000                       // text_offset
    .quad 0x00400000                       // image_size (advisory)
    .quad 0                                // flags
    .quad 0                                // res2
    .quad 0                                // res3
    .quad 0                                // res4
    .word 0x644d5241                       // magic "ARM\x64"
    .word 0                                // res5 (PE COFF offset)
_entry:
    mrs   x0, mpidr_el1                   // park secondary cores
    and   x0, x0, #0xFF
    cbz   x0, core0
hang:
    wfe
    b     hang
core0:
    ldr   x0, =STACK_TOP
    mov   sp, x0
    // --- make the payload self-sufficient on cache state -------------------
    // U-Boot's `go` leaves its caches enabled and boot.cmd's `dcache off` is
    // unverified on real silicon. The DE2 engine scans the framebuffer out of DRAM
    // by DMA, so a stale cached FB line shows as garbage. Disable the D/I-caches
    // here ourselves (EL-aware: the A64 hands off in EL2 after ATF, but handle EL1
    // too). We deliberately KEEP the MMU on (U-Boot's tables): with the MMU off,
    // AArch64 forces all data accesses to Device-nGnRnE (strict alignment + slow),
    // which could fault on an unaligned access the harness can't model. With C=0 +
    // MMU on, DRAM stays Normal (unaligned-tolerant) but uncached -> coherent FB.
    // (boot.cmd's `dcache flush` already cleaned any dirty lines before `go`.)
    mov   x2, #0x1004                     // bits 2 (C/D$) | 12 (I/I$); leave M (MMU) set
    mrs   x0, CurrentEL
    lsr   x0, x0, #2
    and   x0, x0, #3
    cmp   x0, #2
    b.lt  cache_el1
    mrs   x1, sctlr_el2
    bic   x1, x1, x2
    msr   sctlr_el2, x1
    b     cache_done
cache_el1:
    mrs   x1, sctlr_el1
    bic   x1, x1, x2
    msr   sctlr_el1, x1
cache_done:
    dsb   sy
    isb
.ifdef PANELDBG
    // Debug: route CPU exceptions to our own vector (a fast LED flutter that never
    // returns) instead of U-Boot's handler (which resets -> reboot loop). So a fault
    // in panel_init shows as "stage blinks up to N, then fast flutter" — pinning the
    // crashing block — instead of an ambiguous repeating reboot.
    ldr   x0, =dbg_vectors
    mrs   x1, CurrentEL
    lsr   x1, x1, #2
    and   x1, x1, #3
    cmp   x1, #2
    b.lt  vbar_el1
    msr   vbar_el2, x0
    b     vbar_done
vbar_el1:
    msr   vbar_el1, x0
vbar_done:
    isb
.endif
.ifdef LEDTEST
    // No-serial / no-display output channel test: blink the PinePhone status LED
    // (green, PD18 = GPIO114, the same LED the SPL lights). A steady blink proves
    // the payload runs and that we can drive the LED for staged progress beacons.
    bl    led_init
ledtest_loop:
    mov   w0, #1
    bl    led_set
    bl    led_halfsec
    mov   w0, #0
    bl    led_set
    bl    led_halfsec
    b     ledtest_loop
.endif
    ldr   x0, =VARS                       // clear the variable block
    mov   w2, #256
mclr:
    str   wzr, [x0], #4
    subs  w2, w2, #1
    b.ne  mclr
.ifndef PBOOT
    bl    panel_init                      // FULL DSI panel bring-up (clocks, PMIC,
                                          // DSI host, D-PHY, ST7703, TCON0, backlight).
                                          // SKIPPED under PBOOT: a kernel-loader bootloader
                                          // (p-boot) already lit the panel + set up the FB.
.endif
.ifdef PBOOT
.ifdef PANELDBG
    // p-boot has ALREADY lit the panel; the DE2/TCON0/DSI/D-PHY it programmed are live and
    // scanning out its splashscreen. Dump them NOW, BEFORE fb_init touches anything, to
    // capture p-boot's pristine WORKING configuration. This is the value-by-value reference
    // to diff against our native panel_init dump (PINEPHONE-BRINGUP §8: the decisive
    // comparison that source/register-source matching cannot make — any differing register
    // is the bug). UART0 is already up: p-boot's own serial console left it running.
    ldr   x0, =s_pbref
    bl    uart_puts
    bl    dump_all
    // WARM panel-state reference: read back p-boot's lit/working panel's DCS status/config
    // registers (LP read, drops the bus to LP briefly). Diff offline against the native cold
    // paneldbg dcs_panel_dump: a register that differs cold-vs-warm = the DCS config that
    // didn't take on the cold panel (PINEPHONE-BRINGUP.md §8, 2026-06-25). Runs BEFORE fb_init.
    bl    dcs_panel_dump
.endif
.ifdef NATIVEPANEL
    // DIFFERENTIAL TEST (build.sh pbootnative = PBOOT+PANELDBG+DE2TEST+NATIVEPANEL): after
    // p-boot has lit the panel, RE-RUN our full native panel_init (clocks/TCON0/DSI/D-PHY/
    // st7703/dsi_start) on top — tearing down p-boot's working pipe and rebuilding it with
    // OUR code, starting from p-boot's warm clock/DRAM state. With DE2TEST also set, fb_init
    // then reprograms DE2 natively (already proven correct). Outcomes:
    //   launcher shows -> our panel_init CODE is functionally correct; the native-cold
    //     failure is ENVIRONMENTAL (U-Boot's clock/DRAM cascade != p-boot's SPL) -> stop
    //     perturbing panel.inc.s, compare the non-display clock tree U-Boot vs p-boot SPL.
    //   black -> our panel_init actively breaks a working pipe -> the bug is one of its
    //     blocks; bisect by adding them incrementally (clocks, then +TCON0, then +DSI/DPHY).
    // CONFIRMED black 2026-06-22 -> bug IS in panel_init. DSIONLY (build.sh pbootdsi) bisects:
    // re-init ONLY the DSI link (DSI/D-PHY/panel/dsi_start) on p-boot's clocks+TCON0.
.ifdef CLKTCON0ONLY
    bl    panel_clktcon0_only           // re-run ONLY our clocks + tcon0_init on p-boot's live
.else                                   //   DSI/D-PHY/panel — the one perturbation never cleanly
.ifdef DSISTARTONLY                     //   done. survives=clocks/TCON0 seq safe; black=our PLL/
    bl    panel_dsistart_only           //   TCON0 reprogram transient disrupts the pipe.
.else
.ifdef DSIHOSTONLY
    bl    panel_dsihost_only            // dsi_host_init + dsi_start (no dphy) — splits last 2
.else
.ifdef NODPHY
    bl    panel_nodphy_only             // whole DSI link MINUS dphy — airtight dphy confirm
.else
.ifdef ST7703ONLY
    bl    panel_st7703_only             // re-DCS WITHOUT reset — splits reset-artifact vs st7703
.else
.ifdef DPHYONLY
    bl    panel_dphy_only               // last untested block: our dphy on p-boot's everything
.else
.ifdef DSIONLY
    bl    panel_dsi_only
.else
    bl    panel_init
.endif
.endif
.endif
.endif
.endif
.endif
.endif
.endif
.endif
.ifdef PANELDBG
    mov   w0, #2                          // GREEN (post): entering fb_init
    bl    led_stage
.endif
    bl    fb_init                         // point the DE2 UI layer at our framebuffer
.ifdef PANELDBG
    ldr   x0, =s_de2
    ldr   x1, =GLB_CTL
    ldr   w1, [x1]
    bl    print_reg                       // DE2 mixer global control
    ldr   x0, =s_ovl
    ldr   x1, =OVL_TOPADD
    ldr   w1, [x1]
    bl    print_reg                       // UI overlay framebuffer address
.endif
    mov   w0, #160                        // let the DE2 pipeline + panel settle before
    bl    delay_ms                        // first content (NuttX waits 160ms here)
.ifdef PANELDBG
    mov   w0, #1                          // RED (post): entering fs_init
    bl    led_stage
.endif
.ifdef POST
    // On-screen POST beacon: with no serial console the panel is our only output.
    // Each stage paints a distinct solid colour; a freeze leaves the last-reached
    // stage's colour on screen. (Bonus: pure R/G/B also reveals an R<->B channel
    // swap — if "red" shows as blue, the firmware delivers BGR.)
    mov   w0, #0xFF                       // RED  0x00FF0000 -> fb_init returned, FB writable
    lsl   w0, w0, #16
    bl    post_fill
    bl    post_delay
.endif
    bl    fs_init                         // load/format the USV1 disk
.ifdef PANELDBG
    mov   w0, #4                          // BLUE (post): entering draw_launcher
    bl    led_stage
.endif
.ifdef POST
    mov   w0, #0xFF00                     // GREEN 0x0000FF00 -> fs_init returned
    bl    post_fill
    bl    post_delay
    mov   w0, #0xFF                       // BLUE  0x000000FF -> about to draw launcher
    bl    post_fill
    bl    post_delay
.endif
    bl    draw_launcher
.ifdef PANELDBG
    ldr   x0, =s_done
    bl    uart_puts                       // reached the main loop, no fault
    // FULL register readback dump: did our writes actually STICK on hardware? Each value is
    // compared offline against p-boot's known-good; any mismatch = a silently-dropped write.
    bl    dump_all
    // Is TCON0 actually SCANNING? Sample GINT0 (its IRQ/status flags) repeatedly ~50ms
    // apart. If the value CHANGES across samples, TCON0 IS triggering frames -> the stall
    // is downstream (DSI video transfer or the panel). If it stays STATIC, TCON0's frame
    // trigger never fires -> the stall is TCON0 itself. (PINEPHONE-BRINGUP §8 candidate #1
    // — the decisive split.) Also re-print GCTL to confirm TCON0 is still enabled.
    ldr   x0, =s_tcon
    ldr   x1, =TCON0+0x00
    ldr   w1, [x1]
    bl    print_reg                       // TCON0 GCTL (bit31 = enabled)
    mov   w19, #3
tcon_scan:
    // GINT0 BIT(11) = TCON0 frame-done latch (p-boot's display_frame_done). We READ it,
    // then CLEAR it (write 0): if it RE-SETS on the next sample, TCON0 is triggering a
    // NEW frame every ~50ms = continuously scanning (stall is downstream: DSI HS xfer or
    // panel). If it stays 0 after the first clear, TCON0 fired ONCE then stopped (the
    // SAFE_PERIOD auto-retrigger isn't re-firing = a TCON0-side stall).
    ldr   x1, =TCON0+0x04
    ldr   w20, [x1]
    ldr   x0, =s_gint
    mov   w1, w20
    bl    print_reg
    ldr   x1, =TCON0+0x04                  // clear the frame-done latch
    str   wzr, [x1]
    dsb   sy
    // DE2 GLB_STATUS bits[1:0] = is mixer0 actually producing output? (p-boot polls this.)
    ldr   x1, =0x01100004
    ldr   w1, [x1]
    ldr   x0, =s_glbst
    bl    print_reg
    // DSI_BASIC_CTL0 bit0 (INSTRU_EN) = is the DSI video instruction loop still running?
    ldr   x1, =DSI+0x10
    ldr   w1, [x1]
    ldr   x0, =s_dsi0
    bl    print_reg
    mov   w0, #50
    bl    delay_ms
    subs  w19, w19, #1
    b.ne  tcon_scan
    // NEW STRATEGY #1 (PINEPHONE-BRINGUP §8): measure the ACTUAL TCON0 frame period
    // against the fixed 24 MHz cntpct. Compare native (this build) vs p-boot (pbootdbg):
    // a different period = a clock at the wrong RATE despite matching register bits.
    bl    measure_frame_period
    mov   w0, #6                          // CYAN held (visual backup)
    bl    led_rgb
.endif
mainloop:
    bl    wait_vblank
.ifndef PBOOT
    // Re-commit the DE2 double-buffered registers EVERY frame. The mixer's blender/layer/
    // backdrop config is double-buffered: writes land in a shadow set and only become the
    // ACTIVE scanout config when GLB_DBUFFER latches at vsync. fb_init writes it once; if
    // that single latch didn't take (e.g. no vsync yet, or a timing race with the just-
    // -enabled mixer), the ACTIVE config stays cleared = black, even though every register
    // READS BACK correct (the readback is the shadow, not the active set). A real compositor
    // commits per frame; doing so here is both correct and robust against a missed latch.
    ldr   x0, =GLB_DBUF
    mov   w1, #1
    str   w1, [x0]
    dsb   sy
.endif
    bl    render_partials
    bl    read_keys
    bl    clock_advance
    bl    update
    ldr   x0, =v_dirty
    ldr   w1, [x0]
    cbz   w1, mainloop
    bl    full_redraw
    b     mainloop

// ============================================================================
// framebuffer bring-up (Allwinner A64 Display Engine 2.0, mixer 0 UI layer)
// ============================================================================
// panel_init (panel.inc.s) has already lit the XBD599 panel and is clocking it at
// its native 720x1440 via TCON0. Here we program the DE2 mixer: the global output
// and blender are panel-sized, and our 480x640 portrait framebuffer is the UI
// overlay LAYER at the top-left (COORD 0). The rest of the panel shows the blender's
// black background. The drawing primitives are pitch-relative, so all M1-M3 content
// renders unchanged into the 480-wide buffer.
fb_init:
.ifdef PBOOT
.ifndef DE2TEST
    // DIFFERENTIAL PERTURBATION TEST (build.sh de2test = PBOOT+PANELDBG+DE2TEST): when
    // DE2TEST is set we DELIBERATELY skip this adopt path and fall through to our native
    // DE2 programming below, running it ON TOP of p-boot's known-good TCON0/DSI/DPHY/clocks
    // (p-boot has already lit the panel). The [pboot-ref] dump_all above captured p-boot's
    // working DE2 first. If the panel goes BLACK after our reprogram -> our DE2 mixer config
    // is the bug. If it shows our RED backdrop / launcher -> our DE2 is CORRECT and the
    // native wall is upstream (our TCON0/DSI/DPHY/clock bring-up). One cycle bisects it.
    // p-boot already lit the panel and left a live framebuffer scanning out via DE2.
    // ADOPT it: read the overlay address + pitch the DE2 is currently scanning and draw
    // straight into THAT buffer; do NOT reprogram DE2 (our own scanout bring-up is the
    // part that doesn't work yet). The primitives are pitch-relative, so our 480x640
    // content lands at the top-left of p-boot's (panel-native) framebuffer. Fall back to
    // our fixed FB if the overlay reads 0 (p-boot used a different layer) — at least no
    // null deref.
    ldr   x0, =OVL_TOPADD                 // p-boot's live FB base (32-bit phys)
    ldr   w3, [x0]
    cbnz  w3, pb_haveb
    ldr   w3, =PINE_FB
pb_haveb:
    ldr   x0, =OVL_PITCH                  // p-boot's FB pitch (bytes/row)
    ldr   w4, [x0]
    cbnz  w4, pb_havep
    mov   w4, #PINE_PITCH
pb_havep:
    // p-boot's FB is the panel-native 720x1440; our content is SCRW x SCRH (480x640).
    // Clear the whole FB to black (wipes p-boot's splashscreen), then offset fb_base so
    // our content is CENTERED instead of crammed in the top-left corner.
    mov   x7, x3                          // x7 = FB base
    mov   w8, #1440                       // panel height
    umull x8, w8, w4                      // total bytes = 1440 * pitch
    add   x8, x7, x8                      // end address
pb_clr:
    str   wzr, [x7], #4
    cmp   x7, x8
    b.lo  pb_clr
    dsb   sy
    mov   w5, #((1440-SCRH)/2)            // vertical centering offset (rows) = 400
    umull x6, w5, w4                      // * pitch
    add   x3, x3, x6
    mov   w5, #(((720-SCRW)/2)*4)         // horizontal centering offset (bytes) = 480
    add   x3, x3, x5
    ldr   x2, =fb_base
    str   x3, [x2]
    ldr   x2, =fb_pitch
    str   w4, [x2]
    ret
.endif                                    // ifndef DE2TEST (DE2TEST falls through to native DE2)
.endif
    // Bring the DE2 block itself out of gate/reset before touching MIXER0. de_clk_init
    // (end of panel_init) enabled the DE *bus* clock at the CCU, but the Display Engine
    // has its own internal
    // clock gates + per-module reset; without these the MIXER0 writes below are dropped
    // and read back 0 on hardware (the Unicorn harness masks this — it maps DE2 as RAM).
    ldr   x0, =DE_SCLK_GATE               // pixel clock gate, mixer0
    mov   w1, #1
    str   w1, [x0]
    ldr   x0, =DE_HCLK_GATE               // AHB bus clock gate, mixer0
    mov   w1, #1
    str   w1, [x0]
    // CYCLE the mixer0 internal reset (assert -> brief settle -> de-assert) rather than
    // only de-asserting: forces the DE sub-block to its cold-boot default in case the
    // U-Boot handoff left it half-initialised. Inline busy-delay keeps fb_init a leaf.
    ldr   x0, =DE_AHB_RESET
    str   wzr, [x0]                       // assert mixer0 module reset
    dsb   sy
    mov   w1, #0x40000
fbrst_d:
    subs  w1, w1, #1
    b.ne  fbrst_d
    ldr   x0, =DE_AHB_RESET               // de-assert mixer0 module reset
    mov   w1, #1
    str   w1, [x0]
    dsb   sy
    // Zero the entire MIXER0 register block (0x6000 bytes). The reset state of some
    // mixer registers is non-zero/indeterminate; NuttX a64_de_init clears it before
    // any config. (Harness-safe: DE2 region is a RAM sink there.)
    ldr   x0, =0x01100000
    add   x2, x0, #0x6000
fbclr:
    str   wzr, [x0], #4
    cmp   x0, x2
    b.lo  fbclr
    // Disable every DE2 enhancement / scaler block — if any powers up live it sits in
    // the datapath between the UI layer and TCON0 and blanks the output (NuttX disables
    // all of these in a64_de_init). Addresses = MIXER0 + block offset.
    ldr   x0, =0x01120000                 // VS_CTRL    (video scaler)
    str   wzr, [x0]
    ldr   x0, =0x01130000                 // UNDOC 0x130000
    str   wzr, [x0]
    ldr   x0, =0x01140000                 // UIS_CTRL1  (UI scaler 1)
    str   wzr, [x0]
    ldr   x0, =0x01150000                 // UIS_CTRL2  (UI scaler 2)
    str   wzr, [x0]
    ldr   x0, =0x011A0000                 // FCE
    str   wzr, [x0]
    ldr   x0, =0x011A2000                 // BWS
    str   wzr, [x0]
    ldr   x0, =0x011A4000                 // LTI
    str   wzr, [x0]
    ldr   x0, =0x011A6000                 // PEAKING
    str   wzr, [x0]
    ldr   x0, =0x011A8000                 // ASE
    str   wzr, [x0]
    ldr   x0, =0x011AA000                 // FCC
    str   wzr, [x0]
    ldr   x0, =0x011B0000                 // DRC
    str   wzr, [x0]
    ldr   x0, =DE_TCON_MUX                // route mixer0 -> TCON0 (clear bit0)
    str   wzr, [x0]
    dsb   sy
    ldr   x0, =GLB_CTL                    // mixer enable
    mov   w1, #1
    str   w1, [x0]
    ldr   x0, =GLB_SIZE                   // global output = panel native size
    ldr   w1, =PANEL_SZ
    str   w1, [x0]
    // blender: pipe 0 over a black background, panel-sized output, layer-sized input
    ldr   x0, =BLD_FILL                   // enable ONLY pipe 0 (P0_EN bit8 | P0_FCEN bit0).
    ldr   w1, =0x00000101                 // was 0x701 = enable pipes 0,1,2 -> phantom pipes
    str   w1, [x0]                        // 1,2 (no layer) composited black OVER our layer.
    ldr   x0, =BLD_FILLCOL                // pipe 0 fill colour = opaque black
    ldr   w1, =0xFF000000
    str   w1, [x0]
    ldr   x0, =BLD_RTCTL                  // route pipe 0 <- channel 1 (our UI overlay).
    ldr   w1, =0x00000001                 // was 0x321 = the 3-channel route (pipes 0,1,2 <-
    str   w1, [x0]                        // channels 1,2,3); we only have channel 1.
    ldr   x0, =BLD_SIZE
    ldr   w1, =PANEL_SZ
    str   w1, [x0]
    ldr   x0, =BLD_BKCOL
.ifdef PANELDBG
    ldr   w1, =0xFFFF0000                 // RED backdrop (DECISIVE scanout test): the DE2
                                          // blender emits this regardless of any layer. RED on
                                          // the panel => the DE2->TCON0->DSI video path WORKS
                                          // (only our layer config is then suspect); still black
                                          // => path dead downstream of TCON0 (confirmed scanning
                                          // 2026-06-21). Re-run with reset-cycle/divider fixes.
.else
    ldr   w1, =0xFF000000                 // opaque black backdrop (production)
.endif
    str   w1, [x0]
    ldr   x0, =BLD_CH_ISZ
    ldr   w1, =LAYER_SZ                   // channel 0 input = our 480x640 layer
    str   w1, [x0]
    ldr   x0, =BLD_CH_OFF
    str   wzr, [x0]
    ldr   x0, =BLD_MODE
    ldr   w1, =0x03010301                 // SRC over DST
    str   w1, [x0]
    // UI overlay layer 0 (XRGB8888) -> our framebuffer, top-left
    ldr   x0, =OVL_ATTR
    ldr   w1, =0xFF000405                 // glob-alpha=FF, fmt=XRGB8888(4<<8), LAY_EN
    str   w1, [x0]
    ldr   x0, =OVL_MBSIZE
    ldr   w1, =LAYER_SZ
    str   w1, [x0]
    ldr   x0, =OVL_SIZE
    ldr   w1, =LAYER_SZ
    str   w1, [x0]
    ldr   x0, =OVL_COORD
    str   wzr, [x0]
    ldr   x0, =OVL_PITCH
    ldr   w1, =PINE_PITCH
    str   w1, [x0]
    ldr   x0, =OVL_TOPADD
    ldr   w1, =PINE_FB
    str   w1, [x0]
    ldr   x0, =GLB_DBUF                   // commit the double-buffered registers
    mov   w1, #1
    str   w1, [x0]
    // record the framebuffer for the primitives
    ldr   x0, =fb_base
    ldr   x1, =PINE_FB
    str   x1, [x0]
    ldr   x0, =fb_pitch
    ldr   w1, =PINE_PITCH
    str   w1, [x0]
    dsb   sy
    ret

// wait_vblank: pace one frame off the ARM generic timer (cntpct_el0, 24 MHz)
wait_vblank:
    mrs   x0, cntpct_el0
    ldr   x1, =FRAME_TICKS
    add   x1, x0, x1
wv1:
    mrs   x0, cntpct_el0
    cmp   x0, x1
    b.lo  wv1
    ret

.ifdef POST
// post_fill: w0 = raw XRGB colour; fill the top-left SCRW x SCRH block of the
// (adopted) framebuffer directly — palette-independent, so it works at the very
// first boot stages before load_palette has run. Leaf.
post_fill:
    ldr   x4, =fb_base
    ldr   x4, [x4]
    ldr   x5, =fb_pitch
    ldr   w5, [x5]
    mov   w6, #SCRH
pf_row:
    mov   x7, x4
    mov   w8, #SCRW
pf_col:
    str   w0, [x7], #4
    subs  w8, w8, #1
    b.ne  pf_col
    add   x4, x4, w5, uxtw
    subs  w6, w6, #1
    b.ne  pf_row
    // Flush the filled FB out of any data cache to DRAM (PoC) so the DE2 DMA reads our
    // pixels, not stale DRAM. If caches are (unexpectedly) still on, THIS is what makes
    // the POST colour appear -> proves the dark screen was cache coherency, not the
    // display path. dc cvac is a no-op when the cache is already off (harmless).
    ldr   x4, =fb_base
    ldr   x4, [x4]
    ldr   x5, =fb_pitch
    ldr   w5, [x5]
    mov   w6, #SCRH
    mul   w5, w5, w6                       // total FB bytes = pitch * SCRH
    add   x5, x4, x5
pf_flush:
    dc    cvac, x4
    add   x4, x4, #64
    cmp   x4, x5
    b.lo  pf_flush
    dsb   sy
    ret

// post_delay: spin ~0.5 s off the generic timer so each beacon colour is visible.
post_delay:
    mrs   x0, cntpct_el0
    ldr   x1, =12000000                   // ~0.5 s at 24 MHz
    add   x1, x0, x1
pd1:
    mrs   x0, cntpct_el0
    cmp   x0, x1
    b.lo  pd1
    ret
.endif

// (PD18 status-LED helpers — led_init / led_set / led_halfsec — live in
// panel.inc.s now, shared by the LEDTEST loop above and the PANELDBG stage beacon.)

// read_keys: real input via the A64 UART0 serial console (16550). Each received
// byte is one keypress; WASD = d-pad, Enter/Space = A, Backspace/DEL = B. AUTOTEST
// builds replace this with the scripted pad (auto_input). Leaf.
read_keys:
.ifdef AUTOTEST
    b     auto_input
.endif
    ldr   x0, =v_pad                      // v_padp = v_pad
    ldr   w1, [x0]
    ldr   x2, =v_padp
    str   w1, [x2]
    ldr   x3, =UART0_LSR
    ldr   w4, [x3]
    tst   w4, #0x01                       // data ready?
    b.eq  rk_none
    ldr   x3, =UART0_RBR
    ldr   w4, [x3]
    and   w4, w4, #0xFF
    mov   w5, #0
    cmp   w4, #'w'
    b.eq  rk_u
    cmp   w4, #'W'
    b.eq  rk_u
    cmp   w4, #'s'
    b.eq  rk_d
    cmp   w4, #'S'
    b.eq  rk_d
    cmp   w4, #'a'
    b.eq  rk_l
    cmp   w4, #'A'
    b.eq  rk_l
    cmp   w4, #'d'
    b.eq  rk_r
    cmp   w4, #'D'
    b.eq  rk_r
    cmp   w4, #0x0D
    b.eq  rk_a
    cmp   w4, #' '
    b.eq  rk_a
    cmp   w4, #0x08
    b.eq  rk_b
    cmp   w4, #0x7F
    b.eq  rk_b
    b     rk_store
rk_u:
    mov   w5, #PAD_U
    b     rk_store
rk_d:
    mov   w5, #PAD_D
    b     rk_store
rk_l:
    mov   w5, #PAD_L
    b     rk_store
rk_r:
    mov   w5, #PAD_R
    b     rk_store
rk_a:
    mov   w5, #PAD_A
    b     rk_store
rk_b:
    mov   w5, #PAD_B
rk_store:
    ldr   x0, =v_pad
    str   w5, [x0]
    b     rk_edge
rk_none:
    ldr   x0, =v_pad
    str   wzr, [x0]
rk_edge:
    ldr   x0, =v_pad
    ldr   w1, [x0]
    ldr   x2, =v_padp
    ldr   w2, [x2]
    mvn   w2, w2
    and   w1, w1, w2
    ldr   x0, =v_pade
    str   w1, [x0]
    ret

// ============================================================================
// framebuffer primitives  (32bpp XRGB; addr = fb_base + y*pitch + x*4)
// ============================================================================
pchar:
    sub   w2, w2, #32
    ldr   x3, =font_data
    add   x3, x3, w2, uxtw #3
    ldr   x4, =fb_base
    ldr   x4, [x4]
    ldr   x5, =fb_pitch
    ldr   w5, [x5]
    umull x6, w1, w5
    add   x4, x4, x6
    add   x4, x4, w0, uxtw #2
    ldr   x6, =palette
    ldr   x7, =d_fg
    ldr   w7, [x7]
    ldr   w7, [x6, w7, uxtw #2]
    ldr   x8, =d_bg
    ldr   w8, [x8]
    ldr   w8, [x6, w8, uxtw #2]
    mov   w9, #8
pchar_row:
    ldrb  w10, [x3], #1
    mov   w11, #0x80
pchar_col:
    tst   w10, w11
    csel  w12, w7, w8, ne
    str   w12, [x4], #4
    lsr   w11, w11, #1
    cbnz  w11, pchar_col
    add   x4, x4, x5
    sub   x4, x4, #32
    subs  w9, w9, #1
    b.ne  pchar_row
    ret

pstr:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, w0
    mov   w20, w1
    mov   x21, x2
pstr_l:
    ldrb  w2, [x21], #1
    cbz   w2, pstr_d
    mov   w0, w19
    mov   w1, w20
    bl    pchar
    add   w19, w19, #8
    b     pstr_l
pstr_d:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

frect:
    ldr   x4, =palette
    ldr   x5, =d_fg
    ldr   w5, [x5]
    ldr   w4, [x4, w5, uxtw #2]
    ldr   x10, =fb_base
    ldr   x10, [x10]
    ldr   x11, =fb_pitch
    ldr   w11, [x11]
fr_row:
    cbz   w3, fr_done
    umull x6, w1, w11
    add   x6, x10, x6
    add   x6, x6, w0, uxtw #2
    mov   w7, w2
fr_col:
    str   w4, [x6], #4
    subs  w7, w7, #1
    b.ne  fr_col
    add   w1, w1, #1
    sub   w3, w3, #1
    b     fr_row
fr_done:
    ret

picon:
    ldr   x3, =icon_data
    lsl   w11, w2, #8
    add   x3, x3, w11, uxtw
    ldr   x9, =palette
    ldr   x4, =fb_base
    ldr   x4, [x4]
    ldr   x5, =fb_pitch
    ldr   w5, [x5]
    umull x6, w1, w5
    add   x4, x4, x6
    add   x4, x4, w0, uxtw #2
    mov   w6, #16
pic_row:
    mov   w7, #16
pic_col:
    ldrb  w8, [x3], #1
    ldr   w10, [x9, w8, uxtw #2]
    str   w10, [x4], #4
    subs  w7, w7, #1
    b.ne  pic_col
    add   x4, x4, x5
    sub   x4, x4, #64
    subs  w6, w6, #1
    b.ne  pic_row
    ret

setfb:
    ldr   x2, =d_fg
    str   w0, [x2]
    ldr   x2, =d_bg
    str   w1, [x2]
    ret

set_fg:
    ldr   x1, =d_fg
    str   w0, [x1]
    ret

load_palette:
    ldr   x0, =v_theme
    ldr   w0, [x0]
    ldr   x1, =theme_pals
    lsl   w4, w0, #6
    add   x1, x1, w4, uxtw
    ldr   x2, =palette
    mov   w3, #16
lp_l:
    ldr   w0, [x1], #4
    str   w0, [x2], #4
    subs  w3, w3, #1
    b.ne  lp_l
    ret

clear_screen:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #0
    bl    set_fg
    mov   w0, #0
    mov   w1, #0
    mov   w2, #SCRW
    mov   w3, #SCRH
    bl    frect
    ldp   x29, x30, [sp], #16
    ret

two_digits:
    mov   w1, #'0'
td_l:
    cmp   w0, #10
    b.lo  td_d
    sub   w0, w0, #10
    add   w1, w1, #1
    b     td_l
td_d:
    add   w0, w0, #'0'
    ret

// ============================================================================
// launcher (M1) — 4-column icon grid, selected label inverted
// ============================================================================
draw_launcher:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    bl    load_palette
    bl    clear_screen
    mov   w0, #1
    bl    set_fg
    mov   w0, #0
    mov   w1, #0
    mov   w2, #SCRW
    mov   w3, #16
    bl    frect
    mov   w0, #0
    mov   w1, #1
    bl    setfb
    mov   w0, #8
    mov   w1, #4
    ldr   x2, =s_title
    bl    pstr
    mov   w19, #0
dl_item:
    mov   w0, w19
    bl    icon_x
    mov   w20, w0
    mov   w0, w19
    bl    icon_y
    mov   w1, w0
    mov   w0, w20
    mov   w2, w19
    bl    picon
    mov   w0, w19
    bl    draw_label_for
    add   w19, w19, #1
    cmp   w19, #NICONS
    b.ne  dl_item
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

icon_x:
    and   w0, w0, #3
    mov   w1, #112
    mul   w0, w0, w1
    add   w0, w0, #16
    ret
icon_y:
    lsr   w0, w0, #2
    mov   w1, #120
    mul   w0, w0, w1
    add   w0, w0, #48
    ret

draw_label_for:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, w0
    bl    icon_x
    mov   w20, w0
    mov   w0, w19
    bl    icon_y
    add   w21, w0, #20
    ldr   x0, =v_sel
    ldr   w0, [x0]
    cmp   w0, w19
    b.ne  dlf_normal
    mov   w0, #0
    mov   w1, #1
    b     dlf_set
dlf_normal:
    mov   w0, #1
    mov   w1, #0
dlf_set:
    bl    setfb
    ldr   x1, =icon_lbl
    ldr   w2, [x1, w19, uxtw #2]
    mov   w0, w20
    mov   w1, w21
    bl    pstr
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

draw_highlight:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_selp
    ldr   w0, [x0]
    bl    draw_label_for
    ldr   x0, =v_sel
    ldr   w0, [x0]
    bl    draw_label_for
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// input / navigation (M2)
// ============================================================================
update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_inapp
    ldr   w0, [x0]
    cbnz  w0, up_app
    bl    nav_input
    ldp   x29, x30, [sp], #16
    ret
up_app:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_B
    b.eq  up_disp
    bl    enter_launcher
    ldp   x29, x30, [sp], #16
    ret
up_disp:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #3
    b.ne  up_d1
    bl    music_tick
up_d1:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #5
    b.ne  up_d2
    bl    theme_input
up_d2:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #7
    b.ne  up_pm
    bl    dostris_update
up_pm:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #9
    b.ne  up_ol
    bl    pacman_update
up_ol:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #8
    b.ne  up_tk
    bl    outlast_update
up_tk:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #6
    b.ne  up_fl
    bl    tracker_update
up_fl:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #4
    b.ne  up_np
    bl    files_update
up_np:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #10
    b.ne  up_pt
    bl    paint_update
up_pt:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #2
    b.ne  up_d3
    bl    notepad_update
up_d3:
    ldp   x29, x30, [sp], #16
    ret

nav_input:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  nav_dir
    ldr   x0, =v_sel
    ldr   w0, [x0]
    ldr   x1, =v_app
    str   w0, [x1]
    ldr   x1, =v_inapp
    mov   w2, #1
    str   w2, [x1]
    bl    enter_app
    ldp   x29, x30, [sp], #16
    ret
nav_dir:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_U
    b.eq  nd1
    bl    sel_up
nd1:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_D
    b.eq  nd2
    bl    sel_down
nd2:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_L
    b.eq  nd3
    bl    sel_left
nd3:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_R
    b.eq  nd4
    bl    sel_right
nd4:
    ldp   x29, x30, [sp], #16
    ret

sel_right:
    ldr   x2, =v_sel
    ldr   w0, [x2]
    ldr   x3, =v_selp
    str   w0, [x3]
    add   w0, w0, #1
    cmp   w0, #NICONS
    csel  w0, wzr, w0, hs
    str   w0, [x2]
    b     mark_hl
sel_left:
    ldr   x2, =v_sel
    ldr   w0, [x2]
    ldr   x3, =v_selp
    str   w0, [x3]
    cbnz  w0, sl_dec
    mov   w0, #NICONS
sl_dec:
    sub   w0, w0, #1
    str   w0, [x2]
    b     mark_hl
sel_down:
    ldr   x2, =v_sel
    ldr   w0, [x2]
    add   w1, w0, #4
    cmp   w1, #NICONS
    b.hs  sd_no
    ldr   x3, =v_selp
    str   w0, [x3]
    str   w1, [x2]
    b     mark_hl
sd_no:
    ret
sel_up:
    ldr   x2, =v_sel
    ldr   w0, [x2]
    cmp   w0, #4
    b.lo  su_no
    ldr   x3, =v_selp
    str   w0, [x3]
    sub   w0, w0, #4
    str   w0, [x2]
    b     mark_hl
su_no:
    ret
mark_hl:
    ldr   x0, =pf_hl
    mov   w1, #1
    str   w1, [x0]
    ret

enter_app:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #7
    b.ne  ea1
    bl    dostris_init
ea1:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #3
    b.ne  ea_pm
    bl    music_init
ea_pm:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #9
    b.ne  ea_ol
    bl    pacman_init
ea_ol:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #8
    b.ne  ea_tk
    bl    outlast_init
ea_tk:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #6
    b.ne  ea_fl
    bl    tracker_init
ea_fl:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #4
    b.ne  ea_np
    bl    files_init
ea_np:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #10
    b.ne  ea_npd
    bl    paint_init
ea_npd:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #2
    b.ne  ea2
    bl    notepad_init
ea2:
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

enter_launcher:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_inapp
    str   wzr, [x0]
    bl    music_silence
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// render_partials / full_redraw
// ============================================================================
render_partials:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_inapp
    ldr   w0, [x0]
    cbnz  w0, rp_app
    ldr   x0, =pf_hl
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    draw_highlight
    ldr   x0, =pf_hl
    str   wzr, [x0]
rp_done:
    ldp   x29, x30, [sp], #16
    ret
rp_app:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #1
    b.eq  rp_clock
    cmp   w0, #3
    b.eq  rp_music
    cmp   w0, #7
    b.eq  rp_dostris
    ldp   x29, x30, [sp], #16
    ret
rp_clock:
    ldr   x0, =pf_clk
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    draw_clock_time
    ldr   x0, =pf_clk
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret
rp_music:
    ldr   x0, =pf_score
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    draw_music_status
    ldr   x0, =pf_score
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret
rp_dostris:
    ldr   x0, =pf_pc
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    draw_piece_partial
    ldr   x0, =pf_pc
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret

full_redraw:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_dirty
    str   wzr, [x0]
    ldr   x0, =pf_hl
    str   wzr, [x0]
    ldr   x0, =pf_pc
    str   wzr, [x0]
    ldr   x0, =v_inapp
    ldr   w0, [x0]
    cbnz  w0, fr_app2
    bl    draw_launcher
    ldp   x29, x30, [sp], #16
    ret
fr_app2:
    bl    draw_app
    ldp   x29, x30, [sp], #16
    ret

    .include "panel.inc.s"
    .include "apps.inc.s"
    .include "dostris.inc.s"
    .include "paint.inc.s"
    .include "pacman.inc.s"
    .include "outlast.inc.s"
    .include "tracker.inc.s"
    .include "fs.inc.s"
