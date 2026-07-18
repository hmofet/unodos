; ============================================================================
; UnoDOS / NEC PC Engine (HuC6280 + HuC6270 VDC) — milestones 1-3.
; ============================================================================
; The SIXTH fresh contract-driven port and the FIRST on the HuC6280 (a 65C02
; superset; ca65 --cpu huc6280). The PC Engine screen is 256x224 = 32x28 BAT
; cells — like the NES nametable — so this reuses the NES's 4-column grid
; launcher and 6502 app/Dostris logic, swapping the draw layer to the VDC.
; MINIMAL profile (CONTRACT-ARCH §9): one full-screen app, directional nav.
;
; Memory: MPR0=$F8 (8KB RAM at $0000 -> zero page + stack + work), MPR1=$FF
; (hardware I/O at $2000 -> VDC $2000, VCE $2400, PSG $2800, joypad $3000),
; MPR7=$00 (ROM bank 0 at $E000 -> entry + vectors), MPR2-6 = ROM banks 1-5.
; The VDC BAT is at VRAM $0000; tiles are uploaded to VRAM $1000 (CG index $100),
; so a BAT entry for tile N is ($01<<8)|N  (palette 0). One 16-colour VCE palette.
;
; Contract-owned (Phase 4): the cell geometry comes from unogen
; ([world.pce] -> gen/pce/sys_gen.inc).
; ============================================================================

.setcpu "huc6280"
.include "../unodef/gen/pce/sys_gen.inc"       ; SCRCOLS / SCRROWS (the genuine Contract overlap)

VDC_AR   = $2000          ; VDC address/status
VDC_DL   = $2002          ; VDC data low
VDC_DH   = $2003          ; VDC data high
VCE_CTRL = $2400          ; VCE control: dot clock + B/W (must be set on real HW)
VCE_CTA  = $2402          ; VCE colour-table address
VCE_CTW  = $2404          ; VCE colour-table data
PSG_AR   = $2800
PSG_FL   = $2802
PSG_FH   = $2803
PSG_CR   = $2804
PSG_BAL  = $2805
PSG_WAV  = $2806
PSG_MAIN = $2801
JOY      = $3000

; joypad bits (active-high after we invert): d-pad + buttons
PAD_U    = $10
PAD_D    = $40
PAD_L    = $80
PAD_R    = $20
PAD_A    = $01           ; button I = launch / select
PAD_B    = $02           ; button II = back

BW       = 10
BH       = 12
BORG_COL = 11
BORG_ROW = 6
FALLRATE = 30

; ---- zero page ----
ptr      = $00
ptr2     = $02
col      = $04
row      = $05
tmp      = $06
addrlo   = $08
addrhi   = $09
tile     = $0A
idx      = $0B
grp      = $0C
v_pad    = $10
v_padp   = $11
v_pade   = $12
v_inapp  = $13
v_sel    = $14
v_selp   = $15
v_app    = $16
v_dirty  = $17
v_frac   = $18
v_ss     = $19
v_mm     = $1A
v_hh     = $1B
v_theme  = $1C
m_idx    = $1D
m_timer  = $1E
m_play   = $1F
a_idx    = $20
a_tmr    = $21
a_pad    = $22
a_gpause = $23
pf_hl    = $24
pf_clk   = $25
pf_pal   = $26
pf_pc    = $27
pf_score = $28
pf_bd    = $29          ; dostris board dirty (lock/lineclear): board-only repaint
g_type   = $30
g_rot    = $31
g_px     = $32
g_py     = $33
g_state  = $34
g_fall   = $35
g_lines  = $36
g_seed   = $37
g_tx     = $39
g_ty     = $3A
g_tmp    = $3B
g_srot   = $3C
g_row    = $3D
g_lt     = $3E
g_pt     = $3F
mlo      = $40
mhi      = $41
g_oldpx  = $42
g_oldpy  = $43
g_oldrot = $44
g_bx     = $45
g_by     = $46
g_n      = $47
numstr   = $50           ; 4
clk_str  = $54           ; 9
g_board  = $0400         ; BW*BH = 120 (work RAM)

; ============================================================================
; boot — must live in bank 0 ($E000), the only bank mapped at reset
; ============================================================================
.segment "STARTUP"
start:
    sei
    csh
    cld
    lda #$F8
    tam #$01                ; MPR0 = RAM
    lda #$FF
    tam #$02                ; MPR1 = I/O ($2000)
    lda #$01
    tam #$04                ; MPR2 = ROM bank 1 ($4000)
    lda #$02
    tam #$08                ; MPR3 = ROM bank 2 ($6000)
    lda #$03
    tam #$10                ; MPR4 = ROM bank 3 ($8000)
    lda #$04
    tam #$20                ; MPR5 = ROM bank 4 ($A000)
    lda #$05
    tam #$40                ; MPR6 = ROM bank 5 ($C000)
    ldx #$FF
    txs
    jmp boot_main           ; into the main bank ($4000+, now mapped)

