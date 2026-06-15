; ============================================================================
; UnoDOS/C64 Notepad app (milestone 2) - full-screen text editor over NOTEBUF
; (2048 bytes). Ported from apple2/notepad.i: append-only/cursor-at-end editing
; (printable keys insert, RETURN inserts CR $0D, DEL backspaces, F1 saves via
; fs_save, RUN/STOP returns to Files). C64 key codes; the kernel's keymap hands
; us bare ASCII (no bit-7), so insert takes printable $20..$7E directly.
;
; Screen: row0 = "Notepad: NAME" + separator; rows1-22 (APP_VIEW_ROWS) show the
; tail of the buffer scrolled so the cursor line is visible; row23 = status
; "Ln:n  Col:n  Bytes:n" + help/SAVED/FULL.
; ============================================================================

; ---- zero page ($58-$66) ----
zpNPtr   equ $58   ; (2) pointer into NOTEBUF
zpNRem   equ $5A   ; (2) bytes remaining in scan
zpNTot   equ $5C   ; (2) pass1: total line count (1-based)
zpNFCol  equ $5E   ; (2) pass1: final (cursor) column, 0-based
zpNSkip  equ $60   ; (2) lines to skip (tail scroll)
zpNVis   equ $62   ; pass2: current line visible flag
zpNLine  equ $63   ; (2) pass2: current line number (0-based)
zpNCol   equ $65   ; pass2: current screen column (capped 40)
zpNRow   equ $66   ; pass2: current screen row

; notepad_load - A = directory index: copy name, read file into NOTEBUF, enter
; Notepad. Caller draws (jmp notepad_draw).
notepad_load:
        sta note_idx
        jsr fs_entry_ptr
        ldy #0
nl_name:
        lda (zpFSPtr),y
        sta note_name,y
        iny
        cpy #12
        bne nl_name
        lda #<NOTEBUF
        sta zpFSDat
        lda #>NOTEBUF
        sta zpFSDat+1
        lda note_idx
        jsr fs_read             ; zpFSSize = byte size
        lda zpFSSize
        sta note_len
        lda zpFSSize+1
        sta note_len+1
        lda #0
        sta note_dirty
        sta note_flash
        lda #2
        sta app_mode
        jsr sid_click
        rts

notepad_close:
        lda #1
        sta app_mode
        jmp files_draw

; notepad_key - route a key while Notepad is active (zpTmp = key code).
notepad_key:
        lda zpTmp
        cmp #K_ESC
        bne nk_n1
        jmp notepad_close
nk_n1:
        cmp #K_SAVE
        bne nk_n2
        jmp notepad_save
nk_n2:
        cmp #K_BS
        bne nk_n3
        jmp notepad_backspace
nk_n3:
        cmp #K_RET
        bne nk_n4
        lda #$0D
        jmp notepad_insert
nk_n4:
        lda zpTmp
        cmp #$20                ; below space -> control/nav, ignore
        bcc nk_ignore
        cmp #$7F
        bcs nk_ignore           ; DEL and above -> ignore
        jmp notepad_insert
nk_ignore:
        rts

; notepad_insert - A = byte to append (or redraw + FULL flash if at the cap).
notepad_insert:
        sta zpTmp
        lda note_len+1
        cmp #>NOTE_MAXLEN
        bcc ni_ok
        lda note_len
        cmp #<NOTE_MAXLEN
        bcc ni_ok
        lda #2
        sta note_flash
        jsr sid_click
        jmp notepad_draw
ni_ok:
        lda #<NOTEBUF
        clc
        adc note_len
        sta zpNPtr
        lda #>NOTEBUF
        adc note_len+1
        sta zpNPtr+1
        lda zpTmp
        ldy #0
        sta (zpNPtr),y
        inc note_len
        bne ni_noc
        inc note_len+1
ni_noc:
        lda #1
        sta note_dirty
        lda #0
        sta note_flash
        jmp notepad_draw

notepad_backspace:
        lda note_len
        ora note_len+1
        beq nb_done
        lda note_len
        bne nb_lo
        dec note_len+1
nb_lo:
        dec note_len
        lda #1
        sta note_dirty
        lda #0
        sta note_flash
nb_done:
        jmp notepad_draw

; notepad_save - F1: fs_save(note_name, NOTEBUF, note_len). The saved entry is
; always last (delete-then-append), so note_idx/files_sel = count-1.
notepad_save:
        lda #<note_name
        sta zpFSName
        lda #>note_name
        sta zpFSName+1
        lda #<NOTEBUF
        sta zpFSDat
        lda #>NOTEBUF
        sta zpFSDat+1
        lda note_len
        sta zpFSSize
        lda note_len+1
        sta zpFSSize+1
        jsr fs_save
        cmp #$FF
        beq ns_full
        lda FSBASE+FSC_COUNT
        sec
        sbc #1
        sta note_idx
        sta files_sel
        lda #0
        sta note_dirty
        lda #1
        sta note_flash
        jsr sid_click
        jmp notepad_draw
ns_full:
        lda #2
        sta note_flash
        jsr sid_click
        jmp notepad_draw

