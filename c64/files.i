; ============================================================================
; UnoDOS/C64 Files app (milestone 2) - full-screen catalog browser over the
; USV1 mini-FS (fs.i). Ported from apple2/files.i: same UI, C64 key codes,
; $FF (8-bit) inversion for the selection highlight, SID blip on launch.
;
; Screen: row0 = "Files" title + separator; rows1-15 = up to FS_MAXFILES
; entries (name cols1-12, size right of col20); row23 = help or, while
; files_confirm=1, "Delete NAME? (Y/N)". CRSR<>/^v move, RETURN opens in
; Notepad, D deletes (y/n), R rescans, RUN/STOP returns to the desktop.
; ============================================================================

files_open:
        lda #1
        sta app_mode
        lda #0
        sta files_sel
        sta files_confirm
        jsr sid_click
        jmp files_draw

files_close:
        lda #0
        sta app_mode
        jsr draw_desktop
        jsr draw_sysinfo_win
        jsr draw_clock_win
        jmp draw_icons

; files_key - route a key while Files is active (zpTmp = key code).
files_key:
        lda files_confirm
        beq fk_normal
        lda zpTmp
        cmp #$59                ; 'Y'
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
        cmp #K_ESC
        bne fk_n1
        jmp files_close
fk_n1:
        cmp #K_RIGHT
        bne fk_n2
        jmp files_next
fk_n2:
        cmp #K_LEFT
        bne fk_n3
        jmp files_prev
fk_n3:
        cmp #K_RET
        bne fk_n4
        jmp files_open_selected
fk_n4:
        cmp #$44                ; 'D'
        beq fk_del
        cmp #$52                ; 'R'
        beq fk_rescan
        rts
fk_del:
        jmp files_delete_prompt
fk_rescan:
        jsr fs_init
        jmp files_draw

files_next:
        lda FSBASE+FSC_COUNT
        bne fn_go
        jmp files_draw
fn_go:
        lda files_sel
        clc
        adc #1
        cmp FSBASE+FSC_COUNT
        bcc fn_ok
        lda #0
fn_ok:
        sta files_sel
        jmp files_draw

files_prev:
        lda FSBASE+FSC_COUNT
        bne fp_go
        jmp files_draw
fp_go:
        lda files_sel
        bne fp_dec
        lda FSBASE+FSC_COUNT
        sec
        sbc #1
        sta files_sel
        jmp files_draw
fp_dec:
        dec files_sel
        jmp files_draw

files_open_selected:
        lda FSBASE+FSC_COUNT
        bne fos_go
        rts
fos_go:
        lda files_sel
        jsr notepad_load
        jmp notepad_draw

files_delete_prompt:
        lda FSBASE+FSC_COUNT
        bne fdp_go
        jmp files_draw
fdp_go:
        lda #1
        sta files_confirm
        jmp files_draw

files_delete_at_sel:
        lda files_sel
        jsr fs_delete
        lda files_sel
        cmp FSBASE+FSC_COUNT
        bcc fdas_done           ; still in range
        lda FSBASE+FSC_COUNT
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

; files_draw - full redraw.
files_draw:
        jsr app_clear           ; white screen, zpFCol = COL_WIN
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
        lda #$FF
        sta zpFPat
        jsr fill_rows
; ---- directory listing ----
        lda FSBASE+FSC_COUNT
        bne fd_has
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
fd_has:
        ldx #0
fd_row:
        cpx FSBASE+FSC_COUNT
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
        lda #$FF
        sta zpInv
fd_pat:
        lda zpInv
        beq fd_nohi
        ; highlight band: fill the row black, text drawn inverted -> white-on-black.
        ; fill_rows clobbers X (its column counter), but X is our entry-loop
        ; index - save/restore it across the call.
        stx zpSlot
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
        lda #$FF
        sta zpFPat
        jsr fill_rows
        ldx zpSlot
fd_nohi:
        txa
        jsr fs_entry_ptr        ; zpFSPtr = &entry
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
        jsr fs_entry_ptr
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
