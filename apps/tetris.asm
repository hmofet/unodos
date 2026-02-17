; ============================================================================
; TETRIS.BIN - Tetris clone for UnoDOS
; Fullscreen (non-windowed) game with Korobeiniki music and toolkit buttons
; ============================================================================

[ORG 0x0000]

; --- BIN Header (80 bytes) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic
    db 'Tetris', 0                  ; App name (12 bytes padded)
    times (0x04 + 12) - ($ - $$) db 0

; 16x16 icon bitmap (64 bytes, 2bpp CGA)
; T-tetromino shape with scattered blocks
    db 0x00, 0x00, 0x00, 0x00      ; Row 0:  ................
    db 0x00, 0x00, 0x00, 0x00      ; Row 1:  ................
    db 0x3F, 0xFF, 0xFF, 0xC0      ; Row 2:  ..XXXXXXXXXXXX..
    db 0x3F, 0xFF, 0xFF, 0xC0      ; Row 3:  ..XXXXXXXXXXXX..
    db 0x00, 0x3F, 0xC0, 0x00      ; Row 4:  ......XXXX......
    db 0x00, 0x3F, 0xC0, 0x00      ; Row 5:  ......XXXX......
    db 0x00, 0x3F, 0xC0, 0x00      ; Row 6:  ......XXXX......
    db 0x00, 0x3F, 0xC0, 0x00      ; Row 7:  ......XXXX......
    db 0x00, 0x00, 0x00, 0x00      ; Row 8:  ................
    db 0x05, 0x50, 0x55, 0x00      ; Row 9:  ..C.C...C.C.....  (cyan=01)
    db 0x05, 0x50, 0x55, 0x00      ; Row 10: ..C.C...C.C.....
    db 0x00, 0x00, 0x00, 0x00      ; Row 11: ................
    db 0x00, 0xA0, 0x0A, 0x80      ; Row 12: ....M.....M.M...  (magenta=10)
    db 0x00, 0xA0, 0x0A, 0x80      ; Row 13: ....M.....M.M...
    db 0x00, 0x00, 0x00, 0x00      ; Row 14: ................
    db 0x00, 0x00, 0x00, 0x00      ; Row 15: ................

    times 0x50 - ($ - $$) db 0     ; Pad to offset 0x50

; ============================================================================
; Entry point
; ============================================================================
entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Initialize RNG seed from BIOS tick
    call read_tick
    mov [cs:rng_seed], ax

    ; Clear screen
    call clear_screen

    ; Draw static UI elements
    call draw_static_ui

    ; Set game state to menu (waiting for New Game)
    mov byte [cs:game_state], STATE_MENU

; ============================================================================
; Main loop
; ============================================================================
.main_loop:
    cmp byte [cs:quit_flag], 1
    je .exit_game

    sti

    ; --- Poll keyboard directly (bypasses per-task event queue) ---
    mov ah, API_KBD_GETCHAR
    int 0x80
    test al, al
    jz .no_key_event

    ; AL = keycode (arrows: 128-131)
    cmp byte [cs:game_state], STATE_PLAYING
    jne .check_pause_key

    ; Game keys (only when playing)
    cmp al, 130                     ; Left arrow
    je .move_left
    cmp al, 131                     ; Right arrow
    je .move_right
    cmp al, 128                     ; Up arrow = rotate
    je .rotate
    cmp al, 129                     ; Down arrow = soft drop
    je .soft_drop
    cmp al, ' '                     ; Space = hard drop
    je .hard_drop
    cmp al, 'p'
    je .pause_game
    cmp al, 'P'
    je .pause_game
    jmp .no_key_event

.check_pause_key:
    cmp byte [cs:game_state], STATE_PAUSED
    jne .no_key_event
    cmp al, 'p'
    je .unpause_game
    cmp al, 'P'
    je .unpause_game
    jmp .no_key_event

.move_left:
    call try_move_left
    jmp .no_key_event
.move_right:
    call try_move_right
    jmp .no_key_event
.rotate:
    call try_rotate
    jmp .no_key_event
.soft_drop:
    call try_soft_drop
    jmp .no_key_event
.hard_drop:
    call do_hard_drop
    jmp .no_key_event
.pause_game:
    mov byte [cs:game_state], STATE_PAUSED
    ; Turn off music
    mov ah, API_SPEAKER_OFF
    int 0x80
    ; Show paused text
    mov bx, 56
    mov cx, 88
    mov si, str_paused
    mov ah, API_GFX_DRAW_STRING_INV
    int 0x80
    jmp .no_key_event
.unpause_game:
    mov byte [cs:game_state], STATE_PLAYING
    ; Clear paused text
    mov bx, 56
    mov cx, 88
    mov dx, 48
    mov si, 12
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    ; Restart music timing
    call read_tick
    mov [cs:music_tick], ax
    mov [cs:last_drop_tick], ax
    jmp .no_key_event
.no_key_event:
    ; --- Handle mouse clicks ---
    call check_mouse

    ; --- Game logic (only when playing) ---
    cmp byte [cs:game_state], STATE_PLAYING
    jne .main_loop

    call music_update
    call check_drop_timer

    jmp .main_loop

.exit_game:
    ; Turn off speaker
    mov ah, API_SPEAKER_OFF
    int 0x80

    ; Clear screen
    call clear_screen

    pop es
    pop ds
    popa
    retf

; ============================================================================
; Constants
; ============================================================================
API_GFX_DRAW_PIXEL      equ 0
API_GFX_DRAW_RECT       equ 1
API_GFX_DRAW_FILLED_RECT equ 2
API_GFX_DRAW_CHAR        equ 3
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_GFX_DRAW_STRING_INV equ 6
API_KBD_GETCHAR          equ 11
API_MOUSE_GET_STATE      equ 28
API_GFX_SET_FONT         equ 48
API_DRAW_BUTTON          equ 51
API_HIT_TEST             equ 53
API_THEME_SET_COLORS     equ 54
API_SPEAKER_TONE         equ 41
API_SPEAKER_OFF          equ 42

STATE_MENU               equ 0
STATE_PLAYING            equ 1
STATE_PAUSED             equ 2
STATE_GAMEOVER           equ 3

BOARD_X                  equ 40
BOARD_Y                  equ 16
BOARD_COLS               equ 10
BOARD_ROWS               equ 20
CELL_SIZE                equ 8

