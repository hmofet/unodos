; ============================================================================
; PACMAN.BIN - Pac-Man clone for UnoDOS
; Fullscreen CGA game (320x200, 4-color)
; ============================================================================

[ORG 0x0000]

; --- BIN Header (80 bytes) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic
    db 'Pac-Man', 0                 ; App name (12 bytes padded)
    times (0x04 + 12) - ($ - $$) db 0

; 16x16 icon bitmap (64 bytes, 2bpp CGA)
; Pac-Man facing right with open mouth
    db 0x03, 0xFC, 0x3F, 0xC0      ; Row 0:  ....XXXXXXXX....
    db 0x0F, 0xFF, 0xFF, 0xF0      ; Row 1:  ..XXXXXXXXXXXX..
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 2:  .XXXXXXXXXXXXXXX.
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 3:  .XXXXXXXXXXXXXXX.
    db 0xFF, 0xFF, 0xFF, 0x00      ; Row 4:  XXXXXXXXXXXX....
    db 0xFF, 0xFF, 0xF0, 0x00      ; Row 5:  XXXXXXXXXX......
    db 0xFF, 0xFF, 0x00, 0x00      ; Row 6:  XXXXXXXX........
    db 0xFF, 0xFF, 0x00, 0x00      ; Row 7:  XXXXXXXX........
    db 0xFF, 0xFF, 0xF0, 0x00      ; Row 8:  XXXXXXXXXX......
    db 0xFF, 0xFF, 0xFF, 0x00      ; Row 9:  XXXXXXXXXXXX....
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 10: .XXXXXXXXXXXXXXX.
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 11: .XXXXXXXXXXXXXXX.
    db 0x0F, 0xFF, 0xFF, 0xF0      ; Row 12: ..XXXXXXXXXXXX..
    db 0x03, 0xFC, 0x3F, 0xC0      ; Row 13: ....XXXXXXXX....
    db 0x00, 0x00, 0x00, 0x00      ; Row 14: ................
    db 0x00, 0x00, 0x00, 0x00      ; Row 15: ................

    times 0x50 - ($ - $$) db 0     ; Pad to offset 0x50

; ============================================================================
; API equates
; ============================================================================
API_GFX_DRAW_PIXEL      equ 0
API_GFX_DRAW_RECT       equ 1
API_GFX_DRAW_FILLED_RECT equ 2
API_GFX_DRAW_CHAR       equ 3
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_GFX_DRAW_STRING_INV equ 6
API_EVENT_GET           equ 9
API_APP_YIELD           equ 34
API_SPEAKER_TONE        equ 41
API_SPEAKER_OFF         equ 42
API_GFX_SET_FONT        equ 48
API_THEME_SET_COLORS    equ 54
API_THEME_GET_COLORS    equ 55
API_GET_TICK            equ 63
API_FILLED_RECT_COLOR   equ 67
API_DRAW_HLINE          equ 69
API_DRAW_VLINE          equ 70
API_GFX_DRAW_SPRITE     equ 94
API_MOUSE_SET_VISIBLE   equ 101
API_WORD_TO_STRING      equ 91

EVENT_KEY_PRESS         equ 1

; Game states
STATE_TITLE             equ 0
STATE_READY             equ 1
STATE_PLAYING           equ 2
STATE_DEATH             equ 3
STATE_LEVELUP           equ 4
STATE_GAMEOVER          equ 5

; Directions
DIR_UP                  equ 0
DIR_DOWN                equ 1
DIR_LEFT                equ 2
DIR_RIGHT               equ 3
DIR_NONE                equ 0xFF

; Tile types
TILE_EMPTY              equ 0
TILE_WALL               equ 1
TILE_DOT                equ 2
TILE_POWER              equ 3
TILE_GHOST_H            equ 4
TILE_GATE               equ 5

; Ghost states
GHOST_IN_HOUSE          equ 0
GHOST_CHASE             equ 1
GHOST_SCATTER           equ 2
GHOST_FRIGHTENED        equ 3
GHOST_EATEN             equ 4

; Maze dimensions
MAZE_COLS               equ 28
MAZE_ROWS               equ 25
TILE_SIZE               equ 8
MAZE_PX_W               equ 224      ; 28 * 8
HUD_X                   equ 228
NUM_GHOSTS              equ 3
FRIGHT_DURATION         equ 109      ; ~6 seconds at 18.2 Hz
FRIGHT_FLASH            equ 36       ; Last 2 seconds
MOVES_PER_TICK          equ 4

; ============================================================================
; Entry point
; ============================================================================
entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Save theme colors
    mov ah, API_THEME_GET_COLORS
    int 0x80
    mov [cs:saved_text_clr], al
    mov [cs:saved_bg_clr], bl
    mov [cs:saved_win_clr], cl

    ; Set game theme (white on black)
    mov al, 3
    mov bl, 0
    mov cl, 3
    mov ah, API_THEME_SET_COLORS
    int 0x80

    ; Set small font for HUD
    mov al, 1                       ; 8x8 font
    mov ah, API_GFX_SET_FONT
    int 0x80

    ; Hide mouse cursor
    mov al, 0
    mov ah, API_MOUSE_SET_VISIBLE
    int 0x80

    ; Init RNG seed
    mov ah, API_GET_TICK
    int 0x80
    mov [cs:rng_seed], ax

    ; Clear screen
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 200
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Init high score
    mov word [cs:high_score], 0

    ; Show title screen
    call draw_title
    mov byte [cs:game_state], STATE_TITLE

; ============================================================================
; Main loop
; ============================================================================
.main_loop:
    cmp byte [cs:quit_flag], 1
    je .exit_game

    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; --- Input ---
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_KEY_PRESS
    jne .no_event

    cmp dl, 27                      ; ESC = quit
    je .set_quit

    ; Dispatch by game state
    cmp byte [cs:game_state], STATE_TITLE
    je .title_key
    cmp byte [cs:game_state], STATE_PLAYING
    je .game_key
    cmp byte [cs:game_state], STATE_GAMEOVER
    je .gameover_key
    jmp .no_event

.title_key:
    ; Any key -> start game
    call init_level
    jmp .no_event

.game_key:
    cmp dl, 128
    je .dir_up
    cmp dl, 129
    je .dir_down
    cmp dl, 130
    je .dir_left
    cmp dl, 131
    je .dir_right
    cmp dl, 'w'
    je .dir_up
    cmp dl, 'W'
    je .dir_up
    cmp dl, 's'
    je .dir_down
    cmp dl, 'S'
    je .dir_down
    cmp dl, 'a'
    je .dir_left
    cmp dl, 'A'
    je .dir_left
    cmp dl, 'd'
    je .dir_right
    cmp dl, 'D'
    je .dir_right
    jmp .no_event

.dir_up:
    mov byte [cs:pac_next_dir], DIR_UP
    jmp .no_event
.dir_down:
    mov byte [cs:pac_next_dir], DIR_DOWN
    jmp .no_event
.dir_left:
    mov byte [cs:pac_next_dir], DIR_LEFT
    jmp .no_event
.dir_right:
    mov byte [cs:pac_next_dir], DIR_RIGHT
    jmp .no_event

.gameover_key:
    ; Any key -> title screen
    call draw_title
    mov byte [cs:game_state], STATE_TITLE
    jmp .no_event

.set_quit:
    mov byte [cs:quit_flag], 1
    jmp .no_event

.no_event:
    ; --- Tick-based game logic ---
    mov ah, API_GET_TICK
    int 0x80
    cmp ax, [cs:last_tick]
    je .main_loop
    mov [cs:last_tick], ax

    ; Dispatch by state
    cmp byte [cs:game_state], STATE_PLAYING
    je .tick_playing
    cmp byte [cs:game_state], STATE_READY
    je .tick_ready
    cmp byte [cs:game_state], STATE_DEATH
    je .tick_death
    cmp byte [cs:game_state], STATE_LEVELUP
    je .tick_levelup
    jmp .main_loop

.tick_playing:
    ; Save old positions once before substeps (for erase)
    call save_old_positions
    ; Substep loop: N movement steps per tick for arcade speed
    mov byte [cs:move_steps], MOVES_PER_TICK
.move_substep:
    call move_pac_man
    call check_dot_eat
    call move_ghosts
    call check_ghost_collision
    dec byte [cs:move_steps]
    jnz .move_substep
    ; Draw once per tick (after all substeps)
    call erase_pac
    call draw_pac
    call draw_ghosts
    ; Timers and animation once per tick
    call update_pac_animation
    call update_fright_timer
    call update_mode_timer
    call update_ghost_release
    call update_power_flash
    call update_sound
    jmp .main_loop

