; ============================================================================
; UnoDOS/C64 Theme - a DISK-LOADED app (app3.bin, icon 3). Desktop colour-scheme
; picker. Was built into the kernel (theme.i, app_mode 3); now a full-screen
; disk-loaded app. A preset is a (desktop colour, VIC border colour) pair; the
; kernel vars theme_desk / theme_border (read by the kernel's draw_desktop) are
; exported via the API so this app can rewrite them - on RUN/STOP the app calls
; return_to_desktop, which redraws the launcher with the new theme.
;
; Full-screen list: CRSR up/down move the cursor, RETURN/SPACE apply, STOP back.
; App ABI: first three words are JMPs (init / key / tick). See sys.inc.
; ============================================================================

        processor 6502
        include "sys.inc"
        include "build/kernel_api.inc"

NTHEMES equ 6

        org APP_BASE
        jmp th_init
        jmp theme_key
        jmp th_tick

th_init:
        lda th_cur
        sta th_sel
        jsr sid_click
        jmp theme_draw

th_tick:
        rts

theme_key:
        lda zpTmp
        cmp #K_ESC
        bne tk_n1
        jmp return_to_desktop
tk_n1:
        cmp #K_DOWN
        bne tk_n2
        lda th_sel
        clc
        adc #1
        cmp #NTHEMES
        bcc tk_setsel
        lda #0
        jmp tk_setsel
tk_n2:
        cmp #K_UP
        bne tk_n3
        lda th_sel
        bne tk_dec
        lda #(NTHEMES-1)
        jmp tk_setsel
tk_dec:
        sec
        sbc #1
        jmp tk_setsel
tk_n3:
        cmp #K_RET
        beq tk_apply
        cmp #K_SPACE
        beq tk_apply
        rts
tk_setsel:
        ldx th_sel              ; remember old cursor row for the incremental redraw
        stx th_osel
        sta th_sel
        jmp theme_nav_redraw
tk_apply:
        lda th_sel
        sta th_cur
        jsr theme_apply
        jsr sid_click
        jmp theme_draw

; theme_apply - set theme_desk/theme_border (kernel vars) from preset th_cur.
theme_apply:
        ldx th_cur
        lda th_desk_tab,x
        sta theme_desk
        lda th_border_tab,x
        sta theme_border
        rts

; theme_draw - title + preset list (cursor = th_sel, "*" = applied th_cur) +
; a swatch row tinted with each preset's desktop colour.
theme_draw:
        jsr app_clear
        lda #<msg_theme_title
        sta zpPtr
        lda #>msg_theme_title
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
        ; list presets
        ldx #0
th_row:
        jsr theme_row_draw
        ldx zpSlot
        inx
        cpx #NTHEMES
        beq th_rowdone
        jmp th_row
th_rowdone:
        ; help line
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda #<msg_theme_help
        sta zpPtr
        lda #>msg_theme_help
        sta zpPtr+1
        jmp draw_string

; theme_row_draw - draw one preset row (X = index): "*"/space marker, the
; highlight band if X == th_sel (the band is filled AFTER the marker, so on the
; cursor row it covers the marker - preserved exactly), the name, and the
; colour swatch. zpSlot holds the index on return. Factored verbatim from the
; old th_row loop so the full theme_draw stays byte-identical.
theme_row_draw:
        stx zpSlot
        lda #COL_WIN            ; text colour for marker/name (app_clear default)
        sta zpFCol
        ; row = 2 + x*2
        txa
        asl
        clc
        adc #2
        sta zpRow
        lda #2
        sta zpCol
        ; "*" if applied else " "
        lda #0
        sta zpInv
        ldx zpSlot
        cpx th_cur
        bne th_nostar
        lda #$2A                ; '*'
        jmp th_star
th_nostar:
        lda #$20
th_star:
        jsr draw_char
        ; highlight band if cursor
        ldx zpSlot
        cpx th_sel
        bne th_nohi
        lda #0
        sta zpFX
        lda zpRow
        asl
        asl
        asl
        sta zpFY
        lda #20
        sta zpFW
        lda #8
        sta zpFH
        lda #$FF
        sta zpFPat
        jsr fill_rows
        lda #$FF
        sta zpInv
th_nohi:
        lda #4
        sta zpCol
        ldx zpSlot
        lda th_name_lo,x
        sta zpPtr
        lda th_name_hi,x
        sta zpPtr+1
        jsr draw_string
        ; colour swatch (cols 24..31) tinted with the preset's desktop colour
        ldx zpSlot
        lda th_desk_tab,x
        sta zpFCol
        lda #24
        sta zpCX
        lda zpRow
        sta zpCY
        lda #8
        sta zpCW
        lda #1
        sta zpCH
        jsr color_fill
        lda #24
        sta zpFX
        lda zpRow
        asl
        asl
        asl
        sta zpFY
        lda #8
        sta zpFW
        lda #8
        sta zpFH
        lda #$55
        sta zpFPat
        jsr fill_rows
        lda #COL_WIN             ; restore default text colour for the next row
        sta zpFCol
        rts

; theme_nav_redraw - a cursor move only shifts the highlight band; th_cur (the
; "*"), swatches, title and help line don't change. Erase the old cursor row's
; band (cols 0..19) back to background, then repaint the old row (now
; unselected) and the new row (selected). Byte-identical to a full theme_draw.
theme_nav_redraw:
        lda th_osel             ; old row pixel Y = (2 + osel*2)*8
        asl
        clc
        adc #2
        asl
        asl
        asl
        sta zpFY
        lda #0
        sta zpFX
        lda #20
        sta zpFW
        lda #8
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows
        ldx th_osel
        jsr theme_row_draw
        ldx th_sel
        jsr theme_row_draw
        rts

; preset tables (desktop colour fg<<4|bg, VIC border colour, name)
th_desk_tab:   dc.b $E6,$D5,$A4,$78,$FB,$60
th_border_tab: dc.b $06,$05,$04,$08,$0B,$00
th_name_lo:    dc.b <msg_th_ocean,<msg_th_forest,<msg_th_grape,<msg_th_amber,<msg_th_slate,<msg_th_night
th_name_hi:    dc.b >msg_th_ocean,>msg_th_forest,>msg_th_grape,>msg_th_amber,>msg_th_slate,>msg_th_night
msg_theme_title: dc.b "Theme",0
msg_theme_help:  dc.b "CRSR move  RET apply  STOP back",0
msg_th_ocean:  dc.b "Ocean",0
msg_th_forest: dc.b "Forest",0
msg_th_grape:  dc.b "Grape",0
msg_th_amber:  dc.b "Amber",0
msg_th_slate:  dc.b "Slate",0
msg_th_night:  dc.b "Night",0

; app state (reset on each launch). th_cur starts at 0 (Ocean = the kernel
; default theme_desk/theme_border); applying a preset persists for this run.
th_sel:  dc.b 0
th_osel: dc.b 0       ; prior cursor row (for the incremental nav redraw)
th_cur:  dc.b 0
