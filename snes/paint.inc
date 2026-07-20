; ============================================================================
; UnoDOS/SNES Paint (proc 10) - the shared MacPaint-style editor, realised
; with the SNES's "canvas of UNIQUE tiles" trick (the console has no bitmap
; mode): the 160x96 canvas = 20x12 cells, each its own 4bpp tile at
; T_PTCAN.., so it behaves like a bitmap inside the cell desktop.
;
; A pixel is painted DIRECTLY into the planar-tile shadow (PT_CANV in bank
; $7F) - no separate byte-per-pixel store, no pack step - and the tile is
; queued; the NMI DMAs a bounded number of dirty tiles to VRAM each vblank
; (the per-frame DMA budget, HANDOFF SS2/SS7). Painting is cursor-driven:
; move the desktop pointer over the canvas and hold the button.
;
; DEVIATION from the Genesis editor: pencil + eraser only (no line/rect/oval/
; fill/spray), a 12-colour pen palette (palette line 3), and no tape I/O -
; the unique-tile CANVAS is what this milestone proves. Flagged in HANDOFF.
;   [ / ]  previous / next pen colour     c  cycle pen (wraps 4..15)
;   e      toggle eraser (paints PT_BG)   n  clear
; Picking a pen ([ / ] / c) drops back out of eraser mode.
; ============================================================================

PT_W  = 160
PT_H  = 96
PT_TW = 20                  ; canvas width in tiles
PT_TH = 12
PT_BG = 3                   ; background pen = palette-3 white

; pt_setpx - A0 = x, A1 = y, A2 = colour (0-15): paint into the tile shadow,
; queue the tile. Clips to the canvas. Clobbers A/X/Y/S0-S3.
.proc pt_setpx
.a16
.i16
        lda A0
        bmi @rej
        cmp #PT_W
        bcs @rej
        lda A1
        bmi @rej
        cmp #PT_H
        bcc @ok
@rej:   rts
@ok:    ; tileidx = (y>>3)*PT_TW + (x>>3)
        lda A1
        lsr a
        lsr a
        lsr a                   ; ty
        sta S0
        asl a
        asl a                   ; ty*4
        clc
        adc S0                  ; ty*5
        asl a
        asl a                   ; ty*20
        sta S1
        lda A0
        lsr a
        lsr a
        lsr a                   ; tx
        clc
        adc S1
        sta S2                  ; tileidx
        ; byte offset of (row) within the tile: planes 0/1 at row*2,
        ; planes 2/3 at 16 + row*2. tile base = tileidx*32.
        asl a                   ; tileidx*2 (re-derive below; A clobbered)
        lda S2
        ; tileidx*32
        asl a
        asl a
        asl a
        asl a
        asl a
        sta S3                  ; tile base offset
        lda A1
        and #$0007              ; row
        asl a                   ; row*2
        clc
        adc S3
        tax                     ; X = plane0/1 offset
        ; bit mask = $80 >> (x&7)
        lda A0
        and #$0007
        sta S0                  ; col
        lda #$0080
        ldy S0
        beq @mok
@msh:   lsr a
        dey
        bne @msh
@mok:   sta S1                  ; mask (8-bit value in low byte)
        ; write the 4 planes
        lda A2
        sta S0                  ; colour
        ; plane 0 -> X, plane 1 -> X+1, plane 2 -> X+16, plane 3 -> X+17
        ; bit 0
        sep #$20
.a8
        lda S0
        and #$01
        jsr pt_planebit         ; uses X (offset), S1 (mask); X advanced caller-side
        rep #$20
        inx
        sep #$20
.a8
        lda S0
        lsr a
        and #$01
        jsr pt_planebit
        rep #$20
        txa
        clc
        adc #15                 ; X was X0+1; +15 -> X0+16
        tax
        sep #$20
.a8
        lda S0
        lsr a
        lsr a
        and #$01
        jsr pt_planebit
        rep #$20
        inx
        sep #$20
.a8
        lda S0
        lsr a
        lsr a
        lsr a
        and #$01
        jsr pt_planebit
        rep #$20
        ; queue the tile
        lda S2
        jsr pt_enqueue
@out:   rts
.endproc

; pt_planebit - A (8-bit, 0/1) = set/clear bit; X = PT_CANV byte offset,
; S1 low = bitmask. 8-bit A on entry/exit.
.proc pt_planebit
.a8
.i16
        cmp #$01
        bne @clr
        lda f:PT_CANV,x
        ora S1
        sta f:PT_CANV,x
        rts
@clr:   lda S1
        eor #$FF
        and f:PT_CANV,x
        sta f:PT_CANV,x
        rts
.endproc

; pt_enqueue - A = tile index: push onto the dirty ring if not already there.
.proc pt_enqueue
.a16
.i16
        tax                     ; X = tileidx
        sep #$20
.a8
        lda f:PT_INQ,x
        bne @done               ; already queued
        lda #1
        sta f:PT_INQ,x
        rep #$20
