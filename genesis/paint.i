; ============================================================================
; UnoDOS/Genesis Paint (proc 10) - the shared MacPaint-style editor on
; VDP tiles. The canvas is a region of UNIQUE tiles (one per cell), so
; it behaves like a bitmap inside the cell desktop: a byte-per-pixel
; backing store lives in the spare work-RAM bank at $FF0000 and dirty
; tiles are re-packed to VRAM after every stroke step.
;
; Tools: pencil, brush, eraser, line, rect, filled rect, oval, filled
; oval, flood fill, spray - same set as the Mac/Amiga editors.
;
; Color selector = the platform's full gamut: the strip shows the 15
; usable entries of palette line 3; r/g/b tune the SELECTED entry's
; 3-bit channels live in CRAM, so all 512 Genesis colors are reachable
; (entries 5-15; 1-4 carry the UI scheme and stay locked). Closing
; Paint restores the line-3 defaults (OutLast scenery + soft keyboard).
;
; The canvas is 160x96 (20x12 cells, 240 tiles at T_PTCAN); a 2KB-class
; Notepad page it is not - saving goes to TAPE ('t' write / 'y' read,
; ~2.5 minutes of audio for the 15KB canvas). SRAM (8KB) and the
; internal Sega CD BRAM (125 blocks) are too small for a full canvas.
;
;   1..9,0   select tool          [ / ]   previous / next pen
;   r/g/b    tune the pen color   n       clear   t/y  tape save/load
; ============================================================================

PT_W        equ 160
PT_H        equ 96
PT_TW       equ 20                  ; canvas width in tiles
PT_TH       equ 12
PT_TOOLS    equ 10
PT_BGPEN    equ 15                  ; background = line-3 black
PT_STKN     equ 500                 ; flood stack entries
T_PTCAN     equ 256                 ; first canvas tile (240 used)
PTCANVAS    equ $FF0000             ; byte-per-pixel backing store

PTT_PENCIL  equ 0
PTT_BRUSH   equ 1
PTT_ERASER  equ 2
PTT_LINE    equ 3
PTT_RECT    equ 4
PTT_FRECT   equ 5
PTT_OVAL    equ 6
PTT_FOVAL   equ 7
PTT_FILL    equ 8
PTT_SPRAY   equ 9

; ---------------------------------------------------------------- geometry
; pt_origin - a2 = window -> d0/d1 = canvas origin in PIXELS
pt_origin:
        move.w  WX(a2),d0
        addq.w  #4,d0
        lsl.w   #3,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        lsl.w   #3,d1
        rts

; ---------------------------------------------------------------- canvas
; pt_px - d0/d1 = canvas coords, d2 = pen: store + grow the dirty rect.
; Preserves everything; clips.
pt_px:
        movem.l d0-d3/a0/a4,-(sp)
        tst.w   d0
        bmi     .out
        tst.w   d1
        bmi     .out
        cmp.w   #PT_W,d0
        bge     .out
        cmp.w   #PT_H,d1
        bge     .out
        lea     VARS,a4
        move.w  d1,d3
        mulu    #PT_W,d3
        add.w   d0,d3
        lea     PTCANVAS,a0
        move.b  d2,(a0,d3.l)
        ; dirty rect (tile coords)
        move.w  d0,d3
        lsr.w   #3,d3               ; tx
        cmp.w   v_pt_dx0(a4),d3
        bge     .dx1
        move.w  d3,v_pt_dx0(a4)
.dx1:   cmp.w   v_pt_dx1(a4),d3
        ble     .dy0
        move.w  d3,v_pt_dx1(a4)
.dy0:   move.w  d1,d3
        lsr.w   #3,d3               ; ty
        cmp.w   v_pt_dy0(a4),d3
        bge     .dy1
        move.w  d3,v_pt_dy0(a4)
.dy1:   cmp.w   v_pt_dy1(a4),d3
        ble     .out
        move.w  d3,v_pt_dy1(a4)
.out:   movem.l (sp)+,d0-d3/a0/a4
        rts

