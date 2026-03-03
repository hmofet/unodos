; ============================================================================
; OUTLASTV.BIN - VGA Pseudo-3D Racing Game for UnoDOS
; Fullscreen game with 256-color VGA graphics, sky gradient,
; multi-color car, rumble strips. Supports all VGA modes.
; ============================================================================

[ORG 0x0000]

; --- BIN Header (80 bytes) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic
    db 'OutLast VGA', 0            ; App name (12 bytes padded)
    times (0x04 + 12) - ($ - $$) db 0

; 16x16 icon bitmap (64 bytes, 2bpp CGA)
; Road with car icon (same as CGA version)
    db 0x00, 0x00, 0x00, 0x00      ; Row 0
    db 0x15, 0x55, 0x55, 0x54      ; Row 1:  cyan horizon
    db 0x15, 0x55, 0x55, 0x54      ; Row 2
    db 0x00, 0xFF, 0xF0, 0x00      ; Row 3:  white road
    db 0x00, 0xFF, 0xF0, 0x00      ; Row 4
    db 0x01, 0xFF, 0xFC, 0x00      ; Row 5
    db 0x01, 0xFF, 0xFC, 0x00      ; Row 6
    db 0x03, 0xFF, 0xFF, 0x00      ; Row 7
    db 0x07, 0xFF, 0xFF, 0xC0      ; Row 8
    db 0x0F, 0xFF, 0xFF, 0xC0      ; Row 9
    db 0x0F, 0xFF, 0xFF, 0xF0      ; Row 10
    db 0x00, 0xAA, 0xA8, 0x00      ; Row 11: magenta car
    db 0x00, 0xAA, 0xA8, 0x00      ; Row 12
    db 0x00, 0xFF, 0xF0, 0x00      ; Row 13: white wheels
    db 0x00, 0x00, 0x00, 0x00      ; Row 14
    db 0x00, 0x00, 0x00, 0x00      ; Row 15

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

    ; Save current video mode for restore on exit (kernel API, handles SVGA)
    mov ah, API_GET_VIDEO_MODE
    int 0x80
    mov [cs:saved_video_mode], al

    ; Switch to VGA mode 13h (320x200, 256 color)
    mov al, 0x13
    mov ah, API_SET_VIDEO_MODE
    int 0x80

    ; Hide mouse cursor during game
    mov al, 0
    mov ah, API_MOUSE_SET_VISIBLE
    int 0x80

    ; Create fullscreen frameless window
    xor bx, bx
    xor cx, cx
    mov dx, 320
    mov si, 200
    mov ax, cs
    mov es, ax
    mov di, fs_win_title
    mov al, 0x04                    ; Frameless
    mov ah, API_WIN_CREATE
    int 0x80
    jc .no_fs_win
    mov [cs:fs_win_handle], al