BTN_NEWGAME_X            equ 168
BTN_NEWGAME_Y            equ 124
BTN_NEWGAME_W            equ 58
BTN_PAUSE_X              equ 230
BTN_PAUSE_Y              equ 124
BTN_PAUSE_W              equ 40
BTN_QUIT_X               equ 274
BTN_QUIT_Y               equ 124
BTN_QUIT_W               equ 36
BTN_H                    equ 14

PREVIEW_X                equ 170
PREVIEW_Y                equ 82

; ============================================================================
; Game variables
; ============================================================================
game_state:     db 0
quit_flag:      db 0
cur_piece:      db 0                ; Piece type 0-6
cur_rot:        db 0                ; Rotation 0-3
cur_x:          db 3                ; Board column (signed)
cur_y:          db 0                ; Board row (signed)
cur_color:      db 0                ; Color 1-3
next_piece:     db 0
score_lo:       dw 0                ; Low word of score
score_hi:       dw 0                ; High word (for display)
lines:          dw 0
level:          db 1
drop_speed:     dw 18               ; Ticks per auto-drop
last_drop_tick: dw 0
rng_seed:       dw 0
prev_mouse_btn: db 0

; Music state
music_idx:      dw 0
music_tick:     dw 0
music_dur:      dw 0
music_gap:      db 0                ; 0=playing note, 1=in gap

; Temp variables for draw_cell
cell_sx:        dw 0
cell_sy:        dw 0

; Temp for collision check
chk_type:       db 0
chk_rot:        db 0
chk_x:          db 0
chk_y:          db 0

; Line clear temp
clear_count:    db 0
clear_rows:     times 4 db 0        ; Up to 4 rows cleared at once

; Number display buffer
num_buf:        times 6 db 0        ; "00000\0"

; Board: 10 cols x 20 rows, 1 byte per cell (0=empty, 1-3=color)
board:          times 200 db 0

; ============================================================================
; Piece data: 7 pieces x 4 rotations x 4 cells x 2 bytes (col,row)
; Lookup: piece_data + type*32 + rot*8
; ============================================================================
piece_data:
    ; Piece 0: I-piece (cyan)
    db 0,1, 1,1, 2,1, 3,1          ; Rot 0: horizontal
    db 2,0, 2,1, 2,2, 2,3          ; Rot 1: vertical
    db 0,2, 1,2, 2,2, 3,2          ; Rot 2: horizontal (lower)
    db 1,0, 1,1, 1,2, 1,3          ; Rot 3: vertical (left)

    ; Piece 1: O-piece (white)
    db 1,0, 2,0, 1,1, 2,1          ; All rotations same
    db 1,0, 2,0, 1,1, 2,1
    db 1,0, 2,0, 1,1, 2,1
    db 1,0, 2,0, 1,1, 2,1

    ; Piece 2: T-piece (magenta)
    db 1,0, 0,1, 1,1, 2,1          ; Rot 0: T pointing down
    db 0,0, 0,1, 1,1, 0,2          ; Rot 1: T pointing right
    db 0,0, 1,0, 2,0, 1,1          ; Rot 2: T pointing up
    db 1,0, 0,1, 1,1, 1,2          ; Rot 3: T pointing left

    ; Piece 3: S-piece (cyan)
    db 1,0, 2,0, 0,1, 1,1          ; Rot 0
    db 0,0, 0,1, 1,1, 1,2          ; Rot 1
    db 1,0, 2,0, 0,1, 1,1          ; Rot 2
    db 0,0, 0,1, 1,1, 1,2          ; Rot 3

    ; Piece 4: Z-piece (magenta)
    db 0,0, 1,0, 1,1, 2,1          ; Rot 0
    db 1,0, 0,1, 1,1, 0,2          ; Rot 1
    db 0,0, 1,0, 1,1, 2,1          ; Rot 2
    db 1,0, 0,1, 1,1, 0,2          ; Rot 3

    ; Piece 5: J-piece (white)
    db 0,0, 0,1, 1,1, 2,1          ; Rot 0
    db 0,0, 1,0, 0,1, 0,2          ; Rot 1
    db 0,0, 1,0, 2,0, 2,1          ; Rot 2
    db 1,0, 1,1, 0,2, 1,2          ; Rot 3

    ; Piece 6: L-piece (cyan)
    db 2,0, 0,1, 1,1, 2,1          ; Rot 0
    db 0,0, 0,1, 0,2, 1,2          ; Rot 1
    db 0,0, 1,0, 2,0, 0,1          ; Rot 2
    db 0,0, 1,0, 1,1, 1,2          ; Rot 3

; Color per piece type (0-6)
piece_colors:
    db 1, 3, 2, 1, 2, 3, 1         ; I=cyan, O=white, T=mag, S=cyan, Z=mag, J=white, L=cyan

; ============================================================================
; Korobeiniki melody (Tetris Theme A)
; Format: dw frequency, duration_ticks (0=rest, 0xFFFF=end)
; ============================================================================
NOTE_E5  equ 659
NOTE_B4  equ 494
NOTE_C5  equ 523
NOTE_D5  equ 587
NOTE_A4  equ 440
NOTE_GS4 equ 415
NOTE_F5  equ 698
NOTE_G5  equ 784
NOTE_A5  equ 880

BEAT_E   equ 3                      ; Eighth note
BEAT_Q   equ 5                      ; Quarter note
BEAT_DQ  equ 8                      ; Dotted quarter
BEAT_H   equ 10                     ; Half note

korobeiniki:
    ; Phrase 1: E B C D | C B A A | C E D C | B . C D | E C A A
    dw NOTE_E5, BEAT_Q
    dw NOTE_B4, BEAT_E
    dw NOTE_C5, BEAT_E
    dw NOTE_D5, BEAT_Q
    dw NOTE_C5, BEAT_E
    dw NOTE_B4, BEAT_E
    dw NOTE_A4, BEAT_Q
    dw NOTE_A4, BEAT_E
    dw NOTE_C5, BEAT_E
    dw NOTE_E5, BEAT_Q
    dw NOTE_D5, BEAT_E
    dw NOTE_C5, BEAT_E
    dw NOTE_B4, BEAT_DQ
    dw NOTE_C5, BEAT_E
    dw NOTE_D5, BEAT_Q
    dw NOTE_E5, BEAT_Q
    dw NOTE_C5, BEAT_Q
    dw NOTE_A4, BEAT_Q
    dw NOTE_A4, BEAT_H
    dw 0, BEAT_E                    ; Rest
    ; Phrase 2: D . F A | G F E . | C E D C | B B C D | E C A A
    dw NOTE_D5, BEAT_DQ
    dw NOTE_F5, BEAT_E
    dw NOTE_A5, BEAT_Q
    dw NOTE_G5, BEAT_E
    dw NOTE_F5, BEAT_E
    dw NOTE_E5, BEAT_DQ
    dw NOTE_C5, BEAT_E
    dw NOTE_E5, BEAT_Q
    dw NOTE_D5, BEAT_E
    dw NOTE_C5, BEAT_E
    dw NOTE_B4, BEAT_Q
    dw NOTE_B4, BEAT_E
    dw NOTE_C5, BEAT_E
    dw NOTE_D5, BEAT_Q
    dw NOTE_E5, BEAT_Q
    dw NOTE_C5, BEAT_Q
    dw NOTE_A4, BEAT_Q
    dw NOTE_A4, BEAT_H
    dw 0, BEAT_E                    ; Rest
    ; End marker
    dw 0xFFFF, 0