.segment "CODE"
boot_main:
    jsr vdc_init
    jsr load_palette
    jsr load_tiles
    jsr clear_bat
    lda #0
    sta v_inapp
    sta v_sel
    sta v_selp
    sta v_theme
    sta v_dirty
    jsr draw_launcher
    jsr display_on
main:
    jsr wait_vbl
    jsr render_partials
    jsr read_pad
    jsr clock_advance
    jsr update
    lda v_dirty
    beq main
    jsr full_redraw
    bra main

; ============================================================================
; VDC / VCE helpers
; ============================================================================
vdc_init:
    ; VCE control: 5.37 MHz dot clock (256-wide), colour. A HuCard booted via a
    ; flash cart has NO System Card to set this up, so the VCE state is undefined at
    ; reset — without this write the display is black on real PC Engine hardware
    ; (the harness models the palette but not the VCE control register, so it passed
    ; in emulation). Matches the VDC's 256-wide HDR ($1F => 32 tiles).
    stz VCE_CTRL
    ldx #0
@l: lda vdc_regs,x
    sta VDC_AR
    inx
    lda vdc_regs,x
    sta VDC_DL
    inx
    lda vdc_regs,x
    sta VDC_DH
    inx
    cpx #(vdc_regs_end - vdc_regs)
    bne @l
    rts
vdc_regs:
    .byte 5,  $00,$00       ; CR off
    .byte 9,  $00,$00       ; MWR 32x32
    .byte $0A,$02,$02       ; HSR
    .byte $0B,$1F,$03       ; HDR (256 wide)
    .byte $0C,$02,$0F       ; VPR
    .byte $0D,$DF,$00       ; VDW (224)
    .byte $0E,$03,$00       ; VCR
    .byte 7,  $00,$00       ; BXR
    .byte 8,  $00,$00       ; BYR
vdc_regs_end:

display_on:
    lda #5
    sta VDC_AR
    lda #$80                ; CR: background enable
    sta VDC_DL
    lda #$00
    sta VDC_DH
    rts
display_off:
    lda #5
    sta VDC_AR
    lda #$00
    sta VDC_DL
    sta VDC_DH
    rts

; wait_vbl: poll the VDC status vblank bit
wait_vbl:
@w: lda VDC_AR              ; read status
    and #$20
    beq @w
    rts

; load_palette: theme_pals[v_theme] (16 words) -> VCE palette 0
load_palette:
    lda v_theme
    asl
    asl
    asl
    asl
    asl                     ; theme*32
    clc
    adc #<theme_pals
    sta ptr
    lda #>theme_pals
    adc #0
    sta ptr+1
    lda #0
    sta VCE_CTA
    sta VCE_CTA+1           ; CTA = 0
    ldy #0
@l: lda (ptr),y
    sta VCE_CTW
    iny
    lda (ptr),y
    sta VCE_CTW+1
    iny
    cpy #32
    bne @l
    rts

; load_tiles: tiles_all (NTILES*32 bytes) -> VRAM word $1000
load_tiles:
    lda #<tiles_all
    sta ptr
    lda #>tiles_all
    sta ptr+1
    lda #0
    sta VDC_AR             ; MAWR
    lda #$00
    sta VDC_DL
    lda #$10
    sta VDC_DH             ; MAWR = $1000
    lda #2
    sta VDC_AR            ; select VWR
    ; NTILES*32 bytes = NTILES*16 words. Loop a 16-bit counter.
    lda #<(NTILES*16)
    sta g_n
    lda #>(NTILES*16)
    sta g_tmp
    ldy #0
@l: lda (ptr),y
    sta VDC_DL
    iny
    lda (ptr),y
    sta VDC_DH
    iny
    bne @nc
    inc ptr+1
@nc:
    ; dec 16-bit word count
    lda g_n
    bne @d
    dec g_tmp
@d: dec g_n
    lda g_n
    ora g_tmp
    bne @l
    rts

; clear_bat: fill the 32x32 BAT with tile 0 (blank)
clear_bat:
    lda #0
    sta VDC_AR
    lda #$00
    sta VDC_DL
    sta VDC_DH            ; MAWR = $0000
    lda #2
    sta VDC_AR
    ldx #0
    ldy #4                ; 4*256 = 1024 entries
@l: lda #$00
    sta VDC_DL
    lda #$01
    sta VDC_DH            ; entry = $0100 (tile 0, palette 0)
    inx
    bne @l
    dey
    bne @l
    rts

