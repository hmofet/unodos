; ============================================================================
; TETRISV.BIN - VGA Tetris for UnoDOS
; Fullscreen game with 256-color VGA graphics, 3D beveled blocks,
; unique colors per piece, and Korobeiniki music
; ============================================================================

[ORG 0x0000]

; --- BIN Header (80 bytes) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic
    db 'Tetris VGA', 0             ; App name (12 bytes padded)
    times (0x04 + 12) - ($ - $$) db 0

; 16x16 icon bitmap (64 bytes, 2bpp CGA)
; T-tetromino shape with scattered blocks
    db 0x00, 0x00, 0x00, 0x00      ; Row 0
    db 0x00, 0x00, 0x00, 0x00      ; Row 1
    db 0x3F, 0xFF, 0xFF, 0xC0      ; Row 2:  ..XXXXXXXXXXXX..
    db 0x3F, 0xFF, 0xFF, 0xC0      ; Row 3:  ..XXXXXXXXXXXX..
    db 0x00, 0x3F, 0xC0, 0x00      ; Row 4:  ......XXXX......
    db 0x00, 0x3F, 0xC0, 0x00      ; Row 5:  ......XXXX......
    db 0x00, 0x3F, 0xC0, 0x00      ; Row 6:  ......XXXX......
    db 0x00, 0x3F, 0xC0, 0x00      ; Row 7:  ......XXXX......
    db 0x00, 0x00, 0x00, 0x00      ; Row 8
    db 0x05, 0x50, 0x55, 0x00      ; Row 9
    db 0x05, 0x50, 0x55, 0x00      ; Row 10
    db 0x00, 0x00, 0x00, 0x00      ; Row 11
    db 0x00, 0xA0, 0x0A, 0x80      ; Row 12
    db 0x00, 0xA0, 0x0A, 0x80      ; Row 13
    db 0x00, 0x00, 0x00, 0x00      ; Row 14
    db 0x00, 0x00, 0x00, 0x00      ; Row 15

    times 0x50 - ($ - $$) db 0     ; Pad to offset 0x50

; ============================================================================
; Entry point
; ============================================================================
API_SET_VIDEO_MODE       equ 95
API_WIN_CREATE           equ 20
API_WIN_DESTROY          equ 21

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Save current video mode for restore on exit
    mov ah, 0x0F
    int 0x10
    mov [cs:saved_video_mode], al

    ; Switch to VGA mode 13h (320x200, 256 color)
    mov al, 0x13
    mov ah, API_SET_VIDEO_MODE
    int 0x80

    ; Create fullscreen frameless window (prevents launcher from redrawing desktop)
    xor bx, bx                     ; X = 0
    xor cx, cx                     ; Y = 0
    mov dx, 320                    ; Width
    mov si, 200                    ; Height
    mov ax, cs
    mov es, ax
    mov di, fs_win_title           ; Empty title
    mov al, 0x04                   ; No TITLE (0x01) or BORDER (0x02) flags
    mov ah, API_WIN_CREATE
    int 0x80
    jc .no_fs_win
    mov [cs:fs_win_handle], al
.no_fs_win:

    ; Save current theme colors for restore on exit
    mov ah, API_THEME_GET_COLORS
    int 0x80
    mov [cs:saved_text_clr], al
    mov [cs:saved_bg_clr], bl
    mov [cs:saved_win_clr], cl

    ; Set up custom VGA palette for piece colors
    call setup_piece_palette

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
    mov ah, API_APP_YIELD
    int 0x80

    ; --- Check events via per-task event queue ---
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_key_event
    cmp al, EVENT_KEY_PRESS
    jne .no_key_event

    ; DL = keycode (arrows: 128-131)
    ; ESC works in ALL states
    cmp dl, 27                      ; ESC?
    je .key_quit

    cmp byte [cs:game_state], STATE_PLAYING
    jne .check_pause_key

    ; Game keys (only when playing)
    cmp dl, 130                     ; Left arrow
    je .move_left
    cmp dl, 131                     ; Right arrow
    je .move_right
    cmp dl, 128                     ; Up arrow = rotate
    je .rotate
    cmp dl, 129                     ; Down arrow = soft drop
    je .soft_drop
    cmp dl, ' '                     ; Space = hard drop
    je .hard_drop
    cmp dl, 'p'
    je .pause_game
    cmp dl, 'P'
    je .pause_game
    jmp .no_key_event

.check_pause_key:
    cmp byte [cs:game_state], STATE_PAUSED
    jne .no_key_event
    cmp dl, 'p'
    je .unpause_game
    cmp dl, 'P'
    je .unpause_game
    jmp .no_key_event

