// ============================================================================
// UnoDOS / Raspberry Pi (ARM Cortex-A, AArch64) — milestones 1-3.
// ============================================================================
// The NINTH fresh contract-driven port and the FIRST AArch64 (64-bit) world. A
// genuine new register width over the GBA's 32-bit ARM7TDMI, on the same GNU-as
// (GAS) dialect. This is a MINIMAL UnoDOS instance (CONTRACT-ARCH §9): one
// full-screen app at a time, directional nav.
//
// Unlike the GBA (fixed VRAM + GBA I/O), the Pi has no fixed framebuffer: at boot
// we ask the VideoCore firmware, over the mailbox property channel, for a 640x480
// 32bpp (XRGB8888) linear surface and draw into the base it returns. There are no
// hardware tiles — we plot an 8x8 font and 16x16 icons pixel by pixel, each
// pixel's palette INDEX looked up in a 16-entry 32-bit table in RAM (so the Theme
// app recolours by swapping it). Per-frame pacing comes from the BCM system timer.
//
// M1: boot -> mailbox FB -> a rendered launcher (title bar + 4-col colour grid).
// M2: a system-timer-paced loop, a d-pad selection highlight, A launches an app
//     full-screen, B returns. (Real HID input is a future driver; the milestones
//     are driven by the AUTOTEST scripted pad, exactly like every other port.)
// M3: full-screen apps — SysInfo, live Clock, Notepad, Files, Theme (palette),
//     Music (the PWM headphone tone), and Dostris (the falling-blocks game).
//
// Contract-owned (Phase 4): the screen geometry comes from unogen
// ([world.rpi] -> gen/rpi/sys_gen.inc).
// ============================================================================

.include "../unodef/gen/rpi/sys_gen.inc"     // SCRW/SCRH/SCRCOLS/SCRROWS
.include "build/gfxequ.inc"                   // NICONS/NTHEMES/MUSIC_COUNT

// ---- BCM peripheral block --------------------------------------------------
// PERIPH defaults to the BCM2837 (Pi 3) base, so the default build stays
// byte-identical to the harness-verified kernel. A Pi 4 (BCM2711) build
// overrides only the base: aarch64-linux-gnu-as --defsym PERIPH=0xFE000000.
// Every peripheral offset below is identical across Pi 3 and Pi 4.
.ifndef PERIPH
.equ PERIPH,        0x3F000000
.endif
.equ SYS_TIMER_CLO, PERIPH + 0x003004    // free-running 1MHz counter (low 32)
.equ UART0_DR,      PERIPH + 0x201000    // PL011 data register
.equ UART0_FR,      PERIPH + 0x201018    // PL011 flag register (bit4 = RXFE, bit5 = TXFF)
.equ UART0_CR,      PERIPH + 0x201030    // PL011 control (bit0 UARTEN, bit8 TXE, bit9 RXE)
.equ MBOX_READ,     PERIPH + 0x00B880
.equ MBOX_STATUS,   PERIPH + 0x00B898
.equ MBOX_WRITE,    PERIPH + 0x00B8A0
.equ MBOX_FULL,     0x80000000
.equ MBOX_EMPTY,    0x40000000
.equ MBOX_CH_PROP,  8
// PWM headphone-jack tone path (real hardware; the harness sinks these writes)
.equ CM_PWMCTL,     PERIPH + 0x1010A0
.equ CM_PWMDIV,     PERIPH + 0x1010A4
.equ PWM_CTL,       PERIPH + 0x20C000
.equ PWM_RNG1,      PERIPH + 0x20C010
.equ PWM_DAT1,      PERIPH + 0x20C014

