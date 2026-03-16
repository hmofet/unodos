; ============================================================================
; PACMANV.BIN - VGA Pac-Man clone for UnoDOS
; Fullscreen VGA game (320x200, 256-color, mode 13h)
; Custom palette, 3D beveled maze, 4 color-coded ghosts, splash screen
; ============================================================================

[ORG 0x0000]

; --- BIN Header (80 bytes) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic
    db 'PacMan VGA', 0             ; App name (12 bytes padded)
    times (0x04 + 12) - ($ - $$) db 0

; 16x16 icon bitmap (64 bytes, 2bpp CGA) - same pac-man face as CGA version
    db 0x03, 0xFC, 0x3F, 0xC0
    db 0x0F, 0xFF, 0xFF, 0xF0
    db 0x3F, 0xFF, 0xFF, 0xFC
    db 0x3F, 0xFF, 0xFF, 0xFC
    db 0xFF, 0xFF, 0xFF, 0x00
    db 0xFF, 0xFF, 0xF0, 0x00
    db 0xFF, 0xFF, 0x00, 0x00
    db 0xFF, 0xFF, 0x00, 0x00
    db 0xFF, 0xFF, 0xF0, 0x00
    db 0xFF, 0xFF, 0xFF, 0x00
    db 0x3F, 0xFF, 0xFF, 0xFC
    db 0x3F, 0xFF, 0xFF, 0xFC
    db 0x0F, 0xFF, 0xFF, 0xF0
    db 0x03, 0xFC, 0x3F, 0xC0
    db 0x00, 0x00, 0x00, 0x00
    db 0x00, 0x00, 0x00, 0x00

    times 0x50 - ($ - $$) db 0

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
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
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
API_SET_VIDEO_MODE      equ 95
API_GET_VIDEO_MODE      equ 100
API_MOUSE_SET_VISIBLE   equ 101

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

; Layout
MAZE_COLS               equ 28
MAZE_ROWS               equ 25
TILE_SIZE               equ 8
HUD_X                   equ 232
NUM_GHOSTS              equ 4
FRIGHT_DURATION         equ 109
FRIGHT_FLASH            equ 36
MOVES_PER_TICK          equ 4

; VGA Palette color indices
CLR_PAC_SHADOW          equ 16
CLR_PAC_BASE            equ 17
CLR_PAC_BRIGHT          equ 18
CLR_PAC_HILITE          equ 19

CLR_BLINKY              equ 21
CLR_PINKY               equ 25
CLR_INKY                equ 29
CLR_CLYDE               equ 33
CLR_FRIGHT              equ 37

CLR_WALL_DEEP           equ 40
CLR_WALL_MID            equ 41
CLR_WALL_LIGHT          equ 42
CLR_WALL_EDGE           equ 43
CLR_WALL_INNER          equ 44
CLR_WALL_SHADE          equ 45
CLR_GATE                equ 46
CLR_GATE_HI             equ 47

CLR_DOT_DIM             equ 48
CLR_DOT                 equ 49
CLR_PELLET              equ 50
CLR_PELLET_FLASH        equ 51

CLR_SCORE               equ 59
CLR_SCORE_DIM           equ 57
CLR_HUD_BG              equ 60
CLR_HUD_BORDER          equ 63

CLR_TITLE_START         equ 64
CLR_SUB_TEXT            equ 73
CLR_EYE_WHITE           equ 74
CLR_EYE_IRIS            equ 75

PAL_ENTRY_COUNT         equ 64
PAL_START_INDEX         equ 16

; ============================================================================
; Entry point
; ============================================================================
entry:
    pusha
    push ds
    push es
    mov ax, cs
    mov ds, ax

    ; Save video mode
    mov ah, API_GET_VIDEO_MODE
    int 0x80
    mov [cs:saved_video_mode], al

    ; Switch to VGA 13h
    mov al, 0x13
    mov ah, API_SET_VIDEO_MODE
    int 0x80

    ; Create fullscreen frameless window
    xor bx, bx
    xor cx, cx
    mov dx, 320
    mov si, 200
    mov ax, cs
    mov es, ax
    mov di, fs_win_title
    mov al, 0x04
    mov ah, API_WIN_CREATE
    int 0x80
    jc .no_fs_win
    mov [cs:fs_win_handle], al
.no_fs_win:

    ; Hide mouse
    mov al, 0
    mov ah, API_MOUSE_SET_VISIBLE
    int 0x80

    ; Save theme
    mov ah, API_THEME_GET_COLORS
    int 0x80
    mov [cs:saved_text_clr], al
    mov [cs:saved_bg_clr], bl
    mov [cs:saved_win_clr], cl

    ; Set VGA palette
    call setup_palette

    ; Set text colors for VGA
    mov al, CLR_SCORE
    mov bl, 0
    mov cl, CLR_SCORE
    mov ah, API_THEME_SET_COLORS
    int 0x80

    ; Set font
    mov al, 1
    mov ah, API_GFX_SET_FONT
    int 0x80

    ; Init RNG
    mov ah, API_GET_TICK
    int 0x80
    mov [cs:rng_seed], ax

    ; Init high score
    mov word [cs:high_score], 0

    ; Show splash screen
    call show_splash

    ; Show title screen
    call draw_title_screen
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

    ; Input
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_KEY_PRESS
    jne .no_event

    cmp dl, 27
    je .set_quit

    cmp byte [cs:game_state], STATE_TITLE
    je .title_key
    cmp byte [cs:game_state], STATE_PLAYING
    je .game_key
    cmp byte [cs:game_state], STATE_GAMEOVER
    je .gameover_key
    jmp .no_event

.title_key:
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

.dir_up:    mov byte [cs:pac_next_dir], DIR_UP
            jmp .no_event
.dir_down:  mov byte [cs:pac_next_dir], DIR_DOWN
            jmp .no_event
.dir_left:  mov byte [cs:pac_next_dir], DIR_LEFT
            jmp .no_event
.dir_right: mov byte [cs:pac_next_dir], DIR_RIGHT
            jmp .no_event

.gameover_key:
    call draw_title_screen
    mov byte [cs:game_state], STATE_TITLE
    jmp .no_event

.set_quit:
    mov byte [cs:quit_flag], 1
    jmp .no_event

.no_event:
    ; Tick-based logic
    mov ah, API_GET_TICK
    int 0x80
    cmp ax, [cs:last_tick]
    je .main_loop
    mov [cs:last_tick], ax

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
    call draw_all_ghosts
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
    ; Clear READY text
    mov bx, 80
    mov cx, 104
    mov dx, 64
    mov si, 10
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    call redraw_tiles_at_ready
    jmp .main_loop

.tick_death:
    dec word [cs:death_timer]
    jnz .main_loop
    dec byte [cs:lives]
    cmp byte [cs:lives], 0
    je .game_over
    call reset_positions
    call draw_entities
    call draw_lives
    mov word [cs:ready_timer], 36
    mov byte [cs:game_state], STATE_READY
    mov bx, 88
    mov cx, 104
    mov si, str_ready
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .main_loop

.game_over:
    mov ax, [cs:score]
    cmp ax, [cs:high_score]
    jbe .no_new_high
    mov [cs:high_score], ax
.no_new_high:
    mov byte [cs:game_state], STATE_GAMEOVER
    mov bx, 72
    mov cx, 104
    mov si, str_gameover
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .main_loop

.tick_levelup:
    dec word [cs:levelup_timer]
    jnz .main_loop
    inc byte [cs:level]
    call init_level
    jmp .main_loop

