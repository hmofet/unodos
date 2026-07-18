; ============================================================================
; UnoDOS / Game Boy — Dostris (falling-blocks game), the M3 interactive showcase.
; The same algorithm the NES/SMS ports use (get_mask / piece_collide / lock /
; clear-lines / gravity), ported to Sharp SM83. Board is BW x BH cells; each byte
; holds a block tile (0 = empty). Piece tables come from the Contract-shaped
; mkdata generator (piece_masks / piece_tiles). While Dostris is the resident app
; it owns the d-pad (left/right move, up or A rotate, down soft-drops).
; ============================================================================

SECTION "dostris", ROM0

; ---- get_mask: g_type,g_rot -> mlo/mhi -------------------------------------
get_mask:
    ld a, [g_type]
    add a, a
    add a, a                                ; type*4
    ld b, a
    ld a, [g_rot]
    add a, b                                ; + rot
    add a, a                                ; *2 (word)
    ld e, a
    ld d, 0
    ld hl, piece_masks
    add hl, de
    ld a, [hl+]
    ld [mlo], a
    ld a, [hl]
    ld [mhi], a
    ret

; ---- rng / mod7 ------------------------------------------------------------
rng:
    ld a, [g_seed]
    ld b, a
    add a, a
    add a, a                                ; *4
    add a, b                                ; *5
    inc a
    ld [g_seed], a
    ret
mod7:
    cp 7
    ret c
    sub 7
    jr mod7

; ---- board_off: g_by,g_bx -> HL = g_board + by*BW + bx ---------------------
board_off:
    ld a, [g_by]
    add a, a                                ; by*2
    ld b, a
    add a, a
    add a, a                                ; by*8
    add a, b                                ; by*10
    ld b, a
    ld a, [g_bx]
    add a, b
    ld e, a
    ld d, 0
    ld hl, g_board
    add hl, de
    ret

; ---- mask_shift: mlo/mhi <<= 1 (next cell into bit15) ----------------------
mask_shift:
    ld a, [mlo]
    add a, a
    ld [mlo], a
    ld a, [mhi]
    adc a, a
    ld [mhi], a
    ret

; ---- piece_collide: trial g_tx,g_ty -> A=1 collide / 0 free ----------------
piece_collide:
    call get_mask
    ld c, 0                                 ; bit index 0..15
.loop:
    ld a, [mhi]
    bit 7, a
    jr z, .next
    ld a, c
    and 3
    ld b, a
    ld a, [g_tx]
    add a, b                                ; bx
    cp BW
    jr nc, .yes
    ld [g_bx], a
    ld a, c
    and $0C
    rrca
    rrca
    ld b, a
    ld a, [g_ty]
    add a, b                                ; by
    cp BH
    jr nc, .yes
    ld [g_by], a
    call board_off
    ld a, [hl]
    and a
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

; ---- moves -----------------------------------------------------------------
ds_left:
    ld a, [g_px]
    dec a
    ld [g_tx], a
    ld a, [g_py]
    ld [g_ty], a
    call piece_collide
    and a
    ret nz
    ld a, [g_tx]
    ld [g_px], a
    ret
ds_right:
    ld a, [g_px]
    inc a
    ld [g_tx], a
    ld a, [g_py]
    ld [g_ty], a
    call piece_collide
    and a
    ret nz
    ld a, [g_tx]
    ld [g_px], a
    ret
ds_rotate:
    ld a, [g_rot]
    ld [g_srot], a
    inc a
    and 3
    ld [g_rot], a
    ld a, [g_px]
    ld [g_tx], a
    ld a, [g_py]
    ld [g_ty], a
    call piece_collide
    and a
    ret z
    ld a, [g_srot]
    ld [g_rot], a                           ; blocked: revert
    ret
ds_softdrop:
    ld a, [g_px]
    ld [g_tx], a
    ld a, [g_py]
    inc a
    ld [g_ty], a
    call piece_collide
    and a
    ret nz
    ld hl, g_py
    inc [hl]
    ret

; ============================================================================
; dostris_update: one frame of game logic (input + gravity)
; ============================================================================
dostris_update:
    ld a, [g_state]
    and a
    ret nz                                  ; game over: nothing (B handled above)
    ; save the pre-move pose
    ld a, [g_px]
    ld [g_oldpx], a
    ld a, [g_py]
    ld [g_oldpy], a
    ld a, [g_rot]
    ld [g_oldrot], a
    ; input
    ld a, [v_pade]
    and PAD_L
    jr z, .r
    call ds_left
.r:
    ld a, [v_pade]
    and PAD_R
    jr z, .rot
    call ds_right
.rot:
    ld a, [v_pade]
    and PAD_U
    jr z, .a
    call ds_rotate
.a:
    ld a, [v_pade]
    and PAD_A
    jr z, .dn
    call ds_rotate
.dn:
    ld a, [v_pad]
    and PAD_D
    jr z, .grav
    call ds_softdrop
