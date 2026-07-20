@ ============================================================================
@ UnoDOS / Nintendo Game Boy Advance (ARM7TDMI) — milestones 1-3.
@ ============================================================================
@ The FIFTH fresh contract-driven port and the FIRST on ARM — a genuinely new
@ 32-bit RISC world (GNU as / arm-none-eabi). The GBA hardware exceeds the
@ minimal profile, but this is a MINIMAL UnoDOS instance (CONTRACT-ARCH §9): one
@ full-screen app at a time, directional nav. It draws into a flat Mode 3
@ framebuffer (240x160, 16bpp BGR555) — no hardware tiles — plotting an 8x8 font
@ and 16x16 icons pixel by pixel, each pixel's palette INDEX looked up in a
@ 16-entry BGR555 table in IWRAM (so the Theme app recolours by swapping it).
@
@ M1: boot to a rendered launcher (title bar + a 4-col colour icon grid).
@ M2: read REG_KEYINPUT, a VCOUNT-polled per-frame loop, a d-pad selection
@     highlight, A launches the selected app full-screen, B returns.
@ M3: full-screen apps — SysInfo, live Clock, Notepad, Files, Theme (palette),
@     Music (the GBA square channel), and Dostris (the falling-blocks game).
@
@ Contract-owned (Phase 4): the screen geometry comes from unogen
@ ([world.gba] -> gen/gba/sys_gen.inc).
@ ============================================================================

.include "../unodef/gen/gba/sys_gen.inc"     @ SCRW/SCRH/SCRCOLS/SCRROWS
.include "build/gfxequ.inc"                   @ NICONS/NTHEMES/MUSIC_COUNT (assemble-time)

.equ REG_DISPCNT,  0x04000000
.equ REG_VCOUNT,   0x04000006
.equ REG_KEYINPUT, 0x04000130
.equ FB,           0x06000000
.equ SND1CNT_L,    0x04000060
.equ SND1CNT_H,    0x04000062
.equ SND1CNT_X,    0x04000064
.equ SNDCNT_L,     0x04000080
.equ SNDCNT_H,     0x04000082
.equ SNDCNT_X,     0x04000084

@ key bits (active-high after we invert KEYINPUT)
.equ PAD_A,   0x01
.equ PAD_B,   0x02
.equ PAD_SEL, 0x04
.equ PAD_ST,  0x08
.equ PAD_R,   0x10
.equ PAD_L,   0x20
.equ PAD_U,   0x40
.equ PAD_D,   0x80
.equ PAD_SR,  0x100        @ R shoulder
.equ PAD_SL,  0x200        @ L shoulder

@ Dostris geometry (in board cells; rendered at 8px cells)
.equ BW, 10
.equ BH, 12
.equ BORG_X, 88            @ board origin in pixels (centres a 10-wide field)
.equ BORG_Y, 24
.equ FALLRATE, 30

@ ---- IWRAM variables (fixed addresses) -------------------------------------
.equ VARS,     0x03000000
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
.equ mhi,      VARS+148
.equ g_oldpx,  VARS+152
.equ g_oldpy,  VARS+156
.equ g_oldrot, VARS+160
.equ g_bx,     VARS+164
.equ g_by,     VARS+168
.equ a_idx,    VARS+172
.equ a_tmr,    VARS+176
.equ a_pad,    VARS+180
.equ a_gpause, VARS+184
.equ palette,  0x03000200      @ 16 BGR555 hwords
.equ clk_str,  0x03000240      @ 9 bytes
.equ numstr,   0x03000250      @ 6 bytes
.equ g_board,  0x03000260      @ BW*BH = 120 bytes

.section .text
.arm
.global _start
_start:
    b main
    .space 0xC0 - 4, 0          @ ROM header area (logo/title/checksum); mGBA boots zeroed

main:
    ldr sp, =0x03007F00         @ IWRAM stack
    ldr r0, =REG_DISPCNT
    ldr r1, =0x0403             @ Mode 3 + BG2 on
    strh r1, [r0]
    @ initial state
    ldr r0, =VARS
    mov r1, #0
    mov r2, #200
