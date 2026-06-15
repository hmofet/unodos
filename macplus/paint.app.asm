; ============================================================================
; UnoDOS/MacPlus Paint - DISK-LOADED app (PAINT.APP, proc 8, slot 4).
; MacPaint-style 1-bit editor; port of the in-kernel paint.i. Canvas store and
; flood stack are KBSS (pt_canvas/pt_stack); tool/pen/rubber-band scratch are
; kernel vars. The click vector (app +12) runs the synchronous drag loop,
; polling the kernel's ISR-maintained mouse_x/mouse_y/mouse_btn (exported).
; ABI: JMP draw/key/tick/click.
; ============================================================================

        mc68000
        include "sysequ.i"
        include "build/kernel_api.inc"

PT_W        equ 256
PT_H        equ 140
PT_TOOLS    equ 10
PT_BGPEN    equ 0
PT_NPENS    equ 4
PT_STKN     equ 510
PAINT_PROC  equ 8
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

        org     APP_LOAD+APPSLOT_PAINT*APP_SLOT_SZ
        dc.l     paint_draw          ; +0
        dc.l     paint_key           ; +4
        dc.l     paint_tickv         ; +8
        dc.l     paint_click         ; +12 (content click: drag loop)
paint_tickv:
        rts

pt_origin:
        move.w  WX(a2),d0
        add.w   #34,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+4,d1
        rts

pt_px:
        movem.l d0-d5/a0,-(sp)
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
        lea     pt_canvas,a0
        move.b  d2,(a0,d3.l)
        move.w  d0,d3
        move.w  d1,d5
        move.w  d2,d4
        bsr     pt_origin
        add.w   d3,d0
        add.w   d5,d1
        moveq   #1,d2
        moveq   #1,d3
        bsr     fill_rect
.out:   movem.l (sp)+,d0-d5/a0
        rts

pt_put:
        move.w  d2,-(sp)
        move.w  pt_pen(pc),d2
        bsr     pt_px
        move.w  (sp)+,d2
        rts

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
        lea     pt_canvas,a0
        moveq   #0,d2
        move.b  (a0,d3.l),d2
.out:   movem.l (sp)+,d3/a0
        rts

pt_dot:
        movem.l d0-d7,-(sp)
        move.w  pt_pen(pc),d2
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

pt_line_seg:
        movem.l d0-d7,-(sp)
        move.w  d4,pt_lsz-vars(a4)
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
        move.w  d4,pt_ldx-vars(a4)
        add.w   d5,d4
        move.w  d4,pt_err-vars(a4)
.loop:  move.w  d3,-(sp)
        move.w  d4,-(sp)
        move.w  pt_lsz(pc),d3
        moveq   #0,d4
        bsr     pt_dot
        move.w  (sp)+,d4
        move.w  (sp)+,d3
        cmp.w   d0,d2
        bne     .cont
        cmp.w   d1,d3
        beq     .done
.cont:  move.w  pt_err(pc),d4
        add.w   d4,d4
        cmp.w   d5,d4
        blt     .ny
        add.w   d5,pt_err-vars(a4)
        add.w   d6,d0
.ny:    cmp.w   pt_ldx(pc),d4
        bgt     .loop
        move.w  pt_ldx(pc),d4
        add.w   d4,pt_err-vars(a4)
        add.w   d7,d1
        bra     .loop
.done:  movem.l (sp)+,d0-d7
        rts

pt_norm:
        cmp.w   d0,d2
        bge     .x
        exg     d0,d2
.x:     cmp.w   d1,d3
        bge     .y
        exg     d1,d3
.y:     rts

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

pt_oval_shape:
        movem.l d0-d7,-(sp)
        bsr     pt_norm
        move.w  d4,pt_lsz-vars(a4)
        move.w  d2,d4
        sub.w   d0,d4
        lsr.w   #1,d4
        move.w  d3,d5
        sub.w   d1,d5
        lsr.w   #1,d5
        tst.w   d4
        beq     .degen
        tst.w   d5
        bne     .ok
.degen: move.w  pt_lsz(pc),d4
        bsr     pt_rect_shape
        bra     .out
.ok:    move.w  d0,d6
        add.w   d4,d6
        move.w  d1,d7
        add.w   d5,d7
        move.w  d1,d2
.rw:    cmp.w   d3,d2
        bgt     .out
        move.w  d2,d0
        sub.w   d7,d0
        muls    d0,d0
        move.w  d4,d1
        mulu    d1,d1
        move.l  d1,-(sp)
        mulu    d1,d0
        move.w  d5,d1
        mulu    d1,d1
        divu    d1,d0
        and.l   #$FFFF,d0
        move.l  (sp)+,d1
        sub.l   d0,d1
        bpl     .h2
        moveq   #0,d1
