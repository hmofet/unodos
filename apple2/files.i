; ============================================================================
; UnoDOS/AppleII Files app (milestone 2) - full-screen catalog browser on
; top of the USV1 mini-FS (fs.i). Lists name+size for each directory entry;
; left/right move the selection (wrapping), Return opens the file in
; Notepad, d/D deletes (with a y/n confirm line), r/R re-reads the catalog
; from disk. ESC returns to the desktop.
;
; Screen: row0 = "Files" title + separator (as draw_desktop's menu bar);
; rows1-15 = up to FS_MAXFILES=15 entries, one per row (name in cols1-12,
; size right of col20); row23 = help line, or while files_confirm=1 a
; "Delete NAME? (Y/N)" prompt.
; ============================================================================

; ---------------------------------------------------------------- files_open
; files_open - Return pressed on the Files icon: enter Files app mode.
files_open:
        lda #1
        sta app_mode
        lda #0
        sta files_sel
        sta files_confirm
        jsr beep_click          ; app launch
        jmp files_draw

; --------------------------------------------------------------- files_close
; files_close - ESC in Files: back to the desktop.
files_close:
        lda #0
        sta app_mode
        jsr draw_desktop
        jsr draw_sysinfo_win
        jsr draw_clock_win
        jmp draw_icons

; ----------------------------------------------------------------- files_key
; files_key - route a key while Files is active (zpTmp = key code).
files_key:
        lda files_confirm
        beq fk_normal

; ---- awaiting delete y/n ----
        lda zpTmp
        cmp #$D9                ; 'Y'
        beq fk_yes
        cmp #$F9                ; 'y'
        beq fk_yes
        lda #0
        sta files_confirm
        jmp files_draw
fk_yes:
        jsr files_delete_at_sel
        lda #0
        sta files_confirm
        jmp files_draw

fk_normal:
        lda zpTmp
        cmp #$9B                ; ESC
        bne fk_n1
        jmp files_close
fk_n1:
        cmp #$95                ; right
        bne fk_n2
        jmp files_next
fk_n2:
        cmp #$88                ; left
        bne fk_n3
        jmp files_prev
fk_n3:
        cmp #$8D                ; Return
        bne fk_n4
        jmp files_open_selected
fk_n4:
        cmp #$C4                ; 'D'
        beq fk_del
        cmp #$E4                ; 'd'
        beq fk_del
        cmp #$D2                ; 'R'
        beq fk_rescan
        cmp #$F2                ; 'r'
        beq fk_rescan
        rts
fk_del:
        jmp files_delete_prompt
fk_rescan:
        jsr fs_init
        jmp files_draw

; ---------------------------------------------------------------- files_next
; files_next - right: move selection forward, wrapping at FSC_COUNT.
files_next:
        lda CATBUF+FSC_COUNT
        beq files_draw
        lda files_sel
        clc
        adc #1
        cmp CATBUF+FSC_COUNT
        bcc fn_ok
        lda #0
fn_ok:
        sta files_sel
        jmp files_draw

; ---------------------------------------------------------------- files_prev
; files_prev - left: move selection back, wrapping at FSC_COUNT.
files_prev:
        lda CATBUF+FSC_COUNT
        beq files_draw
        lda files_sel
        bne fp_dec
        lda CATBUF+FSC_COUNT
        sec
        sbc #1
        sta files_sel
        jmp files_draw
fp_dec:
        dec files_sel
        jmp files_draw

; -------------------------------------------------------- files_open_selected
; files_open_selected - Return: load the selected file into Notepad.
files_open_selected:
        lda CATBUF+FSC_COUNT
        bne fos_go
        rts
fos_go:
        lda files_sel
        jsr notepad_load
        jmp notepad_draw

; -------------------------------------------------------- files_delete_prompt
; files_delete_prompt - d/D: arm the y/n confirm (drawn by files_draw).
files_delete_prompt:
        lda CATBUF+FSC_COUNT
        beq files_draw
        lda #1
        sta files_confirm
        jmp files_draw

; ---------------------------------------------------------- files_delete_at_sel
; files_delete_at_sel - delete the selected entry, then clamp files_sel to
; the new (possibly empty) directory.
files_delete_at_sel:
        lda files_sel
        jsr fs_delete
        lda files_sel
        cmp CATBUF+FSC_COUNT
        bcc fdas_done           ; still in range
        lda CATBUF+FSC_COUNT
        beq fdas_zero
        sec
        sbc #1
        sta files_sel
        rts
fdas_zero:
        lda #0
        sta files_sel
fdas_done:
        rts

; ---------------------------------------------------------------- files_draw
; files_draw - full redraw: title, separator, directory listing (or
; "(no files)"), and the help/confirm line.
files_draw:
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

        lda #<msg_files_title
        sta zpPtr
        lda #>msg_files_title
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

; ---- directory listing ----
        lda CATBUF+FSC_COUNT
        bne fd_has_files
        lda #<msg_files_empty
        sta zpPtr
        lda #>msg_files_empty
        sta zpPtr+1
        lda #1
        sta zpCol
        lda #1
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string
        jmp fd_status

fd_has_files:
        ldx #0
fd_row:
        cpx CATBUF+FSC_COUNT
        bne fd_go
        jmp fd_status
fd_go:
        lda #1
        sta zpCol
        txa
        clc
        adc #1
        sta zpRow

        lda #0
        sta zpInv
        cpx files_sel
        bne fd_pat
        lda #$7F
        sta zpInv
fd_pat:
        lda zpInv
        beq fd_nohi
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
fd_nohi:
        txa
        jsr fs_entry_ptr        ; zpFSPtr = &CATBUF entry (clobbers A)
        jsr draw_name12

        lda #20
        sta zpCol
        ldy #FSE_SIZE
        lda (zpFSPtr),y
        sta zpFSSize
        iny
        lda (zpFSPtr),y
        sta zpFSSize+1
        jsr draw_dec16

        inx
        jmp fd_row

; ---- status / confirm line (row23) ----
fd_status:
        lda files_confirm
        beq fd_help
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_confirm1
        sta zpPtr
        lda #>msg_confirm1
        sta zpPtr+1
        jsr draw_string
        lda files_sel
        jsr fs_entry_ptr        ; clobbers A
        jsr draw_name12_title
        lda #<msg_confirm2
        sta zpPtr
        lda #>msg_confirm2
        sta zpPtr+1
        jmp draw_string
fd_help:
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_files_help
        sta zpPtr
        lda #>msg_files_help
        sta zpPtr+1
        jmp draw_string
