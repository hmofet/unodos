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

// ---- BCM2837 (Pi 3) peripheral block ---------------------------------------
.equ PERIPH,        0x3F000000
.equ SYS_TIMER_CLO, 0x3F003004           // free-running 1MHz counter (low 32)
.equ UART0_DR,      0x3F201000           // PL011 data register
.equ UART0_FR,      0x3F201018           // PL011 flag register (bit4 = RXFE)
.equ MBOX_READ,     0x3F00B880
.equ MBOX_STATUS,   0x3F00B898
.equ MBOX_WRITE,    0x3F00B8A0
.equ MBOX_FULL,     0x80000000
.equ MBOX_EMPTY,    0x40000000
.equ MBOX_CH_PROP,  8
// PWM headphone-jack tone path (real hardware; the harness sinks these writes)
.equ CM_PWMCTL,     0x3F1010A0
.equ CM_PWMDIV,     0x3F1010A4
.equ PWM_CTL,       0x3F20C000
.equ PWM_RNG1,      0x3F20C010
.equ PWM_DAT1,      0x3F20C014

.equ FRAME_US,      16667                 // ~60 Hz frame period (microseconds)

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
// ---- Tracker (app 6) ----
.equ tk_row,   VARS+176                   // cursor row 0..31
.equ tk_col,   VARS+180                   // cursor channel 0..3
.equ tk_prow,  VARS+184                   // playing row
.equ tk_tmr,   VARS+188                   // frames until next row
.equ tk_orow,  VARS+192                   // previous cursor row (partial redraw)
.equ tk_ocol,  VARS+196
.equ tk_oprow, VARS+200                   // previous play row
.equ pf_tk,    VARS+204                   // tracker partial-redraw flag
// ---- OutLast (app 8) ----
.equ ol_carx,  VARS+208                   // car column 0..39
.equ ol_scroll,VARS+212
.equ ol_dist,  VARS+216
.equ ol_over,  VARS+220
.equ ol_ctr,   VARS+224
.equ pf_ol,    VARS+228
// ---- Paint (app 10) ----
.equ pt_cx,    VARS+232                   // cursor col (canvas 0..31 / palette 0..7)
.equ pt_cy,    VARS+236                   // cursor row 0..15, 16 = palette row
.equ pt_ink,   VARS+240                   // palette index 1..8
.equ pf_pt,    VARS+244
.equ pt_ocx,   VARS+248
.equ pt_ocy,   VARS+252
// ---- Notepad (app 2, USV1-backed) ----
.equ np_len,   VARS+256
.equ np_saved, VARS+260                   // 1 once NOTE.TXT exists on the store
// ---- Pac-Man (app 9) ----
.equ pm_state, VARS+268                   // 1 READY / 2 PLAY / 4 OVER
.equ pm_statet,VARS+272                   // READY countdown (frames)
.equ pm_score, VARS+276
.equ pm_lives, VARS+280
.equ pm_dots,  VARS+284
.equ pm_fright,VARS+288                   // frightened frames left
.equ pm_mode,  VARS+292                   // scatter/chase schedule index
.equ pm_modet, VARS+296
.equ pm_kills, VARS+300                   // ghosts eaten this fright
.equ pm_px,    VARS+304                   // pac tile x/y
.equ pm_py,    VARS+308
.equ pm_pdir,  VARS+312                   // 0 up 1 left 2 down 3 right
.equ pm_pnext, VARS+316
.equ pm_ptmr,  VARS+320                   // frames until pac steps
.equ pm_gtmr,  VARS+324                   // frames until ghosts step
.equ pf_pm,    VARS+328
.equ pm_opx,   VARS+332                   // pac's previous tile (erase)
.equ pm_opy,   VARS+336
.equ pm_level, VARS+340
.equ pm_hi,    VARS+344
.equ pm_gtick, VARS+348                   // ghost-step parity (fright half speed)
.equ pm_gh,    VARS+352                   // 4 ghosts x 28 bytes (see GH_*)
.equ GH_X,   0
.equ GH_Y,   4
.equ GH_DIR, 8
.equ GH_ST,  12
.equ GH_TMR, 16
.equ GH_OX,  20
.equ GH_OY,  24
.equ GH_SZ,  28
.equ pm_gi,    VARS+464                   // ghost loop index
.equ pm_tgx,   VARS+468                   // steering target tile
.equ pm_tgy,   VARS+472
.equ pm_bestd, VARS+476                   // steering argmin scratch
.equ pm_bdir,  VARS+480
.equ pm_moved, VARS+484                   // this ghost moved (needs erase)
.equ palette,  VARS+0x200                 // 16 XRGB words
.equ clk_str,  VARS+0x240                 // 9 bytes
.equ numstr,   VARS+0x250                 // 6 bytes
.equ g_board,  VARS+0x260                 // BW*BH bytes
.equ tk_pat,   VARS+0x300                 // 32 rows x 4 channels, 1 byte/cell
.equ str_buf,  VARS+0x380                 // 64-byte text scratch
.equ pt_canvas,VARS+0x400                 // 32x16 canvas, 1 palette idx/cell
.equ np_buf,   VARS+0x600                 // Notepad text (NP_MAX bytes)
.equ pm_maze,  VARS+0x700                 // 28x25 maze bytes (0x700..0x9BC)