.tick_ready:
    dec word [cs:ready_timer]
    jnz .main_loop
    mov byte [cs:game_state], STATE_PLAYING
    ; Clear "READY!" text
    mov bx, 80
    mov cx, 104
    mov dx, 56
    mov si, 10
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Redraw dots that were under the text
    call redraw_tiles_at_ready
    jmp .main_loop

.tick_death:
    dec word [cs:death_timer]
    jnz .main_loop
    ; Death animation done
    dec byte [cs:lives]
    cmp byte [cs:lives], 0
    je .game_over
    ; Reset positions, continue level
    call reset_positions
    call draw_entities
    call draw_lives
    mov word [cs:ready_timer], 36
    mov byte [cs:game_state], STATE_READY
    ; Draw READY text
    mov bx, 88
    mov cx, 104
    mov si, str_ready
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .main_loop

.game_over:
    ; Update high score
    mov ax, [cs:score]
    cmp ax, [cs:high_score]
    jbe .no_new_high
    mov [cs:high_score], ax
.no_new_high:
    mov byte [cs:game_state], STATE_GAMEOVER
    ; Draw GAME OVER text
    mov bx, 72
    mov cx, 104
    mov si, str_gameover
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .main_loop

.tick_levelup:
    dec word [cs:levelup_timer]
    jnz .main_loop
    ; Next level
    inc byte [cs:level]
    call init_level
    jmp .main_loop

.exit_game:
    ; Speaker off
    mov ah, API_SPEAKER_OFF
    int 0x80

    ; Restore theme colors
    mov al, [cs:saved_text_clr]
    mov bl, [cs:saved_bg_clr]
    mov cl, [cs:saved_win_clr]
    mov ah, API_THEME_SET_COLORS
    int 0x80

    ; Show mouse cursor
    mov al, 1
    mov ah, API_MOUSE_SET_VISIBLE
    int 0x80

    pop es
    pop ds
    popa
    retf

; ============================================================================
; draw_title - Show title screen
; ============================================================================
draw_title:
    pusha
    ; Clear screen
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 200
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw PAC-MAN title
    mov bx, 100
    mov cx, 50
    mov si, str_title
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw "Press any key"
    mov bx, 76
    mov cx, 100
    mov si, str_press_key
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw a pac-man sprite as decoration
    mov bx, 148
    mov cx, 70
    mov dh, 7
    mov dl, 7
    mov al, 3                       ; White
    mov si, spr_pac_right_2
    mov ah, API_GFX_DRAW_SPRITE
    int 0x80

    ; Draw some dots as decoration
    mov bx, 168
    mov cx, 73
    mov dx, 2
    mov si, 2
    mov al, 3
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov bx, 180
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov bx, 192
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Draw ghost
    mov bx, 204
    mov cx, 70
    mov dh, 7
    mov dl, 7
    mov al, 2                       ; Magenta
    mov si, spr_ghost_normal
    mov ah, API_GFX_DRAW_SPRITE
    int 0x80

    ; Credits
    mov bx, 80
    mov cx, 140
    mov si, str_credit
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; init_level - Start or restart a level
; ============================================================================
init_level:
    pusha

    ; Copy maze template to working maze
    mov si, maze_template
    mov di, maze_data
    mov cx, MAZE_COLS * MAZE_ROWS
.copy_maze:
    mov al, [cs:si]
    mov [cs:di], al
    inc si
    inc di
    loop .copy_maze

    ; Count total dots
    mov word [cs:total_dots], 0
    mov word [cs:dots_eaten], 0
    mov si, maze_data
    mov cx, MAZE_COLS * MAZE_ROWS
.count_dots:
    mov al, [cs:si]
    cmp al, TILE_DOT
    je .is_dot
    cmp al, TILE_POWER
    je .is_dot
    jmp .not_dot
.is_dot:
    inc word [cs:total_dots]
.not_dot:
    inc si
    loop .count_dots

    ; Reset score only on level 1
    cmp byte [cs:level], 1
    ja .skip_score_reset
    mov word [cs:score], 0
    mov byte [cs:lives], 3
.skip_score_reset:

    ; Reset game vars
    mov word [cs:fright_timer], 0
    mov byte [cs:fright_kills], 0
    mov byte [cs:mode_index], 0
    mov word [cs:mode_timer], 127   ; First scatter phase
    mov byte [cs:mode_is_chase], 0
    mov byte [cs:snd_timer], 0
    mov byte [cs:power_flash], 0
    mov byte [cs:power_flash_tick], 0

    call reset_positions

    ; Clear screen and draw maze
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 200
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    call draw_maze
    call draw_hud
    call draw_entities

    ; Draw "READY!" and wait
    mov bx, 88
    mov cx, 104
    mov si, str_ready
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov word [cs:ready_timer], 36   ; ~2 seconds
    mov byte [cs:game_state], STATE_READY

    popa
    ret

; ============================================================================
; reset_positions - Reset pac-man and ghost positions
; ============================================================================
reset_positions:
    pusha

    ; Pac-Man start: tile (14, 19) -> pixel (112, 152)
    mov word [cs:pac_x], 112
    mov word [cs:pac_y], 152
    mov byte [cs:pac_dir], DIR_RIGHT
    mov byte [cs:pac_next_dir], DIR_RIGHT
    mov byte [cs:pac_anim], 0
    mov byte [cs:pac_anim_tick], 0
    mov byte [cs:pac_alive], 1
    mov word [cs:pac_old_x], 112
    mov word [cs:pac_old_y], 152

    ; Ghost 0 (Blinky): tile (14, 10) -> pixel (112, 80) - above ghost house
    mov word [cs:ghost_x], 112
    mov word [cs:ghost_y], 80
    mov byte [cs:ghost_dir], DIR_LEFT
    mov byte [cs:ghost_state], GHOST_SCATTER
    mov word [cs:ghost_timer], 0
    mov byte [cs:ghost_speed], 1

    ; Ghost 1 (Pinky): tile (13, 12) -> pixel (104, 96) - in ghost house
    mov word [cs:ghost_x + 2], 104
    mov word [cs:ghost_y + 2], 96
    mov byte [cs:ghost_dir + 1], DIR_UP
    mov byte [cs:ghost_state + 1], GHOST_IN_HOUSE
    mov word [cs:ghost_timer + 2], 54      ; Release after ~3s
    mov byte [cs:ghost_speed + 1], 1

    ; Ghost 2 (Clyde): tile (15, 12) -> pixel (120, 96) - in ghost house
    mov word [cs:ghost_x + 4], 120
    mov word [cs:ghost_y + 4], 96
    mov byte [cs:ghost_dir + 2], DIR_UP
    mov byte [cs:ghost_state + 2], GHOST_IN_HOUSE
    mov word [cs:ghost_timer + 4], 109     ; Release after ~6s
    mov byte [cs:ghost_speed + 2], 1

    popa
    ret

; ============================================================================
; draw_maze - Draw the full maze from maze_data
; ============================================================================
draw_maze:
    pusha

    mov byte [cs:_dm_row], 0
.row_loop:
    mov byte [cs:_dm_col], 0
.col_loop:
    movzx bx, byte [cs:_dm_col]
    movzx cx, byte [cs:_dm_row]
    call draw_tile

    inc byte [cs:_dm_col]
    cmp byte [cs:_dm_col], MAZE_COLS
    jb .col_loop
    inc byte [cs:_dm_row]
    cmp byte [cs:_dm_row], MAZE_ROWS
    jb .row_loop

    popa
    ret

_dm_row: db 0
_dm_col: db 0

; ============================================================================
; draw_tile - Draw a single tile at tile coords (BX=col, CX=row)
; ============================================================================
draw_tile:
    pusha

    ; Calculate maze_data offset
    push bx
    mov ax, cx
    mov dl, MAZE_COLS
    mul dl                          ; AX = row * 28
    pop bx
    add ax, bx                     ; AX = row * 28 + col
    mov di, ax                     ; DI = offset into maze_data

    ; Calculate pixel coords
    shl bx, 3                      ; BX = col * 8 = pixel X
    shl cx, 3                      ; CX = row * 8 = pixel Y

    mov al, [cs:maze_data + di]

    cmp al, TILE_WALL
    je .draw_wall
    cmp al, TILE_DOT
    je .draw_dot
    cmp al, TILE_POWER
    je .draw_power
    cmp al, TILE_GATE
    je .draw_gate

    ; TILE_EMPTY or TILE_GHOST_H: black
    mov dx, TILE_SIZE
    mov si, TILE_SIZE
    mov al, 0                       ; Black
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    jmp .tile_done