; pt_get - (d0,d1) -> d2 = canvas pen (clipped reads return bg)
pt_get:
        movem.l d3/a0,-(sp)
        moveq   #PT_BGPEN,d2
        tst.w   d0
        bmi     .out
        tst.w   d1
        bmi     .out
        cmp.w   #PT_W,d0
        bge     .out
        cmp.w   #PT_H,d1
        bge     .out
        move.w  d1,d3
        mulu    #PT_W,d3
        add.w   d0,d3
        lea     PTCANVAS,a0
        moveq   #0,d2
        move.b  (a0,d3.l),d2
.out:   movem.l (sp)+,d3/a0
        rts

; pt_dirty_reset - empty dirty rect
pt_dirty_reset:
        move.l  a4,-(sp)
        lea     VARS,a4
        move.w  #PT_TW,v_pt_dx0(a4)
        move.w  #-1,v_pt_dx1(a4)
        move.w  #PT_TH,v_pt_dy0(a4)
        move.w  #-1,v_pt_dy1(a4)
        move.l  (sp)+,a4
        rts

; pt_dirty_all - mark everything dirty
pt_dirty_all:
        move.l  a4,-(sp)
        lea     VARS,a4
        clr.w   v_pt_dx0(a4)
        move.w  #PT_TW-1,v_pt_dx1(a4)
        clr.w   v_pt_dy0(a4)
        move.w  #PT_TH-1,v_pt_dy1(a4)
        move.l  (sp)+,a4
        rts

; pt_sync - repack the dirty tiles from the backing store into VRAM.
; Tile (tx,ty) -> VRAM (T_PTCAN + ty*PT_TW + tx)*32; each row packs 8
; canvas bytes into 4 nibble-pair bytes (2 words through the data port).
pt_sync:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     VARS,a4
        move.w  v_pt_dy0(a4),d7     ; ty
.ty:    cmp.w   v_pt_dy1(a4),d7
        bgt     .done
        move.w  v_pt_dx0(a4),d6     ; tx
.tx:    cmp.w   v_pt_dx1(a4),d6
        bgt     .nty
        ; VRAM address for this tile
        move.w  d7,d0
        mulu    #PT_TW,d0
        add.w   d6,d0
        add.w   #T_PTCAN,d0
        lsl.l   #5,d0               ; *32 bytes
        ; control = ((addr&$3FFF)<<16) | $40000000 | (addr>>14)
        move.l  d0,d1
        and.l   #$3FFF,d0
        swap    d0
        or.l    #$40000000,d0
        move.l  d1,d3
        moveq   #14,d4
        lsr.l   d4,d3
        and.l   #3,d3
        or.l    d3,d0
        move.l  d0,VDP_CTRL
        ; source: canvas byte (tx*8, ty*8)
        move.w  d7,d0
        lsl.w   #3,d0               ; py
        mulu    #PT_W,d0
        move.w  d6,d1
        lsl.w   #3,d1
        add.w   d1,d0               ; offset
        lea     PTCANVAS,a0
        lea     (a0,d0.l),a0
        lea     VDP_DATA,a1
        moveq   #7,d5               ; 8 rows
.row:   moveq   #0,d0               ; pack 4 pixels -> word, twice
        moveq   #3,d4
.p1:    lsl.w   #4,d0
        move.b  (a0)+,d1
        and.w   #15,d1
        or.w    d1,d0
        dbra    d4,.p1
        move.w  d0,(a1)
        moveq   #0,d0
        moveq   #3,d4
.p2:    lsl.w   #4,d0
        move.b  (a0)+,d1
        and.w   #15,d1
        or.w    d1,d0
        dbra    d4,.p2
        move.w  d0,(a1)
        lea     PT_W-8(a0),a0       ; next canvas row
        dbra    d5,.row
        addq.w  #1,d6
        bra     .tx
.nty:   addq.w  #1,d7
        bra     .ty
.done:  bsr     pt_dirty_reset
        movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; pt_put - plot (d0,d1) with the current pen. Preserves everything.
pt_put:
        move.w  d2,-(sp)
        move.w  VARS+v_pt_pen,d2
        bsr     pt_px
        move.w  (sp)+,d2
        rts

; ---------------------------------------------------------------- shapes
; (same algorithms as the Amiga editor, RAM-canvas only - pt_sync
; flushes after the whole shape)