mclr:
    str r1, [r0], #4
    subs r2, r2, #1
    bne mclr
    bl fs_init                  @ USV1 store in EWRAM (format+seed if absent)
    bl draw_launcher
mainloop:
    bl wait_vblank
    bl render_partials
    bl read_keys
    bl clock_advance
    bl update
    ldr r0, =v_dirty
    ldr r1, [r0]
    cmp r1, #0
    beq mainloop
    bl full_redraw
    b mainloop
.ltorg

@ ============================================================================
@ framebuffer helpers
@ ============================================================================
@ wait_vblank: poll VCOUNT for the start of vblank
wait_vblank:
    ldr r0, =REG_VCOUNT
wv1:
    ldrh r1, [r0]
    cmp r1, #160
    bhs wv1
wv2:
    ldrh r1, [r0]
    cmp r1, #160
    blo wv2
    bx lr
.ltorg

@ read_keys: REG_KEYINPUT -> v_pad (active-high) + v_pade (edges)
read_keys:
.ifdef AUTOTEST
    b auto_input
.endif
    ldr r0, =v_pad
    ldr r1, [r0]
    ldr r2, =v_padp
    str r1, [r2]
    ldr r3, =REG_KEYINPUT
    ldrh r1, [r3]
    mvn r1, r1
    ldr r2, =0x03FF
    and r1, r1, r2             @ r1 = current (active-high)
    str r1, [r0]
    ldr r2, =v_padp
    ldr r2, [r2]
    mvn r2, r2
    and r2, r1, r2            @ edges = new & ~prev
    ldr r3, =v_pade
    str r2, [r3]
    bx lr
.ltorg

@ pchar: r0=px r1=py r2=ascii ; colours from d_fg/d_bg (palette indices)
pchar:
    push {r4-r10, lr}
    sub r2, r2, #32
    ldr r3, =font_data
    add r3, r3, r2, lsl #3      @ font row ptr
    ldr r4, =FB
    mov r5, r1, lsl #8
    sub r5, r5, r1, lsl #4      @ py*240
    add r5, r5, r0
    add r4, r4, r5, lsl #1      @ FB addr of (px,py)
    ldr r6, =palette
    ldr r7, =d_fg
    ldr r7, [r7]
    add r7, r6, r7, lsl #1
    ldrh r7, [r7]              @ fg colour
    ldr r8, =d_bg
    ldr r8, [r8]
    add r8, r6, r8, lsl #1
    ldrh r8, [r8]             @ bg colour
    mov r9, #8
pchar_row:
    ldrb r10, [r3], #1
    mov r2, #0x80
pchar_col:
    tst r10, r2
    movne r0, r7
    moveq r0, r8
    strh r0, [r4], #2
    movs r2, r2, lsr #1
    bne pchar_col
    add r4, r4, #(240*2 - 16)
    subs r9, r9, #1
    bne pchar_row
    pop {r4-r10, pc}
.ltorg

@ pstr: r0=px r1=py r2=strptr (NUL-term); colours from d_fg/d_bg
pstr:
    push {r4-r6, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r2
pstr_l:
    ldrb r2, [r6], #1
    cmp r2, #0
    beq pstr_d
    mov r0, r4
    mov r1, r5
    bl pchar
    add r4, r4, #8
    b pstr_l
pstr_d:
    pop {r4-r6, pc}
.ltorg

@ frect: r0=x r1=y r2=w r3=h ; colour from d_fg
@ Two BGR555 pixels pack into one 32-bit word, so the bulk of each row is filled
@ with word stores (str) instead of one strh per pixel — halving the store count
@ on the port's hottest primitive (clear_screen = a 240x160 frect on every
@ full_redraw). Odd start x / odd width fall back to a single strh at each end.
frect:
    push {r4-r8, lr}
    ldr r4, =palette
    ldr r5, =d_fg
    ldr r5, [r5]
    add r4, r4, r5, lsl #1
    ldrh r4, [r4]             @ colour (16-bit, zero-extended)
    orr r4, r4, r4, lsl #16   @ pack the colour into both halves of a word
