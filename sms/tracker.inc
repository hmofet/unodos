; ============================================================================
; UnoDOS/SMS — Tracker (proc 6). A windowed pattern grid (NT_ROWS x NT_CH note
; cells) that auto-loops, playing the leftmost non-empty voice of the current
; row on the SN76489 PSG (channel 0). While topmost it owns the d-pad: the WM
; sprite hovers a cell; B1 cycles the note (-- C4 D4 E4 F4 G4 A4 B4 C5); B2 closes.
; Mirrors the Paint app's windowed-tile pattern; notes are PSG tone periods.
; ============================================================================

NT_ROWS EQU 14
NT_CH   EQU 4
TK_STEPF EQU 12

tk_crow  EQU $C145
tk_cch   EQU $C146
tk_prow  EQU $C147
tk_ptmr  EQU $C148
tk_r     EQU $C149       ; draw scratch
tk_c     EQU $C14A
tk_pat   EQU $C210       ; NT_ROWS*NT_CH note cells (clear of pcanvas $C150..$C204)

; period = 3579545 / (32 * freq); 0 = rest
tk_periods:
    dw 0, 427, 380, 339, 320, 285, 254, 226, 214
tk_names:
    db "--",0, "C4",0, "D4",0, "E4",0, "F4",0, "G4",0, "A4",0, "B4",0, "C5",0
s_tkmark: db ">",0

; tk_tone: A = note index (0 = rest) -> program PSG ch0.
tk_tone:
    or a
    jr z, tkt_rest
    ld l, a
    ld h, 0
    add hl, hl
    ld de, tk_periods
    add hl, de
    ld e, (hl)
    inc hl
    ld d, (hl)
    ex de, hl
    jp psg_play0
tkt_rest:
    ld a, $9F                        ; ch0 off
    out (PSG_PORT), a
    ret

; tk_play_row: A = row -> play leftmost non-empty note (or rest).
tk_play_row:
    add a, a
    add a, a                         ; row*NT_CH
    ld l, a
    ld h, 0
    ld de, tk_pat
    add hl, de
    ld b, NT_CH
tpr_l:
    ld a, (hl)
    or a
    jr nz, tk_tone
    inc hl
    djnz tpr_l
    xor a
    jr tk_tone

tracker_init:
    ld hl, tk_pat
    ld de, tk_pat+1
    ld bc, NT_ROWS*NT_CH-1
    ld (hl), 0
    ldir
    ; seed a C-major scale in channel 0, rows 0..7
    ld hl, tk_pat
    ld b, 8
    ld c, 1
ti_seed:
    ld (hl), c
    inc c
    ld de, NT_CH
    add hl, de
    djnz ti_seed
    xor a
    ld (tk_crow), a
    ld (tk_cch), a
    ld (tk_prow), a
    ld a, TK_STEPF
    ld (tk_ptmr), a
    ld a, 1
    ld (v_dirty), a
    ret

; tk_cell_idx: -> HL = &tk_pat[tk_crow*NT_CH + tk_cch]
tk_cell_idx:
    ld a, (tk_crow)
    add a, a
    add a, a
    ld b, a
    ld a, (tk_cch)
    add a, b
    ld l, a
    ld h, 0
    ld de, tk_pat
    add hl, de
    ret

tracker_input:
    call topmost_proc               ; ix = tracker window
    ld a, (v_pad_edge)
    bit 5, a
    jp nz, close_topmost
    ld a, (v_pad_edge)
    bit 0, a
    call nz, tk_up
    ld a, (v_pad_edge)
    bit 1, a
    call nz, tk_down
    ld a, (v_pad_edge)
    bit 2, a
    call nz, tk_left
    ld a, (v_pad_edge)
    bit 3, a
    call nz, tk_right
    ld a, (v_pad_edge)
    bit 4, a
    call nz, tk_cycle
    ; auto-play step
    ld a, (v_gpause)
    or a
    jr nz, ti_hover
    ld a, (tk_ptmr)
    dec a
    ld (tk_ptmr), a
    or a
    jr nz, ti_hover
    ld a, TK_STEPF
    ld (tk_ptmr), a
    ld a, (tk_prow)
    inc a
    cp NT_ROWS
    jr c, ti_setp
    xor a
