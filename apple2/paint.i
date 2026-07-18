; ============================================================================
; UnoDOS/AppleII Paint (milestone 3) - the MacPaint-style editor adapted to
; the hi-res screen and a keyboard cursor (HANDOFF-M3 SS3).
;
; The canvas is a 32x34 grid of byte-aligned "fat pixels" (1 byte-column =
; 7px wide x 4px tall each), backed by PAINTBUF (one pattern byte per cell)
; - the documented byte-aligned deviation from MacPaint's pixel grid, the
; same trade the renderer makes everywhere on this port. The gamut is the
; four M1 dither inks (white / 25% / 50% / black); the canvas starts white
; (ink 0), MacPaint-style. Input is a keyboard cursor (arrows + Space)
; rather than mouse drag - the platform deviation. Tools: pencil, rectangle
; (outline + filled) and flood fill; line/oval/brush/spray are omitted on
; the 6502 (honest-deviation rule). Saves/loads PAINT.UNO on the mini-FS
; (header: the raw 1088 cell bytes, row-major).
;
;   Arrows  move      Space  apply (rect/frect: 1st sets the anchor)
;   P/R/F/I tool: Pencil / Rectangle / Frect / Fill
;   [ / ]   prev / next ink     N  clear canvas
;   S / L   save / load PAINT.UNO       Esc  back to the desktop
; ============================================================================

PT_CW   equ 32              ; canvas width in cells
PT_CH   equ 34              ; canvas height in cells
PT_Y0   equ 8              ; top pixel row of canvas row 0
PT_NINK equ 4

; tools
PTT_PEN   equ 0
PTT_RECT  equ 1
PTT_FRECT equ 2
PTT_FILL  equ 3

PAINTBUF equ KBSS+$1000     ; $A000-$A43F: 32x34 cell patterns (page-aligned)
PT_STK   equ KBSS+$1500     ; $A500.. flood-fill stack (PFX,PFY 256 each)
PFX      equ PT_STK         ; $A500
PFY      equ PT_STK+$100    ; $A600

; ink index -> hi-res pattern byte (white / 25% / 50% / black)
pt_inks: dc.b $7F,$11,$2A,$00
pt_inknames_lo: dc.b <pt_in0,<pt_in1,<pt_in2,<pt_in3
pt_inknames_hi: dc.b >pt_in0,>pt_in1,>pt_in2,>pt_in3
pt_in0: dc.b "White",0
pt_in1: dc.b "Lite ",0
pt_in2: dc.b "Gray ",0
pt_in3: dc.b "Black",0
pt_toolnames_lo: dc.b <pt_t0,<pt_t1,<pt_t2,<pt_t3
pt_toolnames_hi: dc.b >pt_t0,>pt_t1,>pt_t2,>pt_t3
pt_t0: dc.b "Pencil",0
pt_t1: dc.b "Rect  ",0
pt_t2: dc.b "Frect ",0
pt_t3: dc.b "Fill  ",0

; ----------------------------------------------------------------- pt_ptr
; pt_ptr - A=col, Y=row -> zpPB/+1 = PAINTBUF + row*32 + col.
pt_ptr:
        sta zpPBT               ; col
        tya
        sta zpPB
        lda #0
        sta zpPB+1
        asl zpPB
        rol zpPB+1              ; *2
        asl zpPB
        rol zpPB+1              ; *4
        asl zpPB
        rol zpPB+1              ; *8
        asl zpPB
        rol zpPB+1              ; *16
        asl zpPB
        rol zpPB+1              ; *32
        lda zpPB
        clc
        adc zpPBT               ; + col
        sta zpPB
        lda zpPB+1
        adc #0
        sta zpPB+1
        lda zpPB
        clc
        adc #<PAINTBUF
        sta zpPB
        lda zpPB+1
        adc #>PAINTBUF
        sta zpPB+1
        rts

; pt_getcell - A=col, Y=row -> A = PAINTBUF cell pattern.
pt_getcell:
        jsr pt_ptr
        ldy #0
        lda (zpPB),y
        rts

; pt_setcell - A=col, Y=row, X=pattern -> store into PAINTBUF.
pt_setcell:
        jsr pt_ptr
        txa
        ldy #0
        sta (zpPB),y
        rts

; pt_render_cell - draw cell (A=col, Y=row) on screen from PAINTBUF.
pt_render_cell:
        sta zpPBT               ; col
        sty zpPBT+1             ; row
        jsr pt_getcell
        sta zpFPat
        lda zpPBT
        sta zpFX
        lda zpPBT+1
        asl
        asl
        clc
        adc #PT_Y0
        sta zpFY
        lda #1
        sta zpFW
        lda #4
        sta zpFH
        jmp fill_rows

