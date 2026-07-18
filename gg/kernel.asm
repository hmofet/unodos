; ============================================================================
; UnoDOS / Sega Game Gear (Z80 + 315-5124 VDP) — milestones 1-3.
; ============================================================================
; The FOURTH fresh contract-driven port. The Game Gear is SMS silicon — the same
; Z80 + VDP — so it REUSES the SMS port's hardware bring-up, the 4bpp tile data,
; the PSG audio, and the Dostris algorithm, consuming `gen/z80/` + `[world.gg]`.
; But the GG LCD shows only the CENTRE 160x144 of the 256x192 frame = 20x18 cells,
; which is exactly the Game Boy's panel — so this port wears the GB's `minimal`
; layout (CONTRACT-ARCH §9): a vertical mini-icon LIST launcher, one full-screen
; app at a time, directional nav. Everything draws at a (6,3) cell offset into the
; visible window. The one real hardware delta from the SMS is CRAM: 12-bit colour
; (2 bytes/entry) instead of 6-bit.
;
; M1: boot to the rendered launcher (title bar + mini-icon list).
; M2: read the pad ($DC), a frame-interrupt loop, an Up/Down selection highlight,
;     button 1 launches the selected app full-screen, button 2 returns.
; M3: full-screen apps — SysInfo, live Clock, Notepad, Files, Theme (cycles the
;     CRAM palette), Music (SN76489 PSG), and Dostris (the falling-blocks game).
;
; Contract-owned (Phase 4): the visible tile geometry comes from unogen
; ([world.gg] -> gen/gg/sys_gen.inc); the Z80 surface from gen/z80/.
; ============================================================================

    include "../unodef/gen/z80/unodef.inc"     ; the Contract surface (SYS_*, enums)
    include "../unodef/gen/gg/sys_gen.inc"      ; SCRCOLS / SCRROWS (visible 20x18)

; ---- hardware --------------------------------------------------------------
VDP_DATA    EQU $BE
VDP_CTRL    EQU $BF
PORT_DC     EQU $DC                 ; control pad (active low)
PSG_PORT    EQU $7F
NAME_BASE   EQU $3800               ; tile map base in VRAM
NT_W        EQU 32                  ; physical name-table width
VIS_X       EQU 6                   ; visible-window cell offset (centre of 256x192)
VIS_Y       EQU 3
RAM_TOP     EQU $DFF0

; pad bits (active-high after we invert PORT_DC)
PAD_U       EQU $01
PAD_D       EQU $02
PAD_L       EQU $04
PAD_R       EQU $08
PAD_A       EQU $10                 ; trigger 1 = launch / select
PAD_B       EQU $20                 ; trigger 2 = back

; list launcher geometry (in visible-window cells)
LIST_ROW    EQU 1
LIST_COL    EQU 3

; ---- Dostris geometry ------------------------------------------------------
BW          EQU 10
BH          EQU 12
BORG_COL    EQU 1
BORG_ROW    EQU 2
FALLRATE    EQU 30

; ---- RAM ($C000-$DFFF) -----------------------------------------------------
v_pad       EQU $C000
v_padp      EQU $C001
v_pade      EQU $C002
v_inapp     EQU $C003
v_sel       EQU $C004
v_selp      EQU $C005
v_app       EQU $C006
v_dirty     EQU $C007
v_tick      EQU $C008               ; word
v_frac      EQU $C00A
v_ss        EQU $C00B
v_mm        EQU $C00C
v_hh        EQU $C00D
v_theme     EQU $C00E
m_idx       EQU $C00F
m_timer     EQU $C010
m_play      EQU $C011
a_idx       EQU $C012
a_tmr       EQU $C013
a_pad       EQU $C014
a_gpause    EQU $C015
pf_hl       EQU $C016
pf_clk      EQU $C017
pf_pal      EQU $C018
pf_pc       EQU $C019
pf_score    EQU $C01A
col         EQU $C01B
row         EQU $C01C
g_type      EQU $C01D
g_rot       EQU $C01E
g_px        EQU $C01F
g_py        EQU $C020
g_state     EQU $C021
g_fall      EQU $C022
g_lines     EQU $C023
g_seed      EQU $C024
g_tx        EQU $C025
g_ty        EQU $C026
g_tmp       EQU $C027
g_srot      EQU $C028
g_row       EQU $C029
g_lt        EQU $C02A
g_pt        EQU $C02B
mlo         EQU $C02C
mhi         EQU $C02D
g_oldpx     EQU $C02E
g_oldpy     EQU $C02F
g_oldrot    EQU $C030
g_bx        EQU $C031
g_by        EQU $C032
g_n         EQU $C033
numstr      EQU $C034               ; 4
clk_str     EQU $C038               ; 9
pf_bd       EQU $C041               ; dostris board dirty (lock): board-only repaint
g_board     EQU $C044               ; BW*BH = 120