.draw_wall:
    ; Draw cyan filled tile
    mov dx, TILE_SIZE
    mov si, TILE_SIZE
    mov al, 1                       ; Cyan
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Carve interior to make corridor look
    ; Check if neighbors are non-wall to add black interior border
    push bx
    push cx
    ; Simply draw a smaller black rect inside to create wall "outline" effect
    add bx, 1
    add cx, 1
    mov dx, TILE_SIZE - 2
    mov si, TILE_SIZE - 2
    mov al, 0                       ; Black interior
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    ; Now redraw cyan edges only where there's an adjacent wall
    ; This creates the classic connected wall look
    call draw_wall_connections
    jmp .tile_done

.draw_dot:
    ; Black background then white 2x2 dot centered
    mov dx, TILE_SIZE
    mov si, TILE_SIZE
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    add bx, 3
    add cx, 3
    mov dx, 2
    mov si, 2
    mov al, 3                       ; White
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    jmp .tile_done

.draw_power:
    ; Black background then white 4x4 pellet centered
    mov dx, TILE_SIZE
    mov si, TILE_SIZE
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Only draw if not in flash-off state
    cmp byte [cs:power_flash], 0
    jne .tile_done
    add bx, 2
    add cx, 2
    mov dx, 4
    mov si, 4
    mov al, 3                       ; White
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    jmp .tile_done

.draw_gate:
    ; Black background with magenta horizontal line at center
    mov dx, TILE_SIZE
    mov si, TILE_SIZE
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    add cx, 3
    mov dx, TILE_SIZE
    mov al, 2                       ; Magenta
    mov ah, API_DRAW_HLINE
    int 0x80
    jmp .tile_done

.tile_done:
    popa
    ret

; ============================================================================
; draw_wall_connections - Draw cyan connectors to adjacent walls
; Input: BX=pixel_x, CX=pixel_y, DI=maze offset
; ============================================================================
draw_wall_connections:
    pusha

    ; Check right neighbor
    mov ax, di
    inc ax
    ; Bounds check: col must be < 27
    push ax
    push dx
    mov ax, di
    xor dx, dx
    push bx
    mov bl, MAZE_COLS
    div bl                          ; AL=row, AH=col
    pop bx
    mov dl, ah                      ; DL = current col
    pop dx
    pop ax
    cmp dl, MAZE_COLS - 1
    jae .no_right
    mov si, ax
    cmp byte [cs:maze_data + si], TILE_WALL
    jne .no_right
    ; Draw cyan connector on right edge
    push bx
    push cx
    add bx, 6                       ; X + 6
    add cx, 1
    mov dx, 2
    mov si, 6
    mov al, 1
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
.no_right:

    ; Check bottom neighbor
    mov ax, di
    add ax, MAZE_COLS
    cmp ax, MAZE_COLS * MAZE_ROWS
    jae .no_bottom
    mov si, ax
    cmp byte [cs:maze_data + si], TILE_WALL
    jne .no_bottom
    push bx
    push cx
    add bx, 1
    add cx, 6
    mov dx, 6
    mov si, 2
    mov al, 1
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
.no_bottom:

    ; Check left neighbor
    cmp dl, 0
    je .no_left
    mov ax, di
    dec ax
    mov si, ax
    cmp byte [cs:maze_data + si], TILE_WALL
    jne .no_left
    push bx
    push cx
    add cx, 1
    mov dx, 2
    mov si, 6
    mov al, 1
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
.no_left:

    ; Check top neighbor
    mov ax, di
    sub ax, MAZE_COLS
    jc .no_top
    mov si, ax
    cmp byte [cs:maze_data + si], TILE_WALL
    jne .no_top
    push bx
    push cx
    add bx, 1
    mov dx, 6
    mov si, 2
    mov al, 1
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
.no_top:

    popa
    ret

; ============================================================================
; draw_hud - Draw the HUD on the right side
; ============================================================================
draw_hud:
    pusha

    ; "SCORE"
    mov bx, HUD_X
    mov cx, 4
    mov si, str_score_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    call draw_score

    ; "HI"
    mov bx, HUD_X
    mov cx, 28
    mov si, str_hi_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    call draw_high_score

    ; Lives
    call draw_lives

    ; Level
    call draw_level

    popa
    ret

; ============================================================================
; draw_score / draw_high_score / draw_lives / draw_level
; ============================================================================
draw_score:
    pusha
    ; Clear score area
    mov bx, HUD_X
    mov cx, 14
    mov dx, 80
    mov si, 10
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Convert and draw
    mov ax, [cs:score]
    mov di, score_buf
    call word_to_decimal
    mov bx, HUD_X
    mov cx, 14
    mov si, score_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    popa
    ret

draw_high_score:
    pusha
    mov bx, HUD_X
    mov cx, 38
    mov dx, 80
    mov si, 10
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov ax, [cs:high_score]
    mov di, score_buf
    call word_to_decimal
    mov bx, HUD_X
    mov cx, 38
    mov si, score_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    popa
    ret

draw_lives:
    pusha
    ; Clear lives area
    mov bx, HUD_X
    mov cx, 60
    mov dx, 80
    mov si, 18
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Label
    mov bx, HUD_X
    mov cx, 60
    mov si, str_lives_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Draw pac-man icons for each life
    movzx cx, byte [cs:lives]
    cmp cx, 0
    je .lives_done
    mov bx, HUD_X
    mov byte [cs:_lives_i], 0
.lives_loop:
    push bx
    push cx
    mov cx, 70
    mov dh, 5
    mov dl, 5
    mov al, 3
    mov si, spr_life_icon
    mov ah, API_GFX_DRAW_SPRITE
    int 0x80
    pop cx
    pop bx
    add bx, 10
    inc byte [cs:_lives_i]
    movzx ax, byte [cs:_lives_i]
    cmp al, [cs:lives]
    jb .lives_loop
.lives_done:
    popa
    ret

_lives_i: db 0

draw_level:
    pusha
    mov bx, HUD_X
    mov cx, 88
    mov dx, 80
    mov si, 10
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov bx, HUD_X
    mov cx, 88
    mov si, str_level_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    movzx ax, byte [cs:level]
    mov di, score_buf
    call word_to_decimal
    mov bx, HUD_X + 32
    mov cx, 88
    mov si, score_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    popa
    ret

; ============================================================================
; word_to_decimal - Convert AX to decimal string at CS:DI
; ============================================================================
word_to_decimal:
    pusha
    mov cx, 0                       ; Digit count
    mov bx, 10
.div_loop:
    xor dx, dx
    div bx
    push dx                         ; Save remainder
    inc cx
    test ax, ax
    jnz .div_loop
    ; Pop digits into buffer
    mov si, di
.pop_loop:
    pop ax
    add al, '0'
    mov [cs:si], al
    inc si
    loop .pop_loop
    mov byte [cs:si], 0            ; Null terminate
    popa
    ret

; ============================================================================
; move_pac_man - Move pac-man by one step
; ============================================================================
move_pac_man:
    pusha

    cmp byte [cs:pac_alive], 1
    jne .move_done

    ; Check if at tile center (aligned to 8 pixels on both axes)
    mov ax, [cs:pac_x]
    and ax, 7
    jnz .continue_moving
    mov ax, [cs:pac_y]
    and ax, 7
    jnz .continue_moving

    ; At tile center: try to turn to pac_next_dir
    movzx ax, byte [cs:pac_next_dir]
    call pac_can_move_dir           ; Returns CF=0 if can move
    jc .try_current
    ; Turn successful
    mov al, [cs:pac_next_dir]
    mov [cs:pac_dir], al
    jmp .do_move

.try_current:
    ; Can we continue in current direction?
    movzx ax, byte [cs:pac_dir]
    call pac_can_move_dir
    jc .move_done                   ; Blocked, stop

.do_move:
    ; Move in current direction
    movzx ax, byte [cs:pac_dir]
    jmp .continue_moving

.continue_moving:
    ; Actually move
    cmp byte [cs:pac_dir], DIR_UP
    je .move_up
    cmp byte [cs:pac_dir], DIR_DOWN
    je .move_down
    cmp byte [cs:pac_dir], DIR_LEFT
    je .move_left
    cmp byte [cs:pac_dir], DIR_RIGHT
    je .move_right
    jmp .move_done

.move_up:
    sub word [cs:pac_y], 1
    jmp .move_done
