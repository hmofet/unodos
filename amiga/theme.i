; ============================================================================
; UnoDOS/68K theme engine + Theme app (proc 5) + boot splash
;
; 8 preset palettes shared with the other ports (slot roles: 0 = desktop,
; 1 = accent/cyan-role, 2 = accent2/magenta-role, 3 = text/white-role).
; Preset 1 "Classic" is the PC VGA palette. Custom mode edits the live
; palette one 4-bit channel at a time (Amiga 12-bit color).
; ============================================================================

; apply_theme - write theme_pal into the copper list COLOR00-03 value words
apply_theme:
        movem.l a0-a1,-(sp)
        move.l  cop_colptr(pc),a0
        lea     theme_pal(pc),a1
        move.w  (a1)+,2(a0)
        move.w  (a1)+,6(a0)
        move.w  (a1)+,10(a0)
        move.w  (a1)+,14(a0)
        movem.l (sp)+,a0-a1
        rts

; theme_draw - a2 = window
theme_draw:
        movem.l d0-d7/a0-a4,-(sp)
        ; clear content area
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
        ; preset rows
        move.w  WX(a2),d6
        addq.w  #6,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+3,d5
        moveq   #0,d7               ; row index
.row:   cmp.w   thm_sel(pc),d7
        bne     .nobar
        ; selection bar (accent color, like Files)
        move.w  d6,d0
        subq.w  #2,d0
        move.w  d5,d1
        subq.w  #1,d1
        move.w  WW(a2),d2
        sub.w   #12,d2
        moveq   #10,d3
        moveq   #1,d4
        bsr     fill_rect
.nobar:
        move.w  d7,d0
        lsl.w   #2,d0
        lea     thm_name_tab(pc),a0
        move.l  (a0,d0.w),a0
        move.w  d6,d0
        move.w  d5,d1
        cmp.w   thm_sel(pc),d7
        beq     .selrow
        moveq   #3,d2
        bsr     draw_string
        bra     .drawn
.selrow:
        moveq   #0,d2               ; desktop-color text on the accent bar
        moveq   #1,d3
        bsr     draw_string_bg
.drawn:
        add.w   #10,d5
        addq.w  #1,d7
        cmp.w   #8,d7
        blt     .row
        ; custom editor line: "Slot n  R G B" with per-channel values
        addq.w  #4,d5
        lea     npstat(pc),a0       ; reuse the notepad scratch line
        lea     str_th_slot(pc),a1
        bsr     str_append
        move.w  thm_slot(pc),d0
        bsr     fmt_dec
.s1:    tst.b   (a0)+
        bne     .s1
        subq.l  #1,a0
        lea     str_th_rgb(pc),a1
        bsr     str_append
        ; current slot value nibbles
        move.w  thm_slot(pc),d0
        add.w   d0,d0
        lea     theme_pal(pc),a1
        move.w  (a1,d0.w),d1        ; $0RGB
        move.w  d1,d0
        lsr.w   #8,d0
        and.w   #15,d0
        bsr     fmt_dec
.s2:    tst.b   (a0)+
        bne     .s2
        subq.l  #1,a0
        move.b  #'/',(a0)+
        move.w  d1,d0
        lsr.w   #4,d0
        and.w   #15,d0
        bsr     fmt_dec
