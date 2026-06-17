# ============================================================================
# UnoDOS / PowerPC Macintosh (32-bit PowerPC) — milestones 1-3.
# ============================================================================
# The ELEVENTH fresh contract-driven port and the FIRST PowerPC (big-endian RISC)
# world — a brand-new ISA needing a new GAS-PPC dialect (# comments) and a
# from-scratch harness. It boots over Open Firmware (NO Mac OS): OF loads this
# client program into RAM and enters _start with the IEEE-1275 client-interface
# entry in r5. The kernel makes a few OF client calls to find the `screen` device
# and read its framebuffer address + linebytes, then draws into that linear
# surface directly. MINIMAL profile (CONTRACT-ARCH §9): a 4-column icon launcher,
# one full-screen app at a time, directional nav.
#
# There are no hardware tiles — we plot an 8x8 font and 16x16 icons pixel by pixel
# into a 640x480 32bpp (XRGB8888) framebuffer, each pixel's palette INDEX looked
# up in a 16-entry 32-bit table in RAM (so the Theme app recolours by swapping it).
# PowerPC is big-endian, so the framebuffer words store as XRGB byte-for-byte.
#
# M1: boot -> OF client calls -> a rendered launcher (title bar + 4-col grid).
# M2: a frame-paced loop, a d-pad selection highlight, A launches an app
#     full-screen, B returns. (Real ADB input is a future driver; the milestones
#     are driven by the AUTOTEST scripted pad, like every other port.)
# M3: full-screen apps — SysInfo, live Clock, Notepad, Files, Theme (palette),
#     Music (UI/timing; the Mac sound path is a future driver), and Dostris.
#
# Contract-owned (Phase 4): the screen geometry comes from unogen
# ([world.ppcmac] -> gen/ppcmac/sys_gen.inc).
# ============================================================================

.include "../unodef/gen/ppcmac/sys_gen.inc"      # SCRW/SCRH/SCRCOLS/SCRROWS
.include "build/gfxequ.inc"                       # NICONS/NTHEMES/MUSIC_COUNT

# ---- helper macros: load/store from an absolute symbol ----------------------
.macro LA rD, sym
    lis   \rD, \sym@ha
    addi  \rD, \rD, \sym@l
.endm
.macro LWZA rD, sym
    lis   \rD, \sym@ha
    lwz   \rD, \sym@l(\rD)
.endm
.macro STWA rS, sym, rT
    lis   \rT, \sym@ha
    stw   \rS, \sym@l(\rT)
.endm

# pad bits (active-high) — same layout as the Pi/GBA so AUTOTEST scripts match
.equ PAD_A,   0x01
.equ PAD_B,   0x02
.equ PAD_SEL, 0x04
.equ PAD_ST,  0x08
.equ PAD_R,   0x10
.equ PAD_L,   0x20
.equ PAD_U,   0x40
.equ PAD_D,   0x80

.equ FRAME_SPIN, 0x00010000      # busy-loop iterations per frame (~60 Hz on real HW)

# Dostris geometry (board cells; 16px cells)
.equ BW, 10
.equ BH, 14
.equ CELL, 16
.equ BORG_X, 224
.equ BORG_Y, 64
.equ FALLRATE, 30

# ---- fixed RAM layout -------------------------------------------------------
.equ STACK_TOP, 0x00300000
.equ VARS,      0x00400000        # cleared at boot
.equ FBINFO,    0x00420000        # OF entry + framebuffer base/pitch (NOT cleared)
.equ of_entry,  FBINFO+0
.equ fb_base,   FBINFO+4
.equ fb_pitch,  FBINFO+8
.equ ci_buf,    0x00430000        # OF client-interface argument array
.equ gp_buf,    0x00430100        # OF getprop receive buffer
.equ PPC_FB,    0x01000000        # fallback FB (overwritten by OF screen address)

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
.equ palette,  VARS+0x200         # 16 XRGB words
.equ clk_str,  VARS+0x240         # 9 bytes
.equ numstr,   VARS+0x250         # 6 bytes
.equ g_board,  VARS+0x260         # BW*BH bytes