.move_down:
    add word [cs:pac_y], 1
    jmp .move_done
.move_left:
    sub word [cs:pac_x], 1
    ; Tunnel wrap left
    cmp word [cs:pac_x], 0xFFFF    ; Went negative (unsigned)
    jb .move_done
    mov word [cs:pac_x], 223       ; Wrap to right side
    jmp .move_done
.move_right:
    add word [cs:pac_x], 1
    ; Tunnel wrap right
    cmp word [cs:pac_x], 224
    jb .move_done
    mov word [cs:pac_x], 0         ; Wrap to left side

.move_done:
    popa
    ret

; ============================================================================
; pac_can_move_dir - Check if pac-man can move in direction AX
; Returns: CF=0 if can move, CF=1 if blocked
; ============================================================================
pac_can_move_dir:
    push bx
    push cx
    push dx

    ; Get current tile center
    mov bx, [cs:pac_x]
    mov cx, [cs:pac_y]
    shr bx, 3                      ; tile_x
    shr cx, 3                      ; tile_y

    ; Compute next tile in direction AX
    cmp al, DIR_UP
    je .pcd_up
    cmp al, DIR_DOWN
    je .pcd_down
    cmp al, DIR_LEFT
    je .pcd_left
    ; DIR_RIGHT
    inc bx
    cmp bx, MAZE_COLS
    jae .pcd_wrap_right
    jmp .pcd_check
.pcd_wrap_right:
    xor bx, bx                     ; Wrap
    jmp .pcd_check
.pcd_up:
    dec cx
    jmp .pcd_check
.pcd_down:
    inc cx
    cmp cx, MAZE_ROWS
    jb .pcd_check
    jmp .pcd_blocked
.pcd_left:
    dec bx
    cmp bx, 0xFFFF
    jne .pcd_check
    mov bx, MAZE_COLS - 1          ; Wrap
    jmp .pcd_check

.pcd_check:
    ; Bounds check
    cmp cx, MAZE_ROWS
    jae .pcd_blocked
    ; Get tile at (bx, cx)
    mov ax, cx
    mov dl, MAZE_COLS
    mul dl
    add ax, bx
    mov bx, ax
    mov al, [cs:maze_data + bx]
    cmp al, TILE_WALL
    je .pcd_blocked
    cmp al, TILE_GATE
    je .pcd_blocked
    cmp al, TILE_GHOST_H
    je .pcd_blocked
    ; Can move
    clc
    pop dx
    pop cx
    pop bx
    ret

.pcd_blocked:
    stc
    pop dx
    pop cx
    pop bx
    ret

; ============================================================================
; check_dot_eat - Check if pac-man is on a dot/power pellet
; ============================================================================
check_dot_eat:
    pusha

    ; Get tile at pac-man center
    mov ax, [cs:pac_x]
    add ax, 3
    shr ax, 3                       ; tile_x
    mov bx, ax
    mov ax, [cs:pac_y]
    add ax, 3
    shr ax, 3                       ; tile_y
    mov dl, MAZE_COLS
    mul dl
    add ax, bx
    mov di, ax

    mov al, [cs:maze_data + di]

    cmp al, TILE_DOT
    je .eat_dot
    cmp al, TILE_POWER
    je .eat_power
    jmp .eat_done

.eat_dot:
    mov byte [cs:maze_data + di], TILE_EMPTY
    add word [cs:score], 10
    inc word [cs:dots_eaten]
    ; Sound: short blip
    mov byte [cs:snd_type], 1
    mov byte [cs:snd_timer], 2
    mov bx, 440
    mov ah, API_SPEAKER_TONE
    int 0x80
    call draw_score
    call check_level_complete
    jmp .eat_done

.eat_power:
    mov byte [cs:maze_data + di], TILE_EMPTY
    add word [cs:score], 50
    inc word [cs:dots_eaten]
    ; Activate frightened mode
    mov word [cs:fright_timer], FRIGHT_DURATION
    mov byte [cs:fright_kills], 0
    ; Reverse all active ghosts, make frightened
    mov cl, 0
.fright_loop:
    cmp cl, NUM_GHOSTS
    jae .fright_done
    movzx bx, cl
    cmp byte [cs:ghost_state + bx], GHOST_IN_HOUSE
    je .fright_next
    cmp byte [cs:ghost_state + bx], GHOST_EATEN
    je .fright_next
    mov byte [cs:ghost_state + bx], GHOST_FRIGHTENED
    ; Reverse direction
    mov al, [cs:ghost_dir + bx]
    call reverse_dir
    mov [cs:ghost_dir + bx], al
.fright_next:
    inc cl
    jmp .fright_loop
.fright_done:
    ; Sound: power pellet sweep
    mov byte [cs:snd_type], 2
    mov byte [cs:snd_timer], 4
    mov bx, 1000
    mov ah, API_SPEAKER_TONE
    int 0x80
    call draw_score
    call check_level_complete
    jmp .eat_done

.eat_done:
    popa
    ret

; ============================================================================
; check_level_complete
; ============================================================================
check_level_complete:
    mov ax, [cs:dots_eaten]
    cmp ax, [cs:total_dots]
    jne .not_complete
    ; Level complete!
    mov byte [cs:game_state], STATE_LEVELUP
    mov word [cs:levelup_timer], 36 ; ~2 seconds flash
    mov word [cs:fright_timer], 0
.not_complete:
    ret

; ============================================================================
; reverse_dir - Reverse direction in AL
; ============================================================================
reverse_dir:
    cmp al, DIR_UP
    je .rev_down
    cmp al, DIR_DOWN
    je .rev_up
    cmp al, DIR_LEFT
    je .rev_right
    ; DIR_RIGHT -> LEFT
    mov al, DIR_LEFT
    ret
.rev_down:
    mov al, DIR_DOWN
    ret
.rev_up:
    mov al, DIR_UP
    ret
.rev_right:
    mov al, DIR_RIGHT
    ret

; ============================================================================
; save_old_positions - Save entity positions before substeps (for erase)
; ============================================================================
save_old_positions:
    pusha
    mov ax, [cs:pac_x]
    mov [cs:pac_old_x], ax
    mov ax, [cs:pac_y]
    mov [cs:pac_old_y], ax
    mov bx, 0
.sop_loop:
    cmp bx, NUM_GHOSTS * 2
    jae .sop_done
    mov ax, [cs:ghost_x + bx]
    mov [cs:ghost_old_x + bx], ax
    mov ax, [cs:ghost_y + bx]
    mov [cs:ghost_old_y + bx], ax
    add bx, 2
    jmp .sop_loop
.sop_done:
    popa
    ret

; ============================================================================
; update_ghost_release - Release ghosts from house (called once per tick)
; ============================================================================
update_ghost_release:
    pusha
    mov bl, 1               ; Start at ghost 1 (Blinky=0 already out)
.ugr_loop:
    cmp bl, NUM_GHOSTS
    jae .ugr_done
    movzx bx, bl
    cmp byte [cs:ghost_state + bx], GHOST_IN_HOUSE
    jne .ugr_next
    ; Decrement release timer
    shl bx, 1
    cmp word [cs:ghost_timer + bx], 0
    je .ugr_release
    dec word [cs:ghost_timer + bx]
    shr bx, 1
    jmp .ugr_next
.ugr_release:
    ; Move ghost to gate exit and activate
    mov word [cs:ghost_x + bx], 112
    mov word [cs:ghost_y + bx], 80
    shr bx, 1
    mov al, [cs:mode_is_chase]
    cmp al, 0
    je .ugr_set_scatter
    mov byte [cs:ghost_state + bx], GHOST_CHASE
    jmp .ugr_released
.ugr_set_scatter:
    mov byte [cs:ghost_state + bx], GHOST_SCATTER
.ugr_released:
    mov byte [cs:ghost_dir + bx], DIR_LEFT
.ugr_next:
    inc bl
    jmp .ugr_loop
.ugr_done:
    popa
    ret

; ============================================================================
; move_ghosts - Move all ghosts
; ============================================================================
move_ghosts:
    pusha
    mov byte [cs:_ghost_idx], 0
.ghost_loop:
    movzx bx, byte [cs:_ghost_idx]
    cmp bl, NUM_GHOSTS
    jae .ghosts_done

    mov al, [cs:ghost_state + bx]
    cmp al, GHOST_IN_HOUSE
    je .ghost_next                  ; Skip in-house ghosts (release handled separately)

    call move_one_ghost

.ghost_next:
    inc byte [cs:_ghost_idx]
    jmp .ghost_loop