; pt_cursor - draw the cursor cell inverted (pattern EOR $7F) so it shows
; over any ink.
pt_cursor:
        lda pt_cx
        ldy pt_cy
        jsr pt_getcell
        eor #$7F
        sta zpFPat
        lda pt_cx
        sta zpFX
        lda pt_cy
        asl
        asl
        clc
        adc #PT_Y0
        sta zpFY
        lda #1
        sta zpFW
        lda #4
        sta zpFH
        jmp fill_rows

; pt_uncursor - restore the cursor cell from PAINTBUF.
pt_uncursor:
        lda pt_cx
        ldy pt_cy
        jmp pt_render_cell

; ----------------------------------------------------------------- pt_apply
; pt_apply - put the current ink in cell (A=col, Y=row), both in PAINTBUF
; and on screen.
pt_apply:
        sta zpPBT
        sty zpPBT+1
        ldx pt_ink
        lda pt_inks,x
        sta zpPBInk
        lda zpPBT
        ldy zpPBT+1
        ldx zpPBInk
        jsr pt_setcell
        lda zpPBT
        ldy zpPBT+1
        jmp pt_render_cell

; --------------------------------------------------------------- pt_fillrect
; pt_fillrect - fill the rectangle (pt_ax,pt_ay)-(pt_cx,pt_cy) inclusive
; with the current ink. outline = pt_outline flag (1 = border only).
pt_fillrect:
        ; normalise to x0<=x1, y0<=y1 in pt_rx0..pt_ry1
        lda pt_ax
        sta pt_rx0
        lda pt_cx
        sta pt_rx1
        lda pt_ax
        cmp pt_cx
        bcc pfr_xok
        lda pt_cx
        sta pt_rx0
        lda pt_ax
        sta pt_rx1
pfr_xok:
        lda pt_ay
        sta pt_ry0
        lda pt_cy
        sta pt_ry1
        lda pt_ay
        cmp pt_cy
        bcc pfr_yok
        lda pt_cy
        sta pt_ry0
        lda pt_ay
        sta pt_ry1
pfr_yok:
        lda pt_ry0
        sta pt_ry                ; row iterator
pfr_row:
        lda pt_rx0
        sta pt_rx                ; col iterator
pfr_col:
        ; outline mode: only edges
        lda pt_outline
        beq pfr_put
        ; on edge if row==ry0/ry1 or col==rx0/rx1
        lda pt_ry
        cmp pt_ry0
        beq pfr_put
        cmp pt_ry1
        beq pfr_put
        lda pt_rx
        cmp pt_rx0
        beq pfr_put
        cmp pt_rx1
        beq pfr_put
        jmp pfr_next
pfr_put:
        lda pt_rx
        ldy pt_ry
        jsr pt_apply
pfr_next:
        inc pt_rx
        lda pt_rx
        cmp pt_rx1
        bcc pfr_col
        beq pfr_col
        inc pt_ry
        lda pt_ry
        cmp pt_ry1
        bcc pfr_row
        beq pfr_row
        rts

; --------------------------------------------------------------- pt_flood
; pt_flood - 4-connected flood fill from (pt_cx,pt_cy): replace the target
; pattern with the current ink. Stack-bounded (256 cells); on overflow it
; just stops pushing (partial fill - documented cap).
pt_flood:
        lda pt_cx
        ldy pt_cy
        jsr pt_getcell
        sta pt_target           ; pattern to replace
        ldx pt_ink
        lda pt_inks,x
        cmp pt_target
        bne pf_start            ; ink == target: nothing to do
        rts
pf_start:
        lda #0
        sta pt_sp
        ; push start
        lda pt_cx
        ldx pt_sp
        sta PFX,x
        lda pt_cy
        sta PFY,x
        inc pt_sp
pf_loop:
        lda pt_sp
        beq pf_done
        dec pt_sp
        ldx pt_sp
        lda PFX,x
        sta pt_px
        lda PFY,x
        sta pt_py
        ; cell still the target?
        lda pt_px
        ldy pt_py
        jsr pt_getcell
        cmp pt_target
        bne pf_loop
        ; paint it
        lda pt_px
        ldy pt_py
        jsr pt_apply
        ; push 4 neighbours (bounds-checked)
        lda pt_px               ; left
        beq pf_right
        sec
        sbc #1
        ldy pt_py
        jsr pf_push
pf_right:
        lda pt_px
        cmp #(PT_CW-1)
        bcs pf_up
        clc
        adc #1
        ldy pt_py
        jsr pf_push
pf_up:
        lda pt_py
        beq pf_down
        ; A=col=px, Y=py-1
        ldy pt_py
        dey
        lda pt_px
        jsr pf_push
pf_down:
        lda pt_py
        cmp #(PT_CH-1)
        bcs pf_cont
        ldy pt_py
        iny
        lda pt_px
        jsr pf_push