; notepad_draw - full redraw: title, separator, tail-scrolled text, status.
notepad_draw:
        jsr app_clear
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
        lda #<msg_notepad_title
        sta zpPtr
        lda #>msg_notepad_title
        sta zpPtr+1
        lda #1
        sta zpCol
        lda #0
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string
        lda #<note_name
        sta zpFSPtr
        lda #>note_name
        sta zpFSPtr+1
        jsr draw_name12_title
; ---- separator ----
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
; ---- pass 1: total line count (zpNTot) + cursor column (zpNFCol) ----
        lda #1
        sta zpNTot
        lda #0
        sta zpNTot+1
        sta zpNFCol
        sta zpNFCol+1
        lda #<NOTEBUF
        sta zpNPtr
        lda #>NOTEBUF
        sta zpNPtr+1
        lda note_len
        sta zpNRem
        lda note_len+1
        sta zpNRem+1
np1_loop:
        lda zpNRem
        ora zpNRem+1
        beq np1_done
        ldy #0
        lda (zpNPtr),y
        cmp #$0D
        bne np1_notcr
        inc zpNTot
        bne np1_notot
        inc zpNTot+1
np1_notot:
        lda #0
        sta zpNFCol
        sta zpNFCol+1
        jmp np1_adv
np1_notcr:
        inc zpNFCol
        bne np1_adv
        inc zpNFCol+1
np1_adv:
        inc zpNPtr
        bne np1_noc
        inc zpNPtr+1
np1_noc:
        lda zpNRem
        bne np1_remlo
        dec zpNRem+1
np1_remlo:
        dec zpNRem
        jmp np1_loop
np1_done:
; ---- zpNSkip = max(0, zpNTot - APP_VIEW_ROWS) ----
        lda zpNTot
        sec
        sbc #APP_VIEW_ROWS
        sta zpNSkip
        lda zpNTot+1
        sbc #0
        sta zpNSkip+1
        bpl np_skok
        lda #0
        sta zpNSkip
        sta zpNSkip+1
np_skok:
; ---- pass 2: render visible lines, cols 0-39, rows 1-22 ----
        lda #0
        sta zpNLine
        sta zpNLine+1
        jsr np_calc_vis
        lda #0
        sta zpNCol
        lda #1
        sta zpNRow
        lda #<NOTEBUF
        sta zpNPtr
        lda #>NOTEBUF
        sta zpNPtr+1
        lda note_len
        sta zpNRem
        lda note_len+1
        sta zpNRem+1
np2_loop:
        lda zpNRem
        ora zpNRem+1
        beq np2_done
        ldy #0
        lda (zpNPtr),y
        cmp #$0D
        bne np2_char
        lda zpNVis
        beq np2_nodraw
        inc zpNRow
np2_nodraw:
        lda #0
        sta zpNCol
        inc zpNLine
        bne np2_noline
        inc zpNLine+1
np2_noline:
        jsr np_calc_vis
        jmp np2_adv
np2_char:
        lda zpNVis
        beq np2_adv
        lda zpNCol
        cmp #40
        bcs np2_adv
        sta zpCol
        lda zpNRow
        sta zpRow
        lda #0
        sta zpInv
        ldy #0
        lda (zpNPtr),y
        jsr draw_char
        inc zpNCol
np2_adv:
        inc zpNPtr
        bne np2_noc
        inc zpNPtr+1
np2_noc:
        lda zpNRem
        bne np2_remlo
        dec zpNRem+1
np2_remlo:
        dec zpNRem
        jmp np2_loop
np2_done:
; ---- status line (row 23): "Ln:n  Col:n  Bytes:n" + help/SAVED/FULL ----
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_ln
        sta zpPtr
        lda #>msg_ln
        sta zpPtr+1
        jsr draw_string
        lda zpNTot
        sta zpFSSize
        lda zpNTot+1
        sta zpFSSize+1
        jsr draw_dec16
        lda #<msg_col
        sta zpPtr
        lda #>msg_col
        sta zpPtr+1
        jsr draw_string
        lda zpNFCol
        clc
        adc #1
        sta zpFSSize
        lda zpNFCol+1
        adc #0
        sta zpFSSize+1
        jsr draw_dec16
        lda #<msg_bytes
        sta zpPtr
        lda #>msg_bytes
        sta zpPtr+1
        jsr draw_string
        lda note_len
        sta zpFSSize
        lda note_len+1
        sta zpFSSize+1
        jsr draw_dec16
        lda note_flash
        beq nd_help
        cmp #1
        beq nd_saved
        lda #<msg_full
        sta zpPtr
        lda #>msg_full
        sta zpPtr+1
        jmp draw_string
nd_saved:
        lda #<msg_saved
        sta zpPtr
        lda #>msg_saved
        sta zpPtr+1
        jmp draw_string
nd_help:
        lda zpCol
        clc
        adc #19
        cmp #41
        bcs nd_done
        lda #<msg_note_help
        sta zpPtr
        lda #>msg_note_help
        sta zpPtr+1
        jmp draw_string
nd_done:
        rts

; np_calc_vis - zpNVis = (zpNLine >= zpNSkip) ? 1 : 0 (16-bit unsigned).
np_calc_vis:
        sec
        lda zpNLine
        sbc zpNSkip
        lda zpNLine+1
        sbc zpNSkip+1
        lda #0
        sta zpNVis
        bcc ncv_done
        lda #1
        sta zpNVis
ncv_done:
        rts