; ============================================================================
; reset / interrupt vectors
; ============================================================================
    ORG $0000
reset:
    di
    im 1
    jp boot

    DEFS $0038-$, $00
int_handler:                        ; $0038 — frame (VBlank) interrupt
    push af
    in a, (VDP_CTRL)                ; ack: reading the status port clears the flag
    push hl
    ld hl, v_tick
    inc (hl)
    jr nz, .nc
    inc hl
    inc (hl)
.nc:
    pop hl
    pop af
    ei
    reti

    DEFS $0066-$, $00
nmi_handler:                        ; $0066 — pause
    retn

boot:
    ld sp, RAM_TOP
    ; Sega mapper: slots 0/1/2 -> banks 0/1/2
    xor a
    ld ($FFFC), a
    ld ($FFFD), a
    inc a
    ld ($FFFE), a
    inc a
    ld ($FFFF), a
    ; clear 8 KB work RAM
    ld hl, $C000
    ld de, $C001
    ld bc, $1FFF
    ld (hl), 0
    ldir

    call vdp_init
    call hide_sprites               ; the VDP still scans the SAT — terminate it
    xor a
    ld (v_inapp), a
    ld (v_sel), a
    ld (v_selp), a
    ld (v_theme), a
    ld (v_dirty), a

    call load_tiles
    call clear_names
    call draw_launcher              ; display still off
    ld a, $E0
    call set_r1                     ; display ON + frame interrupt
    ei
    ; fall through to main

; ============================================================================
; main loop
; ============================================================================
main:
    halt                            ; wait for the frame interrupt
    call render_partials
    call read_pad
    call clock_advance
    call update
    ld a, (v_dirty)
    or a
    jr z, main
    call full_redraw
    jr main

; ============================================================================
; VDP / palette / VRAM helpers
; ============================================================================
vdp_init:
    ld hl, vdp_regs
    ld b, vdp_regs_end - vdp_regs
    ld c, VDP_CTRL
    otir
    ret
vdp_regs:
    db $04,$80      ; R0: Mode 4
    db $80,$81      ; R1: display OFF
    db $FF,$82      ; R2: name table base = $3800
    db $FF,$83
    db $FF,$84
    db $FF,$85
    db $FB,$86
    db $00,$87      ; R7: border = palette index 0
    db $00,$88      ; R8: hscroll 0
    db $00,$89      ; R9: vscroll 0
    db $FF,$8A      ; R10: line interrupt off
vdp_regs_end:

; set_r1: A = R1 value -> write it (display/frame-int control)
set_r1:
    out (VDP_CTRL), a
    ld a, $81
    out (VDP_CTRL), a
    ret

; load_palette: theme_pals[v_theme] (32 bytes, 12-bit) -> CRAM 0..31
load_palette:
    ld a, (v_theme)
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                      ; v_theme * 32
    ld de, theme_pals
    add hl, de
    xor a
    out (VDP_CTRL), a
    ld a, $C0                       ; CRAM write, address 0
    out (VDP_CTRL), a
    ld b, 32
    ld c, VDP_DATA
    otir
    ret

; load_tiles: tiles_all (NTILES*32 bytes) -> VRAM $0000
load_tiles:
    ld hl, $0000
    call vram_set_addr
    ld hl, tiles_all
    ld de, NTILES*32
.l: ld a, (hl)
    out (VDP_DATA), a
    inc hl
    dec de
    ld a, d
    or e
    jr nz, .l
    ret

; clear_names: the full 32x28 map -> tile 0
clear_names:
    ld hl, NAME_BASE
    call vram_set_addr
    ld de, NT_W*28
.l: xor a
    out (VDP_DATA), a
    out (VDP_DATA), a
    dec de
    ld a, d
    or e
    jr nz, .l
    ret

; hide_sprites: write Y=$D0 to SAT[0] so the VDP stops sprite processing
hide_sprites:
    ld hl, $3F00                    ; sprite attribute table (R5)
    call vram_set_addr
    ld a, $D0
    out (VDP_DATA), a
    ret

; vram_set_addr: HL = VRAM address, set the write pointer
vram_set_addr:
    ld a, l
    out (VDP_CTRL), a
    ld a, h
    or $40
    out (VDP_CTRL), a
    ret

