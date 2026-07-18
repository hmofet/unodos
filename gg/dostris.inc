; ============================================================================
; UnoDOS / Game Gear — Dostris (falling-blocks game). The same algorithm every
; port uses (get_mask / piece_collide / lock / clear-lines / gravity), in Z80;
; the board renders full-screen at the (6,3) visible offset via xy_to_vram.
; ============================================================================

get_mask:
    ld a, (g_type)
    add a, a
    add a, a
    ld b, a
    ld a, (g_rot)
    add a, b
    add a, a
    ld e, a
    ld d, 0
    ld hl, piece_masks
    add hl, de
    ld a, (hl)
    ld (mlo), a
    inc hl
    ld a, (hl)
    ld (mhi), a
    ret

rng:
    ld a, (g_seed)
    ld b, a
    add a, a
    add a, a
    add a, b
    inc a
    ld (g_seed), a
    ret
mod7:
    cp 7
    ret c
    sub 7
    jr mod7

board_off:
    ld a, (g_by)
    add a, a
    ld b, a
    add a, a
    add a, a
    add a, b                                ; by*10
    ld b, a
    ld a, (g_bx)
    add a, b
    ld e, a
    ld d, 0
    ld hl, g_board
    add hl, de
    ret

mask_shift:
    ld a, (mlo)
    add a, a
    ld (mlo), a
    ld a, (mhi)
    adc a, a
    ld (mhi), a
    ret

piece_collide:
    call get_mask
    ld c, 0
.loop:
    ld a, (mhi)
    bit 7, a
    jr z, .next
    ld a, c
    and 3
    ld b, a
    ld a, (g_tx)
    add a, b
    cp BW
    jr nc, .yes
    ld (g_bx), a
    ld a, c
    and $0C
    rrca
    rrca
    ld b, a
    ld a, (g_ty)
    add a, b
    cp BH
    jr nc, .yes
    ld (g_by), a
    call board_off
    ld a, (hl)
    or a
    jr nz, .yes
.next:
    call mask_shift
    inc c
    ld a, c
    cp 16
    jr nz, .loop
    xor a
    ret
.yes:
    ld a, 1
    ret

ds_left:
    ld a, (g_px)
    dec a
    ld (g_tx), a
    ld a, (g_py)
    ld (g_ty), a
    call piece_collide
    or a
    ret nz
    ld a, (g_tx)
    ld (g_px), a
    ret
ds_right:
    ld a, (g_px)
    inc a
    ld (g_tx), a
    ld a, (g_py)
    ld (g_ty), a
    call piece_collide
    or a
    ret nz
    ld a, (g_tx)
    ld (g_px), a
    ret
ds_rotate:
    ld a, (g_rot)
    ld (g_srot), a
    inc a
    and 3
    ld (g_rot), a
    ld a, (g_px)
    ld (g_tx), a
    ld a, (g_py)
    ld (g_ty), a
    call piece_collide
    or a
    ret z
    ld a, (g_srot)
    ld (g_rot), a
    ret
ds_softdrop:
    ld a, (g_px)
    ld (g_tx), a
    ld a, (g_py)
    inc a
    ld (g_ty), a
    call piece_collide
    or a
    ret nz
    ld hl, g_py
    inc (hl)
    ret

; ============================================================================
; dostris_update: one frame of game logic
; ============================================================================
dostris_update:
    ld a, (g_state)
    or a
    ret nz
    ld a, (g_px)
    ld (g_oldpx), a
    ld a, (g_py)
    ld (g_oldpy), a
    ld a, (g_rot)
    ld (g_oldrot), a
    ld a, (v_pade)
    and PAD_L
    jr z, .r
    call ds_left
.r:
    ld a, (v_pade)
    and PAD_R
    jr z, .rot
    call ds_right
.rot:
    ld a, (v_pade)
    and PAD_U
    jr z, .a
    call ds_rotate
.a:
    ld a, (v_pade)
    and PAD_A
    jr z, .dn
    call ds_rotate
.dn:
    ld a, (v_pad)
    and PAD_D
    jr z, .grav
    call ds_softdrop