.ghosts_done:
    popa
    ret

_ghost_idx: db 0

; ============================================================================
; move_one_ghost - Move ghost BX by one step
; ============================================================================
move_one_ghost:
    pusha

    ; Check if at tile center
    shl bx, 1
    mov ax, [cs:ghost_x + bx]
    shr bx, 1
    and ax, 7
    jnz .ghost_continue
    shl bx, 1
    mov ax, [cs:ghost_y + bx]
    shr bx, 1
    and ax, 7
    jnz .ghost_continue

    ; At tile center: choose direction
    call ghost_choose_direction

.ghost_continue:
    ; Move in current direction
    mov al, [cs:ghost_dir + bx]
    shl bx, 1                      ; Word index for x/y arrays

    cmp al, DIR_UP
    je .g_up
    cmp al, DIR_DOWN
    je .g_down
    cmp al, DIR_LEFT
    je .g_left
    ; DIR_RIGHT
    add word [cs:ghost_x + bx], 1
    cmp word [cs:ghost_x + bx], 224
    jb .g_moved
    mov word [cs:ghost_x + bx], 0
    jmp .g_moved
.g_up:
    sub word [cs:ghost_y + bx], 1
    jmp .g_moved
.g_down:
    add word [cs:ghost_y + bx], 1
    jmp .g_moved
.g_left:
    sub word [cs:ghost_x + bx], 1
    cmp word [cs:ghost_x + bx], 0xFFFF
    jb .g_moved
    mov word [cs:ghost_x + bx], 223
.g_moved:
    popa
    ret

; ============================================================================
; ghost_choose_direction - Choose best direction for ghost BX
; ============================================================================
ghost_choose_direction:
    pusha

    ; Get ghost's current tile
    shl bx, 1
    mov ax, [cs:ghost_x + bx]
    shr ax, 3
    mov [cs:_gc_tile_x], ax
    mov ax, [cs:ghost_y + bx]
    shr ax, 3
    mov [cs:_gc_tile_y], ax
    shr bx, 1

    ; Determine target based on state
    mov al, [cs:ghost_state + bx]
    cmp al, GHOST_FRIGHTENED
    je .gc_random
    cmp al, GHOST_EATEN
    je .gc_target_house
    cmp al, GHOST_SCATTER
    je .gc_scatter_target
    ; GHOST_CHASE: use AI target
    jmp .gc_chase_target

.gc_random:
    ; Random direction
    call random_byte
    and al, 3
    mov [cs:_gc_best_dir], al
    ; Verify it's valid
    call .gc_validate_best
    jmp .gc_apply

.gc_target_house:
    ; Target: ghost house entrance (14, 10)
    mov word [cs:_gc_target_x], 14
    mov word [cs:_gc_target_y], 10
    jmp .gc_find_best

.gc_scatter_target:
    ; Each ghost has a corner target
    cmp bl, 0
    je .gc_s0
    cmp bl, 1
    je .gc_s1
    ; Ghost 2: bottom-left
    mov word [cs:_gc_target_x], 1
    mov word [cs:_gc_target_y], 23
    jmp .gc_find_best
.gc_s0:
    ; Ghost 0: top-right
    mov word [cs:_gc_target_x], 26
    mov word [cs:_gc_target_y], 1
    jmp .gc_find_best
.gc_s1:
    ; Ghost 1: top-left
    mov word [cs:_gc_target_x], 1
    mov word [cs:_gc_target_y], 1
    jmp .gc_find_best

.gc_chase_target:
    cmp bl, 0
    je .gc_c0
    cmp bl, 1
    je .gc_c1
    ; Ghost 2 (Clyde): target pac-man if far, else scatter
    mov ax, [cs:pac_x]
    shr ax, 3
    sub ax, [cs:_gc_tile_x]
    ; abs
    test ax, ax
    jns .gc_c2_pos_x
    neg ax
.gc_c2_pos_x:
    mov cx, ax
    mov ax, [cs:pac_y]
    shr ax, 3
    sub ax, [cs:_gc_tile_y]
    test ax, ax
    jns .gc_c2_pos_y
    neg ax
.gc_c2_pos_y:
    add cx, ax                      ; Manhattan distance
    cmp cx, 8
    jbe .gc_s1                      ; Close: scatter to corner (reuse ghost1's corner)
    ; Far: chase pac-man directly
    jmp .gc_c0

.gc_c0:
    ; Blinky: target pac-man's tile directly
    mov ax, [cs:pac_x]
    shr ax, 3
    mov [cs:_gc_target_x], ax
    mov ax, [cs:pac_y]
    shr ax, 3
    mov [cs:_gc_target_y], ax
    jmp .gc_find_best

.gc_c1:
    ; Pinky: target 4 tiles ahead of pac-man
    mov ax, [cs:pac_x]
    shr ax, 3
    mov cx, ax
    mov ax, [cs:pac_y]
    shr ax, 3
    mov dx, ax
    movzx ax, byte [cs:pac_dir]
    cmp al, DIR_UP
    je .gc_c1_up
    cmp al, DIR_DOWN
    je .gc_c1_down
    cmp al, DIR_LEFT
    je .gc_c1_left
    add cx, 4
    jmp .gc_c1_set
.gc_c1_up:
    sub dx, 4
    jmp .gc_c1_set
.gc_c1_down:
    add dx, 4
    jmp .gc_c1_set
.gc_c1_left:
    sub cx, 4
.gc_c1_set:
    mov [cs:_gc_target_x], cx
    mov [cs:_gc_target_y], dx
    jmp .gc_find_best

.gc_find_best:
    ; Try all 4 directions, find one with minimum Manhattan distance to target
    ; Priority: up, left, down, right (for tie-breaking)
    mov word [cs:_gc_best_dist], 0xFFFF
    mov byte [cs:_gc_best_dir], DIR_UP

    ; Current direction's reverse (can't go back)
    mov al, [cs:ghost_dir + bx]
    call reverse_dir
    mov [cs:_gc_reverse], al

    ; Try UP
    mov al, DIR_UP
    call .gc_try_dir
    ; Try LEFT
    mov al, DIR_LEFT
    call .gc_try_dir
    ; Try DOWN
    mov al, DIR_DOWN
    call .gc_try_dir
    ; Try RIGHT
    mov al, DIR_RIGHT
    call .gc_try_dir

    jmp .gc_apply

; .gc_try_dir: Try direction in AL
.gc_try_dir:
    push bx
    push cx
    push dx

    ; Skip if reverse
    cmp al, [cs:_gc_reverse]
    je .gc_try_skip

    ; Compute next tile
    mov cx, [cs:_gc_tile_x]
    mov dx, [cs:_gc_tile_y]
    cmp al, DIR_UP
    je .gc_try_up
    cmp al, DIR_DOWN
    je .gc_try_down
    cmp al, DIR_LEFT
    je .gc_try_left
    ; RIGHT
    inc cx
    cmp cx, MAZE_COLS
    jb .gc_try_check
    xor cx, cx
    jmp .gc_try_check
.gc_try_up:
    dec dx
    cmp dx, 0xFFFF
    jne .gc_try_check
    jmp .gc_try_skip
.gc_try_down:
    inc dx
    cmp dx, MAZE_ROWS
    jb .gc_try_check
    jmp .gc_try_skip
.gc_try_left:
    dec cx
    cmp cx, 0xFFFF
    jne .gc_try_check
    mov cx, MAZE_COLS - 1

.gc_try_check:
    ; Check if tile is passable
    push ax
    mov ax, dx
    push bx
    mov bl, MAZE_COLS
    mul bl
    pop bx
    add ax, cx
    mov bx, ax
    mov ah, [cs:maze_data + bx]
    cmp ah, TILE_WALL
    je .gc_try_skip_pop
    ; Gate: only eaten ghosts can enter
    cmp ah, TILE_GATE
    jne .gc_try_passable
    ; Check if this ghost is eaten
    pop ax
    push ax
    movzx bx, byte [cs:_ghost_idx]
    cmp byte [cs:ghost_state + bx], GHOST_EATEN
    jne .gc_try_skip_pop
.gc_try_passable:
    pop ax

    ; Compute Manhattan distance to target
    push ax
    mov bx, cx
    sub bx, [cs:_gc_target_x]
    test bx, bx
    jns .gc_try_pos_x
    neg bx
.gc_try_pos_x:
    mov cx, dx
    sub cx, [cs:_gc_target_y]
    test cx, cx
    jns .gc_try_pos_y
    neg cx
