; ============================================================================
; UnoDOS/SMS — Paint (proc 10). A windowed cell canvas + palette row, drawn as
; VDP name-table tiles. While the Paint window is topmost it owns the d-pad
; (main_loop routes to paint_input): the cursor (the WM sprite) hovers a cell;
; B1 paints the current colour on the canvas / picks a swatch on the palette row;
; B2 closes. Colours are the solid-colour tiles (T_SOLW..T_SORANGE) + erase(0).
; Canvas is PCW x PCH tiles in pcanvas; palette row sits just below it.
; ============================================================================

PCW EQU 18
PCH EQU 10
NSW EQU 8

p_cx     EQU $C140       ; cursor col (0..PCW-1 on canvas, 0..NSW-1 on palette)
p_cy     EQU $C141       ; cursor row (0..PCH-1 canvas, PCH = palette row)
p_col    EQU $C142       ; current colour tile
p_r      EQU $C143       ; draw loop scratch (row)
p_c      EQU $C144       ; draw loop scratch (col)
pcanvas  EQU $C150       ; PCW*PCH tiles

paint_swatches:
    db 191, 192, 193, 246, 247, 248, 249, 0   ; white cyan magenta green yellow red orange erase

paint_init:
    ld hl, pcanvas
    ld de, pcanvas+1
    ld bc, PCW*PCH-1
    ld (hl), 0
    ldir
    ; seed a small demo pattern (top-left)
    ld a, 191
    ld (pcanvas+0), a
    ld a, 192
    ld (pcanvas+1), a
    ld a, 193
    ld (pcanvas+2), a
    xor a
    ld (p_cx), a
    ld (p_cy), a
    ld a, 191
    ld (p_col), a
    ld a, 1
    ld (v_dirty), a
    ret

; paint_cell: B=col C=row A=tile -> write one name-table cell.
paint_cell:
    push af
    call cell_addr
    call vram_set_addr
    pop af
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
    ret

; ---- draw (ix = paint window) ---------------------------------------------
paint_draw:
    ; canvas cells
    ld hl, pcanvas
    xor a
    ld (p_r), a
pd_row:
    ld a, (p_r)
    cp PCH
    jr nc, pd_palette
    xor a
    ld (p_c), a
pd_col:
    ld a, (p_c)
    cp PCW
    jr nc, pd_rownext
    ld a, (ix+WX)
    inc a
    ld b, a
    ld a, (p_c)
    add a, b
    ld b, a                         ; col = WX+1+p_c
    ld a, (ix+WY)
    inc a
    ld c, a
    ld a, (p_r)
    add a, c
    ld c, a                         ; row = WY+1+p_r
    ld a, (hl)
    push hl
    call paint_cell
    pop hl
    inc hl
    ld a, (p_c)
    inc a
    ld (p_c), a
    jr pd_col
pd_rownext:
    ld a, (p_r)
    inc a
    ld (p_r), a
    jr pd_row
pd_palette:
    ; swatch row at WY+1+PCH+1
    xor a
    ld (p_c), a
pd_sw:
    ld a, (p_c)
    cp NSW
    jr nc, pd_curcol
    ld a, (ix+WX)
    inc a
    ld b, a
    ld a, (p_c)
    add a, b
    ld b, a
    ld a, (ix+WY)
    add a, PCH+2
    ld c, a
    ld hl, paint_swatches
    ld a, (p_c)
    ld e, a
    ld d, 0
    add hl, de
    ld a, (hl)
    push hl
    call paint_cell
    pop hl
    ld a, (p_c)
    inc a
    ld (p_c), a
    jr pd_sw
pd_curcol:
    ; current-colour indicator + hint
    ld a, (ix+WX)
    add a, NSW+2
    ld b, a
    ld a, (ix+WY)
    add a, PCH+2
    ld c, a
    ld a, (p_col)
    call paint_cell
    ld a, (ix+WX)
    inc a
    ld d, a
    ld a, (ix+WY)
    add a, PCH+3
    ld e, a
    ld hl, s_paint_hint
    ld c, T_FONT
    call puts_at
    ret

; ---- input (Paint owns the d-pad while topmost) ---------------------------
paint_input:
    call topmost_proc               ; ix = paint window
    ld a, (v_pad_edge)
    bit 5, a                        ; B2 -> close
    jp nz, close_topmost
    ld a, (p_cy)
    cp PCH
    jr z, pi_pal
    ; --- canvas ---
    ld a, (v_pad_edge)
    bit 2, a
    call nz, pi_cleft
    ld a, (v_pad_edge)
    bit 3, a
    call nz, pi_cright
    ld a, (v_pad_edge)
    bit 0, a
    call nz, pi_cup
    ld a, (v_pad_edge)
    bit 1, a
    call nz, pi_cdown
    ld a, (v_pad_edge)
    bit 4, a
    call nz, pi_paint
    jp pi_fin