.grav:
  IFDEF AUTOTEST
    ld a, (a_gpause)
    or a
    jr nz, .check
  ENDIF
    ld hl, g_fall
    inc (hl)
    ld a, (hl)
    cp FALLRATE
    jr c, .check
    xor a
    ld (g_fall), a
    ld a, (g_px)
    ld (g_tx), a
    ld a, (g_py)
    inc a
    ld (g_ty), a
    call piece_collide
    or a
    jr z, .fall
    call dostris_lock
    call dostris_clearlines
    call dostris_spawn
    ; Only the board region changed (locked cells / collapsed rows / new piece).
    ; Repaint just the board via dostris_draw instead of the display-off
    ; full_redraw, which blanked the whole screen every landing (the "black
    ; settle"). dostris_draw redraws walls+floor+board+piece+score from g_board,
    ; so it reconstructs any lock outcome (plain lock, line collapse, game over)
    ; without clearing the static chrome/desktop.
    ld a, 1
    ld (pf_bd), a
    xor a
    ld (pf_pc), a
    ret
.fall:
    ld hl, g_py
    inc (hl)
.check:
    ld a, (g_px)
    ld hl, g_oldpx
    cp (hl)
    jr nz, .moved
    ld a, (g_py)
    ld hl, g_oldpy
    cp (hl)
    jr nz, .moved
    ld a, (g_rot)
    ld hl, g_oldrot
    cp (hl)
    jr nz, .moved
    ret
.moved:
    ld a, 1
    ld (pf_pc), a
    ret

dostris_lock:
    ld a, (g_type)
    ld e, a
    ld d, 0
    ld hl, piece_tiles
    add hl, de
    ld a, (hl)
    ld (g_lt), a
    ld a, (g_px)
    ld (g_tx), a
    ld a, (g_py)
    ld (g_ty), a
    call get_mask
    ld c, 0
.loop:
    ld a, (mhi)
    bit 7, a
    jr z, .next
    ld a, c
    and 3
    ld b, a
    ld a, (g_tx)
    add a, b
    ld (g_bx), a
    ld a, c
    and $0C
    rrca
    rrca
    ld b, a
    ld a, (g_ty)
    add a, b
    ld (g_by), a
    push bc
    call board_off
    ld a, (g_lt)
    ld (hl), a
    pop bc
.next:
    call mask_shift
    inc c
    ld a, c
    cp 16
    jr nz, .loop
    ret

dostris_spawn:
    call rng
    call mod7
    ld (g_type), a
    xor a
    ld (g_rot), a
    ld (g_fall), a
    ld a, 3
    ld (g_px), a
    xor a
    ld (g_py), a
    ld a, (g_px)
    ld (g_tx), a
    ld a, (g_py)
    ld (g_ty), a
    call piece_collide
    or a
    jr z, .ok
    ld a, 1
    ld (g_state), a
.ok:
    ld a, (g_px)
    ld (g_oldpx), a
    ld a, (g_py)
    ld (g_oldpy), a
    ld a, (g_rot)
    ld (g_oldrot), a
    ret

dostris_init:
    ld hl, g_board
    ld b, BW*BH
    xor a
.clr:
    ld (hl), a
    inc hl
    djnz .clr
    xor a
    ld (g_lines), a
    ld (g_state), a
    ld (g_fall), a
    ld a, (v_tick)
    or 1
    ld (g_seed), a
    call dostris_spawn
    ret

dostris_clearlines:
    ld a, BH-1
    ld (g_row), a
.loop:
    ld a, (g_row)
    cp $FF
    ret z
    call row_full
    or a
    jr z, .dec
    call collapse_row
    ld hl, g_lines
    inc (hl)
    jr .loop
.dec:
    ld hl, g_row
    dec (hl)
    jr .loop

row_full:
    ld a, (g_row)
    add a, a
    ld b, a
    add a, a
    add a, a
    add a, b
    ld e, a
    ld d, 0
    ld hl, g_board
    add hl, de
    ld b, BW
.loop:
    ld a, (hl)
    or a
    jr z, .no
    inc hl
    djnz .loop
    ld a, 1
    ret
.no:
    xor a
    ret

collapse_row:
    ld a, (g_row)
    ld (g_tmp), a
.loop:
    ld a, (g_tmp)
    or a
    jr z, .top
    ld a, (g_tmp)
    add a, a
    ld b, a
    add a, a
    add a, a
    add a, b                                ; tmp*10
    ld c, a                                 ; dst index
    sub BW
    ld e, a                                 ; src index
    ld d, 0
    ld hl, g_board
    add hl, de                              ; hl = src
    ld a, c
    ld e, a
    ld d, 0
    push hl
    ld hl, g_board
    add hl, de                              ; hl = dst
    ex de, hl                               ; de = dst
    pop hl                                  ; hl = src
    ld b, BW
.copy:
    ld a, (hl)
    ld (de), a
    inc hl
    inc de
    djnz .copy
    ld hl, g_tmp
    dec (hl)
    jr .loop
.top:
    ld hl, g_board
    ld b, BW
.clr:
    ld (hl), 0
    inc hl
    djnz .clr
    ret

; ============================================================================
; rendering (display off for the full draw / frame-time for the partial)
; ============================================================================
dostris_draw:
    ld c, 0
.walls:
    ld a, BORG_COL-1
    ld (col), a
    ld a, c
    add a, BORG_ROW
    ld (row), a
    push bc
    call xy_to_vram
    ld a, T_WHITE
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
    pop bc
    ld a, BORG_COL+BW
    ld (col), a
    ld a, c
    add a, BORG_ROW
    ld (row), a
    push bc
    call xy_to_vram
    ld a, T_WHITE
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
    pop bc
    inc c
    ld a, c
    cp BH
    jr nz, .walls
    ld a, BORG_COL-1
    ld (col), a
    ld a, BORG_ROW+BH
    ld (row), a
    call xy_to_vram
    ld b, BW+2
.floor:
    ld a, T_WHITE
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
    djnz .floor
    xor a
    ld (g_row), a
.brow:
    ld a, (g_row)
    cp BH
    jr nc, .after
    ld a, BORG_COL
    ld (col), a
    ld a, (g_row)
    add a, BORG_ROW
    ld (row), a
    call xy_to_vram
    ld a, (g_row)
    add a, a
    ld b, a
    add a, a
    add a, a
    add a, b                                ; row*10
    ld e, a
    ld d, 0
    ld hl, g_board
    add hl, de
    ld b, BW
.cell:
    ld a, (hl)
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
    inc hl
    djnz .cell
    ld hl, g_row
    inc (hl)
    jr .brow
.after:
    ld a, (g_state)
    or a
    jr nz, .score
    ld a, (g_px)
    ld (g_tx), a
    ld a, (g_py)
    ld (g_ty), a
    ld a, (g_type)
    ld e, a
    ld d, 0
    ld hl, piece_tiles
    add hl, de
    ld a, (hl)
    ld (g_pt), a
    call draw_piece_cells
.score:
    call dostris_draw_score
    ld a, (g_state)
    or a
    ret z
    ld a, BORG_COL
    ld (col), a
    ld a, BORG_ROW+5
    ld (row), a
    ld hl, s_gameover
    ld b, T_FONTINV
    jp wstr

dostris_draw_score:
    ld a, BORG_COL+BW+1
    ld (col), a
    ld a, BORG_ROW
    ld (row), a
    ld hl, s_lines
    ld b, T_FONT
    call wstr
    ld a, (g_lines)
    call two_digits
    ld (numstr+1), a
    ld a, d
    ld (numstr), a
    xor a
    ld (numstr+2), a
    ld a, BORG_COL+BW+1
    ld (col), a
    ld a, BORG_ROW+1
    ld (row), a
    ld hl, numstr
    ld b, T_FONT
    jp wstr

draw_piece_cells:
    call get_mask
    ld c, 0
.loop:
    ld a, (mhi)
    bit 7, a
    jr z, .next
    ld a, c
    and 3
    ld b, a
    ld a, (g_tx)
    add a, b
    add a, BORG_COL
    ld (col), a
    ld a, c
    and $0C
    rrca
    rrca
    ld b, a
    ld a, (g_ty)
    add a, b
    add a, BORG_ROW
    ld (row), a
    push bc
    call xy_to_vram
    ld a, (g_pt)
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
    pop bc
.next:
    call mask_shift
    inc c
    ld a, c
    cp 16
    jr nz, .loop
    ret

draw_piece_partial:
    ld a, (g_oldpx)
    ld (g_tx), a
    ld a, (g_oldpy)
    ld (g_ty), a
    ld a, (g_rot)
    push af
    ld a, (g_oldrot)
    ld (g_rot), a
    xor a
    ld (g_pt), a
    call draw_piece_cells
    pop af
    ld (g_rot), a
    ld a, (g_px)
    ld (g_tx), a
    ld a, (g_py)
    ld (g_ty), a
    ld a, (g_type)
    ld e, a
    ld d, 0
    ld hl, piece_tiles
    add hl, de
    ld a, (hl)
    ld (g_pt), a
    call draw_piece_cells
    ld a, (g_px)
    ld (g_oldpx), a
    ld a, (g_py)
    ld (g_oldpy), a
    ld a, (g_rot)
    ld (g_oldrot), a
    ret

s_lines:    db "Ln", 0
s_gameover: db "OVER", 0