pf_cont:
        jmp pf_loop
pf_done:
        rts

; pf_push - push (A=col, Y=row) if the stack has room and the cell is the
; target pattern.
pf_push:
        sta zpPBT               ; col
        sty zpPBT+1             ; row
        jsr pt_getcell          ; A = cell at (col,row)
        cmp pt_target
        bne pfp_skip
        lda pt_sp
        cmp #255
        bcs pfp_skip            ; stack full: drop (partial fill)
        ldx pt_sp
        lda zpPBT
        sta PFX,x
        lda zpPBT+1
        sta PFY,x
        inc pt_sp
pfp_skip:
        rts

; ----------------------------------------------------------------- pt_status
; pt_status - redraw the status line (tool + ink) at row 21.
pt_status:
        lda #0
        sta zpFX
        lda #168
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #8
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows
        lda #0
        sta zpCol
        lda #21
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_pt_tool
        sta zpPtr
        lda #>msg_pt_tool
        sta zpPtr+1
        jsr draw_string
        ldx pt_tool
        lda pt_toolnames_lo,x
        sta zpPtr
        lda pt_toolnames_hi,x
        sta zpPtr+1
        jsr draw_string
        lda #16
        sta zpCol
        lda #<msg_pt_ink
        sta zpPtr
        lda #>msg_pt_ink
        sta zpPtr+1
        jsr draw_string
        ldx pt_ink
        lda pt_inknames_lo,x
        sta zpPtr
        lda pt_inknames_hi,x
        sta zpPtr+1
        jmp draw_string

; ----------------------------------------------------------------- paint_draw
; paint_draw - full redraw: title, separator, all canvas cells, status,
; help, cursor.
paint_draw:
        lda #0
        sta zpFX
        lda #0
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #8
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows
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
        ; separator
        lda #0
        sta zpFX
        lda #7
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #1
        sta zpFH
        lda #$7F
        sta zpFPat
        jsr fill_rows
        ; clear content + status region
        lda #0
        sta zpFX
        lda #APP_CONTENT_Y
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #(APP_CONTENT_H+8)
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows
        ; render all canvas cells
        lda #0
        sta pt_py
pd_row:
        lda #0
        sta pt_px
pd_col:
        lda pt_px
        ldy pt_py
        jsr pt_render_cell
        inc pt_px
        lda pt_px
        cmp #PT_CW
        bne pd_col
        inc pt_py
        lda pt_py
        cmp #PT_CH
        bne pd_row
        ; status + help
        jsr pt_status
        lda #0
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_pt_help
        sta zpPtr
        lda #>msg_pt_help
        sta zpPtr+1
        jsr draw_string
        jmp pt_cursor

; ----------------------------------------------------------------- pt_clearbuf
; pt_clearbuf - fill PAINTBUF with ink 0 (white).
pt_clearbuf:
        lda pt_inks             ; ink 0 pattern
        ldx #0
ptc_lo:
        sta PAINTBUF,x
        sta PAINTBUF+$100,x
        sta PAINTBUF+$200,x
        sta PAINTBUF+$300,x
        inx
        bne ptc_lo
        ; tail $A400..$A43F (1088 = $440 bytes)
        ldx #0
ptc_tail:
        sta PAINTBUF+$400,x
        inx
        cpx #$40
        bne ptc_tail
        rts

; ============================================================================
; open / close / keys
; ============================================================================

paint_open:
        lda #8
        sta app_mode
        jsr beep_click
        lda #16
        sta pt_cx               ; centre-ish cursor
        lda #16
        sta pt_cy
        lda #PTT_PEN
        sta pt_tool
        lda #3
        sta pt_ink              ; default pen: black
        lda #0
        sta pt_anchor
        jsr pt_clearbuf
        jmp paint_draw

paint_close:
        lda #0
        sta app_mode
        jsr draw_desktop
        jsr draw_sysinfo_win
        jsr draw_clock_win
        jmp draw_icons

; paint_key - route a key while Paint is active (zpTmp = key code).
paint_key:
        lda zpTmp
        cmp #$9B                ; ESC
        bne pk_1
        jmp paint_close
pk_nret:                        ; near rts for the bounded movement handlers
        rts
pk_1:
        cmp #$8B                ; up
        bne pk_2
        lda pt_cy
        beq pk_nret
        jsr pt_uncursor
        dec pt_cy
        jmp pk_cur
pk_2:
        cmp #$8A                ; down
        bne pk_3
        lda pt_cy
        cmp #(PT_CH-1)
        bcs pk_nret
        jsr pt_uncursor
        inc pt_cy
        jmp pk_cur
