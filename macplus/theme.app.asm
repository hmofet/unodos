; ============================================================================
; UnoDOS/MacPlus Theme - DISK-LOADED app (THEME.APP, proc 11, slot 7).
; Desktop dither-scheme picker. Was theme.i (in-kernel); now a separate 68K
; binary on the FAT12 volume, assembled at its slot org and linked to the
; kernel by absolute address via build/kernel_api.inc. The kernel's four
; logical colours render through the mutable pat_tab; a preset rewrites all
; four and repaints (the whole screen is the live preview).
;   up/down select preset   Enter apply
;
; ABI (see diskapp.i): first four longs = JMP draw/key/tick/click.
; build.sh runs this source through mkapp.py first to convert kernel-symbol
; PC-relative refs to absolute (vasm rejects pc-relative to far equates), so
; here we may still write `pat_tab(pc)` etc. for readability.
; ============================================================================

        mc68000
        include "sysequ.i"
        include "build/kernel_api.inc"

TH_NPRESETS equ 6

        org     APP_LOAD+APPSLOT_THEME*APP_SLOT_SZ
        dc.l     theme_draw          ; +0  draw  (d3=proc, a2=window)
        dc.l     theme_key           ; +4  key   (d1=ascii d2=raw a2=win) -> d0
        dc.l     theme_tick          ; +8  tick
        dc.l     theme_click         ; +12 click (unused)

theme_tick:
        rts
theme_click:
        rts

; app-local state (persists while the app is resident in its slot)
th_sel:  dc.w   0                   ; selected preset row
th_cur:  dc.w   0                   ; applied preset

; preset table: 8 bytes each = colours 0..3 x (even,odd) row patterns
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
        move.w  d0,th_cur            ; (kernel var; abs after mkapp)
        lsl.w   #3,d0
        lea     th_presets(pc),a0
        lea     (a0,d0.w),a0
        lea     pat_tab,a1
        moveq   #7,d1
.cp:    move.b  (a0)+,(a1)+
        dbra    d1,.cp
        jsr     repaint_all
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
        jsr     fill_rect
        move.w  WX(a2),d6
        addq.w  #6,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+3,d5
        lea     str_th_title(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        jsr     draw_string
        add.w   #14,d5
        moveq   #0,d7
.row:   cmp.w   #TH_NPRESETS,d7
        bge     .foot
        cmp.w   th_sel,d7
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
        jsr     fill_rect
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
        cmp.w   th_sel,d7
        bne     .dn
        moveq   #0,d2               ; ...white on the selection bar
        moveq   #3,d3
.dn:    jsr     draw_string_bg
        cmp.w   th_cur,d7
        bne     .nm
        movem.l d5-d7,-(sp)
        lea     str_th_mark(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        moveq   #-1,d3
        cmp.w   th_sel,d7
        bne     .dm
        moveq   #0,d2
        moveq   #3,d3
.dm:    jsr     draw_string_bg
        movem.l (sp)+,d5-d7
.nm:    add.w   #11,d5
        addq.w  #1,d7
        bra     .row
.foot:  addq.w  #4,d5
        lea     str_th_foot(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        jsr     draw_string
        movem.l (sp)+,d0-d7/a0-a4
        rts

; theme_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
theme_key:
        cmp.b   #K_UP,d2
        beq     .up
        cmp.b   #K_DOWN,d2
        beq     .down
        cmp.b   #13,d1              ; Enter: apply
        beq     .apply
        moveq   #1,d0
        rts
.up:    move.w  th_sel,d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,th_sel
        bra     .redraw
.down:  move.w  th_sel,d0
        cmp.w   #TH_NPRESETS-1,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,th_sel
        bra     .redraw
.apply: move.w  th_sel,d0
        bsr     theme_apply         ; repaints everything (live preview)
        moveq   #0,d0
        rts
.redraw:
        ; the kernel's app_disp_key redraws the topmost window when we consume
        moveq   #0,d0
        rts

str_th_title:   dc.b    "Desktop dither scheme",0
str_th_mark:    dc.b    "*",0
str_th_foot:    dc.b    "arrows: select  Enter: apply",0
        even
        end
