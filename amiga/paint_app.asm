; ============================================================================
; UnoDOS/68K Paint - DISK-LOADED app (PAINT.APP, proc 10). Was paint.i's full
; MacPaint-style editor built into the kernel; now a separate -Fbin binary
; loaded off DF1 into its window slot. Links to the kernel only via APIVEC.
;
; The canvas backing store (pt_canvas) and the flood-fill scanline stack
; (pt_stack) stay in the kernel BSS and are published to the app via APIVEC
; (their runtime addresses). The tool / pen / rubber-band state (pt_*) and the
; live mouse (mouse_x/mouse_y/mouse_btn) live in the kernel 'vars' block. The
; copper colour pointer (cop_colptr) and the extended game palette (ext_palette)
; are exported kernel data; pt_restore_palette stays kernel-resident (run by
; close_window). The kernel dispatches a body click to APP_CLICK, where the
; synchronous drag loop polls the vblank-updated mouse and draws.
;
; Under -DAUTOTEST_PAINT the app self-renders a demo scene in paint_open (the
; kernel has no mouse to inject), exercising the shape/flood primitives.
; ============================================================================

        mc68000
        include "sysabi.i"
        include "build/kernel_api.inc"

PT_W        equ 256
PT_H        equ 140
PT_TOOLS    equ 10
PT_BGPEN    equ 3                   ; canvas background pen (white)
PT_STKN     equ 510                 ; flood-fill stack entries

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

        org     APPSLOT0
; ---- JMP table (open/draw/key/tick/click) ----
        jmp     paint_open(pc)
        jmp     paint_draw(pc)
        jmp     paint_key(pc)
        jmp     paint_tick(pc)
        jmp     paint_click(pc)

paint_tick:
        rts

paint_open:
        ; reset state on launch (canvas persists across opens)
        movem.l d0/a0/a4,-(sp)
        KDATA   a4,vars
        move.b  VO_pt_init(a4),d0
        bne     .opened
        st      VO_pt_init(a4)
        bsr     pt_clear
.opened:
        movem.l (sp)+,d0/a0/a4
        ifd     AUTOTEST_PAINT
        ; demo scene through the real shape primitives (line/rect/oval/fill all
        ; exercise the canvas store + run renderer). a2 = the Paint window.
        movem.l d0-d7/a0-a1/a4,-(sp)
        KDATA   a4,vars
        move.w  #9,VO_pt_pen(a4)    ; red-ish game pen
        moveq   #30,d0
        moveq   #24,d1
        move.w  #120,d2
        moveq   #90,d3
        moveq   #1,d4
        bsr     pt_rect_shape
        move.w  #5,VO_pt_pen(a4)
        move.w  #150,d0
        moveq   #40,d1
        move.w  #240,d2
        move.w  #120,d3
        moveq   #1,d4
        bsr     pt_oval_shape
        move.w  #3,VO_pt_pen(a4)
        moveq   #10,d0
        move.w  #130,d1
        move.w  #250,d2
        moveq   #20,d3
        moveq   #1,d4
        bsr     pt_line_seg
        move.w  #14,VO_pt_pen(a4)
        move.w  #170,d0
        moveq   #60,d1
        bsr     pt_flood            ; fill inside the oval
        movem.l (sp)+,d0-d7/a0-a1/a4
        endc
        rts

; ---------------------------------------------------------------- geometry
; pt_origin - a2 = window -> d0/d1 = canvas screen origin
pt_origin:
        move.w  WX(a2),d0
        add.w   #34,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+4,d1
        rts

; ---------------------------------------------------------------- pixels
; pt_px - d0/d1 = canvas coords, d2 = pen: store + show. a2 = window.
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
        KDATA   a0,pt_canvas
        move.b  d2,(a0,d3.l)
        move.w  d0,d3
        move.w  d1,d5
        move.w  d2,d4
        bsr     pt_origin
        add.w   d3,d0
        add.w   d5,d1
        moveq   #1,d2
        moveq   #1,d3
        KCALL   fill_rect