.section .text
.globl _start
_start:
    LA    r1, STACK_TOP                   # set up the stack
    STWA  r5, of_entry, r6                # save the OF client-interface entry
    LA    r3, VARS                        # clear the variable block
    li    r4, 256
    mtctr r4
clr:
    li    r0, 0
    stw   r0, 0(r3)
    addi  r3, r3, 4
    bdnz  clr
    bl    fb_init                         # ask Open Firmware for the framebuffer
    bl    draw_launcher
mainloop:
    bl    wait_vblank
    bl    render_partials
    bl    read_keys
    bl    clock_advance
    bl    update
    LWZA  r3, v_dirty
    cmpwi r3, 0
    beq   mainloop
    bl    full_redraw
    b     mainloop

# ============================================================================
# Open Firmware client interface
# ============================================================================
# of_call: r3 = pointer to the CI argument array; OF fills the return cells.
of_call:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r11, of_entry
    mtctr r11
    bctrl                                 # call OF (r3 = &array preserved)
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# of_finddevice: r3 = device-name ptr -> r3 = phandle
of_finddevice:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    mr    r5, r3                          # save name
    LA    r3, ci_buf
    LA    r4, s_finddevice
    stw   r4, 0(r3)
    li    r4, 1
    stw   r4, 4(r3)                       # nargs
    li    r4, 1
    stw   r4, 8(r3)                       # nrets
    stw   r5, 12(r3)                      # arg0 = name
    bl    of_call
    LA    r3, ci_buf
    lwz   r3, 16(r3)                      # ret0 = phandle
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# of_getprop: r3 = phandle, r4 = prop-name ptr, r5 = buf, r6 = buflen -> r3 = size
of_getprop:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LA    r7, ci_buf
    LA    r8, s_getprop
    stw   r8, 0(r7)
    li    r8, 4
    stw   r8, 4(r7)                       # nargs
    li    r8, 1
    stw   r8, 8(r7)                       # nrets
    stw   r3, 12(r7)                      # phandle
    stw   r4, 16(r7)                      # prop name
    stw   r5, 20(r7)                      # buf
    stw   r6, 24(r7)                      # buflen
    mr    r3, r7
    bl    of_call
    LA    r3, ci_buf
    lwz   r3, 28(r3)                      # ret0 = size
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# fb_init: find the `screen` device, read its framebuffer address + linebytes.
fb_init:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    LA    r3, s_screen
    bl    of_finddevice
    mr    r14, r3                         # r14 = screen phandle
    # address -> fb_base
    mr    r3, r14
    LA    r4, s_address
    LA    r5, gp_buf
    li    r6, 4
    bl    of_getprop
    LWZA  r3, gp_buf
    STWA  r3, fb_base, r4
    # linebytes -> fb_pitch
    mr    r3, r14
    LA    r4, s_linebytes
    LA    r5, gp_buf
    li    r6, 4
    bl    of_getprop
    LWZA  r3, gp_buf
    STWA  r3, fb_pitch, r4
    lwz   r14, 8(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

# wait_vblank: a calibrated busy loop (~one frame). PowerPC's Time Base would pace
# this on real hardware; a spin keeps the minimal port self-contained.
wait_vblank:
    lis   r3, (FRAME_SPIN >> 16)
    ori   r3, r3, (FRAME_SPIN & 0xFFFF)
    mtctr r3
wv1:
    bdnz  wv1
    blr

read_keys:
.ifdef AUTOTEST
    b     auto_input
.endif
    li    r3, 0
    STWA  r3, v_pad, r4
    STWA  r3, v_pade, r4
    blr

# ============================================================================
# framebuffer primitives  (32bpp XRGB; addr = fb_base + y*pitch + x*4)
# ============================================================================
# pchar: r3=px r4=py r5=ascii ; colours from d_fg/d_bg. Leaf.
pchar:
    subi  r5, r5, 32
    LA    r6, font_data
    slwi  r0, r5, 3
    add   r6, r6, r0
    LWZA  r7, fb_base
    LWZA  r8, fb_pitch
    mullw r0, r4, r8
    add   r7, r7, r0
    slwi  r0, r3, 2
    add   r7, r7, r0                      # r7 = pixel addr
    LA    r9, palette
    LWZA  r10, d_fg
    slwi  r10, r10, 2
    lwzx  r10, r9, r10                    # fg colour
    LWZA  r11, d_bg
    slwi  r11, r11, 2
    lwzx  r11, r9, r11                    # bg colour
    li    r12, 8
pchar_row:
    lbz   r9, 0(r6)
    addi  r6, r6, 1
    li    r5, 0x80
pchar_col:
    and.  r0, r9, r5
    beq   pc_bg
    stw   r10, 0(r7)
    b     pc_adv
pc_bg:
    stw   r11, 0(r7)
pc_adv:
    addi  r7, r7, 4
    srwi. r5, r5, 1
    bne   pchar_col
    add   r7, r7, r8
    subi  r7, r7, 32
    subic. r12, r12, 1
    bne   pchar_row
    blr

# pstr: r3=px r4=py r5=strptr (NUL-terminated). Non-leaf.
pstr:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    mr    r14, r3                         # x
    mr    r15, r4                         # y
    mr    r16, r5                         # strptr
pstr_l:
    lbz   r5, 0(r16)
    cmpwi r5, 0
    beq   pstr_d
    mr    r3, r14
    mr    r4, r15
    bl    pchar
    addi  r14, r14, 8
    addi  r16, r16, 1
    b     pstr_l
pstr_d:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

# frect: r3=x r4=y r5=w r6=h ; colour from d_fg. Leaf.
frect:
    LA    r7, palette
    LWZA  r8, d_fg
    slwi  r8, r8, 2
    lwzx  r7, r7, r8                      # colour word
    LWZA  r9, fb_base
    LWZA  r10, fb_pitch
fr_row:
    cmpwi r6, 0
    beq   fr_done
    mullw r0, r4, r10
    add   r11, r9, r0
    slwi  r0, r3, 2
    add   r11, r11, r0
    mr    r12, r5
fr_col:
    stw   r7, 0(r11)
    addi  r11, r11, 4
    subic. r12, r12, 1
    bne   fr_col
    addi  r4, r4, 1
    subi  r6, r6, 1
    b     fr_row
fr_done:
    blr

# picon: r3=px r4=py r5=icon idx -> 16x16 icon. Leaf.
picon:
    LA    r6, icon_data
    slwi  r0, r5, 8
    add   r6, r6, r0
    LA    r9, palette
    LWZA  r7, fb_base
    LWZA  r8, fb_pitch
    mullw r0, r4, r8
    add   r7, r7, r0
    slwi  r0, r3, 2
    add   r7, r7, r0
    li    r12, 16
pic_row:
    li    r5, 16
pic_col:
    lbz   r0, 0(r6)
    addi  r6, r6, 1
    slwi  r0, r0, 2
    lwzx  r0, r9, r0
    stw   r0, 0(r7)
    addi  r7, r7, 4
    subic. r5, r5, 1
    bne   pic_col
    add   r7, r7, r8
    subi  r7, r7, 64
    subic. r12, r12, 1
    bne   pic_row
    blr

# setfb: r3=fg index, r4=bg index. Leaf.
setfb:
    STWA  r3, d_fg, r5
    STWA  r4, d_bg, r5
    blr

# set_fg: r3=fg index. Leaf.
set_fg:
    STWA  r3, d_fg, r4
    blr

# load_palette: copy theme_pals[v_theme] (16 words) -> palette. Leaf.
load_palette:
    LWZA  r3, v_theme
    slwi  r3, r3, 6                       # theme*64
    LA    r4, theme_pals
    add   r4, r4, r3                      # src
    LA    r5, palette                     # dst
    li    r6, 16
lp_l:
    lwz   r0, 0(r4)
    stw   r0, 0(r5)
    addi  r4, r4, 4
    addi  r5, r5, 4
    subic. r6, r6, 1
    bne   lp_l
    blr

# clear_screen: fill the whole framebuffer with palette[0]. Non-leaf.
clear_screen:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 0
    bl    set_fg
    li    r3, 0
    li    r4, 0
    li    r5, SCRW
    li    r6, SCRH
    bl    frect
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# two_digits: r3 = 0..99 -> r4 = tens char, r3 = units char. Leaf.
two_digits:
    li    r4, '0'
td_l:
    cmpwi r3, 10
    blt   td_d
    subi  r3, r3, 10
    addi  r4, r4, 1
    b     td_l
td_d:
    addi  r3, r3, '0'
    blr

# ============================================================================
# launcher (M1)
# ============================================================================
draw_launcher:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    bl    load_palette
    bl    clear_screen
    li    r3, 1
    bl    set_fg
    li    r3, 0
    li    r4, 0
    li    r5, SCRW
    li    r6, 16
    bl    frect
    li    r3, 0
    li    r4, 1
    bl    setfb
    li    r3, 8
    li    r4, 4
    LA    r5, s_title
    bl    pstr
    li    r14, 0
dl_item:
    mr    r3, r14
    bl    icon_x
    mr    r15, r3
    mr    r3, r14
    bl    icon_y
    mr    r4, r3
    mr    r3, r15
    mr    r5, r14
    bl    picon
    mr    r3, r14
    bl    draw_label_for
    addi  r14, r14, 1
    cmpwi r14, NICONS
    bne   dl_item
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

# icon_x: r3=index -> r3 = pixel x. Leaf.
icon_x:
    andi. r3, r3, 3
    mulli r3, r3, 150
    addi  r3, r3, 36
    blr
# icon_y: r3=index -> r3 = pixel y. Leaf.
icon_y:
    srwi  r3, r3, 2
    mulli r3, r3, 120
    addi  r3, r3, 48
    blr

# draw_label_for: r3 = icon index. Non-leaf.
draw_label_for:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    mr    r14, r3
    bl    icon_x
    mr    r15, r3                         # x
    mr    r3, r14
    bl    icon_y
    addi  r16, r3, 20                     # label y
    LWZA  r3, v_sel
    cmpw  r3, r14
    bne   dlf_normal
    li    r3, 0
    li    r4, 1
    b     dlf_set
dlf_normal:
    li    r3, 1
    li    r4, 0
dlf_set:
    bl    setfb
    LA    r4, icon_lbl
    slwi  r5, r14, 2
    lwzx  r5, r4, r5                      # label ptr
    mr    r3, r15
    mr    r4, r16
    bl    pstr
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

draw_highlight:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_selp
    bl    draw_label_for
    LWZA  r3, v_sel
    bl    draw_label_for
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ============================================================================
# input / navigation (M2)
# ============================================================================
update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_inapp
    cmpwi r3, 0
    bne   up_app
    bl    nav_input
    b     up_ret
up_app:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_B
    beq   up_disp
    bl    enter_launcher
    b     up_ret
up_disp:
    LWZA  r3, v_app
    cmpwi r3, 3
    bne   up_d1
    bl    music_tick
up_d1:
    LWZA  r3, v_app
    cmpwi r3, 5
    bne   up_d2
    bl    theme_input
up_d2:
    LWZA  r3, v_app
    cmpwi r3, 7
    bne   up_ret
    bl    dostris_update
up_ret:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

nav_input:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_pade
    andi. r0, r3, PAD_A
    beq   nav_dir
    LWZA  r3, v_sel
    STWA  r3, v_app, r4
    li    r3, 1
    STWA  r3, v_inapp, r4
    bl    enter_app
    b     ni_ret
nav_dir:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_U
    beq   nd1
    bl    sel_up
nd1:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_D
    beq   nd2
    bl    sel_down
nd2:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_L
    beq   nd3
    bl    sel_left
nd3:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_R
    beq   nd4
    bl    sel_right
nd4:
ni_ret:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# grid navigation. Leaf helpers.
sel_right:
    LWZA  r3, v_sel
    STWA  r3, v_selp, r4
    addi  r3, r3, 1
    cmpwi r3, NICONS
    blt   sr_st
    li    r3, 0
sr_st:
    STWA  r3, v_sel, r4
    b     mark_hl
sel_left:
    LWZA  r3, v_sel
    STWA  r3, v_selp, r4
    cmpwi r3, 0
    bne   sl_dec
    li    r3, NICONS
sl_dec:
    subi  r3, r3, 1
    STWA  r3, v_sel, r4
    b     mark_hl
sel_down:
    LWZA  r3, v_sel
    addi  r5, r3, 4
    cmpwi r5, NICONS
    bge   sd_no
    STWA  r3, v_selp, r4
    STWA  r5, v_sel, r4
    b     mark_hl
sd_no:
    blr
sel_up:
    LWZA  r3, v_sel
    cmpwi r3, 4
    blt   su_no
    STWA  r3, v_selp, r4
    subi  r3, r3, 4
    STWA  r3, v_sel, r4
    b     mark_hl
su_no:
    blr
mark_hl:
    li    r3, 1
    STWA  r3, pf_hl, r4
    blr

enter_app:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_app
    cmpwi r3, 7
    bne   ea1
    bl    dostris_init
ea1:
    LWZA  r3, v_app
    cmpwi r3, 3
    bne   ea2
    bl    music_init
ea2:
    li    r3, 1
    STWA  r3, v_dirty, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

enter_launcher:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 0
    STWA  r3, v_inapp, r4
    bl    music_silence
    li    r3, 1
    STWA  r3, v_dirty, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ============================================================================
# render_partials / full_redraw
# ============================================================================
render_partials:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_inapp
    cmpwi r3, 0
    bne   rp_app
    LWZA  r3, pf_hl
    cmpwi r3, 0
    beq   rp_done
    bl    draw_highlight
    li    r3, 0
    STWA  r3, pf_hl, r4
    b     rp_done
rp_app:
    LWZA  r3, v_app
    cmpwi r3, 1
    beq   rp_clock
    cmpwi r3, 3
    beq   rp_music
    cmpwi r3, 7
    beq   rp_dostris
    b     rp_done
rp_clock:
    LWZA  r3, pf_clk
    cmpwi r3, 0
    beq   rp_done
    bl    draw_clock_time
    li    r3, 0
    STWA  r3, pf_clk, r4
    b     rp_done
rp_music:
    LWZA  r3, pf_score
    cmpwi r3, 0
    beq   rp_done
    bl    draw_music_status
    li    r3, 0
    STWA  r3, pf_score, r4
    b     rp_done
rp_dostris:
    LWZA  r3, pf_pc
    cmpwi r3, 0
    beq   rp_done
    bl    draw_piece_partial
    li    r3, 0
    STWA  r3, pf_pc, r4
rp_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

full_redraw:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 0
    STWA  r3, v_dirty, r4
    li    r3, 0
    STWA  r3, pf_hl, r4
    li    r3, 0
    STWA  r3, pf_pc, r4
    LWZA  r3, v_inapp
    cmpwi r3, 0
    bne   fr_app2
    bl    draw_launcher
    b     fred_ret
fr_app2:
    bl    draw_app
fred_ret:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

    .include "apps.inc.s"
    .include "dostris.inc.s"