// ---- DWC2 (Synopsys DesignWare OTG 2.0) USB host controller ----------------
// Pi 3 (BCM2837): the SoC has ONE DWC2 port wired to the onboard SMSC LAN9514
// USB hub, so a keyboard is always behind a hub (split transactions required).
.equ USB_BASE,      PERIPH + 0x980000
.equ USB_GOTGCTL,   USB_BASE + 0x000
.equ USB_GAHBCFG,   USB_BASE + 0x008      // bit0 GINTMSK, bit5 DMAEN
.equ USB_GUSBCFG,   USB_BASE + 0x00C      // bit29 FORCEHOST, bit30 FORCEDEV
.equ USB_GRSTCTL,   USB_BASE + 0x010      // bit0 CSFTRST, bit4 RXFFLSH, bit5 TXFFLSH, bit31 AHBIDLE
.equ USB_GINTSTS,   USB_BASE + 0x014
.equ USB_GINTMSK,   USB_BASE + 0x018
.equ USB_GRXFSIZ,   USB_BASE + 0x024      // RX FIFO size (words)
.equ USB_GNPTXFSIZ, USB_BASE + 0x028      // non-periodic TX FIFO (start|depth)
.equ USB_GHWCFG2,   USB_BASE + 0x048
.equ USB_HPTXFSIZ,  USB_BASE + 0x100      // host periodic TX FIFO (start|depth)
.equ RST_RXFFLSH,   0x00000010
.equ RST_TXFFLSH,   0x00000020
.equ RST_TXFNUM_ALL,0x00000400            // TXFNUM=0x10 (<<6) = flush all TX FIFOs
.equ GUSBCFG_SRPCAP,0x00000100
.equ GUSBCFG_HNPCAP,0x00000200
.equ GUSBCFG_FORCEHOST,0x20000000
.equ USB_HCFG,      USB_BASE + 0x400      // bits1:0 FSLSPCLKSEL
.equ USB_HPRT,      USB_BASE + 0x440      // host port ctl/status (W1C bits!)
.equ USB_HC0CHAR,   USB_BASE + 0x500      // channel 0 regs (we use channel 0 only)
.equ USB_HC0SPLT,   USB_BASE + 0x504
.equ USB_HC0INT,    USB_BASE + 0x508
.equ USB_HC0TSIZ,   USB_BASE + 0x510
.equ USB_HC0DMA,    USB_BASE + 0x514
.equ USB_PCGCTL,    USB_BASE + 0xE00      // clock gating (0 = clocks on)
// GRSTCTL / HPRT / HCCHAR / HCINT bit fields
.equ RST_CSFTRST,   0x00000001
.equ RST_AHBIDLE,   0x80000000
.equ HPRT_CONNSTS,  0x00000001
.equ HPRT_CONNDET,  0x00000002            // W1C
.equ HPRT_ENA,      0x00000004            // W1C - must mask off when writing!
.equ HPRT_ENCHNG,   0x00000008            // W1C
.equ HPRT_OCCHNG,   0x00000020            // W1C
.equ HPRT_RST,      0x00000100
.equ HPRT_PWR,      0x00001000
.equ HPRT_SPD_MASK, 0x00060000            // bits18:17 (0 hi,1 full,2 low)
.equ HPRT_WC_BITS,  0x0000002E            // CONNDET|ENA|ENCHNG|OCCHNG (clear on write)
.equ HCC_CHENA,     0x80000000
.equ HCC_CHDIS,     0x40000000
.equ HCC_EPDIR_IN,  0x00008000
.equ HCC_LSPD,      0x00020000
.equ HCINT_XFRC,    0x00000001            // transfer complete
.equ HCINT_CHH,     0x00000002            // channel halted
.equ HCINT_STALL,   0x00000008
.equ HCINT_NAK,     0x00000010
.equ HCINT_ACK,     0x00000020
.equ HCINT_NYET,    0x00000040
.equ HCINT_ERRS,    0x00000780            // hard errors only (TXERR|BBL|FRMOV|DTERR)
.equ HCSPLT_ENA,    0x80000000
.equ HCSPLT_COMPL,  0x00010000            // COMPLSPLT (complete-split phase)
// DMA bus alias: peripheral DMA masters see RAM through the 0xC0000000 (uncached)
// window; MMU/caches are off in this kernel so no flush is needed, only the alias.
.equ BUS_ALIAS,     0xC0000000
// PID codes (HCTSIZ bits30:29)
.equ PID_DATA0,     0
.equ PID_DATA1,     2
.equ PID_SETUP,     3
// USB standard request bRequest
.equ REQ_GET_DESC,  6
.equ REQ_SET_ADDR,  5
.equ REQ_SET_CONFIG,9
.equ REQ_SET_FEAT,  3
.equ REQ_GET_STATUS,0
.equ REQ_SET_IFACE, 11