pi_pal:
    ld a, (v_pad_edge)
    bit 2, a
    call nz, pi_pleft
    ld a, (v_pad_edge)
    bit 3, a
    call nz, pi_pright
    ld a, (v_pad_edge)
    bit 0, a
    call nz, pi_tocanvas
    ld a, (v_pad_edge)
    bit 1, a
    call nz, pi_tocanvas
    ld a, (v_pad_edge)
    bit 4, a
    call nz, pi_pick
pi_fin:
    call paint_hover
    ret

pi_cleft:
    ld a, (p_cx)
    or a
    ret z
    dec a
    ld (p_cx), a
    jp set_dirty
pi_cright:
    ld a, (p_cx)
    cp PCW-1
    ret nc
    inc a
    ld (p_cx), a
    jp set_dirty
pi_cup:
    ld a, (p_cy)
    or a
    jr z, pi_cup_pal
    dec a
    ld (p_cy), a
    jp set_dirty
pi_cup_pal:
    ld a, PCH
    ld (p_cy), a
    jp set_dirty
pi_cdown:
    ld a, (p_cy)
    cp PCH-1
    jr nc, pi_cdown_pal
    inc a
    ld (p_cy), a
    jp set_dirty
pi_cdown_pal:
    ld a, PCH
    ld (p_cy), a
    jp set_dirty
pi_paint:
    ; pcanvas[p_cy*PCW + p_cx] = p_col
    ld a, (p_cy)
    ld l, a
    ld h, 0
    ld d, h
    ld e, l
    add hl, hl          ; *2
    add hl, hl          ; *4
    add hl, hl          ; *8
    add hl, hl          ; *16
    add hl, de          ; *17
    add hl, de          ; *18 (PCW)
    ld a, (p_cx)
    ld e, a
    ld d, 0
    add hl, de
    ld de, pcanvas
    add hl, de
    ld a, (p_col)
    ld (hl), a
    jp set_dirty
pi_pleft:
    ld a, (p_cx)
    or a
    ret z
    dec a
    ld (p_cx), a
    jp set_dirty
pi_pright:
    ld a, (p_cx)
    cp NSW-1
    ret nc
    inc a
    ld (p_cx), a
    jp set_dirty
pi_tocanvas:
    ld a, (p_cx)
    cp PCW
    jr c, pt_ok
    ld a, PCW-1
    ld (p_cx), a
pt_ok:
    ld a, PCH-1
    ld (p_cy), a
    jp set_dirty
pi_pick:
    ld hl, paint_swatches
    ld a, (p_cx)
    ld e, a
    ld d, 0
    add hl, de
    ld a, (hl)
    ld (p_col), a
    jp set_dirty

; paint_hover: park the WM sprite cursor on the current cell.
paint_hover:
    ld a, (p_cy)
    cp PCH
    jr z, ph_pal
    ld a, (ix+WX)
    inc a
    ld b, a
    ld a, (p_cx)
    add a, b
    add a, a
    add a, a
    add a, a
    ld (v_curx), a
    ld a, (ix+WY)
    inc a
    ld b, a
    ld a, (p_cy)
    add a, b
    add a, a
    add a, a
    add a, a
    ld (v_cury), a
    ret
ph_pal:
    ld a, (ix+WX)
    inc a
    ld b, a
    ld a, (p_cx)
    add a, b
    add a, a
    add a, a
    add a, a
    ld (v_curx), a
    ld a, (ix+WY)
    add a, PCH+2
    add a, a
    add a, a
    add a, a
    ld (v_cury), a
    ret

s_paint_hint: db "B1 paint",0

  IFDEF AUTOTEST_PAINT
auto_script:
    db 5, 140,140, 0                ; settle on the Paint icon
    db 2, 140,140, $10              ; click -> launch Paint
    db 4, 140,140, 0
    db 2, 0,0, $10                  ; paint (0,0)
    db 2, 0,0, 0
    db 2, 0,0, $08                  ; right
    db 2, 0,0, 0
    db 2, 0,0, $08                  ; right
    db 2, 0,0, 0
    db 2, 0,0, $10                  ; paint (2,0)
    db 2, 0,0, 0
    db 2, 0,0, $02                  ; down
    db 2, 0,0, 0
    db 2, 0,0, $10                  ; paint (2,1)
    db 2, 0,0, 0
    db 2, 0,0, $02                  ; down
    db 2, 0,0, 0
    db 2, 0,0, $10                  ; paint (2,2)
    db 4, 0,0, 0
    db 0,0,0,0
  ENDIF