fr_row:
    cmp r3, #0
    beq fr_done
    ldr r5, =FB
    mov r6, r1, lsl #8
    sub r6, r6, r1, lsl #4
    add r6, r6, r0
    add r5, r5, r6, lsl #1    @ r5 = row base (halfword-aligned)
    movs r7, r2              @ r7 = pixels left this row
    beq fr_rownext
    tst r5, #2               @ word-aligned start?
    beq fr_words
    strh r4, [r5], #2        @ leading unaligned pixel -> now word-aligned
    subs r7, r7, #1
    beq fr_rownext
fr_words:
    cmp r7, #2
    blt fr_tail
    str r4, [r5], #4        @ two pixels at once
    sub r7, r7, #2
    b fr_words
fr_tail:
    cmp r7, #0
    beq fr_rownext
    strh r4, [r5], #2       @ trailing odd pixel
fr_rownext:
    add r1, r1, #1
    sub r3, r3, #1
    b fr_row
fr_done:
    pop {r4-r8, pc}
.ltorg

@ picon: r0=px r1=py r2=icon idx -> plot a 16x16 icon
picon:
    push {r4-r10, lr}
    ldr r3, =icon_data
    add r3, r3, r2, lsl #8     @ icon*256
    ldr r9, =palette
    ldr r4, =FB
    mov r5, r1, lsl #8
    sub r5, r5, r1, lsl #4
    add r5, r5, r0
    add r4, r4, r5, lsl #1
    mov r6, #16
pic_row:
    mov r7, #16
pic_col:
    ldrb r8, [r3], #1
    add r10, r9, r8, lsl #1
    ldrh r10, [r10]
    strh r10, [r4], #2
    subs r7, r7, #1
    bne pic_col
    add r4, r4, #(240*2 - 32)
    subs r6, r6, #1
    bne pic_row
    pop {r4-r10, pc}
.ltorg

@ setfb: r0=fg index, r1=bg index -> store d_fg/d_bg
setfb:
    ldr r2, =d_fg
    str r0, [r2]
    ldr r2, =d_bg
    str r1, [r2]
    bx lr

@ load_palette: copy theme_pals[v_theme] (16 hwords) -> palette
load_palette:
    push {lr}
    ldr r0, =v_theme
    ldr r0, [r0]
    ldr r1, =theme_pals
    add r1, r1, r0, lsl #5     @ theme*32 bytes
    ldr r2, =palette
    mov r3, #16
lp_l:
    ldrh r0, [r1], #2
    strh r0, [r2], #2
    subs r3, r3, #1
    bne lp_l
    pop {pc}
.ltorg

@ clear_screen: fill the whole framebuffer with palette[0] (desktop)
clear_screen:
    push {lr}
    mov r0, #0
    bl set_fg
    mov r0, #0
    mov r1, #0
    mov r2, #240
    mov r3, #160
    bl frect
    pop {pc}
set_fg:
    ldr r1, =d_fg
    str r0, [r1]
    bx lr
.ltorg

@ two_digits: r0 = 0..99 -> r1 = tens char, r0 = units char
two_digits:
    mov r1, #'0'
td_l:
    cmp r0, #10
    blo td_d
    sub r0, r0, #10
    add r1, r1, #1
    b td_l
td_d:
    add r0, r0, #'0'
    bx lr
.ltorg

@ ============================================================================
@ launcher (M1) — 4-column icon grid, selected label inverted
@ ============================================================================
@ icon i: col=i%4 -> x=col*58+4 ; row=i/4 -> y=row*46+18 ; label at y+18
draw_launcher:
    push {r4-r6, lr}
    bl load_palette
    bl clear_screen
    @ title bar: white strip + inverted title
    mov r0, #1
    bl set_fg
    mov r0, #0
    mov r1, #0
    mov r2, #240
    mov r3, #14
    bl frect
    mov r0, #0
    mov r1, #1
    bl setfb                  @ inverted: blue on white
    mov r0, #4
    mov r1, #3
    ldr r2, =s_title
    bl pstr
    @ icon grid
    mov r4, #0                @ index