.no_fs_win:

    ; Save current theme colors
    mov ah, API_THEME_GET_COLORS
    int 0x80
    mov [cs:saved_text_clr], al
    mov [cs:saved_bg_clr], bl
    mov [cs:saved_win_clr], cl

    ; Store screen dimensions (320x200 for mode 13h)
    mov word [cs:scr_w], 320
    mov word [cs:scr_h], 200

    ; Set custom VGA palette
    call setup_palette

    ; Initialize RNG seed from BIOS tick
    mov ah, API_GET_TICK
    int 0x80
    mov [cs:rng_seed], ax

    ; Set white text on black background for VGA
    mov al, 15                      ; text color = white (VGA palette 15)
    mov bl, 0                       ; Black bg
    mov cl, 15                      ; White win
    mov ah, API_THEME_SET_COLORS
    int 0x80

    ; Clear screen
    mov bx, 0
    mov cx, 0
    mov dx, [cs:scr_w]
    mov si, [cs:scr_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw title screen
    call draw_title
    mov byte [cs:game_state], STATE_TITLE

    ; Start title theme music
    mov si, song_title
    call start_song

; ============================================================================
; Main loop
; ============================================================================
.main_loop:
    cmp byte [cs:quit_flag], 1
    je .exit_game

    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; --- Check events ---
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_KEY_PRESS
    jne .no_event

    cmp dl, 27                      ; ESC
    je .set_quit

    cmp byte [cs:game_state], STATE_TITLE
    je .title_key
    cmp byte [cs:game_state], STATE_PLAYING
    je .game_key
    cmp byte [cs:game_state], STATE_GAMEOVER
    je .gameover_key
    jmp .no_event

.title_key:
    call init_game
    jmp .no_event

.gameover_key:
    mov bx, 0
    mov cx, 0
    mov dx, [cs:scr_w]
    mov si, [cs:scr_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    call draw_title
    mov byte [cs:game_state], STATE_TITLE
    mov si, song_title
    call start_song
    jmp .no_event

.game_key:
    ; Block steering/accel/brake when crashed
    cmp byte [cs:crash_timer], 0
    jne .no_event
    cmp dl, 130                     ; Left
    je .steer_left
    cmp dl, 131                     ; Right
    je .steer_right
    cmp dl, 128                     ; Up = accelerate
    je .accelerate
    cmp dl, 129                     ; Down = brake
    je .brake
    jmp .no_event

.steer_left:
    sub word [cs:player_x], 3
    ; Clamp to left edge
    mov ax, [cs:scr_w]
    shr ax, 3                       ; min = scr_w / 8
    cmp [cs:player_x], ax
    jge .no_event
    mov [cs:player_x], ax
    jmp .no_event

.steer_right:
    add word [cs:player_x], 3
    ; Clamp to right edge
    mov ax, [cs:scr_w]
    mov bx, ax
    shr bx, 3
    sub ax, bx                      ; max = scr_w - scr_w/8
    cmp [cs:player_x], ax
    jle .no_event
    mov [cs:player_x], ax
    jmp .no_event

.accelerate:
    ; UP = turbo boost (car auto-accelerates, UP is extra)
    cmp word [cs:player_speed], MAX_SPEED
    jge .no_event
    add word [cs:player_speed], 4
    cmp word [cs:player_speed], MAX_SPEED
    jle .no_event
    mov word [cs:player_speed], MAX_SPEED
    jmp .no_event

.brake:
    sub word [cs:player_speed], 8
    cmp word [cs:player_speed], 0
    jge .no_event
    mov word [cs:player_speed], 0
    jmp .no_event

.set_quit:
    mov byte [cs:quit_flag], 1
    jmp .no_event

.no_event:
    ; Tick-based updates (all states: music, game logic)
    mov ah, API_GET_TICK
    int 0x80
    cmp ax, [cs:last_tick]
    je .main_loop               ; Same tick, skip
    mov [cs:last_tick], ax

    ; Music plays in all states
    call play_music_tick

    ; Game logic only during gameplay
    cmp byte [cs:game_state], STATE_PLAYING
    jne .main_loop

    ; Crash recovery: decrement timer, force speed=0
    cmp byte [cs:crash_timer], 0
    je .not_crashed
    dec byte [cs:crash_timer]
    mov word [cs:player_speed], 0
    ; When crash ends, center car on road
    cmp byte [cs:crash_timer], 0
    jne .still_crashed
    mov ax, [cs:road_left_at_car]
    add ax, [cs:road_right_at_car]
    shr ax, 1
    mov [cs:player_x], ax
.still_crashed:
    jmp .not_on_grass                ; Skip auto-accel and grass check
.not_crashed:

    ; Auto-accelerate (car speeds up on its own)
    cmp word [cs:player_speed], MAX_SPEED
    jge .at_max
    inc word [cs:player_speed]
.at_max:

    ; Grass penalty: slow down if car is off the road
    mov ax, [cs:player_x]
    cmp ax, [cs:road_left_at_car]
    jl .on_grass
    cmp ax, [cs:road_right_at_car]
    jg .on_grass
    jmp .not_on_grass
.on_grass:
    ; Slowdown on grass (gentle)
    cmp word [cs:player_speed], 8
    jle .grass_min
    dec word [cs:player_speed]
    jmp .not_on_grass
.grass_min:
    mov word [cs:player_speed], 5
.not_on_grass:

    ; Curve drift: road curves push the car sideways
    ; Without steering input, car drifts off road in curves
    call update_curve
    mov ax, [cs:current_curve]
    sar ax, 3                        ; Drift = curve / 8 pixels per frame (gentle)
    sub [cs:player_x], ax            ; Subtract: positive curve = right turn = car drifts left

    ; Advance camera
    mov ax, [cs:player_speed]
    shr ax, 1
    add [cs:camera_z], ax

    ; Move traffic cars
    call update_traffic

    ; Update score (only when on road and not crashed)
    cmp byte [cs:crash_timer], 0
    jne .skip_score
    mov ax, [cs:player_x]
    cmp ax, [cs:road_left_at_car]
    jl .skip_score
    cmp ax, [cs:road_right_at_car]
    jg .skip_score
    mov ax, [cs:player_speed]
    shr ax, 2
    add [cs:score], ax
.skip_score:

    ; Timer
    inc word [cs:timer_counter]
    cmp word [cs:timer_counter], 18
    jb .no_timer_dec
    mov word [cs:timer_counter], 0
    cmp word [cs:time_left], 0
    je .game_over_trigger
    dec word [cs:time_left]
.no_timer_dec:

    ; Clamp player
    mov ax, [cs:scr_w]
    shr ax, 3
    cmp [cs:player_x], ax
    jge .clamp_right
    mov [cs:player_x], ax
.clamp_right:
    mov ax, [cs:scr_w]
    mov bx, ax
    shr bx, 3
    sub ax, bx
    cmp [cs:player_x], ax
    jle .render_frame
    mov [cs:player_x], ax

.render_frame:
    call draw_sky
    call draw_road
    call draw_obstacles
    call draw_traffic
    call draw_car
    call draw_hud
    call check_obstacle_collision
    call check_traffic_collision
    jmp .main_loop

.game_over_trigger:
    mov byte [cs:game_state], STATE_GAMEOVER
    call stop_music
    call draw_game_over
    jmp .main_loop

.exit_game:
    call stop_music

    ; Destroy fullscreen window
    cmp byte [cs:fs_win_handle], 0
    je .no_destroy
    mov al, [cs:fs_win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80
.no_destroy:

    ; Restore theme colors
    mov al, [cs:saved_text_clr]
    mov bl, [cs:saved_bg_clr]
    mov cl, [cs:saved_win_clr]
    mov ah, API_THEME_SET_COLORS
    int 0x80

    ; Restore mouse cursor before exit
    mov al, 1
    mov ah, API_MOUSE_SET_VISIBLE
    int 0x80

    ; Restore original video mode (handles CGA/VGA/SVGA)
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
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_APP_YIELD           equ 34
API_THEME_SET_COLORS    equ 54
API_THEME_GET_COLORS    equ 55
API_GET_TICK            equ 63
API_FILLED_RECT_COLOR   equ 67
API_DRAW_HLINE          equ 69
API_WORD_TO_STRING      equ 91
API_SPEAKER_TONE        equ 41
API_SPEAKER_OFF         equ 42
API_SET_VIDEO_MODE      equ 95
API_GET_VIDEO_MODE      equ 100
API_MOUSE_SET_VISIBLE   equ 101

EVENT_KEY_PRESS         equ 1

STATE_TITLE             equ 0
STATE_PLAYING           equ 1
STATE_GAMEOVER          equ 2

MAX_SPEED               equ 60
ROAD_BASE_HW            equ 16      ; Road half-width at screen bottom
CAR_W                   equ 40
CAR_H                   equ 24
TRACK_SEGMENTS          equ 32
SEGMENT_LENGTH          equ 80      ; Longer segments = longer sustained curves

; VGA palette color indices
CLR_SKY_TOP             equ 16
CLR_SKY_BOT             equ 23
CLR_ROAD_DARK           equ 24
CLR_ROAD_LIGHT          equ 25
CLR_ROAD_MARK           equ 26
CLR_GRASS_LIGHT         equ 27
CLR_GRASS_DARK          equ 28
CLR_RUMBLE_RED          equ 29
CLR_RUMBLE_WHITE        equ 30
CLR_CAR_BODY            equ 31
CLR_CAR_DARK            equ 32
CLR_CAR_WIND            equ 33
CLR_CAR_WHEEL           equ 34
CLR_HUD_BG              equ 35

; Title screen palette indices
CLR_TITLE_SKY0          equ 36
CLR_TITLE_SKY1          equ 37
CLR_TITLE_SKY2          equ 38
CLR_TITLE_SKY3          equ 39
CLR_TITLE_SKY4          equ 40
CLR_TITLE_SKY5          equ 41
CLR_TITLE_GOLD          equ 42
CLR_TITLE_SHADOW        equ 43

; Title bitmap font constants
TITLE_BLK_W             equ 6
TITLE_BLK_H             equ 5
TITLE_GAP               equ 4
TITLE_FONT_ROWS         equ 7
TITLE_START_X           equ 43
TITLE_START_Y           equ 15

; Note frequencies (Hz)
NOTE_REST               equ 0
NOTE_C3                 equ 131
NOTE_D3                 equ 147
NOTE_E3                 equ 165
NOTE_F3                 equ 175
NOTE_G3                 equ 196
NOTE_A3                 equ 220
NOTE_B3                 equ 247
NOTE_C4                 equ 262
NOTE_D4                 equ 294
NOTE_E4                 equ 330
NOTE_F4                 equ 349
NOTE_G4                 equ 392
NOTE_A4                 equ 440
NOTE_B4                 equ 494
NOTE_C5                 equ 523
NOTE_D5                 equ 587
NOTE_E5                 equ 659
NOTE_G5                 equ 784

; Durations (BIOS ticks, ~55ms each)
DUR_16TH                equ 2
DUR_8TH                 equ 3
DUR_QUARTER             equ 6
DUR_DOT_Q               equ 9
DUR_HALF                equ 12
DUR_WHOLE               equ 24

; ============================================================================
; setup_palette - Set custom VGA palette colors via BIOS
; ============================================================================
setup_palette:
    pusha

    ; Use INT 10h AX=1010h to set individual palette entries
    ; BX = color index, CH = green, CL = blue, DH = red (0-63 VGA range)

    ; 16-23: Sky gradient (dark blue to light blue)
    mov bx, CLR_SKY_TOP            ; index 16
    mov dh, 0                       ; red
    mov ch, 0                       ; green
    mov cl, 15                      ; blue
    call set_pal_entry
    inc bx
    mov cl, 18
    call set_pal_entry
    inc bx
    mov cl, 22
    call set_pal_entry
    inc bx
    mov ch, 5
    mov cl, 26
    call set_pal_entry
    inc bx
    mov ch, 10
    mov cl, 30
    call set_pal_entry
    inc bx
    mov ch, 15
    mov cl, 35
    call set_pal_entry
    inc bx
    mov ch, 20
    mov cl, 40
    call set_pal_entry
    inc bx
    mov ch, 28
    mov cl, 45
    call set_pal_entry

    ; 24: Road dark gray
    mov bx, CLR_ROAD_DARK
    mov dh, 20
    mov ch, 20
    mov cl, 20
    call set_pal_entry

    ; 25: Road light gray
    mov bx, CLR_ROAD_LIGHT
    mov dh, 30
    mov ch, 30
    mov cl, 30
    call set_pal_entry

    ; 26: Road marking white
    mov bx, CLR_ROAD_MARK
    mov dh, 63
    mov ch, 63
    mov cl, 63
    call set_pal_entry

    ; 27: Grass light green
    mov bx, CLR_GRASS_LIGHT
    mov dh, 10
    mov ch, 40
    mov cl, 5
    call set_pal_entry

    ; 28: Grass dark green
    mov bx, CLR_GRASS_DARK
    mov dh, 5
    mov ch, 25
    mov cl, 3
    call set_pal_entry

    ; 29: Rumble strip red
    mov bx, CLR_RUMBLE_RED
    mov dh, 50
    mov ch, 10
    mov cl, 5
    call set_pal_entry

    ; 30: Rumble strip white
    mov bx, CLR_RUMBLE_WHITE
    mov dh, 63
    mov ch, 63
    mov cl, 63
    call set_pal_entry

    ; 31: Car body red
    mov bx, CLR_CAR_BODY
    mov dh, 55
    mov ch, 5
    mov cl, 5
    call set_pal_entry

    ; 32: Car body dark
    mov bx, CLR_CAR_DARK
    mov dh, 35
    mov ch, 3
    mov cl, 3
    call set_pal_entry

    ; 33: Car windshield blue
    mov bx, CLR_CAR_WIND
    mov dh, 15
    mov ch, 25
    mov cl, 50
    call set_pal_entry

    ; 34: Car wheels black
    mov bx, CLR_CAR_WHEEL
    mov dh, 5
    mov ch, 5
    mov cl, 5
    call set_pal_entry

    ; 35: HUD background
    mov bx, CLR_HUD_BG
    mov dh, 3
    mov ch, 3
    mov cl, 8
    call set_pal_entry

    ; Title screen sunset colors (36-43)
    ; 36: Deep navy
    mov bx, CLR_TITLE_SKY0
    mov dh, 0
    mov ch, 0
    mov cl, 12
    call set_pal_entry

    ; 37: Dark purple
    mov bx, CLR_TITLE_SKY1
    mov dh, 8
    mov ch, 0
    mov cl, 20
    call set_pal_entry

    ; 38: Purple-red
    mov bx, CLR_TITLE_SKY2
    mov dh, 25
    mov ch, 2
    mov cl, 15
    call set_pal_entry

    ; 39: Red-orange
    mov bx, CLR_TITLE_SKY3
    mov dh, 45
    mov ch, 8
    mov cl, 5
    call set_pal_entry

    ; 40: Orange
    mov bx, CLR_TITLE_SKY4
    mov dh, 60
    mov ch, 30
    mov cl, 5
    call set_pal_entry

    ; 41: Yellow-orange (horizon glow)
    mov bx, CLR_TITLE_SKY5
    mov dh, 63
    mov ch, 50
    mov cl, 10
    call set_pal_entry

    ; 42: Gold (title text)
    mov bx, CLR_TITLE_GOLD
    mov dh, 63
    mov ch, 55
    mov cl, 10
    call set_pal_entry

    ; 43: Dark brown (title shadow)
    mov bx, CLR_TITLE_SHADOW
    mov dh, 15
    mov ch, 5
    mov cl, 0
    call set_pal_entry

    popa
    ret

set_pal_entry:
    ; BX = index, DH = red, CH = green, CL = blue (all 0-63)
    push ax
    mov ax, 0x1010
    int 0x10
    pop ax
    ret

; ============================================================================
; init_game - Reset game state
; ============================================================================
init_game:
    pusha

    ; Center player
    mov ax, [cs:scr_w]
    shr ax, 1
    mov [cs:player_x], ax
    mov word [cs:player_speed], 0
    mov word [cs:camera_z], 0
    mov word [cs:score], 0
    mov word [cs:time_left], 60
    mov word [cs:timer_counter], 0
    mov byte [cs:decel_counter], 0
    mov word [cs:current_curve], 0
    mov byte [cs:crash_timer], 0
    ; Reset traffic positions
    mov word [cs:traffic_z], 400
    mov word [cs:traffic_z + 2], 1600
    mov word [cs:traffic_z + 4], 800
    mov word [cs:traffic_z + 6], 2000
    mov byte [cs:game_state], STATE_PLAYING

    ; Initialize road edges at car Y to prevent false grass detection
    mov ax, [cs:scr_w]
    shr ax, 1
    sub ax, 100                      ; Approximate road left
    mov [cs:road_left_at_car], ax
    add ax, 200                      ; Approximate road right
    mov [cs:road_right_at_car], ax

    mov ah, API_GET_TICK
    int 0x80
    mov [cs:last_tick], ax

    ; Compute horizon_y = scr_h * 2 / 5 (40% from top)
    mov ax, [cs:scr_h]
    shl ax, 1
    xor dx, dx
    mov bx, 5
    div bx
    mov [cs:horizon_y], ax

    ; Compute car_y = scr_h - 25
    mov ax, [cs:scr_h]
    sub ax, 25
    mov [cs:car_y], ax

    ; Clear screen
    mov bx, 0
    mov cx, 0
    mov dx, [cs:scr_w]
    mov si, [cs:scr_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Start gameplay song (cycles through 3 songs)
    movzx bx, byte [cs:game_song_idx]
    shl bx, 1
    mov si, [cs:song_table + bx]
    call start_song
    inc byte [cs:game_song_idx]
    cmp byte [cs:game_song_idx], 3
    jb .song_ok
    mov byte [cs:game_song_idx], 0
.song_ok:

    popa
    ret

; ============================================================================
; update_curve - Get current curve from track data
; ============================================================================
update_curve:
    pusha
    mov ax, [cs:camera_z]
    xor dx, dx
    mov bx, SEGMENT_LENGTH
    div bx
    and ax, TRACK_SEGMENTS - 1
    shl ax, 1
    mov bx, ax
    mov ax, [cs:track_data + bx]
    mov [cs:current_curve], ax
    popa
    ret

; ============================================================================
; draw_sky - Draw sky gradient
; ============================================================================
draw_sky:
    pusha

    ; Divide sky area (below HUD) into 8 bands
    mov ax, [cs:horizon_y]
    sub ax, 12                       ; Skip HUD area (top 12 pixels)
    xor dx, dx
    mov bx, 8
    div bx
    mov [cs:sky_band_h], ax         ; Height per band

    mov word [cs:sky_y], 12          ; Start below HUD
    mov byte [cs:sky_idx], CLR_SKY_TOP

.sky_loop:
    movzx ax, byte [cs:sky_idx]
    cmp al, CLR_SKY_BOT
    ja .sky_done

    mov bx, 0
    mov cx, [cs:sky_y]
    mov dx, [cs:scr_w]
    mov si, [cs:sky_band_h]
    mov al, [cs:sky_idx]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    mov ax, [cs:sky_band_h]
    add [cs:sky_y], ax
    inc byte [cs:sky_idx]
    jmp .sky_loop

.sky_done:
    ; Fill any remaining gap to horizon
    mov ax, [cs:sky_y]
    cmp ax, [cs:horizon_y]
    jae .sky_exit
    mov bx, 0
    mov cx, ax
    mov dx, [cs:scr_w]
    mov si, [cs:horizon_y]
    sub si, ax
    mov al, CLR_SKY_BOT
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.sky_exit:
    popa
    ret

; ============================================================================
; draw_road - Render the perspective road
; ============================================================================
draw_road:
    pusha

    ; Compute depth_scale based on screen size
    ; depth_scale = scr_w * 15 (gives good perspective for 320)
    mov ax, [cs:scr_w]
    mov bx, 15
    mul bx
    mov [cs:depth_scale], ax

    ; Initialize curve accumulator - starts at 0 for smooth progressive curve
    mov word [cs:curve_accum], 0

    ; Start from screen bottom, draw upward toward horizon (full detail everywhere)
    mov ax, [cs:scr_h]
    dec ax
    mov [cs:strip_y], ax

.strip_loop:
    mov ax, [cs:strip_y]
    cmp ax, [cs:horizon_y]
    jle .strips_done

    ; Z = depth_scale / (strip_y - horizon_y)
    mov ax, [cs:depth_scale]
    xor dx, dx
    mov bx, [cs:strip_y]
    sub bx, [cs:horizon_y]
    cmp bx, 1
    jl .next_strip
    div bx
    mov [cs:strip_z], ax

    ; Road half-width = ROAD_BASE_HW * 256 / Z
    mov ax, ROAD_BASE_HW
    shl ax, 8
    xor dx, dx
    mov bx, [cs:strip_z]
    cmp bx, 1
    jl .next_strip
    div bx
    ; Clamp to half screen width
    mov bx, [cs:scr_w]
    shr bx, 1
    cmp ax, bx
    jbe .hw_ok
    mov ax, bx
.hw_ok:
    mov [cs:strip_hw], ax

    ; Per-strip curve lookup with interpolation for smooth bends
    mov ax, [cs:camera_z]
    add ax, [cs:strip_z]
    xor dx, dx
    mov bx, SEGMENT_LENGTH
    div bx                           ; AX = seg index, DX = fraction (0-39)
    push dx                          ; Save fraction
    and ax, TRACK_SEGMENTS - 1
    shl ax, 1                        ; Word offset
    mov bx, ax
    mov si, [cs:track_data + bx]     ; SI = curve_a
    add bx, 2
    and bx, (TRACK_SEGMENTS * 2) - 1 ; Wrap
    mov cx, [cs:track_data + bx]     ; CX = curve_b
    sub cx, si                        ; CX = delta (curve_b - curve_a)
    pop ax                            ; AX = fraction
    imul cx                           ; DX:AX = fraction * delta
    mov cx, SEGMENT_LENGTH
    idiv cx                           ; AX = interpolated offset
    add ax, si                        ; AX = smooth curve value

    ; Accumulate curve offset (builds progressive bend)
    add [cs:curve_accum], ax

    ; Road center = scr_w/2 + (curve_accum >> 5)
    mov ax, [cs:curve_accum]
    sar ax, 5
    mov bx, [cs:scr_w]
    shr bx, 1
    add ax, bx
    mov [cs:strip_cx], ax

    ; Compute edges
    mov bx, [cs:strip_cx]
    sub bx, [cs:strip_hw]
    cmp bx, 0
    jge .l_ok
    xor bx, bx
.l_ok:
    mov [cs:road_left], bx

    mov bx, [cs:strip_cx]
    add bx, [cs:strip_hw]
    cmp bx, [cs:scr_w]
    jle .r_ok
    mov bx, [cs:scr_w]
.r_ok:
    mov [cs:road_right], bx

    ; Store road edges in lookup table for obstacle drawing
    mov ax, [cs:strip_y]
    sub ax, [cs:horizon_y]
    shr ax, 1                        ; Index = (strip_y - horizon_y) / 2
    cmp ax, 60
    jae .skip_edge_store
    shl ax, 1                        ; Word offset
    mov di, ax
    mov ax, [cs:road_left]
    mov [cs:road_edge_left + di], ax
    mov ax, [cs:road_right]
    mov [cs:road_edge_right + di], ax
.skip_edge_store:

    ; Save road edges near car Y for grass collision check
    ; Loop goes bottom-to-top; capture while strip_y >= car_y (last capture = closest to car)
    mov ax, [cs:strip_y]
    cmp ax, [cs:car_y]
    jb .not_car_y
    mov ax, [cs:road_left]
    mov [cs:road_left_at_car], ax
    mov ax, [cs:road_right]
    mov [cs:road_right_at_car], ax
.not_car_y:

    ; Rumble strip width = max(2, strip_hw / 10)
    mov ax, [cs:strip_hw]
    xor dx, dx
    mov bx, 10
    div bx
    cmp ax, 2
    jge .rumble_ok
    mov ax, 2
.rumble_ok:
    mov [cs:rumble_w], ax

    ; Segment color alternation
    mov ax, [cs:camera_z]
    add ax, [cs:strip_z]
    shr ax, 3
    test al, 1
    jnz .odd_seg

.even_seg:
    mov byte [cs:road_color], CLR_ROAD_LIGHT
    mov byte [cs:grass_color], CLR_GRASS_LIGHT
    mov byte [cs:rumble_color], CLR_RUMBLE_WHITE
    mov byte [cs:has_stripe], 0
    jmp .do_draw

.odd_seg:
    mov byte [cs:road_color], CLR_ROAD_DARK
    mov byte [cs:grass_color], CLR_GRASS_DARK
    mov byte [cs:rumble_color], CLR_RUMBLE_RED
    mov byte [cs:has_stripe], 1

.do_draw:
    ; Left grass (2px tall strips to halve API calls / reduce flicker)
    mov ax, [cs:road_left]
    cmp ax, 0
    je .skip_lg
    mov bx, 0
    mov cx, [cs:strip_y]
    mov dx, [cs:road_left]
    mov si, 2
    mov al, [cs:grass_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.skip_lg:

    ; Left rumble strip
    mov bx, [cs:road_left]
    mov cx, [cs:strip_y]
    mov dx, [cs:rumble_w]
    mov si, 2
    mov al, [cs:rumble_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Road surface
    mov bx, [cs:road_left]
    add bx, [cs:rumble_w]
    mov cx, [cs:strip_y]
    mov dx, [cs:road_right]
    sub dx, [cs:road_left]
    sub dx, [cs:rumble_w]
    sub dx, [cs:rumble_w]
    cmp dx, 0
    jle .skip_road
    mov si, 2
    mov al, [cs:road_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.skip_road:

    ; Right rumble strip
    mov bx, [cs:road_right]
    sub bx, [cs:rumble_w]
    mov cx, [cs:strip_y]
    mov dx, [cs:rumble_w]
    mov si, 2
    mov al, [cs:rumble_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Right grass
    mov bx, [cs:road_right]
    mov cx, [cs:strip_y]
    mov dx, [cs:scr_w]
    sub dx, [cs:road_right]
    cmp dx, 0
    jle .skip_rg
    mov si, 2
    mov al, [cs:grass_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.skip_rg:

    ; Center stripe (dashed line on even segments)
    cmp byte [cs:has_stripe], 0
    je .next_strip
    mov ax, [cs:strip_hw]
    shr ax, 4
    cmp ax, 1
    jge .sw_ok
    mov ax, 1
.sw_ok:
    mov dx, ax
    mov bx, [cs:strip_cx]
    shr dx, 1
    sub bx, dx
    cmp bx, 0
    jge .cs_ok
    xor bx, bx
.cs_ok:
    mov cx, [cs:strip_y]
    mov ax, [cs:strip_hw]
    shr ax, 4
    cmp ax, 1
    jge .sw2_ok
    mov ax, 1
.sw2_ok:
    mov dx, ax
    mov si, 2
    mov al, CLR_ROAD_MARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

.next_strip:
    sub word [cs:strip_y], 2
    jmp .strip_loop

.strips_done:
    popa
    ret

; ============================================================================
; draw_obstacles - Draw roadside trees along the track
; ============================================================================
draw_obstacles:
    pusha

    ; Compute camera position in track (camera_z % TRACK_TOTAL_LEN)
    mov ax, [cs:camera_z]
    xor dx, dx
    mov bx, TRACK_TOTAL_LEN
    div bx
    mov [cs:obs_cam_pos], dx

    mov byte [cs:obs_idx], 0
.obs_loop:
    cmp byte [cs:obs_idx], NUM_OBSTACLES
    jge .obs_done

    ; Get relative Z distance for this obstacle
    xor bh, bh
    mov bl, [cs:obs_idx]
    shl bx, 1
    mov ax, [cs:obstacle_z + bx]
    sub ax, [cs:obs_cam_pos]
    jge .obs_pos_ok
    add ax, TRACK_TOTAL_LEN
.obs_pos_ok:
    mov [cs:obs_rel_z], ax

    ; Skip if too close or too far
    cmp ax, 30
    jl .obs_next
    cmp ax, 400
    jg .obs_next

    ; screen_y = horizon_y + depth_scale / rel_z
    mov ax, [cs:depth_scale]
    xor dx, dx
    mov bx, [cs:obs_rel_z]
    div bx
    add ax, [cs:horizon_y]
    cmp ax, [cs:scr_h]
    jge .obs_next
    mov [cs:obs_screen_y], ax

    ; tree_h = 1600 / rel_z (capped at 40, min 2)
    mov ax, 1600
    xor dx, dx
    mov bx, [cs:obs_rel_z]
    div bx
    cmp ax, 40
    jle .vt_h_ok
    mov ax, 40
.vt_h_ok:
    cmp ax, 2
    jge .vt_h_min
    mov ax, 2
.vt_h_min:
    mov [cs:obs_tree_h], ax

    ; tree_w = 800 / rel_z (capped at 24, min 2)
    mov ax, 800
    xor dx, dx
    mov bx, [cs:obs_rel_z]
    div bx
    cmp ax, 24
    jle .vt_w_ok
    mov ax, 24
.vt_w_ok:
    cmp ax, 2
    jge .vt_w_min
    mov ax, 2
.vt_w_min:
    mov [cs:obs_tree_w], ax

    ; Look up road edge at screen_y from lookup table
    mov ax, [cs:obs_screen_y]
    sub ax, [cs:horizon_y]
    shr ax, 1                        ; strip index
    cmp ax, 60
    jae .obs_next
    shl ax, 1                        ; word offset
    mov di, ax

    ; Determine X position based on side (left or right of road)
    xor bh, bh
    mov bl, [cs:obs_idx]
    mov al, [cs:obstacle_side + bx]
    cmp al, 0
    je .obs_left

    ; Right side: tree_x = road_edge_right + 4
    mov bx, [cs:road_edge_right + di]
    add bx, 4
    jmp .obs_draw

.obs_left:
    ; Left side: tree_x = road_edge_left - tree_w - 4
    mov bx, [cs:road_edge_left + di]
    sub bx, [cs:obs_tree_w]
    sub bx, 4

.obs_draw:
    mov [cs:obs_tree_x], bx

    ; Clamp X to screen
    cmp bx, 0
    jl .obs_next
    mov ax, [cs:scr_w]
    sub ax, 10
    cmp bx, ax
    jg .obs_next

    ; Draw trunk (narrow, bottom half of tree)
    mov dx, [cs:obs_tree_w]
    shr dx, 2                        ; trunk_w = tree_w / 4
    cmp dx, 2
    jge .vt_tw_ok
    mov dx, 2
.vt_tw_ok:
    mov si, [cs:obs_tree_h]
    shr si, 1                        ; trunk_h = tree_h / 2
    cmp si, 1
    jge .vt_th_ok
    mov si, 1
.vt_th_ok:
    ; trunk X = tree_x + (tree_w - trunk_w) / 2
    mov bx, [cs:obs_tree_x]
    mov ax, [cs:obs_tree_w]
    sub ax, dx
    shr ax, 1
    add bx, ax
    mov cx, [cs:obs_screen_y]
    sub cx, si                        ; trunk top Y
    mov al, CLR_CAR_WHEEL             ; Dark trunk
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Draw canopy (full width, top half of tree)
    mov bx, [cs:obs_tree_x]
    mov dx, [cs:obs_tree_w]
    mov si, [cs:obs_tree_h]
    shr si, 1                        ; canopy_h = tree_h / 2
    cmp si, 2
    jge .vt_ch_ok
    mov si, 2
.vt_ch_ok:
    mov cx, [cs:obs_screen_y]
    sub cx, [cs:obs_tree_h]          ; canopy top Y
    mov al, CLR_GRASS_LIGHT           ; Green canopy
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

.obs_next:
    inc byte [cs:obs_idx]
    jmp .obs_loop

.obs_done:
    popa
    ret

; ============================================================================
; check_obstacle_collision - Check if car hit a roadside tree
; ============================================================================
check_obstacle_collision:
    pusha

    ; Only check when not already crashed
    cmp byte [cs:crash_timer], 0
    jne .col_done

    ; Compute camera position in track
    mov ax, [cs:camera_z]
    xor dx, dx
    mov bx, TRACK_TOTAL_LEN
    div bx
    ; DX = camera_z % TRACK_TOTAL_LEN

    xor ch, ch
    xor cl, cl
.col_loop:
    cmp cl, NUM_OBSTACLES
    jge .col_done

    ; Get relative Z for this obstacle
    push cx
    xor bh, bh
    mov bl, cl
    shl bx, 1
    mov ax, [cs:obstacle_z + bx]
    sub ax, dx
    jge .col_pos_ok
    add ax, TRACK_TOTAL_LEN
.col_pos_ok:
    ; AX = rel_z; crash zone is rel_z 1-12
    cmp ax, 12
    jg .col_next

    ; Obstacle is at the car's position — check X overlap
    pop cx
    push cx
    xor bh, bh
    mov bl, cl
    mov al, [cs:obstacle_side + bx]
    cmp al, 0
    je .col_left

    ; Right side: crash if player far enough off-road to the right
    mov ax, [cs:player_x]
    mov bx, [cs:road_right_at_car]
    add bx, 5                        ; 5px grace zone
    cmp ax, bx
    jle .col_next
    jmp .col_crash

.col_left:
    ; Left side: crash if player far enough off-road to the left
    mov ax, [cs:player_x]
    mov bx, [cs:road_left_at_car]
    sub bx, 5
    cmp ax, bx
    jge .col_next

.col_crash:
    mov byte [cs:crash_timer], 36     ; ~2 seconds at 18 ticks/sec
    mov word [cs:player_speed], 0
    pop cx
    jmp .col_done

.col_next:
    pop cx
    inc cl
    jmp .col_loop

.col_done:
    popa
    ret

; ============================================================================
; update_traffic - Move traffic cars along the track
; ============================================================================
update_traffic:
    pusha
    xor cx, cx
.ut_loop:
    cmp cl, NUM_TRAFFIC
    jge .ut_done
    xor bh, bh
    mov bl, cl
    push cx
    ; Get direction
    mov al, [cs:traffic_dir + bx]
    shl bx, 1                        ; Word index for traffic_z
    cmp al, 1
    je .ut_same
    ; Oncoming: traffic_z -= TRAFFIC_ONC_SPEED
    mov ax, [cs:traffic_z + bx]
    sub ax, TRAFFIC_ONC_SPEED
    cmp ax, 0
    jge .ut_store
    add ax, TRACK_TOTAL_LEN
    jmp .ut_store
.ut_same:
    ; Same direction: traffic_z += TRAFFIC_SAME_SPEED
    mov ax, [cs:traffic_z + bx]
    add ax, TRAFFIC_SAME_SPEED
    cmp ax, TRACK_TOTAL_LEN
    jl .ut_store
    sub ax, TRACK_TOTAL_LEN
.ut_store:
    mov [cs:traffic_z + bx], ax
    pop cx
    inc cl
    jmp .ut_loop
.ut_done:
    popa
    ret

; ============================================================================
; draw_traffic - Draw traffic cars on the road
; ============================================================================
draw_traffic:
    pusha

    ; Compute camera position in track
    mov ax, [cs:camera_z]
    xor dx, dx
    mov bx, TRACK_TOTAL_LEN
    div bx
    mov [cs:traf_cam_pos], dx

    mov byte [cs:traf_idx], 0
.dt_loop:
    cmp byte [cs:traf_idx], NUM_TRAFFIC
    jge .dt_done

    ; Compute relative Z
    xor bh, bh
    mov bl, [cs:traf_idx]
    shl bx, 1
    mov ax, [cs:traffic_z + bx]
    sub ax, [cs:traf_cam_pos]
    jge .dt_pos_ok
    add ax, TRACK_TOTAL_LEN
.dt_pos_ok:
    mov [cs:traf_rel_z], ax

    ; Skip if too close or too far
    cmp ax, 20
    jl .dt_next
    cmp ax, 400
    jg .dt_next

    ; screen_y = horizon_y + depth_scale / rel_z
    mov ax, [cs:depth_scale]
    xor dx, dx
    mov bx, [cs:traf_rel_z]
    div bx
    add ax, [cs:horizon_y]
    cmp ax, [cs:scr_h]
    jge .dt_next
    mov [cs:traf_screen_y], ax

    ; Car size: h = 1500 / rel_z, w = 2100 / rel_z (3x scale for visibility)
    mov ax, 1500
    xor dx, dx
    mov bx, [cs:traf_rel_z]
    div bx
    cmp ax, 40
    jle .dt_h_ok
    mov ax, 40
.dt_h_ok:
    cmp ax, 2
    jge .dt_h_min
    mov ax, 2
.dt_h_min:
    mov [cs:traf_car_h], ax

    mov ax, 2100
    xor dx, dx
    mov bx, [cs:traf_rel_z]
    div bx
    cmp ax, 50
    jle .dt_w_ok
    mov ax, 50
.dt_w_ok:
    cmp ax, 3
    jge .dt_w_min
    mov ax, 3
.dt_w_min:
    mov [cs:traf_car_w], ax

    ; Look up road edges at screen_y
    mov ax, [cs:traf_screen_y]
    sub ax, [cs:horizon_y]
    shr ax, 1
    cmp ax, 60
    jae .dt_next
    shl ax, 1
    mov di, ax

    ; Compute lane center X
    ; quarter = (road_right - road_left) / 4
    mov ax, [cs:road_edge_right + di]
    sub ax, [cs:road_edge_left + di]
    shr ax, 2                        ; Quarter-width

    xor bh, bh
    mov bl, [cs:traf_idx]
    cmp byte [cs:traffic_lane + bx], 0
    je .dt_left_lane

    ; Right lane: x = road_right - quarter
    mov bx, [cs:road_edge_right + di]
    sub bx, ax
    jmp .dt_got_center
.dt_left_lane:
    ; Left lane: x = road_left + quarter
    mov bx, [cs:road_edge_left + di]
    add bx, ax
.dt_got_center:
    ; Center the car sprite on lane center
    sub bx, [cs:traf_car_w]
    shr word [cs:traf_car_w], 0      ; (no-op, just for clarity)
    ; BX = lane_center - car_w (we'll use full car_w for the rect from BX)
    ; Actually: rect_x = center - car_w/2
    add bx, [cs:traf_car_w]          ; undo sub, back to center
    mov ax, [cs:traf_car_w]
    shr ax, 1
    sub bx, ax                        ; BX = center - car_w/2
    mov [cs:traf_car_x], bx

    ; Clamp to screen
    cmp bx, 0
    jl .dt_next
    mov ax, [cs:scr_w]
    sub ax, 10
    cmp bx, ax
    jg .dt_next

    ; Determine color based on direction
    xor bh, bh
    mov bl, [cs:traf_idx]
    cmp byte [cs:traffic_dir + bx], 1
    je .dt_same_color

    ; Oncoming: white car
    mov byte [cs:traf_body_clr], CLR_RUMBLE_WHITE
    jmp .dt_draw_body
.dt_same_color:
    ; Same direction: blue car
    mov byte [cs:traf_body_clr], CLR_CAR_WIND

.dt_draw_body:
    ; Draw car body
    mov bx, [cs:traf_car_x]
    mov cx, [cs:traf_screen_y]
    sub cx, [cs:traf_car_h]
    mov dx, [cs:traf_car_w]
    mov si, [cs:traf_car_h]
    mov al, [cs:traf_body_clr]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Draw windshield (dark strip at top, half width)
    mov bx, [cs:traf_car_x]
    mov ax, [cs:traf_car_w]
    shr ax, 2                         ; Inset = car_w / 4
    add bx, ax
    mov cx, [cs:traf_screen_y]
    sub cx, [cs:traf_car_h]
    mov dx, [cs:traf_car_w]
    shr dx, 1                         ; Windshield = half car width
    mov si, [cs:traf_car_h]
    shr si, 2                         ; Windshield height = car_h / 4
    cmp si, 1
    jge .dt_ws_ok
    mov si, 1
.dt_ws_ok:
    mov al, CLR_CAR_WHEEL             ; Dark windshield
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

.dt_next:
    inc byte [cs:traf_idx]
    jmp .dt_loop

.dt_done:
    popa
    ret

traf_body_clr:  db 0

; ============================================================================
; check_traffic_collision - Check if player hit a traffic car
; ============================================================================
check_traffic_collision:
    pusha

    ; Only check when not already crashed
    cmp byte [cs:crash_timer], 0
    jne .tc_done

    ; Compute camera position in track
    mov ax, [cs:camera_z]
    xor dx, dx
    mov bx, TRACK_TOTAL_LEN
    div bx
    ; DX = camera_z % TRACK_TOTAL_LEN

    xor ch, ch
    xor cl, cl
.tc_loop:
    cmp cl, NUM_TRAFFIC
    jge .tc_done

    push cx
    xor bh, bh
    mov bl, cl
    shl bx, 1
    mov ax, [cs:traffic_z + bx]
    sub ax, dx
    jge .tc_pos_ok
    add ax, TRACK_TOTAL_LEN
.tc_pos_ok:
    ; Crash zone: rel_z 1-15
    cmp ax, 15
    jg .tc_next

    ; Traffic car is at player's position — compute lane X
    pop cx
    push cx
    xor bh, bh
    mov bl, cl
    ; Compute lane center at car_y level
    mov ax, [cs:road_right_at_car]
    sub ax, [cs:road_left_at_car]
    shr ax, 2                         ; quarter
    cmp byte [cs:traffic_lane + bx], 0
    je .tc_left_lane
    ; Right lane
    mov bx, [cs:road_right_at_car]
    sub bx, ax
    jmp .tc_check_x
.tc_left_lane:
    mov bx, [cs:road_left_at_car]
    add bx, ax
.tc_check_x:
    ; BX = traffic lane center, check if player overlaps (within 25px)
    mov ax, [cs:player_x]
    sub ax, bx
    ; AX = signed distance
    cmp ax, 25
    jg .tc_next
    cmp ax, -25
    jl .tc_next
    ; Collision!
    mov byte [cs:crash_timer], 36
    mov word [cs:player_speed], 0
    pop cx
    jmp .tc_done

.tc_next:
    pop cx
    inc cl
    jmp .tc_loop

.tc_done:
    popa
    ret

; ============================================================================
; draw_car - Draw the player's car with VGA colors (40x24)
; ============================================================================
draw_car:
    pusha

    ; Skip drawing on odd frames when crashed (flash effect)
    cmp byte [cs:crash_timer], 0
    je .draw_car_ok
    test byte [cs:crash_timer], 1
    jnz .car_done
.draw_car_ok:

    ; 1. Shadow (46x3, below car)
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2 + 3
    mov cx, [cs:car_y]
    add cx, CAR_H
    mov dx, CAR_W + 6
    mov si, 3
    mov al, CLR_CAR_WHEEL
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 2. Body (40x24, red)
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2
    mov cx, [cs:car_y]
    mov dx, CAR_W
    mov si, CAR_H
    mov al, CLR_CAR_BODY
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 3. Roof (30x3, dark, centered top)
    mov bx, [cs:player_x]
    sub bx, 15
    mov cx, [cs:car_y]
    mov dx, 30
    mov si, 3
    mov al, CLR_CAR_DARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 4. Rear window (24x5, blue, centered y+3)
    mov bx, [cs:player_x]
    sub bx, 12
    mov cx, [cs:car_y]
    add cx, 3
    mov dx, 24
    mov si, 5
    mov al, CLR_CAR_WIND
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 5. Left side panel (4x18, dark, y+3)
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2
    mov cx, [cs:car_y]
    add cx, 3
    mov dx, 4
    mov si, 18
    mov al, CLR_CAR_DARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 6. Right side panel (4x18, dark, y+3)
    mov bx, [cs:player_x]
    add bx, CAR_W / 2 - 4
    mov cx, [cs:car_y]
    add cx, 3
    mov dx, 4
    mov si, 18
    mov al, CLR_CAR_DARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 7. Rear bumper bar (40x3, dark, y+21)
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2
    mov cx, [cs:car_y]
    add cx, 21
    mov dx, CAR_W
    mov si, 3
    mov al, CLR_CAR_DARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 8. Left taillight (4x2, red on dark bumper)
    mov bx, [cs:player_x]
    sub bx, 18
    mov cx, [cs:car_y]
    add cx, 21
    mov dx, 4
    mov si, 2
    mov al, CLR_CAR_BODY
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 9. Right taillight (4x2, red on dark bumper)
    mov bx, [cs:player_x]
    add bx, 14
    mov cx, [cs:car_y]
    add cx, 21
    mov dx, 4
    mov si, 2
    mov al, CLR_CAR_BODY
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 10. Left wheel (6x6, black)
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2 - 2
    mov cx, [cs:car_y]
    add cx, CAR_H - 6
    mov dx, 6
    mov si, 6
    mov al, CLR_CAR_WHEEL
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; 11. Right wheel (6x6, black)
    mov bx, [cs:player_x]
    add bx, CAR_W / 2 - 8
    mov cx, [cs:car_y]
    add cx, CAR_H - 6
    mov dx, 6
    mov si, 6
    mov al, CLR_CAR_WHEEL
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

.car_done:
    popa
    ret

; ============================================================================
; draw_hud - Draw speed, score, time at top
; ============================================================================
draw_hud:
    pusha

    ; HUD background bar
    mov bx, 0
    mov cx, 0
    mov dx, [cs:scr_w]
    mov si, 10
    mov al, CLR_HUD_BG
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Speed
    mov bx, 4
    mov cx, 1
    mov si, str_speed
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov dx, [cs:player_speed]
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov bx, 52
    mov cx, 1
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Score
    mov bx, 120
    mov cx, 1
    mov si, str_score
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov dx, [cs:score]
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov bx, 168
    mov cx, 1
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Time
    mov bx, 250
    mov cx, 1
    mov si, str_time
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov dx, [cs:time_left]
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov bx, 290
    mov cx, 1
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; draw_title - Graphical title screen with sunset, road, car, bitmap title
; ============================================================================
draw_title:
    pusha

    ; === Sunset sky gradient (6 bands, y=0 to y=102) ===
    ; Band 0: Deep navy (y=0-17)
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 17
    mov al, CLR_TITLE_SKY0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Band 1: Dark purple (y=17-34)
    mov bx, 0
    mov cx, 17
    mov dx, 320
    mov si, 17
    mov al, CLR_TITLE_SKY1
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Band 2: Purple-red (y=34-51)
    mov bx, 0
    mov cx, 34
    mov dx, 320
    mov si, 17
    mov al, CLR_TITLE_SKY2
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Band 3: Red-orange (y=51-68)
    mov bx, 0
    mov cx, 51
    mov dx, 320
    mov si, 17
    mov al, CLR_TITLE_SKY3
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Band 4: Orange (y=68-85)
    mov bx, 0
    mov cx, 68
    mov dx, 320
    mov si, 17
    mov al, CLR_TITLE_SKY4
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Band 5: Yellow-orange horizon (y=85-102)
    mov bx, 0
    mov cx, 85
    mov dx, 320
    mov si, 17
    mov al, CLR_TITLE_SKY5
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; === Ground (dark green, y=102 to y=200) ===
    mov bx, 0
    mov cx, 102
    mov dx, 320
    mov si, 98
    mov al, CLR_GRASS_DARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; === Road perspective (10 bands from horizon to bottom) ===
    ; halfwidth = 10 + (y - 102), gives nice perspective trapezoid
    mov word [cs:strip_y], 102  ; reuse gameplay scratch variable
    mov byte [cs:sky_idx], 10   ; band counter (reuse scratch)

.title_road_band:
    cmp byte [cs:sky_idx], 0
    je .title_road_done

    ; Compute halfwidth = 10 + (y - 102)
    mov ax, [cs:strip_y]
    sub ax, 102
    add ax, 10                  ; AX = halfwidth

    ; Road rect: x = 160-hw, w = 2*hw, h = 10
    mov bx, 160
    sub bx, ax                  ; BX = x
    mov cx, [cs:strip_y]        ; CX = y
    shl ax, 1                   ; AX = width
    mov dx, ax                  ; DX = width
    mov si, 10                  ; SI = height

    ; Color: dark far, light near
    cmp cx, 152
    jl .title_far_road
    mov al, CLR_ROAD_LIGHT
    jmp .title_draw_road_band
.title_far_road:
    mov al, CLR_ROAD_DARK
.title_draw_road_band:
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    add word [cs:strip_y], 10
    dec byte [cs:sky_idx]
    jmp .title_road_band
.title_road_done:

    ; === Center line dashes (perspective) ===
    ; Dash 1 (far)
    mov bx, 159
    mov cx, 108
    mov dx, 2
    mov si, 6
    mov al, CLR_ROAD_MARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Dash 2
    mov bx, 158
    mov cx, 122
    mov dx, 3
    mov si, 8
    mov al, CLR_ROAD_MARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Dash 3
    mov bx, 157
    mov cx, 138
    mov dx, 5
    mov si, 10
    mov al, CLR_ROAD_MARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Dash 4 (near)
    mov bx, 155
    mov cx, 158
    mov dx, 7
    mov si, 12
    mov al, CLR_ROAD_MARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; === Car silhouette (rear view, centered at x=160) ===

    ; Shadow under car
    mov bx, 118
    mov cx, 182
    mov dx, 84
    mov si, 5
    mov al, CLR_CAR_WHEEL
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Roof
    mov bx, 145
    mov cx, 146
    mov dx, 30
    mov si, 6
    mov al, CLR_CAR_DARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Rear window
    mov bx, 139
    mov cx, 152
    mov dx, 42
    mov si, 8
    mov al, CLR_CAR_WIND
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Body upper
    mov bx, 128
    mov cx, 160
    mov dx, 64
    mov si, 8
    mov al, CLR_CAR_BODY
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Body lower / fenders
    mov bx, 122
    mov cx, 168
    mov dx, 76
    mov si, 8
    mov al, CLR_CAR_BODY
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Bumper
    mov bx, 124
    mov cx, 176
    mov dx, 72
    mov si, 4
    mov al, CLR_CAR_DARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Left wheel
    mov bx, 114
    mov cx, 170
    mov dx, 10
    mov si, 14
    mov al, CLR_CAR_WHEEL
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Right wheel
    mov bx, 196
    mov cx, 170
    mov dx, 10
    mov si, 14
    mov al, CLR_CAR_WHEEL
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Left taillight
    mov bx, 130
    mov cx, 162
    mov dx, 5
    mov si, 3
    mov al, CLR_TITLE_SKY3      ; Red-orange
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Right taillight
    mov bx, 185
    mov cx, 162
    mov dx, 5
    mov si, 3
    mov al, CLR_TITLE_SKY3
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; === Title text "OUTLAST" ===

    ; Shadow pass (offset +2, +2)
    mov byte [cs:title_draw_clr], CLR_TITLE_SHADOW
    mov word [cs:title_draw_x], TITLE_START_X + 2
    mov word [cs:title_draw_y], TITLE_START_Y + 2
    call render_bitmap_title

    ; Main pass (gold)
    mov byte [cs:title_draw_clr], CLR_TITLE_GOLD
    mov word [cs:title_draw_x], TITLE_START_X
    mov word [cs:title_draw_y], TITLE_START_Y
    call render_bitmap_title

    ; === "Press any key" text ===
    mov bx, 112
    mov cx, 192
    mov si, str_press_key
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; render_bitmap_title - Draw "OUTLAST" using bitmap font data
; Input: title_draw_x, title_draw_y, title_draw_clr set before call
; ============================================================================
render_bitmap_title:
    pusha

    mov ax, [cs:title_draw_x]
    mov [cs:title_cur_x], ax

    xor si, si                  ; SI = letter index (0-6)

.rt_letter_loop:
    cmp si, 7
    jge .rt_all_done

    push si                     ; save letter index

    ; Get font data offset for this letter
    movzx ax, byte [cs:title_order + si]
    mov bl, TITLE_FONT_ROWS
    mul bl                      ; AX = unique_letter_idx * 7
    mov bp, ax                  ; BP = offset within title_font

    mov cx, [cs:title_draw_y]   ; CX = current Y
    xor di, di                  ; DI = row counter (0-6)

.rt_row_loop:
    cmp di, TITLE_FONT_ROWS
    jge .rt_letter_done

    mov al, [cs:title_font + bp]
    inc bp

    test al, al                 ; skip empty rows
    jz .rt_empty_row

    push bp                     ; save font pointer
    push di                     ; save row counter
    push cx                     ; save Y

    mov dl, al                  ; DL = row bits (5 columns, bit 4=leftmost)
    mov dh, 5                   ; DH = column counter
    mov bx, [cs:title_cur_x]   ; BX = current X

.rt_col_loop:
    test dl, 0x10               ; test leftmost bit
    jz .rt_no_block

    ; Draw filled block at (BX, CX)
    push bx
    push cx
    push dx

    mov dx, TITLE_BLK_W         ; width
    mov si, TITLE_BLK_H         ; height
    mov al, [cs:title_draw_clr]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    pop dx
    pop cx
    pop bx

.rt_no_block:
    add bx, TITLE_BLK_W         ; advance X
    shl dl, 1                   ; shift to next bit
    dec dh
    jnz .rt_col_loop

    pop cx                      ; restore Y
    pop di                      ; restore row counter
    pop bp                      ; restore font pointer

.rt_empty_row:
    add cx, TITLE_BLK_H         ; advance Y
    inc di
    jmp .rt_row_loop

.rt_letter_done:
    add word [cs:title_cur_x], (5 * TITLE_BLK_W) + TITLE_GAP
    pop si                      ; restore letter index
    inc si
    jmp .rt_letter_loop

.rt_all_done:
    popa
    ret

; ============================================================================
; Music system - PC speaker note player
; ============================================================================

; start_song: SI = pointer to song data (array of dw freq, dw duration pairs)
start_song:
    mov [cs:music_song], si
    mov [cs:music_ptr], si
    mov word [cs:music_ticks], 0
    ret

; stop_music: silence the speaker
stop_music:
    push ax
    mov ah, API_SPEAKER_OFF
    int 0x80
    mov word [cs:music_ticks], 0
    mov word [cs:music_ptr], 0
    pop ax
    ret

; play_music_tick: called once per BIOS tick (~55ms)
; Counts down note duration, advances to next note when done
play_music_tick:
    pusha

    cmp word [cs:music_ptr], 0
    je .mt_done                     ; No song playing

    cmp word [cs:music_ticks], 0
    jne .mt_counting

    ; Time for next note
    mov si, [cs:music_ptr]
    mov bx, [cs:si]                 ; BX = frequency
    cmp bx, 0xFFFF
    jne .mt_not_end

    ; End of song - loop back to start
    mov si, [cs:music_song]
    mov bx, [cs:si]

.mt_not_end:
    mov cx, [cs:si + 2]             ; CX = duration in ticks
    mov [cs:music_ticks], cx
    add si, 4                       ; Advance past this note
    mov [cs:music_ptr], si

    cmp bx, NOTE_REST
    je .mt_rest

    ; Play tone
    mov ah, API_SPEAKER_TONE
    int 0x80
    jmp .mt_done

.mt_rest:
    mov ah, API_SPEAKER_OFF
    int 0x80
    jmp .mt_done

.mt_counting:
    dec word [cs:music_ticks]

.mt_done:
    popa
    ret

; ============================================================================
; draw_game_over
; ============================================================================
draw_game_over:
    pusha

    mov bx, 80
    mov cx, 70
    mov dx, 160
    mov si, 60
    mov al, CLR_HUD_BG
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    mov bx, 110
    mov cx, 80
    mov si, str_gameover
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 100
    mov cx, 100
    mov si, str_final_score
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov dx, [cs:score]
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov bx, 200
    mov cx, 100
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 70
    mov cx, 120
    mov si, str_press_key
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; Track data - curve values (signed words)
; ============================================================================
track_data:
    dw 0, 0, 0, 0                  ; Segments 0-3: straight
    dw 0, 0, 0, 0                  ; Segments 4-7: straight
    dw 5, 15, 25, 30               ; Segments 8-11: gentle right curve
    dw 30, 25, 15, 5               ; Segments 12-15: ease out right
    dw 0, 0, 0, 0                  ; Segments 16-19: straight
    dw 0, 0, 0, 0                  ; Segments 20-23: straight
    dw -5, -15, -25, -30           ; Segments 24-27: gentle left curve
    dw -30, -25, -15, 0            ; Segments 28-31: ease out left

; ============================================================================
; Data
; ============================================================================

; Save state
saved_video_mode: db 0x04
saved_text_clr: db 0
saved_bg_clr:   db 0
saved_win_clr:  db 0
fs_win_handle:  db 0
fs_win_title:   db 0                ; Empty title for frameless window

; Screen dimensions
scr_w:          dw 320
scr_h:          dw 200

; Game state
game_state:     db STATE_TITLE
quit_flag:      db 0
rng_seed:       dw 0
last_tick:      dw 0

; Computed layout
horizon_y:      dw 80
car_y:          dw 168
depth_scale:    dw 4800
stop_y:         dw 193

; Player
player_x:       dw 160
player_speed:   dw 0
camera_z:       dw 0

; Game stats
score:          dw 0
time_left:      dw 60
timer_counter:  dw 0
decel_counter:  db 0                ; Decelerate every 3rd frame

; Track
current_curve:  dw 0
curve_accum:    dw 0
road_left_at_car:  dw 0            ; Road edge at car Y for grass check
road_right_at_car: dw 0

; Rendering scratch
strip_y:        dw 0
strip_z:        dw 0
strip_hw:       dw 0
strip_cx:       dw 0
road_left:      dw 0
road_right:     dw 0
rumble_w:       dw 0
road_color:     db 0
grass_color:    db 0
rumble_color:   db 0
has_stripe:     db 0
sky_y:          dw 0
sky_band_h:     dw 0
sky_idx:        db 0

; Crash state
crash_timer:    db 0                ; Countdown: >0 = car is crashed

; Obstacles (roadside trees)
NUM_OBSTACLES           equ 8
TRACK_TOTAL_LEN         equ (TRACK_SEGMENTS * SEGMENT_LENGTH) ; 2560
obstacle_z:     dw 200, 520, 900, 1300, 1600, 1900, 2200, 2480
obstacle_side:  db 1, 0, 1, 0, 1, 0, 1, 0  ; 0=left, 1=right of road

; Road edge lookup table (60 strips max, indexed by (screen_y - horizon_y) / 2)
road_edge_left:  times 60 dw 0
road_edge_right: times 60 dw 0

; Obstacle rendering scratch
obs_cam_pos:    dw 0
obs_rel_z:      dw 0
obs_screen_y:   dw 0
obs_tree_h:     dw 0
obs_tree_w:     dw 0
obs_tree_x:     dw 0
obs_idx:        db 0

; Traffic cars
NUM_TRAFFIC             equ 4
TRAFFIC_SAME_SPEED      equ 15      ; Same-direction speed (units/tick)
TRAFFIC_ONC_SPEED       equ 15      ; Oncoming speed (units/tick)
traffic_z:      dw 400, 1600, 800, 2000     ; Track positions
traffic_dir:    db 1, 1, 0, 0               ; 1=same direction, 0=oncoming
traffic_lane:   db 1, 0, 0, 1               ; 0=left lane, 1=right lane

; Traffic rendering scratch
traf_cam_pos:   dw 0
traf_rel_z:     dw 0
traf_screen_y:  dw 0
traf_car_h:     dw 0
traf_car_w:     dw 0
traf_car_x:     dw 0
traf_idx:       db 0

; Bitmap font for "OUTLAST" title (6 unique letters x 7 rows)
; Each byte = 5 columns (bit 4 = leftmost, bit 0 = rightmost)
title_font:
    ; O
    db 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E
    ; U
    db 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E
    ; T
    db 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04
    ; L
    db 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F
    ; A
    db 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11
    ; S
    db 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E

; Letter order: O=0, U=1, T=2, L=3, A=4, S=5, T=2
title_order:    db 0, 1, 2, 3, 4, 5, 2

; Title renderer scratch
title_draw_x:   dw 0
title_draw_y:   dw 0
title_draw_clr: db 0
title_cur_x:    dw 0

; Music state
music_ptr:      dw 0                ; Pointer to current note in song
music_song:     dw 0                ; Pointer to song start (for looping)
music_ticks:    dw 0                ; Remaining ticks for current note
game_song_idx:  db 0                ; Cycles through 3 gameplay songs

; Song pointer table
song_table:     dw song_game1, song_game2, song_game3

; Title theme - "Twilight Road" (A minor, atmospheric)
song_title:
    dw NOTE_A3, DUR_HALF
    dw NOTE_REST, DUR_8TH
    dw NOTE_C4, DUR_QUARTER
    dw NOTE_D4, DUR_QUARTER
    dw NOTE_E4, DUR_HALF
    dw NOTE_REST, DUR_8TH
    dw NOTE_D4, DUR_QUARTER
    dw NOTE_C4, DUR_QUARTER
    dw NOTE_A3, DUR_HALF
    dw NOTE_REST, DUR_8TH
    dw NOTE_E4, DUR_QUARTER
    dw NOTE_G4, DUR_QUARTER
    dw NOTE_A4, DUR_HALF
    dw NOTE_REST, DUR_8TH
    dw NOTE_G4, DUR_QUARTER
    dw NOTE_E4, DUR_QUARTER
    dw NOTE_D4, DUR_HALF
    dw NOTE_REST, DUR_QUARTER
    dw NOTE_A3, DUR_QUARTER
    dw NOTE_C4, DUR_QUARTER
    dw NOTE_E4, DUR_QUARTER
    dw NOTE_D4, DUR_QUARTER
    dw NOTE_C4, DUR_QUARTER
    dw NOTE_A3, DUR_DOT_Q
    dw NOTE_REST, DUR_HALF
    dw 0xFFFF, 0

; Game song 1 - "Sunset Drive" (C major, upbeat)
song_game1:
    dw NOTE_E5, DUR_8TH
    dw NOTE_D5, DUR_8TH
    dw NOTE_C5, DUR_QUARTER
    dw NOTE_REST, DUR_16TH
    dw NOTE_E5, DUR_8TH
    dw NOTE_D5, DUR_8TH
    dw NOTE_C5, DUR_8TH
    dw NOTE_D5, DUR_8TH
    dw NOTE_E5, DUR_QUARTER
    dw NOTE_REST, DUR_16TH
    dw NOTE_D5, DUR_8TH
    dw NOTE_C5, DUR_8TH
    dw NOTE_B4, DUR_QUARTER
    dw NOTE_REST, DUR_16TH
    dw NOTE_D5, DUR_8TH
    dw NOTE_C5, DUR_8TH
    dw NOTE_B4, DUR_8TH
    dw NOTE_C5, DUR_8TH
    dw NOTE_D5, DUR_QUARTER
    dw NOTE_REST, DUR_16TH
    dw NOTE_C5, DUR_QUARTER
    dw NOTE_E5, DUR_QUARTER
    dw NOTE_G5, DUR_QUARTER
    dw NOTE_E5, DUR_QUARTER
    dw NOTE_D5, DUR_QUARTER
    dw NOTE_C5, DUR_QUARTER
    dw NOTE_B4, DUR_QUARTER
    dw NOTE_REST, DUR_8TH
    dw NOTE_C5, DUR_QUARTER
    dw NOTE_D5, DUR_QUARTER
    dw NOTE_E5, DUR_HALF
    dw NOTE_REST, DUR_QUARTER
    dw 0xFFFF, 0

; Game song 2 - "Night Chase" (E minor, driving)
song_game2:
    dw NOTE_E4, DUR_8TH
    dw NOTE_E4, DUR_8TH
    dw NOTE_G4, DUR_QUARTER
    dw NOTE_A4, DUR_8TH
    dw NOTE_G4, DUR_8TH
    dw NOTE_E4, DUR_QUARTER
    dw NOTE_REST, DUR_16TH
    dw NOTE_E4, DUR_8TH
    dw NOTE_E4, DUR_8TH
    dw NOTE_G4, DUR_QUARTER
    dw NOTE_B4, DUR_8TH
    dw NOTE_A4, DUR_8TH
    dw NOTE_G4, DUR_QUARTER
    dw NOTE_REST, DUR_16TH
    dw NOTE_C5, DUR_QUARTER
    dw NOTE_B4, DUR_8TH
    dw NOTE_A4, DUR_8TH
    dw NOTE_G4, DUR_QUARTER
    dw NOTE_E4, DUR_QUARTER
    dw NOTE_REST, DUR_16TH
    dw NOTE_B4, DUR_8TH
    dw NOTE_A4, DUR_8TH
    dw NOTE_G4, DUR_QUARTER
    dw NOTE_E4, DUR_8TH
    dw NOTE_D4, DUR_8TH
    dw NOTE_E4, DUR_QUARTER
    dw NOTE_REST, DUR_8TH
    dw NOTE_E4, DUR_8TH
    dw NOTE_G4, DUR_8TH
    dw NOTE_A4, DUR_QUARTER
    dw NOTE_B4, DUR_QUARTER
    dw NOTE_A4, DUR_QUARTER
    dw NOTE_G4, DUR_QUARTER
    dw NOTE_REST, DUR_QUARTER
    dw 0xFFFF, 0

; Game song 3 - "Coastal Rush" (G major, bouncy)
song_game3:
    dw NOTE_G4, DUR_QUARTER
    dw NOTE_B4, DUR_8TH
    dw NOTE_D5, DUR_8TH
    dw NOTE_C5, DUR_QUARTER
    dw NOTE_B4, DUR_QUARTER
    dw NOTE_REST, DUR_16TH
    dw NOTE_A4, DUR_QUARTER
    dw NOTE_C5, DUR_8TH
    dw NOTE_B4, DUR_8TH
    dw NOTE_A4, DUR_QUARTER
    dw NOTE_G4, DUR_QUARTER
    dw NOTE_REST, DUR_16TH
    dw NOTE_G4, DUR_8TH
    dw NOTE_A4, DUR_8TH
    dw NOTE_B4, DUR_QUARTER
    dw NOTE_D5, DUR_QUARTER
    dw NOTE_C5, DUR_8TH
    dw NOTE_B4, DUR_8TH
    dw NOTE_A4, DUR_QUARTER
    dw NOTE_REST, DUR_16TH
    dw NOTE_G4, DUR_QUARTER
    dw NOTE_B4, DUR_QUARTER
    dw NOTE_D5, DUR_QUARTER
    dw NOTE_G5, DUR_QUARTER
    dw NOTE_D5, DUR_QUARTER
    dw NOTE_B4, DUR_QUARTER
    dw NOTE_G4, DUR_HALF
    dw NOTE_REST, DUR_QUARTER
    dw 0xFFFF, 0

; Strings
str_press_key:  db 'Press any key', 0
str_speed:      db 'Speed:', 0
str_score:      db 'Score:', 0
str_time:       db 'Time:', 0
str_gameover:   db 'GAME OVER', 0
str_final_score: db 'Final Score:', 0
num_buf:        times 8 db 0