MELODY_NOTES equ ($ - korobeiniki) / 4 - 1  ; Exclude end marker

; ============================================================================
; Strings
; ============================================================================
str_score:      db 'Score:', 0
str_lines:      db 'Lines:', 0
str_level:      db 'Level:', 0
str_next:       db 'Next:', 0
str_newgame:    db 'New Game', 0
str_pause:      db 'Pause', 0
str_quit:       db 'Quit', 0
str_help1:      db 'Arrows:Move', 0
str_help2:      db 'Up:Rot Space:Drop', 0
str_gameover:   db 'GAME OVER', 0
str_paused:     db 'PAUSED', 0

; Title character data for colored rendering
title_chars:    db 'TETRIS'
title_clrs:     db 1, 2, 3, 1, 2, 3    ; cyan, magenta, white per letter

; ============================================================================
; clear_screen - Fill entire screen with black
; ============================================================================
clear_screen:
    push ax
    push bx
    push cx
    push dx
    push si
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 200
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; draw_static_ui - Draw border, labels, buttons, help text
; ============================================================================
draw_static_ui:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es

    ; Set medium font (8x8)
    mov al, 1
    mov ah, API_GFX_SET_FONT
    int 0x80

    ; Board border (white outline)
    mov bx, BOARD_X - 1
    mov cx, BOARD_Y - 1
    mov dx, BOARD_COLS * CELL_SIZE + 2
    mov si, BOARD_ROWS * CELL_SIZE + 2
    mov ah, API_GFX_DRAW_RECT
    int 0x80

    ; Title "TETRIS" in alternating colors (large font)
    mov al, 2                       ; Large font 8x12
    mov ah, API_GFX_SET_FONT
    int 0x80

    mov byte [cs:cell_px], 0        ; Letter index
    mov word [cs:cell_sx], 184      ; X position
.title_color_loop:
    xor bx, bx
    mov bl, [cs:cell_px]
    mov al, [cs:title_clrs + bx]    ; Color for this letter
    xor bl, bl                      ; Keep desktop_bg black
    mov cl, 3                       ; Keep win_color white
    mov ah, API_THEME_SET_COLORS
    int 0x80
    xor bx, bx
    mov bl, [cs:cell_px]
    mov al, [cs:title_chars + bx]   ; Character
    mov bx, [cs:cell_sx]            ; X position
    mov cx, 2                       ; Y position
    mov ah, API_GFX_DRAW_CHAR
    int 0x80
    add word [cs:cell_sx], 12       ; Large font advance
    inc byte [cs:cell_px]
    cmp byte [cs:cell_px], 6
    jb .title_color_loop

    ; Restore white text color
    mov al, 3
    xor bl, bl
    mov cl, 3
    mov ah, API_THEME_SET_COLORS
    int 0x80

    ; Switch back to medium font for labels
    mov al, 1
    mov ah, API_GFX_SET_FONT
    int 0x80

    ; Score label
    mov bx, 168
    mov cx, 26
    mov si, str_score
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Lines label
    mov bx, 168
    mov cx, 38
    mov si, str_lines
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Level label
    mov bx, 168
    mov cx, 50
    mov si, str_level
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; "Next:" label
    mov bx, 168
    mov cx, 68
    mov si, str_next
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Next piece preview border
    mov bx, PREVIEW_X - 3
    mov cx, PREVIEW_Y - 3
    mov dx, 38
    mov si, 38
    mov ah, API_GFX_DRAW_RECT
    int 0x80

    ; Set small font for compact button labels
    mov al, 0
    mov ah, API_GFX_SET_FONT
    int 0x80

    ; Buttons
    mov ax, cs
    mov es, ax

    ; New Game button
    mov bx, BTN_NEWGAME_X
    mov cx, BTN_NEWGAME_Y
    mov dx, BTN_NEWGAME_W
    mov si, BTN_H
    mov di, str_newgame
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; Pause button
    mov bx, BTN_PAUSE_X
    mov cx, BTN_PAUSE_Y
    mov dx, BTN_PAUSE_W
    mov si, BTN_H
    mov di, str_pause
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; Quit button
    mov bx, BTN_QUIT_X
    mov cx, BTN_QUIT_Y
    mov dx, BTN_QUIT_W
    mov si, BTN_H
    mov di, str_quit
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; Help text (already small font)
    mov bx, 168
    mov cx, 148
    mov si, str_help1
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 168
    mov cx, 156
    mov si, str_help2
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Restore medium font
    mov al, 1
    mov ah, API_GFX_SET_FONT
    int 0x80

    ; Display initial score values
    call update_score_display

    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; draw_cell - Draw one 8x8 cell on the board with 3D effect
; Input: AL=color (0-3), BL=col (0-9), BH=row (0-19)
; ============================================================================
draw_cell:
    pusha

    ; If color is 0, just clear the cell
    test al, al
    jz .clear_cell

    mov [cs:cell_color], al

    ; Calculate screen X = BOARD_X + col * 8
    xor ah, ah
    mov al, bl
    shl ax, 3
    add ax, BOARD_X
    mov [cs:cell_sx], ax

    ; Calculate screen Y = BOARD_Y + row * 8
    xor ah, ah
    mov al, bh
    shl ax, 3
    add ax, BOARD_Y
    mov [cs:cell_sy], ax

    ; Draw 3D block: 1px white highlight on top and left, fill rest with color
    ; Top edge (8 pixels wide, white)
    mov byte [cs:cell_py], 0
    mov byte [cs:cell_px], 0
