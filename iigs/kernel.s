; ============================================================================
; UnoDOS/Apple IIGS - milestone 0 kernel: enable Super Hi-Res and paint the
; UnoDOS splash (color desktop + a framed window + text), proving the
; toolchain, the ProDOS block-boot chain, and the SHR 4bpp renderer.
;
; Entry: boot.s JMPs here at $00:2000 already in NATIVE mode with 16-bit
; A/X/Y, PBR=DBR=0.  "Kernel-normal" register convention for the draw code:
; D=0, 8-bit A (M=1), 16-bit X/Y (X=0), DBR=$E1 (the SHR bank) while drawing.
;
; Super Hi-Res framebuffer (bank $E1):
;   $2000-$9CFF  32000 bytes pixel data: 200 rows x 160 bytes,
;                4bpp / 2px per byte, HIGH nibble = LEFT pixel.
;   $9D00-$9DC7  200 scanline control bytes (palette select + mode)
;   $9E00-$9FFF  16 palette lines x 16 colors x 2 bytes ($0RGB little-endian)
; NEWVIDEO ($C029) bit7 enables SHR; we write $C1.
; ============================================================================

.p816
.smart +

; Font + palette data go in RODATA (NOT at the CODE origin $2000, where the
; boot stage jumps to start:).  RODATA links after CODE, still in bank 0.
.segment "RODATA"
.include "gen_data.inc"
.segment "CODE"

; ----------------------------------------------------------- soft switches
NEWVIDEO = $C029

; ----------------------------------------------------------- SHR layout (bank E1)
SHR_PIX  = $2000
SHR_SCB  = $9D00
SHR_PAL  = $9E00
SHR_BANK = $E1
ROWBYTES = 160
NPIX     = 32000          ; 200 * 160

; ----------------------------------------------------------- zero page scratch
GP      = $10             ; 16-bit SHR dest offset (within bank $E1)
GIDX    = $12             ; 16-bit font index
STRP    = $1C             ; 24-bit string pointer ($1C lo,$1D hi,$1E bank)
fbits   = $14             ; current font row bits
pxtmp   = $15             ; pixel-byte build temp
rowc    = $16             ; glyph row / band row counter
cur_fg  = $17             ; foreground palette index (0..15)
cur_bg  = $18             ; background palette index
cur_fgh = $19             ; fg << 4
cur_bgh = $1A             ; bg << 4
mtmp    = $20             ; 16-bit math temp
mtmp2   = $22
; rectangle / position parameters (16-bit words; high byte 0)
PX      = $30             ; xbyte
PY      = $32             ; yrow
PW      = $34             ; width in bytes
PH      = $36             ; height in rows
PB      = $38             ; fill byte value

; SETSTR: point STRP (24-bit, bank 0) at a NUL-terminated string label.
.macro SETSTR lbl
        lda #<lbl
        sta STRP
        lda #>lbl
        sta STRP+1
        stz STRP+2
.endmacro

; pixel-byte builder: A := (cur_*h | cur_*) per the two mask bits of fbits.
.macro PIXBYTE mh, ml
.local hi0, hidone, lo0, lodone
        lda fbits
        and #mh
        beq hi0
        lda cur_fgh
        bra hidone
hi0:    lda cur_bgh
hidone: sta pxtmp
        lda fbits
        and #ml
        beq lo0
        lda cur_fg
        bra lodone
lo0:    lda cur_bg
lodone: ora pxtmp
.endmacro

.segment "CODE"
start:
        clc
        xce                    ; ensure native (boot already did; belt+braces)
        rep #$30               ; 16-bit A/X/Y
        ldx #$01FF
        txs                    ; stack in page 1
        lda #0
        tcd                    ; direct page = 0

        ; ---- enable Super Hi-Res ----
        sep #$20               ; 8-bit A
        lda #$C1               ; SHR on (bit7) + linear
        sta NEWVIDEO

        ; ---- DBR = $E1 so absolute stores hit the SHR bank ----
        lda #SHR_BANK
        pha
        plb
        rep #$10               ; kernel-normal: 8-bit A, 16-bit X/Y

        ; ---- clear 200 scanline control bytes -> palette 0, 320 mode ----
        ldx #0
        lda #$00
