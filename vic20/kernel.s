; ============================================================================
; UnoDOS / Commodore VIC-20 (MOS 6502 + VIC 6560/6561) — milestones 1-3.
; ============================================================================
; The SEVENTH fresh contract-driven port — 6502 again (like the NES), reusing
; gen/6502/ + dasm. MINIMAL profile (CONTRACT-ARCH §9): a character-cell LIST
; launcher, one full-screen app at a time, directional nav via the joystick.
;
; Display: the VIC is a 22x23 CHARACTER matrix. The screen matrix lives at $1E00
; (char codes), colour RAM at $9600 (per-cell nibble = 8 fg colours over the
; global background); a custom 8x8 1bpp char set is copied to $3000 at boot. So
; "drawing" is a direct store to $1E00+row*22+col (and a colour to $9600+...) —
; the simplest display layer of any UnoDOS port. An INVERTED font (complement
; bitmaps) gives title-bar/selection bars.
;
; Targets a RAM-expanded VIC-20 (+8K): PRG at $1201, code+data through $2FFF,
; the char set at $3000, the screen at $1E00. Verified headlessly on a py65 6502
; core (vic20/harness.py) that renders the character matrix — like the C64/Apple
; II ports.
;
; Contract-owned (Phase 4): the cell geometry comes from unogen
; ([world.vic20] -> gen/vic20/sys_gen.inc).
; ============================================================================

        processor 6502
        include "../unodef/gen/6502/unodef.inc"     ; Contract surface (SYS_*, enums)
        include "../unodef/gen/vic20/sys_gen.inc"    ; SCRCOLS / SCRROWS

SCREEN  = $1E00
COLOR   = $9600
CHARSET = $3000
COL2SCR = $7800        ; COLOR - SCREEN

; joystick bits (active-high after read_pad inverts)
PAD_U    = $04
PAD_D    = $08
PAD_L    = $10
PAD_FIRE = $20         ; launch (in launcher) / back (in app) / rotate handled by Up
PAD_R    = $80

BW       = 10
BH       = 12
BORG_COL = 6
BORG_ROW = 4
FALLRATE = 30

; ---- zero page ----
strptr  = $00
scrptr  = $02
colptr  = $04
col     = $06
row     = $07
tmp     = $08          ; char base
pcol    = $09          ; current draw colour
idx     = $0A
grp     = $0B
dcptr   = $0C          ; draw_content table pointer (word)
v_pad   = $10
v_padp  = $11
v_pade  = $12
v_inapp = $13
v_sel   = $14
v_selp  = $15
v_app   = $16
v_dirty = $17
v_frac  = $18
v_ss    = $19
v_mm    = $1A
v_hh    = $1B
v_theme = $1C
m_idx   = $1D
m_timer = $1E
m_play  = $1F
a_idx   = $20
a_tmr   = $21
a_pad   = $22
a_gpause= $23
pf_hl   = $24
pf_clk  = $25
pf_pc   = $26
pf_score= $27
pf_bd   = $28          ; dostris board dirty (lock/lineclear): board-only repaint
g_type  = $30
g_rot   = $31
g_px    = $32
g_py    = $33
g_state = $34
g_fall  = $35
g_lines = $36
g_seed  = $37
g_tx    = $39
g_ty    = $3A
g_tmp   = $3B
g_srot  = $3C
g_row   = $3D
g_lt    = $3E
g_pt    = $3F
mlo     = $40
mhi     = $41
g_oldpx = $42
g_oldpy = $43
g_oldrot= $44
g_bx    = $45
g_by    = $46
g_n     = $47
numstr  = $50          ; 4
clk_str = $54          ; 9
g_board = $0340        ; BW*BH = 120

        org $1201
; BASIC stub: 10 SYS 4109
        dc.w basic_end, 10
        dc.b $9E, "4109", 0
basic_end:
        dc.w 0
; ============================================================================
start:                  ; $120D = 4109
        sei
        cld
        ldx #$FF
        txs
        jsr vic_init
        jsr copy_charset
        lda #0
        sta v_inapp
        sta v_sel
        sta v_selp
        sta v_theme
        sta v_dirty
        jsr draw_launcher
main:
        jsr wait_frame
        jsr render_partials
        jsr read_pad
        jsr clock_advance
        jsr update
        lda v_dirty
        beq main
        jsr full_redraw
        jmp main