.exit_game:
    mov ah, API_SPEAKER_OFF
    int 0x80

    ; Destroy fullscreen window
    cmp byte [cs:fs_win_handle], 0xFF
    je .no_fs_destroy
    mov al, [cs:fs_win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80
.no_fs_destroy:

    ; Restore theme
    mov al, [cs:saved_text_clr]
    mov bl, [cs:saved_bg_clr]
    mov cl, [cs:saved_win_clr]
    mov ah, API_THEME_SET_COLORS
    int 0x80

    ; Show mouse
    mov al, 1
    mov ah, API_MOUSE_SET_VISIBLE
    int 0x80

    ; Restore video mode
    mov al, [cs:saved_video_mode]
    mov ah, API_SET_VIDEO_MODE
    int 0x80

    pop es
    pop ds
    popa
    retf

; ============================================================================
; setup_palette - Load custom 64-entry VGA palette via DAC ports
; ============================================================================
setup_palette:
    pusha
    mov dx, 0x3C8
    mov al, PAL_START_INDEX
    out dx, al
    mov dx, 0x3C9
    mov si, vga_palette
    mov cx, PAL_ENTRY_COUNT * 3
.sp_loop:
    mov al, [cs:si]
    out dx, al
    inc si
    dec cx
    jnz .sp_loop
    popa
    ret

; ============================================================================
; show_splash - Splash screen with rainbow title and chase animation
; ============================================================================
show_splash:
    pusha
    ; Clear screen black
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 200
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Draw "PAC" in large block letters
    call draw_splash_title
    ; Draw "MAN" below
    ; Draw subtitle
    mov bx, 92
    mov cx, 92
    mov si, str_subtitle
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Rainbow line
    mov byte [cs:_splash_rx], 0
.rainbow_line:
    movzx bx, byte [cs:_splash_rx]
    mov cx, 82
    mov dx, 4
    mov si, 1
    movzx ax, byte [cs:_splash_rx]
    shr al, 4                      ; 0-19 -> color index within 8
    and al, 7
    add al, CLR_TITLE_START
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    add byte [cs:_splash_rx], 4
    cmp byte [cs:_splash_rx], 0     ; Wrapped past 255
    je .rainbow_done
    jmp .rainbow_line
.rainbow_done:
    ; Finish remaining pixels (256-319)
    mov bx, 256
.rainbow_line2:
    cmp bx, 320
    jae .rainbow_end
    push bx
    mov cx, 82
    mov dx, 4
    mov si, 1
    mov ax, bx
    shr al, 4
    and al, 7
    add al, CLR_TITLE_START
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop bx
    add bx, 4
    jmp .rainbow_line2
.rainbow_end:

    ; "Press any key" text
    mov bx, 84
    mov cx, 170
    mov si, str_press_key
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Version line
    mov bx, 96
    mov cx, 188
    mov si, str_version
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Splash animation loop: ~3 seconds or keypress
    mov word [cs:splash_timer], 54
    mov word [cs:splash_anim_x], 0
.splash_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    mov ah, API_EVENT_GET
    int 0x80
    jc .splash_no_key
    cmp al, EVENT_KEY_PRESS
    je .splash_done
.splash_no_key:
    mov ah, API_GET_TICK
    int 0x80
    cmp ax, [cs:splash_last_tick]
    je .splash_loop
    mov [cs:splash_last_tick], ax

    ; Animate: pac-man + ghosts moving across screen
    call animate_splash

    dec word [cs:splash_timer]
    jnz .splash_loop

.splash_done:
    popa
    ret

_splash_rx: db 0

; ============================================================================
; draw_splash_title - Rainbow block-letter "PAC-MAN"
; ============================================================================
draw_splash_title:
    pusha
    ; Draw each letter as block-art
    ; Letters: P A C - M A N at Y=20, block size 5x4 pixels, letter spacing
    mov word [cs:_spl_x], 42       ; Starting X
    mov si, splash_letters
    mov byte [cs:_spl_letter], 0

.spl_letter_loop:
    cmp byte [cs:_spl_letter], 7
    jae .spl_done

    mov byte [cs:_spl_row], 0
.spl_row_loop:
    cmp byte [cs:_spl_row], 7
    jae .spl_row_done

    mov al, [cs:si]                 ; Row bitmap
    mov byte [cs:_spl_bits], al
    mov byte [cs:_spl_col], 0
    mov word [cs:_spl_cx], 0       ; Column pixel offset

.spl_col_loop:
    cmp byte [cs:_spl_col], 5
    jae .spl_col_done

    test byte [cs:_spl_bits], 0x80
    jz .spl_no_block

    ; Draw block at (start_x + col*5, 20 + row*5)
    mov bx, [cs:_spl_x]
    add bx, [cs:_spl_cx]
    movzx cx, byte [cs:_spl_row]
    mov ax, cx
    mov cl, 5
    mul cl
    add ax, 20
    mov cx, ax
    mov dx, 4
    mov si, 4
    ; Rainbow color based on absolute column position
    mov ax, [cs:_spl_x]
    add ax, [cs:_spl_cx]
    shr ax, 3
    and al, 7
    add al, CLR_TITLE_START
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Reload SI to letter data
    movzx ax, byte [cs:_spl_letter]
    mov si, 7
    mul si
    add ax, splash_letters
    movzx si, byte [cs:_spl_row]
    add ax, si
    mov si, ax

.spl_no_block:
    shl byte [cs:_spl_bits], 1
    add word [cs:_spl_cx], 5
    inc byte [cs:_spl_col]
    jmp .spl_col_loop

.spl_col_done:
    ; Advance SI to next row
    movzx ax, byte [cs:_spl_letter]
    mov si, 7
    mul si
    add ax, splash_letters
    movzx si, byte [cs:_spl_row]
    inc si
    add ax, si
    mov si, ax
    inc byte [cs:_spl_row]
    jmp .spl_row_loop

.spl_row_done:
    add word [cs:_spl_x], 34       ; Letter width + gap
    inc byte [cs:_spl_letter]
    ; Set SI to next letter
    movzx ax, byte [cs:_spl_letter]
    mov si, 7
    mul si
    add ax, splash_letters
    mov si, ax
    jmp .spl_letter_loop

.spl_done:
    popa
    ret

_spl_x:      dw 0
_spl_cx:     dw 0
_spl_letter: db 0
_spl_row:    db 0
_spl_col:    db 0
_spl_bits:   db 0

; Splash block-letter data: P A C - M A N (5 columns wide x 7 rows, MSB-first)
splash_letters:
    ; P
    db 0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x80
    ; A
    db 0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88
    ; C
    db 0x78, 0x80, 0x80, 0x80, 0x80, 0x80, 0x78
    ; -
    db 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x00
    ; M
    db 0x88, 0xD8, 0xA8, 0x88, 0x88, 0x88, 0x88
    ; A
    db 0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88
    ; N
    db 0x88, 0xC8, 0xA8, 0x98, 0x88, 0x88, 0x88

; ============================================================================
; animate_splash - Move pac-man + ghosts across row 128
; ============================================================================
animate_splash:
    pusha
    ; Erase old positions
    mov bx, 0
    mov cx, 124
    mov dx, 320
    mov si, 16
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Draw dots ahead of pac-man
    mov bx, [cs:splash_anim_x]
    add bx, 16
.spl_dots:
    cmp bx, 310
    jae .spl_dots_done
    push bx
    mov cx, 130
    mov dx, 3
    mov si, 3
    mov al, CLR_DOT
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop bx
    add bx, 16
    jmp .spl_dots
.spl_dots_done:

    ; Draw pac-man (simple yellow circle)
    mov bx, [cs:splash_anim_x]
    cmp bx, 320
    jae .spl_reset
    mov cx, 126
    mov dx, 10
    mov si, 10
    mov al, CLR_PAC_BRIGHT
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Mouth cut (right-facing)
    mov bx, [cs:splash_anim_x]
    add bx, 7
    mov cx, 130
    mov dx, 4
    mov si, 3
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Draw 4 ghosts trailing
    mov byte [cs:_spl_gi], 0
.spl_ghost_loop:
    cmp byte [cs:_spl_gi], 4
    jae .spl_ghost_done
    movzx bx, byte [cs:_spl_gi]
    inc bx
    shl bx, 4                      ; bx = (gi+1) * 16
    mov ax, [cs:splash_anim_x]
    sub ax, bx
    cmp ax, 0
    jl .spl_ghost_next
    mov bx, ax
    mov cx, 126
    mov dx, 10
    mov si, 10
    ; Ghost color
    movzx di, byte [cs:_spl_gi]
    mov al, [cs:ghost_color_table + di]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Eyes
    mov ax, [cs:splash_anim_x]
    movzx bx, byte [cs:_spl_gi]
    inc bx
    shl bx, 4
    sub ax, bx
    mov bx, ax
    add bx, 2
    mov cx, 128
    mov dx, 2
    mov si, 3
    mov al, CLR_EYE_WHITE
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov ax, [cs:splash_anim_x]
    movzx bx, byte [cs:_spl_gi]
    inc bx
    shl bx, 4
    sub ax, bx
    mov bx, ax
    add bx, 6
    mov cx, 128
    mov dx, 2
    mov si, 3
    mov al, CLR_EYE_WHITE
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.spl_ghost_next:
    inc byte [cs:_spl_gi]
    jmp .spl_ghost_loop
.spl_ghost_done:

    add word [cs:splash_anim_x], 3
    cmp word [cs:splash_anim_x], 400
    jb .spl_anim_ret
.spl_reset:
    mov word [cs:splash_anim_x], 0
.spl_anim_ret:
    popa
    ret

_spl_gi: db 0

; ============================================================================
; draw_title_screen - Title/menu screen
; ============================================================================
draw_title_screen:
    pusha
    ; Clear screen
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 200
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Title
    mov bx, 108
    mov cx, 50
    mov si, str_title
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Pac-man sprite (yellow square with mouth)
    mov bx, 148
    mov cx, 72
    mov dx, 9
    mov si, 9
    mov al, CLR_PAC_BRIGHT
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov bx, 154
    mov cx, 75
    mov dx, 4
    mov si, 3
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Press any key
    mov bx, 84
    mov cx, 110
    mov si, str_press_key
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; init_level - Start or restart a level
; ============================================================================
init_level:
    pusha

    ; Copy maze template to working data
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

    ; Reset score on level 1
    cmp byte [cs:level], 1
    ja .skip_score_reset
    mov word [cs:score], 0
    mov byte [cs:lives], 3
.skip_score_reset:

    ; Reset game vars
    mov word [cs:fright_timer], 0
    mov byte [cs:fright_kills], 0
    mov byte [cs:mode_index], 0
    mov word [cs:mode_timer], 127
    mov byte [cs:mode_is_chase], 0
    mov byte [cs:snd_timer], 0
    mov byte [cs:power_flash], 0
    mov byte [cs:power_flash_tick], 0

    call reset_positions

    ; Clear screen and draw
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 200
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    call draw_maze
    call draw_hud_panel
    call draw_hud
    call draw_entities

    ; READY!
    mov bx, 88
    mov cx, 104
    mov si, str_ready
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov word [cs:ready_timer], 36
    mov byte [cs:game_state], STATE_READY

    popa
    ret

; ============================================================================
; reset_positions
; ============================================================================
reset_positions:
    pusha

    ; Pac-Man: tile (14,19) = pixel (112,152)
    mov word [cs:pac_x], 112
    mov word [cs:pac_y], 152
    mov byte [cs:pac_dir], DIR_RIGHT
    mov byte [cs:pac_next_dir], DIR_RIGHT
    mov byte [cs:pac_anim], 0
    mov byte [cs:pac_anim_tick], 0
    mov byte [cs:pac_alive], 1
    mov word [cs:pac_old_x], 112
    mov word [cs:pac_old_y], 152

    ; Ghost 0 (Blinky): above ghost house
    mov word [cs:ghost_x], 112
    mov word [cs:ghost_y], 80
    mov byte [cs:ghost_dir], DIR_LEFT
    mov byte [cs:ghost_state], GHOST_SCATTER
    mov word [cs:ghost_timer], 0

    ; Ghost 1 (Pinky): in house
    mov word [cs:ghost_x + 2], 104
    mov word [cs:ghost_y + 2], 96
    mov byte [cs:ghost_dir + 1], DIR_UP
    mov byte [cs:ghost_state + 1], GHOST_IN_HOUSE
    mov word [cs:ghost_timer + 2], 54

    ; Ghost 2 (Inky): in house
    mov word [cs:ghost_x + 4], 112
    mov word [cs:ghost_y + 4], 96
    mov byte [cs:ghost_dir + 2], DIR_UP
    mov byte [cs:ghost_state + 2], GHOST_IN_HOUSE
    mov word [cs:ghost_timer + 4], 90

    ; Ghost 3 (Clyde): in house
    mov word [cs:ghost_x + 6], 120
    mov word [cs:ghost_y + 6], 96
    mov byte [cs:ghost_dir + 3], DIR_UP
    mov byte [cs:ghost_state + 3], GHOST_IN_HOUSE
    mov word [cs:ghost_timer + 6], 127

    popa
    ret

; ============================================================================
; draw_maze - VGA beveled maze
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
; draw_tile - Draw single tile at (BX=col, CX=row) with VGA shading
; ============================================================================
draw_tile:
    pusha

    ; Calculate maze offset
    push bx
    mov ax, cx
    mov dl, MAZE_COLS
    mul dl
    pop bx
    add ax, bx
    mov di, ax

    ; Pixel coords
    shl bx, 3
    shl cx, 3

    mov al, [cs:maze_data + di]

    cmp al, TILE_WALL
    je .draw_wall
    cmp al, TILE_DOT
    je .draw_dot
    cmp al, TILE_POWER
    je .draw_power
    cmp al, TILE_GATE
    je .draw_gate

    ; Empty / ghost house: black
    mov dx, TILE_SIZE
    mov si, TILE_SIZE
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    jmp .tile_done

.draw_wall:
    ; Fill with mid-blue
    mov dx, TILE_SIZE
    mov si, TILE_SIZE
    mov al, CLR_WALL_MID
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Beveled edges: highlight on top/left, shadow on bottom/right
    ; Check neighbors for exposed edges
    call draw_wall_bevel
    jmp .tile_done

.draw_dot:
    ; Black bg + warm white 2x2 dot with shadow
    mov dx, TILE_SIZE
    mov si, TILE_SIZE
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    push bx
    push cx
    add bx, 3
    add cx, 3
    mov dx, 2
    mov si, 2
    mov al, CLR_DOT
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    ; Shadow pixel
    push bx
    push cx
    add bx, 5
    add cx, 5
    mov dx, 1
    mov si, 1
    mov al, CLR_DOT_DIM
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    jmp .tile_done

.draw_power:
    ; Black bg + large gold pellet
    mov dx, TILE_SIZE
    mov si, TILE_SIZE
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    cmp byte [cs:power_flash], 0
    jne .tile_done
    push bx
    push cx
    add bx, 1
    add cx, 1
    mov dx, 6
    mov si, 6
    mov al, CLR_PELLET
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    ; Highlight border on top-left
    push bx
    push cx
    add bx, 1
    add cx, 1
    mov dx, 6
    mov al, CLR_PELLET_FLASH
    mov ah, API_DRAW_HLINE
    int 0x80
    pop cx
    pop bx
    jmp .tile_done

.draw_gate:
    ; Black bg + shimmering magenta gate line
    mov dx, TILE_SIZE
    mov si, TILE_SIZE
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    push bx
    push cx
    add cx, 3
    mov dx, TILE_SIZE
    mov al, CLR_GATE
    mov ah, API_DRAW_HLINE
    int 0x80
    pop cx
    pop bx
    push bx
    push cx
    add cx, 4
    mov dx, TILE_SIZE
    mov al, CLR_GATE_HI
    mov ah, API_DRAW_HLINE
    int 0x80
    pop cx
    pop bx

.tile_done:
    popa
    ret

; ============================================================================
; draw_wall_bevel - Add 3D bevel to wall tile
; Input: BX=pixel_x, CX=pixel_y, DI=maze offset
; ============================================================================
draw_wall_bevel:
    pusha

    ; Top edge: if neighbor above is not wall, draw highlight
    mov ax, di
    sub ax, MAZE_COLS
    jc .bevel_no_top
    mov si, ax
    cmp byte [cs:maze_data + si], TILE_WALL
    je .bevel_no_top
    push bx
    push cx
    mov dx, TILE_SIZE
    mov al, CLR_WALL_EDGE
    mov ah, API_DRAW_HLINE
    int 0x80
    pop cx
    pop bx
.bevel_no_top:

    ; Left edge: if neighbor left is not wall
    ; Get column from DI
    mov ax, di
    xor dx, dx
    push bx
    mov bl, MAZE_COLS
    div bl
    pop bx
    ; AH = column
    cmp ah, 0
    je .bevel_no_left
    mov ax, di
    dec ax
    mov si, ax
    cmp byte [cs:maze_data + si], TILE_WALL
    je .bevel_no_left
    push bx
    push cx
    mov si, TILE_SIZE
    mov al, CLR_WALL_EDGE
    mov ah, API_DRAW_VLINE
    int 0x80
    pop cx
    pop bx
.bevel_no_left:

    ; Bottom edge: shadow
    mov ax, di
    add ax, MAZE_COLS
    cmp ax, MAZE_COLS * MAZE_ROWS
    jae .bevel_no_bottom
    mov si, ax
    cmp byte [cs:maze_data + si], TILE_WALL
    je .bevel_no_bottom
    push bx
    push cx
    add cx, 7
    mov dx, TILE_SIZE
    mov al, CLR_WALL_DEEP
    mov ah, API_DRAW_HLINE
    int 0x80
    pop cx
    pop bx
.bevel_no_bottom:

    ; Right edge: shadow
    mov ax, di
    xor dx, dx
    push bx
    mov bl, MAZE_COLS
    div bl
    pop bx
    cmp ah, MAZE_COLS - 1
    jae .bevel_no_right
    mov ax, di
    inc ax
    mov si, ax
    cmp byte [cs:maze_data + si], TILE_WALL
    je .bevel_no_right
    push bx
    push cx
    add bx, 7
    mov si, TILE_SIZE
    mov al, CLR_WALL_DEEP
    mov ah, API_DRAW_VLINE
    int 0x80
    pop cx
    pop bx
.bevel_no_right:

    popa
    ret

; ============================================================================
; draw_hud_panel - Navy gradient background for HUD
; ============================================================================
draw_hud_panel:
    pusha
    ; Fill HUD area with dark navy
    mov bx, 224
    mov cx, 0
    mov dx, 96
    mov si, 200
    mov al, CLR_HUD_BG
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Left border
    mov bx, 224
    mov cx, 0
    mov si, 200
    mov al, CLR_HUD_BORDER
    mov ah, API_DRAW_VLINE
    int 0x80
    popa
    ret

; ============================================================================
; draw_hud / draw_score / draw_high_score / draw_lives / draw_level
; ============================================================================
draw_hud:
    pusha
    mov bx, HUD_X
    mov cx, 8
    mov si, str_score_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    call draw_score
    mov bx, HUD_X
    mov cx, 40
    mov si, str_hi_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    call draw_high_score
    call draw_lives
    call draw_level
    popa
    ret

draw_score:
    pusha
    mov bx, HUD_X
    mov cx, 20
    mov dx, 80
    mov si, 10
    mov al, CLR_HUD_BG
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov ax, [cs:score]
    mov di, score_buf
    call word_to_decimal
    mov bx, HUD_X
    mov cx, 20
    mov si, score_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    popa
    ret

draw_high_score:
    pusha
    mov bx, HUD_X
    mov cx, 52
    mov dx, 80
    mov si, 10
    mov al, CLR_HUD_BG
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov ax, [cs:high_score]
    mov di, score_buf
    call word_to_decimal
    mov bx, HUD_X
    mov cx, 52
    mov si, score_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    popa
    ret

draw_lives:
    pusha
    mov bx, HUD_X
    mov cx, 76
    mov dx, 80
    mov si, 20
    mov al, CLR_HUD_BG
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov bx, HUD_X
    mov cx, 76
    mov si, str_lives_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Draw pac-man icons for lives
    movzx cx, byte [cs:lives]
    cmp cx, 0
    je .lives_done
    mov bx, HUD_X
    mov byte [cs:_lives_i], 0
.lives_loop:
    push bx
    push cx
    mov cx, 88
    mov dx, 7
    mov si, 7
    mov al, CLR_PAC_BRIGHT
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    add bx, 12
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
    mov cx, 110
    mov dx, 80
    mov si, 10
    mov al, CLR_HUD_BG
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov bx, HUD_X
    mov cx, 110
    mov si, str_level_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    movzx ax, byte [cs:level]
    mov di, score_buf
    call word_to_decimal
    mov bx, HUD_X + 40
    mov cx, 110
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
    mov cx, 0
    mov bx, 10
.div_loop:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .div_loop
    mov si, di
.pop_loop:
    pop ax
    add al, '0'
    mov [cs:si], al
    inc si
    loop .pop_loop
    mov byte [cs:si], 0
    popa
    ret

; ============================================================================
; VGA Entity Drawing - Composited multi-color sprites
; ============================================================================

; draw_pac - Draw pac-man as composited colored shape
draw_pac:
    pusha
    mov bx, [cs:pac_x]
    mov cx, [cs:pac_y]
    ; Body: 7x7 bright yellow
    push bx
    push cx
    add bx, 1
    add cx, 1
    mov dx, 7
    mov si, 7
    mov al, CLR_PAC_BRIGHT
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    ; Highlight top edge
    push bx
    push cx
    add bx, 1
    add cx, 1
    mov dx, 7
    mov al, CLR_PAC_HILITE
    mov ah, API_DRAW_HLINE
    int 0x80
    pop cx
    pop bx
    ; Shadow bottom
    push bx
    push cx
    add bx, 1
    add cx, 7
    mov dx, 7
    mov al, CLR_PAC_SHADOW
    mov ah, API_DRAW_HLINE
    int 0x80
    pop cx
    pop bx

    ; Mouth cut based on direction and animation
    cmp byte [cs:pac_anim], 0
    je .pac_eye_only                ; Closed mouth - just draw eye

    ; Mouth size depends on anim frame
    mov dl, 2                       ; Frame 1: small
    cmp byte [cs:pac_anim], 2
    jne .pac_mouth_size_ok
    mov dl, 4                       ; Frame 2: large
.pac_mouth_size_ok:
    movzx si, dl
    movzx dx, dl

    mov bx, [cs:pac_x]
    mov cx, [cs:pac_y]

    cmp byte [cs:pac_dir], DIR_RIGHT
    je .pac_mouth_right
    cmp byte [cs:pac_dir], DIR_LEFT
    je .pac_mouth_left
    cmp byte [cs:pac_dir], DIR_UP
    je .pac_mouth_up
    ; Down
    add bx, 3
    add cx, 8
    sub cx, si
    jmp .pac_draw_mouth
.pac_mouth_right:
    add bx, 8
    sub bx, dx
    add cx, 3
    jmp .pac_draw_mouth
.pac_mouth_left:
    add bx, 1
    add cx, 3
    jmp .pac_draw_mouth
.pac_mouth_up:
    add bx, 3
    add cx, 1

.pac_draw_mouth:
    mov al, 0                       ; Black
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

.pac_eye_only:
    ; Eye based on direction
    mov bx, [cs:pac_x]
    mov cx, [cs:pac_y]
    cmp byte [cs:pac_dir], DIR_RIGHT
    je .pac_eye_r
    cmp byte [cs:pac_dir], DIR_LEFT
    je .pac_eye_l
    cmp byte [cs:pac_dir], DIR_UP
    je .pac_eye_u
    ; Down
    add bx, 3
    add cx, 5
    jmp .pac_draw_eye
.pac_eye_r:
    add bx, 5
    add cx, 3
    jmp .pac_draw_eye
.pac_eye_l:
    add bx, 3
    add cx, 3
    jmp .pac_draw_eye
.pac_eye_u:
    add bx, 3
    add cx, 2

.pac_draw_eye:
    mov dx, 1
    mov si, 1
    mov al, 0                       ; Black eye
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    popa
    ret

; erase_pac - Erase at old position by redrawing tiles
erase_pac:
    pusha
    mov bx, [cs:pac_old_x]
    mov cx, [cs:pac_old_y]
    call erase_entity
    popa
    ret

; draw_ghost_vga - Draw ghost at BX=x, CX=y, ghost index in _dg_i
draw_ghost_vga:
    pusha
    ; Determine color
    movzx di, byte [cs:_dg_i]
    mov al, [cs:ghost_state + di]

    cmp al, GHOST_EATEN
    je .dg_eyes_only
    cmp al, GHOST_FRIGHTENED
    je .dg_fright_color

    ; Normal: use ghost color table
    mov al, [cs:ghost_color_table + di]
    jmp .dg_draw_body

.dg_fright_color:
    mov al, CLR_FRIGHT
    ; Flash near end
    cmp word [cs:fright_timer], FRIGHT_FLASH
    ja .dg_draw_body
    test byte [cs:power_flash_tick], 4
    jz .dg_draw_body
    mov al, CLR_EYE_WHITE           ; Flash white
    jmp .dg_draw_body

.dg_draw_body:
    mov [cs:_dg_color], al
    ; Body: 7x7
    push bx
    push cx
    add bx, 1
    add cx, 1
    mov dx, 7
    mov si, 7
    mov al, [cs:_dg_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    ; Rounded top: 5x1 at top
    push bx
    push cx
    add bx, 2
    mov dx, 5
    mov si, 1
    mov al, [cs:_dg_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    ; Wavy bottom: alternating pixels at y+8
    push bx
    push cx
    add cx, 8
    add bx, 1
    mov dx, 1
    mov si, 1
    mov al, [cs:_dg_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    add bx, 2
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    add bx, 2
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    add bx, 2
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    ; Highlight top edge
    push bx
    push cx
    add bx, 2
    add cx, 1
    mov dx, 5
    mov al, [cs:_dg_color]
    inc al                          ; Lighter shade
    mov ah, API_DRAW_HLINE
    int 0x80
    pop cx
    pop bx

    ; Eyes (skip if frightened)
    movzx di, byte [cs:_dg_i]
    cmp byte [cs:ghost_state + di], GHOST_FRIGHTENED
    je .dg_fright_face

.dg_draw_eyes:
    ; Left eye white
    push bx
    push cx
    add bx, 2
    add cx, 3
    mov dx, 2
    mov si, 2
    mov al, CLR_EYE_WHITE
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    ; Right eye white
    push bx
    push cx
    add bx, 5
    add cx, 3
    mov dx, 2
    mov si, 2
    mov al, CLR_EYE_WHITE
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    ; Iris: direction-dependent offset
    movzx di, byte [cs:_dg_i]
    mov al, [cs:ghost_dir + di]
    xor dl, dl                      ; dx offset for iris
    cmp al, DIR_LEFT
    je .iris_left
    cmp al, DIR_RIGHT
    je .iris_right
    jmp .iris_draw
.iris_left:
    mov dl, 0
    jmp .iris_draw
.iris_right:
    mov dl, 1
.iris_draw:
    ; Left iris
    push bx
    push cx
    add bx, 2
    movzx ax, dl
    add bx, ax
    add cx, 4
    mov dx, 1
    mov si, 1
    mov al, CLR_EYE_IRIS
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    ; Right iris
    push bx
    push cx
    add bx, 5
    movzx di, byte [cs:_dg_i]
    mov al, [cs:ghost_dir + di]
    xor dl, dl
    cmp al, DIR_RIGHT
    jne .iris2
    mov dl, 1
.iris2:
    movzx ax, dl
    add bx, ax
    add cx, 4
    mov dx, 1
    mov si, 1
    mov al, CLR_EYE_IRIS
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    jmp .dg_done

.dg_fright_face:
    ; Frightened face: zigzag mouth
    push bx
    push cx
    add bx, 2
    add cx, 5
    mov dx, 5
    mov si, 1
    mov al, CLR_EYE_WHITE
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    jmp .dg_done

.dg_eyes_only:
    ; Eaten: just draw eyes
    push bx
    push cx
    add bx, 2
    add cx, 3
    mov dx, 2
    mov si, 2
    mov al, CLR_EYE_WHITE
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx
    push bx
    push cx
    add bx, 5
    add cx, 3
    mov dx, 2
    mov si, 2
    mov al, CLR_EYE_WHITE
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx
    pop bx

.dg_done:
    popa
    ret

_dg_color: db 0

; draw_entities - Draw all entities
draw_entities:
    pusha
    call draw_pac
    call draw_all_ghosts
    popa
    ret

; draw_all_ghosts - Draw all 4 ghosts
draw_all_ghosts:
    pusha
    mov byte [cs:_dg_i], 0
.dg_loop:
    cmp byte [cs:_dg_i], NUM_GHOSTS
    jae .dg_done_all

    movzx bx, byte [cs:_dg_i]
    cmp byte [cs:ghost_state + bx], GHOST_IN_HOUSE
    je .dg_next

    ; Erase old
    movzx si, byte [cs:_dg_i]
    shl si, 1
    mov bx, [cs:ghost_old_x + si]
    mov cx, [cs:ghost_old_y + si]
    call erase_entity

    ; Draw new
    movzx si, byte [cs:_dg_i]
    shl si, 1
    mov bx, [cs:ghost_x + si]
    mov cx, [cs:ghost_y + si]
    call draw_ghost_vga
    jmp .dg_next_inc

.dg_next:
    ; Still erase in case ghost just entered house
    movzx si, byte [cs:_dg_i]
    shl si, 1
    mov bx, [cs:ghost_old_x + si]
    mov cx, [cs:ghost_old_y + si]
    call erase_entity

.dg_next_inc:
    inc byte [cs:_dg_i]
    jmp .dg_loop
.dg_done_all:
    popa
    ret

_dg_i: db 0

; erase_entity - Redraw tiles under 9x9 entity at BX, CX
erase_entity:
    pusha
    mov ax, bx
    shr ax, 3
    mov [cs:_ee_tx], ax
    mov ax, cx
    shr ax, 3
    mov [cs:_ee_ty], ax

    ; Up to 2x2 tiles
    mov bx, [cs:_ee_tx]
    mov cx, [cs:_ee_ty]
    call draw_tile

    mov bx, [cs:_ee_tx]
    inc bx
    cmp bx, MAZE_COLS
    jae .ee_no_right
    mov cx, [cs:_ee_ty]
    call draw_tile
.ee_no_right:

    mov bx, [cs:_ee_tx]
    mov cx, [cs:_ee_ty]
    inc cx
    cmp cx, MAZE_ROWS
    jae .ee_no_bottom
    call draw_tile
.ee_no_bottom:

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
; redraw_tiles_at_ready
; ============================================================================
redraw_tiles_at_ready:
    pusha
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
; Movement - identical logic to CGA version
; ============================================================================
move_pac_man:
    pusha
    cmp byte [cs:pac_alive], 1
    jne .move_done

    ; At tile center?
    mov ax, [cs:pac_x]
    and ax, 7
    jnz .continue_moving
    mov ax, [cs:pac_y]
    and ax, 7
    jnz .continue_moving

    ; Try next dir
    movzx ax, byte [cs:pac_next_dir]
    call pac_can_move_dir
    jc .try_current
    mov al, [cs:pac_next_dir]
    mov [cs:pac_dir], al
    jmp .do_move
.try_current:
    movzx ax, byte [cs:pac_dir]
    call pac_can_move_dir
    jc .move_done
.do_move:
.continue_moving:
    cmp byte [cs:pac_dir], DIR_UP
    je .m_up
    cmp byte [cs:pac_dir], DIR_DOWN
    je .m_down
    cmp byte [cs:pac_dir], DIR_LEFT
    je .m_left
    add word [cs:pac_x], 1
    cmp word [cs:pac_x], 224
    jb .wrap_done
    mov word [cs:pac_x], 0
    jmp .wrap_done
.m_up:
    sub word [cs:pac_y], 1
    jmp .wrap_done
.m_down:
    add word [cs:pac_y], 1
    jmp .wrap_done
.m_left:
    sub word [cs:pac_x], 1
    cmp word [cs:pac_x], 0xFFFF
    jb .wrap_done
    mov word [cs:pac_x], 223
.wrap_done:
.move_done:
    popa
    ret

pac_can_move_dir:
    push bx
    push cx
    push dx
    mov bx, [cs:pac_x]
    mov cx, [cs:pac_y]
    shr bx, 3
    shr cx, 3
    cmp al, DIR_UP
    je .pcd_up
    cmp al, DIR_DOWN
    je .pcd_down
    cmp al, DIR_LEFT
    je .pcd_left
    inc bx
    cmp bx, MAZE_COLS
    jae .pcd_wrap_r
    jmp .pcd_check
.pcd_wrap_r:
    xor bx, bx
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
    mov bx, MAZE_COLS - 1
.pcd_check:
    cmp cx, MAZE_ROWS
    jae .pcd_blocked
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
; Dot eating
; ============================================================================
check_dot_eat:
    pusha
    mov ax, [cs:pac_x]
    add ax, 3
    shr ax, 3
    mov bx, ax
    mov ax, [cs:pac_y]
    add ax, 3
    shr ax, 3
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
    mov word [cs:fright_timer], FRIGHT_DURATION
    mov byte [cs:fright_kills], 0
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
    mov al, [cs:ghost_dir + bx]
    call reverse_dir
    mov [cs:ghost_dir + bx], al
.fright_next:
    inc cl
    jmp .fright_loop
.fright_done:
    mov byte [cs:snd_type], 2
    mov byte [cs:snd_timer], 4
    mov bx, 1000
    mov ah, API_SPEAKER_TONE
    int 0x80
    call draw_score
    call check_level_complete

.eat_done:
    popa
    ret

check_level_complete:
    mov ax, [cs:dots_eaten]
    cmp ax, [cs:total_dots]
    jne .not_complete
    mov byte [cs:game_state], STATE_LEVELUP
    mov word [cs:levelup_timer], 36
    mov word [cs:fright_timer], 0
.not_complete:
    ret

reverse_dir:
    cmp al, DIR_UP
    je .r_dn
    cmp al, DIR_DOWN
    je .r_up
    cmp al, DIR_LEFT
    je .r_rt
    mov al, DIR_LEFT
    ret
.r_dn: mov al, DIR_DOWN
       ret
.r_up: mov al, DIR_UP
       ret
.r_rt: mov al, DIR_RIGHT
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
; Ghost movement (4 ghosts)
; ============================================================================
move_ghosts:
    pusha
    mov byte [cs:_mg_i], 0
.mg_loop:
    movzx bx, byte [cs:_mg_i]
    cmp bl, NUM_GHOSTS
    jae .mg_done

    mov al, [cs:ghost_state + bx]
    cmp al, GHOST_IN_HOUSE
    je .mg_next                 ; Skip in-house ghosts (release handled separately)

    call move_one_ghost

.mg_next:
    inc byte [cs:_mg_i]
    jmp .mg_loop
.mg_done:
    popa
    ret

_mg_i: db 0

move_one_ghost:
    pusha
    shl bx, 1
    mov ax, [cs:ghost_x + bx]
    shr bx, 1
    and ax, 7
    jnz .gc_move
    shl bx, 1
    mov ax, [cs:ghost_y + bx]
    shr bx, 1
    and ax, 7
    jnz .gc_move
    call ghost_choose_direction
.gc_move:
    mov al, [cs:ghost_dir + bx]
    shl bx, 1
    cmp al, DIR_UP
    je .g_up
    cmp al, DIR_DOWN
    je .g_down
    cmp al, DIR_LEFT
    je .g_left
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
; ghost_choose_direction - AI for ghost BX
; ============================================================================
ghost_choose_direction:
    pusha
    shl bx, 1
    mov ax, [cs:ghost_x + bx]
    shr ax, 3
    mov [cs:_gc_tile_x], ax
    mov ax, [cs:ghost_y + bx]
    shr ax, 3
    mov [cs:_gc_tile_y], ax
    shr bx, 1

    mov al, [cs:ghost_state + bx]
    cmp al, GHOST_FRIGHTENED
    je .gc_random
    cmp al, GHOST_EATEN
    je .gc_target_house
    cmp al, GHOST_SCATTER
    je .gc_scatter
    jmp .gc_chase

.gc_random:
    call random_byte
    and al, 3
    mov [cs:_gc_best_dir], al
    jmp .gc_apply

.gc_target_house:
    mov word [cs:_gc_target_x], 14
    mov word [cs:_gc_target_y], 10
    jmp .gc_find_best

.gc_scatter:
    cmp bl, 0
    je .gc_s0
    cmp bl, 1
    je .gc_s1
    cmp bl, 2
    je .gc_s2
    mov word [cs:_gc_target_x], 1
    mov word [cs:_gc_target_y], 23
    jmp .gc_find_best
.gc_s0:
    mov word [cs:_gc_target_x], 26
    mov word [cs:_gc_target_y], 1
    jmp .gc_find_best
.gc_s1:
    mov word [cs:_gc_target_x], 1
    mov word [cs:_gc_target_y], 1
    jmp .gc_find_best
.gc_s2:
    mov word [cs:_gc_target_x], 26
    mov word [cs:_gc_target_y], 23
    jmp .gc_find_best

.gc_chase:
    cmp bl, 0
    je .gc_c0
    cmp bl, 1
    je .gc_c1
    cmp bl, 2
    je .gc_c2
    ; Ghost 3 (Clyde): chase if far, scatter if close
    mov ax, [cs:pac_x]
    shr ax, 3
    sub ax, [cs:_gc_tile_x]
    test ax, ax
    jns .gc_c3_px
    neg ax
.gc_c3_px:
    mov cx, ax
    mov ax, [cs:pac_y]
    shr ax, 3
    sub ax, [cs:_gc_tile_y]
    test ax, ax
    jns .gc_c3_py
    neg ax
.gc_c3_py:
    add cx, ax
    cmp cx, 8
    jbe .gc_s2                      ; Close: scatter
    ; Fall through to chase directly

.gc_c0:
    ; Blinky: direct chase
    mov ax, [cs:pac_x]
    shr ax, 3
    mov [cs:_gc_target_x], ax
    mov ax, [cs:pac_y]
    shr ax, 3
    mov [cs:_gc_target_y], ax
    jmp .gc_find_best

.gc_c1:
    ; Pinky: 4 tiles ahead
    mov ax, [cs:pac_x]
    shr ax, 3
    mov cx, ax
    mov ax, [cs:pac_y]
    shr ax, 3
    mov dx, ax
    cmp byte [cs:pac_dir], DIR_UP
    je .gc_c1_up
    cmp byte [cs:pac_dir], DIR_DOWN
    je .gc_c1_dn
    cmp byte [cs:pac_dir], DIR_LEFT
    je .gc_c1_lt
    add cx, 4
    jmp .gc_c1_set
.gc_c1_up: sub dx, 4
           jmp .gc_c1_set
.gc_c1_dn: add dx, 4
           jmp .gc_c1_set
.gc_c1_lt: sub cx, 4
.gc_c1_set:
    mov [cs:_gc_target_x], cx
    mov [cs:_gc_target_y], dx
    jmp .gc_find_best

.gc_c2:
    ; Inky: 2*(2 tiles ahead) - Blinky position
    mov ax, [cs:pac_x]
    shr ax, 3
    mov cx, ax
    mov ax, [cs:pac_y]
    shr ax, 3
    mov dx, ax
    cmp byte [cs:pac_dir], DIR_UP
    je .gc_c2_up
    cmp byte [cs:pac_dir], DIR_DOWN
    je .gc_c2_dn
    cmp byte [cs:pac_dir], DIR_LEFT
    je .gc_c2_lt
    add cx, 2
    jmp .gc_c2_calc
.gc_c2_up: sub dx, 2
           jmp .gc_c2_calc
.gc_c2_dn: add dx, 2
           jmp .gc_c2_calc
.gc_c2_lt: sub cx, 2
.gc_c2_calc:
    ; target = 2*ahead - blinky_pos
    shl cx, 1
    shl dx, 1
    mov ax, [cs:ghost_x]           ; Blinky x (ghost 0)
    shr ax, 3
    sub cx, ax
    mov ax, [cs:ghost_y]
    shr ax, 3
    sub dx, ax
    mov [cs:_gc_target_x], cx
    mov [cs:_gc_target_y], dx
    jmp .gc_find_best

.gc_find_best:
    mov word [cs:_gc_best_dist], 0xFFFF
    mov byte [cs:_gc_best_dir], DIR_UP
    ; Reverse of current direction
    movzx bx, byte [cs:_mg_i]
    mov al, [cs:ghost_dir + bx]
    call reverse_dir
    mov [cs:_gc_reverse], al

    mov al, DIR_UP
    call .gc_try_dir
    mov al, DIR_LEFT
    call .gc_try_dir
    mov al, DIR_DOWN
    call .gc_try_dir
    mov al, DIR_RIGHT
    call .gc_try_dir
    jmp .gc_apply

.gc_try_dir:
    push bx
    push cx
    push dx
    cmp al, [cs:_gc_reverse]
    je .gc_skip
    mov cx, [cs:_gc_tile_x]
    mov dx, [cs:_gc_tile_y]
    cmp al, DIR_UP
    je .gc_t_up
    cmp al, DIR_DOWN
    je .gc_t_dn
    cmp al, DIR_LEFT
    je .gc_t_lt
    inc cx
    cmp cx, MAZE_COLS
    jb .gc_t_chk
    xor cx, cx
    jmp .gc_t_chk
.gc_t_up:
    dec dx
    cmp dx, 0xFFFF
    jne .gc_t_chk
    jmp .gc_skip
.gc_t_dn:
    inc dx
    cmp dx, MAZE_ROWS
    jb .gc_t_chk
    jmp .gc_skip
.gc_t_lt:
    dec cx
    cmp cx, 0xFFFF
    jne .gc_t_chk
    mov cx, MAZE_COLS - 1
.gc_t_chk:
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
    je .gc_skip_pop
    cmp ah, TILE_GATE
    jne .gc_t_pass
    movzx bx, byte [cs:_mg_i]
    cmp byte [cs:ghost_state + bx], GHOST_EATEN
    jne .gc_skip_pop
.gc_t_pass:
    pop ax
    push ax
    mov bx, cx
    sub bx, [cs:_gc_target_x]
    test bx, bx
    jns .gc_t_px
    neg bx
.gc_t_px:
    mov cx, dx
    sub cx, [cs:_gc_target_y]
    test cx, cx
    jns .gc_t_py
    neg cx
.gc_t_py:
    add bx, cx
    cmp bx, [cs:_gc_best_dist]
    jae .gc_skip_pop
    mov [cs:_gc_best_dist], bx
    pop ax
    mov [cs:_gc_best_dir], al
    jmp .gc_skip
.gc_skip_pop:
    pop ax
.gc_skip:
    pop dx
    pop cx
    pop bx
    ret

.gc_apply:
    movzx bx, byte [cs:_mg_i]
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
; Ghost collision
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
    mov ax, [cs:pac_x]
    sub ax, [cs:ghost_x + bx]
    test ax, ax
    jns .col_px
    neg ax
.col_px:
    cmp ax, 6
    jae .col_next_shr
    mov dx, ax
    mov ax, [cs:pac_y]
    sub ax, [cs:ghost_y + bx]
    test ax, ax
    jns .col_py
    neg ax
.col_py:
    cmp ax, 6
    jae .col_next_shr
    shr bx, 1
    mov al, [cs:ghost_state + bx]
    cmp al, GHOST_FRIGHTENED
    je .eat_ghost
    ; Die
    mov byte [cs:pac_alive], 0
    mov byte [cs:game_state], STATE_DEATH
    mov word [cs:death_timer], 27
    mov byte [cs:snd_type], 3
    mov byte [cs:snd_timer], 18
    mov bx, 500
    mov ah, API_SPEAKER_TONE
    int 0x80
    jmp .col_done
.eat_ghost:
    mov byte [cs:ghost_state + bx], GHOST_EATEN
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
    mov ax, [cs:score]
    cmp ax, [cs:high_score]
    jbe .no_hi
    mov [cs:high_score], ax
    call draw_high_score
.no_hi:
    popa
    ret

; ============================================================================
; Timers and animation
; ============================================================================
update_fright_timer:
    cmp word [cs:fright_timer], 0
    je .ft_done
    dec word [cs:fright_timer]
    cmp word [cs:fright_timer], 0
    jne .ft_done
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
    je .ft_scat
    mov byte [cs:ghost_state + bx], GHOST_CHASE
    jmp .ft_next
.ft_scat:
    mov byte [cs:ghost_state + bx], GHOST_SCATTER
.ft_next:
    inc cl
    jmp .ft_loop
.ft_end:
    popa
.ft_done:
    ret

update_mode_timer:
    cmp word [cs:fright_timer], 0
    jne .mt_done
    dec word [cs:mode_timer]
    cmp word [cs:mode_timer], 0
    jne .mt_done
    pusha
    inc byte [cs:mode_index]
    xor byte [cs:mode_is_chase], 1
    movzx bx, byte [cs:mode_index]
    shl bx, 1
    mov ax, [cs:mode_schedule + bx]
    mov [cs:mode_timer], ax
    mov cl, 0
.mt_rev:
    cmp cl, NUM_GHOSTS
    jae .mt_end
    movzx bx, cl
    cmp byte [cs:ghost_state + bx], GHOST_FRIGHTENED
    je .mt_rnext
    cmp byte [cs:ghost_state + bx], GHOST_EATEN
    je .mt_rnext
    cmp byte [cs:ghost_state + bx], GHOST_IN_HOUSE
    je .mt_rnext
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
.mt_rnext:
    inc cl
    jmp .mt_rev
.mt_end:
    popa
.mt_done:
    ret

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

update_power_flash:
    inc byte [cs:power_flash_tick]
    cmp byte [cs:power_flash_tick], 8
    jb .pf_done
    mov byte [cs:power_flash_tick], 0
    xor byte [cs:power_flash], 1
    pusha
    mov si, maze_data
    xor cx, cx
.pf_loop:
    cmp cx, MAZE_COLS * MAZE_ROWS
    jae .pf_end
    cmp byte [cs:si], TILE_POWER
    jne .pf_next
    push cx
    mov ax, cx
    xor dx, dx
    mov bx, MAZE_COLS
    div bx
    mov bx, dx
    mov cx, ax
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

update_sound:
    cmp byte [cs:snd_timer], 0
    je .us_done
    dec byte [cs:snd_timer]
    cmp byte [cs:snd_type], 1
    je .us_chomp
    cmp byte [cs:snd_type], 2
    je .us_power
    cmp byte [cs:snd_type], 3
    je .us_death
    cmp byte [cs:snd_type], 4
    je .us_eat
    jmp .us_end
.us_chomp:
    test byte [cs:snd_timer], 1
    jz .us_end
    pusha
    mov bx, 880
    mov ah, API_SPEAKER_TONE
    int 0x80
    popa
    jmp .us_end
.us_power:
    pusha
    movzx ax, byte [cs:snd_timer]
    mov bx, 125
    mul bx
    add ax, 400
    mov bx, ax
    mov ah, API_SPEAKER_TONE
    int 0x80
    popa
    jmp .us_end
.us_death:
    pusha
    movzx ax, byte [cs:snd_timer]
    mov bx, 25
    mul bx
    add ax, 100
    mov bx, ax
    mov ah, API_SPEAKER_TONE
    int 0x80
    popa
    jmp .us_end
.us_eat:
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
.us_end:
    cmp byte [cs:snd_timer], 0
    jne .us_done
    pusha
    mov ah, API_SPEAKER_OFF
    int 0x80
    popa
.us_done:
    ret

random_byte:
    push bx
    push cx
    mov ax, [cs:rng_seed]
    mov bx, 25173
    mul bx
    add ax, 13849
    mov [cs:rng_seed], ax
    mov al, ah
    pop cx
    pop bx
    ret

; ============================================================================
; VGA Palette data (64 entries x 3 bytes = 192 bytes)
; ============================================================================
vga_palette:
    ; 16-19: Pac-Man yellow
    db 32, 28,  0,  50, 45,  0,  63, 58,  0,  63, 63, 32
    ; 20-23: Blinky red
    db 28,  0,  0,  50,  4,  4,  63, 10, 10,  63, 32, 32
    ; 24-27: Pinky pink
    db 40, 12, 28,  56, 24, 44,  63, 40, 56,  63, 52, 63
    ; 28-31: Inky cyan
    db  0, 24, 32,   0, 40, 50,   8, 56, 63,  32, 63, 63
    ; 32-35: Clyde orange
    db 36, 20,  0,  52, 32,  0,  63, 44,  0,  63, 56, 24
    ; 36-39: Frightened blue
    db  0,  0, 24,   8,  8, 42,  16, 16, 56,  32, 32, 63
    ; 40-43: Maze wall
    db  0,  0, 20,   0,  4, 36,   4, 10, 48,  12, 20, 63
    ; 44-45: Wall bevel
    db  8, 16, 56,   0,  0, 12
    ; 46-47: Gate
    db 48, 16, 40,  63, 24, 56
    ; 48-49: Dots
    db 48, 44, 36,  63, 60, 52
    ; 50-51: Power pellet
    db 63, 52,  0,  63, 63, 48
    ; 52-55: Fruit
    db 48,  0,  0,  63, 12, 12,  56, 20, 36,  63, 40, 52
    ; 56-59: Score text
    db 28, 16,  0,  42, 28,  0,  56, 40,  0,  63, 52, 12
    ; 60-63: HUD navy
    db  0,  0,  8,   0,  0, 12,   0,  2, 16,   2,  4, 20
    ; 64-71: Splash rainbow
    db 63,  0,  0,  63, 32,  0,  63, 63,  0,   0, 63,  0
    db  0, 63, 63,   0,  0, 63,  32,  0, 63,  63,  0, 48
    ; 72-73: Sub-text gold
    db 48, 36,  0,  63, 50, 12
    ; 74-75: Eyes
    db 63, 63, 63,  16, 16, 56
    ; 76-77: Fright flash
    db 63, 63, 63,  16, 16, 56
    ; 78-79: Level flash
    db 63, 63, 63,   0,  0,  0

; Ghost base color table (per ghost index)
ghost_color_table: db CLR_BLINKY, CLR_PINKY, CLR_INKY, CLR_CLYDE

; ============================================================================
; Maze template (28 x 25 = 700 bytes) - same as CGA version
; ============================================================================
maze_template:
    db 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
    db 1,3,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,3,1
    db 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
    db 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
    db 1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1
    db 1,2,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,2,1
    db 1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1
    db 1,1,1,1,1,2,1,1,1,1,0,1,0,0,0,0,1,0,1,1,1,1,2,1,1,1,1,1
    db 0,0,0,0,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0
    db 0,0,0,0,1,2,1,0,1,1,1,1,5,0,0,5,1,1,1,1,0,1,2,1,0,0,0,0
    db 1,1,1,1,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,1,1,1,1
    db 0,0,0,0,0,2,0,0,1,4,4,4,4,4,4,4,4,4,4,1,0,0,2,0,0,0,0,0
    db 0,0,0,0,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,0,0,0,0
    db 0,0,0,0,1,2,1,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1,2,1,0,0,0,0
    db 1,1,1,1,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,1,1,1,1
    db 1,2,2,2,2,2,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,2,2,2,2,2,1
    db 1,2,1,1,1,2,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,2,1,1,1,2,1
    db 1,3,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,3,1
    db 1,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,1
    db 1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1
    db 1,2,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,2,1
    db 1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1
    db 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
    db 1,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,1
    db 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1

maze_data:
    times MAZE_COLS * MAZE_ROWS db 0

mode_schedule:
    dw 127, 364, 127, 364, 91, 364, 91, 0xFFFF

; ============================================================================
; Game state
; ============================================================================
saved_video_mode: db 0x04
fs_win_handle:  db 0xFF
fs_win_title:   db 0
saved_text_clr: db 0
saved_bg_clr:   db 0
saved_win_clr:  db 0
quit_flag:      db 0
game_state:     db STATE_TITLE
last_tick:      dw 0
rng_seed:       dw 0
move_steps:     db 0

score:          dw 0
high_score:     dw 0
lives:          db 3
level:          db 1
total_dots:     dw 0
dots_eaten:     dw 0

pac_x:          dw 112
pac_y:          dw 152
pac_old_x:      dw 112
pac_old_y:      dw 152
pac_dir:        db DIR_RIGHT
pac_next_dir:   db DIR_RIGHT
pac_anim:       db 0
pac_anim_tick:  db 0
pac_alive:      db 1

; 4 ghosts
ghost_x:        dw 112, 104, 112, 120
ghost_y:        dw 80, 96, 96, 96
ghost_old_x:    dw 112, 104, 112, 120
ghost_old_y:    dw 80, 96, 96, 96
ghost_dir:      db 0, 0, 0, 0
ghost_state:    db GHOST_SCATTER, GHOST_IN_HOUSE, GHOST_IN_HOUSE, GHOST_IN_HOUSE
ghost_timer:    dw 0, 54, 90, 127

fright_timer:   dw 0
fright_kills:   db 0
mode_index:     db 0
mode_timer:     dw 127
mode_is_chase:  db 0

ready_timer:    dw 36
death_timer:    dw 0
levelup_timer:  dw 0

snd_type:       db 0
snd_timer:      db 0
power_flash:    db 0
power_flash_tick: db 0

splash_timer:     dw 0
splash_anim_x:    dw 0
splash_last_tick: dw 0

score_buf:      times 8 db 0

; Strings
str_title:       db 'PAC-MAN', 0
str_press_key:   db 'Press any key', 0
str_ready:       db 'READY!', 0
str_gameover:    db 'GAME  OVER', 0
str_score_label: db 'SCORE', 0
str_hi_label:    db 'HIGH', 0
str_lives_label: db 'LIVES', 0
str_level_label: db 'LEVEL', 0
str_subtitle:    db 'A UnoDOS Game', 0
str_version:     db 'v1.0  VGA', 0
str_credit:      db 'UnoDOS  2026', 0