.grav:
  IF AUTOTEST
    ld a, [a_gpause]
    and a
    jr nz, .check
  ENDC
    ld hl, g_fall
    inc [hl]
    ld a, [hl]
    cp FALLRATE
    jr c, .check
    xor a
    ld [g_fall], a
    ld a, [g_px]
    ld [g_tx], a
    ld a, [g_py]
    inc a
    ld [g_ty], a
    call piece_collide
    and a
    jr z, .fall
    call dostris_lock
    call dostris_clearlines
    call dostris_spawn
    ; Only the board region changed (locked cells / collapsed rows / new piece).
    ; Repaint just the board via dostris_draw in vblank instead of raising
    ; v_dirty, which did an lcd_off + a 1024-entry clear_map + a full
    ; draw_app every landing (a visible LCD-off settle). dostris_draw rebuilds
    ; walls+floor+board+piece+score from g_board, covering every lock outcome.
    ld a, 1
    ld [pf_bd], a
    xor a
    ld [pf_pc], a
    ret
.fall:
    ld hl, g_py
    inc [hl]
.check:
    ld a, [g_px]
    ld hl, g_oldpx
    cp [hl]
    jr nz, .moved
    ld a, [g_py]
    ld hl, g_oldpy
    cp [hl]
    jr nz, .moved
    ld a, [g_rot]
    ld hl, g_oldrot
    cp [hl]
    jr nz, .moved
    ret
.moved:
    ld a, 1
    ld [pf_pc], a
    ret

; ---- dostris_lock: stamp the current piece into the board ------------------
dostris_lock:
    ld a, [g_type]
    ld e, a
    ld d, 0
    ld hl, piece_tiles
    add hl, de
    ld a, [hl]
    ld [g_lt], a
    ld a, [g_px]
    ld [g_tx], a
    ld a, [g_py]
    ld [g_ty], a
    call get_mask
    ld c, 0
.loop:
    ld a, [mhi]
    bit 7, a
    jr z, .next
    ld a, c
    and 3
    ld b, a
    ld a, [g_tx]
    add a, b
    ld [g_bx], a
    ld a, c
    and $0C
    rrca
    rrca
    ld b, a
    ld a, [g_ty]
    add a, b
    ld [g_by], a
    push bc
    call board_off
    ld a, [g_lt]
    ld [hl], a
    pop bc
.next:
    call mask_shift
    inc c
    ld a, c
    cp 16
    jr nz, .loop
    ret

; ---- dostris_spawn: a new piece at the top -> game over if blocked ----------
dostris_spawn:
    call rng
    call mod7
    ld [g_type], a
    xor a
    ld [g_rot], a
    ld [g_fall], a
    ld a, 3
    ld [g_px], a
    xor a
    ld [g_py], a
    ld a, [g_px]
    ld [g_tx], a
    ld a, [g_py]
    ld [g_ty], a
    call piece_collide
    and a
    jr z, .ok
    ld a, 1
    ld [g_state], a
.ok:
    ld a, [g_px]
    ld [g_oldpx], a
    ld a, [g_py]
    ld [g_oldpy], a
    ld a, [g_rot]
    ld [g_oldrot], a
    ret

; ---- dostris_init: fresh board + first piece -------------------------------
dostris_init:
    ld hl, g_board
    ld c, BW*BH
    xor a
.clr:
    ld [hl+], a
    dec c
    jr nz, .clr
    xor a
    ld [g_lines], a
    ld [g_state], a
    ld [g_fall], a
    ld a, [v_tick]
    or 1
    ld [g_seed], a
    call dostris_spawn
    ret

; ---- line clearing ---------------------------------------------------------
dostris_clearlines:
    ld a, BH-1
    ld [g_row], a
.loop:
    ld a, [g_row]
    cp $FF
    ret z
    call row_full
    and a
    jr z, .dec
    call collapse_row
    ld hl, g_lines
    inc [hl]
    jr .loop
.dec:
    ld hl, g_row
    dec [hl]
    jr .loop

; row_full: -> A=1 if row g_row is full, else A=0
row_full:
    ld a, [g_row]
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
.loop:
    ld a, [hl+]
    and a
    jr z, .no
    dec b
    jr nz, .loop
    ld a, 1
    ret
.no:
    xor a
    ret

; collapse_row: drop every row above g_row down by one, clear row 0
collapse_row:
    ld a, [g_row]
    ld [g_tmp], a
.loop:
    ld a, [g_tmp]
    and a
    jr z, .top
    ; dst index = g_tmp*10
    ld a, [g_tmp]
    add a, a
    ld b, a
    add a, a
    add a, a
    add a, b                                ; tmp*10
    ld c, a                                 ; dst index
    sub BW
    ld e, a                                 ; src index = dst-10
    ld d, 0
    ld hl, g_board
    add hl, de                              ; hl = src ptr
    ld a, c
    ld e, a
    ld d, 0
    push hl                                 ; src
    ld hl, g_board
    add hl, de                              ; dst ptr
    ld d, h
    ld e, l                                 ; de = dst
    pop hl                                  ; hl = src
    ld b, BW
.copy:
    ld a, [hl+]
    ld [de], a
    inc de
    dec b
    jr nz, .copy
    ld hl, g_tmp
    dec [hl]
    jr .loop