.top_edge:
    mov bx, [cs:cell_sx]
    xor ah, ah
    mov al, [cs:cell_px]
    add bx, ax
    mov cx, [cs:cell_sy]
    mov al, 3                       ; White highlight
    mov ah, API_GFX_DRAW_PIXEL
    int 0x80
    inc byte [cs:cell_px]
    cmp byte [cs:cell_px], CELL_SIZE
    jb .top_edge

    ; Left edge (7 pixels tall, white) - skip row 0 (already drawn)
    mov byte [cs:cell_py], 1
.left_edge:
    mov bx, [cs:cell_sx]
    mov cx, [cs:cell_sy]
    xor ah, ah
    mov al, [cs:cell_py]
    add cx, ax
    mov al, 3                       ; White highlight
    mov ah, API_GFX_DRAW_PIXEL
    int 0x80
    inc byte [cs:cell_py]
    cmp byte [cs:cell_py], CELL_SIZE
    jb .left_edge

    ; Fill inner area (7x7 from (1,1) to (7,7)) with piece color
    mov byte [cs:cell_py], 1
.fill_row:
    mov byte [cs:cell_px], 1
.fill_col:
    mov bx, [cs:cell_sx]
    xor ah, ah
    mov al, [cs:cell_px]
    add bx, ax
    mov cx, [cs:cell_sy]
    xor ah, ah
    mov al, [cs:cell_py]
    add cx, ax
    mov al, [cs:cell_color]
    mov ah, API_GFX_DRAW_PIXEL
    int 0x80
    inc byte [cs:cell_px]
    cmp byte [cs:cell_px], CELL_SIZE
    jb .fill_col
    inc byte [cs:cell_py]
    cmp byte [cs:cell_py], CELL_SIZE
    jb .fill_row

    popa
    ret

.clear_cell:
    ; Use clear_area API for fast erase
    mov ch, bh                      ; Save row before BX is overwritten

    xor ah, ah
    mov al, bl
    shl ax, 3
    add ax, BOARD_X
    mov bx, ax                      ; BX = screen X

    xor ah, ah
    mov al, ch                      ; Restore saved row
    shl ax, 3
    add ax, BOARD_Y
    mov cx, ax                      ; CX = screen Y

    mov dx, CELL_SIZE
    mov si, CELL_SIZE
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    popa
    ret

; Temp for draw_cell inner loop
cell_color: db 0
cell_px:    db 0
cell_py:    db 0

; ============================================================================
; get_piece_cells - Get the 4 cell offsets for a piece
; Input: AL=type, AH=rotation
; Output: SI points to 8 bytes (col0,row0, col1,row1, ...)
; ============================================================================
get_piece_cells:
    push ax
    push bx

    ; SI = piece_data + type*32 + rot*8
    xor bx, bx
    mov bl, al
    shl bx, 5                       ; type * 32
    add bx, piece_data

    mov al, ah
    xor ah, ah                      ; Clear AH before shift
    shl ax, 3                       ; rot * 8
    add bx, ax
    mov si, bx

    pop bx
    pop ax
    ret

; ============================================================================
; draw_piece - Draw current piece on board
; ============================================================================
draw_piece:
    pusha

    mov al, [cs:cur_piece]
    mov ah, [cs:cur_rot]
    call get_piece_cells
    ; SI points to 4 (col,row) pairs

    mov cx, 4                       ; 4 cells
.dp_loop:
    push cx
    mov al, [cs:si]                 ; col offset
    add al, [cs:cur_x]
    mov bl, al                      ; BL = board col

    mov al, [cs:si + 1]            ; row offset
    add al, [cs:cur_y]
    mov bh, al                      ; BH = board row

    ; Skip if above board (row < 0)
    test bh, 0x80
    jnz .dp_skip

    mov al, [cs:cur_color]
    call draw_cell

.dp_skip:
    add si, 2
    pop cx
    dec cx
    jnz .dp_loop

    popa
    ret

; ============================================================================
; erase_piece - Erase current piece from board
; ============================================================================
erase_piece:
    pusha

    mov al, [cs:cur_piece]
    mov ah, [cs:cur_rot]
    call get_piece_cells

    mov cx, 4
.ep_loop:
    push cx
    mov al, [cs:si]
    add al, [cs:cur_x]
    mov bl, al

    mov al, [cs:si + 1]
    add al, [cs:cur_y]
    mov bh, al

    test bh, 0x80
    jnz .ep_skip

    xor al, al                      ; Color 0 = erase
    call draw_cell

.ep_skip:
    add si, 2
    pop cx
    dec cx
    jnz .ep_loop

    popa
    ret

; ============================================================================
; check_collision - Check if piece can be placed
; Input: chk_type, chk_rot, chk_x, chk_y (set before calling)
; Output: CF=1 if collision, CF=0 if OK
; ============================================================================
check_collision:
    pusha

    mov al, [cs:chk_type]
    mov ah, [cs:chk_rot]
    call get_piece_cells

    mov cx, 4
.cc_loop:
    push cx

    ; Compute absolute col
    mov al, [cs:si]
    add al, [cs:chk_x]
    mov dl, al                      ; DL = col (signed)

    ; Compute absolute row
    mov al, [cs:si + 1]
    add al, [cs:chk_y]
    mov dh, al                      ; DH = row (signed)

    ; Check wall: col < 0
    test dl, 0x80
    jnz .cc_hit

    ; Check wall: col >= 10
    cmp dl, BOARD_COLS
    jge .cc_hit

    ; Check floor: row >= 20
    cmp dh, BOARD_ROWS
    jge .cc_hit

    ; Row < 0 is OK (above board)
    test dh, 0x80
    jnz .cc_ok_cell

    ; Check board occupancy: board[row*10 + col]
    mov cl, dl                       ; Save column (mul clobbers DX)
    xor ax, ax
    mov al, dh
    mov bx, BOARD_COLS
    mul bx                           ; AX = row * 10 (DX clobbered)
    xor bx, bx
    mov bl, cl                       ; Restore column from CL
    add ax, bx                       ; AX = row*10 + col
    mov bx, ax
    cmp byte [cs:board + bx], 0
    jne .cc_hit

.cc_ok_cell:
    add si, 2
    pop cx
    dec cx
    jnz .cc_loop

    ; No collision
    popa
    clc
    ret

.cc_hit:
    pop cx                           ; Balance push cx
    popa
    stc
    ret

; ============================================================================
; try_move_left / try_move_right
; ============================================================================
try_move_left:
    push ax
    mov al, [cs:cur_piece]
    mov [cs:chk_type], al
    mov al, [cs:cur_rot]
    mov [cs:chk_rot], al
    mov al, [cs:cur_x]
    dec al
    mov [cs:chk_x], al
    mov al, [cs:cur_y]
    mov [cs:chk_y], al
    call check_collision
    jc .tml_blocked
    call erase_piece
    dec byte [cs:cur_x]
    call draw_piece