// ---- USV1 storage (fs.inc.s) — a fixed RAM region the harness persists ----
.equ FS_BASE,  0x00380000                 // magic/count/used + dir + heap (4KB)
.equ FS_HEAP,  FS_BASE+256
.equ FS_HEAPSZ,3840                       // 4096-256
.equ FS_MAXF,  15
.equ NP_MAX,   250

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
    mov   w2, #640                        // 640 words = 2.5KB (incl. app buffers)
mclr:
    str   wzr, [x0], #4
    subs  w2, w2, #1
    b.ne  mclr
    bl    fb_init                         // ask the GPU for a framebuffer
    bl    fs_init                         // mount (or format+seed) the USV1 store
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
    ldr   w1, =0x48006                    // set pixel order (1 = RGB)
    str   w1, [x0, #64]
    mov   w1, #4
    str   w1, [x0, #68]
    str   w1, [x0, #72]
    mov   w1, #1
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
    ldr   x0, =v_pad                      // v_padp = v_pad
    ldr   w1, [x0]
    ldr   x2, =v_padp
    str   w1, [x2]
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

// fmt_dec: w0 = value 0..99999, x1 = dest -> decimal ASCII, NUL-terminated,
// leading zeros suppressed. Leaf (clobbers w0-w5, x1).
fmt_dec:
    mov   w2, #10000
    mov   w3, #0                          // started flag
fd_dig:
    udiv  w4, w0, w2
    msub  w0, w4, w2, w0                  // remainder
    cbnz  w4, fd_put
    cbnz  w3, fd_put
    cmp   w2, #1
    b.ne  fd_next                         // suppress leading zero
fd_put:
    add   w4, w4, #'0'
    strb  w4, [x1], #1
    mov   w3, #1
fd_next:
    mov   w5, #10
    udiv  w2, w2, w5
    cbnz  w2, fd_dig
    strb  wzr, [x1]
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
    b.ne  up_d3
    bl    dostris_update
up_d3:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #2
    b.ne  up_d4
    bl    notepad_input
up_d4:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #6
    b.ne  up_d5
    bl    tracker_update
up_d5:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #8
    b.ne  up_d6
    bl    outlast_update
up_d6:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #9
    b.ne  up_d7
    bl    pacman_update
up_d7:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #10
    b.ne  up_d8
    bl    paint_update
up_d8:
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
    b.ne  ea2
    bl    music_init
ea2:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #2
    b.ne  ea3
    bl    notepad_enter
ea3:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #6
    b.ne  ea4
    bl    tracker_init
ea4:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #8
    b.ne  ea5
    bl    outlast_init
ea5:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #9
    b.ne  ea6
    bl    pacman_init
ea6:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #10
    b.ne  ea7
    bl    paint_init
ea7:
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
    cmp   w0, #6
    b.eq  rp_tracker
    cmp   w0, #8
    b.eq  rp_outlast
    cmp   w0, #9
    b.eq  rp_pacman
    cmp   w0, #10
    b.eq  rp_paint
    ldp   x29, x30, [sp], #16
    ret
rp_tracker:
    ldr   x0, =pf_tk
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    tracker_partial
    ldr   x0, =pf_tk
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret
rp_outlast:
    ldr   x0, =pf_ol
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    outlast_partial
    ldr   x0, =pf_ol
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret
rp_pacman:
    ldr   x0, =pf_pm
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    pacman_partial
    ldr   x0, =pf_pm
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret
rp_paint:
    ldr   x0, =pf_pt
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    paint_partial
    ldr   x0, =pf_pt
    str   wzr, [x0]
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
    ldr   x0, =pf_tk
    str   wzr, [x0]
    ldr   x0, =pf_ol
    str   wzr, [x0]
    ldr   x0, =pf_pm
    str   wzr, [x0]
    ldr   x0, =pf_pt
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
    .include "fs.inc.s"
    .include "tracker.inc.s"
    .include "outlast.inc.s"
    .include "pacman.inc.s"
    .include "paint.inc.s"
