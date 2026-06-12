; ============================================================================
; UnoDOS/Genesis theme engine + Theme app (proc 8) - milestone 3.
;
; The 8 preset palettes shared with the other ports (PORT-SPEC slot
; roles: 0 = desktop, 1 = accent, 2 = accent2, 3 = text), stored as
; Genesis CRAM words ($0BGR, 3-bit channels - every color here is the
; Amiga preset's $0RGB with R and B swapped and the low bit dropped).
; Preset 1 "Classic VGA" reproduces the boot palette exactly.
;
; Applying a theme rewrites the themed entries of all four CRAM palette
; lines (the UI attribute schemes); the fixed game colors (entries 5-15
; of lines 2/3) are untouched. CRAM writes happen here, in main-loop
; context only, like every other VDP access.
;
; Custom mode edits the active colors one 3-bit channel at a time
; (r/g/b keys, +2 per press, wraps), like the Amiga port's 4-bit
; editor scaled to Genesis color depth.
; ============================================================================

; theme_init - boot: active colors = Classic VGA (matches pal_data)
theme_init:
        movem.l d0/a0-a1/a4,-(sp)
        lea     VARS,a4
        lea     thm_presets(pc),a0
        lea     v_theme(a4),a1
        move.l  (a0)+,(a1)+
        move.l  (a0),(a1)
        clr.w   v_thm_sel(a4)
        clr.w   v_thm_slot(a4)
        movem.l (sp)+,d0/a0-a1/a4
        rts

; apply_theme - write the active colors into the themed CRAM entries
; (main-loop context only). thm_map = (CRAM index, role) byte pairs.
apply_theme:
        movem.l d0-d3/a0-a1/a4,-(sp)
        lea     VARS,a4
        lea     thm_map(pc),a0
.ent:   moveq   #0,d0
        move.b  (a0)+,d0
        bmi     .done               ; $FF terminator
        moveq   #0,d1
        move.b  (a0)+,d1            ; role 0-3
        add.w   d1,d1
        lea     v_theme(a4),a1
        move.w  (a1,d1.w),d2        ; the role's color
        add.w   d0,d0               ; CRAM byte address
        moveq   #0,d3
        move.w  d0,d3
        swap    d3
        or.l    #$C0000000,d3       ; CRAM write
        move.l  d3,VDP_CTRL
        move.w  d2,VDP_DATA
        bra     .ent
.done:  movem.l (sp)+,d0-d3/a0-a1/a4
        rts

; themed CRAM entries: (color index, role) - see kernel.asm pal_data
; line 0 NORM: 0 backdrop, 1 text, 2 desktop, 3 accent, 4 accent2,
;              13-15 cursor sprite (accent, desktop, text)
; line 1 INV:  1 desktop, 2 text, 3 accent, 4 accent2
; line 2 ACC:  0 desktop, 1 accent, 2 desktop, 3 text, 4 accent2
; line 3 KEY:  0 desktop, 1 desktop, 2 accent, 3 text, 4 accent2
thm_map:
        dc.b    0,0,  1,3,  2,0,  3,1,  4,2,  13,1, 14,0, 15,3
        dc.b    16,0, 17,0, 18,3, 19,1, 20,2
        dc.b    32,0, 33,1, 34,0, 35,3, 36,2
        dc.b    48,0, 49,0, 50,1, 51,3, 52,2
        dc.b    $FF
        even

; ----------------------------------------------------------------------------
; theme_draw - a2 = window
; ----------------------------------------------------------------------------
theme_draw:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     VARS,a4
        ; clear content
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        subq.w  #2,d3
        move.w  #ATTR_NORM+T_SOLBG,d4
        bsr     fill_cells
        ; preset rows
        move.w  WX(a2),d6
        addq.w  #2,d6
        move.w  WY(a2),d5
        addq.w  #2,d5
        moveq   #0,d7
.row:   move.w  #ATTR_NORM,d4
        cmp.w   v_thm_sel(a4),d7
        bne     .attr
        move.w  #ATTR_INV,d4
        ; selection bar
        movem.l d1/d3,-(sp)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  d5,d1
        add.w   d7,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        moveq   #1,d3
        movem.l d4,-(sp)
        add.w   #T_SOLBG,d4
        bsr     fill_cells
        movem.l (sp)+,d4
        movem.l (sp)+,d1/d3
.attr:  move.w  d7,d0
        lsl.w   #2,d0
        lea     thm_name_tab(pc),a0
        move.l  (a0,d0.w),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   d7,d1
        bsr     draw_str
        addq.w  #1,d7
        cmp.w   #8,d7
        blt     .row
        ; custom editor line: "Slot n  R/G/B x/x/x" (3-bit steps 0-7)
        lea     v_npstat(a4),a0
        lea     str_th_slot(pc),a1
        bsr     str_append
        move.w  v_thm_slot(a4),d0
        bsr     fmt_dec
.s1:    tst.b   (a0)+
        bne     .s1
        subq.l  #1,a0
        lea     str_th_rgb(pc),a1
        bsr     str_append
        move.w  v_thm_slot(a4),d0
        add.w   d0,d0
        lea     v_theme(a4),a1
        move.w  (a1,d0.w),d1        ; $0BGR
        move.w  d1,d0
        lsr.w   #1,d0
        and.w   #7,d0               ; R
        bsr     fmt_dec
.s2:    tst.b   (a0)+
        bne     .s2
        subq.l  #1,a0
        move.b  #'/',(a0)+
        move.w  d1,d0
        lsr.w   #5,d0
        and.w   #7,d0               ; G
        bsr     fmt_dec
.s3:    tst.b   (a0)+
        bne     .s3
        subq.l  #1,a0
        move.b  #'/',(a0)+
        move.w  d1,d0
        moveq   #9,d2
        lsr.w   d2,d0
        and.w   #7,d0               ; B
        bsr     fmt_dec
        lea     v_npstat(a4),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   #9,d1
        move.w  #ATTR_NORM,d4
        bsr     draw_str
        ; footer
        lea     str_th_foot(pc),a0
        move.w  d6,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #2,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; ----------------------------------------------------------------------------
; theme_key - d1 = ascii, d2 = raw -> d0 = 0 consumed / 1 not
; ----------------------------------------------------------------------------
theme_key:
        lea     VARS,a4
        cmp.b   #$4C,d2             ; up
        beq     .up
        cmp.b   #$4D,d2             ; down
        beq     .down
        cmp.b   #$4F,d2             ; left = prev slot
        beq     .slotl
        cmp.b   #$4E,d2             ; right = next slot
        beq     .slotr
        cmp.b   #13,d1              ; Enter (pad C) = apply preset
        beq     .apply
        cmp.b   #'r',d1
        beq     .incr
        cmp.b   #'g',d1
        beq     .incg
        cmp.b   #'b',d1
        beq     .incb
        moveq   #1,d0               ; not consumed (incl. ESC)
        rts
.up:    move.w  v_thm_sel(a4),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,v_thm_sel(a4)
        bra     .redraw
.down:  move.w  v_thm_sel(a4),d0
        cmp.w   #7,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,v_thm_sel(a4)
        bra     .redraw
.slotl: move.w  v_thm_slot(a4),d0
        subq.w  #1,d0
        bge     .slotok
        moveq   #3,d0
.slotok:
        move.w  d0,v_thm_slot(a4)
        bra     .redraw
.slotr: move.w  v_thm_slot(a4),d0
        addq.w  #1,d0
        cmp.w   #4,d0
        blt     .slotok
        moveq   #0,d0
        bra     .slotok
.apply: move.w  v_thm_sel(a4),d0
        lsl.w   #3,d0               ; *8 bytes per preset
        lea     thm_presets(pc),a0
        lea     (a0,d0.w),a0
        lea     v_theme(a4),a1
        move.l  (a0)+,(a1)+
        move.l  (a0),(a1)
        bsr     apply_theme
        bra     .redraw
.incr:  moveq   #0,d3               ; R nibble shift
        bra     .inc
.incg:  moveq   #4,d3
        bra     .inc
.incb:  moveq   #8,d3
.inc:   move.w  v_thm_slot(a4),d0
        add.w   d0,d0
        lea     v_theme(a4),a0
        lea     (a0,d0.w),a0
        move.w  (a0),d1             ; $0BGR
        move.w  d1,d2
        lsr.w   d3,d2
        and.w   #15,d2
        addq.w  #2,d2               ; 3-bit step
        and.w   #14,d2              ; wrap
        moveq   #15,d4
        lsl.w   d3,d4
        not.w   d4
        and.w   d4,d1               ; clear the channel
        lsl.w   d3,d2
        or.w    d2,d1
        move.w  d1,(a0)
        bsr     apply_theme
        bra     .redraw
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

; ---------------------------------------------------------------- data
str_t_theme:    dc.b    "Theme",0
name_theme:     dc.b    "Theme",0
str_th_slot:    dc.b    "Custom slot ",0
str_th_rgb:     dc.b    " RGB ",0
str_th_foot:    dc.b    "C:apply r/g/b </>:slot",0
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

; 8 presets x 4 colors (desktop, accent, accent2, text), Genesis $0BGR.
; Converted from the shared $0RGB presets (amiga/theme.i): swap R<->B,
; mask each channel to 3 bits. Preset 1 = the boot palette.
thm_presets:
        dc.w    $0A00,$0AA0,$0A0A,$0EEE     ; Classic VGA
        dc.w    $0000,$0E44,$0AAA,$0EEE     ; Midnight
        dc.w    $0040,$04A4,$04EE,$0EEE     ; Forest
        dc.w    $0004,$044E,$00AE,$0EEE     ; Sunset
        dc.w    $0400,$0A80,$0EE4,$0EEE     ; Ocean
        dc.w    $0422,$0A88,$0CCC,$0EEE     ; Slate
        dc.w    $0404,$0E4E,$0EE4,$0EEE     ; Candy
        dc.w    $0000,$004A,$00AE,$0EEE     ; Amber
