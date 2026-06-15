; ============================================================================
; UnoDOS/C64 Dostris (milestone 3) - falling-block puzzle, ported from
; apple2/dostris.i. The logic (piece masks, collision, line-clear, scoring,
; gravity) is the Apple II 6502 code essentially unchanged; the C64 difference
; is COLOUR: the board stores piece+1 per cell, and each block draws in its
; tetromino's colour via per-cell screen RAM (the Apple II port is black/white).
;
; Board: 10x20, 1 byte/cell (0 empty, else piece+1) at DOSBOARD ($4C00). Each
; cell renders as one 8x8 char-cell. Full-screen (app_mode=4): board at cells
; (2..11, rows 1..20), HUD at cols 14..39.
;
;   CRSR left/right  move      CRSR up   rotate (CW)
;   CRSR down        soft drop  SPACE     hard drop
;   P  pause/resume   N  new game   RUN/STOP  back to desktop
;
; Gravity: game_tick calls dostris_tick each mainloop pass; dos_dctr counts to
; DOS_RATE-min(level,2)*DOS_STEP passes, then one soft-drop (so it speeds up).
; ============================================================================

DOSBOARD equ $4C00
DOS_RATE equ 90         ; mainloop passes per drop at level 0 (harness-tuned)
DOS_STEP equ 25

; ---- zero page ($67-$75) ----
zpDosMask  equ $67   ; (2)
zpDosBit   equ $69
zpDosBX    equ $6A
zpDosBY    equ $6B
zpDosTX    equ $6C
zpDosTY    equ $6D
zpDosT1    equ $6E
zpDosT2    equ $6F
zpDosFull  equ $70
zpDosRow   equ $71
zpDosSrc   equ $72   ; (2)
zpDosDst   equ $74   ; (2)

piece_tab:
        dc.b $F0,$00,$44,$44,$00,$0F,$22,$22  ; I
        dc.b $60,$06,$60,$06,$60,$06,$60,$06  ; O
        dc.b $70,$02,$64,$04,$40,$0E,$20,$26  ; T
        dc.b $60,$03,$62,$04,$C0,$06,$20,$46  ; S
        dc.b $30,$06,$64,$02,$60,$0C,$40,$26  ; Z
        dc.b $10,$07,$26,$02,$E0,$08,$40,$64  ; J
        dc.b $40,$07,$22,$06,$E0,$02,$60,$44  ; L
dos_bit_tab:  dc.b 1,2,4,8,16,32,64,128
piece_color:  dc.b 3,7,4,5,2,6,8         ; I cyan O yellow T purple S green Z red J blue L orange
das_pts_lo:   dc.b <40,<100,<300,<1200
das_pts_hi:   dc.b >40,>100,>300,>1200
dos_name0: dc.b "I",0
dos_name1: dc.b "O",0
dos_name2: dc.b "T",0
dos_name3: dc.b "S",0
dos_name4: dc.b "Z",0
dos_name5: dc.b "J",0
dos_name6: dc.b "L",0
dos_name_lo: dc.b <dos_name0,<dos_name1,<dos_name2,<dos_name3,<dos_name4,<dos_name5,<dos_name6
dos_name_hi: dc.b >dos_name0,>dos_name1,>dos_name2,>dos_name3,>dos_name4,>dos_name5,>dos_name6

dos_load_mask:
        pha
        txa
        asl
        sta zpDosT1
        pla
        asl
        asl
        asl
        clc
        adc zpDosT1
        tax
        lda piece_tab,x
        sta zpDosMask
        lda piece_tab+1,x
        sta zpDosMask+1
        rts

dos_cell_at_bit:
        lda zpDosBit
        and #3
        clc
        adc zpDosTX
        sta zpDosBX
        lda zpDosBit
        lsr
        lsr
        clc
        adc zpDosTY
        sta zpDosBY
        lda zpDosBit
        cmp #8
        bcc dcb_lo
        sec
        sbc #8
        tax
        lda dos_bit_tab,x
        and zpDosMask+1
        rts