; pt_dot - d0/d1 center, d3 = size, d4 = 0 ink / 1 erase
pt_dot:
        movem.l d0-d7,-(sp)
        move.w  VARS+v_pt_pen,d2
        tst.w   d4
        beq     .ink
        moveq   #PT_BGPEN,d2
.ink:   move.w  d0,d5
        move.w  d1,d6
        move.w  d3,d7
        lsr.w   #1,d7
        sub.w   d7,d5
        sub.w   d7,d6
        moveq   #0,d7
.row:   cmp.w   d3,d7
        bge     .done
        moveq   #0,d4
.col:   cmp.w   d3,d4
        bge     .nrow
        move.w  d5,d0
        add.w   d4,d0
        move.w  d6,d1
        add.w   d7,d1
        bsr     pt_px
        addq.w  #1,d4
        bra     .col
.nrow:  addq.w  #1,d7
        bra     .row
.done:  movem.l (sp)+,d0-d7
        rts

; pt_line_seg - (d0,d1)->(d2,d3), dot size d4
pt_line_seg:
        movem.l d0-d7/a4,-(sp)
        lea     VARS,a4
        move.w  d4,v_pt_lsz(a4)
        moveq   #1,d6
        move.w  d2,d4
        sub.w   d0,d4
        bge     .dxok
        neg.w   d4
        moveq   #-1,d6
.dxok:  moveq   #1,d7
        move.w  d3,d5
        sub.w   d1,d5
        bge     .dyok
        neg.w   d5
        moveq   #-1,d7
.dyok:  neg.w   d5
        move.w  d4,v_pt_ldx(a4)
        add.w   d5,d4
        move.w  d4,v_pt_err(a4)
.loop:  move.w  d3,-(sp)
        move.w  d4,-(sp)
        move.w  v_pt_lsz(a4),d3
        moveq   #0,d4
        bsr     pt_dot
        move.w  (sp)+,d4
        move.w  (sp)+,d3
        cmp.w   d0,d2
        bne     .cont
        cmp.w   d1,d3
        beq     .done
.cont:  move.w  v_pt_err(a4),d4
        add.w   d4,d4               ; e2 = 2*err - ONE value for BOTH
        cmp.w   d5,d4               ; tests (recomputing after the x
        blt     .ny                 ; step desyncs the walk and can
        add.w   d5,v_pt_err(a4)     ; step over the endpoint forever)
        add.w   d6,d0
.ny:    cmp.w   v_pt_ldx(a4),d4
        bgt     .loop
        move.w  v_pt_ldx(a4),d4
        add.w   d4,v_pt_err(a4)
        add.w   d7,d1
        bra     .loop
.done:  movem.l (sp)+,d0-d7/a4
        rts

; pt_norm - order (d0,d1)-(d2,d3)
pt_norm:
        cmp.w   d0,d2
        bge     .x
        exg     d0,d2
.x:     cmp.w   d1,d3
        bge     .y
        exg     d1,d3
.y:     rts

; pt_rect_shape - (d0,d1)-(d2,d3), d4 = 0 frame / 1 filled
pt_rect_shape:
        movem.l d0-d7,-(sp)
        bsr     pt_norm
        move.w  d4,d7
        move.w  d1,d6
.row:   cmp.w   d3,d6
        bgt     .done
        tst.w   d7
        bne     .full
        cmp.w   d1,d6
        beq     .full
        cmp.w   d3,d6
        bne     .sides
.full:  movem.l d0-d1,-(sp)
        move.w  d6,d1
.px:    bsr     pt_put
        addq.w  #1,d0
        cmp.w   d2,d0
        ble     .px
        movem.l (sp)+,d0-d1
        bra     .next
.sides: movem.l d0-d1,-(sp)
        move.w  d6,d1
        bsr     pt_put
        move.w  d2,d0
        bsr     pt_put
        movem.l (sp)+,d0-d1
.next:  addq.w  #1,d6
        bra     .row
.done:  movem.l (sp)+,d0-d7
        rts