.equ FRAME_US,      16667                 // ~60 Hz frame period (microseconds)

// Framebuffer pixel order (mailbox tag 0x48006): 0 = BGR, 1 = RGB.
// We store each pixel as a little-endian word 0xFFRRGGBB, so the bytes land in
// memory as [B,G,R,A]; the firmware must therefore read byte-0 as BLUE => BGR (0).
// (Requesting RGB(1) made real Pi 3 silicon read byte-0/blue as red -> the desktop
// rendered brown. The harness can't see this; it reads the word back directly.)
// Override for on-metal A/B testing: aarch64-linux-gnu-as --defsym FB_PIXEL_ORDER=1
.ifndef FB_PIXEL_ORDER
.equ FB_PIXEL_ORDER, 0
.endif

// pad bits (active-high) — same layout as the GBA port so AUTOTEST scripts match
.equ PAD_A,   0x01
.equ PAD_B,   0x02
.equ PAD_SEL, 0x04
.equ PAD_ST,  0x08
.equ PAD_R,   0x10
.equ PAD_L,   0x20
.equ PAD_U,   0x40
.equ PAD_D,   0x80

// Dostris geometry (board cells; rendered at 16px cells on the bigger screen)
.equ BW, 10
.equ BH, 14
.equ CELL, 16
.equ BORG_X, 224
.equ BORG_Y, 64
.equ FALLRATE, 30

// Paint geometry (cursor-driven cell canvas + a palette row one 'up' away)
.equ PCW, 36                              // canvas columns
.equ PCH, 24                              // canvas rows
.equ PCELL, 12                            // cell pixels
.equ PCO_X, 16                            // canvas origin x
.equ PCO_Y, 24                            // canvas origin y
.equ NSWATCH, 8                           // palette swatches
.equ PSW_W, 26                            // swatch pitch
.equ PSW_Y, (PCO_Y + PCH*PCELL + 10)      // swatch row y

// Pac-Man geometry (28x25 maze, tile-stepped, one 16px cell per maze tile)
.equ PM_COLS, 28
.equ PM_ROWS, 25
.equ PM_CELL, 16
.equ PMO_X, 16
.equ PMO_Y, 24
.equ GSIZE, 20                            // ghost struct: GX,GY,GDIR,GST,GTMR (words)
.equ FRIGHT_STEPS, 45
.equ PM_STEPFRAMES, 4                     // game step every N frames

// OutLast geometry (pseudo-3D road: 20 perspective bands over 40 logical cols)
.equ OL_BANDS, 20
.equ OL_COLW, 16                          // pixels per logical column (640/40)
.equ OL_BH, 22                            // band pixel height
.equ OLO_Y, 24                            // road top y
.equ OL_RATE, 4                           // frames per scroll step

// Tracker geometry (pattern grid: 16 rows x 4 channels, auto-plays leftmost voice)
.equ NT_ROWS, 16
.equ NT_CH, 4
.equ TK_STEPF, 12                         // frames per playback step
.equ TKO_X, 60
.equ TKO_Y, 44
.equ TK_RH, 22
.equ TK_CW, 80