; ---- vic_init: program the VIC registers + clear screen ----
vic_init:
        lda #$0C
        sta $9000          ; horizontal origin
        lda #$26
        sta $9001          ; vertical origin
        lda #SCRCOLS
        sta $9002          ; 22 columns (+ video matrix bit 9 = 0)
        lda #SCRROWS*2
        sta $9003          ; 23 rows (bits1-6), 8x8 chars
        ; $9005: video matrix at $1E00 + char base at $3000.
        ; (For the headless harness the exact nibble encoding is moot — it reads
        ; the configured $1E00/$9600/$3000 directly; this value targets real HW.)
        lda #$CC
        sta $9005
        lda #$66
        sta $900F          ; background = blue (6), border = blue
        rts

; ---- copy_charset: charset_data -> $3000 (NCHARS*8 bytes) ----
copy_charset:
        lda #<charset_data
        sta strptr
        lda #>charset_data
        sta strptr+1
        lda #<CHARSET
        sta scrptr
        lda #>CHARSET
        sta scrptr+1
        ldx #>(NCHARS*8)+1   ; page count (round up)
        ldy #0
.cp:    lda (strptr),y
        sta (scrptr),y
        iny
        bne .cp
        inc strptr+1
        inc scrptr+1
        dex
        bne .cp
        rts

; ---- wait_frame: sync to ~60 Hz via the VIC raster ($9004) ----
wait_frame:
.w1:    lda $9004
        bne .w1
.w2:    lda $9004
        beq .w2
        rts

; ---- cellptr: col,row -> scrptr = SCREEN+row*22+col ; colptr = scrptr+$7800 ----
cellptr:
        lda row
        asl
        tay
        lda row_off,y
        clc
        adc col
        sta scrptr
        lda row_off+1,y
        adc #0
        clc
        adc #>SCREEN
        sta scrptr+1
        lda scrptr
        sta colptr
        lda scrptr+1
        clc
        adc #>COL2SCR
        sta colptr+1
        rts

; ---- puts: strptr=string, col/row, tmp=char base, pcol=colour ----
puts:
        jsr cellptr
        ldy #0
.l:     lda (strptr),y
        beq .done
        sec
        sbc #32
        clc
        adc tmp
        sta (scrptr),y
        lda pcol
        sta (colptr),y
        iny
        bne .l
.done:  rts

; ---- putcell: A=char, X=offset-in-row (writes via scrptr/colptr,y=X) -- helper
; (we mostly use puts + direct stores)

; ---- draw_content: strptr2 ($0C? reuse grp area) -> table {db col,row; dw str} ----
draw_content:
.dcl:   ldy #0
        lda (dcptr),y
        cmp #$FF
        beq .dcd
        sta col
        iny
        lda (dcptr),y
        sta row
        iny
        lda (dcptr),y
        sta strptr
        iny
        lda (dcptr),y
        sta strptr+1
        ; advance dcptr by 4
        lda dcptr
        clc
        adc #4
        sta dcptr
        lda dcptr+1
        adc #0
        sta dcptr+1
        lda #T_FONT
        sta tmp
        lda #1
        sta pcol
        jsr puts
        jmp .dcl
.dcd:   rts

two_digits:
        ldx #$30
.t:     cmp #10
        bcc .d
        sbc #10
        inx
        jmp .t
.d:     clc
        adc #$30
        rts

; ---- label_pos: A=icon idx -> strptr=label, col=2, row=1+idx ----
label_pos:
        sta idx
        lda #2
        sta col
        lda idx
        clc
        adc #1
        sta row
        lda idx
        asl
        tay
        lda icon_lbl,y
        sta strptr
        lda icon_lbl+1,y
        sta strptr+1
        rts

; ---- draw_launcher: title bar + mini-icon list ----
draw_launcher:
        jsr clear_screen
        ; title bar (row 0): inverted blanks across the width + title
        lda #0
        sta col
        sta row
        jsr cellptr
        ldy #0
.tb:    lda #T_FONTINV          ; inverted space = solid white
        sta (scrptr),y
        lda #1
        sta (colptr),y
        iny
        cpy #SCRCOLS
        bne .tb
        lda #<s_title
        sta strptr
        lda #>s_title
        sta strptr+1
        lda #1
        sta col
        lda #0
        sta row
        lda #T_FONTINV
        sta tmp
        lda #1
        sta pcol
        jsr puts
        ; list items
        ldx #0
.it:    stx idx
        ; mini-icon at (0, 1+idx)
        lda #0
        sta col
        txa
        clc
        adc #1
        sta row
        jsr cellptr
        txa
        clc
        adc #T_MINI
        ldy #0
        sta (scrptr),y
        lda #3                  ; cyan icon
        sta (colptr),y
        ; label
        lda idx
        jsr label_pos
        lda #T_FONT
        ldy idx
        cpy v_sel
        bne .nf
        lda #T_FONTINV