; xy_to_vram: (col),(row) -> VDP write addr at NAME_BASE + ((row+VIS_Y)*32 +
;             (col+VIS_X)) * 2.  Preserves HL and BC.
xy_to_vram:
    push hl
    push bc
    ld a, (row)
    add a, VIS_Y
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                      ; (row+VIS_Y) * 32
    ld a, (col)
    add a, VIS_X
    ld c, a
    ld b, 0
    add hl, bc                      ; + (col+VIS_X)
    add hl, hl                      ; * 2 (2 bytes/entry)
    ld bc, NAME_BASE
    add hl, bc
    ld a, l
    out (VDP_CTRL), a
    ld a, h
    or $40
    out (VDP_CTRL), a
    pop bc
    pop hl
    ret

; wstr: HL = string (NUL-term), (col)/(row) set, B = tile base
wstr:
    call xy_to_vram                 ; preserves HL, B
.l: ld a, (hl)
    or a
    ret z
    sub 32
    add a, b
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
    inc hl
    jr .l

; draw_content: HL = table {db col,row ; dw str}, end col=$FF
draw_content:
.l: ld a, (hl)
    cp $FF
    ret z
    ld (col), a
    inc hl
    ld a, (hl)
    ld (row), a
    inc hl
    ld e, (hl)
    inc hl
    ld d, (hl)
    inc hl
    push hl
    ex de, hl                       ; HL = string
    ld b, T_FONT
    call wstr
    pop hl
    jr .l

; draw_chrome: DE = title string -> title bar + "B=Back" footer
draw_chrome:
    push de
    xor a
    ld (col), a
    ld (row), a
    call xy_to_vram
    ld b, SCRCOLS
.tb:
    ld a, T_WHITE
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
    djnz .tb
    ld a, 1
    ld (col), a
    xor a
    ld (row), a
    pop de
    ex de, hl                       ; HL = title
    ld b, T_FONTINV
    call wstr
    ld a, 1
    ld (col), a
    ld a, SCRROWS-1
    ld (row), a
    ld hl, s_back
    ld b, T_FONT
    jp wstr

; two_digits: A = 0..99 -> D = tens char, A = units char
two_digits:
    ld d, '0'
.t: cp 10
    jr c, .done
    sub 10
    inc d
    jr .t
.done:
    add a, '0'
    ret

; ============================================================================
; draw_launcher: title bar + the mini-icon list (selected label inverted)
; ============================================================================
draw_launcher:
    call load_palette
    ; title bar (visible row 0)
    xor a
    ld (col), a
    ld (row), a
    call xy_to_vram
    ld b, SCRCOLS
.tb:
    ld a, T_WHITE
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
    djnz .tb
    ld a, 1
    ld (col), a
    xor a
    ld (row), a
    ld hl, s_title
    ld b, T_FONTINV
    call wstr
    ; list items
    ld c, 0
.item:
    ld a, c
    add a, LIST_ROW
    ld (row), a
    ld a, 1
    ld (col), a
    push bc
    call xy_to_vram
    pop bc
    ld a, c
    add a, T_MINI
    out (VDP_DATA), a               ; mini-icon tile
    xor a
    out (VDP_DATA), a
    ld a, LIST_COL
    ld (col), a
    ld a, c
    add a, a
    ld e, a
    ld d, 0
    ld hl, icon_lbl
    add hl, de
    ld a, (hl)
    ld e, a
    inc hl
    ld a, (hl)
    ld d, a
    ex de, hl                       ; HL = label
    ld a, (v_sel)
    cp c
    ld b, T_FONT
    jr nz, .nf
    ld b, T_FONTINV
.nf:
    push bc
    call wstr
    pop bc
    inc c
    ld a, c
    cp NICONS
    jr nz, .item
    ret

; draw_label: C = item index, B = base -> redraw that list label
draw_label:
    ld a, c
    add a, LIST_ROW
    ld (row), a
    ld a, LIST_COL
    ld (col), a
    ld a, c
    add a, a
    ld e, a
    ld d, 0
    ld hl, icon_lbl
    add hl, de
    ld a, (hl)
    ld e, a
    inc hl
    ld a, (hl)
    ld d, a
    ex de, hl
    jp wstr

draw_highlight:
    ld a, (v_selp)
    ld c, a
    ld b, T_FONT
    call draw_label
    ld a, (v_sel)
    ld c, a
    ld b, T_FONTINV
    jp draw_label

; ============================================================================
; input (M2)
; ============================================================================
read_pad:
  IFDEF AUTOTEST
    jp auto_input
  ELSE
    ld a, (v_pad)
    ld (v_padp), a
    in a, (PORT_DC)
    cpl                             ; active-low -> active-high
    and $3F
    ld (v_pad), a
    ld b, a
    ld a, (v_padp)
    cpl
    and b
    ld (v_pade), a
    ret
  ENDIF

