; ============================================================================
; UnoDOS/C64 Paint - a DISK-LOADED app (app8.bin, icon 8). A colour canvas
; editor that leans on the VIC's per-cell colour: a 32x16 grid of 8x8 cells,
; each one of the 16 C64 colours. CRSR moves the cursor, SPACE paints the
; current ink, +/- cycle the ink, F fills the whole canvas, S/L save/load
; PAINT.UNO via the kernel FS, RUN/STOP exits.
;
; Where the Apple II Paint is a 1-bit dither canvas, this is true 16-colour -
; the canvas byte IS the cell colour. The canvas lives in APP_BSS (512 bytes).
; ============================================================================

        processor 6502
        include "sys.inc"
        include "build/kernel_api.inc"

PT_W     equ 32
PT_H     equ 16
PT_OX    equ 2          ; screen cell col of canvas col 0
PT_OY    equ 2
CANVAS   equ APP_BSS    ; 512 bytes (PT_W*PT_H), 1 colour/cell

zpPx     equ zpApp0     ; helper cell col
zpPy     equ zpApp1
zpPI     equ zpApp2
zpPJ     equ zpApp3

        org APP_BASE
        jmp pt_init
        jmp pt_key
        jmp pt_tick

pt_init:
        ; clear canvas to white (colour 1)
        lda #1
        ldx #0
pi_lo:
        sta CANVAS,x
        inx
        bne pi_lo
        ldx #0
pi_hi:
        sta CANVAS+256,x
        inx
        bne pi_hi
        lda #16
        sta pt_cx
        lda #8
        sta pt_cy
        lda #2                  ; start ink = red
        sta pt_ink
        jmp pt_draw_all

pt_tick:
        rts                     ; static app (no animation)

pt_key:
        lda zpTmp
        cmp #K_ESC
        bne ptk1
        jmp return_to_desktop
ptk_ret:                        ; near rts anchor (edge checks branch here)
        rts
ptk1:
        cmp #K_LEFT
        bne ptk2
        lda pt_cx
        beq ptk_ret
        jsr pt_cur_erase
        dec pt_cx
        jmp pt_cur_draw
ptk2:
        cmp #K_RIGHT
        bne ptk3
        lda pt_cx
        cmp #(PT_W-1)
        bcs ptk_ret
        jsr pt_cur_erase
        inc pt_cx
        jmp pt_cur_draw
ptk3:
        cmp #K_UP
        bne ptk4
        lda pt_cy
        beq ptk_ret
        jsr pt_cur_erase
        dec pt_cy
        jmp pt_cur_draw
ptk4:
        cmp #K_DOWN
        bne ptk5
        lda pt_cy
        cmp #(PT_H-1)
        bcs ptk_ret
        jsr pt_cur_erase
        inc pt_cy
        jmp pt_cur_draw
ptk5:
        cmp #K_SPACE
        bne ptk6
        ; paint current cell with the ink
        jsr pt_cur_idx          ; X = cy*32+cx
        lda pt_ink
        sta CANVAS,x
        jmp pt_cur_draw
ptk6:
        cmp #$2B                ; '+' next ink
        bne ptk7
        lda pt_ink
        clc
        adc #1
        and #$0F
        sta pt_ink
        jmp pt_draw_status
ptk7:
        cmp #$2D                ; '-' prev ink
        bne ptk8
        lda pt_ink
        sec
        sbc #1
        and #$0F
        sta pt_ink
        jmp pt_draw_status
ptk8:
        cmp #$46                ; 'F' fill canvas with ink
        bne ptk9
        ldx #0
        lda pt_ink
ptf_lo:
        sta CANVAS,x
        inx
        bne ptf_lo
ptf_hi:
        sta CANVAS+256,x
        inx
        bne ptf_hi
        jmp pt_draw_all
ptk9:
        cmp #$53                ; 'S' save
        bne ptk10
        jmp pt_save
ptk10:
        cmp #$4C                ; 'L' load
        bne ptk_done
        jmp pt_load
ptk_done:
        rts

; pt_cur_idx - X = pt_cy*32 + pt_cx.
pt_cur_idx:
        lda pt_cy
        asl
        asl
        asl
        asl
        asl                     ; *32
        clc
        adc pt_cx
        tax
        rts

; pt_draw_cell - draw canvas cell (zpPx, zpPy) as a solid colour block.
pt_draw_cell:
        lda zpPy
        asl
        asl
        asl
        asl
        asl
        clc
        adc zpPx
        tax
        lda CANVAS,x            ; colour 0..15
        sta zpFCol             ; tmp
        ; cell colour = (c<<4)|c (solid), via color_fill; bitmap stays 0
        asl
        asl
        asl
        asl
        ora zpFCol
        sta zpFCol
        lda zpPx
        clc
        adc #PT_OX
        sta zpCX
        lda zpPy
        clc
        adc #PT_OY
        sta zpCY
        lda #1
        sta zpCW
        sta zpCH
        jmp color_fill

