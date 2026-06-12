; ============================================================================
; UnoDOS/MacPlus Theme app (proc 11) - the 1-bit analog of the color
; ports' palette Theme: selectable DITHER SCHEMES. The kernel's four
; logical colors render through the mutable pat_tab (2 bytes per color:
; even/odd row patterns); a preset rewrites all four and repaints, so the
; whole screen is the live preview. Preset 5 is a full video invert
; (every pattern complemented - the classic accessibility look).
;
;   up / down   select preset        Enter   apply
; ============================================================================

THEME_PROC  equ 11
TH_NPRESETS equ 6

; preset table: 8 bytes each = colors 0..3 x (even,odd) row patterns
th_presets:
        dc.b    $00,$00,$88,$22,$AA,$55,$FF,$FF   ; 0 Classic
        dc.b    $00,$00,$44,$11,$AA,$55,$FF,$FF   ; 1 Fine dots
        dc.b    $00,$00,$88,$22,$FF,$00,$FF,$FF   ; 2 Scanlines
        dc.b    $00,$00,$88,$88,$AA,$AA,$FF,$FF   ; 3 Pinstripe
        dc.b    $00,$00,$88,$22,$88,$44,$FF,$FF   ; 4 Diagonal
        dc.b    $FF,$FF,$77,$DD,$55,$AA,$00,$00   ; 5 Inverted

str_th_p0:      dc.b    "Classic",0
str_th_p1:      dc.b    "Fine dots",0
str_th_p2:      dc.b    "Scanlines",0
str_th_p3:      dc.b    "Pinstripe",0
str_th_p4:      dc.b    "Diagonal",0
str_th_p5:      dc.b    "Inverted",0
        even
th_names:
        dc.l    str_th_p0,str_th_p1,str_th_p2
        dc.l    str_th_p3,str_th_p4,str_th_p5

; theme_apply - d0 = preset: copy its patterns into pat_tab + repaint
theme_apply:
        movem.l d0-d1/a0-a1/a4,-(sp)
        lea     vars(pc),a4
        move.w  d0,th_cur-vars(a4)
        lsl.w   #3,d0
        lea     th_presets(pc),a0
        lea     (a0,d0.w),a0
        lea     pat_tab(pc),a1
        moveq   #7,d1
.cp:    move.b  (a0)+,(a1)+
        dbra    d1,.cp
        bsr     repaint_all
        movem.l (sp)+,d0-d1/a0-a1/a4
        rts

; theme_draw - a2 = window
theme_draw:
        movem.l d0-d7/a0-a4,-(sp)
        ; clear content (white)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        sub.w   #TBAR_H+1,d3
        moveq   #0,d4
        bsr     fill_rect
        move.w  WX(a2),d6
        addq.w  #6,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+3,d5
        lea     str_th_title(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        add.w   #14,d5
        ; preset rows: selection bar inverted, applied preset marked '*'
        moveq   #0,d7
.row:   cmp.w   #TH_NPRESETS,d7
        bge     .foot
        ; selection bar
        cmp.w   th_sel(pc),d7
        bne     .name
        movem.l d5-d7,-(sp)
        move.w  WX(a2),d0
        addq.w  #4,d0
        move.w  d5,d1
        subq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #8,d2
        moveq   #10,d3
        moveq   #3,d4
        bsr     fill_rect
        movem.l (sp)+,d5-d7
.name:  move.w  d7,d0
        lsl.w   #2,d0
        lea     th_names(pc),a0
        move.l  (a0,d0.w),a0
        move.w  d6,d0
        add.w   #12,d0
        move.w  d5,d1
        moveq   #3,d2               ; black...
        moveq   #-1,d3
        cmp.w   th_sel(pc),d7
        bne     .dn
        moveq   #0,d2               ; ...white on the selection bar
        moveq   #3,d3
.dn:    bsr     draw_string_bg
        ; applied marker
        cmp.w   th_cur(pc),d7
        bne     .nm
        movem.l d5-d7,-(sp)
        lea     str_th_mark(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        moveq   #-1,d3
        cmp.w   th_sel(pc),d7
        bne     .dm
        moveq   #0,d2
        moveq   #3,d3
.dm:    bsr     draw_string_bg
        movem.l (sp)+,d5-d7
.nm:    add.w   #11,d5
        addq.w  #1,d7
        bra     .row
.foot:  addq.w  #4,d5
        lea     str_th_foot(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        movem.l (sp)+,d0-d7/a0-a4
        rts

; theme_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
theme_key:
        lea     vars(pc),a4
        cmp.b   #$4C,d2             ; up
        beq     .up
        cmp.b   #$4D,d2             ; down
        beq     .down
        cmp.b   #13,d1              ; Enter: apply
        beq     .apply
        moveq   #1,d0
        rts
.up:    move.w  th_sel(pc),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,th_sel-vars(a4)
        bra     .redraw
.down:  move.w  th_sel(pc),d0
        cmp.w   #TH_NPRESETS-1,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,th_sel-vars(a4)
        bra     .redraw
.apply: move.w  th_sel(pc),d0
        bsr     theme_apply         ; repaints everything (live preview)
        moveq   #0,d0
        rts
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

str_th_title:   dc.b    "Desktop dither scheme",0
str_th_mark:    dc.b    "*",0
str_th_foot:    dc.b    "arrows: select  Enter: apply",0
str_t_theme:    dc.b    "Theme",0
name_theme:     dc.b    "Theme",0
        even