.tml_blocked:
    pop ax
    ret

try_move_right:
    push ax
    mov al, [cs:cur_piece]
    mov [cs:chk_type], al
    mov al, [cs:cur_rot]
    mov [cs:chk_rot], al
    mov al, [cs:cur_x]
    inc al
    mov [cs:chk_x], al
    mov al, [cs:cur_y]
    mov [cs:chk_y], al
    call check_collision
    jc .tmr_blocked
    call erase_piece
    inc byte [cs:cur_x]
    call draw_piece
.tmr_blocked:
    pop ax
    ret

; ============================================================================
; try_rotate
; ============================================================================
try_rotate:
    push ax
    mov al, [cs:cur_piece]
    mov [cs:chk_type], al
    mov al, [cs:cur_rot]
    inc al
    and al, 3                        ; mod 4
    mov [cs:chk_rot], al
    mov al, [cs:cur_x]
    mov [cs:chk_x], al
    mov al, [cs:cur_y]
    mov [cs:chk_y], al
    call check_collision
    jc .tr_blocked
    call erase_piece
    mov al, [cs:chk_rot]
    mov [cs:cur_rot], al
    call draw_piece
.tr_blocked:
    pop ax
    ret

; ============================================================================
; try_soft_drop - Drop piece one row, lock if blocked
; ============================================================================
try_soft_drop:
    push ax
    mov al, [cs:cur_piece]
    mov [cs:chk_type], al
    mov al, [cs:cur_rot]
    mov [cs:chk_rot], al
    mov al, [cs:cur_x]
    mov [cs:chk_x], al
    mov al, [cs:cur_y]
    inc al
    mov [cs:chk_y], al
    call check_collision
    jc .tsd_lock
    call erase_piece
    inc byte [cs:cur_y]
    call draw_piece
    ; Add 1 point for soft drop
    inc word [cs:score_lo]
    call update_score_display
    pop ax
    ret

.tsd_lock:
    call lock_piece
    pop ax
    ret

; ============================================================================
; do_hard_drop - Drop piece to bottom instantly
; ============================================================================
do_hard_drop:
    push ax
.hd_loop:
    mov al, [cs:cur_piece]
    mov [cs:chk_type], al
    mov al, [cs:cur_rot]
    mov [cs:chk_rot], al
    mov al, [cs:cur_x]
    mov [cs:chk_x], al
    mov al, [cs:cur_y]
    inc al
    mov [cs:chk_y], al
    call check_collision
    jc .hd_done
    call erase_piece
    inc byte [cs:cur_y]
    call draw_piece
    ; 2 points per row for hard drop
    add word [cs:score_lo], 2
    jmp .hd_loop
.hd_done:
    call update_score_display
    call lock_piece
    pop ax
    ret

; ============================================================================
; lock_piece - Write piece into board and check lines
; ============================================================================
lock_piece:
    pusha

    ; Write cells into board array
    mov al, [cs:cur_piece]
    mov ah, [cs:cur_rot]
    call get_piece_cells

    mov cx, 4
.lp_loop:
    push cx

    mov al, [cs:si]
    add al, [cs:cur_x]
    mov dl, al                      ; col

    mov al, [cs:si + 1]
    add al, [cs:cur_y]
    mov dh, al                      ; row

    ; Skip if above board
    test dh, 0x80
    jnz .lp_skip

    ; board[row*10 + col] = cur_color
    mov cl, dl                       ; Save column (mul clobbers DX)
    xor ax, ax
    mov al, dh
    mov bx, BOARD_COLS
    mul bx                           ; AX = row * 10 (DX clobbered)
    xor bx, bx
    mov bl, cl                       ; Restore column from CL
    add ax, bx
    mov bx, ax
    mov al, [cs:cur_color]
    mov [cs:board + bx], al

.lp_skip:
    add si, 2
    pop cx
    dec cx
    jnz .lp_loop

    ; Check for completed lines
    call check_lines

    ; Spawn next piece
    call spawn_piece

    ; Check if new piece immediately collides = game over
    mov al, [cs:cur_piece]
    mov [cs:chk_type], al
    mov al, [cs:cur_rot]
    mov [cs:chk_rot], al
    mov al, [cs:cur_x]
    mov [cs:chk_x], al
    mov al, [cs:cur_y]
    mov [cs:chk_y], al
    call check_collision
    jnc .lp_ok

    ; Game over
    mov byte [cs:game_state], STATE_GAMEOVER
    mov ah, API_SPEAKER_OFF
    int 0x80

    ; Draw game over text
    mov bx, 48
    mov cx, 84
    mov si, str_gameover
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.lp_ok:
    popa
    ret

; ============================================================================
; check_lines - Find and clear completed lines
; ============================================================================
check_lines:
    pusha

    mov byte [cs:clear_count], 0

    ; Scan from bottom row (19) to top (0)
    mov byte [cs:chk_y], BOARD_ROWS - 1

.cl_row_loop:
    ; Check if row is full
    xor ax, ax
    mov al, [cs:chk_y]
    mov bx, BOARD_COLS
    mul bx                          ; AX = row * 10
    mov bx, ax                      ; BX = row offset into board

    mov cx, BOARD_COLS
    mov si, 0                       ; column counter
.cl_col_check:
    cmp byte [cs:board + bx + si], 0
    je .cl_not_full
    inc si
    dec cx
    jnz .cl_col_check

    ; Row is full - record it
    push bx
    xor bx, bx
    mov bl, [cs:clear_count]
    mov al, [cs:chk_y]
    mov [cs:clear_rows + bx], al
    inc byte [cs:clear_count]
    pop bx

.cl_not_full:
    dec byte [cs:chk_y]
    cmp byte [cs:chk_y], 0xFF      ; Wrapped below 0
    jne .cl_row_loop

    ; If no lines cleared, done
    cmp byte [cs:clear_count], 0
    je .cl_done

    ; Flash animation
    call animate_line_clear

    ; Collapse rows (process from top to bottom of cleared rows)
    ; clear_rows is in bottom-to-top order, collapse bottom-first
    xor cx, cx
    mov cl, [cs:clear_count]