.gc_try_pos_y:
    add bx, cx                     ; BX = distance
    cmp bx, [cs:_gc_best_dist]
    jae .gc_try_not_better
    mov [cs:_gc_best_dist], bx
    pop ax
    mov [cs:_gc_best_dir], al
    jmp .gc_try_skip
.gc_try_not_better:
    pop ax
    jmp .gc_try_skip

.gc_try_skip_pop:
    pop ax
.gc_try_skip:
    pop dx
    pop cx
    pop bx
    ret

.gc_validate_best:
    ; Make sure random direction is valid
    pusha
    movzx ax, byte [cs:_gc_best_dir]
    mov cx, [cs:_gc_tile_x]
    mov dx, [cs:_gc_tile_y]
    cmp al, DIR_UP
    je .gcv_up
    cmp al, DIR_DOWN
    je .gcv_down
    cmp al, DIR_LEFT
    je .gcv_left
    inc cx
    jmp .gcv_check
.gcv_up:
    dec dx
    jmp .gcv_check
.gcv_down:
    inc dx
    jmp .gcv_check
.gcv_left:
    dec cx
.gcv_check:
    cmp cx, 0xFFFF
    je .gcv_fallback
    cmp cx, MAZE_COLS
    jae .gcv_fallback
    cmp dx, 0xFFFF
    je .gcv_fallback
    cmp dx, MAZE_ROWS
    jae .gcv_fallback
    mov ax, dx
    push bx
    mov bl, MAZE_COLS
    mul bl
    pop bx
    add ax, cx
    mov bx, ax
    cmp byte [cs:maze_data + bx], TILE_WALL
    je .gcv_fallback
    popa
    ret
.gcv_fallback:
    popa
    ; Just keep current direction
    movzx bx, byte [cs:_ghost_idx]
    mov al, [cs:ghost_dir + bx]
    mov [cs:_gc_best_dir], al
    ret

.gc_apply:
    movzx bx, byte [cs:_ghost_idx]
    mov al, [cs:_gc_best_dir]
    mov [cs:ghost_dir + bx], al
    popa
    ret

_gc_tile_x:    dw 0
_gc_tile_y:    dw 0
_gc_target_x:  dw 0
_gc_target_y:  dw 0
_gc_best_dist: dw 0
_gc_best_dir:  db 0
_gc_reverse:   db 0

; ============================================================================
; check_ghost_collision - Check pac-man vs all ghosts
; ============================================================================
check_ghost_collision:
    pusha

    mov cl, 0
.col_loop:
    cmp cl, NUM_GHOSTS
    jae .col_done

    movzx bx, cl
    cmp byte [cs:ghost_state + bx], GHOST_IN_HOUSE
    je .col_next
    cmp byte [cs:ghost_state + bx], GHOST_EATEN
    je .col_next

    shl bx, 1
    ; Check distance
    mov ax, [cs:pac_x]
    sub ax, [cs:ghost_x + bx]
    test ax, ax
    jns .col_pos_x
    neg ax
.col_pos_x:
    cmp ax, 6
    jae .col_next_shr
    mov dx, ax

    mov ax, [cs:pac_y]
    sub ax, [cs:ghost_y + bx]
    test ax, ax
    jns .col_pos_y
    neg ax
.col_pos_y:
    cmp ax, 6
    jae .col_next_shr

    ; Collision!
    shr bx, 1
    mov al, [cs:ghost_state + bx]
    cmp al, GHOST_FRIGHTENED
    je .eat_ghost

    ; Pac-man dies
    mov byte [cs:pac_alive], 0
    mov byte [cs:game_state], STATE_DEATH
    mov word [cs:death_timer], 27   ; ~1.5 seconds
    ; Death sound
    mov byte [cs:snd_type], 3
    mov byte [cs:snd_timer], 18
    mov bx, 500
    mov ah, API_SPEAKER_TONE
    int 0x80
    jmp .col_done

.eat_ghost:
    ; Ghost eaten
    mov byte [cs:ghost_state + bx], GHOST_EATEN
    ; Score: 200 << fright_kills
    mov ax, 200
    movzx cx, byte [cs:fright_kills]
    cmp cx, 0
    je .score_ok
.shift_score:
    shl ax, 1
    loop .shift_score
.score_ok:
    add [cs:score], ax
    inc byte [cs:fright_kills]
    call draw_score
    ; Sound
    mov byte [cs:snd_type], 4
    mov byte [cs:snd_timer], 3
    mov bx, 600
    mov ah, API_SPEAKER_TONE
    int 0x80
    jmp .col_next

.col_next_shr:
    shr bx, 1
.col_next:
    inc cl
    jmp .col_loop

.col_done:
    ; Update high score if needed
    mov ax, [cs:score]
    cmp ax, [cs:high_score]
    jbe .no_hi
    mov [cs:high_score], ax
    call draw_high_score
.no_hi:
    popa
    ret

; ============================================================================
; update_fright_timer - Count down frighten mode
; ============================================================================
update_fright_timer:
    cmp word [cs:fright_timer], 0
    je .ft_done
    dec word [cs:fright_timer]
    cmp word [cs:fright_timer], 0
    jne .ft_done
    ; Frighten ended: return ghosts to chase/scatter
    pusha
    mov cl, 0
.ft_loop:
    cmp cl, NUM_GHOSTS
    jae .ft_end
    movzx bx, cl
    cmp byte [cs:ghost_state + bx], GHOST_FRIGHTENED
    jne .ft_next
    mov al, [cs:mode_is_chase]
    cmp al, 0
    je .ft_set_scatter
    mov byte [cs:ghost_state + bx], GHOST_CHASE
    jmp .ft_next
.ft_set_scatter:
    mov byte [cs:ghost_state + bx], GHOST_SCATTER
.ft_next:
    inc cl
    jmp .ft_loop
.ft_end:
    popa
.ft_done:
    ret

; ============================================================================
; update_mode_timer - Chase/scatter mode switching
; ============================================================================
update_mode_timer:
    cmp word [cs:fright_timer], 0
    jne .mt_done                    ; Don't advance during frighten
    dec word [cs:mode_timer]
    cmp word [cs:mode_timer], 0
    jne .mt_done
    ; Switch mode
    pusha
    inc byte [cs:mode_index]
    xor byte [cs:mode_is_chase], 1  ; Toggle
    ; Load next timer
    movzx bx, byte [cs:mode_index]
    shl bx, 1
    mov ax, [cs:mode_schedule + bx]
    mov [cs:mode_timer], ax
    ; Reverse all non-frightened/eaten ghosts
    mov cl, 0
.mt_rev:
    cmp cl, NUM_GHOSTS
    jae .mt_end
    movzx bx, cl
    cmp byte [cs:ghost_state + bx], GHOST_FRIGHTENED
    je .mt_rev_next
    cmp byte [cs:ghost_state + bx], GHOST_EATEN
    je .mt_rev_next
    cmp byte [cs:ghost_state + bx], GHOST_IN_HOUSE
    je .mt_rev_next
    ; Set new state
    mov al, [cs:mode_is_chase]
    cmp al, 0
    je .mt_set_s
    mov byte [cs:ghost_state + bx], GHOST_CHASE
    jmp .mt_rev_dir
.mt_set_s:
    mov byte [cs:ghost_state + bx], GHOST_SCATTER
.mt_rev_dir:
    mov al, [cs:ghost_dir + bx]
    call reverse_dir
    mov [cs:ghost_dir + bx], al
.mt_rev_next:
    inc cl
    jmp .mt_rev
.mt_end:
    popa
.mt_done:
    ret

; ============================================================================
; update_pac_animation
; ============================================================================
update_pac_animation:
    inc byte [cs:pac_anim_tick]
    cmp byte [cs:pac_anim_tick], 3
    jb .pa_done
    mov byte [cs:pac_anim_tick], 0
    inc byte [cs:pac_anim]
    cmp byte [cs:pac_anim], 3
    jb .pa_done
    mov byte [cs:pac_anim], 0
.pa_done:
    ret

; ============================================================================
; update_power_flash - Flash power pellets
; ============================================================================
update_power_flash:
    inc byte [cs:power_flash_tick]
    cmp byte [cs:power_flash_tick], 8
    jb .pf_done
    mov byte [cs:power_flash_tick], 0
    xor byte [cs:power_flash], 1
    ; Redraw all power pellets
    pusha
    mov si, maze_data
    xor cx, cx                      ; Index