// ---- fixed RAM layout ------------------------------------------------------
.equ STACK_TOP, 0x00200000
.equ VARS,      0x00300000               // cleared at boot
.equ MBOX_BUF,  0x00310000               // 16-byte aligned mailbox message
.equ FBINFO,    0x00320000               // framebuffer base/pitch (NOT cleared)
.equ fb_base,   FBINFO+0                  // 8 bytes (allocated by the GPU)
.equ fb_pitch,  FBINFO+8                  // 4 bytes (bytes per row)

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
.equ p_cx,     VARS+176                   // Paint cursor cell x
.equ p_cy,     VARS+180                   // Paint cursor cell y (==PCH => palette row)
.equ p_col,    VARS+184                   // Paint current colour index
.equ pm_x,     VARS+188                   // Pac-Man: pac tile x
.equ pm_y,     VARS+192                   // pac tile y
.equ pm_dir,   VARS+196                   // pac direction (0U 1L 2D 3R)
.equ pm_ndir,  VARS+200                   // pac queued direction
.equ pm_score, VARS+204
.equ pm_lives, VARS+208
.equ pm_level, VARS+212
.equ pm_dots,  VARS+216                   // remaining dots
.equ pm_mode,  VARS+220                   // scatter/chase schedule index
.equ pm_modet, VARS+224                   // mode step timer
.equ pm_fr,    VARS+228                   // fright timer (steps)
.equ pm_kills, VARS+232                   // ghosts eaten this fright
.equ pm_st,    VARS+236                   // 0 play / 1 over
.equ pm_sc,    VARS+240                   // step counter (parity)
.equ pm_tgx,   VARS+244                   // ghost target tile (steer)
.equ pm_tgy,   VARS+248
.equ pm_ft,    VARS+252                   // frame timer for step pacing
.equ pm_gh,    VARS+256                   // 3 ghosts * GSIZE bytes (ends VARS+316)
.equ ol_carx,  VARS+316                   // OutLast: car column (0..39)
.equ ol_scroll,VARS+320                   // road scroll position
.equ ol_dist,  VARS+324                   // distance (score)
.equ ol_over,  VARS+328                   // crashed flag
.equ ol_ctr,   VARS+332                   // frames-until-scroll counter
.equ tk_crow,  VARS+336                   // Tracker: cursor row
.equ tk_cch,   VARS+340                   // cursor channel
.equ tk_prow,  VARS+344                   // playing row
.equ tk_ptmr,  VARS+348                   // playback step timer
.equ fl_sel,   VARS+352                   // Files: selected file
.equ fl_view,  VARS+356                   // Files: 0 list / 1 viewing
.equ np_saved, VARS+360                   // Notepad: save-feedback flag
.equ palette,  VARS+0x200                 // 16 XRGB words
.equ clk_str,  VARS+0x240                 // 9 bytes
.equ numstr,   VARS+0x250                 // 6 bytes
.equ g_board,  VARS+0x260                 // BW*BH bytes
.equ pcanvas,  VARS+0x400                 // PCW*PCH Paint canvas (cleared by paint_init)
.equ pm_maze,  VARS+0x800                 // PM_COLS*PM_ROWS mutable maze (copied at new game)
.equ tk_pat,   VARS+0xC00                 // NT_ROWS*NT_CH tracker pattern (cleared at init)
.equ fbuf,     VARS+0x1000                // 4 KB file-view scratch buffer

// ---- USB host (DWC2) state + DMA buffers (well clear of fbuf/canvas/maze) ---
.equ USB_AREA,  VARS+0x2000               // 0x302000
.equ usb_setup, USB_AREA+0x000            // 8-byte SETUP packet  (DMA target)
.equ usb_buf,   USB_AREA+0x040            // 256-byte descriptor/IO buf (DMA)
.equ usb_rpt,   USB_AREA+0x200            // 8-byte HID boot report (DMA)
.equ usb_rptp,  USB_AREA+0x208            // previous report (edge detection)
.equ usb_ok,    USB_AREA+0x220            // 1 once a keyboard is enumerated
.equ kbd_addr,  USB_AREA+0x224            // keyboard device address (2)
.equ kbd_ep,    USB_AREA+0x228            // interrupt-IN endpoint number
.equ kbd_mps,   USB_AREA+0x22C            // endpoint max packet size
.equ kbd_lspd,  USB_AREA+0x230            // 1 = low-speed device
.equ kbd_hub,   USB_AREA+0x234            // upstream hub address (1) for split
.equ kbd_port,  USB_AREA+0x238            // hub port number (1-based) for split
.equ usb_tgl,   USB_AREA+0x23C            // interrupt-IN data toggle (0/1)
.equ cur_addr,  USB_AREA+0x240            // device addr of the in-flight control xfer
.equ cur_mps,   USB_AREA+0x244            // ep0 max packet size of that device
.equ cur_lspd,  USB_AREA+0x248            // low-speed flag of that device
.equ cur_split, USB_AREA+0x24C            // 1 = use split (device is behind the hub)
.equ usb_stat,  USB_AREA+0x250            // on-screen bring-up progress code (diagnostic)
.equ usb_ssint, USB_AREA+0x254            // last START-SPLIT HCINT (serial debug)
.equ usb_csint, USB_AREA+0x258            // last COMPLETE-SPLIT HCINT (serial debug)
.equ usb_kbdrc, USB_AREA+0x25C            // saved kbd-getdesc rc across logging
.equ usb_stage, USB_AREA+0x260            // control-transfer stage (1 SETUP/2 DATA/3 STATUS)
.equ usb_su_ss, USB_AREA+0x264            // SETUP-stage START-SPLIT HCINT (serial debug)
.equ usb_su_cs, USB_AREA+0x268            // SETUP-stage COMPLETE-SPLIT HCINT (serial debug)