.top:
    ld hl, g_board
    ld b, BW
.clr:
    xor a
    ld [hl+], a
    dec b
    jr nz, .clr
    ret

; ============================================================================
; rendering
; ============================================================================
; dostris_draw: full board + frame + piece + score (LCD off / vblank-safe).
dostris_draw:
    ; left + right walls
    ld c, 0
.walls:
    ld a, BORG_COL-1
    ld [col], a
    ld a, c
    add a, BORG_ROW
    ld [row], a
    push bc
    call xy_to_hl
    ld a, T_WHITE
    ld [hl], a
    pop bc
    ld a, BORG_COL+BW
    ld [col], a
    ld a, c
    add a, BORG_ROW
    ld [row], a
    push bc
    call xy_to_hl
    ld a, T_WHITE
    ld [hl], a
    pop bc
    inc c
    ld a, c
    cp BH
    jr nz, .walls
    ; floor
    ld a, BORG_COL-1
    ld [col], a
    ld a, BORG_ROW+BH
    ld [row], a
    call xy_to_hl
    ld b, BW+2
.floor:
    ld a, T_WHITE
    ld [hl+], a
    dec b
    jr nz, .floor
    ; board cells
    xor a
    ld [g_row], a
.brow:
    ld a, [g_row]
    cp BH
    jr nc, .after
    ld a, BORG_COL
    ld [col], a
    ld a, [g_row]
    add a, BORG_ROW
    ld [row], a
    call xy_to_hl
    push hl                                 ; VRAM dst
    ld a, [g_row]
    add a, a
    ld b, a
    add a, a
    add a, a
    add a, b                                ; row*10
    ld e, a
    ld d, 0
    ld hl, g_board
    add hl, de
    ld d, h
    ld e, l                                 ; de = board src
    pop hl                                  ; hl = VRAM dst
    ld b, BW
.cell:
    ld a, [de]
    ld [hl+], a
    inc de
    dec b
    jr nz, .cell
    ld hl, g_row
    inc [hl]
    jr .brow
.after:
    ld a, [g_state]
    and a
    jr nz, .score
    ld a, [g_px]
    ld [g_tx], a
    ld a, [g_py]
    ld [g_ty], a
    ld a, [g_type]
    ld e, a
    ld d, 0
    ld hl, piece_tiles
    add hl, de
    ld a, [hl]
    ld [g_pt], a
    call draw_piece_cells
.score:
    call dostris_draw_score
    ld a, [g_state]
    and a
    ret z
    ld a, BORG_COL
    ld [col], a
    ld a, BORG_ROW+5
    ld [row], a
    call xy_to_hl
    ld de, s_gameover
    ld b, T_FONTINV
    jp wstr

dostris_draw_score:
    ld a, BORG_COL+BW+1
    ld [col], a
    ld a, BORG_ROW
    ld [row], a
    call xy_to_hl
    ld de, s_lines
    ld b, T_FONT
    call wstr
    ld a, [g_lines]
    call two_digits
    ld [numstr+1], a
    ld a, d
    ld [numstr], a
    xor a
    ld [numstr+2], a
    ld a, BORG_COL+BW+1
    ld [col], a
    ld a, BORG_ROW+1
    ld [row], a
    call xy_to_hl
    ld de, numstr
    ld b, T_FONT
    jp wstr

; draw_piece_cells: write g_pt at each cell of mask(g_type,g_rot) ------------
draw_piece_cells:
    call get_mask
    ld c, 0
.loop:
    ld a, [mhi]
    bit 7, a
    jr z, .next
    ld a, c
    and 3
    ld b, a
    ld a, [g_tx]
    add a, b
    add a, BORG_COL
    ld [col], a
    ld a, c
    and $0C
    rrca
    rrca
    ld b, a
    ld a, [g_ty]
    add a, b
    add a, BORG_ROW
    ld [row], a
    push bc
    call xy_to_hl
    ld a, [g_pt]
    ld [hl], a
    pop bc
.next:
    call mask_shift
    inc c
    ld a, c
    cp 16
    jr nz, .loop
    ret

; draw_piece_partial: erase the old pose, draw the new (vblank) --------------
draw_piece_partial:
    ld a, [g_oldpx]
    ld [g_tx], a
    ld a, [g_oldpy]
    ld [g_ty], a
    ld a, [g_rot]
    push af
    ld a, [g_oldrot]
    ld [g_rot], a
    xor a
    ld [g_pt], a
    call draw_piece_cells
    pop af
    ld [g_rot], a
    ld a, [g_px]
    ld [g_tx], a
    ld a, [g_py]
    ld [g_ty], a
    ld a, [g_type]
    ld e, a
    ld d, 0
    ld hl, piece_tiles
    add hl, de
    ld a, [hl]
    ld [g_pt], a
    call draw_piece_cells
    ld a, [g_px]
    ld [g_oldpx], a
    ld a, [g_py]
    ld [g_oldpy], a
    ld a, [g_rot]
    ld [g_oldrot], a
    ret

s_lines:    db "Ln", 0
s_gameover: db "OVER", 0
