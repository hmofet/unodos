; ============================================================================
; UnoDOS / WonderSwan — Dostris (the shared falling-blocks algorithm in x86-16).
; The board renders as 8x8 tile cells: empty = blank tile (pal0), filled = the
; solid block tile (TILE_BLOCK) with the piece's palette (2-7) for its shade.
; ============================================================================

; get_mask: g_type,g_rot -> mlo
get_mask:
    mov ax, [g_type]
    shl ax, 2
    add ax, [g_rot]
    shl ax, 1
    mov bx, ax
    mov ax, [cs:piece_masks+bx]
    mov [mlo], ax
    ret

; piece_collide: trial g_tx,g_ty (signed) -> AX=1 collide / 0 free
piece_collide:
    call get_mask
    mov si, [mlo]
    xor bp, bp
.loop:
    test si, 0x8000
    jz .next
    mov ax, bp
    and ax, 3
    add ax, [g_tx]           ; cell x (signed)
    cmp ax, BW
    jge .yes
    cmp ax, 0
    jl .yes
    mov di, ax               ; save cell x
    mov ax, bp
    and ax, 12
    shr ax, 2
    add ax, [g_ty]           ; cell y
    cmp ax, BHT
    jge .yes
    cmp ax, 0
    jl .yes
    mov bx, BW
    mul bx                   ; AX = cell y * BW
    add ax, di               ; + cell x
    mov bx, ax
    mov al, [g_board+bx]
    test al, al
    jnz .yes
.next:
    shl si, 1
    inc bp
    cmp bp, 16
    jne .loop
    xor ax, ax
    ret
.yes:
    mov ax, 1
    ret

; ---- moves -----------------------------------------------------------------
ds_left:
    mov ax, [g_px]
    dec ax
    mov [g_tx], ax
    mov ax, [g_py]
    mov [g_ty], ax
    call piece_collide
    test ax, ax
    jnz .no
    mov ax, [g_tx]
    mov [g_px], ax
.no:
    ret

ds_right:
    mov ax, [g_px]
    inc ax
    mov [g_tx], ax
    mov ax, [g_py]
    mov [g_ty], ax
    call piece_collide
    test ax, ax
    jnz .no
    mov ax, [g_tx]
    mov [g_px], ax
.no:
    ret

ds_softdrop:
    mov ax, [g_px]
    mov [g_tx], ax
    mov ax, [g_py]
    inc ax
    mov [g_ty], ax
    call piece_collide
    test ax, ax
    jnz .no
    inc word [g_py]
.no:
    ret

ds_rotate:
    mov ax, [g_rot]
    mov [g_srot], ax
    inc ax
    and ax, 3
    mov [g_rot], ax
    mov ax, [g_px]
    mov [g_tx], ax
    mov ax, [g_py]
    mov [g_ty], ax
    call piece_collide
    test ax, ax
    jz .ok
    mov ax, [g_srot]
    mov [g_rot], ax
.ok:
    ret

rng:
    mov ax, [g_seed]
    mov bx, ax
    shl ax, 2
    add ax, bx               ; *5
    inc ax
    mov [g_seed], ax
    and ax, 0xFF
    ret

dostris_spawn:
    call rng
.mod:
    cmp ax, 7
    jb .ok7
    sub ax, 7
    jmp .mod
.ok7:
    mov [g_type], ax
    xor ax, ax
    mov [g_rot], ax
    mov [g_fall], ax
    mov word [g_px], 3
    mov word [g_tx], 3
    mov word [g_py], 0
    mov word [g_ty], 0
    call piece_collide
    test ax, ax
    jz .ok
    mov word [g_state], 1
.ok:
    mov ax, [g_px]
    mov [g_oldpx], ax
    mov ax, [g_py]
    mov [g_oldpy], ax
    mov ax, [g_rot]
    mov [g_oldrot], ax
    ret

dostris_init:
    mov di, g_board
    mov cx, BW*BHT
    xor al, al
    rep stosb
    xor ax, ax
    mov [g_lines], ax
    mov [g_state], ax
    mov [g_fall], ax
    in al, LINE_CUR          ; seed from a changing source
    xor ah, ah
    or ax, 1
    mov [g_seed], ax
    call dostris_spawn
    ret

