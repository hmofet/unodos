; ============================================================================
; OUTRUN.BIN - Pseudo-3D Racing Game for UnoDOS (CGA version)
; Fullscreen (non-windowed) game with perspective road rendering
; ============================================================================

[ORG 0x0000]

; --- BIN Header (80 bytes) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic
    db 'OutRun', 0                  ; App name (12 bytes padded)
    times (0x04 + 12) - ($ - $$) db 0

; 16x16 icon bitmap (64 bytes, 2bpp CGA)
; Road with car icon
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

    ; Save current theme colors for restore on exit
    mov ah, API_THEME_GET_COLORS
    int 0x80
    mov [cs:saved_text_clr], al
    mov [cs:saved_bg_clr], bl
    mov [cs:saved_win_clr], cl

    ; Initialize RNG seed from BIOS tick
    mov ah, API_GET_TICK
    int 0x80
    mov [cs:rng_seed], ax

    ; Set white text on black background for CGA
    mov al, 3                       ; text color = white
    mov bl, 0                       ; bg = black
    mov cl, 3                       ; win = white
    mov ah, API_THEME_SET_COLORS
    int 0x80

    ; Clear screen
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 200
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Show title screen
    call draw_title

    ; Set game state
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

    ; --- Check events ---
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_KEY_PRESS
    jne .no_event

    ; DL = keycode
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
    ; Any key starts game
    call init_game
    jmp .no_event

.gameover_key:
    ; Any key returns to title
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 200
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    call draw_title
    mov byte [cs:game_state], STATE_TITLE
    jmp .no_event

.game_key:
    cmp dl, 130                     ; Left arrow
    je .steer_left
    cmp dl, 131                     ; Right arrow
    je .steer_right
    cmp dl, 128                     ; Up arrow = accelerate
    je .accelerate
    cmp dl, 129                     ; Down arrow = brake
    je .brake
    jmp .no_event

.steer_left:
    sub word [cs:player_x], 3
    cmp word [cs:player_x], 40
    jge .no_event
    mov word [cs:player_x], 40
    jmp .no_event

.steer_right:
    add word [cs:player_x], 3
    cmp word [cs:player_x], 280
    jle .no_event
    mov word [cs:player_x], 280
    jmp .no_event

.accelerate:
    cmp word [cs:player_speed], MAX_SPEED
    jge .no_event
    add word [cs:player_speed], 2
    jmp .no_event

.brake:
    cmp word [cs:player_speed], 0
    jle .no_event
    sub word [cs:player_speed], 4
    jns .no_event
    mov word [cs:player_speed], 0
    jmp .no_event

.set_quit:
    mov byte [cs:quit_flag], 1
    jmp .no_event

.no_event:
    ; --- Game tick ---
    cmp byte [cs:game_state], STATE_PLAYING
    jne .main_loop

    ; Throttle game speed using tick timer
    mov ah, API_GET_TICK
    int 0x80
    cmp ax, [cs:last_tick]
    je .main_loop                   ; Same tick, skip frame
    mov [cs:last_tick], ax

    ; Apply natural deceleration
    cmp word [cs:player_speed], 0
    je .no_decel
    dec word [cs:player_speed]
.no_decel:

    ; Advance camera based on speed
    mov ax, [cs:player_speed]
    shr ax, 1
    add [cs:camera_z], ax

    ; Update score (based on speed)
    mov ax, [cs:player_speed]
    shr ax, 2
    add [cs:score], ax

    ; Check for game over (time expired)
    ; Decrement timer every 18 ticks (~1 second)
    inc word [cs:timer_counter]
    cmp word [cs:timer_counter], 18
    jb .no_timer_dec
    mov word [cs:timer_counter], 0
    cmp word [cs:time_left], 0
    je .game_over_trigger
    dec word [cs:time_left]
.no_timer_dec:

    ; Compute current curve from track data
    call update_curve

    ; Apply curve to player position (drift)
    mov ax, [cs:current_curve]
    sar ax, 3                       ; Gentle drift
    movzx bx, byte [cs:player_speed + 1]  ; High byte
    test bx, bx
    jz .no_curve_drift
    imul bx
    sar ax, 3
