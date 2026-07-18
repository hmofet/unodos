; ============================================================================
; UnoDOS / Nintendo Game Boy + Game Boy Color (Sharp SM83) — milestones 1-3.
; ============================================================================
; A bare-metal UnoDOS for the Game Boy. The THIRD fresh contract-driven port
; (after SMS and NES) and the FIRST on the Sharp SM83 (`gbz80`) — a genuinely
; new unogen dialect (rgbds). Like the NES it is the Contract's MINIMAL profile
; (CONTRACT-ARCH §9): 8 KB RAM, no mouse, NO window manager — one full-screen
; app at a time with directional navigation (§8). The launcher is a VERTICAL
; LIST (the small 160x144 LCD suits a list better than a grid) — a deliberate
; demonstration that the same Contract scales to a different layout.
;
; M1: boot to a rendered full-screen launcher (title bar + a mini-icon list).
; M2: read the joypad ($FF00), a vblank-interrupt per-frame loop, a directional
;     SELECTION highlight (Up/Down move it, the selected label inverts), A
;     launches the selected app full-screen, B returns to the launcher.
; M3: full-screen apps — SysInfo, live Clock, Notepad, Files, Theme (palette),
;     Music (the GB APU pulse channel), and Dostris (the falling-blocks game).
;
; Display model (mirrors the NES `minimal` floor): the VBlank ISR is minimal (a
; 60 Hz tick), the main loop syncs on `halt` and does SMALL partial tile-map
; writes during vblank (flicker-free); big changes redraw with the LCD off.
; Colour: one ROM runs on both — DMG sets BGP (4 greys), GBC writes a real BG
; palette (the UnoDOS blue/white/cyan/magenta theme), detected at boot (A=$11).
;
; Contract-owned (CONTRACT-ARCH Phase 4): the tile screen geometry comes from
; unogen ([world.gb] -> gen/gb/sys_gen.inc); the call surface from gen/gbz80/.
; ============================================================================

INCLUDE "build/cfg.inc"                    ; AUTOTEST / AT_* build switches
INCLUDE "../unodef/gen/gbz80/unodef.inc"   ; the Contract surface (SYS_*, enums)
INCLUDE "../unodef/gen/gb/sys_gen.inc"     ; SCRCOLS / SCRROWS
INCLUDE "gb_equ.inc"                        ; T_*/NICONS/NTILES/NTHEMES (rgbds needs EQUs first)

; ---- hardware registers ----------------------------------------------------
DEF rP1     EQU $FF00
DEF rIF     EQU $FF0F
DEF rNR10   EQU $FF10
DEF rNR11   EQU $FF11
DEF rNR12   EQU $FF12
DEF rNR13   EQU $FF13
DEF rNR14   EQU $FF14
DEF rNR50   EQU $FF24
DEF rNR51   EQU $FF25
DEF rNR52   EQU $FF26
DEF rLCDC   EQU $FF40
DEF rSCY    EQU $FF42
DEF rSCX    EQU $FF43
DEF rLY     EQU $FF44
DEF rBGP    EQU $FF47
DEF rBCPS   EQU $FF68
DEF rBCPD   EQU $FF69
DEF rIE     EQU $FFFF

DEF MAP     EQU $9800                       ; BG tile map base
DEF TILES   EQU $8000                       ; BG tile data base

; joypad bits (after read_pad: 1 = pressed)
DEF PAD_A   EQU $01
DEF PAD_B   EQU $02
DEF PAD_SEL EQU $04
DEF PAD_ST  EQU $08
DEF PAD_R   EQU $10
DEF PAD_L   EQU $20
DEF PAD_U   EQU $40
DEF PAD_D   EQU $80

; ---- Dostris geometry ------------------------------------------------------
DEF BW       EQU 10                          ; board width (cells)
DEF BH       EQU 12                          ; board height (cells)
DEF BORG_COL EQU 1                           ; board origin (fits the visible region)
DEF BORG_ROW EQU 2
DEF FALLRATE EQU 30                          ; frames per gravity step (~0.5 s)

; list launcher geometry
DEF LIST_ROW EQU 1                           ; first item row
DEF LIST_COL EQU 3                           ; label column (mini-icon at col 1)