.out:   movem.l (sp)+,d0-d5/a0
        rts

; pt_put - plot (d0,d1) with the current pen. a4 = &vars.
pt_put:
        move.w  d2,-(sp)
        move.w  VO_pt_pen(a4),d2
        bsr     pt_px
        move.w  (sp)+,d2
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
        KDATA   a0,pt_canvas
        moveq   #0,d2
        move.b  (a0,d3.l),d2
.out:   movem.l (sp)+,d3/a0
        rts

; pt_dot - d0/d1 = center, d3 = size (1/4/8), d4 = 0 ink / 1 erase. a4 = &vars
pt_dot:
        movem.l d0-d7,-(sp)
        move.w  VO_pt_pen(a4),d2
        tst.w   d4
        beq     .ink
        moveq   #PT_BGPEN,d2
.ink:   move.w  d0,d5
        move.w  d1,d6
        move.w  d3,d7
        lsr.w   #1,d7
        sub.w   d7,d5
        sub.w   d7,d6
        moveq   #0,d7               ; row
.row:   cmp.w   d3,d7
        bge     .done
        moveq   #0,d4               ; col
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

; pt_line_seg - (d0,d1) -> (d2,d3), dot size d4 (Bresenham). a4 = &vars
pt_line_seg:
        movem.l d0-d7,-(sp)
        move.w  d4,VO_pt_lsz(a4)
        moveq   #1,d6               ; sx
        move.w  d2,d4
        sub.w   d0,d4
        bge     .dxok
        neg.w   d4
        moveq   #-1,d6
.dxok:  moveq   #1,d7               ; sy
        move.w  d3,d5
        sub.w   d1,d5
        bge     .dyok
        neg.w   d5
        moveq   #-1,d7
.dyok:  neg.w   d5                  ; dy = -abs(y1-y0)
        move.w  d4,VO_pt_ldx(a4)
        add.w   d5,d4
        move.w  d4,VO_pt_err(a4)    ; err = dx + dy
.loop:  move.w  d3,-(sp)
        move.w  d4,-(sp)
        move.w  VO_pt_lsz(a4),d3
        moveq   #0,d4
        bsr     pt_dot
        move.w  (sp)+,d4
        move.w  (sp)+,d3
        cmp.w   d0,d2
        bne     .cont
        cmp.w   d1,d3
        beq     .done
.cont:  move.w  VO_pt_err(a4),d4
        add.w   d4,d4               ; e2 = 2*err
        cmp.w   d5,d4
        blt     .ny
        add.w   d5,VO_pt_err(a4)
        add.w   d6,d0
.ny:    cmp.w   VO_pt_ldx(a4),d4
        bgt     .loop
        move.w  VO_pt_ldx(a4),d4
        add.w   d4,VO_pt_err(a4)
        add.w   d7,d1
        bra     .loop
.done:  movem.l (sp)+,d0-d7
        rts

; pt_norm - order (d0,d1)-(d2,d3) as top-left / bottom-right
pt_norm:
        cmp.w   d0,d2
        bge     .x
        exg     d0,d2
.x:     cmp.w   d1,d3
        bge     .y
        exg     d1,d3
.y:     rts

; pt_rect_shape - (d0,d1)-(d2,d3), d4 = 0 frame / 1 filled. a4 = &vars
pt_rect_shape:
        movem.l d0-d7,-(sp)
        bsr     pt_norm
        move.w  d4,d7               ; filled flag
        move.w  d1,d6               ; current row
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

; pt_oval_shape - bounding box (d0,d1)-(d2,d3), d4 = 0 frame / 1 filled. a4=&vars
pt_oval_shape:
        movem.l d0-d7,-(sp)
        bsr     pt_norm
        move.w  d4,VO_pt_lsz(a4)    ; filled flag (pt_oval_row reads it)
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
.degen: move.w  VO_pt_lsz(a4),d4
        bsr     pt_rect_shape
        bra     .out