pk_3:
        cmp #$88                ; left
        bne pk_4
        lda pt_cx
        beq pk_nret
        jsr pt_uncursor
        dec pt_cx
        jmp pk_cur
pk_4:
        cmp #$95                ; right
        bne pk_5
        lda pt_cx
        cmp #(PT_CW-1)
        bcs pk_nret
        jsr pt_uncursor
        inc pt_cx
        jmp pk_cur
pk_5:
        cmp #$A0                ; space: apply
        bne pk_6
        jmp pt_do_apply
pk_6:
        cmp #$D0                ; 'P' pencil
        bne pk_7
        lda #PTT_PEN
        jmp pk_settool
pk_7:
        cmp #$D2                ; 'R' rect
        bne pk_8
        lda #PTT_RECT
        jmp pk_settool
pk_8:
        cmp #$C6                ; 'F' fill
        bne pk_9
        lda #PTT_FILL
        jmp pk_settool
pk_9:
        cmp #$C9                ; 'I' frect (filled rect)
        bne pk_10
        lda #PTT_FRECT
        jmp pk_settool
pk_10:
        cmp #$DD                ; ']' next ink
        bne pk_11
        lda pt_ink
        clc
        adc #1
        cmp #PT_NINK
        bcc pk_inkset
        lda #0
        jmp pk_inkset
pk_11:
        cmp #$DB                ; '[' prev ink
        bne pk_12
        lda pt_ink
        bne pk_inkdec
        lda #PT_NINK
pk_inkdec:
        sec
        sbc #1
        jmp pk_inkset
pk_12:
        cmp #$CE                ; 'N' clear canvas
        bne pk_13
        jsr pt_clearbuf
        lda #0
        sta pt_anchor
        jmp paint_draw
pk_13:
        cmp #$D3                ; 'S' save
        bne pk_14
        jmp pt_save
pk_14:
        cmp #$CC                ; 'L' load
        bne pk_ret
        jmp pt_load
pk_ret:
        rts
pk_cur:
        jmp pt_cursor
pk_settool:
        sta pt_tool
        lda #0
        sta pt_anchor           ; switching tools drops any pending anchor
        jsr pt_status
        jmp pt_cursor
pk_inkset:
        sta pt_ink
        jsr pt_status
        jmp pt_cursor

; pt_do_apply - Space: behaviour depends on the tool.
pt_do_apply:
        lda pt_tool
        cmp #PTT_PEN
        bne pda_chkfill
        lda pt_cx
        ldy pt_cy
        jsr pt_apply
        jmp pt_cursor
pda_chkfill:
        cmp #PTT_FILL
        bne pda_shape
        jsr pt_flood
        jmp pt_cursor
pda_shape:
        ; rect / frect: first Space sets the anchor, second draws
        lda pt_anchor
        bne pda_draw
        lda pt_cx
        sta pt_ax
        lda pt_cy
        sta pt_ay
        lda #1
        sta pt_anchor
        jmp pt_cursor           ; anchor marked; cursor shows position
pda_draw:
        lda #0
        sta pt_anchor
        lda pt_tool
        cmp #PTT_RECT
        bne pda_filled
        lda #1
        sta pt_outline
        jmp pda_go
pda_filled:
        lda #0
        sta pt_outline
pda_go:
        jsr pt_fillrect
        jmp pt_cursor

; ----------------------------------------------------------------- pt_save
pt_save:
        lda #<pt_fname
        sta zpFSName
        lda #>pt_fname
        sta zpFSName+1
        lda #<PAINTBUF
        sta zpFSDat
        lda #>PAINTBUF
        sta zpFSDat+1
        lda #<(PT_CW*PT_CH)
        sta zpFSSize
        lda #>(PT_CW*PT_CH)
        sta zpFSSize+1
        jsr fs_save
        jsr beep_click
        jmp pt_cursor

; ----------------------------------------------------------------- pt_load
pt_load:
        lda #<pt_fname
        sta zpFSName
        lda #>pt_fname
        sta zpFSName+1
        jsr fs_find
        cmp #$FF
        beq ptl_none
        pha
        lda #<PAINTBUF
        sta zpFSDat
        lda #>PAINTBUF
        sta zpFSDat+1
        lda #5                  ; PAINTBUF = 1088 B = 5 sectors (fs_read cap)
        sta zpFSTmp
        pla
        jsr fs_read
        jsr beep_click
        jmp paint_draw
ptl_none:
        jmp pt_cursor

pt_fname:      dc.b "PAINT.UNO",0,0,0
msg_pt_title:  dc.b "Paint",0
msg_pt_tool:   dc.b "Tool:",0
msg_pt_ink:    dc.b "Ink:",0
msg_pt_help:   dc.b "Arrows Spc=draw PRFI=tool []=ink N=clr SL=file",0
