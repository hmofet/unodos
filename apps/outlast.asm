; ============================================================================
; OUTLAST.BIN - Pseudo-3D Racing Game for UnoDOS (CGA version)
; Fullscreen (non-windowed) game with perspective road rendering
; ============================================================================

[ORG 0x0000]

; --- BIN Header (80 bytes) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic
    db 'OutLast', 0                 ; App name (12 bytes padded)
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
    mov si, song_title
    call start_song
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
    je .main_loop                   ; Same tick, skip
    mov [cs:last_tick], ax

    ; Music plays in all states
    call play_music_tick

    ; Game logic only during gameplay
    cmp byte [cs:game_state], STATE_PLAYING
    jne .main_loop

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
    ; Heavy slowdown on grass
    cmp word [cs:player_speed], 5
    jle .grass_min
    sub word [cs:player_speed], 3
    jmp .not_on_grass
.grass_min:
    mov word [cs:player_speed], 2
.not_on_grass:

    ; Curve drift: road curves push the car sideways
    call update_curve
    mov ax, [cs:current_curve]
    sar ax, 3                        ; Drift = curve / 8 pixels per frame
    sub [cs:player_x], ax            ; Positive curve = right turn = car drifts left

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
    call stop_music
    call draw_game_over
    jmp .main_loop

.exit_game:
    call stop_music

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
API_SPEAKER_TONE        equ 41
API_SPEAKER_OFF         equ 42
API_WORD_TO_STRING      equ 91

; Title bitmap font constants
TITLE_BLK_W             equ 6
TITLE_BLK_H             equ 5
TITLE_GAP               equ 4
TITLE_FONT_ROWS         equ 7
TITLE_START_X           equ 43
TITLE_START_Y           equ 10

; Note frequencies (Hz)
NOTE_REST               equ 0
NOTE_A3                 equ 220
NOTE_B3                 equ 247
NOTE_C4                 equ 262
NOTE_D4                 equ 294
NOTE_E4                 equ 330
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

EVENT_KEY_PRESS         equ 1

STATE_TITLE             equ 0
STATE_PLAYING           equ 1
STATE_GAMEOVER          equ 2

MAX_SPEED               equ 60
HORIZON_Y               equ 80
ROAD_BASE_W             equ 22      ; Base road half-width at bottom
CAR_Y                   equ 168     ; Car Y position
CAR_W                   equ 32
CAR_H                   equ 20
TRACK_SEGMENTS          equ 32
SEGMENT_LENGTH          equ 80      ; Longer segments = longer sustained curves

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
    mov byte [cs:decel_counter], 0
    mov word [cs:current_curve], 0
    mov byte [cs:game_state], STATE_PLAYING

    ; Initialize road edges at car Y to prevent false grass detection
    mov word [cs:road_left_at_car], 60
    mov word [cs:road_right_at_car], 260

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

    ; Draw sky (below HUD area to horizon)
    mov bx, 0
    mov cx, 12                       ; Start below HUD to avoid flicker
    mov dx, 320
    mov si, HORIZON_Y - 12
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

    ; Initialize curve accumulator - starts at 0 for smooth progressive curve
    mov word [cs:curve_accum], 0

    ; Draw road strips bottom-to-top (near to far), stop at car area
    mov word [cs:strip_y], CAR_Y - 1

.strip_loop:
    cmp word [cs:strip_y], HORIZON_Y
    jle .strips_done

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

    ; Road center = 160 + (curve_accum >> 5)
    mov ax, [cs:curve_accum]
    sar ax, 5
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

    ; Save road edges at car Y position for grass collision check
    cmp word [cs:strip_y], CAR_Y
    jne .not_car_y
    mov ax, [cs:road_left]
    mov [cs:road_left_at_car], ax
    mov ax, [cs:road_right]
    mov [cs:road_right_at_car], ax
.not_car_y:

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
    ; Draw left grass (2px tall strips to halve API calls / reduce flicker)
    mov ax, [cs:road_left]
    cmp ax, 0
    je .skip_left_grass
    mov bx, 0
    mov cx, [cs:strip_y]
    mov dx, [cs:road_left]
    mov si, 2
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
    mov si, 2
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
    mov si, 2
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
    mov si, 2
    mov al, 0                       ; Black center stripe
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

.next_strip:
    sub word [cs:strip_y], 2
    jmp .strip_loop