.ok:    move.w  d0,d6
        add.w   d4,d6               ; cx
        move.w  d1,d7
        add.w   d5,d7               ; cy
        move.w  d1,d2               ; d2 = row
.rw:    cmp.w   d3,d2
        bgt     .out
        move.w  d2,d0
        sub.w   d7,d0
        muls    d0,d0               ; dy^2
        move.w  d4,d1
        mulu    d1,d1               ; a^2
        move.l  d1,-(sp)            ; keep a^2
        mulu    d1,d0               ; a^2 * dy^2
        move.w  d5,d1
        mulu    d1,d1               ; b^2
        divu    d1,d0
        and.l   #$FFFF,d0
        move.l  (sp)+,d1
        sub.l   d0,d1               ; h^2
        bpl     .h2
        moveq   #0,d1
.h2:    moveq   #0,d0               ; h = isqrt(h^2) by increment
.sq:    move.w  d0,VO_pt_err(a4)
        addq.w  #1,d0
        mulu    d0,d0
        cmp.l   d1,d0
        bhi     .goth
        move.w  VO_pt_err(a4),d0
        addq.w  #1,d0
        bra     .sq
.goth:  move.w  VO_pt_err(a4),d0    ; h
        bsr     pt_oval_row
        addq.w  #1,d2
        bra     .rw
.out:   movem.l (sp)+,d0-d7
        rts

; pt_oval_row - d0 = h, d2 = row, d6 = cx, pt_lsz = filled. a4 = &vars
pt_oval_row:
        movem.l d0-d5,-(sp)
        move.w  d6,d3
        sub.w   d0,d3               ; left
        move.w  d6,d4
        add.w   d0,d4               ; right
        tst.w   VO_pt_lsz(a4)
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

; pt_flood - scanline flood fill from (d0,d1) with the current pen. a4 = &vars
pt_flood:
        movem.l d0-d7/a0-a1,-(sp)
        bsr     pt_get              ; d2 = target pen
        move.w  d2,d7               ; from
        cmp.w   VO_pt_pen(a4),d7
        beq     .done
        KDATA   a1,pt_stack
        moveq   #0,d6               ; stack count
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
.lend:  move.w  d0,d4               ; left edge
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

; pt_rand -> d0.w pseudo-random (16-bit LFSR). a4 = &vars
pt_rand:
        move.w  VO_pt_rnd(a4),d0
        lsl.w   #1,d0
        bcc     .nx
        eor.w   #$1D87,d0
.nx:    bne     .ok
        move.w  #$ACE1,d0
.ok:    move.w  d0,VO_pt_rnd(a4)
        rts

; ---------------------------------------------------------------- drawing
; paint_draw - a2 = window
paint_draw:
        movem.l d0-d7/a0-a1/a4,-(sp)
        KDATA   a4,vars
        ; tool cells: 2 x 5 grid at wx+4
        moveq   #0,d7
.tool:  cmp.w   #PT_TOOLS,d7
        bge     .pens
        bsr     pt_tool_rect        ; d7 -> d0/d1 (x,y)
        moveq   #12,d2
        moveq   #12,d3
        moveq   #1,d4               ; accent bg
        cmp.w   VO_pt_tool(a4),d7
        bne     .tbg
        moveq   #2,d4               ; selected = accent2
.tbg:   KCALL   fill_rect
        ; glyph: the tool digit (1-9, 0)
        movem.l d0-d1,-(sp)
        addq.w  #3,d0
        addq.w  #2,d1
        move.w  d7,d2
        addq.w  #1,d2
        cmp.w   #10,d2
        bne     .dig
        moveq   #0,d2