; pt_cur_erase - redraw the cursor cell plainly (from the canvas).
pt_cur_erase:
        lda pt_cx
        sta zpPx
        lda pt_cy
        sta zpPy
        jmp pt_draw_cell

; pt_cur_draw - redraw the cursor cell + a white box overlay, then status.
pt_cur_draw:
        lda pt_cx
        sta zpPx
        lda pt_cy
        sta zpPy
        jsr pt_draw_cell
        ; box overlay (white outline over the cell colour)
        jsr pt_cur_idx
        lda CANVAS,x
        ora #$10                ; fg=white, bg=cell colour
        sta zpFCol
        lda #<spr_cursor
        sta zpFontPtr
        lda #>spr_cursor
        sta zpFontPtr+1
        lda #0
        sta zpInv
        lda pt_cx
        clc
        adc #PT_OX
        sta zpCol
        lda pt_cy
        clc
        adc #PT_OY
        sta zpRow
        jsr blit_cell
        jmp pt_draw_status

pt_draw_all:
        jsr app_clear
        lda #<msg_pt_title
        sta zpPtr
        lda #>msg_pt_title
        sta zpPtr+1
        lda #1
        sta zpCol
        lda #0
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string
        lda #0
        sta zpFX
        lda #7
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #1
        sta zpFH
        lda #$FF
        sta zpFPat
        jsr fill_rows
        ; canvas cells
        lda #0
        sta zpPy
pda_rl:
        lda #0
        sta zpPx
pda_cl:
        jsr pt_draw_cell
        inc zpPx
        lda zpPx
        cmp #PT_W
        bne pda_cl
        inc zpPy
        lda zpPy
        cmp #PT_H
        bne pda_rl
        jsr pt_cur_draw         ; cursor overlay (also draws status)
        rts

; pt_draw_status - ink swatch + help on row 23.
pt_draw_status:
        ; clear status row colour first
        lda #COL_WIN
        sta zpFCol
        lda #0
        sta zpCX
        lda #23
        sta zpCY
        lda #SCRCOLS
        sta zpCW
        lda #1
        sta zpCH
        jsr color_fill
        lda #0
        sta zpFX
        lda #184
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #8
        sta zpFH
        lda #$00
        sta zpFPat
        jsr fill_rows
        ; "Ink:" + a colour swatch cell
        lda #<msg_pt_ink
        sta zpPtr
        lda #>msg_pt_ink
        sta zpPtr+1
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        jsr draw_string
        ; swatch at col 6
        lda pt_ink
        sta zpPx
        asl
        asl
        asl
        asl
        ora zpPx
        sta zpFCol
        lda #6
        sta zpCX
        lda #23
        sta zpCY
        lda #2
        sta zpCW
        lda #1
        sta zpCH
        jsr color_fill
        lda #6
        sta zpFX
        lda #184
        sta zpFY
        lda #2
        sta zpFW
        lda #8
        sta zpFH
        lda #$FF
        sta zpFPat
        jsr fill_rows
        ; help
        lda #<msg_pt_help
        sta zpPtr
        lda #>msg_pt_help
        sta zpPtr+1
        lda #10
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        jmp draw_string

; ---- save / load PAINT.UNO (512 bytes = the canvas) ----
pt_save:
        lda #<pt_fname
        sta zpFSName
        lda #>pt_fname
        sta zpFSName+1
        lda #<CANVAS
        sta zpFSDat
        lda #>CANVAS
        sta zpFSDat+1
        lda #<512
        sta zpFSSize
        lda #>512
        sta zpFSSize+1
        jsr fs_save
        jsr sid_click
        rts
pt_load:
        lda #<pt_fname
        sta zpFSName
        lda #>pt_fname
        sta zpFSName+1
        jsr fs_find
        cmp #$FF
        beq pt_load_done
        pha
        lda #<CANVAS
        sta zpFSDat
        lda #>CANVAS
        sta zpFSDat+1
        lda #<512               ; CANVAS = 512 bytes (fs_read cap)
        sta zpFSMax
        lda #>512
        sta zpFSMax+1
        pla
        jsr fs_read
        jsr sid_click
        jmp pt_draw_all
pt_load_done:
        rts

spr_cursor: dc.b $FF,$81,$81,$81,$81,$81,$81,$FF
pt_fname:   dc.b "PAINT.UNO",0,0,0
msg_pt_title: dc.b "Paint",0
msg_pt_ink:   dc.b "Ink:",0
msg_pt_help:  dc.b "SPC paint +/- ink F fill S/L STOP",0

pt_cx:  dc.b 16
pt_cy:  dc.b 8
pt_ink: dc.b 2