.h2:    moveq   #0,d0
.sq:    move.w  d0,pt_err-vars(a4)
        addq.w  #1,d0
        mulu    d0,d0
        cmp.l   d1,d0
        bhi     .goth
        move.w  pt_err(pc),d0
        addq.w  #1,d0
        bra     .sq
.goth:  move.w  pt_err(pc),d0
        bsr     pt_oval_row
        addq.w  #1,d2
        bra     .rw
.out:   movem.l (sp)+,d0-d7
        rts

pt_oval_row:
        movem.l d0-d5,-(sp)
        move.w  d6,d3
        sub.w   d0,d3
        move.w  d6,d4
        add.w   d0,d4
        tst.w   pt_lsz-vars(a4)
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

pt_flood:
        movem.l d0-d7/a0-a1,-(sp)
        bsr     pt_get
        move.w  d2,d7
        cmp.w   pt_pen(pc),d7
        beq     .done
        lea     pt_stack,a1
        moveq   #0,d6
        move.w  d0,(a1)
        move.w  d1,2(a1)
        moveq   #1,d6
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
.lend:  move.w  d0,d4
.r:     cmp.w   #PT_W,d0
        bge     .rend
        bsr     pt_get
        cmp.w   d7,d2
        bne     .rend
        bsr     pt_put
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
        bge     .nup2
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
.nup2:  addq.w  #1,d0
        bra     .r
.rend:  bra     .pop
.done:  movem.l (sp)+,d0-d7/a0-a1
        rts

pt_rand:
        move.w  pt_rnd(pc),d0
        lsl.w   #1,d0
        bcc     .nx
        eor.w   #$1D87,d0
.nx:    bne     .ok
        move.w  #$ACE1,d0
.ok:    move.w  d0,pt_rnd-vars(a4)
        rts

paint_draw:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     vars(pc),a4
        bsr     paint_opened
        moveq   #0,d7
.tool:  cmp.w   #PT_TOOLS,d7
        bge     .pens
        bsr     pt_tool_rect
        moveq   #12,d2
        moveq   #12,d3
        moveq   #0,d4
        cmp.w   pt_tool(pc),d7
        bne     .tbg
        moveq   #3,d4
.tbg:   bsr     fill_rect
        moveq   #12,d2
        moveq   #12,d3
        bsr     rect_outline_fg
        movem.l d0-d1,-(sp)
        addq.w  #3,d0
        addq.w  #2,d1
        move.w  d7,d2
        addq.w  #1,d2
        cmp.w   #10,d2
        bne     .dig
        moveq   #0,d2
.dig:   add.w   #'0',d2
        lea     pt_chbuf,a0
        move.b  d2,(a0)
        clr.b   1(a0)
        moveq   #3,d2
        moveq   #-1,d3
        cmp.w   pt_tool(pc),d7
        bne     .gl
        moveq   #0,d2
        moveq   #3,d3
.gl:    bsr     draw_string_bg
        movem.l (sp)+,d0-d1
        addq.w  #1,d7
        bra     .tool
.pens:  moveq   #0,d7
.pen:   cmp.w   #PT_NPENS,d7
        bge     .canvas
        bsr     pt_pen_rect
        moveq   #15,d2
        moveq   #9,d3
        move.w  d7,d4
        bsr     fill_rect
        moveq   #15,d2
        moveq   #9,d3
        bsr     rect_outline_fg
        cmp.w   pt_pen(pc),d7
        bne     .npen
        addq.w  #6,d0
        add.w   #11,d1
        moveq   #4,d2
        moveq   #2,d3
        moveq   #3,d4
        bsr     fill_rect
.npen:  addq.w  #1,d7
        bra     .pen
.canvas:
        bsr     pt_origin
        subq.w  #1,d0
        subq.w  #1,d1
        move.w  #PT_W+2,d2
        move.w  #PT_H+2,d3
        bsr     rect_outline_fg
        bsr     pt_repaint
        lea     str_pt_foot(pc),a0
        move.w  WX(a2),d0
        addq.w  #4,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #8,d1
        moveq   #3,d2
        bsr     draw_string
        movem.l (sp)+,d0-d7/a0-a1/a4
        rts

pt_tool_rect:
        move.w  d7,d0
        and.w   #1,d0
        mulu    #14,d0
        add.w   WX(a2),d0
        addq.w  #4,d0
        move.w  d7,d1
        lsr.w   #1,d1
        mulu    #14,d1
        add.w   WY(a2),d1
        add.w   #TBAR_H+4,d1
        rts