.dig:   add.w   #'0',d2
        lea     VO_pt_chbuf(a4),a0
        move.b  d2,(a0)
        clr.b   1(a0)
        moveq   #0,d2               ; desktop-color glyph
        KCALL   draw_string
        movem.l (sp)+,d0-d1
        addq.w  #1,d7
        bra     .tool
.pens:  ; pen strip: 32 swatches of 8x10 along the bottom
        moveq   #0,d7
.pen:   cmp.w   #32,d7
        bge     .canvas
        bsr     pt_pen_rect         ; d7 -> d0/d1
        moveq   #7,d2
        moveq   #9,d3
        move.w  d7,d4
        KCALL   fill_rect
        cmp.w   VO_pt_pen(a4),d7
        bne     .npen
        ; selection: white tick under the active pen
        addq.w  #2,d0
        add.w   #10,d1
        moveq   #3,d2
        moveq   #1,d3
        moveq   #3,d4
        KCALL   fill_rect
.npen:  addq.w  #1,d7
        bra     .pen
.canvas:
        bsr     pt_repaint
        ; footer hint
        lea     str_pt_foot(pc),a0
        move.w  WX(a2),d0
        addq.w  #4,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #8,d1
        moveq   #1,d2
        KCALL   draw_string
        movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; pt_tool_rect - d7 = tool -> d0/d1 = cell screen pos
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

; pt_pen_rect - d7 = pen -> d0/d1 = swatch screen pos
pt_pen_rect:
        move.w  d7,d0
        lsl.w   #3,d0
        add.w   WX(a2),d0
        add.w   #34,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #26,d1
        rts

; pt_repaint - render the canvas as horizontal runs
pt_repaint:
        movem.l d0-d7/a0-a1,-(sp)
        bsr     pt_origin
        move.w  d0,d5               ; screen origin
        move.w  d1,d6
        KDATA   a0,pt_canvas
        moveq   #0,d7               ; row
.row:   cmp.w   #PT_H,d7
        bge     .done
        moveq   #0,d1               ; x
.run:   cmp.w   #PT_W,d1
        bge     .nrow
        moveq   #0,d4
        move.b  (a0),d4             ; run pen
        move.w  d1,d2               ; run start
.ext:   addq.w  #1,d1
        addq.l  #1,a0
        cmp.w   #PT_W,d1
        bge     .flush
        cmp.b   (a0),d4
        beq     .ext
.flush: movem.l d1,-(sp)
        move.w  d1,d3
        sub.w   d2,d3               ; len
        move.w  d2,d0
        add.w   d5,d0
        move.w  d7,d1
        add.w   d6,d1
        move.w  d3,d2
        moveq   #1,d3
        KCALL   fill_rect
        movem.l (sp)+,d1
        move.w  d1,d2
        bra     .run
.nrow:  addq.w  #1,d7
        bra     .row
.done:  movem.l (sp)+,d0-d7/a0-a1
        rts

; ---------------------------------------------------------------- input
; paint_click - d0/d1 = click position (pixels), a2 = topmost window.
paint_click:
        movem.l d0-d7/a0-a1/a4,-(sp)
        KDATA   a4,vars
        move.w  d0,d5               ; click x/y
        move.w  d1,d6
        ; tools?
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
        move.w  d7,VO_pt_tool(a4)
        bsr     paint_draw
        bra     .out
.ntool: addq.w  #1,d7
        bra     .ttool
.tpen:  ; pens?
        moveq   #0,d7
.tp:    cmp.w   #32,d7
        bge     .tcanvas
        bsr     pt_pen_rect
        cmp.w   d0,d5
        blt     .np
        move.w  d0,d2
        addq.w  #7,d2
        cmp.w   d2,d5
        bgt     .np
        cmp.w   d1,d6
        blt     .np
        move.w  d1,d2
        add.w   #10,d2
        cmp.w   d2,d6
        bgt     .np
        move.w  d7,VO_pt_pen(a4)
        bsr     paint_draw
        bra     .out