.strips_done:
    ; Draw bottom section (CAR_Y to screen bottom) using saved road edges
    ; Left grass
    mov bx, 0
    mov cx, CAR_Y
    mov dx, [cs:road_left_at_car]
    cmp dx, 0
    je .skip_bot_lg
    mov si, 200 - CAR_Y
    mov al, 1                       ; Cyan = grass
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.skip_bot_lg:
    ; Road surface
    mov bx, [cs:road_left_at_car]
    mov cx, CAR_Y
    mov dx, [cs:road_right_at_car]
    sub dx, bx
    cmp dx, 0
    jle .skip_bot_road
    mov si, 200 - CAR_Y
    mov al, 3                       ; White = road
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.skip_bot_road:
    ; Right grass
    mov bx, [cs:road_right_at_car]
    mov cx, CAR_Y
    mov dx, 320
    sub dx, bx
    cmp dx, 0
    jle .skip_bot_rg
    mov si, 200 - CAR_Y
    mov al, 1                       ; Cyan = grass
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.skip_bot_rg:

    popa
    ret

; ============================================================================
; draw_car - Draw the player's car (32x20)
; ============================================================================
draw_car:
    pusha

    ; Car shadow
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2 + 2
    mov cx, CAR_Y + CAR_H
    mov dx, CAR_W + 4
    mov si, 2
    mov al, 0                       ; Black
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
    sub bx, CAR_W / 2 - 4
    mov cx, CAR_Y
    mov dx, CAR_W - 8
    mov si, 5
    mov al, 1                       ; Cyan
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Wheels (black) - 5x5 each
    ; Left wheel
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2 - 2
    mov cx, CAR_Y + CAR_H - 5
    mov dx, 5
    mov si, 5
    mov al, 0                       ; Black
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; Right wheel
    mov bx, [cs:player_x]
    add bx, CAR_W / 2 - 7
    mov cx, CAR_Y + CAR_H - 5
    mov dx, 5
    mov si, 5
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
; draw_title - Graphical title screen with road, car, bitmap title (CGA)
; ============================================================================
draw_title:
    pusha

    ; === Night sky (black background already cleared) ===
    ; Horizon line (cyan)
    mov bx, 0
    mov cx, 100
    mov dx, 320
    mov al, 1                       ; Cyan
    mov ah, API_DRAW_HLINE
    int 0x80
    mov cx, 101
    mov ah, API_DRAW_HLINE
    int 0x80

    ; === Road perspective (10 bands from horizon to bottom) ===
    mov word [cs:strip_y], 102      ; Reuse scratch
    mov byte [cs:has_stripe], 10    ; Band counter (reuse scratch)

.title_road_band:
    cmp byte [cs:has_stripe], 0
    je .title_road_done

    ; halfwidth = 10 + (y - 102)
    mov ax, [cs:strip_y]
    sub ax, 102
    add ax, 10

    mov bx, 160
    sub bx, ax
    mov cx, [cs:strip_y]
    shl ax, 1
    mov dx, ax
    mov si, 10
    mov al, 3                       ; White (road)
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    add word [cs:strip_y], 10
    dec byte [cs:has_stripe]
    jmp .title_road_band
.title_road_done:

    ; === Center line dashes ===
    mov bx, 159
    mov cx, 108
    mov dx, 2
    mov si, 6
    mov al, 0                       ; Black (on white road = contrast)
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    mov bx, 158
    mov cx, 124
    mov dx, 3
    mov si, 8
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    mov bx, 157
    mov cx, 142
    mov dx, 5
    mov si, 10
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    mov bx, 155
    mov cx, 162
    mov dx, 7
    mov si, 12
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; === Car silhouette (rear view, centered) ===

    ; Shadow
    mov bx, 120
    mov cx, 182
    mov dx, 80
    mov si, 4
    mov al, 0                       ; Black
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Roof
    mov bx, 147
    mov cx, 148
    mov dx, 26
    mov si, 5
    mov al, 2                       ; Magenta
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Rear window
    mov bx, 142
    mov cx, 153
    mov dx, 36
    mov si, 7
    mov al, 1                       ; Cyan
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Body
    mov bx, 130
    mov cx, 160
    mov dx, 60
    mov si, 16
    mov al, 2                       ; Magenta
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Bumper
    mov bx, 128
    mov cx, 176
    mov dx, 64
    mov si, 4
    mov al, 0                       ; Black
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Left wheel
    mov bx, 120
    mov cx, 170
    mov dx, 10
    mov si, 14
    mov al, 0                       ; Black
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Right wheel
    mov bx, 190
    mov cx, 170
    mov dx, 10
    mov si, 14
    mov al, 0                       ; Black
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; === Title text "OUTLAST" ===

    ; Shadow pass (offset +2, +2, cyan)
    mov byte [cs:title_draw_clr], 1 ; Cyan shadow
    mov word [cs:title_draw_x], TITLE_START_X + 2
    mov word [cs:title_draw_y], TITLE_START_Y + 2
    call render_bitmap_title

    ; Main pass (white)
    mov byte [cs:title_draw_clr], 3 ; White
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
; render_bitmap_title - Draw "OUTLAST" using bitmap font data (CGA)
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

    push si

    ; Get font data offset
    movzx ax, byte [cs:title_order + si]
    mov bl, TITLE_FONT_ROWS
    mul bl
    mov bp, ax

    mov cx, [cs:title_draw_y]
    xor di, di