pt_pen_rect:
        move.w  d7,d0
        mulu    #18,d0
        add.w   WX(a2),d0
        add.w   #34,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #26,d1
        rts

pt_repaint:
        movem.l d0-d7/a0-a1,-(sp)
        bsr     pt_origin
        move.w  d0,d5
        move.w  d1,d6
        lea     pt_canvas,a0
        moveq   #0,d7
.row:   cmp.w   #PT_H,d7
        bge     .done
        moveq   #0,d1
.run:   cmp.w   #PT_W,d1
        bge     .nrow
        moveq   #0,d4
        move.b  (a0),d4
        move.w  d1,d2
.ext:   addq.w  #1,d1
        addq.l  #1,a0
        cmp.w   #PT_W,d1
        bge     .flush
        cmp.b   (a0),d4
        beq     .ext
.flush: movem.l d1,-(sp)
        move.w  d1,d3
        sub.w   d2,d3
        move.w  d2,d0
        add.w   d5,d0
        move.w  d7,d1
        add.w   d6,d1
        move.w  d3,d2
        moveq   #1,d3
        bsr     fill_rect
        movem.l (sp)+,d1
        move.w  d1,d2
        bra     .run
.nrow:  addq.w  #1,d7
        bra     .row
.done:  movem.l (sp)+,d0-d7/a0-a1
        rts

; paint_click - d0/d1 = click position (pixels), a2 = topmost window.
paint_click:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     vars(pc),a4
        move.w  d0,d5
        move.w  d1,d6
        moveq   #0,d7
.ttool: cmp.w   #PT_TOOLS,d7
        bge     .tpen
        bsr     pt_tool_rect
        cmp.w   d0,d5
        blt     .ntool
        move.w  d0,d2
        add.w   #12,d2
        cmp.w   d2,d5
        bgt     .ntool
        cmp.w   d1,d6
        blt     .ntool
        move.w  d1,d2
        add.w   #12,d2
        cmp.w   d2,d6
        bgt     .ntool
        move.w  d7,pt_tool-vars(a4)
        bsr     paint_draw
        bra     .out
.ntool: addq.w  #1,d7
        bra     .ttool
.tpen:  moveq   #0,d7
.tp:    cmp.w   #PT_NPENS,d7
        bge     .tcanvas
        bsr     pt_pen_rect
        cmp.w   d0,d5
        blt     .np
        move.w  d0,d2
        add.w   #15,d2
        cmp.w   d2,d5
        bgt     .np
        cmp.w   d1,d6
        blt     .np
        move.w  d1,d2
        add.w   #10,d2
        cmp.w   d2,d6
        bgt     .np
        move.w  d7,pt_pen-vars(a4)
        bsr     paint_draw
        bra     .out
.np:    addq.w  #1,d7
        bra     .tp
.tcanvas:
        bsr     pt_origin
        move.w  d5,d2
        sub.w   d0,d2
        move.w  d6,d3
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
        move.w  pt_tool(pc),d4
        cmp.w   #PTT_FILL,d4
        bne     .nfill
        bsr     pt_flood
        bra     .commit
.nfill: cmp.w   #PTT_LINE,d4
        blt     .freehand
        cmp.w   #PTT_FOVAL,d4
        ble     .shape
.freehand:
        bsr     pt_drag_free
        bra     .commit
.shape: bsr     pt_drag_shape
.commit:
        bsr     redraw_topmost      ; canvas changed: repaint the window
.out:   movem.l (sp)+,d0-d7/a0-a1/a4
        rts

pt_canvas_mouse:
        bsr     pt_origin
        move.w  d0,d2
        move.w  d1,d3
        move.w  mouse_x(pc),d0
        sub.w   d2,d0
        move.w  mouse_y(pc),d1
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
        move.b  mouse_btn(pc),d2
        rts

pt_drag_free:
        movem.l d0-d7,-(sp)
        move.w  d0,d5
        move.w  d1,d6
.lp:    move.w  pt_tool(pc),d4
        cmp.w   #PTT_SPRAY,d4
        beq     .spray
        moveq   #1,d3
        cmp.w   #PTT_BRUSH,d4
        bne     .nb
        moveq   #4,d3
.nb:    cmp.w   #PTT_ERASER,d4
        bne     .ne
        moveq   #8,d3
.ne:    moveq   #0,d4
        move.w  pt_tool(pc),d2
        cmp.w   #PTT_ERASER,d2
        bne     .seg
        moveq   #1,d4