; pt_oval_shape - bounding box (d0,d1)-(d2,d3), d4 = 0 frame / 1 filled
pt_oval_shape:
        movem.l d0-d7/a4,-(sp)
        lea     VARS,a4
        bsr     pt_norm
        move.w  d4,v_pt_lsz(a4)
        move.w  d2,d4
        sub.w   d0,d4
        lsr.w   #1,d4               ; a
        move.w  d3,d5
        sub.w   d1,d5
        lsr.w   #1,d5               ; b
        tst.w   d4
        beq     .degen
        tst.w   d5
        bne     .ok
.degen: move.w  v_pt_lsz(a4),d4
        bsr     pt_rect_shape
        bra     .out
.ok:    move.w  d0,d6
        add.w   d4,d6               ; cx
        move.w  d1,d7
        add.w   d5,d7               ; cy
        move.w  d1,d2               ; row
.rw:    cmp.w   d3,d2
        bgt     .out
        move.w  d2,d0
        sub.w   d7,d0
        muls    d0,d0               ; dy^2
        move.w  d4,d1
        mulu    d1,d1               ; a^2
        move.l  d1,-(sp)
        mulu    d1,d0               ; a^2*dy^2
        move.w  d5,d1
        mulu    d1,d1               ; b^2 (word)
        divu    d1,d0
        and.l   #$FFFF,d0
        move.l  (sp)+,d1
        sub.l   d0,d1               ; h^2
        bpl     .h2
        moveq   #0,d1
.h2:    moveq   #0,d0
.sq:    move.w  d0,v_pt_err(a4)
        addq.w  #1,d0
        mulu    d0,d0
        cmp.l   d1,d0
        bhi     .goth
        move.w  v_pt_err(a4),d0
        addq.w  #1,d0
        bra     .sq
.goth:  move.w  v_pt_err(a4),d0
        bsr     pt_oval_row
        addq.w  #1,d2
        bra     .rw
.out:   movem.l (sp)+,d0-d7/a4
        rts

; pt_oval_row - d0 = h, d2 = row, d6 = cx, v_pt_lsz = filled
pt_oval_row:
        movem.l d0-d5,-(sp)
        move.w  d6,d3
        sub.w   d0,d3
        move.w  d6,d4
        add.w   d0,d4
        tst.w   VARS+v_pt_lsz
        beq     .frame
        move.w  d3,d0
        move.w  d2,d1
.px:    bsr     pt_put
        addq.w  #1,d0
        cmp.w   d4,d0
        ble     .px
        bra     .done
.frame: move.w  d3,d0
        move.w  d2,d1
        bsr     pt_put
        move.w  d4,d0
        bsr     pt_put
.done:  movem.l (sp)+,d0-d5
        rts

; pt_flood - scanline flood fill from (d0,d1) with the current pen
pt_flood:
        movem.l d0-d7/a0-a1,-(sp)
        bsr     pt_get
        move.w  d2,d7               ; from pen
        cmp.w   VARS+v_pt_pen,d7
        beq     .done
        lea     VARS+v_pt_stk,a1
        move.w  d0,(a1)
        move.w  d1,2(a1)
        moveq   #1,d6               ; stack count
.pop:   tst.w   d6
        beq     .done
        subq.w  #1,d6
        move.w  d6,d3
        lsl.w   #2,d3
        move.w  (a1,d3.w),d0
        move.w  2(a1,d3.w),d1
        bsr     pt_get
        cmp.w   d7,d2
        bne     .pop
.l:     tst.w   d0
        beq     .lend
        subq.w  #1,d0
        bsr     pt_get
        cmp.w   d7,d2
        beq     .l
        addq.w  #1,d0
.lend:
.r:     cmp.w   #PT_W,d0
        bge     .pop
        bsr     pt_get
        cmp.w   d7,d2
        bne     .pop
        bsr     pt_put
        ; queue up/down neighbours that still hold the target pen
        tst.w   d1
        beq     .ndn
        subq.w  #1,d1
        bsr     pt_get
        cmp.w   d7,d2
        bne     .nup
        cmp.w   #PT_STKN,d6
        bge     .nup
        move.w  d6,d3
        lsl.w   #2,d3
        move.w  d0,(a1,d3.w)
        move.w  d1,2(a1,d3.w)
        addq.w  #1,d6