.rt_row_loop:
    cmp di, TITLE_FONT_ROWS
    jge .rt_letter_done

    mov al, [cs:title_font + bp]
    inc bp

    test al, al
    jz .rt_empty_row

    push bp
    push di
    push cx

    mov dl, al
    mov dh, 5
    mov bx, [cs:title_cur_x]

.rt_col_loop:
    test dl, 0x10
    jz .rt_no_block

    push bx
    push cx
    push dx

    mov dx, TITLE_BLK_W
    mov si, TITLE_BLK_H
    mov al, [cs:title_draw_clr]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    pop dx
    pop cx
    pop bx

.rt_no_block:
    add bx, TITLE_BLK_W
    shl dl, 1
    dec dh
    jnz .rt_col_loop

    pop cx
    pop di
    pop bp

.rt_empty_row:
    add cx, TITLE_BLK_H
    inc di
    jmp .rt_row_loop

.rt_letter_done:
    add word [cs:title_cur_x], (5 * TITLE_BLK_W) + TITLE_GAP
    pop si
    inc si
    jmp .rt_letter_loop

.rt_all_done:
    popa
    ret

; ============================================================================
; Music system - PC speaker note player
; ============================================================================

start_song:
    mov [cs:music_song], si
    mov [cs:music_ptr], si
    mov word [cs:music_ticks], 0
    ret

stop_music:
    push ax
    mov ah, API_SPEAKER_OFF
    int 0x80
    mov word [cs:music_ticks], 0
    mov word [cs:music_ptr], 0
    pop ax
    ret

play_music_tick:
    pusha

    cmp word [cs:music_ptr], 0
    je .mt_done

    cmp word [cs:music_ticks], 0
    jne .mt_counting

    mov si, [cs:music_ptr]
    mov bx, [cs:si]
    cmp bx, 0xFFFF
    jne .mt_not_end

    mov si, [cs:music_song]
    mov bx, [cs:si]

.mt_not_end:
    mov cx, [cs:si + 2]
    mov [cs:music_ticks], cx
    add si, 4
    mov [cs:music_ptr], si

    cmp bx, NOTE_REST
    je .mt_rest

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
    dw 5, 15, 25, 35               ; Segments 4-7: gentle entry into right
    dw 40, 45, 45, 40              ; Segments 8-11: sustained right curve
    dw 35, 25, 12, 5               ; Segments 12-15: ease out of right
    dw 0, 0, 0, 0                  ; Segments 16-19: straight
    dw -5, -15, -25, -35           ; Segments 20-23: gentle entry into left
    dw -40, -45, -45, -40          ; Segments 24-27: sustained left curve
    dw -35, -25, -12, 0            ; Segments 28-31: ease out of left

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
decel_counter:  db 0                ; Decelerate every 3rd frame

; Current track state
current_curve:  dw 0                ; Current curve value
curve_accum:    dw 0                ; Per-frame curve accumulator
road_left_at_car:  dw 0            ; Road edge at car Y for grass check
road_right_at_car: dw 0

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

; Bitmap font for "OUTLAST" title (6 unique letters x 7 rows)
title_font:
    db 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E  ; O
    db 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E  ; U
    db 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04  ; T
    db 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F  ; L
    db 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11  ; A
    db 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E  ; S

title_order:    db 0, 1, 2, 3, 4, 5, 2

; Title renderer scratch
title_draw_x:   dw 0
title_draw_y:   dw 0
title_draw_clr: db 0
title_cur_x:    dw 0

; Music state
music_ptr:      dw 0
music_song:     dw 0
music_ticks:    dw 0
game_song_idx:  db 0

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