.seg:   move.w  d3,-(sp)
        move.w  d0,d2
        move.w  d1,d3
        move.w  d5,d0
        move.w  d6,d1
        move.w  (sp)+,d7
        exg     d4,d7
        tst.w   d7
        beq     .ink
        move.w  pt_pen(pc),-(sp)
        move.w  #PT_BGPEN,pt_pen-vars(a4)
        bsr     pt_line_seg
        move.w  (sp)+,pt_pen-vars(a4)
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
.next:  bsr     pt_canvas_mouse
        tst.w   d2
        beq     .done
        cmp.w   #PTT_SPRAY,pt_tool-vars(a4)
        bne     .lp
        move.w  d0,d5
        move.w  d1,d6
        bra     .lp
.done:  movem.l (sp)+,d0-d7
        rts

pt_drag_shape:
        movem.l d0-d7,-(sp)
        move.w  d0,d5
        move.w  d1,d6
        move.w  d0,pt_px1-vars(a4)
        move.w  d1,pt_py1-vars(a4)
        sf      pt_band-vars(a4)
.lp:    bsr     pt_canvas_mouse
        tst.w   d2
        beq     .release
        cmp.w   pt_px1(pc),d0
        bne     .mv
        cmp.w   pt_py1(pc),d1
        beq     .lp
.mv:    bsr     pt_band_undraw
        move.w  d0,pt_px1-vars(a4)
        move.w  d1,pt_py1-vars(a4)
        bsr     pt_band_draw
        bra     .lp
.release:
        bsr     pt_band_undraw
        move.w  d5,d0
        move.w  d6,d1
        move.w  pt_px1(pc),d2
        move.w  pt_py1(pc),d3
        move.w  pt_tool(pc),d4
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
.done:  movem.l (sp)+,d0-d7
        rts

pt_band_draw:
        movem.l d0-d7,-(sp)
        st      pt_band-vars(a4)
        move.w  d5,pt_px0-vars(a4)
        move.w  d6,pt_py0-vars(a4)
        bsr     pt_band_edges_pen
        movem.l (sp)+,d0-d7
        rts

pt_band_undraw:
        movem.l d0-d7,-(sp)
        move.b  pt_band(pc),d0
        beq     .out
        sf      pt_band-vars(a4)
        bsr     pt_band_edges_restore
.out:   movem.l (sp)+,d0-d7
        rts

pt_band_edges_pen:
        movem.l d0-d7,-(sp)
        move.w  pt_px0(pc),d0
        move.w  pt_py0(pc),d1
        move.w  pt_px1(pc),d2
        move.w  pt_py1(pc),d3
        bsr     pt_norm
        movem.l d0-d3,-(sp)
        bsr     pt_origin
        move.w  d0,d6
        move.w  d1,d7
        movem.l (sp)+,d0-d3
        move.w  d2,d4
        sub.w   d0,d4
        addq.w  #1,d4
        movem.l d0-d4,-(sp)
        add.w   d6,d0
        move.w  d1,d5
        add.w   d7,d5
        move.w  d5,d1
        move.w  d4,d2
        moveq   #1,d3
        moveq   #3,d4
        bsr     fill_rect
        movem.l (sp)+,d0-d4
        movem.l d0-d4,-(sp)
        add.w   d6,d0
        move.w  d3,d1
        add.w   d7,d1
        move.w  d4,d2
        moveq   #1,d3
        moveq   #3,d4
        bsr     fill_rect
        movem.l (sp)+,d0-d4
        movem.l d0-d4,-(sp)
        move.w  d3,d5
        sub.w   d1,d5
        addq.w  #1,d5
        add.w   d6,d0
        add.w   d7,d1
        moveq   #1,d2
        move.w  d5,d3
        moveq   #3,d4
        bsr     fill_rect
        movem.l (sp)+,d0-d4
        move.w  d3,d5
        sub.w   d1,d5
        addq.w  #1,d5
        move.w  d2,d0
        add.w   d6,d0
        add.w   d7,d1
        moveq   #1,d2
        move.w  d5,d3
        moveq   #3,d4
        bsr     fill_rect
        movem.l (sp)+,d0-d7
        rts

pt_band_edges_restore:
        movem.l d0-d7,-(sp)
        move.w  pt_px0(pc),d0
        move.w  pt_py0(pc),d1
        move.w  pt_px1(pc),d2
        move.w  pt_py1(pc),d3
        bsr     pt_norm
        move.w  d1,d7
        bsr     pt_restore_row
        move.w  d3,d7
        bsr     pt_restore_row
        move.w  d0,d7
        bsr     pt_restore_col
        move.w  d2,d7
        bsr     pt_restore_col
        movem.l (sp)+,d0-d7
        rts