dcb_lo:
        tax
        lda dos_bit_tab,x
        and zpDosMask
        rts

dos_board_idx:
        lda zpDosBY
        asl
        sta zpDosT2
        lda zpDosBY
        asl
        asl
        asl
        clc
        adc zpDosT2
        clc
        adc zpDosBX
        rts

; dos_draw_cell - zpDosBX/zpDosBY, A = cell value (0 empty, else piece+1). One
; 8x8 char-cell at (2+bx, 1+by): filled = piece colour block, empty = white.
dos_draw_cell:
        cmp #0
        beq ddc_empty
        sec
        sbc #1
        tax
        lda piece_color,x
        asl
        asl
        asl
        asl
        sta zpFCol              ; fg = piece colour
        lda #$FF
        jmp ddc_go
ddc_empty:
        lda #COL_WIN
        sta zpFCol
        lda #$00
ddc_go:
        sta zpFPat
        lda zpDosBX
        clc
        adc #2
        sta zpCX
        sta zpFX
        lda zpDosBY
        clc
        adc #1
        sta zpCY
        lda #1
        sta zpCW
        sta zpCH
        jsr color_fill
        lda zpDosBY
        clc
        adc #1
        asl
        asl
        asl
        sta zpFY
        lda #1
        sta zpFW
        lda #8
        sta zpFH
        jmp fill_rows

dostris_fits:
        jsr dos_load_mask
        lda #0
        sta zpDosBit
df_loop:
        jsr dos_cell_at_bit
        beq df_next
        lda zpDosBX
        cmp #10
        bcs df_collide
        lda zpDosBY
        cmp #20
        bcs df_collide
        jsr dos_board_idx
        tax
        lda DOSBOARD,x
        bne df_collide
df_next:
        inc zpDosBit
        lda zpDosBit
        cmp #16
        bne df_loop
        lda #0
        rts
df_collide:
        lda #1
        rts

dostris_draw_piece:
        lda dos_piece
        ldx dos_rot
        jsr dos_load_mask
        lda dos_px
        sta zpDosTX
        lda dos_py
        sta zpDosTY
        lda #0
        sta zpDosBit
dp_loop:
        jsr dos_cell_at_bit
        beq dp_next
        lda dos_piece
        clc
        adc #1
        jsr dos_draw_cell
dp_next:
        inc zpDosBit
        lda zpDosBit
        cmp #16
        bne dp_loop
        rts

dostris_erase_piece:
        lda dos_piece
        ldx dos_rot
        jsr dos_load_mask
        lda dos_px
        sta zpDosTX
        lda dos_py
        sta zpDosTY
        lda #0
        sta zpDosBit
ep_loop:
        jsr dos_cell_at_bit
        beq ep_next
        jsr dos_board_idx
        tax
        lda DOSBOARD,x
        jsr dos_draw_cell       ; board value (0 reveals empty, else locked colour)
ep_next:
        inc zpDosBit
        lda zpDosBit
        cmp #16
        bne ep_loop
        rts

dostris_lock:
        lda dos_piece
        ldx dos_rot
        jsr dos_load_mask
        lda dos_px
        sta zpDosTX
        lda dos_py
        sta zpDosTY
        lda #0
        sta zpDosBit
lk_loop:
        jsr dos_cell_at_bit
        beq lk_next
        jsr dos_board_idx
        tax
        lda dos_piece
        clc
        adc #1
        sta DOSBOARD,x
lk_next:
        inc zpDosBit
        lda zpDosBit
        cmp #16
        bne lk_loop
        rts

dos_row_full:
        lda zpDosRow
        sta zpDosBY
        lda #0
        sta zpDosBX
rf_loop:
        jsr dos_board_idx
        tax
        lda DOSBOARD,x
        beq rf_notfull
        inc zpDosBX
        lda zpDosBX
        cmp #10
        bne rf_loop
        lda #1
        rts
