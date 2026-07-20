; ============================================================================
; UnoDOS/SMS — OutLast (proc 8). The pseudo-3D racer (C64 band model) fitted to a
; window: OL_BANDS rows, each a grass strip + a road span whose half-width grows
; and whose centre snakes; a steerable car on the bottom band; off-road = CRASH.
; Drawn with fill_cells (green grass / white road / yellow stripes / red car).
; While topmost it owns the d-pad (L/R steer, B2 close). Distance is the score.
; ============================================================================

OL_BANDS EQU 10
OL_COLS  EQU 18
OL_RATE  EQU 4

ol_carx   EQU $C250
ol_scroll EQU $C251
ol_dist   EQU $C252       ; word
ol_over   EQU $C254
ol_ctr    EQU $C255
ol_r      EQU $C256       ; draw band scratch

; ol_band: A = band -> B = left col, C = right col (0..OL_COLS-1)
ol_band:
    push af
    srl a
    srl a
    add a, 2                         ; hw = 2 + band/4
    ld d, a
    pop af
    ld e, a                          ; band
    ld a, (ol_scroll)
    add a, e
    and 7
    cp 4
    jr c, ob_pos
    sub 8                            ; curve -4..3
ob_pos:
    add a, 9                         ; centre = 9 + curve
    ld e, a
    sub d                            ; left = centre - hw
    jp p, ob_lok
    xor a
ob_lok:
    ld b, a
    ld a, e
    add a, d                         ; right = centre + hw
    cp OL_COLS
    jr c, ob_rok
    ld a, OL_COLS-1
ob_rok:
    ld c, a
    ret

outlast_init:
    ld a, 9
    ld (ol_carx), a
    xor a
    ld (ol_scroll), a
    ld (ol_dist), a
    ld (ol_dist+1), a
    ld (ol_over), a
    ld a, OL_RATE
    ld (ol_ctr), a
    ld a, 1
    ld (v_dirty), a
    ret

ol_check:
    ld a, OL_BANDS-1
    call ol_band                    ; b=left c=right
    ld a, (ol_carx)
    cp b
    jr c, oc_off                    ; carx < left
    cp c
    jr z, oc_on
    jr nc, oc_off                   ; carx > right
oc_on:
    ret
oc_off:
    ld a, 1
    ld (ol_over), a
    ret

outlast_input:
    call topmost_proc               ; ix = window
    ld a, (v_pad_edge)
    bit 5, a
    jp nz, close_topmost
    ld a, (ol_over)
    or a
    jr nz, oi_hover
    ld a, (v_pad_edge)
    bit 2, a
    call nz, ol_left
    ld a, (v_pad_edge)
    bit 3, a
    call nz, ol_right
    ld a, (v_gpause)
    or a
    jr nz, oi_hover
    ld a, (ol_ctr)
    dec a
    ld (ol_ctr), a
    or a
    jr nz, oi_hover
    ld a, OL_RATE
    ld (ol_ctr), a
    ld a, (ol_scroll)
    inc a
    ld (ol_scroll), a
    ld hl, (ol_dist)
    inc hl
    ld (ol_dist), hl
    call ol_check
    ld a, 1
    ld (v_dirty), a
oi_hover:
    ; park the sprite on the car
    ld a, (ix+WX)
    inc a
    ld b, a
    ld a, (ol_carx)
    add a, b
    add a, a
    add a, a
    add a, a
    ld (v_curx), a
    ld a, (ix+WY)
    add a, OL_BANDS
    add a, a
    add a, a
    add a, a
    ld (v_cury), a
    ret
ol_left:
    ld a, (ol_carx)
    or a
    ret z
    dec a
    ld (ol_carx), a
    jp set_dirty
ol_right:
    ld a, (ol_carx)
    cp OL_COLS-1
    ret nc
    inc a
    ld (ol_carx), a
    jp set_dirty

; ol_setrow: reset dp_row = WY+1+ol_r (fill_cells advances dp_row each call).
ol_setrow:
    ld a, (ix+WY)
    inc a
    ld d, a
    ld a, (ol_r)
    add a, d
    ld (dp_row), a
    ret

; ---- draw (ix = window) ---------------------------------------------------
outlast_draw:
    xor a
    ld (ol_r), a
od_band:
    ld a, (ol_r)
    cp OL_BANDS
    jp nc, od_car
    ld a, (ol_r)
    call ol_band                    ; b=left c=right
    push bc
    ; grass row
    ld a, (ix+WX)
    inc a
    ld (dp_col), a
    ld a, (ix+WY)
    inc a
    ld d, a
    ld a, (ol_r)
    add a, d
    ld (dp_row), a
    ld a, OL_COLS
    ld (dp_w), a
    ld a, 1
    ld (dp_h), a
    ld a, T_SGREEN
    ld (dp_tile), a
    call fill_cells
    pop bc
    push bc
    ; road span
    ld a, (ix+WX)
    inc a
    add a, b
    ld (dp_col), a
    ld a, c
    sub b
    inc a
    ld (dp_w), a
    call ol_setrow
    ld a, T_SOLW
    ld (dp_tile), a
    call fill_cells
    pop bc
    ; stripes on alternate bands
    ld a, (ol_scroll)
    ld d, a
    ld a, (ol_r)
    add a, d
    and 1
    jr z, od_nostripe
    push bc
    ld a, (ix+WX)
    inc a
    add a, b
    ld (dp_col), a
    ld a, 1
    ld (dp_w), a
    call ol_setrow
    ld a, T_SYELLOW
    ld (dp_tile), a
    call fill_cells
    pop bc
    ld a, (ix+WX)
    inc a
    add a, c
    ld (dp_col), a
    ld a, 1
    ld (dp_w), a
    call ol_setrow
    ld a, T_SYELLOW
    ld (dp_tile), a
    call fill_cells
od_nostripe:
    ld a, (ol_r)
    inc a
    ld (ol_r), a
    jp od_band
od_car:
    ld a, (ix+WX)
    inc a
    ld b, a
    ld a, (ol_carx)
    add a, b
    ld (dp_col), a
    ld a, (ix+WY)
    add a, OL_BANDS
    ld (dp_row), a
    ld a, 1
    ld (dp_w), a
    ld (dp_h), a
    ld a, T_SRED
    ld (dp_tile), a
    call fill_cells
    ; HUD: Dist + crash
    ld a, (ix+WX)
    add a, OL_COLS+1
    ld d, a
    ld a, (ix+WY)
    inc a
    ld e, a
    ld hl, s_ol_dist
    ld c, T_FONT
    call puts_at
    ld a, (ol_dist)
    ld hl, g_numstr
    call put2
    xor a
    ld (hl), a
    ld a, (ix+WX)
    add a, OL_COLS+1
    ld d, a
    ld a, (ix+WY)
    add a, 2
    ld e, a
    ld hl, g_numstr
    ld c, T_FONT
    call puts_at
    ld a, (ol_over)
    or a
    ret z
    ld a, (ix+WX)
    add a, 6
    ld d, a
    ld a, (ix+WY)
    add a, 5
    ld e, a
    ld hl, s_ol_crash
    ld c, T_FONT
    call puts_at
    ret

s_ol_dist:  db "D",0
s_ol_crash: db "CRASH",0

  IFDEF AUTOTEST_OUTLAST
auto_script:
    db 5, 12, 138, 0                 ; settle on the OutLast icon (i=8: col1,row17)
    db 2, 12, 138, $10               ; (approx) click -> launch OutLast
    db 4, 12, 138, 0
    db 90, 0,0, 0                   ; drive straight (stays centred, shows the road)
    db 0,0,0,0
  ENDIF