dostris_lock:
    mov bx, [g_type]
    mov al, [cs:piece_pal+bx]
    xor ah, ah
    mov [g_lt], ax
    mov ax, [g_px]
    mov [g_tx], ax
    mov ax, [g_py]
    mov [g_ty], ax
    call get_mask
    mov si, [mlo]
    xor bp, bp
.loop:
    test si, 0x8000
    jz .next
    mov ax, bp
    and ax, 3
    add ax, [g_tx]
    mov di, ax               ; cell x
    mov ax, bp
    and ax, 12
    shr ax, 2
    add ax, [g_ty]
    mov bx, BW
    mul bx
    add ax, di
    mov bx, ax
    mov al, [g_lt]
    mov [g_board+bx], al
.next:
    shl si, 1
    inc bp
    cmp bp, 16
    jne .loop
    ret

dostris_clearlines:
    mov bp, BHT-1
.loop:
    cmp bp, 0
    jl .done
    mov [g_row], bp
    call row_full
    test ax, ax
    jz .dec
    call collapse_row
    inc word [g_lines]
    jmp .loop                ; recheck same row
.dec:
    dec bp
    jmp .loop
.done:
    ret

; row_full: g_row -> AX=1 if full (preserves BP)
row_full:
    mov ax, [g_row]
    mov bx, BW
    mul bx
    mov si, ax
    add si, g_board
    mov cx, BW
.l:
    mov al, [si]
    test al, al
    jz .no
    inc si
    loop .l
    mov ax, 1
    ret
.no:
    xor ax, ax
    ret

; collapse_row: drop everything above g_row down one, clear row 0 (preserves BP)
collapse_row:
    mov bx, [g_row]
.loop:
    cmp bx, 0
    jle .top
    mov ax, bx
    mov dx, BW
    mul dx                   ; AX = r*BW
    mov di, ax
    sub ax, BW
    mov si, ax
    add di, g_board
    add si, g_board
    mov cx, BW
.copy:
    mov al, [si]
    mov [di], al
    inc si
    inc di
    loop .copy
    dec bx
    jmp .loop
.top:
    mov di, g_board
    mov cx, BW
    xor al, al
    rep stosb
    ret

; dostris_update: one frame of game logic
dostris_update:
    mov ax, [g_state]
    test ax, ax
    jnz .ret
    mov ax, [g_px]
    mov [g_oldpx], ax
    mov ax, [g_py]
    mov [g_oldpy], ax
    mov ax, [g_rot]
    mov [g_oldrot], ax
    mov ax, [v_pade]
    test ax, PAD_L
    jz .nl
    call ds_left
.nl:
    mov ax, [v_pade]
    test ax, PAD_R
    jz .nr
    call ds_right
.nr:
    mov ax, [v_pade]
    test ax, PAD_U
    jz .nu
    call ds_rotate
.nu:
    mov ax, [v_pade]
    test ax, PAD_A
    jz .na
    call ds_rotate
.na:
    mov ax, [v_pad]
    test ax, PAD_D
    jz .nd
    call ds_softdrop
.nd:
    mov ax, [a_gpause]
    test ax, ax
    jnz .check
    inc word [g_fall]
    mov ax, [g_fall]
    cmp ax, FALLRATE
    jb .check
    mov word [g_fall], 0
    mov ax, [g_px]
    mov [g_tx], ax
    mov ax, [g_py]
    inc ax
    mov [g_ty], ax
    call piece_collide
    test ax, ax
    jz .fall
    call dostris_lock
    call dostris_clearlines
    call dostris_spawn
    mov word [v_dirty], 1
    mov word [pf_pc], 0
    ret
.fall:
    inc word [g_py]
.check:
    mov ax, [g_px]
    cmp ax, [g_oldpx]
    jne .moved
    mov ax, [g_py]
    cmp ax, [g_oldpy]
    jne .moved
    mov ax, [g_rot]
    cmp ax, [g_oldrot]
    jne .moved
.ret:
    ret
.moved:
    mov word [pf_pc], 1
    ret

; ============================================================================
; rendering
; ============================================================================
; dblk: CL=col, CH=row, DL=pal (0=empty) -> write a board cell
dblk:
    test dl, dl
    jz .e
    mov al, TILE_BLOCK
    mov ah, dl
    shl ah, 1
    call puttile
    ret