rf_notfull:
        lda #0
        rts

dos_shift_down:
        lda zpDosRow
        sta zpDosT1
sd_loop:
        lda zpDosT1
        beq sd_clear0
        sec
        sbc #1
        sta zpDosBY
        lda #0
        sta zpDosBX
        jsr dos_board_idx
        clc
        adc #<DOSBOARD
        sta zpDosSrc
        lda #>DOSBOARD
        adc #0
        sta zpDosSrc+1
        lda zpDosT1
        sta zpDosBY
        lda #0
        sta zpDosBX
        jsr dos_board_idx
        clc
        adc #<DOSBOARD
        sta zpDosDst
        lda #>DOSBOARD
        adc #0
        sta zpDosDst+1
        ldy #0
sd_cp:
        lda (zpDosSrc),y
        sta (zpDosDst),y
        iny
        cpy #10
        bne sd_cp
        dec zpDosT1
        jmp sd_loop
sd_clear0:
        lda #<DOSBOARD
        sta zpDosDst
        lda #>DOSBOARD
        sta zpDosDst+1
        ldy #0
sd_clr:
        lda #0
        sta (zpDosDst),y
        iny
        cpy #10
        bne sd_clr
        rts

dostris_clear_lines:
        lda #0
        sta zpDosFull
cl_scan:
        lda #19
        sta zpDosRow
cl_rowloop:
        jsr dos_row_full
        beq cl_rownext
        jsr dos_shift_down
        inc zpDosFull
        jmp cl_scan
cl_rownext:
        lda zpDosRow
        beq cl_done
        dec zpDosRow
        jmp cl_rowloop
cl_done:
        rts

dostris_apply_score:
        lda zpDosFull
        beq das_pts0
        clc
        adc dos_lines
        sta dos_lines
        lsr
        lsr
        sta dos_level
        ldx zpDosFull
        lda das_pts_lo-1,x
        clc
        adc dos_score
        sta dos_score
        lda das_pts_hi-1,x
        adc dos_score+1
        sta dos_score+1
        rts
das_pts0:
        lda dos_score
        clc
        adc #1
        sta dos_score
        bcc das_done
        inc dos_score+1
das_done:
        rts

dos_rand7:
        lda dos_rng
        asl
        asl
        clc
        adc dos_rng
        clc
        adc #1
        sta dos_rng
dr7_mod:
        cmp #7
        bcc dr7_done
        sbc #7
        jmp dr7_mod
dr7_done:
        rts

dostris_spawn:
        lda dos_next
        sta dos_piece
        jsr dos_rand7
        sta dos_next
        lda #0
        sta dos_rot
        sta dos_py
        sta zpDosTY
        lda #3
        sta dos_px
        sta zpDosTX
        lda dos_piece
        ldx dos_rot
        jsr dostris_fits
        beq dsp_ok
        lda #1
        sta dos_over
dsp_ok:
        rts

dostris_newgame:
        lda #<DOSBOARD
        sta zpDosDst
        lda #>DOSBOARD
        sta zpDosDst+1
        ldy #0
dng_clr:
        lda #0
        sta (zpDosDst),y
        iny
        cpy #200
        bne dng_clr
        lda #0
        sta dos_score
        sta dos_score+1
        sta dos_lines
        sta dos_level
        sta dos_dctr
        sta dos_paused
        sta dos_over
        jsr dos_rand7
        sta dos_next
        jsr dostris_spawn
        rts

dostris_softdrop:
        lda #0
        sta dos_justlock
        lda dos_px
        sta zpDosTX
        lda dos_py
        clc
        adc #1
        sta zpDosTY
        lda dos_piece
        ldx dos_rot
        jsr dostris_fits
        bne dsd_lock
        jsr dostris_erase_piece
        inc dos_py
        jsr dostris_draw_piece
        rts
