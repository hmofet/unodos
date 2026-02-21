; ============================================================================
; OUTRUNV.BIN - VGA Pseudo-3D Racing Game for UnoDOS
; Fullscreen game with 256-color VGA graphics, sky gradient,
; multi-color car, rumble strips. Supports all VGA modes.
; ============================================================================

[ORG 0x0000]

; --- BIN Header (80 bytes) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic
    db 'OutRun VGA', 0             ; App name (12 bytes padded)
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

    ; Save current video mode for restore on exit
    mov ah, 0x0F
    int 0x10
    mov [cs:saved_video_mode], al

    ; Switch to VGA mode 13h (320x200, 256 color)
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

    ; Set colors for VGA rendering
    mov al, 0
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
    jmp .no_event

.game_key:
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
    cmp byte [cs:game_state], STATE_PLAYING
    jne .main_loop

    ; Frame throttle
    mov ah, API_GET_TICK
    int 0x80
    cmp ax, [cs:last_tick]
    je .main_loop
    mov [cs:last_tick], ax

    ; Apply deceleration
    cmp word [cs:player_speed], 0
    je .no_decel
    dec word [cs:player_speed]
.no_decel:

    ; Advance camera
    mov ax, [cs:player_speed]
    shr ax, 1
    add [cs:camera_z], ax

    ; Update score
    mov ax, [cs:player_speed]
    shr ax, 2
    add [cs:score], ax

    ; Timer
    inc word [cs:timer_counter]
    cmp word [cs:timer_counter], 18
    jb .no_timer_dec
    mov word [cs:timer_counter], 0
    cmp word [cs:time_left], 0
    je .game_over_trigger
    dec word [cs:time_left]
.no_timer_dec:

    ; Update curve
    call update_curve

    ; Apply curve drift
    mov ax, [cs:current_curve]
    sar ax, 3
    movzx bx, byte [cs:player_speed + 1]
    test bx, bx
    jz .no_curve_drift
    imul bx
    sar ax, 3
.no_curve_drift:
    add [cs:player_x], ax

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
    call draw_car
    call draw_hud
    jmp .main_loop

.game_over_trigger:
    mov byte [cs:game_state], STATE_GAMEOVER
    call draw_game_over
    jmp .main_loop

.exit_game:
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

    ; Restore video mode
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
API_SET_VIDEO_MODE      equ 95

EVENT_KEY_PRESS         equ 1

STATE_TITLE             equ 0
STATE_PLAYING           equ 1
STATE_GAMEOVER          equ 2

MAX_SPEED               equ 60
ROAD_BASE_HW            equ 120     ; Road half-width at screen bottom
CAR_W                   equ 20
CAR_H                   equ 12
TRACK_SEGMENTS          equ 32
SEGMENT_LENGTH          equ 40

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
    mov word [cs:current_curve], 0
    mov byte [cs:game_state], STATE_PLAYING

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

    ; Divide horizon into 8 bands
    mov ax, [cs:horizon_y]
    xor dx, dx
    mov bx, 8
    div bx
    mov [cs:sky_band_h], ax         ; Height per band

    mov word [cs:sky_y], 0
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

    ; Start from horizon + 1
    mov ax, [cs:horizon_y]
    inc ax
    mov [cs:strip_y], ax

    ; Compute stop Y = car_y - 2
    mov ax, [cs:car_y]
    sub ax, 2
    mov [cs:stop_y], ax

.strip_loop:
    mov ax, [cs:strip_y]
    cmp ax, [cs:stop_y]
    jge .strips_done

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

    ; Road center = scr_w/2 + curve_offset
    mov ax, [cs:current_curve]
    imul word [cs:strip_z]
    sar ax, 8
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
    ; Left grass
    mov ax, [cs:road_left]
    cmp ax, 0
    je .skip_lg
    mov bx, 0
    mov cx, [cs:strip_y]
    mov dx, [cs:road_left]
    mov si, 1
    mov al, [cs:grass_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.skip_lg:

    ; Left rumble strip
    mov bx, [cs:road_left]
    mov cx, [cs:strip_y]
    mov dx, [cs:rumble_w]
    mov si, 1
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
    mov si, 1
    mov al, [cs:road_color]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.skip_road:

    ; Right rumble strip
    mov bx, [cs:road_right]
    sub bx, [cs:rumble_w]
    mov cx, [cs:strip_y]
    mov dx, [cs:rumble_w]
    mov si, 1
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
    mov si, 1
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
    mov si, 1
    mov al, CLR_ROAD_MARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

.next_strip:
    inc word [cs:strip_y]
    jmp .strip_loop

.strips_done:
    popa
    ret

; ============================================================================
; draw_car - Draw the player's car with VGA colors
; ============================================================================
draw_car:
    pusha

    ; Clear car area (draw road color underneath)
    mov bx, 0
    mov cx, [cs:car_y]
    sub cx, 2
    mov dx, [cs:scr_w]
    mov si, CAR_H + 6
    mov al, CLR_ROAD_LIGHT
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Car shadow (dark ellipse)
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2 + 2
    mov cx, [cs:car_y]
    add cx, CAR_H
    mov dx, CAR_W + 4
    mov si, 2
    mov al, CLR_CAR_WHEEL
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Car body (red)
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2
    mov cx, [cs:car_y]
    mov dx, CAR_W
    mov si, CAR_H
    mov al, CLR_CAR_BODY
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Dark accents on sides
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2
    mov cx, [cs:car_y]
    add cx, 2
    mov dx, 2
    mov si, CAR_H - 3
    mov al, CLR_CAR_DARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov bx, [cs:player_x]
    add bx, CAR_W / 2 - 2
    mov cx, [cs:car_y]
    add cx, 2
    mov dx, 2
    mov si, CAR_H - 3
    mov al, CLR_CAR_DARK
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Windshield (blue)
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2 - 3
    mov cx, [cs:car_y]
    mov dx, CAR_W - 6
    mov si, 3
    mov al, CLR_CAR_WIND
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Wheels (black)
    mov bx, [cs:player_x]
    sub bx, CAR_W / 2 - 1
    mov cx, [cs:car_y]
    add cx, CAR_H - 3
    mov dx, 3
    mov si, 3
    mov al, CLR_CAR_WHEEL
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    mov bx, [cs:player_x]
    add bx, CAR_W / 2 - 4
    mov cx, [cs:car_y]
    add cx, CAR_H - 3
    mov dx, 3
    mov si, 3
    mov al, CLR_CAR_WHEEL
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

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
; draw_title - Title screen
; ============================================================================
draw_title:
    pusha

    mov bx, 100
    mov cx, 60
    mov si, str_title
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 50
    mov cx, 80
    mov si, str_subtitle
    mov ah, API_GFX_DRAW_STRING
    int 0x80

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
car_y:          dw 175
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

; Track
current_curve:  dw 0

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

; Strings
str_title:      db 'OUTRUN VGA', 0
str_subtitle:   db 'VGA Racing Challenge', 0
str_inst1:      db 'Arrows: Steer/Speed', 0
str_inst2:      db 'ESC: Quit', 0
str_press_key:  db 'Press any key to start', 0
str_speed:      db 'Speed:', 0
str_score:      db 'Score:', 0
str_time:       db 'Time:', 0
str_gameover:   db 'GAME OVER', 0
str_final_score: db 'Final Score:', 0
num_buf:        times 8 db 0