.e:
    xor ax, ax
    call puttile
    ret

; app_dostris: draw the whole board over the chrome
app_dostris:
    call dostris_draw
    ret

dostris_draw:
    ; side walls (palette 7 = black block -> the well frame)
    xor bp, bp               ; by
.walls:
    cmp bp, BHT
    jge .floor
    mov cl, BORG_COL-1
    mov ax, bp
    add ax, BORG_ROW
    mov ch, al
    mov dl, 7
    call dblk
    mov cl, BORG_COL+BW
    mov ax, bp
    add ax, BORG_ROW
    mov ch, al
    mov dl, 7
    call dblk
    inc bp
    jmp .walls
.floor:
    mov bp, BORG_COL-1
.fl:
    cmp bp, BORG_COL+BW
    jg .cells
    mov ax, bp
    mov cl, al
    mov ch, BORG_ROW+BHT
    mov dl, 7
    call dblk
    inc bp
    jmp .fl
.cells:
    xor bp, bp               ; by
.row:
    cmp bp, BHT
    jge .piece
    xor si, si               ; bx
.col:
    cmp si, BW
    jge .rowend
    mov ax, bp
    mov bx, BW
    mul bx
    add ax, si
    mov bx, ax
    mov al, [g_board+bx]
    mov dl, al
    mov ax, si
    add ax, BORG_COL
    mov cl, al
    mov ax, bp
    add ax, BORG_ROW
    mov ch, al
    call dblk
    inc si
    jmp .col
.rowend:
    inc bp
    jmp .row
.piece:
    mov ax, [g_state]
    test ax, ax
    jnz .score
    mov ax, [g_px]
    mov [g_tx], ax
    mov ax, [g_py]
    mov [g_ty], ax
    mov bx, [g_type]
    mov al, [cs:piece_pal+bx]
    xor ah, ah
    mov [g_pt], ax
    call draw_piece_cells
.score:
    call dostris_draw_score
    mov ax, [g_state]
    test ax, ax
    jz .done
    mov cl, BORG_COL
    mov ch, BORG_ROW+5
    mov dl, 1
    mov si, s_over
    call putstr
.done:
    ret

dostris_draw_score:
    mov cl, BORG_COL+BW+2
    mov ch, BORG_ROW
    mov dl, 0
    mov si, s_lines
    call putstr
    mov cl, BORG_COL+BW+2
    mov ch, BORG_ROW+1
    mov ax, [g_lines]
    call put2
    ret

; draw_piece_cells: plot g_pt at each set cell of mask(g_type,g_rot) at g_tx,g_ty
draw_piece_cells:
    call get_mask
    mov si, [mlo]
    xor bp, bp
.loop:
    test si, 0x8000
    jz .next
    mov ax, bp
    and ax, 3
    add ax, [g_tx]
    add ax, BORG_COL
    mov cl, al
    mov ax, bp
    and ax, 12
    shr ax, 2
    add ax, [g_ty]
    add ax, BORG_ROW
    mov ch, al
    mov ax, [g_pt]
    mov dl, al
    call dblk
.next:
    shl si, 1
    inc bp
    cmp bp, 16
    jne .loop
    ret

; draw_piece_partial: erase old pose (pal0) then draw new pose
draw_piece_partial:
    mov ax, [g_oldpx]
    mov [g_tx], ax
    mov ax, [g_oldpy]
    mov [g_ty], ax
    mov ax, [g_rot]
    push ax                  ; save current rot
    mov ax, [g_oldrot]
    mov [g_rot], ax
    mov word [g_pt], 0
    call draw_piece_cells
    pop ax
    mov [g_rot], ax
    mov ax, [g_px]
    mov [g_tx], ax
    mov ax, [g_py]
    mov [g_ty], ax
    mov bx, [g_type]
    mov al, [cs:piece_pal+bx]
    xor ah, ah
    mov [g_pt], ax
    call draw_piece_cells
    mov ax, [g_px]
    mov [g_oldpx], ax
    mov ax, [g_py]
    mov [g_oldpy], ax
    mov ax, [g_rot]
    mov [g_oldrot], ax
    ret

s_lines    db "Ln",0
s_over     db "OVER",0
