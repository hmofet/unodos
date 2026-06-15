; ============================================================================
; UnoDOS/68K Theme - DISK-LOADED app (THEME.APP, proc 5). Was theme.i's
; theme_draw/theme_key built into the kernel; now a separate -Fbin binary
; loaded off DF1 into its window slot and dispatched through the JMP table
; below. Links to the kernel only through the APIVEC table (see sysabi.i):
; kernel routines via KCALL, shared kernel data via KDATA + VO_* offsets.
;
; The 8 preset palettes + custom RGB editor are unchanged; the live palette
; (theme_pal) and the copper color pointer (cop_colptr) stay kernel-owned
; (apply_theme, also kernel-resident, writes them through the copper list).
; ============================================================================

        mc68000
        include "sysabi.i"
        include "build/kernel_api.inc"      ; VO_* vars offsets + symbol check

; All apps assemble at APPSLOT0; the image is position-independent (self refs
; are PC-relative, kernel refs are absolute through APIVEC), so the loader can
; place it at any window slot and it still runs correctly.
        org     APPSLOT0

; ---- JMP table (the app ABI: open/draw/key/tick). PC-relative so the image
; runs correctly at whatever slot the loader places it. ----
        jmp     theme_open(pc)
        jmp     theme_draw(pc)
        jmp     theme_key(pc)
        jmp     theme_tick(pc)

; ---------------------------------------------------------------------------
theme_open:
        ; nothing to initialize - thm_sel/thm_slot persist in kernel vars
        rts

theme_tick:
        rts

; theme_draw - a2 = window
theme_draw:
        movem.l d0-d7/a0-a4,-(sp)
        KDATA   a4,vars                 ; a4 = &vars (thm_sel/thm_slot live here)
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
        KCALL   fill_rect
        ; preset rows
        move.w  WX(a2),d6
        addq.w  #6,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+3,d5
        moveq   #0,d7                   ; row index
.row:   move.w  VO_thm_sel(a4),d0
        cmp.w   d0,d7
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
        KCALL   fill_rect
.nobar:
        move.w  d7,d0
        lsl.w   #2,d0
        lea     thm_name_tab(pc),a0
        move.l  (a0,d0.w),a0
        move.w  d6,d0
        move.w  d5,d1
        move.w  VO_thm_sel(a4),d2
        cmp.w   d2,d7
        beq     .selrow
        moveq   #3,d2
        KCALL   draw_string
        bra     .drawn
.selrow:
        moveq   #0,d2                   ; desktop-color text on the accent bar
        moveq   #1,d3
        KCALL   draw_string_bg
.drawn:
        add.w   #10,d5
        addq.w  #1,d7
        cmp.w   #8,d7
        blt     .row
        ; custom editor line: "Slot n  R G B" with per-channel values
        addq.w  #4,d5
        KDATA   a0,npstat               ; a0 = shared scratch line
        lea     str_th_slot(pc),a1
        KCALL   str_append
        move.w  VO_thm_slot(a4),d0
        KCALL   fmt_dec
.s1:    tst.b   (a0)+
        bne     .s1
        subq.l  #1,a0
        lea     str_th_rgb(pc),a1
        KCALL   str_append
        ; current slot value nibbles
        move.w  VO_thm_slot(a4),d0
        add.w   d0,d0
        KDATA   a1,theme_pal
        move.w  (a1,d0.w),d1            ; $0RGB
        move.w  d1,d0
        lsr.w   #8,d0
        and.w   #15,d0
        KCALL   fmt_dec
.s2:    tst.b   (a0)+
        bne     .s2
        subq.l  #1,a0
        move.b  #'/',(a0)+
        move.w  d1,d0
        lsr.w   #4,d0
        and.w   #15,d0
        KCALL   fmt_dec
.s3:    tst.b   (a0)+
        bne     .s3
        subq.l  #1,a0
        move.b  #'/',(a0)+
        move.w  d1,d0
        and.w   #15,d0
        KCALL   fmt_dec
        KDATA   a0,npstat
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        KCALL   draw_string
        ; footer
        add.w   #12,d5
        lea     str_th_foot(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #1,d2
        KCALL   draw_string
        movem.l (sp)+,d0-d7/a0-a4
        rts

; theme_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
theme_key:
        KDATA   a4,vars
        cmp.b   #$4C,d2                 ; up
        beq     .up
        cmp.b   #$4D,d2                 ; down
        beq     .down
        cmp.b   #$4F,d2                 ; left  = prev slot
        beq     .slotl
        cmp.b   #$4E,d2                 ; right = next slot
        beq     .slotr
        cmp.b   #13,d1                  ; Enter = apply preset
        beq     .apply
        cmp.b   #'r',d1
        beq     .incr
        cmp.b   #'g',d1
        beq     .incg
        cmp.b   #'b',d1
        beq     .incb
        moveq   #1,d0                   ; not consumed (incl. ESC)
        rts
.up:    move.w  VO_thm_sel(a4),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,VO_thm_sel(a4)
        bra     .redraw
.down:  move.w  VO_thm_sel(a4),d0
        cmp.w   #7,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,VO_thm_sel(a4)
        bra     .redraw
.slotl: move.w  VO_thm_slot(a4),d0
        subq.w  #1,d0
        bge     .slotok
        moveq   #3,d0
.slotok:
        move.w  d0,VO_thm_slot(a4)
        bra     .redraw
.slotr: move.w  VO_thm_slot(a4),d0
        addq.w  #1,d0
        cmp.w   #4,d0
        blt     .slotok
        moveq   #0,d0
        bra     .slotok
.apply: move.w  VO_thm_sel(a4),d0
        lsl.w   #3,d0                   ; *8 bytes per preset
        lea     thm_presets(pc),a0
        lea     (a0,d0.w),a0
        KDATA   a1,theme_pal
        move.l  (a0)+,(a1)+
        move.l  (a0),(a1)
        KCALL   apply_theme
        bra     .redraw
.incr:  moveq   #8,d3                   ; shift for R nibble
        bra     .inc
.incg:  moveq   #4,d3
        bra     .inc
.incb:  moveq   #0,d3
.inc:   move.w  VO_thm_slot(a4),d0
        add.w   d0,d0
        KDATA   a0,theme_pal
        lea     (a0,d0.w),a0
        move.w  (a0),d1                 ; $0RGB
        move.w  d1,d2
        lsr.w   d3,d2
        and.w   #15,d2
        addq.w  #1,d2
        and.w   #15,d2                  ; wrap
        moveq   #15,d4
        lsl.w   d3,d4
        not.w   d4
        and.w   d4,d1                   ; clear channel
        lsl.w   d3,d2
        or.w    d2,d1
        move.w  d1,(a0)
        KCALL   apply_theme
        bra     .redraw
.redraw:
        KCALL   redraw_topmost
        moveq   #0,d0
        rts

; ---- app-private data (moved out of the kernel) ----
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
; 8 presets x 4 colors, Amiga 12-bit $0RGB.
thm_presets:
        dc.w    $000A,$00AA,$0A0A,$0FFF     ; Classic VGA
        dc.w    $0000,$055F,$0AAA,$0FFF     ; Midnight
        dc.w    $0050,$05A5,$0FF5,$0FFF     ; Forest
        dc.w    $0500,$0F55,$0FA0,$0FFF     ; Sunset
        dc.w    $0005,$008A,$05FF,$0FFF     ; Ocean
        dc.w    $0334,$088A,$0CCD,$0FFF     ; Slate
        dc.w    $0505,$0F5F,$05FF,$0FFF     ; Candy
        dc.w    $0000,$0A50,$0FA0,$0FFF     ; Amber
        end