.np:    addq.w  #1,d7
        bra     .tp
.tcanvas:
        ; inside the canvas?
        bsr     pt_origin
        move.w  d5,d2
        sub.w   d0,d2               ; canvas x
        move.w  d6,d3
        sub.w   d1,d3               ; canvas y
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
        move.w  VO_pt_tool(a4),d4
        cmp.w   #PTT_FILL,d4
        bne     .nfill
        bsr     pt_flood
        bra     .out
.nfill: cmp.w   #PTT_LINE,d4
        blt     .freehand
        cmp.w   #PTT_FOVAL,d4
        ble     .shape              ; line/rect/frect/oval/foval
.freehand:
        bsr     pt_drag_free
        bra     .out
.shape: bsr     pt_drag_shape
.out:   movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; pt_canvas_mouse -> d0/d1 = mouse in canvas coords (clamped), d2 = button.
; a2 = window, a4 = &vars.
pt_canvas_mouse:
        bsr     pt_origin
        move.w  d0,d2
        move.w  d1,d3
        move.w  VO_mouse_x(a4),d0
        sub.w   d2,d0
        move.w  VO_mouse_y(a4),d1
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
        move.b  VO_mouse_btn(a4),d2
        rts

; pt_drag_free - d0/d1 = start: pencil / brush / eraser / spray until release.
pt_drag_free:
        movem.l d0-d7,-(sp)
        move.w  d0,d5               ; last x/y
        move.w  d1,d6
.lp:    move.w  VO_pt_tool(a4),d4
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
        move.w  VO_pt_tool(a4),d2
        cmp.w   #PTT_ERASER,d2
        bne     .seg
        moveq   #1,d4
.seg:   move.w  d3,-(sp)
        move.w  d0,d2
        move.w  d1,d3
        move.w  d5,d0
        move.w  d6,d1
        move.w  (sp)+,d7
        exg     d4,d7               ; d4 = size
        tst.w   d7
        beq     .ink
        move.w  VO_pt_pen(a4),-(sp)
        move.w  #PT_BGPEN,VO_pt_pen(a4)
        bsr     pt_line_seg
        move.w  (sp)+,VO_pt_pen(a4)
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
.next:  bsr     pt_canvas_mouse     ; d0/d1 = pos, d2 = button
        tst.w   d2
        beq     .done
        cmp.w   #PTT_SPRAY,VO_pt_tool(a4)
        bne     .lp
        move.w  d0,d5               ; spray tracks the cursor directly
        move.w  d1,d6
        bra     .lp
.done:  movem.l (sp)+,d0-d7
        rts

; pt_drag_shape - d0/d1 = anchor: rubber-band then commit on release.
pt_drag_shape:
        movem.l d0-d7,-(sp)
        move.w  d0,d5               ; anchor
        move.w  d1,d6
        move.w  d0,VO_pt_px1(a4)    ; current end
        move.w  d1,VO_pt_py1(a4)
        sf      VO_pt_band(a4)
.lp:    bsr     pt_canvas_mouse
        tst.w   d2
        beq     .release
        cmp.w   VO_pt_px1(a4),d0
        bne     .mv
        cmp.w   VO_pt_py1(a4),d1
        beq     .lp
.mv:    bsr     pt_band_undraw
        move.w  d0,VO_pt_px1(a4)
        move.w  d1,VO_pt_py1(a4)
        bsr     pt_band_draw
        bra     .lp
.release:
        bsr     pt_band_undraw
        move.w  d5,d0
        move.w  d6,d1
        move.w  VO_pt_px1(a4),d2
        move.w  VO_pt_py1(a4),d3
        move.w  VO_pt_tool(a4),d4
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

