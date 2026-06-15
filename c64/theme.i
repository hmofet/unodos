; ============================================================================
; UnoDOS/C64 Theme app (milestone 3) - desktop colour-scheme picker. The C64's
; per-cell colour makes this the cheapest, most striking app on the machine: a
; preset is just a (desktop colour, VIC border colour) pair, and applying it
; re-tints the whole dithered desktop + border in one redraw.
;
; theme_desk / theme_border (kernel vars) are read by draw_desktop /
; restore_desktop; this app rewrites them. Full-screen list (app_mode=3):
; CRSR up/down move the cursor, RETURN/SPACE apply, RUN/STOP returns.
; ============================================================================

NTHEMES equ 6

theme_open:
        lda #3
        sta app_mode
        lda th_cur
        sta th_sel
        jsr sid_click
        jmp theme_draw

theme_close:
        lda #0
        sta app_mode
        jsr draw_desktop        ; redraws with the (possibly new) theme
        jsr draw_sysinfo_win
        jsr draw_clock_win
        jmp draw_icons

theme_key:
        lda zpTmp
        cmp #K_ESC
        bne tk_n1
        jmp theme_close
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
        sta th_sel
        jmp theme_draw
tk_apply:
        lda th_sel
        sta th_cur
        jsr theme_apply
        jsr sid_click
        jmp theme_draw

; theme_apply - set theme_desk/theme_border from preset th_cur.
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
        stx zpSlot
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