.nup:   addq.w  #1,d1
.ndn:   cmp.w   #PT_H-1,d1
        bge     .nx
        addq.w  #1,d1
        bsr     pt_get
        cmp.w   d7,d2
        bne     .ndn2
        cmp.w   #PT_STKN,d6
        bge     .ndn2
        move.w  d6,d3
        lsl.w   #2,d3
        move.w  d0,(a1,d3.w)
        move.w  d1,2(a1,d3.w)
        addq.w  #1,d6
.ndn2:  subq.w  #1,d1
.nx:    addq.w  #1,d0
        bra     .r
.done:  movem.l (sp)+,d0-d7/a0-a1
        rts

; pt_rand -> d0.w LFSR
pt_rand:
        movem.l a4,-(sp)
        lea     VARS,a4
        move.w  v_pt_rnd(a4),d0
        lsl.w   #1,d0
        bcc     .nx
        eor.w   #$1D87,d0
.nx:    bne     .ok
        move.w  #$ACE1,d0
.ok:    move.w  d0,v_pt_rnd(a4)
        movem.l (sp)+,a4
        rts

; pt_clear - canvas to background + full sync
pt_clear:
        movem.l d0-d1/a0,-(sp)
        lea     PTCANVAS,a0
        move.l  #(PT_W*PT_H/4)-1,d0
        move.l  #$0F0F0F0F,d1       ; PT_BGPEN in every byte
.cl:    move.l  d1,(a0)+
        subq.l  #1,d0
        bpl     .cl
        bsr     pt_dirty_all
        bsr     pt_sync
        movem.l (sp)+,d0-d1/a0
        rts

; ---------------------------------------------------------------- drawing
; paint_draw - a2 = window: chrome + canvas name-table cells
paint_draw:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     VARS,a4
        tst.b   v_pt_init(a4)
        bne     .inited
        st      v_pt_init(a4)
        move.w  #3,v_pt_pen(a4)     ; start on white
        bsr     pt_pal_init
        bsr     pt_clear
.inited:
        ; tool cells: column of 10 digits at wx+1, two cells wide hit box
        moveq   #0,d7
.tool:  cmp.w   #PT_TOOLS,d7
        bge     .canvas
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        add.w   d7,d1
        move.w  d7,d2
        addq.w  #1,d2
        cmp.w   #10,d2
        bne     .dig
        moveq   #0,d2
.dig:   add.w   #'0',d2
        move.w  #ATTR_NORM,d4
        cmp.w   v_pt_tool(a4),d7
        bne     .glyph
        move.w  #ATTR_INV,d4
.glyph: bsr     draw_char
        addq.w  #1,d7
        bra     .tool
.canvas:
        ; canvas cells: ATTR_KEY + unique tiles
        moveq   #0,d6               ; ty
.cy:    cmp.w   #PT_TH,d6
        bge     .strip
        moveq   #0,d5               ; tx
.cx:    cmp.w   #PT_TW,d5
        bge     .ncy
        move.w  WX(a2),d0
        addq.w  #4,d0
        add.w   d5,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        add.w   d6,d1
        bsr     cell_addr
        move.w  d6,d2
        mulu    #PT_TW,d2
        add.w   d5,d2
        add.w   #T_PTCAN+ATTR_KEY,d2
        move.w  d2,VDP_DATA
        addq.w  #1,d5
        bra     .cx
.ncy:   addq.w  #1,d6
        bra     .cy
.strip: ; pen swatches: 15 cells on row wy+h-3
        moveq   #1,d7               ; pen 1..15
.pen:   cmp.w   #16,d7
        bge     .foot
        move.w  WX(a2),d0
        addq.w  #4,d0
        add.w   d7,d0
        subq.w  #1,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #3,d1
        bsr     pt_pen_tile         ; d7 -> d2 = tile
        move.w  #ATTR_KEY,d4
        add.w   d2,d4
        move.w  d4,d2
        bsr     cell_addr
        move.w  d2,VDP_DATA
        ; selection marker: '^' under the active pen
        moveq   #' ',d2
        cmp.w   v_pt_pen(a4),d7
        bne     .mark
        moveq   #'^',d2
.mark:  addq.w  #1,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_char
        addq.w  #1,d7
        bra     .pen