.no_curve_drift:
    add [cs:player_x], ax

    ; Clamp player to road bounds
    cmp word [cs:player_x], 40
    jge .clamp_right
    mov word [cs:player_x], 40
.clamp_right:
    cmp word [cs:player_x], 280
    jle .render_frame
    mov word [cs:player_x], 280

.render_frame:
    call draw_road
    call draw_car
    call draw_hud
    jmp .main_loop

.game_over_trigger:
    mov byte [cs:game_state], STATE_GAMEOVER
    call draw_game_over
    jmp .main_loop

.exit_game:
    ; Restore theme colors
    mov al, [cs:saved_text_clr]
    mov bl, [cs:saved_bg_clr]
    mov cl, [cs:saved_win_clr]
    mov ah, API_THEME_SET_COLORS
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
API_APP_YIELD           equ 34
API_THEME_SET_COLORS    equ 54
API_THEME_GET_COLORS    equ 55
API_GET_TICK            equ 63
API_FILLED_RECT_COLOR   equ 67
API_DRAW_HLINE          equ 69
API_WORD_TO_STRING      equ 91

EVENT_KEY_PRESS         equ 1

STATE_TITLE             equ 0
STATE_PLAYING           equ 1
STATE_GAMEOVER          equ 2

MAX_SPEED               equ 60
HORIZON_Y               equ 80
ROAD_BASE_W             equ 120     ; Base road half-width at bottom
CAR_Y                   equ 175     ; Car Y position
CAR_W                   equ 16
CAR_H                   equ 10
TRACK_SEGMENTS          equ 32
SEGMENT_LENGTH          equ 40      ; Z-units per segment

; ============================================================================
; init_game - Reset game state
; ============================================================================
init_game:
    pusha

    mov word [cs:player_x], 160
    mov word [cs:player_speed], 0
    mov word [cs:camera_z], 0
    mov word [cs:score], 0
    mov word [cs:time_left], 60     ; 60 seconds
    mov word [cs:timer_counter], 0
    mov word [cs:current_curve], 0
    mov byte [cs:game_state], STATE_PLAYING

    ; Get starting tick
    mov ah, API_GET_TICK
    int 0x80
    mov [cs:last_tick], ax

    ; Clear screen
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 200
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    popa
    ret

; ============================================================================
; update_curve - Get current curve value from track data
; ============================================================================
update_curve:
    pusha

    ; segment_index = (camera_z / SEGMENT_LENGTH) % TRACK_SEGMENTS
    mov ax, [cs:camera_z]
    xor dx, dx
    mov bx, SEGMENT_LENGTH
    div bx                          ; AX = segment index
    and ax, TRACK_SEGMENTS - 1      ; Modulo (power of 2)
    shl ax, 1                       ; *2 for word array
    mov bx, ax
    mov ax, [cs:track_data + bx]
    mov [cs:current_curve], ax

    popa
    ret

; ============================================================================
; draw_road - Render the pseudo-3D road view
; ============================================================================
draw_road:
    pusha

    ; Draw sky (top portion)
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, HORIZON_Y
    mov al, 0                       ; Black (sky)
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Horizon line
    mov bx, 0
    mov cx, HORIZON_Y
    mov dx, 320
    mov al, 1                       ; Cyan
    mov ah, API_DRAW_HLINE
    int 0x80

    ; Draw road strips from horizon down
    ; For each strip: compute road width and position from perspective

    mov word [cs:strip_y], HORIZON_Y + 1

.strip_loop:
    cmp word [cs:strip_y], 195      ; Stop before car area
    jge .strips_done

    ; Compute Z depth: Z = depth_scale / (strip_y - HORIZON_Y)
    ; depth_scale = 4800 (tuned for good perspective)
    mov ax, 4800
    xor dx, dx
    mov bx, [cs:strip_y]
    sub bx, HORIZON_Y
    cmp bx, 1
    jl .next_strip
    div bx                          ; AX = Z (depth)
    mov [cs:strip_z], ax

    ; Road half-width = ROAD_BASE_W * 256 / Z
    mov ax, ROAD_BASE_W
    shl ax, 8
    xor dx, dx
    mov bx, [cs:strip_z]
    cmp bx, 1
    jl .next_strip
    div bx                          ; AX = road half-width in pixels
    cmp ax, 160
    jbe .width_ok
    mov ax, 160