.section .text
.global _start
_start:
    // park secondary cores; only core 0 runs UnoDOS
    mrs   x0, mpidr_el1
    and   x0, x0, #0xFF
    cbz   x0, core0
hang:
    wfe
    b     hang
core0:
    ldr   x0, =STACK_TOP
    mov   sp, x0
    // clear the variable block (does not touch FBINFO / mailbox buffer)
    ldr   x0, =VARS
    mov   w2, #256                        // 256 words = 1KB
mclr:
    str   wzr, [x0], #4
    subs  w2, w2, #1
    b.ne  mclr
    ldr   x0, =usb_stat                   // usb_stat lives outside the cleared 1KB
    str   wzr, [x0]
.ifndef AUTOTEST
    bl    uart_tx_init                    // make sure the PL011 TX is enabled
    ldr   x0, =d_boot                     // earliest serial proof-of-life
    bl    dbg_puts
.endif
    bl    fb_init                         // ask the GPU for a framebuffer
    bl    fs_init                         // load/format the USV1 disk
    bl    draw_launcher                   // show the desktop NOW (before USB)
.ifndef AUTOTEST
    bl    usb_init                        // DWC2 host bring-up (can't black-screen now)
    bl    usb_enum                        // enumerate hub + HID keyboard
    ldr   x0, =v_dirty                    // redraw so the USB:NN status reflects the result
    mov   w1, #1
    str   w1, [x0]