clr_scb:
        sta SHR_SCB,x
        inx
        cpx #200
        bcc clr_scb

        ; ---- copy palette line 0 from the kernel image (bank 0) ----
        ldx #0
cp_pal:
        lda f:pal_main,x       ; bank-0 source (long addressing)
        sta SHR_PAL,x          ; DBR=$E1 destination
        inx
        cpx #32
        bcc cp_pal

        ; ---- desktop: fill all 32000 pixel bytes with index 0 (blue) ----
        lda #$00
        sta PB
        jsr fill_screen

        ; ================= menu bar: rows 0..9, light grey (5) =================
        jsr p_clear
        lda #ROWBYTES
        sta PW
        lda #10
        sta PH
        lda #$55               ; two index-5 pixels
        sta PB
        jsr fill_band
        ; title text: blue (0) on grey (5)
        lda #0
        ldx #5
        jsr set_color
        lda #2                 ; xbyte
        ldx #1                 ; yrow
        jsr set_pos
        SETSTR menu_title
        jsr draw_string

        ; ============== centre window panel: frame + body + titlebar ==========
        ; black backing rect
        jsr p_clear
        lda #30
        sta PX
        lda #40
        sta PY
        lda #100
        sta PW
        lda #120
        sta PH
        lda #$44               ; black (4)
        sta PB
        jsr fill_band
        ; cyan body inset 1 byte / 2 rows
        lda #31
        sta PX
        lda #42
        sta PY
        lda #98
        sta PW
        lda #116
        sta PH
        lda #$22               ; cyan (2)
        sta PB
        jsr fill_band
        ; deep-blue title bar (rows 42..53)
        lda #31
        sta PX
        lda #42
        sta PY
        lda #98
        sta PW
        lda #12
        sta PH
        lda #$66               ; deep blue (6)
        sta PB
        jsr fill_band

        ; panel title: white (1) on deep blue (6)
        lda #1
        ldx #6
        jsr set_color
        lda #34
        ldx #44                ; yrow inside the panel title bar (42..53)
        jsr set_pos
        SETSTR panel_title
        jsr draw_string

        ; body lines: white (1) on cyan (2)
        lda #1
        ldx #2
        jsr set_color
        lda #34
        ldx #60                ; body lines inside the cyan panel body
        jsr set_pos
        SETSTR line1
        jsr draw_string
        lda #34
        ldx #74
        jsr set_pos
        SETSTR line2
        jsr draw_string
        lda #34
        ldx #88
        jsr set_pos
        SETSTR line3
        jsr draw_string
        lda #34
        ldx #102
        jsr set_pos
        SETSTR line4
        jsr draw_string

done:
        stp                    ; CPU halts; Mega II keeps the SHR image on screen

; ============================================================================
; fill_screen: fill all NPIX pixel bytes with PB (replicated into a word).
; ============================================================================
fill_screen:
        rep #$20
        lda PB
        and #$00FF
        sta mtmp
        asl a
        asl a
        asl a
        asl a
        asl a
        asl a
        asl a
        asl a
        ora mtmp               ; word = byteval:byteval
        ldx #0
fs_loop:
        sta SHR_PIX,x
        inx
        inx
        cpx #NPIX
        bcc fs_loop
        sep #$20
        rts

; ============================================================================
; fill_band: fill PW-byte x PH-row rectangle at (PX,PY) with PB. DBR=$E1.
; ============================================================================
fill_band:
        jsr calc_gp            ; GP = SHR offset of (PX,PY)
        lda PH                 ; low byte (PH <= 200)
        sta rowc
fb_row:
        ldy #0