.key_quit:
    mov byte [cs:quit_flag], 1
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
    mov ah, API_SPEAKER_OFF
    int 0x80
    mov bx, 56
    mov cx, 88
    mov si, str_paused
    mov ah, API_GFX_DRAW_STRING_INV
    int 0x80
    jmp .no_key_event
.unpause_game:
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
    jmp .no_key_event
.no_key_event:
    ; --- Handle mouse clicks ---
    call check_mouse

    ; --- Game logic (only when playing) ---
    cmp byte [cs:game_state], STATE_PLAYING
    jne .main_loop

    cmp byte [cs:sound_enabled], 0
    je .skip_music
    call music_update
.skip_music:
    call check_drop_timer

    jmp .main_loop

.exit_game:
    ; Turn off speaker
    mov ah, API_SPEAKER_OFF
    int 0x80

    ; Destroy fullscreen window
    cmp byte [cs:fs_win_handle], 0xFF
    je .no_fs_destroy
    mov al, [cs:fs_win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80
.no_fs_destroy:

    ; Restore theme colors
    mov al, [cs:saved_text_clr]
    mov bl, [cs:saved_bg_clr]
    mov cl, [cs:saved_win_clr]
    mov ah, API_THEME_SET_COLORS
    int 0x80

    ; Restore original video mode
    mov al, [cs:saved_video_mode]
    mov ah, API_SET_VIDEO_MODE
    int 0x80

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
API_EVENT_GET            equ 9
API_MOUSE_GET_STATE      equ 28
API_APP_YIELD            equ 34
API_GFX_SET_FONT         equ 48
API_DRAW_BUTTON          equ 51
API_HIT_TEST             equ 53
API_THEME_SET_COLORS     equ 54
API_THEME_GET_COLORS     equ 55
API_SPEAKER_TONE         equ 41
API_SPEAKER_OFF          equ 42
API_DRAW_CHECKBOX        equ 56
API_GET_TICK             equ 63
API_FILLED_RECT_COLOR    equ 67
API_DRAW_HLINE           equ 69
API_DRAW_VLINE           equ 70
API_DELAY_TICKS          equ 73
API_WORD_TO_STRING       equ 91

EVENT_KEY_PRESS          equ 1

STATE_MENU               equ 0
STATE_PLAYING            equ 1
STATE_PAUSED             equ 2
STATE_GAMEOVER           equ 3

BOARD_X                  equ 40
BOARD_Y                  equ 10
BOARD_COLS               equ 10
BOARD_ROWS               equ 20
CELL_SIZE                equ 9

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

CHK_SOUND_X              equ 168
CHK_SOUND_Y              equ 168
CHK_SOUND_W              equ 36
CHK_SOUND_H              equ 10

; VGA palette indices for piece colors (3 entries each: highlight, base, shadow)
; Entries 16-36 in the VGA palette
PAL_GRID                 equ 37     ; Dark grid lines
PAL_BORDER               equ 38     ; UI border color

; Piece base color palette indices (the "middle" of each triplet)
; draw_cell uses base-1 for highlight, base+1 for shadow
PAL_I_BASE               equ 17     ; Cyan
PAL_O_BASE               equ 20     ; Yellow
PAL_T_BASE               equ 23     ; Purple
PAL_S_BASE               equ 26     ; Green
PAL_Z_BASE               equ 29     ; Red
PAL_J_BASE               equ 32     ; Blue
PAL_L_BASE               equ 35     ; Orange

; ============================================================================
; Game variables
; ============================================================================
game_state:     db 0
quit_flag:      db 0
cur_piece:      db 0                ; Piece type 0-6
cur_rot:        db 0                ; Rotation 0-3
cur_x:          db 3                ; Board column (signed)
cur_y:          db 0                ; Board row (signed)
cur_color:      db 0                ; VGA palette base index
next_piece:     db 0
score_lo:       dw 0
score_hi:       dw 0
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
music_gap:      db 0
sound_enabled:  db 1

; Saved theme colors and video mode (restored on exit)
saved_text_clr:   db 0
saved_bg_clr:     db 0
saved_win_clr:    db 0
saved_video_mode: db 0x04
fs_win_handle: db 0xFF              ; Fullscreen window handle (0xFF = none)
fs_win_title: db 0                  ; Empty title string

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
clear_rows:     times 4 db 0
flash_count:    db 0

; Number display buffer
num_buf:        times 6 db 0

; Board: 10 cols x 20 rows, 1 byte per cell (0=empty, palette_base=color)
board:          times 200 db 0

; ============================================================================
; Piece data: 7 pieces x 4 rotations x 4 cells x 2 bytes (col,row)
; ============================================================================
piece_data:
    ; Piece 0: I-piece
    db 0,1, 1,1, 2,1, 3,1          ; Rot 0
    db 2,0, 2,1, 2,2, 2,3          ; Rot 1
    db 0,2, 1,2, 2,2, 3,2          ; Rot 2
    db 1,0, 1,1, 1,2, 1,3          ; Rot 3

    ; Piece 1: O-piece
    db 1,0, 2,0, 1,1, 2,1
    db 1,0, 2,0, 1,1, 2,1
    db 1,0, 2,0, 1,1, 2,1
    db 1,0, 2,0, 1,1, 2,1

    ; Piece 2: T-piece
    db 1,0, 0,1, 1,1, 2,1
    db 0,0, 0,1, 1,1, 0,2
    db 0,0, 1,0, 2,0, 1,1
    db 1,0, 0,1, 1,1, 1,2

    ; Piece 3: S-piece
    db 1,0, 2,0, 0,1, 1,1
    db 0,0, 0,1, 1,1, 1,2
    db 1,0, 2,0, 0,1, 1,1
    db 0,0, 0,1, 1,1, 1,2

    ; Piece 4: Z-piece
    db 0,0, 1,0, 1,1, 2,1
    db 1,0, 0,1, 1,1, 0,2
    db 0,0, 1,0, 1,1, 2,1
    db 1,0, 0,1, 1,1, 0,2

    ; Piece 5: J-piece
    db 0,0, 0,1, 1,1, 2,1
    db 0,0, 1,0, 0,1, 0,2
    db 0,0, 1,0, 2,0, 2,1
    db 1,0, 1,1, 0,2, 1,2

    ; Piece 6: L-piece
    db 2,0, 0,1, 1,1, 2,1
    db 0,0, 0,1, 0,2, 1,2
    db 0,0, 1,0, 2,0, 0,1
    db 0,0, 1,0, 1,1, 1,2

; VGA palette base index per piece type
piece_colors:
    db PAL_I_BASE               ; I = Cyan
    db PAL_O_BASE               ; O = Yellow
    db PAL_T_BASE               ; T = Purple
    db PAL_S_BASE               ; S = Green
    db PAL_Z_BASE               ; Z = Red
    db PAL_J_BASE               ; J = Blue
    db PAL_L_BASE               ; L = Orange

; Title color palette indices (base color per letter)
title_pal_colors:
    db PAL_I_BASE               ; T - Cyan
    db PAL_Z_BASE               ; E - Red
    db PAL_L_BASE               ; T - Orange
    db PAL_S_BASE               ; R - Green
    db PAL_J_BASE               ; I - Blue
    db PAL_T_BASE               ; S - Purple
    db PAL_O_BASE               ; ! - Yellow

; ============================================================================
; VGA Palette Data (entries 16-38)
; 3 bytes per entry: R, G, B (6-bit values 0-63)
; Triplets: highlight, base, shadow for each piece
; ============================================================================
vga_piece_palette:
    ; I-piece Cyan (entries 16-18)
    db 32, 63, 63                   ; 16: Highlight (bright cyan)
    db  0, 48, 48                   ; 17: Base (cyan)
    db  0, 24, 24                   ; 18: Shadow (dark cyan)
    ; O-piece Yellow (entries 19-21)
    db 63, 63, 32                   ; 19: Highlight (bright yellow)
    db 48, 48,  0                   ; 20: Base (yellow)
    db 24, 24,  0                   ; 21: Shadow (dark yellow)
    ; T-piece Purple (entries 22-24)
    db 48, 32, 63                   ; 22: Highlight (bright purple)
    db 32,  0, 48                   ; 23: Base (purple)
    db 16,  0, 24                   ; 24: Shadow (dark purple)
    ; S-piece Green (entries 25-27)
    db 32, 63, 32                   ; 25: Highlight (bright green)
    db  0, 48,  0                   ; 26: Base (green)
    db  0, 24,  0                   ; 27: Shadow (dark green)
    ; Z-piece Red (entries 28-30)
    db 63, 32, 32                   ; 28: Highlight (bright red)
    db 48,  0,  0                   ; 29: Base (red)
    db 24,  0,  0                   ; 30: Shadow (dark red)
    ; J-piece Blue (entries 31-33)
    db 32, 32, 63                   ; 31: Highlight (bright blue)
    db  0,  0, 48                   ; 32: Base (blue)
    db  0,  0, 24                   ; 33: Shadow (dark blue)
    ; L-piece Orange (entries 34-36)
    db 63, 48, 32                   ; 34: Highlight (bright orange)
    db 48, 24,  0                   ; 35: Base (orange)
    db 24, 12,  0                   ; 36: Shadow (dark orange)
    ; Grid and border colors
    db  8,  8, 12                   ; 37: Dark grid lines
    db 32, 32, 40                   ; 38: UI border (light gray-blue)

VGA_PAL_COUNT equ 23                ; 23 entries (16 through 38)

; ============================================================================
; setup_piece_palette - Set VGA palette entries for piece colors
; ============================================================================
setup_piece_palette:
    pusha
    ; Write palette entries 16-38 via I/O port
    mov dx, 0x3C8                   ; DAC write index port
    mov al, 16                      ; Start at palette entry 16
    out dx, al
    mov dx, 0x3C9                   ; DAC data port
    mov si, vga_piece_palette
    mov cx, VGA_PAL_COUNT * 3       ; 23 entries × 3 bytes
.spp_loop:
    mov al, [cs:si]
    out dx, al
    inc si
    dec cx
    jnz .spp_loop
    popa
    ret

; ============================================================================
; Korobeiniki melody (Tetris Theme A)
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

BEAT_E   equ 3
BEAT_Q   equ 5
BEAT_DQ  equ 8
BEAT_H   equ 10

korobeiniki:
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
    dw 0, BEAT_E
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
    dw 0, BEAT_E
    dw 0xFFFF, 0

MELODY_NOTES equ ($ - korobeiniki) / 4 - 1

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
str_sound:      db 'Sound', 0

; Title character data
title_chars:    db 'TETRIS!'
title_clrs:     db 1, 2, 3, 1, 2, 3, 1     ; CGA fallback colors

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

    ; Draw board border using VGA border color
    mov bx, BOARD_X - 2
    mov cx, BOARD_Y - 2
    mov dx, BOARD_COLS * CELL_SIZE + 4
    mov si, BOARD_ROWS * CELL_SIZE + 4
    mov al, PAL_BORDER
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Clear inside the border (black board area)
    mov bx, BOARD_X
    mov cx, BOARD_Y
    mov dx, BOARD_COLS * CELL_SIZE
    mov si, BOARD_ROWS * CELL_SIZE
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw grid lines on the board (subtle dark lines)
    call draw_grid

    ; Title "TETRIS!" in piece colors (large font)
    mov al, 2                       ; Large font 8x14
    mov ah, API_GFX_SET_FONT
    int 0x80

    mov byte [cs:cell_px], 0        ; Letter index
    mov word [cs:cell_sx], 172      ; X position
.title_color_loop:
    xor bx, bx
    mov bl, [cs:cell_px]
    mov al, [cs:title_pal_colors + bx]  ; VGA palette base color
    xor bl, bl
    mov cl, 3
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
    cmp byte [cs:cell_px], 7
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
    mov dx, 40
    mov si, 40
    mov al, PAL_BORDER
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Clear inside
    mov bx, PREVIEW_X - 1
    mov cx, PREVIEW_Y - 1
    mov dx, 36
    mov si, 36
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Set small font for compact button labels
    mov al, 0
    mov ah, API_GFX_SET_FONT
    int 0x80

    ; Buttons
    mov ax, cs
    mov es, ax

    mov bx, BTN_NEWGAME_X
    mov cx, BTN_NEWGAME_Y
    mov dx, BTN_NEWGAME_W
    mov si, BTN_H
    mov di, str_newgame
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    mov bx, BTN_PAUSE_X
    mov cx, BTN_PAUSE_Y
    mov dx, BTN_PAUSE_W
    mov si, BTN_H
    mov di, str_pause
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    mov bx, BTN_QUIT_X
    mov cx, BTN_QUIT_Y
    mov dx, BTN_QUIT_W
    mov si, BTN_H
    mov di, str_quit
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; Help text
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

    ; Sound checkbox
    mov bx, CHK_SOUND_X
    mov cx, CHK_SOUND_Y
    mov si, str_sound
    mov al, [cs:sound_enabled]
    mov ah, API_DRAW_CHECKBOX
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
; draw_grid - Draw subtle grid lines on the board
; ============================================================================
draw_grid:
    pusha

    ; Vertical grid lines
    mov bx, BOARD_X + CELL_SIZE
    mov cx, BOARD_Y
    mov dx, BOARD_ROWS * CELL_SIZE
.dg_vcol:
    cmp bx, BOARD_X + BOARD_COLS * CELL_SIZE
    jge .dg_hlines
    mov al, PAL_GRID
    mov ah, API_DRAW_VLINE
    int 0x80
    add bx, CELL_SIZE
    jmp .dg_vcol

.dg_hlines:
    ; Horizontal grid lines
    mov bx, BOARD_X
    mov cx, BOARD_Y + CELL_SIZE
    mov dx, BOARD_COLS * CELL_SIZE
.dg_hrow:
    cmp cx, BOARD_Y + BOARD_ROWS * CELL_SIZE
    jge .dg_done
    mov al, PAL_GRID
    mov ah, API_DRAW_HLINE
    int 0x80
    add cx, CELL_SIZE
    jmp .dg_hrow

.dg_done:
    popa
    ret

; ============================================================================
; draw_cell - Draw one cell with 3D beveled effect
; Input: AL=color (0=empty, or VGA palette base index), BL=col (0-9), BH=row (0-19)
; ============================================================================
draw_cell:
    pusha

    ; If color is 0, just clear the cell
    test al, al
    jz .clear_cell

    mov [cs:cell_color], al
    mov ch, bh                      ; Save row before BX is destroyed

    ; Calculate screen X = BOARD_X + col * CELL_SIZE
    xor ah, ah
    mov al, bl
    mov bx, CELL_SIZE
    mul bx
    add ax, BOARD_X
    mov [cs:cell_sx], ax

    ; Calculate screen Y = BOARD_Y + row * CELL_SIZE
    mov al, ch                      ; Use saved row
    xor ah, ah
    mov bx, CELL_SIZE
    mul bx
    add ax, BOARD_Y
    mov [cs:cell_sy], ax

    ; 1. Fill entire cell with BASE color
    mov bx, [cs:cell_sx]
    mov cx, [cs:cell_sy]
    mov dx, CELL_SIZE
    mov si, CELL_SIZE
    mov al, [cs:cell_color]         ; Base palette index
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 2. Top edge: highlight (base - 1)
    mov bx, [cs:cell_sx]
    mov cx, [cs:cell_sy]
    mov dx, CELL_SIZE
    mov al, [cs:cell_color]
    dec al                          ; Highlight = base - 1
    mov ah, API_DRAW_HLINE
    int 0x80

    ; 3. Left edge: highlight (base - 1)
    mov bx, [cs:cell_sx]
    mov cx, [cs:cell_sy]
    inc cx                          ; Skip top-left corner (already drawn)
    mov dx, CELL_SIZE - 1
    mov al, [cs:cell_color]
    dec al
    mov ah, API_DRAW_VLINE
    int 0x80

    ; 4. Bottom edge: shadow (base + 1)
    mov bx, [cs:cell_sx]
    mov cx, [cs:cell_sy]
    add cx, CELL_SIZE - 1           ; Bottom row
    mov dx, CELL_SIZE
    mov al, [cs:cell_color]
    inc al                          ; Shadow = base + 1
    mov ah, API_DRAW_HLINE
    int 0x80

    ; 5. Right edge: shadow (base + 1)
    mov bx, [cs:cell_sx]
    add bx, CELL_SIZE - 1           ; Right column
    mov cx, [cs:cell_sy]
    inc cx                          ; Skip top-right corner
    mov dx, CELL_SIZE - 2           ; Skip bottom-right (already drawn)
    mov al, [cs:cell_color]
    inc al
    mov ah, API_DRAW_VLINE
    int 0x80

    popa
    ret

.clear_cell:
    ; Clear cell and redraw grid lines through it
    mov ch, bh                      ; Save row

    xor ah, ah
    mov al, bl
    push bx
    mov bx, CELL_SIZE
    mul bx
    pop bx
    add ax, BOARD_X
    push ax                         ; Save screen X
    mov [cs:cell_sx], ax

    xor ah, ah
    mov al, ch
    push bx
    mov bx, CELL_SIZE
    mul bx
    pop bx
    add ax, BOARD_Y
    mov [cs:cell_sy], ax

    pop bx                          ; BX = screen X
    mov cx, ax                      ; CX = screen Y
    mov dx, CELL_SIZE
    mov si, CELL_SIZE
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Redraw grid lines through cleared cell
    ; Left edge vertical grid line (only if not column 0)
    mov bx, [cs:cell_sx]
    cmp bx, BOARD_X
    je .no_left_grid
    mov cx, [cs:cell_sy]
    mov dx, CELL_SIZE
    mov al, PAL_GRID
    mov ah, API_DRAW_VLINE
    int 0x80
.no_left_grid:
    ; Top edge horizontal grid line (only if not row 0)
    mov cx, [cs:cell_sy]
    cmp cx, BOARD_Y
    je .no_top_grid
    mov bx, [cs:cell_sx]
    mov dx, CELL_SIZE
    mov al, PAL_GRID
    mov ah, API_DRAW_HLINE
    int 0x80
.no_top_grid:

    popa
    ret

; Temp for draw_cell
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
    mov bl, al
    xor bh, bh
    shl bx, 5                       ; type * 32
    add bx, piece_data
    mov al, ah
    xor ah, ah
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
    mov cx, 4
.dp_loop:
    push cx
    mov al, [cs:si]
    add al, [cs:cur_x]
    mov bl, al
    mov al, [cs:si + 1]
    add al, [cs:cur_y]
    mov bh, al
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
    xor al, al
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
; Input: chk_type, chk_rot, chk_x, chk_y
; Output: CF=1 if collision
; ============================================================================
check_collision:
    pusha
    mov al, [cs:chk_type]
    mov ah, [cs:chk_rot]
    call get_piece_cells
    mov cx, 4
.cc_loop:
    push cx
    mov al, [cs:si]
    add al, [cs:chk_x]
    mov dl, al
    mov al, [cs:si + 1]
    add al, [cs:chk_y]
    mov dh, al
    test dl, 0x80
    jnz .cc_hit
    cmp dl, BOARD_COLS
    jge .cc_hit
    cmp dh, BOARD_ROWS
    jge .cc_hit
    test dh, 0x80
    jnz .cc_ok_cell
    mov cl, dl
    xor ax, ax
    mov al, dh
    mov bx, BOARD_COLS
    mul bx
    xor bx, bx
    mov bl, cl
    add ax, bx
    mov bx, ax
    cmp byte [cs:board + bx], 0
    jne .cc_hit
.cc_ok_cell:
    add si, 2
    pop cx
    dec cx
    jnz .cc_loop
    popa
    clc
    ret
.cc_hit:
    pop cx
    popa
    stc
    ret

; ============================================================================
; Movement functions
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

try_rotate:
    push ax
    mov al, [cs:cur_piece]
    mov [cs:chk_type], al
    mov al, [cs:cur_rot]
    inc al
    and al, 3
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
    inc word [cs:score_lo]
    call update_score_display
    pop ax
    ret
.tsd_lock:
    call lock_piece
    pop ax
    ret

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
    mov al, [cs:cur_piece]
    mov ah, [cs:cur_rot]
    call get_piece_cells
    mov cx, 4
.lp_loop:
    push cx
    mov al, [cs:si]
    add al, [cs:cur_x]
    mov dl, al
    mov al, [cs:si + 1]
    add al, [cs:cur_y]
    mov dh, al
    test dh, 0x80
    jnz .lp_skip
    mov cl, dl
    xor ax, ax
    mov al, dh
    mov bx, BOARD_COLS
    mul bx
    xor bx, bx
    mov bl, cl
    add ax, bx
    mov bx, ax
    mov al, [cs:cur_color]
    mov [cs:board + bx], al
.lp_skip:
    add si, 2
    pop cx
    dec cx
    jnz .lp_loop
    call check_lines
    call spawn_piece
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
    mov byte [cs:game_state], STATE_GAMEOVER
    mov ah, API_SPEAKER_OFF
    int 0x80
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
    mov byte [cs:chk_y], BOARD_ROWS - 1
.cl_row_loop:
    xor ax, ax
    mov al, [cs:chk_y]
    mov bx, BOARD_COLS
    mul bx
    mov bx, ax
    mov cx, BOARD_COLS
    mov si, 0
.cl_col_check:
    cmp byte [cs:board + bx + si], 0
    je .cl_not_full
    inc si
    dec cx
    jnz .cl_col_check
    push bx
    xor bx, bx
    mov bl, [cs:clear_count]
    mov al, [cs:chk_y]
    mov [cs:clear_rows + bx], al
    inc byte [cs:clear_count]
    pop bx
.cl_not_full:
    dec byte [cs:chk_y]
    cmp byte [cs:chk_y], 0xFF
    jne .cl_row_loop
    cmp byte [cs:clear_count], 0
    je .cl_done
    call animate_line_clear
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
    call draw_board
    call update_scoring
    xor ax, ax
    mov al, [cs:clear_count]
    add [cs:lines], ax
    mov ax, [cs:lines]
    xor dx, dx
    mov bx, 10
    div bx
    inc al
    cmp al, 15
    jbe .cl_level_ok
    mov al, 15
.cl_level_ok:
    mov [cs:level], al
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
; collapse_row - Remove row AL and shift above down
; ============================================================================
collapse_row:
    pusha
    xor cx, cx
    mov cl, al
.cr_shift_loop:
    cmp cl, 0
    je .cr_clear_top
    xor ax, ax
    mov al, cl
    mov bx, BOARD_COLS
    mul bx
    mov di, ax
    xor ax, ax
    mov al, cl
    dec al
    mov bx, BOARD_COLS
    mul bx
    mov si, ax
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
    mov bx, 0
.cr_zero:
    mov byte [cs:board + bx], 0
    inc bx
    cmp bx, BOARD_COLS
    jb .cr_zero
    popa
    ret

; ============================================================================
; animate_line_clear - Flash completed rows with white then black
; ============================================================================
animate_line_clear:
    pusha
    mov byte [cs:flash_count], 3
.alc_flash:
    ; Flash rows white
    xor cx, cx
    mov cl, [cs:clear_count]
    xor bx, bx
.alc_white:
    push cx
    push bx
    mov al, [cs:clear_rows + bx]
    mov bh, al
    mov cx, BOARD_COLS
    xor bl, bl
.alc_wrow:
    push cx
    mov al, 3                       ; White (system palette)
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
    mov cx, 2
    mov ah, API_DELAY_TICKS
    int 0x80
    ; Flash rows black
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
    xor al, al
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
    mov cx, 2
    mov ah, API_DELAY_TICKS
    int 0x80
    dec byte [cs:flash_count]
    jnz .alc_flash
    popa
    ret

; ============================================================================
; draw_board - Redraw all board cells from board array
; ============================================================================
draw_board:
    pusha
    xor bh, bh
.db_row:
    xor bl, bl
.db_col:
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
    ; Redraw grid on empty cells
    call draw_grid
    popa
    ret

; ============================================================================
; update_scoring - Add points based on lines cleared
; ============================================================================
update_scoring:
    pusha
    xor ax, ax
    mov al, [cs:clear_count]
    cmp al, 1
    je .us_1
    cmp al, 2
    je .us_2
    cmp al, 3
    je .us_3
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
    xor bx, bx
    mov bl, [cs:level]
    inc bx
    mul bx
    add [cs:score_lo], ax
    adc [cs:score_hi], dx
    popa
    ret

; ============================================================================
; spawn_piece - Set current piece from next, generate new next
; ============================================================================
spawn_piece:
    push ax
    mov al, [cs:next_piece]
    mov [cs:cur_piece], al
    mov byte [cs:cur_rot], 0
    mov byte [cs:cur_x], 3
    mov byte [cs:cur_y], 0
    ; Set color to VGA palette base index
    xor ah, ah
    mov al, [cs:cur_piece]
    mov si, piece_colors
    add si, ax
    mov al, [cs:si]
    mov [cs:cur_color], al
    call random_piece
    mov [cs:next_piece], al
    call draw_next_preview
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
    mov ax, [cs:rng_seed]
    mov bx, 25173
    mul bx
    add ax, 13849
    mov [cs:rng_seed], ax
    mov al, ah
    xor ah, ah
    xor dx, dx
    mov bx, 7
    div bx
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
    mov dx, 34
    mov si, 34
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    ; Get piece cells for next_piece, rotation 0
    mov al, [cs:next_piece]
    xor ah, ah
    call get_piece_cells
    ; Get VGA palette color for next piece
    xor bx, bx
    mov bl, [cs:next_piece]
    mov dl, [cs:piece_colors + bx]  ; DL = VGA palette base
    ; Draw 4 cells in preview area
    mov cx, 4
.np_loop:
    push cx
    push dx
    xor ax, ax
    mov al, [cs:si]
    push bx
    mov bx, CELL_SIZE
    mul bx
    pop bx
    add ax, PREVIEW_X
    mov [cs:cell_sx], ax
    xor ax, ax
    mov al, [cs:si + 1]
    push bx
    mov bx, CELL_SIZE
    mul bx
    pop bx
    add ax, PREVIEW_Y
    mov [cs:cell_sy], ax
    pop dx
    push dx
    mov al, dl                      ; color
    call draw_preview_cell
    pop dx
    add si, 2
    pop cx
    dec cx
    jnz .np_loop
    popa
    ret

; ============================================================================
; draw_preview_cell - Draw a 3D cell at cell_sx, cell_sy
; Input: AL=VGA palette base index
; ============================================================================
draw_preview_cell:
    pusha
    mov [cs:cell_color], al

    ; 1. Fill with base color
    mov bx, [cs:cell_sx]
    mov cx, [cs:cell_sy]
    mov dx, CELL_SIZE
    mov si, CELL_SIZE
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 2. Top highlight
    mov bx, [cs:cell_sx]
    mov cx, [cs:cell_sy]
    mov dx, CELL_SIZE
    mov al, [cs:cell_color]
    dec al
    mov ah, API_DRAW_HLINE
    int 0x80

    ; 3. Left highlight
    mov bx, [cs:cell_sx]
    mov cx, [cs:cell_sy]
    inc cx
    mov dx, CELL_SIZE - 1
    mov al, [cs:cell_color]
    dec al
    mov ah, API_DRAW_VLINE
    int 0x80

    ; 4. Bottom shadow
    mov bx, [cs:cell_sx]
    mov cx, [cs:cell_sy]
    add cx, CELL_SIZE - 1
    mov dx, CELL_SIZE
    mov al, [cs:cell_color]
    inc al
    mov ah, API_DRAW_HLINE
    int 0x80

    ; 5. Right shadow
    mov bx, [cs:cell_sx]
    add bx, CELL_SIZE - 1
    mov cx, [cs:cell_sy]
    inc cx
    mov dx, CELL_SIZE - 2
    mov al, [cs:cell_color]
    inc al
    mov ah, API_DRAW_VLINE
    int 0x80

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
    call read_tick
    mov [cs:last_drop_tick], ax
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
; check_mouse - Handle mouse button clicks
; ============================================================================
check_mouse:
    pusha
    mov ah, API_MOUSE_GET_STATE
    int 0x80
    test dl, 1
    jz .cm_up
    cmp byte [cs:prev_mouse_btn], 0
    jne .cm_done
    mov byte [cs:prev_mouse_btn], 1
    mov bx, BTN_NEWGAME_X
    mov cx, BTN_NEWGAME_Y
    mov dx, BTN_NEWGAME_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .cm_new_game
    mov bx, BTN_PAUSE_X
    mov cx, BTN_PAUSE_Y
    mov dx, BTN_PAUSE_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .cm_toggle_pause
    mov bx, BTN_QUIT_X
    mov cx, BTN_QUIT_Y
    mov dx, BTN_QUIT_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .cm_quit
    mov bx, CHK_SOUND_X
    mov cx, CHK_SOUND_Y
    mov dx, CHK_SOUND_W
    mov si, CHK_SOUND_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .cm_toggle_sound
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
.cm_toggle_sound:
    xor byte [cs:sound_enabled], 1
    mov al, 0
    mov ah, API_GFX_SET_FONT
    int 0x80
    mov bx, CHK_SOUND_X
    mov cx, CHK_SOUND_Y
    mov si, str_sound
    mov al, [cs:sound_enabled]
    mov ah, API_DRAW_CHECKBOX
    int 0x80
    mov al, 1
    mov ah, API_GFX_SET_FONT
    int 0x80
    cmp byte [cs:sound_enabled], 0
    jne .cm_sound_on
    mov ah, API_SPEAKER_OFF
    int 0x80
    jmp .cm_done
.cm_sound_on:
    cmp byte [cs:game_state], STATE_PLAYING
    jne .cm_done
    call read_tick
    mov [cs:music_tick], ax
    jmp .cm_done
.cm_done:
    popa
    ret

; ============================================================================
; start_new_game - Reset and begin
; ============================================================================
start_new_game:
    pusha
    mov cx, 200
    mov di, 0
.sng_clear:
    mov byte [cs:board + di], 0
    inc di
    dec cx
    jnz .sng_clear
    mov word [cs:score_lo], 0
    mov word [cs:score_hi], 0
    mov word [cs:lines], 0
    mov byte [cs:level], 1
    mov word [cs:drop_speed], 18
    mov word [cs:music_idx], 0
    mov byte [cs:music_gap], 0
    call clear_screen
    ; Re-apply piece palette (in case it was corrupted)
    call setup_piece_palette
    call draw_static_ui
    mov bx, BOARD_X
    mov cx, BOARD_Y
    mov dx, BOARD_COLS * CELL_SIZE
    mov si, BOARD_ROWS * CELL_SIZE
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    call draw_grid
    call random_piece
    mov [cs:next_piece], al
    call spawn_piece
    call draw_piece
    cmp byte [cs:sound_enabled], 0
    je .sng_no_music
    mov si, korobeiniki
    mov bx, [cs:si]
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
    mov byte [cs:game_state], STATE_PLAYING
    call read_tick
    mov [cs:last_drop_tick], ax
    popa
    ret

; ============================================================================
; music_update - Non-blocking music state machine
; ============================================================================
music_update:
    pusha
    call read_tick
    mov bx, ax
    sub bx, [cs:music_tick]
    cmp byte [cs:music_gap], 1
    je .mu_in_gap
    cmp bx, [cs:music_dur]
    jb .mu_done
    mov ah, API_SPEAKER_OFF
    int 0x80
    call read_tick
    mov [cs:music_tick], ax
    mov byte [cs:music_gap], 1
    jmp .mu_done
.mu_in_gap:
    cmp bx, 1
    jb .mu_done
    add word [cs:music_idx], 1
    mov si, [cs:music_idx]
    shl si, 2
    add si, korobeiniki
    mov bx, [cs:si]
    cmp bx, 0xFFFF
    jne .mu_not_end
    mov word [cs:music_idx], 0
    mov si, korobeiniki
    mov bx, [cs:si]
.mu_not_end:
    mov ax, [cs:si + 2]
    mov [cs:music_dur], ax
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
    mov dx, [cs:score_lo]
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov bx, 240
    mov cx, 26
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov dx, [cs:lines]
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov bx, 240
    mov cx, 38
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    movzx dx, byte [cs:level]
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov bx, 240
    mov cx, 50
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    popa
    ret

; ============================================================================
; read_tick - Read tick counter via kernel API
; ============================================================================
read_tick:
    mov ah, API_GET_TICK
    int 0x80
    ret