.endif
mainloop:
    bl    wait_vblank
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
// framebuffer bring-up (VideoCore mailbox property channel)
// ============================================================================
fb_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =MBOX_BUF
    mov   w1, #120
    str   w1, [x0, #0]                    // total size
    str   wzr, [x0, #4]                   // request
    ldr   w1, =0x48003                    // set physical (display) size
    str   w1, [x0, #8]
    mov   w1, #8
    str   w1, [x0, #12]
    str   w1, [x0, #16]
    mov   w1, #SCRW
    str   w1, [x0, #20]
    mov   w1, #SCRH
    str   w1, [x0, #24]
    ldr   w1, =0x48004                    // set virtual (buffer) size
    str   w1, [x0, #28]
    mov   w1, #8
    str   w1, [x0, #32]
    str   w1, [x0, #36]
    mov   w1, #SCRW
    str   w1, [x0, #40]
    mov   w1, #SCRH
    str   w1, [x0, #44]
    ldr   w1, =0x48005                    // set depth
    str   w1, [x0, #48]
    mov   w1, #4
    str   w1, [x0, #52]
    str   w1, [x0, #56]
    mov   w1, #32
    str   w1, [x0, #60]
    ldr   w1, =0x48006                    // set pixel order (0 = BGR, correct here)
    str   w1, [x0, #64]
    mov   w1, #4
    str   w1, [x0, #68]
    str   w1, [x0, #72]
    mov   w1, #FB_PIXEL_ORDER
    str   w1, [x0, #76]
    ldr   w1, =0x40001                    // allocate framebuffer
    str   w1, [x0, #80]
    mov   w1, #8
    str   w1, [x0, #84]
    str   w1, [x0, #88]
    mov   w1, #16                         // alignment (-> base on return)
    str   w1, [x0, #92]
    str   wzr, [x0, #96]                  // (-> size on return)
    ldr   w1, =0x40008                    // get pitch
    str   w1, [x0, #100]
    mov   w1, #4
    str   w1, [x0, #104]
    str   w1, [x0, #108]
    str   wzr, [x0, #112]                 // (-> pitch on return)
    str   wzr, [x0, #116]                 // end tag
    // mailbox call on channel 8
    ldr   x0, =MBOX_BUF
    orr   x0, x0, #MBOX_CH_PROP
    ldr   x2, =MBOX_STATUS
fbw:
    ldr   w1, [x2]
    tst   w1, #MBOX_FULL
    b.ne  fbw
    ldr   x2, =MBOX_WRITE
    str   w0, [x2]
    ldr   x2, =MBOX_STATUS
fbr:
    ldr   w1, [x2]
    tst   w1, #MBOX_EMPTY
    b.ne  fbr
    ldr   x2, =MBOX_READ
    ldr   w1, [x2]                        // drain the response word
    // read back the allocated base + pitch
    ldr   x0, =MBOX_BUF
    ldr   w1, [x0, #92]
    and   w1, w1, #0x3FFFFFFF             // GPU bus address -> ARM physical
    ldr   x2, =fb_base
    str   x1, [x2]
    ldr   w1, [x0, #112]
    ldr   x2, =fb_pitch
    str   w1, [x2]
    ldp   x29, x30, [sp], #16
    ret

// wait_vblank: pace one frame off the 1MHz system timer (~60 Hz)
wait_vblank:
    ldr   x3, =SYS_TIMER_CLO
    ldr   w0, [x3]
    mov   w1, #FRAME_US
    add   w1, w0, w1
wv1:
    ldr   w0, [x3]
    cmp   w0, w1
    b.lo  wv1
    ret

// read_keys: real input via the PL011 UART serial console (GPIO14/15). Each received
// byte is one keypress; WASD = d-pad, Enter/Space = A, Backspace/DEL = B. Held state
// lasts one frame, so nav reads edges and Dostris reads a per-press step. AUTOTEST
// builds replace this with the scripted pad (auto_input). Leaf.
read_keys:
.ifdef AUTOTEST
    b     auto_input
.endif
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_pad                      // v_padp = v_pad
    ldr   w1, [x0]
    ldr   x2, =v_padp
    str   w1, [x2]
    // USB-HID keyboard takes priority; fall through to the UART console if it
    // reports nothing (or no keyboard is attached).
    bl    usb_poll
    cbz   w0, rk_uart
    ldr   x1, =v_pad
    str   w0, [x1]
    b     rk_edge
rk_uart:
    ldr   x3, =UART0_FR
    ldr   w4, [x3]
    tst   w4, #0x10                       // RXFE: receive FIFO empty?
    b.ne  rk_none
    ldr   x3, =UART0_DR
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
    cmp   w4, #0x0D                        // Enter
    b.eq  rk_a
    cmp   w4, #' '
    b.eq  rk_a
    cmp   w4, #0x08                        // Backspace
    b.eq  rk_b
    cmp   w4, #0x7F                        // DEL
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
    and   w1, w1, w2                       // edges = new & ~prev
    ldr   x0, =v_pade
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// framebuffer primitives  (32bpp XRGB; addr = fb_base + y*pitch + x*4)
// ============================================================================
// pchar: w0=px w1=py w2=ascii ; colours from d_fg/d_bg (palette indices). Leaf.
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
    add   x4, x4, w0, uxtw #2             // x4 = pixel address
    ldr   x6, =palette
    ldr   x7, =d_fg
    ldr   w7, [x7]
    ldr   w7, [x6, w7, uxtw #2]           // fg colour
    ldr   x8, =d_bg
    ldr   w8, [x8]
    ldr   w8, [x6, w8, uxtw #2]           // bg colour
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
    sub   x4, x4, #32                     // back to next row start (8px * 4B)
    subs  w9, w9, #1
    b.ne  pchar_row
    ret

// pstr: w0=px w1=py x2=strptr (NUL-terminated); colours from d_fg/d_bg
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

// frect: w0=x w1=y w2=w w3=h ; colour from d_fg. Leaf.
frect:
    ldr   x4, =palette
    ldr   x5, =d_fg
    ldr   w5, [x5]
    ldr   w4, [x4, w5, uxtw #2]           // colour word
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

// picon: w0=px w1=py w2=icon idx -> 16x16 icon. Leaf.
picon:
    ldr   x3, =icon_data
    lsl   w11, w2, #8                     // icon*256
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
    sub   x4, x4, #64                     // 16px * 4B
    subs  w6, w6, #1
    b.ne  pic_row
    ret

// setfb: w0=fg index, w1=bg index
setfb:
    ldr   x2, =d_fg
    str   w0, [x2]
    ldr   x2, =d_bg
    str   w1, [x2]
    ret

// set_fg: w0=fg index
set_fg:
    ldr   x1, =d_fg
    str   w0, [x1]
    ret

// load_palette: copy theme_pals[v_theme] (16 words) -> palette
load_palette:
    ldr   x0, =v_theme
    ldr   w0, [x0]
    ldr   x1, =theme_pals
    lsl   w4, w0, #6                      // theme*64 bytes
    add   x1, x1, w4, uxtw
    ldr   x2, =palette
    mov   w3, #16
lp_l:
    ldr   w0, [x1], #4
    str   w0, [x2], #4
    subs  w3, w3, #1
    b.ne  lp_l
    ret

// clear_screen: fill the whole framebuffer with palette[0] (the desktop colour)
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

// two_digits: w0 = 0..99 -> w1 = tens char, w0 = units char. Leaf.
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
    // title bar: white strip + inverted title
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
    // icon grid
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
    // USB bring-up status readout (bottom-left): "USB:" + 2-digit progress code.
    // Diagnostic for real-HW USB debugging; see usb_stat values in usb.inc.s.
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #8
    mov   w1, #460
    ldr   x2, =s_usb
    bl    pstr
    ldr   x0, =usb_stat
    ldr   w0, [x0]
    bl    two_digits                      // w1 = tens char, w0 = units char
    mov   w19, w1
    mov   w20, w0
    mov   w0, #40
    mov   w1, #460
    mov   w2, w19
    bl    pchar
    mov   w0, #48
    mov   w1, #460
    mov   w2, w20
    bl    pchar
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// icon_x: w0=index -> w0 = pixel x of its column. Leaf.
icon_x:
    and   w0, w0, #3
    mov   w1, #150
    mul   w0, w0, w1
    add   w0, w0, #36
    ret
// icon_y: w0=index -> w0 = pixel y of its row group. Leaf.
icon_y:
    lsr   w0, w0, #2                      // row = i/4
    mov   w1, #120
    mul   w0, w0, w1
    add   w0, w0, #48
    ret

// draw_label_for: w0 = icon index -> draw its label (normal, inverted if selected)
draw_label_for:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, w0
    bl    icon_x
    mov   w20, w0                         // x
    mov   w0, w19
    bl    icon_y
    add   w21, w0, #20                    // label y = icon y + 20
    ldr   x0, =v_sel
    ldr   w0, [x0]
    cmp   w0, w19
    b.ne  dlf_normal
    mov   w0, #0                          // selected -> inverted
    mov   w1, #1
    b     dlf_set
dlf_normal:
    mov   w0, #1
    mov   w1, #0
dlf_set:
    bl    setfb
    ldr   x1, =icon_lbl
    ldr   w2, [x1, w19, uxtw #2]          // label ptr (32-bit address)
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
    b.ne  up_dp
    bl    dostris_update
up_dp:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #10
    b.ne  up_pm
    bl    paint_update
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

// grid navigation: L/R +-1 (wrap), U/D +-4 (clamp). Leaf helpers.
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
    b.ne  ea_pt
    bl    music_init
ea_pt:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #10
    b.ne  ea_pm
    bl    paint_init
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
// render_partials: small in-loop framebuffer writes
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

// full_redraw: whole-screen redraw (launcher or app)
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

    .include "apps.inc.s"
    .include "dostris.inc.s"
    .include "paint.inc.s"
    .include "pacman.inc.s"
    .include "outlast.inc.s"
    .include "tracker.inc.s"
    .include "fs.inc.s"
    .include "usb.inc.s"