dsd_lock:
        lda #1
        sta dos_justlock
        jsr dostris_lock
        jsr dostris_clear_lines
        jsr dostris_apply_score
        jsr dostris_spawn
        jmp dostris_draw_all

; dostris_tick - called every mainloop pass; soft-drops once per
; (DOS_RATE - min(level,2)*DOS_STEP) passes.
dostris_tick:
        lda dos_over
        bne dt_done
        lda dos_paused
        bne dt_done
        inc dos_dctr
        lda dos_level
        cmp #2
        bcc dtr_use
        lda #2
dtr_use:
        sta zpDosT1             ; min(level,2)
        lda zpDosT1
        asl                     ; *2 ...
        clc
        adc zpDosT1             ; *... no, want *DOS_STEP; do it simply:
        ; rate = DOS_RATE - min*DOS_STEP
        lda #0
        sta zpDosT2
        ldx zpDosT1
dtr_mul:
        beq dtr_have
        clc
        lda zpDosT2
        adc #DOS_STEP
        sta zpDosT2
        dex
        jmp dtr_mul
dtr_have:
        lda #DOS_RATE
        sec
        sbc zpDosT2
        cmp dos_dctr
        bcs dt_done             ; dctr < rate -> wait
        lda #0
        sta dos_dctr
        jsr dostris_softdrop
dt_done:
        rts

dostris_open:
        lda #4
        sta app_mode
        jsr sid_click
        jsr dostris_newgame
        jmp dostris_draw_all

dostris_close:
        lda #0
        sta app_mode
        jsr draw_desktop
        jsr draw_sysinfo_win
        jsr draw_clock_win
        jmp draw_icons

dostris_key:
        lda zpTmp
        cmp #K_ESC
        bne dk_n0
        jmp dostris_close
dk_n0:
        cmp #$4E                ; 'N' new game
        bne dk_n1
        jmp dk_new
dk_n1:
        lda dos_over
        bne dk_done
        lda zpTmp
        cmp #$50                ; 'P' pause
        bne dk_n2
        jmp dk_pause
dk_n2:
        lda dos_paused
        bne dk_done
        lda zpTmp
        cmp #K_LEFT
        bne dk_n3
        jmp dk_left
dk_n3:
        cmp #K_RIGHT
        bne dk_n4
        jmp dk_right
dk_n4:
        cmp #K_DOWN
        bne dk_n5
        jmp dostris_softdrop
dk_n5:
        cmp #K_UP
        bne dk_n6
        jmp dk_rotate
dk_n6:
        cmp #K_SPACE
        bne dk_done
        jmp dk_harddrop
dk_done:
        rts
dk_new:
        jsr dostris_newgame
        jmp dostris_draw_all
dk_pause:
        lda dos_paused
        eor #1
        sta dos_paused
        jmp dostris_draw_hud
dk_left:
        jsr dostris_erase_piece
        lda dos_px
        sec
        sbc #1
        sta zpDosTX
        lda dos_py
        sta zpDosTY
        lda dos_piece
        ldx dos_rot
        jsr dostris_fits
        bne dkl_rd
        dec dos_px
dkl_rd:
        jmp dostris_draw_piece
dk_right:
        jsr dostris_erase_piece
        lda dos_px
        clc
        adc #1
        sta zpDosTX
        lda dos_py
        sta zpDosTY
        lda dos_piece
        ldx dos_rot
        jsr dostris_fits
        bne dkr_rd
        inc dos_px
dkr_rd:
        jmp dostris_draw_piece
dk_rotate:
        jsr dostris_erase_piece
        lda dos_px
        sta zpDosTX
        lda dos_py
        sta zpDosTY
        lda dos_rot
        clc
        adc #1
        and #3
        sta zpDosT1
        tax
        lda dos_piece
        jsr dostris_fits
        bne dkrot_rd
        lda zpDosT1
        sta dos_rot
dkrot_rd:
        jmp dostris_draw_piece
dk_harddrop:
dhd_loop:
        jsr dostris_softdrop
        lda dos_justlock
        beq dhd_loop
        rts