fb_col:
        lda PB
        sta (GP),y
        iny
        cpy PW
        bcc fb_col
        rep #$20               ; GP += ROWBYTES
        lda GP
        clc
        adc #ROWBYTES
        sta GP
        sep #$20
        dec rowc
        bne fb_row
        rts

; ============================================================================
; calc_gp: GP = SHR_PIX + PY*160 + PX   (16-bit math; restores 8-bit A)
; ============================================================================
calc_gp:
        rep #$20
        lda PY
        asl a                  ; *2
        asl a                  ; *4
        asl a                  ; *8
        asl a                  ; *16
        asl a                  ; *32
        sta mtmp2              ; PY*32
        asl a                  ; *64
        asl a                  ; *128
        clc
        adc mtmp2              ; *160
        clc
        adc #SHR_PIX
        clc
        adc PX
        sta GP
        sep #$20
        rts

; ============================================================================
; p_clear: zero the 16-bit rectangle parameter words (PX..PH). (8-bit A)
; ============================================================================
p_clear:
        rep #$20
        stz PX
        stz PY
        stz PW
        stz PH
        sep #$20
        rts

; ============================================================================
; set_color: A = fg index, X = bg index. Builds cur_fg/bg and <<4 forms.
; ============================================================================
set_color:
        sta cur_fg
        txa
        sta cur_bg
        lda cur_fg
        asl a
        asl a
        asl a
        asl a
        sta cur_fgh
        lda cur_bg
        asl a
        asl a
        asl a
        asl a
        sta cur_bgh
        rts

; ============================================================================
; set_pos: A = xbyte, X = yrow.  Sets PX/PY and computes GP.
; ============================================================================
set_pos:
        rep #$20
        and #$00FF
        sta PX
        sep #$20
        rep #$20
        txa
        and #$00FF
        sta PY
        sep #$20
        jsr calc_gp
        rts

; ============================================================================
; draw_char: A = ASCII char.  Renders the 8x8 glyph at GP (4bpp), then
; advances GP by one glyph width (4 bytes / 8 px). 8-bit A, 16-bit X/Y.
; ============================================================================
draw_char:
        rep #$20
        and #$00FF
        sec
        sbc #FONT_FIRST
        asl a
        asl a
        asl a                  ; *8
        sta GIDX
        sep #$20
        lda #8
        sta rowc
dc_row:
        ldx GIDX
        lda f:font_data,x
        sta fbits
        rep #$20
        inc GIDX
        sep #$20
        ldy #0
        PIXBYTE $80, $40
        sta (GP),y
        iny
        PIXBYTE $20, $10
        sta (GP),y
        iny
        PIXBYTE $08, $04
        sta (GP),y
        iny
        PIXBYTE $02, $01
        sta (GP),y
        rep #$20               ; GP += ROWBYTES (next scanline)
        lda GP
        clc
        adc #ROWBYTES
        sta GP
        sep #$20
        dec rowc
        beq dc_done
        jmp dc_row
dc_done:
        ; rewind GP to row 0 and advance one glyph width
        rep #$20
        lda GP
        sec
        sbc #ROWBYTES*8
        clc
        adc #4
        sta GP
        sep #$20
        rts

; ============================================================================
; draw_string: STRP -> NUL-terminated string (bank 0). Renders at GP.
; ============================================================================
draw_string:
        ldy #0
ds_loop:
        lda [STRP],y
        beq ds_done
        phy
        jsr draw_char
        ply
        iny
        bra ds_loop
ds_done:
        rts

; ============================================================================
.segment "RODATA"
menu_title:  .byte "UnoDOS 3   Apple IIGS", 0
panel_title: .byte "Welcome to UnoDOS", 0
line1:       .byte "Super Hi-Res desktop", 0
line2:       .byte "65C816 native mode", 0
line3:       .byte "Milestone 0 - boot OK", 0
line4:       .byte "Build 411", 0