.cl_collapse_loop:
    push cx
    dec cl
    xor bx, bx
    mov bl, cl
    mov al, [cs:clear_rows + bx]
    call collapse_row
    pop cx
    dec cx
    jnz .cl_collapse_loop

    ; Redraw entire board after collapse
    call draw_board

    ; Update score
    call update_scoring

    ; Update lines count
    xor ax, ax
    mov al, [cs:clear_count]
    add [cs:lines], ax

    ; Check level up (every 10 lines)
    mov ax, [cs:lines]
    xor dx, dx
    mov bx, 10
    div bx                          ; AX = lines / 10
    inc al                          ; Level = lines/10 + 1
    cmp al, 15
    jbe .cl_level_ok
    mov al, 15                      ; Max level 15
.cl_level_ok:
    mov [cs:level], al

    ; Update drop speed: max(2, 18 - level)
    xor ax, ax
    mov al, 18
    xor bx, bx
    mov bl, [cs:level]
    sub ax, bx
    cmp ax, 2
    jge .cl_speed_ok
    mov ax, 2
.cl_speed_ok:
    mov [cs:drop_speed], ax

    call update_score_display

.cl_done:
    popa
    ret

; ============================================================================
; collapse_row - Remove row AL and shift everything above down
; Input: AL = row to remove
; ============================================================================
collapse_row:
    pusha

    ; Start from the cleared row, copy row above into current row
    xor cx, cx
    mov cl, al                      ; CL = current row

.cr_shift_loop:
    cmp cl, 0
    je .cr_clear_top

    ; Copy row (cl-1) into row (cl)
    ; dst = board + cl * 10
    xor ax, ax
    mov al, cl
    mov bx, BOARD_COLS
    mul bx
    mov di, ax                      ; DI = dest offset

    ; src = board + (cl-1) * 10
    xor ax, ax
    mov al, cl
    dec al
    mov bx, BOARD_COLS
    mul bx
    mov si, ax                      ; SI = src offset

    ; Copy 10 bytes
    mov bx, 0
.cr_copy:
    mov al, [cs:board + si + bx]
    mov [cs:board + di + bx], al
    inc bx
    cmp bx, BOARD_COLS
    jb .cr_copy

    dec cl
    jmp .cr_shift_loop

.cr_clear_top:
    ; Clear top row (row 0)
    mov bx, 0
.cr_zero:
    mov byte [cs:board + bx], 0
    inc bx
    cmp bx, BOARD_COLS
    jb .cr_zero

    popa
    ret

; ============================================================================
; animate_line_clear - Flash completed rows
; ============================================================================
animate_line_clear:
    pusha

    mov byte [cs:cell_px], 3        ; 3 flash cycles
.alc_flash:
    ; Draw all cleared rows as white
    xor cx, cx
    mov cl, [cs:clear_count]
    xor bx, bx
.alc_white:
    push cx
    push bx
    mov al, [cs:clear_rows + bx]
    mov bh, al                      ; row
    mov cx, BOARD_COLS
    xor bl, bl                      ; col = 0
.alc_wrow:
    push cx
    mov al, 3                       ; White
    call draw_cell
    inc bl
    pop cx
    dec cx
    jnz .alc_wrow
    pop bx
    pop cx
    inc bx
    dec cx
    jnz .alc_white

    ; Wait ~2 ticks
    call read_tick
    mov [cs:cell_sy], ax
.alc_w1:
    sti
    call read_tick
    sub ax, [cs:cell_sy]
    cmp ax, 2
    jb .alc_w1

    ; Draw all cleared rows as black
    xor cx, cx
    mov cl, [cs:clear_count]
    xor bx, bx
.alc_black:
    push cx
    push bx
    mov al, [cs:clear_rows + bx]
    mov bh, al
    mov cx, BOARD_COLS
    xor bl, bl
.alc_brow:
    push cx
    xor al, al                      ; Black
    call draw_cell
    inc bl
    pop cx
    dec cx
    jnz .alc_brow
    pop bx
    pop cx
    inc bx
    dec cx
    jnz .alc_black

    ; Wait ~2 ticks
    call read_tick
    mov [cs:cell_sy], ax
.alc_w2:
    sti
    call read_tick
    sub ax, [cs:cell_sy]
    cmp ax, 2
    jb .alc_w2

    dec byte [cs:cell_px]
    jnz .alc_flash

    popa
    ret

; ============================================================================
; draw_board - Redraw all board cells from board array
; ============================================================================
draw_board:
    pusha

    xor bh, bh                      ; row = 0
.db_row:
    xor bl, bl                      ; col = 0
.db_col:
    ; Get board value
    xor ax, ax
    mov al, bh
    push bx
    mov bx, BOARD_COLS
    mul bx
    pop bx
    xor cx, cx
    mov cl, bl
    add ax, cx
    push bx
    mov bx, ax
    mov al, [cs:board + bx]
    pop bx

    call draw_cell

    inc bl
    cmp bl, BOARD_COLS
    jb .db_col
    inc bh
    cmp bh, BOARD_ROWS
    jb .db_row

    popa
    ret

; ============================================================================
; update_scoring - Add points based on lines cleared
; ============================================================================
update_scoring:
    pusha

    ; Score table: 1=40, 2=100, 3=300, 4=1200 (multiplied by level+1)
    xor ax, ax
    mov al, [cs:clear_count]
    cmp al, 1
    je .us_1
    cmp al, 2
    je .us_2
    cmp al, 3
    je .us_3
    ; 4 lines (Tetris!)
    mov ax, 1200
    jmp .us_mult
.us_1:
    mov ax, 40
    jmp .us_mult
.us_2:
    mov ax, 100
    jmp .us_mult
.us_3:
    mov ax, 300
.us_mult:
    ; Multiply by (level + 1)
    xor bx, bx
    mov bl, [cs:level]
    inc bx
    mul bx                          ; AX = base * (level+1), DX=overflow

    ; Add to score
    add [cs:score_lo], ax
    adc [cs:score_hi], dx

    popa
    ret

; ============================================================================
; spawn_piece - Set current piece from next, generate new next
; ============================================================================
spawn_piece:
    push ax

    ; Current = next
    mov al, [cs:next_piece]
    mov [cs:cur_piece], al
    mov byte [cs:cur_rot], 0
    mov byte [cs:cur_x], 3
    mov byte [cs:cur_y], 0

    ; Set color
    xor ah, ah
    mov al, [cs:cur_piece]
    mov si, piece_colors
    add si, ax
    mov al, [cs:si]
    mov [cs:cur_color], al

    ; Generate new next piece
    call random_piece
    mov [cs:next_piece], al

    ; Draw next piece preview
    call draw_next_preview

    ; Reset drop timer
    call read_tick
    mov [cs:last_drop_tick], ax

    pop ax
    ret