; dostris_draw_all - full redraw: app_clear, board (filled cells only), active
; piece, HUD, help.
dostris_draw_all:
        jsr app_clear
        lda #<msg_dos_title
        sta zpPtr
        lda #>msg_dos_title
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
        ; board well outline (cols 1..12, rows 1..21 -> board cells are 2..11)
        lda #1
        sta zpWX
        lda #1
        sta zpWY
        lda #12
        sta zpWW
        lda #21
        sta zpWH
        jsr win_outline
        ; board: draw only filled cells (empties are already white)
        lda #0
        sta zpDosBY
da_rl:
        lda #0
        sta zpDosBX
da_cl:
        jsr dos_board_idx
        tax
        lda DOSBOARD,x
        beq da_skip
        jsr dos_draw_cell
da_skip:
        inc zpDosBX
        lda zpDosBX
        cmp #10
        bne da_cl
        inc zpDosBY
        lda zpDosBY
        cmp #20
        bne da_rl
        jsr dostris_draw_piece
        jsr dostris_draw_hud
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda #<msg_dos_help
        sta zpPtr
        lda #>msg_dos_help
        sta zpPtr+1
        jmp draw_string

dostris_draw_hud:
        lda #COL_WIN
        sta zpFCol
        lda #14
        sta zpCol
        lda #2
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_dos_score
        sta zpPtr
        lda #>msg_dos_score
        sta zpPtr+1
        jsr draw_string
        lda dos_score
        sta zpFSSize
        lda dos_score+1
        sta zpFSSize+1
        lda #21
        sta zpCol
        jsr draw_dec16
        lda #14
        sta zpCol
        lda #3
        sta zpRow
        lda #<msg_dos_lines
        sta zpPtr
        lda #>msg_dos_lines
        sta zpPtr+1
        jsr draw_string
        lda dos_lines
        sta zpFSSize
        lda #0
        sta zpFSSize+1
        lda #21
        sta zpCol
        jsr draw_dec16
        lda #14
        sta zpCol
        lda #4
        sta zpRow
        lda #<msg_dos_level
        sta zpPtr
        lda #>msg_dos_level
        sta zpPtr+1
        jsr draw_string
        lda dos_level
        sta zpFSSize
        lda #0
        sta zpFSSize+1
        lda #21
        sta zpCol
        jsr draw_dec16
        lda #14
        sta zpCol
        lda #6
        sta zpRow
        lda #<msg_dos_next
        sta zpPtr
        lda #>msg_dos_next
        sta zpPtr+1
        jsr draw_string
        ldx dos_next
        lda dos_name_lo,x
        sta zpPtr
        lda dos_name_hi,x
        sta zpPtr+1
        lda #21
        sta zpCol
        jsr draw_string
        ; status row 8
        lda #14
        sta zpCol
        lda #8
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda dos_over
        bne dh_over
        lda dos_paused
        bne dh_paused
        lda #<msg_dos_blank
        sta zpPtr
        lda #>msg_dos_blank
        sta zpPtr+1
        jmp draw_string
dh_over:
        lda #<msg_dos_over
        sta zpPtr
        lda #>msg_dos_over
        sta zpPtr+1
        jmp draw_string
dh_paused:
        lda #<msg_dos_paused
        sta zpPtr
        lda #>msg_dos_paused
        sta zpPtr+1
        jmp draw_string

msg_dos_title:  dc.b "Dostris",0
msg_dos_score:  dc.b "Score:",0
msg_dos_lines:  dc.b "Lines:",0
msg_dos_level:  dc.b "Level:",0
msg_dos_next:   dc.b "Next:",0
msg_dos_over:   dc.b "GAME OVER",0
msg_dos_paused: dc.b "PAUSED",0
msg_dos_blank:  dc.b "         ",0
msg_dos_help:   dc.b "CRSR move/drop  Spc=drop P=pause N=new",0