pt_restore_row:
        movem.l d0-d6/a0,-(sp)
        move.w  d2,d5
        move.w  d0,d4
.px:    cmp.w   d5,d4
        bgt     .done
        move.w  d4,d0
        move.w  d7,d1
        bsr     pt_get
        bsr     pt_px
        addq.w  #1,d4
        bra     .px
.done:  movem.l (sp)+,d0-d6/a0
        rts

pt_restore_col:
        movem.l d0-d6/a0,-(sp)
        move.w  d3,d5
        move.w  d1,d4
.px:    cmp.w   d5,d4
        bgt     .done
        move.w  d7,d0
        move.w  d4,d1
        bsr     pt_get
        bsr     pt_px
        addq.w  #1,d4
        bra     .px
.done:  movem.l (sp)+,d0-d6/a0
        rts

paint_key:
        lea     vars(pc),a4
        cmp.b   #'1',d1
        blt     .nd
        cmp.b   #'9',d1
        bgt     .nd
        moveq   #0,d0
        move.b  d1,d0
        sub.w   #'1',d0
        move.w  d0,pt_tool-vars(a4)
        bra     .redraw
.nd:    cmp.b   #'0',d1
        bne     .npen0
        move.w  #PTT_SPRAY,pt_tool-vars(a4)
        bra     .redraw
.npen0: cmp.b   #'[',d1
        bne     .npl
        move.w  pt_pen(pc),d0
        subq.w  #1,d0
        bge     .setp
        moveq   #PT_NPENS-1,d0
.setp:  move.w  d0,pt_pen-vars(a4)
        bra     .redraw
.npl:   cmp.b   #']',d1
        bne     .nn
        move.w  pt_pen(pc),d0
        addq.w  #1,d0
        cmp.w   #PT_NPENS,d0
        blt     .setp
        moveq   #0,d0
        bra     .setp
.nn:    cmp.b   #'n',d1
        bne     .ns
        bsr     pt_clear
        bra     .redraw
.ns:    cmp.b   #'s',d1
        bne     .nl
        bsr     pt_save
        bra     .redraw
.nl:    cmp.b   #'l',d1
        bne     .nope
        bsr     pt_load
        bra     .redraw
.nope:  moveq   #1,d0
        rts
.redraw:
        moveq   #0,d0               ; consumed; kernel redraws topmost
        rts

pt_clear:
        movem.l d0-d1/a0,-(sp)
        lea     pt_canvas,a0
        move.l  #(PT_W*PT_H/4)-1,d0
        moveq   #PT_BGPEN,d1
.cl:    move.b  d1,(a0)+
        move.b  d1,(a0)+
        move.b  d1,(a0)+
        move.b  d1,(a0)+
        subq.l  #1,d0
        bpl     .cl
        movem.l (sp)+,d0-d1/a0
        rts

pt_save:
        movem.l d0-d1/a0-a1,-(sp)
        move.b  fat_mounted(pc),d0
        bne     .ok
        bsr     files_mount
        move.b  fat_mounted(pc),d0
        beq     .out
.ok:    lea     str_pt_name(pc),a0
        lea     pt_canvas,a1
        move.l  #PT_W*PT_H,d1
        bsr     fat_save_file
        bsr     fat_list_root
.out:   movem.l (sp)+,d0-d1/a0-a1
        rts

pt_load:
        movem.l d0-d1/a0-a1,-(sp)
        move.b  fat_mounted(pc),d0
        bne     .ok
        bsr     files_mount
        move.b  fat_mounted(pc),d0
        beq     .out
.ok:    lea     str_pt_name(pc),a0
        bsr     fat_find_file
        tst.w   d0
        bmi     .out
        move.l  #PT_W*PT_H,d1
        lea     pt_canvas,a1
        bsr     fat_read_file
.out:   movem.l (sp)+,d0-d1/a0-a1
        rts

paint_opened:
        movem.l d0/a0/a4,-(sp)
        lea     vars(pc),a4
        move.b  pt_init(pc),d0
        bne     .out
        st      pt_init-vars(a4)
        bsr     pt_clear
.out:   movem.l (sp)+,d0/a0/a4
        rts

; ---------------------------------------------------------------- data
str_pt_foot:    dc.b    "1-0:tool [/]:ink n:new s/l:disk",0
str_pt_name:    dc.b    "PAINT   UNO"
        even
        end