; ============================================================================
; WRAM variables
; ============================================================================
SECTION "vars", WRAM0[$C000]
boot_a:    ds 1
is_cgb:    ds 1
v_pad:     ds 1
v_padp:    ds 1
v_pade:    ds 1
v_inapp:   ds 1
v_sel:     ds 1
v_selp:    ds 1
v_app:     ds 1
v_dirty:   ds 1
v_tick:    ds 2
v_frac:    ds 1
v_ss:      ds 1
v_mm:      ds 1
v_hh:      ds 1
v_theme:   ds 1
m_idx:     ds 1
m_timer:   ds 1
m_play:    ds 1
a_idx:     ds 1
a_tmr:     ds 1
a_pad:     ds 1
a_gpause:  ds 1
pf_hl:     ds 1
pf_clk:    ds 1
pf_pal:    ds 1
pf_pc:     ds 1
pf_score:  ds 1
pf_bd:     ds 1                                 ; dostris board dirty (lock): board-only repaint
col:       ds 1
row:       ds 1
; Dostris game state
g_type:    ds 1
g_rot:     ds 1
g_px:      ds 1
g_py:      ds 1
g_state:   ds 1
g_fall:    ds 1
g_lines:   ds 1
g_seed:    ds 1
g_tx:      ds 1
g_ty:      ds 1
g_tmp:     ds 1
g_srot:    ds 1
g_row:     ds 1
g_lt:      ds 1
g_pt:      ds 1
mlo:       ds 1
mhi:       ds 1
g_oldpx:   ds 1
g_oldpy:   ds 1
g_oldrot:  ds 1
g_bx:      ds 1
g_by:      ds 1
g_n:       ds 1
numstr:    ds 4
clk_str:   ds 9
g_board:   ds BW*BH

; ============================================================================
; interrupt vector + entry
; ============================================================================
SECTION "VBlank", ROM0[$40]
vblank_isr:
    ; minimal ISR: a 60 Hz tick + (nothing else — never touches VRAM).
    push af
    push hl
    ld hl, v_tick
    inc [hl]
    jr nz, .nc
    inc hl
    inc [hl]
.nc:
    pop hl
    pop af
    reti

SECTION "Entry", ROM0[$100]
    nop
    jp Start
    ds $150 - $104, $00                     ; header (rgbfix fills logo + checksums)

SECTION "Main", ROM0[$150]
Start:
    ld b, a                                 ; save the boot A (=$11 on GBC)
    di
    ld sp, $FFFE
    ; clear WRAM $C000-$DFFF
    ld hl, $C000
    ld de, $2000
.clr:
    xor a
    ld [hl+], a
    dec de
    ld a, d
    or e
    jr nz, .clr
    ; CGB?
    ld a, b
    cp $11
    jr nz, .dmg
    ld a, 1
    ld [is_cgb], a
.dmg:
    call lcd_off
    ; initial OS state
    xor a
    ld [v_inapp], a
    ld [v_sel], a
    ld [v_selp], a
    ld [v_theme], a
    ld [v_dirty], a
    call load_tiles
    call clear_map
    call draw_launcher                      ; LCD still off
    call lcd_on
    ; enable the VBlank interrupt only
    ld a, 1
    ld [rIE], a
    xor a
    ld [rIF], a
    ei
    ; fall through to main

; ============================================================================
; main loop — halt to vblank, flush small partials, run a frame of logic
; ============================================================================
main:
    halt
    nop                                     ; halt-bug guard
    call render_partials                    ; SMALL tile-map writes (in vblank)
    call read_pad
    call clock_advance                      ; the clock runs whether or not its app is open
    call update
    ld a, [v_dirty]
    and a
    jr nz, .full
    ld a, [pf_bd]                           ; dostris board-only change (a lock)?
    and a
    jr z, main
    call board_redraw                       ; LCD-off board repaint, no clear_map
    jr main
.full:
    call full_redraw                        ; LCD-off whole-screen redraw
    jr main

; ============================================================================
; LCD / palette / VRAM helpers
; ============================================================================
lcd_off:
.w: ld a, [rLY]                             ; wait for vblank before disabling
    cp 144
    jr c, .w
    xor a
    ld [rLCDC], a
    ret

lcd_on:
    ld a, $91                               ; LCD on, BG tiles $8000, BG on
    ld [rLCDC], a
    ret

; setup_palettes: BGP (DMG) + the GBC BG palette 0 (if a Color) ---------------
setup_palettes:
    xor a
    ld [rSCX], a
    ld [rSCY], a
    ; DMG BGP = theme_bgp[v_theme]
    ld a, [v_theme]
    ld e, a
    ld d, 0
    ld hl, theme_bgp
    add hl, de
    ld a, [hl]
    ld [rBGP], a
    ; GBC: BG palette 0 = theme_pals[v_theme] (8 bytes)
    ld a, [is_cgb]
    and a
    ret z
    ld a, [v_theme]
    add a, a
    add a, a
    add a, a                                 ; theme*8
    ld e, a
    ld d, 0
    ld hl, theme_pals
    add hl, de
    ld a, $80                               ; BCPS: index 0, auto-increment
    ld [rBCPS], a
    ld b, 8                                 ; 4 colours * 2 bytes