.nf:    sta tmp
        lda #1
        sta pcol
        jsr puts
        ldx idx
        inx
        cpx #NICONS
        bne .it
        rts

clear_screen:
        ldx #0
        lda #0                  ; blank char
.c1:    sta SCREEN,x
        sta SCREEN+$100,x
        inx
        bne .c1
        ; 506 cells -> clear 512 to be safe ($1E00-$1FFF)
        rts

draw_highlight:
        lda v_selp
        jsr label_pos
        lda #T_FONT
        sta tmp
        lda #1
        sta pcol
        jsr puts
        lda v_sel
        jsr label_pos
        lda #T_FONTINV
        sta tmp
        lda #1
        sta pcol
        jmp puts

; ---- input ----
read_pad:
  IF AUTOTEST
        jmp auto_input
  ELSE
        lda v_pad
        sta v_padp
        lda $9111
        eor #$FF
        and #$3C                ; bits2-5: up,down,left,fire
        sta tmp
        lda $9120
        eor #$FF
        and #$80                ; bit7: right
        ora tmp
        sta v_pad
        lda v_padp
        eor #$FF
        and v_pad
        sta v_pade
        rts
  ENDIF

update:
        lda v_inapp
        bne up_app
        jmp nav_input
up_app:
        lda v_pade
        and #PAD_FIRE
        beq up_disp
        jmp enter_launcher
up_disp:
        lda v_app
        cmp #1
        beq up_clk
        cmp #3
        beq up_mus
        cmp #5
        beq up_thm
        cmp #7
        beq up_dos
        rts
up_clk: rts
up_mus: jmp music_tick
up_thm: jmp theme_input
up_dos: jmp dostris_update

nav_input:
        lda v_pade
        and #PAD_FIRE
        beq nd
        lda v_sel
        sta v_app
        lda #1
        sta v_inapp
        jmp enter_app
nd:     lda v_pade
        and #PAD_U
        beq nd2
        jsr sel_up
nd2:    lda v_pade
        and #PAD_D
        beq nd3
        jsr sel_down
nd3:    rts

sel_up:
        lda v_sel
        sta v_selp
        bne su
        lda #NICONS
su:     sec
        sbc #1
        sta v_sel
        jmp mark_hl
sel_down:
        lda v_sel
        sta v_selp
        clc
        adc #1
        cmp #NICONS
        bcc sd
        lda #0
sd:     sta v_sel
mark_hl:
        lda #1
        sta pf_hl
        rts

enter_app:
        lda v_app
        cmp #7
        bne ea1
        jsr dostris_init
ea1:    lda v_app
        cmp #3
        bne ea2
        jsr music_init
ea2:    lda #1
        sta v_dirty
        rts
enter_launcher:
        lda #0
        sta v_inapp
        jsr music_silence
        lda #1
        sta v_dirty
        rts

render_partials:
        lda v_inapp
        bne rp_app
        lda pf_hl
        beq rp_done
        jsr draw_highlight
        lda #0
        sta pf_hl
rp_done:
        rts
rp_app:
        lda v_app
        cmp #1
        beq rp_clk
        cmp #3
        beq rp_mus
        cmp #7
        beq rp_dos
        rts
rp_clk: lda pf_clk
        beq rp_done
        jsr draw_clock_time
        lda #0
        sta pf_clk
        rts
rp_mus: lda pf_score
        beq rp_done
        jsr draw_music_status
        lda #0
        sta pf_score
        rts
rp_dos: lda pf_bd
        beq rp_dos_pc
        jsr dostris_draw_board
        lda #0
        sta pf_bd
        sta pf_pc
        rts
rp_dos_pc:
        lda pf_pc
        beq rp_done
        jsr draw_piece_partial
        lda #0
        sta pf_pc
        rts

full_redraw:
        lda #0
        sta v_dirty
        sta pf_hl
        sta pf_pc
        sta pf_bd
        lda v_inapp
        bne fr_app
        jsr draw_launcher
        rts
fr_app: jsr draw_app
        rts

clock_advance:
        inc v_frac
        lda v_frac
        cmp #60
        bcc ca_done
        lda #0
        sta v_frac
        inc v_ss
        lda v_ss
        cmp #60
        bcc ca_fmt
        lda #0
        sta v_ss
        inc v_mm
        lda v_mm
        cmp #60
        bcc ca_fmt
        lda #0
        sta v_mm
        inc v_hh
        lda v_hh
        cmp #24
        bcc ca_fmt
        lda #0
        sta v_hh
ca_fmt: jsr clock_format
        lda #1
        sta pf_clk
ca_done:
        rts

        include "apps.inc"
        include "dostris.inc"
        include "vic_data.inc"