; pt_band_draw - white preview frame; anchor stashed in vars. a4 = &vars
pt_band_draw:
        movem.l d0-d7,-(sp)
        st      VO_pt_band(a4)
        move.w  d5,VO_pt_px0(a4)
        move.w  d6,VO_pt_py0(a4)
        bsr     pt_band_edges_pen
        movem.l (sp)+,d0-d7
        rts

pt_band_undraw:
        movem.l d0-d7,-(sp)
        move.b  VO_pt_band(a4),d0
        beq     .out
        sf      VO_pt_band(a4)
        bsr     pt_band_edges_restore
.out:   movem.l (sp)+,d0-d7
        rts

; pt_band_edges_pen - draw the 4 frame edges in white via fill_rect
pt_band_edges_pen:
        movem.l d0-d7,-(sp)
        move.w  VO_pt_px0(a4),d0
        move.w  VO_pt_py0(a4),d1
        move.w  VO_pt_px1(a4),d2
        move.w  VO_pt_py1(a4),d3
        bsr     pt_norm
        movem.l d0-d3,-(sp)
        bsr     pt_origin           ; d0/d1 = screen origin
        move.w  d0,d6
        move.w  d1,d7
        movem.l (sp)+,d0-d3
        ; top edge
        move.w  d2,d4
        sub.w   d0,d4
        addq.w  #1,d4               ; width
        movem.l d0-d4,-(sp)
        add.w   d6,d0
        move.w  d1,d5
        add.w   d7,d5
        move.w  d5,d1
        move.w  d4,d2
        moveq   #1,d3
        moveq   #3,d4
        KCALL   fill_rect
        movem.l (sp)+,d0-d4
        ; bottom edge
        movem.l d0-d4,-(sp)
        add.w   d6,d0
        move.w  d3,d1
        add.w   d7,d1
        move.w  d4,d2
        moveq   #1,d3
        moveq   #3,d4
        KCALL   fill_rect
        movem.l (sp)+,d0-d4
        ; left edge
        movem.l d0-d4,-(sp)
        move.w  d3,d5
        sub.w   d1,d5
        addq.w  #1,d5               ; height
        add.w   d6,d0
        add.w   d7,d1
        moveq   #1,d2
        move.w  d5,d3
        moveq   #3,d4
        KCALL   fill_rect
        movem.l (sp)+,d0-d4
        ; right edge
        move.w  d3,d5
        sub.w   d1,d5
        addq.w  #1,d5
        move.w  d2,d0
        add.w   d6,d0
        add.w   d7,d1
        moveq   #1,d2
        move.w  d5,d3
        moveq   #3,d4
        KCALL   fill_rect
        movem.l (sp)+,d0-d7
        rts

; pt_band_edges_restore - repaint the canvas runs under the old frame
pt_band_edges_restore:
        movem.l d0-d7,-(sp)
        move.w  VO_pt_px0(a4),d0
        move.w  VO_pt_py0(a4),d1
        move.w  VO_pt_px1(a4),d2
        move.w  VO_pt_py1(a4),d3
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

; pt_restore_row - repaint canvas row d7 between d0..d2 (clipped)
pt_restore_row:
        movem.l d0-d6/a0,-(sp)
        move.w  d2,d5               ; x end
        move.w  d0,d4               ; x
.px:    cmp.w   d5,d4
        bgt     .done
        move.w  d4,d0
        move.w  d7,d1
        bsr     pt_get              ; d2 = stored pen
        bsr     pt_px               ; rewrite (also redraws on screen)
        addq.w  #1,d4
        bra     .px
.done:  movem.l (sp)+,d0-d6/a0
        rts

; pt_restore_col - repaint canvas column d7 between rows d1..d3
pt_restore_col:
        movem.l d0-d6/a0,-(sp)
        move.w  d3,d5               ; y end
        move.w  d1,d4               ; y
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