.foot:  lea     str_pt_foot(pc),a0
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #2,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        bsr     pt_sync
        movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; pt_pen_tile - d7 = pen 1..15 -> d2 = solid tile for that line-3 index
pt_pen_tile:
        cmp.w   #5,d7
        blt     .ui
        move.w  d7,d2
        add.w   #T_GSOL-5,d2        ; solids for indexes 5..15
        rts
.ui:    cmp.w   #1,d7
        bne     .n1
        moveq   #T_SOLFG,d2
        rts
.n1:    cmp.w   #2,d7
        bne     .n2
        moveq   #T_SOLBG,d2
        rts
.n2:    cmp.w   #3,d7
        bne     .n3
        moveq   #T_SOLCY,d2
        rts
.n3:    moveq   #T_SOLMG,d2
        rts

; ---------------------------------------------------------------- input
; paint_click - d0/d1 = click CELL coords, a2 = topmost window
paint_click:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     VARS,a4
        move.w  d0,d5               ; cell x/y
        move.w  d1,d6
        ; tool column?
        move.w  WX(a2),d2
        addq.w  #1,d2
        cmp.w   d2,d5
        bne     .npen
        move.w  WY(a2),d2
        addq.w  #1,d2
        move.w  d6,d3
        sub.w   d2,d3
        bmi     .npen
        cmp.w   #PT_TOOLS,d3
        bge     .npen
        move.w  d3,v_pt_tool(a4)
        bsr     redraw_topmost
        bra     .out
.npen:  ; pen strip?
        move.w  WY(a2),d2
        add.w   WH(a2),d2
        subq.w  #3,d2
        cmp.w   d2,d6
        bne     .ncv
        move.w  WX(a2),d2
        addq.w  #3,d2
        move.w  d5,d3
        sub.w   d2,d3               ; pen index
        ble     .ncv
        cmp.w   #16,d3
        bge     .ncv
        move.w  d3,v_pt_pen(a4)
        bsr     redraw_topmost
        bra     .out
.ncv:   ; canvas: convert the PIXEL click latch to canvas coords
        bsr     pt_origin           ; d0/d1 = origin px
        move.w  v_click_x(a4),d2
        sub.w   d0,d2
        move.w  v_click_y(a4),d3
        sub.w   d1,d3
        tst.w   d2
        bmi     .out
        tst.w   d3
        bmi     .out
        cmp.w   #PT_W,d2
        bge     .out
        cmp.w   #PT_H,d3
        bge     .out
        move.w  d2,d0
        move.w  d3,d1
        move.w  v_pt_tool(a4),d4
        cmp.w   #PTT_FILL,d4
        bne     .nfill
        bsr     pt_flood
        bsr     pt_sync
        bra     .out
.nfill: cmp.w   #PTT_LINE,d4
        blt     .free
        cmp.w   #PTT_FOVAL,d4
        bgt     .free
        bsr     pt_drag_shape
        bra     .out
.free:  bsr     pt_drag_free
.out:   movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; pt_canvas_mouse -> d0/d1 = cursor in canvas coords (clamped),
; d2 = button. a2 = window, a4 = VARS.
pt_canvas_mouse:
        bsr     pt_origin
        move.w  d0,d2
        move.w  d1,d3
        move.w  v_mouse_x(a4),d0
        sub.w   d2,d0
        move.w  v_mouse_y(a4),d1
        sub.w   d3,d1
        tst.w   d0
        bge     .x0
        moveq   #0,d0
.x0:    cmp.w   #PT_W-1,d0
        ble     .x1
        move.w  #PT_W-1,d0
.x1:    tst.w   d1
        bge     .y0
        moveq   #0,d1
.y0:    cmp.w   #PT_H-1,d1
        ble     .y1
        move.w  #PT_H-1,d1
.y1:    moveq   #0,d2
        move.b  v_mouse_btn(a4),d2
        rts

; pt_drag_free - d0/d1 = start: pencil/brush/eraser/spray until release
pt_drag_free:
        movem.l d0-d7,-(sp)
        move.w  d0,d5
        move.w  d1,d6
.lp:    move.w  v_pt_tool(a4),d4
        cmp.w   #PTT_SPRAY,d4
        beq     .spray
        moveq   #1,d3
        cmp.w   #PTT_BRUSH,d4
        bne     .nb
        moveq   #3,d3