ti_setp:
    ld (tk_prow), a
    call tk_play_row
    ld a, 1
    ld (v_dirty), a
ti_hover:
    call tk_hover
    ret

tk_up:
    ld a, (tk_crow)
    or a
    ret z
    dec a
    ld (tk_crow), a
    jp set_dirty
tk_down:
    ld a, (tk_crow)
    cp NT_ROWS-1
    ret nc
    inc a
    ld (tk_crow), a
    jp set_dirty
tk_left:
    ld a, (tk_cch)
    or a
    ret z
    dec a
    ld (tk_cch), a
    jp set_dirty
tk_right:
    ld a, (tk_cch)
    cp NT_CH-1
    ret nc
    inc a
    ld (tk_cch), a
    jp set_dirty
tk_cycle:
    call tk_cell_idx
    ld a, (hl)
    inc a
    cp 9
    jr c, tc_st
    xor a
tc_st:
    ld (hl), a
    jp set_dirty

tk_hover:
    ld a, (ix+WX)
    add a, 2
    ld b, a
    ld a, (tk_cch)
    add a, a
    add a, a
    add a, b                        ; col = WX+2 + cch*4
    add a, a
    add a, a
    add a, a                        ; *8
    ld (v_curx), a
    ld a, (ix+WY)
    add a, 2
    ld b, a
    ld a, (tk_crow)
    add a, b
    add a, a
    add a, a
    add a, a
    ld (v_cury), a
    ret

; ---- draw (ix = tracker window) -------------------------------------------
tracker_draw:
    xor a
    ld (tk_r), a
tdg_row:
    ld a, (tk_r)
    cp NT_ROWS
    jp nc, tdg_done
    ld a, (tk_prow)
    ld b, a
    ld a, (tk_r)
    cp b
    jr nz, tdg_nomark
    ld a, (ix+WX)
    inc a
    ld d, a
    ld a, (ix+WY)
    add a, 2
    ld b, a
    ld a, (tk_r)
    add a, b
    ld e, a
    ld hl, s_tkmark
    ld c, T_FONT
    call puts_at
tdg_nomark:
    xor a
    ld (tk_c), a
tdg_ch:
    ld a, (tk_c)
    cp NT_CH
    jp nc, tdg_rownext
    ; note = tk_pat[tk_r*NT_CH + tk_c]
    ld a, (tk_r)
    add a, a
    add a, a
    ld b, a
    ld a, (tk_c)
    add a, b
    ld l, a
    ld h, 0
    ld de, tk_pat
    add hl, de
    ld a, (hl)
    ; name = tk_names + note*3
    ld l, a
    ld h, 0
    add hl, hl
    ld e, a
    ld d, 0
    add hl, de                      ; note*3
    ld de, tk_names
    add hl, de
    push hl
    ld a, (ix+WX)
    add a, 2
    ld b, a
    ld a, (tk_c)
    add a, a
    add a, a
    add a, b
    ld d, a                         ; col = WX+2 + tk_c*4
    ld a, (ix+WY)
    add a, 2
    ld b, a
    ld a, (tk_r)
    add a, b
    ld e, a                         ; row = WY+2 + tk_r
    pop hl
    ld c, T_FONT
    call puts_at
    ld a, (tk_c)
    inc a
    ld (tk_c), a
    jp tdg_ch
tdg_rownext:
    ld a, (tk_r)
    inc a
    ld (tk_r), a
    jp tdg_row
tdg_done:
    ret

  IFDEF AUTOTEST_TRACKER
auto_script:
    db 5, 140, 84, 0               ; settle on the Tracker icon (i=6: col17,row10)
    db 2, 140, 84, $10              ; click -> launch Tracker
    db 4, 76, 86, 0
    db 2, 0,0, $02                  ; down
    db 2, 0,0, 0
    db 2, 0,0, $08                  ; right (to channel 1)
    db 2, 0,0, 0
    db 2, 0,0, $10                  ; cycle a note in
    db 2, 0,0, 0
    db 2, 0,0, $10
    db 2, 0,0, 0
    db 2, 0,0, $10
    db 40, 0,0, 0                   ; let the playhead run
    db 0,0,0,0
  ENDIF