.pf_loop:
    cmp cx, MAZE_COLS * MAZE_ROWS
    jae .pf_end
    cmp byte [cs:si], TILE_POWER
    jne .pf_next
    ; Draw this power pellet tile
    push cx
    mov ax, cx
    xor dx, dx
    mov bx, MAZE_COLS
    div bx                          ; AX=row, DX=col
    mov bx, dx                      ; BX=col
    mov cx, ax                      ; CX=row
    call draw_tile
    pop cx
.pf_next:
    inc si
    inc cx
    jmp .pf_loop
.pf_end:
    popa
.pf_done:
    ret

; ============================================================================
; update_sound - Manage sound timers
; ============================================================================
update_sound:
    cmp byte [cs:snd_timer], 0
    je .us_done
    dec byte [cs:snd_timer]
    ; Update sound based on type
    cmp byte [cs:snd_type], 1
    je .us_chomp
    cmp byte [cs:snd_type], 2
    je .us_power
    cmp byte [cs:snd_type], 3
    je .us_death
    cmp byte [cs:snd_type], 4
    je .us_eat_ghost
    jmp .us_check_end

.us_chomp:
    ; Alternate freq
    test byte [cs:snd_timer], 1
    jz .us_check_end
    pusha
    mov bx, 880
    mov ah, API_SPEAKER_TONE
    int 0x80
    popa
    jmp .us_check_end

.us_power:
    ; Descending sweep
    pusha
    movzx ax, byte [cs:snd_timer]
    mov bx, 125
    mul bx                          ; Rough descending freq
    add ax, 400
    mov bx, ax
    mov ah, API_SPEAKER_TONE
    int 0x80
    popa
    jmp .us_check_end

.us_death:
    ; Descending tone
    pusha
    movzx ax, byte [cs:snd_timer]
    mov bx, 25
    mul bx
    add ax, 100
    mov bx, ax
    mov ah, API_SPEAKER_TONE
    int 0x80
    popa
    jmp .us_check_end

.us_eat_ghost:
    ; Ascending blip
    pusha
    movzx ax, byte [cs:snd_timer]
    neg ax
    add ax, 4
    mov bx, 200
    mul bx
    add ax, 600
    mov bx, ax
    mov ah, API_SPEAKER_TONE
    int 0x80
    popa
    jmp .us_check_end

.us_check_end:
    cmp byte [cs:snd_timer], 0
    jne .us_done
    ; Silence
    pusha
    mov ah, API_SPEAKER_OFF
    int 0x80
    popa
.us_done:
    ret

; ============================================================================
; random_byte - Return pseudo-random byte in AL
; ============================================================================
random_byte:
    push bx
    push cx
    mov ax, [cs:rng_seed]
    mov bx, 25173
    mul bx
    add ax, 13849
    mov [cs:rng_seed], ax
    mov al, ah                      ; Use high byte
    pop cx
    pop bx
    ret

; ============================================================================
; Drawing: erase/draw pac-man and ghosts
; ============================================================================

; erase_pac - Erase pac-man at old position by redrawing tiles
erase_pac:
    pusha
    mov bx, [cs:pac_old_x]
    mov cx, [cs:pac_old_y]
    call erase_entity
    popa
    ret

; draw_pac - Draw pac-man sprite at current position
draw_pac:
    pusha
    mov bx, [cs:pac_x]
    mov cx, [cs:pac_y]
    ; Choose sprite based on direction and animation frame
    call get_pac_sprite              ; Returns SI = sprite ptr
    mov dh, 7
    mov dl, 7
    mov al, 3                       ; White
    mov ah, API_GFX_DRAW_SPRITE
    int 0x80
    popa
    ret

; get_pac_sprite - Returns SI = sprite pointer based on pac_dir and pac_anim
get_pac_sprite:
    ; 4 directions x 3 frames = 12 sprites
    ; Each sprite is 7 bytes
    movzx ax, byte [cs:pac_dir]
    mov si, 3
    mul si                          ; AX = dir * 3
    movzx bx, byte [cs:pac_anim]
    add ax, bx                     ; AX = dir * 3 + anim
    mov si, 7
    mul si                          ; AX = byte offset
    add ax, spr_pac_sprites
    mov si, ax
    ret

; draw_entities - Draw all entities (used after level init)
draw_entities:
    pusha
    call draw_pac
    call draw_ghosts
    popa
    ret

; draw_ghosts - Erase old + draw new for all ghosts
draw_ghosts:
    pusha
    mov byte [cs:_dg_i], 0