.l: ld a, [hl+]
    ld [rBCPD], a
    dec b
    jr nz, .l
    ret

; load_tiles: copy NTILES*16 bytes -> $8000 (LCD off) ------------------------
load_tiles:
    ld de, tiles_data
    ld hl, TILES
    ld bc, NTILES*16
.l: ld a, [de]
    ld [hl+], a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .l
    ret

; clear_map: fill the 32x32 map with tile 0 ----------------------------------
clear_map:
    ld hl, MAP
    ld bc, 32*32
.l: xor a
    ld [hl+], a
    dec bc
    ld a, b
    or c
    jr nz, .l
    ret

; xy_to_hl: col,row -> HL = MAP + row*32 + col (preserves DE) -----------------
xy_to_hl:
    ld a, [row]
    ld h, 0
    ld l, a
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                              ; row*32
    ld a, [col]
    add a, l
    ld l, a
    ld a, h
    adc a, 0
    add a, $98                              ; + MAP high byte
    ld h, a
    ret

; wstr: DE = string (NUL-term), HL = dst, B = tile base ---------------------
wstr:
.l: ld a, [de]
    and a
    ret z
    sub 32
    add a, b
    ld [hl+], a
    inc de
    jr .l

; draw_content: HL = table {db col,row ; dw str}, end col=$FF ----------------
draw_content:
.l: ld a, [hl+]
    cp $FF
    ret z
    ld [col], a
    ld a, [hl+]
    ld [row], a
    ld a, [hl+]
    ld e, a
    ld a, [hl+]
    ld d, a                                 ; DE = string
    push hl
    call xy_to_hl
    ld b, T_FONT
    call wstr
    pop hl
    jr .l

; fill_row0: fill the title row with T_WHITE, then wstr the title -----------
; DE = title string
draw_chrome:
    push de
    ld hl, MAP                              ; row 0
    ld b, SCRCOLS
    ld a, T_WHITE
.tb:
    ld [hl+], a
    dec b
    jr nz, .tb
    ; title at (1,0) inverted
    pop de
    ld hl, MAP+1
    ld b, T_FONTINV
    call wstr
    ; footer hint at the bottom row
    ld a, 1
    ld [col], a
    ld a, SCRROWS-1
    ld [row], a
    call xy_to_hl
    ld de, s_back
    ld b, T_FONT
    jp wstr

; two_digits: A = 0..99 -> D = tens char, A = units char ---------------------
two_digits:
    ld d, $30
.t: cp 10
    jr c, .done
    sub 10
    inc d
    jr .t
.done:
    add a, $30
    ret

; ============================================================================
; draw_launcher: title bar + the mini-icon list (selected label inverted)
; ============================================================================
draw_launcher:
    call setup_palettes
    call clear_map
    ; title bar
    ld hl, MAP
    ld b, SCRCOLS
    ld a, T_WHITE
.tb:
    ld [hl+], a
    dec b
    jr nz, .tb
    ld de, s_title
    ld hl, MAP+1
    ld b, T_FONTINV
    call wstr
    ; list items
    ld c, 0
.item:
    ; row = LIST_ROW + c
    ld a, c
    add a, LIST_ROW
    ld [row], a
    ; mini-icon at col 1
    ld a, 1
    ld [col], a
    call xy_to_hl
    ld a, c
    add a, T_MINI
    ld [hl], a
    ; label at LIST_COL
    ld a, LIST_COL
    ld [col], a
    ld a, c
    add a, a
    ld e, a
    ld d, 0
    ld hl, icon_lbl
    add hl, de
    ld a, [hl+]
    ld e, a
    ld a, [hl]
    ld d, a                                 ; DE = label
    call xy_to_hl
    ; base = T_FONTINV if selected else T_FONT
    ld b, T_FONT
    ld a, [v_sel]
    cp c
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

; draw_label: C = item index, B = base -> redraw that list label -------------
draw_label:
    ld a, c
    add a, LIST_ROW
    ld [row], a
    ld a, LIST_COL
    ld [col], a
    ld a, c
    add a, a
    ld e, a
    ld d, 0
    ld hl, icon_lbl
    add hl, de
    ld a, [hl+]
    ld e, a
    ld a, [hl]
    ld d, a
    call xy_to_hl
    jp wstr

draw_highlight:
    ld a, [v_selp]
    ld c, a
    ld b, T_FONT
    call draw_label
    ld a, [v_sel]
    ld c, a
    ld b, T_FONTINV
    jp draw_label

