; ============================================================================
; UnoDOS/AppleII Theme app (milestone 3) - desktop dither-scheme picker.
;
; The renderer's chrome fills (desktop dither, window/icon borders, focused
; title bars, separator lines, selected-icon labels - dither_rect/
; frame_rect/draw_win/draw_icon in kernel.s) all read from the mutable
; 4-byte pat_tab (defined in kernel.s, just above the string table):
;   [0] dither even-row pattern   [1] dither odd-row pattern
;   [2] accent ("on": borders, focused titles, selected icon labels)
;   [3] background ("off": unfocused titles, unselected icon labels)
; theme_apply copies a preset's 4 bytes into pat_tab and repaints the whole
; desktop, so it is a live preview. Window/Files/Notepad CONTENT areas keep
; a fixed black background and white-on-black/black-on-white text under
; every preset, for legibility - only the chrome fills above are themeable
; (documented deviation from macplus's full 4-logical-color pat_tab).
;
;   left/right  select preset        Return  apply (live preview)
;   ESC         back to desktop (pat_tab keeps whatever was last applied)
; ============================================================================

TH_NPRESETS equ 6

; preset table: 4 bytes each, matching pat_tab's layout above.
th_presets:
        dc.b $55,$2A,$7F,$00   ; 0 Classic    - 50% checkerboard desktop
        dc.b $01,$08,$7F,$00   ; 1 Light dots - sparse desktop speckle
        dc.b $7E,$77,$7F,$00   ; 2 Dense dots - heavy desktop speckle
        dc.b $7F,$00,$7F,$00   ; 3 Stripes    - horizontal desktop bands
        dc.b $55,$55,$55,$00   ; 4 Pinstripe  - vertical lines, striped chrome
        dc.b $2A,$55,$00,$7F   ; 5 Inverted   - preset 0 XORed with $7F

th_name0: dc.b "Classic",0
th_name1: dc.b "Light dots",0
th_name2: dc.b "Dense dots",0
th_name3: dc.b "Stripes",0
th_name4: dc.b "Pinstripe",0
th_name5: dc.b "Inverted",0

th_name_lo: dc.b <th_name0,<th_name1,<th_name2,<th_name3,<th_name4,<th_name5
th_name_hi: dc.b >th_name0,>th_name1,>th_name2,>th_name3,>th_name4,>th_name5

; ---------------------------------------------------------------- theme_open
; theme_open - Return pressed on the Theme icon: enter Theme app mode with
; the cursor on the currently-applied preset.
theme_open:
        lda #3
        sta app_mode
        lda th_cur
        sta th_sel
        jsr beep_click          ; app launch
        jmp theme_draw

; --------------------------------------------------------------- theme_close
; theme_close - ESC in Theme: back to the desktop (pat_tab unchanged).
theme_close:
        lda #0
        sta app_mode
        jsr draw_desktop
        jsr draw_sysinfo_win
        jsr draw_clock_win
        jmp draw_icons

; ----------------------------------------------------------------- theme_key
; theme_key - route a key while Theme is active (zpTmp = key code).
theme_key:
        lda zpTmp
        cmp #$9B                ; ESC
        beq theme_close
        cmp #$95                ; right
        beq tk_right
        cmp #$88                ; left
        beq tk_left
        cmp #$8D                ; Return
        beq tk_apply
        rts
tk_left:
        lda th_sel
        bne tkl_dec
        lda #(TH_NPRESETS-1)
        sta th_sel
        jmp theme_draw
tkl_dec:
        dec th_sel
        jmp theme_draw
tk_right:
        lda th_sel
        cmp #(TH_NPRESETS-1)
        bne tkr_inc
        lda #0
        sta th_sel
        jmp theme_draw
tkr_inc:
        inc th_sel
        jmp theme_draw
tk_apply:
        jmp theme_apply

; --------------------------------------------------------------- theme_apply
; theme_apply - Return: copy th_presets[th_sel] into pat_tab, remember it as
; th_cur, and repaint the desktop + open windows + icons (live preview)
; before redrawing the Theme app itself.
theme_apply:
        lda th_sel
        sta th_cur
        asl
        asl
        tax                     ; X = th_sel*4
        ldy #0
ta_cp:
        lda th_presets,x
        sta pat_tab,y
        inx
        iny
        cpy #4
        bne ta_cp
        jsr draw_desktop
        jsr draw_sysinfo_win
        jsr draw_clock_win
        jsr draw_icons
        jmp theme_draw

; ---------------------------------------------------------------- theme_draw
; theme_draw - full redraw: title, separator, preset list (selection bar +
; "* applied" marker), help line.
theme_draw:
; ---- title row ----
        lda #0
        sta zpFX
        lda #0
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #8
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows

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

; ---- separator line ----
        lda #0
        sta zpFX
        lda #7
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #1
        sta zpFH
        lda #$7F
        sta zpFPat
        jsr fill_rows

; ---- clear content + status area (rows1-23) ----
        lda #0
        sta zpFX
        lda #APP_CONTENT_Y
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #(APP_CONTENT_H+8)
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows

; ---- preset list (rows1-6) ----
        ldx #0
td_row:
        cpx #TH_NPRESETS
        beq td_status
        lda #1
        sta zpCol
        txa
        clc
        adc #1
        sta zpRow

        lda #0
        sta zpInv
        cpx th_sel
        bne td_pat
        lda #$7F
        sta zpInv
td_pat:
        lda zpInv
        beq td_nohi
        lda #0
        sta zpFX
        lda zpRow
        asl
        asl
        asl
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #8
        sta zpFH
        lda #$7F
        sta zpFPat
        jsr fill_rows
td_nohi:
        lda th_name_lo,x
        sta zpPtr
        lda th_name_hi,x
        sta zpPtr+1
        jsr draw_string

        cpx th_cur
        bne td_next
        lda #20
        sta zpCol
        lda #<msg_theme_applied
        sta zpPtr
        lda #>msg_theme_applied
        sta zpPtr+1
        jsr draw_string
td_next:
        inx
        jmp td_row

; ---- help line (row23) ----
td_status:
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_theme_help
        sta zpPtr
        lda #>msg_theme_help
        sta zpPtr+1
        jmp draw_string

msg_theme_title:   dc.b "Theme",0
msg_theme_applied: dc.b " * applied",0
msg_theme_help:    dc.b "Left/Right select Enter apply  Esc back",0
