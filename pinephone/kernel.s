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
.equ GLB_SIZE,   0x0110000C      // mixer0 output size
.equ BLD_FILL,   0x01101000      // blender pipe fill-colour control
.equ BLD_CH_ISZ, 0x01101008      // blender ch0 input size
.equ BLD_SIZE,   0x0110108C      // blender output size
.equ OVL_ATTR,   0x01103000      // UI overlay layer0 attribute control
.equ OVL_MBSIZE, 0x01103004      // UI overlay layer0 memory-block size
.equ OVL_COORD,  0x01103008      // UI overlay layer0 coordinate
.equ OVL_PITCH,  0x0110300C      // UI overlay layer0 pitch (bytes/row)
.equ OVL_TOPADD, 0x01103010      // UI overlay layer0 top framebuffer address
.equ OVL_SIZE,   0x01103088      // UI overlay window size
// A64 UART0 (16550-compatible) — the serial console (on the headphone jack)
.equ UART0_RBR,  0x01C28000      // receive buffer register
.equ UART0_LSR,  0x01C28014      // line status register (bit0 = data ready)
// A64 I2S/PCM0 — TX FIFO for the audio codec PCM stream
.equ I2S_TXFIFO, 0x01C22020      // I2S0 TX data FIFO
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
_start:
    mrs   x0, mpidr_el1                   // park secondary cores
    and   x0, x0, #0xFF
    cbz   x0, core0
hang:
    wfe
    b     hang
core0:
    ldr   x0, =STACK_TOP
    mov   sp, x0
    ldr   x0, =VARS                       // clear the variable block
    mov   w2, #256
mclr:
    str   wzr, [x0], #4
    subs  w2, w2, #1
    b.ne  mclr
    bl    fb_init                         // bring up the DE2 UI layer
    bl    fs_init                         // load/format the USV1 disk
    bl    draw_launcher
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
// framebuffer bring-up (Allwinner A64 Display Engine 2.0, mixer 0 UI layer)
// ============================================================================
// Assumes the SPL/U-Boot stage already initialised DRAM and the panel clock path
// (TCON0 + MIPI-DSI), exactly as the Pi relies on the VideoCore firmware for HDMI.
// We program the mixer to scan out our XRGB8888 framebuffer at PINE_FB.
fb_init:
    ldr   x0, =GLB_CTL
    mov   w1, #1
    str   w1, [x0]                        // mixer enable
    ldr   w1, =(((SCRH-1)<<16)|(SCRW-1))
    ldr   x0, =GLB_SIZE
    str   w1, [x0]
    ldr   x0, =BLD_SIZE
    str   w1, [x0]
    ldr   x0, =BLD_CH_ISZ
    str   w1, [x0]
    ldr   x0, =BLD_FILL
    mov   w2, #1
    str   w2, [x0]                        // pipe 0 enable
    ldr   x0, =OVL_ATTR
    ldr   w2, =0xFF000405                 // glob-alpha=FF, fmt=XRGB8888(4<<8), LAY_EN
    str   w2, [x0]
    ldr   x0, =OVL_MBSIZE
    str   w1, [x0]
    ldr   x0, =OVL_SIZE
    str   w1, [x0]
    ldr   x0, =OVL_COORD
    str   wzr, [x0]
    ldr   x0, =OVL_PITCH
    ldr   w2, =PINE_PITCH
    str   w2, [x0]
    ldr   x0, =OVL_TOPADD
    ldr   w2, =PINE_FB
    str   w2, [x0]
    // record the framebuffer for the primitives
    ldr   x0, =fb_base
    ldr   x1, =PINE_FB
    str   x1, [x0]
    ldr   x0, =fb_pitch
    ldr   w1, =PINE_PITCH
    str   w1, [x0]
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

    .include "apps.inc.s"
    .include "dostris.inc.s"
    .include "paint.inc.s"
    .include "pacman.inc.s"
    .include "outlast.inc.s"
    .include "tracker.inc.s"
    .include "fs.inc.s"