; ============================================================================
; random_piece - Return random piece type 0-6 in AL
; ============================================================================
random_piece:
    push bx
    push dx

    ; LCG: seed = seed * 25173 + 13849
    mov ax, [cs:rng_seed]
    mov bx, 25173
    mul bx                          ; DX:AX = seed * 25173
    add ax, 13849
    mov [cs:rng_seed], ax

    ; Result = (seed >> 8) % 7
    mov al, ah                      ; AL = high byte of seed
    xor ah, ah
    xor dx, dx
    mov bx, 7
    div bx                          ; DX = remainder 0-6
    mov ax, dx

    pop dx
    pop bx
    ret

; ============================================================================
; draw_next_preview - Draw next piece in preview box
; ============================================================================
draw_next_preview:
    pusha

    ; Clear preview area
    mov bx, PREVIEW_X
    mov cx, PREVIEW_Y
    mov dx, 32
    mov si, 32
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Get piece cells for next_piece, rotation 0
    mov al, [cs:next_piece]
    xor ah, ah                      ; Rotation 0
    call get_piece_cells
    ; SI = piece cell data

    ; Get color for next piece
    xor bx, bx
    mov bl, [cs:next_piece]
    mov dl, [cs:piece_colors + bx]  ; DL = color

    ; Draw 4 cells in preview area
    mov cx, 4
.np_loop:
    push cx
    push dx

    ; Screen X = PREVIEW_X + col * 8
    xor ax, ax
    mov al, [cs:si]
    shl ax, 3
    add ax, PREVIEW_X
    mov [cs:cell_sx], ax

    ; Screen Y = PREVIEW_Y + row * 8
    xor ax, ax
    mov al, [cs:si + 1]
    shl ax, 3
    add ax, PREVIEW_Y
    mov [cs:cell_sy], ax

    ; Draw the preview cell using draw_cell convention
    ; BL=col offset (use as raw for preview), BH=row offset
    mov al, [cs:si]
    mov bl, al
    mov al, [cs:si + 1]
    mov bh, al

    pop dx
    push dx
    mov al, dl                      ; color

    ; We need to draw at preview coords, not board coords
    ; Manually draw an 8x8 block at cell_sx, cell_sy
    call draw_preview_cell

    pop dx
    add si, 2
    pop cx
    dec cx
    jnz .np_loop

    popa
    ret

; ============================================================================
; draw_preview_cell - Draw a colored 8x8 cell at cell_sx, cell_sy
; Input: AL=color, cell_sx/cell_sy set
; ============================================================================
draw_preview_cell:
    pusha
    mov [cs:cell_color], al

    ; Top edge white
    mov byte [cs:cell_px], 0
.dpc_top:
    mov bx, [cs:cell_sx]
    xor ah, ah
    mov al, [cs:cell_px]
    add bx, ax
    mov cx, [cs:cell_sy]
    mov al, 3
    mov ah, API_GFX_DRAW_PIXEL
    int 0x80
    inc byte [cs:cell_px]
    cmp byte [cs:cell_px], CELL_SIZE
    jb .dpc_top

    ; Left edge white
    mov byte [cs:cell_py], 1
.dpc_left:
    mov bx, [cs:cell_sx]
    mov cx, [cs:cell_sy]
    xor ah, ah
    mov al, [cs:cell_py]
    add cx, ax
    mov al, 3
    mov ah, API_GFX_DRAW_PIXEL
    int 0x80
    inc byte [cs:cell_py]
    cmp byte [cs:cell_py], CELL_SIZE
    jb .dpc_left

    ; Fill inner
    mov byte [cs:cell_py], 1
.dpc_row:
    mov byte [cs:cell_px], 1
.dpc_col:
    mov bx, [cs:cell_sx]
    xor ah, ah
    mov al, [cs:cell_px]
    add bx, ax
    mov cx, [cs:cell_sy]
    xor ah, ah
    mov al, [cs:cell_py]
    add cx, ax
    mov al, [cs:cell_color]
    mov ah, API_GFX_DRAW_PIXEL
    int 0x80
    inc byte [cs:cell_px]
    cmp byte [cs:cell_px], CELL_SIZE
    jb .dpc_col
    inc byte [cs:cell_py]
    cmp byte [cs:cell_py], CELL_SIZE
    jb .dpc_row

    popa
    ret

; ============================================================================
; check_drop_timer - Auto-drop piece when timer expires
; ============================================================================
check_drop_timer:
    push ax
    push bx

    call read_tick
    mov bx, ax
    sub bx, [cs:last_drop_tick]
    cmp bx, [cs:drop_speed]
    jb .cdt_done

    ; Time to drop
    call read_tick
    mov [cs:last_drop_tick], ax

    ; Try to move down
    mov al, [cs:cur_piece]
    mov [cs:chk_type], al
    mov al, [cs:cur_rot]
    mov [cs:chk_rot], al
    mov al, [cs:cur_x]
    mov [cs:chk_x], al
    mov al, [cs:cur_y]
    inc al
    mov [cs:chk_y], al
    call check_collision
    jc .cdt_lock

    call erase_piece
    inc byte [cs:cur_y]
    call draw_piece
    jmp .cdt_done

.cdt_lock:
    call lock_piece

.cdt_done:
    pop bx
    pop ax
    ret

; ============================================================================
; check_mouse - Handle mouse button clicks for UI buttons
; ============================================================================
check_mouse:
    pusha

    mov ah, API_MOUSE_GET_STATE
    int 0x80
    ; BX=X, CX=Y, DL=buttons

    test dl, 1                       ; Left button down?
    jz .cm_up

    cmp byte [cs:prev_mouse_btn], 0
    jne .cm_done                     ; Already held

    mov byte [cs:prev_mouse_btn], 1

    ; Hit test New Game button
    mov bx, BTN_NEWGAME_X
    mov cx, BTN_NEWGAME_Y
    mov dx, BTN_NEWGAME_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .cm_new_game

    ; Hit test Pause button
    mov bx, BTN_PAUSE_X
    mov cx, BTN_PAUSE_Y
    mov dx, BTN_PAUSE_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .cm_toggle_pause

    ; Hit test Quit button
    mov bx, BTN_QUIT_X
    mov cx, BTN_QUIT_Y
    mov dx, BTN_QUIT_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .cm_quit

    jmp .cm_done

.cm_up:
    mov byte [cs:prev_mouse_btn], 0
    jmp .cm_done

.cm_new_game:
    call start_new_game
    jmp .cm_done