.nb:    cmp.w   #PTT_ERASER,d4
        bne     .ne
        moveq   #6,d3
.ne:    moveq   #0,d7
        cmp.w   #PTT_ERASER,d4
        bne     .seg
        moveq   #1,d7
.seg:   move.w  d3,-(sp)
        move.w  d0,d2
        move.w  d1,d3
        move.w  d5,d0
        move.w  d6,d1
        move.w  (sp)+,d4
        tst.w   d7
        beq     .ink
        move.w  v_pt_pen(a4),-(sp)
        move.w  #PT_BGPEN,v_pt_pen(a4)
        bsr     pt_line_seg
        move.w  (sp)+,v_pt_pen(a4)
        bra     .segd
.ink:   bsr     pt_line_seg
.segd:  move.w  d2,d5
        move.w  d3,d6
        bra     .next
.spray: moveq   #5,d7
.sp:    bsr     pt_rand
        move.w  d0,d2
        and.w   #15,d2
        subq.w  #8,d2
        bsr     pt_rand
        move.w  d0,d3
        and.w   #15,d3
        subq.w  #8,d3
        move.w  d5,d0
        add.w   d2,d0
        move.w  d6,d1
        add.w   d3,d1
        bsr     pt_put
        dbra    d7,.sp
.next:  bsr     pt_sync             ; flush this step's tiles
        bsr     pt_canvas_mouse
        tst.w   d2
        beq     .done
        move.w  v_pt_tool(a4),d4
        cmp.w   #PTT_SPRAY,d4
        bne     .lp
        move.w  d0,d5
        move.w  d1,d6
        bra     .lp
.done:  movem.l (sp)+,d0-d7
        rts

; pt_drag_shape - d0/d1 = anchor: track until release, then commit.
; (No rubber band on the tile canvas - the shape commits on release.)
pt_drag_shape:
        movem.l d0-d7,-(sp)
        move.w  d0,d5
        move.w  d1,d6
.lp:    bsr     pt_canvas_mouse
        tst.w   d2
        bne     .lp
        ; release: commit anchor (d5,d6) -> (d0,d1)
        move.w  d0,d2
        move.w  d1,d3
        move.w  d5,d0
        move.w  d6,d1
        move.w  v_pt_tool(a4),d4
        cmp.w   #PTT_LINE,d4
        bne     .nl
        moveq   #1,d4
        bsr     pt_line_seg
        bra     .done
.nl:    cmp.w   #PTT_RECT,d4
        bne     .nr
        moveq   #0,d4
        bsr     pt_rect_shape
        bra     .done
.nr:    cmp.w   #PTT_FRECT,d4
        bne     .no
        moveq   #1,d4
        bsr     pt_rect_shape
        bra     .done
.no:    cmp.w   #PTT_OVAL,d4
        bne     .nfo
        moveq   #0,d4
        bsr     pt_oval_shape
        bra     .done
.nfo:   moveq   #1,d4
        bsr     pt_oval_shape
.done:  bsr     pt_sync
        movem.l (sp)+,d0-d7
        rts

; ---------------------------------------------------------------- keys
; paint_key - d1 = ascii, d2 = raw -> d0 = 0 consumed / 1 not
paint_key:
        lea     VARS,a4
        cmp.b   #'1',d1
        blt     .nd
        cmp.b   #'9',d1
        bgt     .nd
        moveq   #0,d0
        move.b  d1,d0
        sub.w   #'1',d0
        move.w  d0,v_pt_tool(a4)
        bra     .redraw
.nd:    cmp.b   #'0',d1
        bne     .nz
        move.w  #PTT_SPRAY,v_pt_tool(a4)
        bra     .redraw
.nz:    cmp.b   #'[',d1
        bne     .npl
        move.w  v_pt_pen(a4),d0
        subq.w  #1,d0
        bne     .setp
        moveq   #15,d0
.setp:  move.w  d0,v_pt_pen(a4)
        bra     .redraw
.npl:   cmp.b   #']',d1
        bne     .npr
        move.w  v_pt_pen(a4),d0
        addq.w  #1,d0
        cmp.w   #16,d0
        blt     .setp
        moveq   #1,d0
        bra     .setp