.width_ok:
    mov [cs:strip_hw], ax

    ; Road center = 160 + curve_offset
    ; curve_offset = current_curve * Z / 256
    mov ax, [cs:current_curve]
    imul word [cs:strip_z]
    sar ax, 8
    add ax, 160
    mov [cs:strip_cx], ax

    ; Compute road edges
    mov bx, [cs:strip_cx]
    sub bx, [cs:strip_hw]          ; Left edge
    cmp bx, 0
    jge .left_ok
    xor bx, bx
.left_ok:
    mov [cs:road_left], bx

    mov bx, [cs:strip_cx]
    add bx, [cs:strip_hw]          ; Right edge
    cmp bx, 320
    jle .right_ok
    mov bx, 320
.right_ok:
    mov [cs:road_right], bx

    ; Determine segment color (alternating stripes for distance markers)
    ; segment = (camera_z + Z) / 8
    mov ax, [cs:camera_z]
    add ax, [cs:strip_z]
    shr ax, 3
    test al, 1
    jnz .odd_segment

.even_segment:
    ; Even: road=white, grass=cyan, no center stripe
    mov byte [cs:road_color], 3     ; White
    mov byte [cs:grass_color], 1    ; Cyan
    mov byte [cs:has_stripe], 0
    jmp .draw_strip

.odd_segment:
    ; Odd: road=white, grass=magenta, center stripe
    mov byte [cs:road_color], 3     ; White
    mov byte [cs:grass_color], 2    ; Magenta
    mov byte [cs:has_stripe], 1