.a16
        ; PT_QUE[qt] = tileidx; qt = (qt+1) & 255  (long,X only - no long,Y)
        txa
        pha                     ; save tileidx
        lda v_pt_qt
        asl a
        tax                     ; X = queue word offset
        pla                     ; tileidx
        sta f:PT_QUE,x
        lda v_pt_qt
        inc a
        and #$00FF
        sta v_pt_qt
        rts
@done:  rep #$20
.a16
        rts
.endproc

; pt_flush - NMI: DMA up to 24 dirty canvas tiles to VRAM. 8-bit A on entry.
.proc pt_flush
.a8
.i16
        rep #$20
.a16
        ldx #24                 ; per-frame budget
@loop:  lda v_pt_qh
        cmp v_pt_qt
        beq @done               ; queue empty
        phx
        ; tileidx = PT_QUE[qh]
        lda v_pt_qh
        asl a
        tax
        lda f:PT_QUE,x
        sta S0                  ; tileidx
        ; clear in-queue flag
        tax
        sep #$20
.a8
        lda #$00
        sta f:PT_INQ,x
        rep #$20
        ; advance qh
        lda v_pt_qh
        inc a
        and #$00FF
        sta v_pt_qh
        ; VMADDL = VRAM_CHR + (T_PTCAN + tileidx) * 16
        lda S0
        clc
        adc #T_PTCAN
        asl a
        asl a
        asl a
        asl a                   ; *16
        clc
        adc #VRAM_CHR
        sta VMADDL
        ; DMA 32 bytes from PT_CANV + tileidx*32 (bank $7F) to VMDATAL
        sep #$20
.a8
        lda #$01
        sta DMAP0               ; word, fixed B-addr increment A
        lda #<VMDATAL
        sta BBAD0
        rep #$20
.a16
        lda S0
        asl a
        asl a
        asl a
        asl a
        asl a                   ; tileidx*32
        sta A1T0L
        lda #32
        sta DAS0L
        sep #$20
.a8
        lda #$7F
        sta A1B0
        lda #$01
        sta MDMAEN
        rep #$20
.a16
        plx
        dex
        bne @loop
@done:  sep #$20
.a8
        rts
.endproc

; pt_fill_white - fill the whole canvas shadow with PT_BG tiles. A planar tile
; of index 3 = planes 0,1 all set, planes 2,3 clear -> bytes 0-15 $FF, 16-31 $00.
.proc pt_fill_white
.a16
.i16
        ldx #$0000
@t:     ; per tile: 16 bytes $FF then 16 bytes $00
        ldy #$0000
        sep #$20
.a8
@hi:    lda #$FF
        sta f:PT_CANV,x
        inx
        iny
        cpy #16
        bne @hi
        ldy #$0000
        lda #$00
@lo:    sta f:PT_CANV,x
        inx
        iny
        cpy #16
        bne @lo
        rep #$20
        cpx #(PT_TW*PT_TH*32)
        bne @t
        rts
.endproc

; pt_clear - reset the canvas to white and queue every tile
.proc pt_clear
.a16
.i16
        jsr pt_fill_white
        stz LC0
@q:     lda LC0
        jsr pt_enqueue
        inc LC0
        lda LC0
        cmp #(PT_TW*PT_TH)
        bcc @q
        rts
.endproc

; pt_init - boot: pen = 4, cursor centred, queue empty, canvas white + DMA all
.proc pt_init
.a16
.i16
        rep #$30                ; force 16-bit A/X/Y (boot caller width unknown)
        lda #4
        sta v_pt_pen
        lda #(PT_W/2)
        sta v_pt_x
        lda #(PT_H/2)
        sta v_pt_y
        stz v_pt_erase
        stz v_pt_qh
        stz v_pt_qt
        ; clear in-queue flags
        ldx #$0000
        sep #$20
.a8
        lda #$00
@cf:    sta f:PT_INQ,x
        inx
        cpx #(PT_TW*PT_TH)
        bne @cf
        rep #$20
        jsr pt_fill_white
        ; one-shot DMA the whole canvas to VRAM (boot forced-blank context)
        rep #$20
.a16
        lda #(VRAM_CHR + T_PTCAN*16)
        sta VMADDL
        sep #$20
.a8
        lda #$01
        sta DMAP0
        lda #<VMDATAL
        sta BBAD0
        rep #$20
.a16
        lda #$0000
        sta A1T0L
        lda #(PT_TW*PT_TH*32)
        sta DAS0L
        sep #$20
.a8
        lda #$7F
        sta A1B0
        lda #$01
        sta MDMAEN
        rep #$20
.a16
        rts
.endproc

; pt_pen_eff - A = the effective draw colour: PT_BG in eraser mode, else the pen
.proc pt_pen_eff
.a16
.i16
        lda v_pt_erase
        bne @er
        lda v_pt_pen
        rts
@er:    lda #PT_BG
        rts
.endproc

; paint_tick - main loop: when Paint is topmost and the pointer is over the
; canvas with the button down, paint at that pixel (pen or eraser colour).
.proc paint_tick
.a16
.i16
        lda v_zcount
        bne :+
        rts