.cm_toggle_pause:
    cmp byte [cs:game_state], STATE_PLAYING
    je .cm_pause
    cmp byte [cs:game_state], STATE_PAUSED
    je .cm_unpause
    jmp .cm_done

.cm_pause:
    mov byte [cs:game_state], STATE_PAUSED
    mov ah, API_SPEAKER_OFF
    int 0x80
    mov bx, 56
    mov cx, 88
    mov si, str_paused
    mov ah, API_GFX_DRAW_STRING_INV
    int 0x80
    jmp .cm_done

.cm_unpause:
    mov byte [cs:game_state], STATE_PLAYING
    mov bx, 56
    mov cx, 88
    mov dx, 48
    mov si, 12
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    call read_tick
    mov [cs:music_tick], ax
    mov [cs:last_drop_tick], ax
    jmp .cm_done

.cm_quit:
    mov byte [cs:quit_flag], 1
    jmp .cm_done

.cm_done:
    popa
    ret

; ============================================================================
; start_new_game - Reset everything and begin playing
; ============================================================================
start_new_game:
    pusha

    ; Clear board
    mov cx, 200
    mov di, 0
.sng_clear:
    mov byte [cs:board + di], 0
    inc di
    dec cx
    jnz .sng_clear

    ; Reset score
    mov word [cs:score_lo], 0
    mov word [cs:score_hi], 0
    mov word [cs:lines], 0
    mov byte [cs:level], 1
    mov word [cs:drop_speed], 18

    ; Reset music
    mov word [cs:music_idx], 0
    mov byte [cs:music_gap], 0

    ; Clear screen and redraw
    call clear_screen
    call draw_static_ui

    ; Clear the board area (in case UI left marks)
    mov bx, BOARD_X
    mov cx, BOARD_Y
    mov dx, BOARD_COLS * CELL_SIZE
    mov si, BOARD_ROWS * CELL_SIZE
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Generate first two pieces
    call random_piece
    mov [cs:next_piece], al
    call spawn_piece

    ; Draw first piece
    call draw_piece

    ; Start music
    mov si, korobeiniki
    mov bx, [cs:si]                 ; First note frequency
    cmp bx, 0xFFFF
    je .sng_no_music
    test bx, bx
    jz .sng_rest
    mov ah, API_SPEAKER_TONE
    int 0x80
    jmp .sng_music_started
.sng_rest:
    mov ah, API_SPEAKER_OFF
    int 0x80
.sng_music_started:
    mov ax, [cs:si + 2]
    mov [cs:music_dur], ax
    call read_tick
    mov [cs:music_tick], ax
.sng_no_music:

    ; Set state to playing
    mov byte [cs:game_state], STATE_PLAYING

    ; Set drop timer
    call read_tick
    mov [cs:last_drop_tick], ax

    popa
    ret

; ============================================================================
; music_update - Non-blocking music state machine
; ============================================================================
music_update:
    pusha

    ; Read current tick
    call read_tick
    mov bx, ax
    sub bx, [cs:music_tick]         ; BX = elapsed ticks

    cmp byte [cs:music_gap], 1
    je .mu_in_gap

    ; In note: check if duration elapsed
    cmp bx, [cs:music_dur]
    jb .mu_done

    ; Note finished - enter gap (brief silence between notes)
    mov ah, API_SPEAKER_OFF
    int 0x80
    call read_tick
    mov [cs:music_tick], ax
    mov byte [cs:music_gap], 1
    jmp .mu_done

.mu_in_gap:
    ; Gap lasts 1 tick
    cmp bx, 1
    jb .mu_done

    ; Gap finished - advance to next note
    add word [cs:music_idx], 1

    ; Calculate note address
    mov si, [cs:music_idx]
    shl si, 2                       ; 4 bytes per note entry
    add si, korobeiniki

    mov bx, [cs:si]                 ; Frequency
    cmp bx, 0xFFFF
    jne .mu_not_end

    ; Loop song from beginning
    mov word [cs:music_idx], 0
    mov si, korobeiniki
    mov bx, [cs:si]

.mu_not_end:
    ; Save duration
    mov ax, [cs:si + 2]
    mov [cs:music_dur], ax

    ; Play note or rest
    test bx, bx
    jz .mu_rest
    mov ah, API_SPEAKER_TONE
    int 0x80
    jmp .mu_started

.mu_rest:
    mov ah, API_SPEAKER_OFF
    int 0x80

.mu_started:
    call read_tick
    mov [cs:music_tick], ax
    mov byte [cs:music_gap], 0

.mu_done:
    popa
    ret

; ============================================================================
; update_score_display - Redraw score, lines, level values
; ============================================================================
update_score_display:
    pusha

    ; Clear value areas
    mov bx, 240
    mov cx, 26
    mov dx, 60
    mov si, 10
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    mov bx, 240
    mov cx, 38
    mov dx, 60
    mov si, 10
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    mov bx, 240
    mov cx, 50
    mov dx, 60
    mov si, 10
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw score value
    mov ax, [cs:score_lo]
    call word_to_decimal
    mov bx, 240
    mov cx, 26
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw lines value
    mov ax, [cs:lines]
    call word_to_decimal
    mov bx, 240
    mov cx, 38
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw level value
    xor ax, ax
    mov al, [cs:level]
    call word_to_decimal
    mov bx, 240
    mov cx, 50
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; word_to_decimal - Convert AX to decimal string in num_buf
; Input: AX = value
; Output: num_buf filled with null-terminated string
; ============================================================================
word_to_decimal:
    pusha

    mov di, num_buf + 5
    mov byte [cs:di], 0             ; Null terminator
    dec di

    mov bx, 10
    mov cx, 0                       ; Digit count
.wtd_loop:
    xor dx, dx
    div bx                          ; AX = quotient, DX = remainder
    add dl, '0'
    mov [cs:di], dl
    dec di
    inc cx
    test ax, ax
    jnz .wtd_loop

    ; Shift string to start of num_buf
    inc di                          ; DI points to first digit
    mov si, num_buf
    cmp di, si
    je .wtd_done                    ; Already at start
.wtd_shift:
    mov al, [cs:di]
    mov [cs:si], al
    test al, al
    jz .wtd_done
    inc di
    inc si
    jmp .wtd_shift

.wtd_done:
    popa
    ret

; ============================================================================
; read_tick - Read BIOS tick counter
; Output: AX = tick count (low word)
; ============================================================================
read_tick:
    push ds
    push bx
    mov ax, 0x0040
    mov ds, ax
    mov ax, [0x006C]
    pop bx
    pop ds
    ret