; vdc_cell: MAWR = row*32+col, select VWR (uses col,row)
vdc_cell:
    lda row
    sta addrlo
    lda #0
    sta addrhi
    asl addrlo
    rol addrhi
    asl addrlo
    rol addrhi
    asl addrlo
    rol addrhi
    asl addrlo
    rol addrhi
    asl addrlo
    rol addrhi            ; row*32
    lda addrlo
    clc
    adc col
    sta addrlo
    lda addrhi
    adc #0
    sta addrhi
    lda #0
    sta VDC_AR
    lda addrlo
    sta VDC_DL
    lda addrhi
    sta VDC_DH            ; MAWR
    lda #2
    sta VDC_AR           ; VWR
    rts

; putcell: A = tile number -> write BAT entry (palette 0), MAWR auto-increments
putcell:
    sta VDC_DL
    lda #$01
    sta VDC_DH
    rts

; puts: ptr=string, col/row, tmp=tile base
puts:
    jsr vdc_cell
    ldy #0
@l: lda (ptr),y
    beq @done
    sec
    sbc #32
    clc
    adc tmp
    jsr putcell
    iny
    bne @l
@done:
    rts

; cputs: ptr/col/row set; normal font
cputs:
    lda #T_FONT
    sta tmp
    jmp puts

; draw_content: ptr2 -> {db col,row ; dw str}, end col=$FF
draw_content:
    ldy #0
@l: lda (ptr2),y
    cmp #$FF
    beq @done
    sta col
    iny
    lda (ptr2),y
    sta row
    iny
    lda (ptr2),y
    sta ptr
    iny
    lda (ptr2),y
    sta ptr+1
    iny
    phy
    jsr cputs
    ply
    bra @l
@done:
    rts

; draw_icon: A=icon index, col/row -> 2x2 tiles
draw_icon:
    asl
    asl
    clc
    adc #T_ICONS
    sta tile
    jsr vdc_cell
    lda tile
    jsr putcell
    lda tile
    clc
    adc #1
    jsr putcell
    inc row
    jsr vdc_cell
    lda tile
    clc
    adc #2
    jsr putcell
    lda tile
    clc
    adc #3
    jsr putcell
    dec row
    rts

; label_pos: A=icon index -> ptr=label, col, row(label row)
label_pos:
    sta idx
    and #3
    asl
    asl
    asl
    clc
    adc #1
    sta col
    lda idx
    lsr
    lsr
    sta grp
    asl
    asl
    clc
    adc grp
    adc grp
    adc grp
    clc
    adc #3
    clc
    adc #2
    sta row
    lda idx
    asl
    tay
    lda icon_lbl,y
    sta ptr
    lda icon_lbl+1,y
    sta ptr+1
    rts

; ============================================================================
; launcher
; ============================================================================
draw_launcher:
    jsr load_palette
    ; title bar
    lda #0
    sta col
    sta row
    jsr vdc_cell
    ldx #SCRCOLS
@tb: lda #T_WHITE
    jsr putcell
    dex
    bne @tb
    lda #<s_title
    sta ptr
    lda #>s_title
    sta ptr+1
    lda #1
    sta col
    lda #0
    sta row
    lda #T_FONTINV
    sta tmp
    jsr puts
    ldx #0
@ig: stx idx
    txa
    and #3
    asl
    asl
    asl
    clc
    adc #1
    sta col
    txa
    lsr
    lsr
    sta grp
    asl
    asl
    clc
    adc grp
    adc grp
    adc grp
    clc
    adc #3
    sta row
    lda idx
    jsr draw_icon
    lda row
    clc
    adc #2
    sta row
    lda idx
    asl
    tay
    lda icon_lbl,y
    sta ptr
    lda icon_lbl+1,y
    sta ptr+1
    lda #T_FONT
    ldy idx
    cpy v_sel
    bne @nf
    lda #T_FONTINV
@nf: sta tmp
    jsr puts
    ldx idx
    inx
    cpx #NICONS
    bne @ig
    rts

draw_highlight:
    lda v_selp
    jsr label_pos
    lda #T_FONT
    sta tmp
    jsr puts
    lda v_sel
    jsr label_pos
    lda #T_FONTINV
    sta tmp
    jmp puts

; ============================================================================
; input + navigation
; ============================================================================
read_pad:
.ifdef AUTOTEST
    jmp auto_input