.draw_strip:
    ; Draw left grass
    mov ax, [cs:road_left]
    cmp ax, 0
    je .skip_left_grass
    mov bx, 0
    mov cx, [cs:strip_y]
    mov dx, [cs:road_left]
    mov si, 1                       ; 1 pixel high
    mov al, [cs:grass_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.skip_left_grass:

    ; Draw road
    mov bx, [cs:road_left]
    mov cx, [cs:strip_y]
    mov dx, [cs:road_right]
    sub dx, [cs:road_left]
    cmp dx, 0
    jle .skip_road
    mov si, 1
    mov al, [cs:road_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.skip_road:

    ; Draw right grass
    mov bx, [cs:road_right]
    mov cx, [cs:strip_y]
    mov ax, 320
    sub ax, [cs:road_right]
    cmp ax, 0
    jle .skip_right_grass
    mov dx, ax
    mov si, 1
    mov al, [cs:grass_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.skip_right_grass:

    ; Draw center stripe (dashed line)
    cmp byte [cs:has_stripe], 0
    je .next_strip
    ; Center stripe width = max(1, strip_hw / 16)
    mov ax, [cs:strip_hw]
    shr ax, 4
    cmp ax, 1
    jge .stripe_w_ok
    mov ax, 1
.stripe_w_ok:
    mov dx, ax                      ; DX = stripe width
    mov bx, [cs:strip_cx]
    shr dx, 1
    sub bx, dx                      ; Center the stripe
    cmp bx, 0
    jge .stripe_ok
    xor bx, bx
.stripe_ok:
    mov cx, [cs:strip_y]
    mov ax, [cs:strip_hw]
    shr ax, 4
    cmp ax, 1
    jge .sw2_ok
    mov ax, 1
.sw2_ok:
    mov dx, ax
    mov si, 1
    mov al, 0                       ; Black center stripe
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

.next_strip:
    inc word [cs:strip_y]
    jmp .strip_loop

.strips_done:
    popa
    ret

; ============================================================================
; draw_car - Draw the player's car
; ============================================================================
draw_car:
    pusha

    ; Clear car area first
    mov bx, 0
    mov cx, CAR_Y - 2
    mov dx, 320
    mov si, CAR_H + 4
    mov al, 3                       ; White (road beneath car)
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Car body (magenta)
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2
    mov cx, CAR_Y
    mov dx, CAR_W
    mov si, CAR_H
    mov al, 2                       ; Magenta
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Car windshield (cyan stripe on top)
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2 - 2
    mov cx, CAR_Y
    mov dx, CAR_W - 4
    mov si, 3
    mov al, 1                       ; Cyan
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Wheels (black)
    ; Left wheel
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2 - 1
    mov cx, CAR_Y + CAR_H - 2
    mov dx, 3
    mov si, 2
    mov al, 0                       ; Black
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Right wheel
    mov bx, [cs:player_x]
    add bx, CAR_W / 2 - 4
    mov cx, CAR_Y + CAR_H - 2
    mov dx, 3
    mov si, 2
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    popa
    ret

; ============================================================================
; draw_hud - Draw speed, score, and timer
; ============================================================================
draw_hud:
    pusha

    ; Clear HUD area at top
    mov bx, 0
    mov cx, 0
    mov dx, 320
    mov si, 10
    mov al, 0                       ; Black
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
; draw_title - Draw title screen
; ============================================================================
draw_title:
    pusha

    ; "OUTRUN" title
    mov bx, 120
    mov cx, 60
    mov si, str_title
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 70
    mov cx, 80
    mov si, str_subtitle
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Instructions
    mov bx, 60
    mov cx, 120
    mov si, str_inst1
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 60
    mov cx, 135
    mov si, str_inst2
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 70
    mov cx, 160
    mov si, str_press_key
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; draw_game_over - Draw game over screen
; ============================================================================
draw_game_over:
    pusha

    ; Semi-transparent overlay (just clear center)
    mov bx, 80
    mov cx, 70
    mov dx, 160
    mov si, 60
    mov al, 0                       ; Black
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    mov bx, 110
    mov cx, 80
    mov si, str_gameover
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Final score
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
; Track data - curve values for each segment (signed words)
; Positive = curve right, negative = curve left
; ============================================================================
track_data:
    dw 0, 0, 0, 0                  ; Segments 0-3: straight
    dw 10, 20, 30, 30              ; Segments 4-7: gentle right
    dw 20, 10, 0, 0                ; Segments 8-11: straighten
    dw -15, -30, -40, -40          ; Segments 12-15: left curve
    dw -30, -15, 0, 0              ; Segments 16-19: straighten
    dw 0, 0, 5, 15                 ; Segments 20-23: gentle right
    dw 25, 35, 35, 25              ; Segments 24-27: sharp right
    dw 10, -10, -25, -15           ; Segments 28-31: S-curve

; ============================================================================
; Data
; ============================================================================

; Saved state
saved_text_clr: db 0
saved_bg_clr:   db 0
saved_win_clr:  db 0

; Game state
game_state:     db STATE_TITLE
quit_flag:      db 0
rng_seed:       dw 0
last_tick:      dw 0

; Player
player_x:       dw 160              ; Lateral position (center of car)
player_speed:   dw 0                ; Current speed
camera_z:       dw 0                ; Camera position along track

; Game stats
score:          dw 0
time_left:      dw 60               ; Countdown in seconds
timer_counter:  dw 0                ; Tick counter for timer

; Current track state
current_curve:  dw 0                ; Current curve value

; Rendering scratch
strip_y:        dw 0
strip_z:        dw 0
strip_hw:       dw 0                ; Road half-width
strip_cx:       dw 0                ; Road center X
road_left:      dw 0
road_right:     dw 0
road_color:     db 0
grass_color:    db 0
has_stripe:     db 0

; Strings
str_title:      db 'OUTRUN', 0
str_subtitle:   db 'CGA Racing Challenge', 0
str_inst1:      db 'Arrows: Steer/Speed', 0
str_inst2:      db 'ESC: Quit', 0
str_press_key:  db 'Press any key to start', 0
str_speed:      db 'Speed:', 0
str_score:      db 'Score:', 0
str_time:       db 'Time:', 0
str_gameover:   db 'GAME OVER', 0
str_final_score: db 'Final Score:', 0
num_buf:        times 8 db 0