.s3:    tst.b   (a0)+
        bne     .s3
        subq.l  #1,a0
        move.b  #'/',(a0)+
        move.w  d1,d0
        and.w   #15,d0
        bsr     fmt_dec
        lea     npstat(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        ; footer
        add.w   #12,d5
        lea     str_th_foot(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #1,d2
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
        cmp.b   #$4F,d2             ; left  = prev slot
        beq     .slotl
        cmp.b   #$4E,d2             ; right = next slot
        beq     .slotr
        cmp.b   #13,d1              ; Enter = apply preset
        beq     .apply
        cmp.b   #'r',d1
        beq     .incr
        cmp.b   #'g',d1
        beq     .incg
        cmp.b   #'b',d1
        beq     .incb
        moveq   #1,d0               ; not consumed (incl. ESC)
        rts
.up:    move.w  thm_sel(pc),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,thm_sel-vars(a4)
        bra     .redraw
.down:  move.w  thm_sel(pc),d0
        cmp.w   #7,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,thm_sel-vars(a4)
        bra     .redraw
.slotl: move.w  thm_slot(pc),d0
        subq.w  #1,d0
        bge     .slotok
        moveq   #3,d0
.slotok:
        move.w  d0,thm_slot-vars(a4)
        bra     .redraw
.slotr: move.w  thm_slot(pc),d0
        addq.w  #1,d0
        cmp.w   #4,d0
        blt     .slotok
        moveq   #0,d0
        bra     .slotok
.apply: move.w  thm_sel(pc),d0
        lsl.w   #3,d0               ; *8 bytes per preset
        lea     thm_presets(pc),a0
        lea     (a0,d0.w),a0
        lea     theme_pal(pc),a1
        move.l  (a0)+,(a1)+
        move.l  (a0),(a1)
        bsr     apply_theme
        bra     .redraw
.incr:  moveq   #8,d3               ; shift for R nibble
        bra     .inc
.incg:  moveq   #4,d3
        bra     .inc
.incb:  moveq   #0,d3
.inc:   move.w  thm_slot(pc),d0
        add.w   d0,d0
        lea     theme_pal(pc),a0
        lea     (a0,d0.w),a0
        move.w  (a0),d1             ; $0RGB
        move.w  d1,d2
        lsr.w   d3,d2
        and.w   #15,d2
        addq.w  #1,d2
        and.w   #15,d2              ; wrap
        moveq   #15,d4
        lsl.w   d3,d4
        not.w   d4
        and.w   d4,d1               ; clear channel
        lsl.w   d3,d2
        or.w    d2,d1
        move.w  d1,(a0)
        bsr     apply_theme
        bra     .redraw
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

; ============================================================================
; Boot splash: striped Amiga checkmark + "UnoDOS 3" at 2x, ~2s hold
; ============================================================================

; splash_text2x - a0 = string, d0 = x, d1 = y, d2 = color (8x8 font at 2x)
splash_text2x:
        movem.l d0-d7/a0-a2,-(sp)
        move.w  d2,d7               ; color
        move.w  d0,d5               ; pen x
        move.w  d1,d6               ; top y
.ch:    moveq   #0,d0
        move.b  (a0)+,d0
        beq     .done
        sub.w   #32,d0
        lsl.w   #3,d0
        lea     font8x8(pc),a1
        lea     (a1,d0.w),a1        ; glyph rows
        moveq   #0,d3               ; row
.row:   moveq   #0,d2
        move.b  (a1,d3.w),d2        ; row bits (bit 7 = leftmost)
        moveq   #0,d4               ; bit
.bit:   btst    #7,d2
        beq     .nopix
        movem.l d2-d4,-(sp)
        move.w  d4,d0
        add.w   d0,d0
        add.w   d5,d0               ; x + bit*2
        move.w  d3,d1
        add.w   d1,d1
        add.w   d6,d1               ; y + row*2
        moveq   #2,d2
        moveq   #2,d3
        move.w  d7,d4
        bsr     fill_rect
        movem.l (sp)+,d2-d4
.nopix: lsl.b   #1,d2
        addq.w  #1,d4
        cmp.w   #8,d4
        blt     .bit
        addq.w  #1,d3
        cmp.w   #8,d3
        blt     .row
        add.w   #16,d5              ; 2x advance
        bra     .ch
.done:  movem.l (sp)+,d0-d7/a0-a2
        rts

; splash_check_stroke - one striped checkmark stroke set at y offset d5,
; color d6 (draws the full check path as 6x6 blocks)
splash_check_stroke:
        movem.l d0-d7,-(sp)
        ; down-stroke: 14 steps from (104, 52) slope +1
        moveq   #0,d7
.seg1:  move.w  d7,d0
        add.w   d0,d0
        add.w   #104,d0
        move.w  d7,d1
        add.w   d1,d1
        add.w   #52,d1
        add.w   d5,d1
        moveq   #6,d2
        moveq   #6,d3
        move.w  d6,d4
        bsr     fill_rect
        addq.w  #1,d7
        cmp.w   #14,d7
        blt     .seg1
        ; up-stroke: 28 steps from (132, 78) slope -1
        moveq   #0,d7
.seg2:  move.w  d7,d0
        add.w   d0,d0
        add.w   #132,d0
        move.w  #78,d1
        move.w  d7,d2
        add.w   d2,d2
        sub.w   d2,d1
        add.w   d5,d1
        moveq   #6,d2
        moveq   #6,d3
        move.w  d6,d4
        bsr     fill_rect
        addq.w  #1,d7
        cmp.w   #28,d7
        blt     .seg2
        movem.l (sp)+,d0-d7
        rts

; splash_wait_frames - d0 = frame count (polls VPOSR, no ISR dependency)
splash_wait_frames:
        movem.l d0-d1/a6,-(sp)
        lea     CUSTOM,a6
.frame: move.l  4(a6),d1            ; VPOSR/VHPOSR
        and.l   #$0001FF00,d1
        cmp.l   #$00012C00,d1       ; raster line 300
        bne     .frame
.inline:
        move.l  4(a6),d1
        and.l   #$0001FF00,d1
        cmp.l   #$00012C00,d1
        beq     .inline
        subq.w  #1,d0
        bne     .frame
        movem.l (sp)+,d0-d1/a6
        rts

; splash_show - draw splash, hold ~2s, clear (call with display DMA on)
splash_show:
        movem.l d0-d7/a0,-(sp)
        ; striped checkmark: three strokes, colors 3/1/2 top to bottom
        move.w  #-9,d5
        moveq   #3,d6
        bsr     splash_check_stroke
        moveq   #0,d5
        moveq   #1,d6
        bsr     splash_check_stroke
        moveq   #9,d5
        moveq   #2,d6
        bsr     splash_check_stroke
        ; title at 2x: "UnoDOS 3" = 8 chars * 16px = 128 wide
        lea     str_splash1(pc),a0
        move.w  #96,d0
        move.w  #126,d1
        moveq   #3,d2
        bsr     splash_text2x
        ; subtitle
        lea     str_splash2(pc),a0
        move.w  #84,d0
        move.w  #152,d1
        moveq   #1,d2
        bsr     draw_string
        move.w  #100,d0             ; ~2s PAL
        bsr     splash_wait_frames
        bsr     clear_screen
        movem.l (sp)+,d0-d7/a0
        rts

str_splash1:    dc.b    "UnoDOS 3",0
str_splash2:    dc.b    "for Commodore Amiga",0
str_t_theme:    dc.b    "Theme",0
name_theme:     dc.b    "Theme",0
str_th_slot:    dc.b    "Custom  Slot ",0
str_th_rgb:     dc.b    "  R/G/B ",0
str_th_foot:    dc.b    "Enter:apply r/g/b:tune </>:slot",0
str_th_1:       dc.b    "Classic VGA",0
str_th_2:       dc.b    "Midnight",0
str_th_3:       dc.b    "Forest",0
str_th_4:       dc.b    "Sunset",0
str_th_5:       dc.b    "Ocean",0
str_th_6:       dc.b    "Slate",0
str_th_7:       dc.b    "Candy",0
str_th_8:       dc.b    "Amber",0

        even
thm_name_tab:
        dc.l    str_th_1
        dc.l    str_th_2
        dc.l    str_th_3
        dc.l    str_th_4
        dc.l    str_th_5
        dc.l    str_th_6
        dc.l    str_th_7
        dc.l    str_th_8

; 8 presets x 4 colors, Amiga 12-bit $0RGB. Slot roles per PORT-SPEC:
; desktop, accent, accent2, text. Preset 1 = the PC VGA palette.
thm_presets:
        dc.w    $000A,$00AA,$0A0A,$0FFF     ; Classic VGA
        dc.w    $0000,$055F,$0AAA,$0FFF     ; Midnight
        dc.w    $0050,$05A5,$0FF5,$0FFF     ; Forest
        dc.w    $0500,$0F55,$0FA0,$0FFF     ; Sunset
        dc.w    $0005,$008A,$05FF,$0FFF     ; Ocean
        dc.w    $0334,$088A,$0CCD,$0FFF     ; Slate
        dc.w    $0505,$0F5F,$05FF,$0FFF     ; Candy
        dc.w    $0000,$0A50,$0FA0,$0FFF     ; Amber