; ============================================================================
; update: per-frame logic
; ============================================================================
update:
    ld a, (v_inapp)
    or a
    jr nz, up_app
    jp nav_input
up_app:
    ld a, (v_pade)
    and PAD_B
    jr z, up_disp
    jp enter_launcher
up_disp:
    ld a, (v_app)
    cp 3
    jr nz, .n3
    jp music_tick
.n3:
    cp 5
    jr nz, .n5
    jp theme_input
.n5:
    cp 7
    jr nz, .n7
    jp dostris_update
.n7:
    ret

nav_input:
    ld a, (v_pade)
    and PAD_A
    jr z, .dir
    ld a, (v_sel)
    ld (v_app), a
    ld a, 1
    ld (v_inapp), a
    jp enter_app
.dir:
    ld a, (v_pade)
    and PAD_U
    jr z, .down
    call sel_up
.down:
    ld a, (v_pade)
    and PAD_D
    jr z, .done
    call sel_down
.done:
    ret

sel_up:
    ld a, (v_sel)
    ld (v_selp), a
    or a
    jr nz, .dec
    ld a, NICONS
.dec:
    dec a
    ld (v_sel), a
    jr mark_hl
sel_down:
    ld a, (v_sel)
    ld (v_selp), a
    inc a
    cp NICONS
    jr c, .s
    xor a
.s:
    ld (v_sel), a
mark_hl:
    ld a, 1
    ld (pf_hl), a
    ret

enter_app:
    ld a, (v_app)
    cp 7
    jr nz, .nm
    call dostris_init
.nm:
    ld a, (v_app)
    cp 3
    jr nz, .nd
    call music_init
.nd:
    ld a, 1
    ld (v_dirty), a
    ret

enter_launcher:
    xor a
    ld (v_inapp), a
    call music_silence
    ld a, 1
    ld (v_dirty), a
    ret

; ============================================================================
; render_partials: small frame-time tile writes
; ============================================================================
render_partials:
    ld a, (v_inapp)
    or a
    jr nz, rp_app
    ld a, (pf_hl)
    or a
    ret z
    call draw_highlight
    xor a
    ld (pf_hl), a
    ret
rp_app:
    ld a, (v_app)
    cp 1
    jr nz, .n1
    jp rp_clock
.n1:
    cp 3
    jr nz, .n3
    jp rp_music
.n3:
    cp 5
    jr nz, .n5
    jp rp_theme
.n5:
    cp 7
    jr nz, .n7
    jp rp_dostris
.n7:
    ret
rp_clock:
    ld a, (pf_clk)
    or a
    ret z
    call draw_clock_time
    xor a
    ld (pf_clk), a
    ret
rp_music:
    ld a, (pf_score)
    or a
    ret z
    call draw_music_status
    xor a
    ld (pf_score), a
    ret
rp_theme:
    ld a, (pf_pal)
    or a
    ret z
    call draw_theme_status
    xor a
    ld (pf_pal), a
    ret
rp_dostris:
    ld a, (pf_bd)
    or a
    jr nz, .board
    ld a, (pf_pc)
    or a
    ret z
    call draw_piece_partial
    xor a
    ld (pf_pc), a
    ret
.board:
    call dostris_draw
    xor a
    ld (pf_bd), a
    ld (pf_pc), a
    ret

; ============================================================================
; full_redraw: display-off whole-screen redraw
; ============================================================================
full_redraw:
    xor a
    ld (v_dirty), a
    ld (pf_hl), a
    ld (pf_pc), a
    ld (pf_bd), a
    ld a, $A0                       ; display OFF (keep frame interrupt)
    call set_r1
    call clear_names
    ld a, (v_inapp)
    or a
    jr nz, .app
    call draw_launcher
    jr .on
.app:
    call draw_app
.on:
    ld a, $E0                       ; display ON
    call set_r1
    ret

; ---- includes --------------------------------------------------------------
    include "apps.inc"              ; app draws, clock/theme/music, AUTOTEST + strings
    include "dostris.inc"           ; the Dostris game
    include "gen_data.inc"          ; tiles_all, palettes, T_*, piece + music tables

; ---- cartridge header ------------------------------------------------------
    DEFS $7FF0-$, $00
    db "TMR SEGA"
    db $00,$00
    db $00,$00
    db $00,$00,$00
    db $5C                          ; region = Game Gear export, size 32KB
    DEFS $8000-$, $00