dl_item:
    mov r0, r4
    bl icon_x                 @ r0 = x
    mov r5, r0
    mov r0, r4
    bl icon_y                 @ r0 = y
    mov r6, r0
    mov r0, r5
    mov r1, r6
    mov r2, r4
    bl picon
    @ label
    mov r0, r4
    bl draw_label_for         @ draws label r4 at its grid slot, normal/inv by v_sel
    add r4, r4, #1
    cmp r4, #NICONS
    bne dl_item
    pop {r4-r6, pc}
.ltorg

@ icon_x: r0=index -> r0 = pixel x of its column
icon_x:
    and r0, r0, #3
    mov r1, #58
    mul r0, r1, r0
    add r0, r0, #4
    bx lr
@ icon_y: r0=index -> r0 = pixel y of its row group
icon_y:
    mov r1, r0, lsr #2        @ row = i/4
    mov r2, #46
    mul r0, r2, r1
    add r0, r0, #18
    bx lr
.ltorg

@ draw_label_for: r0 = icon index -> draw its label (normal, or inverted if selected)
draw_label_for:
    push {r4-r6, lr}
    mov r4, r0
    bl icon_x
    mov r5, r0               @ x
    mov r0, r4
    bl icon_y
    add r6, r0, #18          @ label y = icon y + 18
    @ choose colours
    ldr r0, =v_sel
    ldr r0, [r0]
    cmp r0, r4
    moveq r0, #0             @ selected -> inverted (blue on white)
    moveq r1, #1
    movne r0, #1             @ normal (white on blue)
    movne r1, #0
    bl setfb
    @ label ptr = icon_lbl[r4]
    ldr r1, =icon_lbl
    ldr r2, [r1, r4, lsl #2]
    mov r0, r5
    mov r1, r6
    bl pstr
    pop {r4-r6, pc}
.ltorg

draw_highlight:
    push {lr}
    ldr r0, =v_selp
    ldr r0, [r0]
    bl draw_label_for
    ldr r0, =v_sel
    ldr r0, [r0]
    bl draw_label_for
    pop {pc}
.ltorg

@ ============================================================================
@ input / navigation (M2)
@ ============================================================================
update:
    push {lr}
    ldr r0, =v_inapp
    ldr r0, [r0]
    cmp r0, #0
    bne up_app
    bl nav_input
    pop {pc}
up_app:
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_B
    beq up_disp
    bl enter_launcher
    pop {pc}
up_disp:
    @ table dispatch (the old cmp/bleq chain compared a clobbered r0 after each
    @ helper returned — with 10 real apps that becomes a genuine hazard)
    ldr r0, =v_app
    ldr r0, [r0]
    ldr r1, =update_tab
    ldr r1, [r1, r0, lsl #2]
    mov lr, pc
    bx r1
    pop {pc}
up_none:
    bx lr
.ltorg

nav_input:
    push {lr}
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_A
    beq nav_dir
    @ launch selected
    ldr r0, =v_sel
    ldr r0, [r0]
    ldr r1, =v_app
    str r0, [r1]
    ldr r1, =v_inapp
    mov r2, #1
    str r2, [r1]
    bl enter_app
    pop {pc}
nav_dir:
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_U
    blne sel_up
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_D
    blne sel_down
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_L
    blne sel_left
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_R
    blne sel_right
    pop {pc}
.ltorg

@ grid navigation: L/R +-1 (wrap), U/D +-4 (clamp)
sel_right:
    ldr r2, =v_sel
    ldr r0, [r2]
    ldr r3, =v_selp
    str r0, [r3]
    add r0, r0, #1
    cmp r0, #NICONS
    movhs r0, #0
    str r0, [r2]
    b mark_hl
sel_left:
    ldr r2, =v_sel
    ldr r0, [r2]
    ldr r3, =v_selp
    str r0, [r3]
    cmp r0, #0
    moveq r0, #NICONS
    sub r0, r0, #1
    str r0, [r2]
    b mark_hl
sel_down:
    ldr r2, =v_sel
    ldr r0, [r2]
    add r1, r0, #4
    cmp r1, #NICONS
    bxhs lr                  @ would leave grid -> no move
    ldr r3, =v_selp
    str r0, [r3]
    str r1, [r2]
    b mark_hl
sel_up:
    ldr r2, =v_sel
    ldr r0, [r2]
    cmp r0, #4
    bxlo lr
    ldr r3, =v_selp
    str r0, [r3]
    sub r0, r0, #4
    str r0, [r2]
    b mark_hl
mark_hl:
    ldr r0, =pf_hl
    mov r1, #1
    str r1, [r0]
    bx lr
.ltorg

enter_app:
    push {lr}
    ldr r0, =v_app
    ldr r0, [r0]
    ldr r1, =init_tab
    ldr r1, [r1, r0, lsl #2]
    mov lr, pc
    bx r1
    ldr r0, =v_dirty
    mov r1, #1
    str r1, [r0]
    pop {pc}

enter_launcher:
    push {lr}
    ldr r0, =v_inapp
    mov r1, #0
    str r1, [r0]
    bl music_silence
    ldr r0, =v_dirty
    mov r1, #1
    str r1, [r0]
    pop {pc}
.ltorg

@ ============================================================================
@ render_partials: small framebuffer writes (in vblank)
@ ============================================================================
render_partials:
    push {lr}
    ldr r0, =v_inapp
    ldr r0, [r0]
    cmp r0, #0
    bne rp_app
    ldr r0, =pf_hl
    ldr r1, [r0]
    cmp r1, #0
    popeq {pc}
    bl draw_highlight
    ldr r0, =pf_hl
    mov r1, #0
    str r1, [r0]
    pop {pc}
rp_app:
    ldr r0, =v_app
    ldr r0, [r0]
    ldr r1, =rp_tab
    ldr r1, [r1, r0, lsl #2]
    mov lr, pc
    bx r1
    pop {pc}
rp_clock:
    push {lr}
    ldr r0, =pf_clk
    ldr r1, [r0]
    cmp r1, #0
    popeq {pc}
    bl draw_clock_time
    ldr r0, =pf_clk
    mov r1, #0
    str r1, [r0]
    pop {pc}
rp_music:
    push {lr}
    ldr r0, =pf_score
    ldr r1, [r0]
    cmp r1, #0
    popeq {pc}
    bl draw_music_status
    ldr r0, =pf_score
    mov r1, #0
    str r1, [r0]
    pop {pc}
rp_dostris:
    push {lr}
    ldr r0, =pf_pc
    ldr r1, [r0]
    cmp r1, #0
    popeq {pc}
    bl draw_piece_partial
    ldr r0, =pf_pc
    mov r1, #0
    str r1, [r0]
    pop {pc}
.ltorg

@ full_redraw: whole-screen redraw (launcher or app)
full_redraw:
    push {lr}
    ldr r0, =v_dirty
    mov r1, #0
    str r1, [r0]
    ldr r0, =pf_hl
    str r1, [r0]
    ldr r0, =pf_pc
    str r1, [r0]
    ldr r0, =v_inapp
    ldr r0, [r0]
    cmp r0, #0
    bne fr_app
    bl draw_launcher
    pop {pc}
fr_app:
    bl draw_app
    pop {pc}
.ltorg

    .include "apps.inc.s"
    .include "dostris.inc.s"
    .include "fs.inc.s"
    .include "tracker.inc.s"
    .include "outlast.inc.s"
    .include "pacman.inc.s"
    .include "paint.inc.s"