.dg_loop:
    movzx bx, byte [cs:_dg_i]
    cmp bl, NUM_GHOSTS
    jae .dg_done

    ; Erase at old position
    movzx si, byte [cs:_dg_i]
    shl si, 1
    mov bx, [cs:ghost_old_x + si]
    mov cx, [cs:ghost_old_y + si]
    call erase_entity

    ; Skip drawing if in house (they're hidden)
    movzx bx, byte [cs:_dg_i]
    cmp byte [cs:ghost_state + bx], GHOST_IN_HOUSE
    je .dg_next

    ; Draw at new position
    movzx si, byte [cs:_dg_i]
    shl si, 1
    mov cx, [cs:ghost_y + si]
    mov bx, [cs:ghost_x + si]

    ; Choose sprite and color
    movzx ax, byte [cs:_dg_i]
    call get_ghost_sprite           ; Returns SI, AL
    mov dh, 7
    mov dl, 7
    mov ah, API_GFX_DRAW_SPRITE
    int 0x80

.dg_next:
    inc byte [cs:_dg_i]
    jmp .dg_loop
.dg_done:
    popa
    ret

_dg_i: db 0

; get_ghost_sprite - Get sprite and color for ghost
; Input: AX = ghost index, BX/CX = position (unused)
; Output: SI = sprite, AL = color
get_ghost_sprite:
    push bx
    mov bx, ax
    mov al, [cs:ghost_state + bx]
    pop bx

    cmp al, GHOST_EATEN
    je .gs_eaten
    cmp al, GHOST_FRIGHTENED
    je .gs_fright

    ; Normal ghost: magenta
    mov si, spr_ghost_normal
    mov al, 2                       ; Magenta
    ret

.gs_fright:
    ; Frightened: cyan, flash near end
    mov si, spr_ghost_fright
    mov al, 1                       ; Cyan
    ; Flash in last 2 seconds
    cmp word [cs:fright_timer], FRIGHT_FLASH
    ja .gs_fright_ok
    test byte [cs:power_flash_tick], 4
    jz .gs_fright_ok
    mov al, 2                       ; Flash to magenta
.gs_fright_ok:
    ret

.gs_eaten:
    ; Eyes only: white
    mov si, spr_ghost_eyes
    mov al, 3                       ; White
    ret

; erase_entity - Redraw tiles under a 7x7 entity at BX=px_x, CX=px_y
erase_entity:
    pusha

    ; Entity covers up to 2x2 tiles
    ; Top-left tile
    mov ax, bx
    shr ax, 3                       ; tile_x
    mov [cs:_ee_tx], ax
    mov ax, cx
    shr ax, 3                       ; tile_y
    mov [cs:_ee_ty], ax

    ; Draw up to 2x2 tiles
    mov bx, [cs:_ee_tx]
    mov cx, [cs:_ee_ty]
    call draw_tile

    ; Right tile
    mov bx, [cs:_ee_tx]
    inc bx
    cmp bx, MAZE_COLS
    jae .ee_no_right
    mov cx, [cs:_ee_ty]
    call draw_tile
.ee_no_right:

    ; Bottom tile
    mov bx, [cs:_ee_tx]
    mov cx, [cs:_ee_ty]
    inc cx
    cmp cx, MAZE_ROWS
    jae .ee_no_bottom
    call draw_tile
.ee_no_bottom:

    ; Bottom-right tile
    mov bx, [cs:_ee_tx]
    inc bx
    cmp bx, MAZE_COLS
    jae .ee_done
    mov cx, [cs:_ee_ty]
    inc cx
    cmp cx, MAZE_ROWS
    jae .ee_done
    call draw_tile

.ee_done:
    popa
    ret

_ee_tx: dw 0
_ee_ty: dw 0

; ============================================================================
; redraw_tiles_at_ready - Redraw tiles that were under READY! text
; ============================================================================
redraw_tiles_at_ready:
    pusha
    ; READY! was at pixel (80, 104) to (136, 114)
    ; That's tiles col 10-16, row 13
    mov byte [cs:_rr_col], 10
.rr_loop:
    movzx bx, byte [cs:_rr_col]
    mov cx, 13
    call draw_tile
    inc byte [cs:_rr_col]
    cmp byte [cs:_rr_col], 17
    jbe .rr_loop
    popa
    ret

_rr_col: db 0

; ============================================================================
; Sprite data (7x7, 1-bit, 1 byte per row)
; ============================================================================

; Pac-Man sprites: 4 directions x 3 frames x 7 bytes = 84 bytes
; Direction order: UP, DOWN, LEFT, RIGHT
; Frame order: closed (circle), mid-open, wide-open

spr_pac_sprites:
; --- UP ---
; Frame 0 (closed)
spr_pac_up_0:
    db 0b00111100    ; .XXXXX.  (note: 7 pixels, bit 7 unused in leftmost)
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b00111100    ; .XXXXX.
; Frame 1 (mid-open)
spr_pac_up_1:
    db 0b00100100    ; ..X..X.
    db 0b01100110    ; .XX..XX
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b00111100    ; .XXXXX.
; Frame 2 (wide-open)
spr_pac_up_2:
    db 0b00000000    ; .......
    db 0b01000010    ; .X...X.
    db 0b01100110    ; .XX.XX.
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b00111100    ; .XXXXX.

; --- DOWN ---
; Frame 0 (closed)
spr_pac_down_0:
    db 0b00111100
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b00111100
; Frame 1 (mid-open)
spr_pac_down_1:
    db 0b00111100
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b01100110
    db 0b00100100
; Frame 2 (wide-open)
spr_pac_down_2:
    db 0b00111100
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b01100110
    db 0b01000010
    db 0b00000000

; --- LEFT ---
; Frame 0 (closed)
spr_pac_left_0:
    db 0b00111100
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b00111100
; Frame 1 (mid-open)
spr_pac_left_1:
    db 0b00111100
    db 0b01111110
    db 0b00111110
    db 0b00011110
    db 0b00111110
    db 0b01111110
    db 0b00111100
; Frame 2 (wide-open)
spr_pac_left_2:
    db 0b00111100
    db 0b01111110
    db 0b00011110
    db 0b00001110
    db 0b00011110
    db 0b01111110
    db 0b00111100

; --- RIGHT ---
; Frame 0 (closed)
spr_pac_right_0:
    db 0b00111100
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b01111110
    db 0b00111100
; Frame 1 (mid-open)
spr_pac_right_1:
    db 0b00111100
    db 0b01111110
    db 0b01111100
    db 0b01111000
    db 0b01111100
    db 0b01111110
    db 0b00111100
; Frame 2 (wide-open)
spr_pac_right_2:
    db 0b00111100
    db 0b01111110
    db 0b01111000
    db 0b01110000
    db 0b01111000
    db 0b01111110
    db 0b00111100

; Ghost sprites (7 bytes each)
spr_ghost_normal:
    db 0b00111100    ; .XXXXX.
    db 0b01111110    ; XXXXXXX
    db 0b01011010    ; X.XX.X. (eyes)
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b01010100    ; X.X.X.. (wavy bottom)

spr_ghost_fright:
    db 0b00111100    ; .XXXXX.
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b01010100    ; X.X.X.. (scared face)
    db 0b01111110    ; XXXXXXX
    db 0b01111110    ; XXXXXXX
    db 0b01010100    ; X.X.X.. (wavy bottom)

spr_ghost_eyes:
    db 0b00000000    ; .......
    db 0b00000000    ; .......
    db 0b01010010    ; .X.X..X (eyes only)
    db 0b01010010    ; .X.X..X
    db 0b00000000    ; .......
    db 0b00000000    ; .......
    db 0b00000000    ; .......

; Life icon (5x5)
spr_life_icon:
    db 0b01110000    ; .XXX.
    db 0b11111000    ; XXXXX
    db 0b11100000    ; XXX..
    db 0b11111000    ; XXXXX
    db 0b01110000    ; .XXX.

; ============================================================================
; Maze template (28 x 25 = 700 bytes)
; Classic Pac-Man-style maze, horizontally symmetric
; ============================================================================
; W=wall(1), D=dot(2), P=power(3), E=empty(0), H=ghost_house(4), G=gate(5)
maze_template:
    ; Row 0: top wall
    db 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
    ; Row 1
    db 1,3,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,3,1
    ; Row 2
    db 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
    ; Row 3
    db 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
    ; Row 4
    db 1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1
    ; Row 5
    db 1,2,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,2,1
    ; Row 6
    db 1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1
    ; Row 7
    db 1,1,1,1,1,2,1,1,1,1,0,1,0,0,0,0,1,0,1,1,1,1,2,1,1,1,1,1
    ; Row 8
    db 0,0,0,0,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0
    ; Row 9
    db 0,0,0,0,1,2,1,0,1,1,1,1,5,0,0,5,1,1,1,1,0,1,2,1,0,0,0,0
    ; Row 10
    db 1,1,1,1,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,1,1,1,1
    ; Row 11
    db 0,0,0,0,0,2,0,0,1,4,4,4,4,4,4,4,4,4,4,1,0,0,2,0,0,0,0,0
    ; Row 12 - tunnel row
    db 0,0,0,0,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,0,0,0,0
    ; Row 13
    db 0,0,0,0,1,2,1,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1,2,1,0,0,0,0
    ; Row 14
    db 1,1,1,1,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,1,1,1,1
    ; Row 15
    db 1,2,2,2,2,2,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,2,2,2,2,2,1
    ; Row 16
    db 1,2,1,1,1,2,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,2,1,1,1,2,1
    ; Row 17
    db 1,3,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,3,1
    ; Row 18
    db 1,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,1
    ; Row 19
    db 1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1
    ; Row 20
    db 1,2,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,2,1
    ; Row 21
    db 1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1
    ; Row 22
    db 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
    ; Row 23
    db 1,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,1
    ; Row 24: bottom wall
    db 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1

; ============================================================================
; Working maze (modified as dots are eaten)
; ============================================================================
maze_data:
    times MAZE_COLS * MAZE_ROWS db 0

; ============================================================================
; Mode schedule (ticks at 18.2 Hz)
; Scatter 7s -> Chase 20s -> Scatter 7s -> Chase 20s ->
; Scatter 5s -> Chase 20s -> Scatter 5s -> Chase forever
; ============================================================================
mode_schedule:
    dw 127, 364, 127, 364, 91, 364, 91, 0xFFFF

; ============================================================================
; Game state variables
; ============================================================================
saved_text_clr: db 0
saved_bg_clr:   db 0
saved_win_clr:  db 0
quit_flag:      db 0
game_state:     db STATE_TITLE
last_tick:      dw 0
rng_seed:       dw 0
move_steps:     db 0

; Score
score:          dw 0
high_score:     dw 0
lives:          db 3
level:          db 1
total_dots:     dw 0
dots_eaten:     dw 0

; Pac-Man state
pac_x:          dw 112
pac_y:          dw 152
pac_old_x:      dw 112
pac_old_y:      dw 152
pac_dir:        db DIR_RIGHT
pac_next_dir:   db DIR_RIGHT
pac_anim:       db 0
pac_anim_tick:  db 0
pac_alive:      db 1

; Ghost state (3 ghosts)
ghost_x:        dw 112, 104, 120
ghost_y:        dw 80, 96, 96
ghost_old_x:    dw 112, 104, 120
ghost_old_y:    dw 80, 96, 96
ghost_dir:      db 0, 0, 0
ghost_state:    db GHOST_SCATTER, GHOST_IN_HOUSE, GHOST_IN_HOUSE
ghost_timer:    dw 0, 54, 109
ghost_speed:    db 1, 1, 1

; Frighten
fright_timer:   dw 0
fright_kills:   db 0

; Mode timer
mode_index:     db 0
mode_timer:     dw 127
mode_is_chase:  db 0

; Ready/death/levelup timers
ready_timer:    dw 36
death_timer:    dw 0
levelup_timer:  dw 0

; Sound
snd_type:       db 0                ; 0=none, 1=chomp, 2=power, 3=death, 4=ghost_eat
snd_timer:      db 0

; Power pellet flash
power_flash:      db 0
power_flash_tick: db 0

; Score buffer
score_buf:      times 8 db 0

; ============================================================================
; String data
; ============================================================================
str_title:       db 'PAC-MAN', 0
str_press_key:   db 'Press any key', 0
str_ready:       db 'READY!', 0
str_gameover:    db 'GAME  OVER', 0
str_score_label: db 'SCORE', 0
str_hi_label:    db 'HI', 0
str_lives_label: db 'LIVES', 0
str_level_label: db 'LVL:', 0
str_credit:      db 'UnoDOS  2026', 0