; ============================================================================
; input (M2)
; ============================================================================
read_pad:
  IF AUTOTEST
    jp auto_input
  ELSE
    ld a, [v_pad]
    ld [v_padp], a
    ld a, $20                               ; select d-pad (P14=0)
    ld [rP1], a
    ld a, [rP1]
    ld a, [rP1]
    and $0F
    swap a
    ld c, a                                 ; d-pad in the high nibble
    ld a, $10                               ; select buttons (P15=0)
    ld [rP1], a
    ld a, [rP1]
    ld a, [rP1]
    ld a, [rP1]
    ld a, [rP1]
    and $0F
    or c                                    ; buttons low nibble + d-pad high
    cpl                                     ; 1 = pressed
    ld [v_pad], a
    ld a, $30
    ld [rP1], a
    ld a, [v_padp]
    cpl
    ld c, a
    ld a, [v_pad]
    and c
    ld [v_pade], a
    ret
  ENDC

; ============================================================================
; update: per-frame logic
; ============================================================================
update:
    ld a, [v_inapp]
    and a
    jr nz, up_app
    jp nav_input
up_app:
    ld a, [v_pade]
    and PAD_B
    jr z, up_disp
    jp enter_launcher
up_disp:
    ld a, [v_app]
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
    ld a, [v_pade]
    and PAD_A
    jr z, .dir
    ld a, [v_sel]
    ld [v_app], a
    ld a, 1
    ld [v_inapp], a
    jp enter_app
.dir:
    ld a, [v_pade]
    and PAD_U
    jr z, .down
    call sel_up
.down:
    ld a, [v_pade]
    and PAD_D
    jr z, .done
    call sel_down
.done:
    ret

sel_up:
    ld a, [v_sel]
    ld [v_selp], a
    and a
    jr nz, .dec
    ld a, NICONS
.dec:
    dec a
    ld [v_sel], a
    jr mark_hl
sel_down:
    ld a, [v_sel]
    ld [v_selp], a
    inc a
    cp NICONS
    jr c, .s
    xor a
.s:
    ld [v_sel], a
mark_hl:
    ld a, 1
    ld [pf_hl], a
    ret

enter_app:
    ld a, [v_app]
    cp 7
    jr nz, .nm
    call dostris_init
.nm:
    ld a, [v_app]
    cp 3
    jr nz, .nd
    call music_init
.nd:
    ld a, 1
    ld [v_dirty], a
    ret

enter_launcher:
    xor a
    ld [v_inapp], a
    call music_silence
    ld a, 1
    ld [v_dirty], a
    ret

; ============================================================================
; render_partials: small vblank-time tile writes
; ============================================================================
render_partials:
    ld a, [v_inapp]
    and a
    jr nz, rp_app
    ld a, [pf_hl]
    and a
    ret z
    call draw_highlight
    xor a
    ld [pf_hl], a
    ret
rp_app:
    ld a, [v_app]
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
    ld a, [pf_clk]
    and a
    ret z
    call draw_clock_time
    xor a
    ld [pf_clk], a
    ret
rp_music:
    ld a, [pf_score]
    and a
    ret z
    call draw_music_status
    xor a
    ld [pf_score], a
    ret
rp_theme:
    ld a, [pf_pal]
    and a
    ret z
    call draw_theme_status
    xor a
    ld [pf_pal], a
    ret
rp_dostris:
    ld a, [pf_pc]
    and a
    ret z
    call draw_piece_partial
    xor a
    ld [pf_pc], a
    ret

; ============================================================================
; full_redraw: LCD-off whole-screen redraw (launcher or app)
; ============================================================================
full_redraw:
    xor a
    ld [v_dirty], a
    ld [pf_hl], a
    ld [pf_pc], a
    ld [pf_bd], a
    call lcd_off
    call clear_map
    ld a, [v_inapp]
    and a
    jr nz, .app
    call draw_launcher
    jr .on
.app:
    call draw_app
.on:
    call lcd_on
    ret

; board_redraw: a Dostris lock only changed the board region. Repaint the board
; (walls+floor+board+piece+score from g_board) with the LCD off - but SKIP the
; 1024-entry clear_map and the chrome/desktop redraw that full_redraw does, since
; only the board changed. dostris_draw is too large to fit one vblank, so the LCD
; is briefly disabled (shorter blank than a full_redraw); avoids clear_map's
; 1024 writes + the app-content chrome pass every landing.
board_redraw:
    xor a
    ld [pf_bd], a
    ld [pf_pc], a
    call lcd_off
    call dostris_draw
    call lcd_on
    ret

; ---- includes --------------------------------------------------------------
INCLUDE "apps.inc"                          ; app draws, clock/theme/music, AUTOTEST + strings
INCLUDE "dostris.inc"                       ; the Dostris game

SECTION "tiles", ROM0
tiles_data:
    INCBIN "build/tiles.bin"
INCLUDE "gb_data.inc"                       ; T_*/NICONS/palette + piece + music tables