:       dec a
        jsr zent_x
        stx S4                  ; window entry
        sep #$20
.a8
        lda v_wintab+WPROC,x
        rep #$20
        and #$00FF
        cmp #10
        beq :+
        rts
:       lda v_mouse_btn
        bne :+
        rts
:       ; canvas origin in pixels: (WX+1)*8, (WY+2)*8
        ldx S4
        lda v_wintab+WX,x
        inc a
        asl a
        asl a
        asl a
        sta S0                  ; ox
        lda v_wintab+WY,x
        clc
        adc #2
        asl a
        asl a
        asl a
        sta S1                  ; oy
        lda v_mouse_x
        sec
        sbc S0
        sta A0                  ; canvas x
        lda v_mouse_y
        sec
        sbc S1
        sta A1
        jsr pt_pen_eff
        sta A2
        jsr pt_setpx
        ; thicken: also the pixel to the right + below (a 2x2 nib)
        inc A0
        jsr pt_setpx
        inc A1
        jsr pt_setpx
        dec A0
        jsr pt_setpx
        rts
.endproc

; paint_draw - S2 = window offset: header (pen), the canvas cells, footer
.proc paint_draw
.a16
.i16
        ; header: "Paint  pen NN"
        lda #.loword(str_pt_title)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        inc a
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        ; pen swatch (current colour on palette line 3)
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #10
        sta A0
        lda v_wintab+WY,x
        inc a
        sta A1
        lda #1
        sta A2
        sta A3
        lda v_pt_pen
        sec
        sbc #4                  ; T_GSOL index 0 = palette idx 4
        clc
        adc #(ATTR_KEY+T_GSOL)
        sta A4
        jsr fill_cells
        ; pen number
        lda v_pt_pen
        sta S0
        jsr fmt_dec
        lda #.loword(v_numbuf)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #12
        sta A0
        lda v_wintab+WY,x
        inc a
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        ; eraser indicator ("ERASE" inverted when on, blanked when off)
        lda v_pt_erase
        bne @eron
        lda #.loword(str_pt_noer)
        sta P0
        lda #ATTR_NORM
        bra @erput
@eron:  lda #.loword(str_pt_er)
        sta P0
        lda #ATTR_INV
@erput: sta A4
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #15
        sta A0
        lda v_wintab+WY,x
        inc a
        sta A1
        jsr draw_str
        ; canvas cells: T_PTCAN + ty*PT_TW + tx on palette line 3
        stz LC1                 ; ty
@row:   stz LC0                 ; tx
@col:   lda LC1
        asl a
        asl a
        sta S0
        lda LC1
        clc
        adc S0                  ; ty*5
        asl a
        asl a                   ; ty*20
        clc
        adc LC0                 ; + tx
        clc
        adc #(ATTR_KEY+T_PTCAN)
        sta A4
        ldx S2
        lda v_wintab+WX,x
        inc a
        clc
        adc LC0
        sta A0
        lda v_wintab+WY,x
        clc
        adc #2
        clc
        adc LC1
        sta A1
        lda #1
        sta A2
        sta A3
        jsr fill_cells
        inc LC0
        lda LC0
        cmp #PT_TW
        bcc @col
        inc LC1
        lda LC1
        cmp #PT_TH
        bcc @row
        ; footer
        lda #.loword(str_pt_foot)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        sec
        sbc #2
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        rts
.endproc

; paint_key - A1 = event data (raw<<8 | ascii)
.proc paint_key
.a16
.i16
        lda A1
        and #$00FF
        cmp #'['
        beq @prev
        cmp #']'
        beq @next
        cmp #'c'
        beq @cyc
        cmp #'e'
        beq @ers
        cmp #'n'
        beq @clear
        rts
@prev:  stz v_pt_erase          ; picking a pen leaves eraser mode
        lda v_pt_pen
        cmp #5
        bcc @rd
        dec a
        sta v_pt_pen
        bra @rd
@next:  stz v_pt_erase
        lda v_pt_pen
        cmp #15
        bcs @rd
        inc a
        sta v_pt_pen
        bra @rd
@cyc:   stz v_pt_erase          ; cycle the CGRAM palette-3 pen row, wrapping
        lda v_pt_pen
        inc a
        cmp #16
        bcc @cset
        lda #4
@cset:  sta v_pt_pen
        bra @rd
@ers:   lda v_pt_erase          ; toggle eraser
        eor #1
        sta v_pt_erase
        bra @rd
@clear: jsr pt_clear
@rd:    jsr redraw_topmost
        rts
.endproc

; ---- data ----
.segment "RODATA"
str_t_paint:  .byte "Paint", 0
name_paint:   .byte "Paint", 0
str_pt_title: .byte "Paint  pen", 0
str_pt_foot:  .byte "[ ]c pen e ers n clr", 0
str_pt_er:    .byte "ERASE", 0
str_pt_noer:  .byte "     ", 0