; ---------------------------------------------------------------- keys
; paint_key - d1 = ascii, d2 = raw -> d0 = 0 consumed / 1 not
paint_key:
        KDATA   a4,vars
        cmp.b   #'1',d1
        blt     .nd
        cmp.b   #'9',d1
        bgt     .nd
        moveq   #0,d0
        move.b  d1,d0
        sub.w   #'1',d0
        move.w  d0,VO_pt_tool(a4)
        bra     .redraw
.nd:    cmp.b   #'0',d1
        bne     .npen0
        move.w  #PTT_SPRAY,VO_pt_tool(a4)
        bra     .redraw
.npen0: cmp.b   #'[',d1
        bne     .npl
        move.w  VO_pt_pen(a4),d0
        subq.w  #1,d0
        bge     .setp
        moveq   #31,d0
.setp:  move.w  d0,VO_pt_pen(a4)
        bra     .redraw
.npl:   cmp.b   #']',d1
        bne     .npr
        move.w  VO_pt_pen(a4),d0
        addq.w  #1,d0
        cmp.w   #32,d0
        blt     .setp
        moveq   #0,d0
        bra     .setp
.npr:   cmp.b   #'r',d1
        bne     .ng
        moveq   #8,d3
        bra     .tune
.ng:    cmp.b   #'g',d1
        bne     .nb
        moveq   #4,d3
        bra     .tune
.nb:    cmp.b   #'b',d1
        bne     .nn
        moveq   #0,d3
        bra     .tune
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
.tune:  ; bump the selected pen's channel (pens 4-31 only)
        move.w  VO_pt_pen(a4),d0
        cmp.w   #4,d0
        blt     .redraw
        bsr     pt_pen_color_ptr    ; a0 -> copper value word
        move.w  (a0),d1
        move.w  d1,d2
        lsr.w   d3,d2
        and.w   #15,d2
        addq.w  #1,d2
        and.w   #15,d2              ; wrap 0-15
        moveq   #15,d4
        lsl.w   d3,d4
        not.w   d4
        and.w   d4,d1
        lsl.w   d3,d2
        or.w    d2,d1
        move.w  d1,(a0)             ; live: the copper reloads each frame
.redraw:
        KCALL   redraw_topmost
        moveq   #0,d0
        rts

; pt_pen_color_ptr - a0 -> the copper COLOR<pen> value word. a4 = &vars
pt_pen_color_ptr:
        KDATA   a0,cop_colptr
        move.l  (a0),a0             ; deref: a0 = copper list pointer
        move.w  VO_pt_pen(a4),d0
        lsl.w   #2,d0               ; 2 words per copper MOVE
        lea     2(a0,d0.w),a0
        rts

; pt_clear - canvas to the background pen
pt_clear:
        movem.l d0-d1/a0,-(sp)
        KDATA   a0,pt_canvas
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

; pt_save / pt_load - PAINT.UNO on the DF1 FAT12 volume. a4 = &vars
pt_save:
        movem.l d0-d1/a0-a1,-(sp)
        move.b  VO_fat_mounted(a4),d0
        beq     .out
        lea     str_pt_name(pc),a0
        KDATA   a1,pt_canvas
        move.l  #PT_W*PT_H,d1
        KCALL   fat_save_file
        KCALL   fat_list_root
.out:   movem.l (sp)+,d0-d1/a0-a1
        rts

pt_load:
        movem.l d0-d1/a0-a1,-(sp)
        move.b  VO_fat_mounted(a4),d0
        beq     .out
        lea     str_pt_name(pc),a0
        KCALL   fat_find_file
        tst.w   d0
        bmi     .out
        move.l  #PT_W*PT_H,d1
        KDATA   a1,pt_canvas
        KCALL   fat_read_file
.out:   movem.l (sp)+,d0-d1/a0-a1
        rts

; ---------------------------------------------------------------- data
str_pt_foot:    dc.b    "1-0:tool [/]:pen r/g/b:tune n:new s/l:disk",0
str_pt_name:    dc.b    "PAINT   UNO"
        even
        end
