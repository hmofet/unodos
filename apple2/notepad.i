; ============================================================================
; UnoDOS/AppleII Notepad app (milestone 2) - full-screen text editor on top
; of NOTEBUF (2048 bytes, fs.i). Append-only/cursor-always-at-end editing:
; printable keys insert, Return inserts CR ($0D) as the line separator,
; left/backspace ($88) deletes the last byte, Ctrl-S ($93) saves via
; fs_save, ESC returns to Files.
;
; Screen: row0 = "Notepad: NAME" title + separator (as files_draw); rows1-22
; (APP_VIEW_ROWS=22) show the tail of the buffer (scrolled so the cursor's
; line is always visible), full 40-column width (cols 0-39); row23 = status
; line "Ln:n  Col:n  Bytes:n" plus help/SAVED/FULL if it fits.
; ============================================================================

; -------------------------------------------------------------- notepad_load
; notepad_load - A = directory index: copy the name into note_name, read the
; file into NOTEBUF, set note_len/note_idx/note_dirty/note_flash, enter
; Notepad (app_mode=2). Caller draws (jmp notepad_draw).
notepad_load:
        sta note_idx
        jsr fs_entry_ptr        ; zpFSPtr = &CATBUF entry (clobbers A)
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
        lda #8                  ; NOTEBUF = 2048 B = 8 sectors (fs_read cap)
        sta zpFSTmp
        lda note_idx
        jsr fs_read             ; zpFSSize = byte size (LE) on exit
        lda zpFSSize
        sta note_len
        lda zpFSSize+1
        sta note_len+1

        lda #0
        sta note_dirty
        sta note_flash
        lda #2
        sta app_mode
        jsr beep_click          ; app launch
        rts

; -------------------------------------------------------------- notepad_close
; notepad_close - ESC: back to Files (app_mode=1).
notepad_close:
        lda #1
        sta app_mode
        jmp files_draw

; --------------------------------------------------------------- notepad_key
; notepad_key - route a key while Notepad is active (zpTmp = key code).
notepad_key:
        lda zpTmp
        cmp #$9B                ; ESC
        bne nk_n1
        jmp notepad_close
nk_n1:
        cmp #$93                ; Ctrl-S
        bne nk_n2
        jmp notepad_save
nk_n2:
        cmp #$88                ; left / backspace
        bne nk_n3
        jmp notepad_backspace
nk_n3:
        cmp #$8D                ; Return
        bne nk_n4
        lda #$0D
        jmp notepad_insert
nk_n4:
        cmp #$A0                ; space..'~' with bit7 set
        bcc nk_ignore
        cmp #$FF                ; DEL - ignore
        beq nk_ignore
        and #$7F
        jmp notepad_insert
nk_ignore:
        rts

; ------------------------------------------------------------ notepad_insert
; notepad_insert - A = byte to append. If note_len==NOTE_MAXLEN, flash FULL
; and redraw without inserting; else append, bump note_len, mark dirty.
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
        jsr beep_click          ; error: buffer full
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
        bne ni_nocarry
        inc note_len+1
ni_nocarry:
        lda #1
        sta note_dirty
        lda #0
        sta note_flash
        jmp notepad_draw

; --------------------------------------------------------- notepad_backspace
; notepad_backspace - left/backspace: drop the last byte (no-op if empty).
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

; -------------------------------------------------------------- notepad_save
; notepad_save - Ctrl-S: fs_save(note_name, NOTEBUF, note_len). On success,
; the saved entry is always last in the directory (fs_save appends after a
; delete-if-existing), so note_idx/files_sel = CATBUF+FSC_COUNT-1; flash
; SAVED. On failure (disk full), flash FULL. Either way, redraw.
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

        lda CATBUF+FSC_COUNT
        sec
        sbc #1
        sta note_idx
        sta files_sel
        lda #0
        sta note_dirty
        lda #1
        sta note_flash
        jsr beep_click          ; save complete
        jmp notepad_draw
ns_full:
        lda #2
        sta note_flash
        jsr beep_click          ; error: disk full
        jmp notepad_draw

; --------------------------------------------------------------- notepad_draw
; notepad_draw - full redraw: title, separator, tail-scrolled text buffer,
; status line.
notepad_draw:
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

; ---- pass 1: total line count (zpNTot, 1-based) and cursor column
;      (zpNFCol, 0-based) ----
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

np_p1_loop:
        lda zpNRem
        ora zpNRem+1
        beq np_p1_done
        ldy #0
        lda (zpNPtr),y
        cmp #$0D
        bne np_p1_notcr
        inc zpNTot
        bne np_p1_notot
        inc zpNTot+1
np_p1_notot:
        lda #0
        sta zpNFCol
        sta zpNFCol+1
        jmp np_p1_adv
np_p1_notcr:
        inc zpNFCol
        bne np_p1_adv
        inc zpNFCol+1
np_p1_adv:
        inc zpNPtr
        bne np_p1_noc
        inc zpNPtr+1
np_p1_noc:
        lda zpNRem
        bne np_p1_remlo
        dec zpNRem+1
np_p1_remlo:
        dec zpNRem
        jmp np_p1_loop
np_p1_done:

; ---- zpNSkip = max(0, zpNTot - APP_VIEW_ROWS) ----
        lda zpNTot
        sec
        sbc #APP_VIEW_ROWS
        sta zpNSkip
        lda zpNTot+1
        sbc #0
        sta zpNSkip+1
        bpl np_skip_ok
        lda #0
        sta zpNSkip
        sta zpNSkip+1
np_skip_ok:

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

np_p2_loop:
        lda zpNRem
        ora zpNRem+1
        beq np_p2_done
        ldy #0
        lda (zpNPtr),y
        cmp #$0D
        bne np_p2_char
        lda zpNVis
        beq np_p2_nodraw
        inc zpNRow
np_p2_nodraw:
        lda #0
        sta zpNCol
        inc zpNLine
        bne np_p2_noline
        inc zpNLine+1
np_p2_noline:
        jsr np_calc_vis
        jmp np_p2_adv
np_p2_char:
        lda zpNVis
        beq np_p2_adv
        lda zpNCol
        cmp #40
        bcs np_p2_adv
        sta zpCol
        lda zpNRow
        sta zpRow
        lda #0
        sta zpInv
        ldy #0
        lda (zpNPtr),y
        jsr draw_char
        inc zpNCol
np_p2_adv:
        inc zpNPtr
        bne np_p2_noc
        inc zpNPtr+1
np_p2_noc:
        lda zpNRem
        bne np_p2_remlo
        dec zpNRem+1
np_p2_remlo:
        dec zpNRem
        jmp np_p2_loop
np_p2_done:

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

; ------------------------------------------------------------- np_calc_vis
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