.npr:   cmp.b   #'r',d1
        bne     .ng
        moveq   #0,d3               ; R shift in $0BGR... R = bits 1-3
        bra     .tune
.ng:    cmp.b   #'g',d1
        bne     .nb
        moveq   #4,d3
        bra     .tune
.nb:    cmp.b   #'b',d1
        bne     .nn
        moveq   #8,d3
        bra     .tune
.nn:    cmp.b   #'n',d1
        bne     .nt
        bsr     pt_clear
        bra     .redraw
.nt:    cmp.b   #'t',d1
        bne     .ny
        lea     str_pt_name(pc),a0  ; canvas -> tape (~2.5 min)
        lea     PTCANVAS,a1
        move.w  #PT_W*PT_H,d0
        bsr     tape_save_blk
        bra     .redraw
.ny:    cmp.b   #'y',d1
        bne     .nope
        lea     PTCANVAS,a0         ; tape -> canvas
        move.l  a0,v_tp_dst(a4)
        lea     v_numbuf(a4),a0
        move.l  a0,v_tp_nmd(a4)
        move.w  #PT_W*PT_H,v_tp_max(a4)
        bsr     tape_load_core
        bsr     pt_dirty_all
        bra     .redraw
.nope:  moveq   #1,d0
        rts
.tune:  ; bump the selected line-3 entry's channel (entries 5-15 only)
        move.w  v_pt_pen(a4),d0
        cmp.w   #5,d0
        blt     .redraw
        add.w   #48,d0              ; CRAM index (line 3)
        lea     v_ptpal(a4),a0
        move.w  v_pt_pen(a4),d1
        add.w   d1,d1
        lea     (a0,d1.w),a0        ; RAM copy slot
        move.w  (a0),d1
        move.w  d1,d2
        lsr.w   d3,d2
        and.w   #15,d2
        addq.w  #2,d2               ; 3-bit step
        and.w   #14,d2
        moveq   #15,d4
        lsl.w   d3,d4
        not.w   d4
        and.w   d4,d1
        lsl.w   d3,d2
        or.w    d2,d1
        move.w  d1,(a0)
        ; CRAM write
        add.w   d0,d0               ; byte address
        moveq   #0,d2
        move.w  d0,d2
        swap    d2
        or.l    #$C0000000,d2
        move.l  d2,VDP_CTRL
        move.w  d1,VDP_DATA
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

; pt_pal_init - seed the RAM copy of line 3 from the ROM defaults
pt_pal_init:
        movem.l d0-d1/a0-a1/a4,-(sp)
        lea     VARS,a4
        lea     pal_data+96(pc),a0  ; line 3
        lea     v_ptpal(a4),a1
        moveq   #15,d0
.cp:    move.w  (a0)+,(a1)+
        dbra    d0,.cp
        movem.l (sp)+,d0-d1/a0-a1/a4
        rts

; pt_restore_pal - closing Paint: line 3 back to the ROM defaults
; (then the theme re-applies its entries)
pt_restore_pal:
        movem.l d0-d2/a0/a4,-(sp)
        lea     VARS,a4
        lea     pal_data+96(pc),a0
        moveq   #0,d1               ; CRAM index 48
.cp:    cmp.w   #16,d1
        bge     .done
        move.w  d1,d0
        add.w   #48,d0
        add.w   d0,d0
        moveq   #0,d2
        move.w  d0,d2
        swap    d2
        or.l    #$C0000000,d2
        move.l  d2,VDP_CTRL
        move.w  (a0)+,VDP_DATA
        addq.w  #1,d1
        bra     .cp
.done:  bsr     pt_pal_init         ; RAM copy back in sync
        bsr     apply_theme         ; themed entries win again
        movem.l (sp)+,d0-d2/a0/a4
        rts

; ---------------------------------------------------------------- data
str_t_paint:    dc.b    "Paint",0
name_paint:     dc.b    "Paint",0
str_pt_foot:    dc.b    "1-0:tool [/]:pen rgb:tune t/y:tape",0
str_pt_name:    dc.b    "PAINT.UNO",0,0,0
        even