.endif
    lda v_pad
    sta v_padp
    ; PCE joypad: write $01 (CLR=0,SEL=1) to read d-pad nibble, $03 then read buttons.
    ; Simplified 2-read protocol.
    lda #$01
    sta JOY                ; SEL=1 -> d-pad on low nibble
    nop
    nop
    lda JOY
    and #$0F
    eor #$0F              ; active-low -> high; this nibble = d-pad (L D R U order varies)
    asl
    asl
    asl
    asl                   ; move to high nibble
    sta tmp
    lda #$00
    sta JOY               ; SEL=0 -> buttons
    nop
    nop
    lda JOY
    and #$0F
    eor #$0F
    ora tmp               ; low nibble = buttons, high = d-pad
    sta v_pad
    lda #$03
    sta JOY
    lda v_padp
    eor #$FF
    and v_pad
    sta v_pade
    rts

update:
    lda v_inapp
    bne @app
    jmp nav_input
@app:
    lda v_pade
    and #PAD_B
    beq @disp
    jmp enter_launcher
@disp:
    lda v_app
    cmp #1
    beq @clk
    cmp #3
    beq @mus
    cmp #5
    beq @thm
    cmp #7
    beq @dos
    rts
@clk: rts
@mus: jmp music_tick
@thm: jmp theme_input
@dos: jmp dostris_update

nav_input:
    lda v_pade
    and #PAD_A
    beq @dir
    lda v_sel
    sta v_app
    lda #1
    sta v_inapp
    jmp enter_app
@dir:
    lda v_pade
    and #PAD_R
    beq @l
    jsr sel_right
@l: lda v_pade
    and #PAD_L
    beq @d
    jsr sel_left
@d: lda v_pade
    and #PAD_D
    beq @u
    jsr sel_down
@u: lda v_pade
    and #PAD_U
    beq @done
    jsr sel_up
@done:
    rts

sel_right:
    lda v_sel
    sta v_selp
    clc
    adc #1
    cmp #NICONS
    bcc @s
    lda #0
@s: sta v_sel
    jmp mark_hl
sel_left:
    lda v_sel
    sta v_selp
    bne @d
    lda #NICONS
@d: sec
    sbc #1
    sta v_sel
    jmp mark_hl
sel_down:
    lda v_sel
    clc
    adc #4
    cmp #NICONS
    bcs @no
    pha
    lda v_sel
    sta v_selp
    pla
    sta v_sel
    jmp mark_hl
@no: rts
sel_up:
    lda v_sel
    cmp #4
    bcc @no
    sta v_selp
    sec
    sbc #4
    sta v_sel
    jmp mark_hl
@no: rts
mark_hl:
    lda #1
    sta pf_hl
    rts

enter_app:
    lda v_app
    cmp #7
    bne @m
    jsr dostris_init
@m: lda v_app
    cmp #3
    bne @d
    jsr music_init
@d: lda #1
    sta v_dirty
    rts
enter_launcher:
    lda #0
    sta v_inapp
    jsr music_silence
    lda #1
    sta v_dirty
    rts

; ============================================================================
; render_partials / full_redraw
; ============================================================================
render_partials:
    lda v_inapp
    bne @app
    lda pf_hl
    bne @hl
    rts
@hl: jsr draw_highlight
    lda #0
    sta pf_hl
    rts
@app:
    lda v_app
    cmp #1
    bne @n1
    jmp rp_clock
@n1: cmp #3
    bne @n3
    jmp rp_music
@n3: cmp #5
    bne @n5
    jmp rp_theme
@n5: cmp #7
    bne @n7
    jmp rp_dostris
@n7: rts
rp_clock:
    lda pf_clk
    bne @go
    rts
@go: jsr draw_clock_time
    lda #0
    sta pf_clk
    rts
rp_music:
    lda pf_score
    bne @go
    rts
@go: jsr draw_music_status
    lda #0
    sta pf_score
    rts
rp_theme:
    lda pf_pal
    bne @go
    rts
@go: jsr draw_theme_status
    lda #0
    sta pf_pal
    rts
rp_dostris:
    lda pf_bd
    bne @board
    lda pf_pc
    bne @go
    rts
@board: jsr dostris_draw_board
    lda #0
    sta pf_bd
    sta pf_pc
    rts
@go: jsr draw_piece_partial
    lda #0
    sta pf_pc
    rts

full_redraw:
    lda #0
    sta v_dirty
    sta pf_hl
    sta pf_pc
    sta pf_bd
    jsr display_off
    jsr clear_bat
    lda v_inapp
    bne @app
    jsr draw_launcher
    bra @on
@app:
    jsr draw_app
@on:
    jsr display_on
    rts

two_digits:
    ldx #$30
@t: cmp #10
    bcc @d
    sbc #10
    inx
    bra @t
@d: clc
    adc #$30
    rts

.include "apps.inc"
.include "dostris.inc"
.include "pce_data.inc"

.segment "VECTORS"
    .word start            ; IRQ2/BRK
    .word start            ; IRQ1
    .word start            ; TIMER
    .word start            ; NMI
    .word start            ; RESET
