; CLOCK.BIN - Analog Clock for UnoDOS
; Displays analog clock face with hour/minute/second hands
; and digital time below, in a draggable window. Updates once per second.
;
; Build: nasm -f bin -o clock.bin clock.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Clock', 0                   ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    ; Clock face: cyan circle, white hands
    db 0x00, 0x55, 0x55, 0x00      ; Row 0:  ..cccccccc..
    db 0x05, 0x00, 0x00, 0x50      ; Row 1:  .c........c.
    db 0x14, 0x03, 0x00, 0x04      ; Row 2:  c..WW......c
    db 0x40, 0x03, 0x00, 0x01      ; Row 3:  c..WW......c
    db 0x40, 0x03, 0x00, 0x01      ; Row 4:  c..WW......c
    db 0x40, 0x03, 0x00, 0x01      ; Row 5:  c..WW......c
    db 0x40, 0x03, 0x00, 0x01      ; Row 6:  c..WW......c
    db 0x40, 0x03, 0xFF, 0x01      ; Row 7:  c..WWWWWWW.c
    db 0x40, 0x00, 0x00, 0x01      ; Row 8:  c..........c
    db 0x40, 0x00, 0x00, 0x01      ; Row 9:  c..........c
    db 0x40, 0x00, 0x00, 0x01      ; Row 10: c..........c
    db 0x40, 0x00, 0x00, 0x01      ; Row 11: c..........c
    db 0x14, 0x00, 0x00, 0x04      ; Row 12: c..........c
    db 0x05, 0x00, 0x00, 0x50      ; Row 13: .c........c.
    db 0x00, 0x55, 0x55, 0x00      ; Row 14: ..cccccccc..
    db 0x00, 0x00, 0x00, 0x00      ; Row 15

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API constants
API_GFX_DRAW_PIXEL      equ 0
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34
API_DRAW_LINE           equ 71
API_GET_RTC_TIME        equ 72

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Clock face layout (content-relative coordinates)
CENTER_X    equ 54                  ; Face center X (108/2)
CENTER_Y    equ 42                  ; Face center Y
SEC_RADIUS  equ 36                  ; Second hand length
MIN_RADIUS  equ 30                  ; Minute hand length
HR_RADIUS   equ 20                  ; Hour hand length
MARK_INNER  equ 34                  ; Hour marker inner radius
MARK_OUTER  equ 38                  ; Hour marker outer radius
DIGI_X      equ 22                  ; Digital time X ((108-64)/2)
DIGI_Y      equ 86                  ; Digital time Y (below face)

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create clock window: X=105, Y=44, W=110, H=108
    mov bx, 105
    mov cx, 44
    mov dx, 110                     ; Content width = 108
    mov si, 120                     ; Content height ~ 110
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:win_handle], al

    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    mov byte [cs:last_secs], 0xFF   ; Force first draw

; ---- Main Loop ----
.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; Read RTC time (CH=hours BCD, CL=minutes BCD, DH=seconds BCD)
    mov ah, API_GET_RTC_TIME
    int 0x80

    ; Only redraw when seconds change
    cmp dh, [cs:last_secs]
    je .check_event
    mov [cs:last_secs], dh

    ; Save BCD time values
    mov [cs:rtc_hours], ch
    mov [cs:rtc_mins], cl
    mov [cs:rtc_secs], dh

    ; Convert BCD to binary for hand position math
    mov al, ch
    call .bcd_to_bin
    mov [cs:bin_hours], al

    mov al, [cs:rtc_mins]
    call .bcd_to_bin
    mov [cs:bin_mins], al

    mov al, [cs:rtc_secs]
    call .bcd_to_bin
    mov [cs:bin_secs], al

    ; Compute second hand position (0-59)
    mov al, [cs:bin_secs]
    mov [cs:sec_pos], al

    ; Compute minute hand position (0-59)
    mov al, [cs:bin_mins]
    mov [cs:min_pos], al

    ; Compute hour hand position: (hours%12)*5 + minutes/12
    mov al, [cs:bin_hours]
    cmp al, 12
    jb .hr_ok
    sub al, 12
.hr_ok:
    mov cl, 5
    mul cl                          ; AL = hours*5 (max 55)
    mov bl, al                      ; save in BL
    mov al, [cs:bin_mins]
    xor ah, ah
    mov cl, 12
    div cl                          ; AL = minutes/12 (0-4)
    add al, bl
    mov [cs:hr_pos], al

    ; --- Redraw clock face ---

    ; Clear entire content area
    mov bx, 0
    mov cx, 0
    mov dx, 108
    mov si, 96
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw 12 hour markers
    call .draw_markers

    ; Draw hour hand (white)
    mov al, [cs:hr_pos]
    mov bl, HR_RADIUS
    call .compute_endpoint          ; DX=x2, SI=y2
    mov bx, CENTER_X
    mov cx, CENTER_Y
    mov al, 3                       ; white
    mov ah, API_DRAW_LINE
    int 0x80

    ; Draw minute hand (white)
    mov al, [cs:min_pos]
    mov bl, MIN_RADIUS
    call .compute_endpoint
    mov bx, CENTER_X
    mov cx, CENTER_Y
    mov al, 3
    mov ah, API_DRAW_LINE
    int 0x80

    ; Draw second hand (cyan)
    mov al, [cs:sec_pos]
    mov bl, SEC_RADIUS
    call .compute_endpoint
    mov bx, CENTER_X
    mov cx, CENTER_Y
    mov al, 1                       ; cyan
    mov ah, API_DRAW_LINE
    int 0x80

    ; Draw center dot (white pixel)
    mov bx, CENTER_X
    mov cx, CENTER_Y
    mov al, 3
    mov ah, API_GFX_DRAW_PIXEL
    int 0x80

    ; Format and draw digital time "HH:MM:SS"
    call .format_time
    mov bx, DIGI_X
    mov cx, DIGI_Y
    mov si, time_str
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.check_event:
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    mov byte [cs:last_secs], 0xFF   ; Force redraw on next loop
    jmp .main_loop
.not_redraw:
    cmp al, EVENT_KEY_PRESS
    jne .no_event
    cmp dl, 27                      ; ESC key?
    je .exit_ok
.no_event:
    jmp .main_loop

; ---- Subroutines ----

; compute_endpoint: Convert clock position + radius to screen coordinates
; Input:  AL = position (0-59), BL = radius (pixels)
; Output: DX = X coordinate, SI = Y coordinate (content-relative)
; Preserves: AX, BX, CX
.compute_endpoint:
    push ax
    push bx
    push cx
    push di

    ; X = CENTER_X + sin(pos) * radius / 32
    xor ah, ah
    mov di, ax                      ; DI = position index
    mov al, [cs:sin_table + di]     ; signed byte: sin value
    imul bl                         ; AX = sin * radius (signed)
    sar ax, 5                       ; /32
    add ax, CENTER_X
    mov dx, ax                      ; DX = X result

    ; Y = CENTER_Y - cos(pos) * radius / 32
    ; cos(pos) = sin((pos + 15) % 60)
    add di, 15
    cmp di, 60
    jb .ce_no_wrap
    sub di, 60
.ce_no_wrap:
    mov al, [cs:sin_table + di]     ; signed byte: cos value
    imul bl                         ; AX = cos * radius (signed)
    sar ax, 5                       ; /32
    neg ax                          ; -cos (screen Y is inverted)
    add ax, CENTER_Y
    mov si, ax                      ; SI = Y result

    pop di
    pop cx
    pop bx
    pop ax
    ret

; draw_markers: Draw 12 hour markers around the face
.draw_markers:
    push ax
    push bx
    push cx
    push dx
    push si

    xor cx, cx                      ; CX = marker index (0-11)
.marker_loop:
    ; Position = index * 5 (maps to 0, 5, 10, ... 55)
    mov ax, cx
    push cx                         ; save loop counter
    mov cl, 5
    mul cl                          ; AL = clock position

    ; Compute inner endpoint
    mov bl, MARK_INNER
    call .compute_endpoint          ; DX=inner_x, SI=inner_y
    mov [cs:temp_x], dx
    mov [cs:temp_y], si

    ; Compute outer endpoint (AL preserved by compute_endpoint)
    mov bl, MARK_OUTER
    call .compute_endpoint          ; DX=outer_x, SI=outer_y

    ; Draw marker line: inner → outer (white)
    mov bx, [cs:temp_x]
    mov cx, [cs:temp_y]
    ; DX, SI = outer point (already set)
    mov al, 3                       ; white
    mov ah, API_DRAW_LINE
    int 0x80

    pop cx                          ; restore loop counter
    inc cx
    cmp cx, 12
    jb .marker_loop

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; bcd_to_bin: Convert BCD byte to binary
; Input:  AL = BCD value (e.g. 0x23 = 23 decimal)
; Output: AL = binary value (e.g. 23)
; Clobbers: AH
.bcd_to_bin:
    push cx
    mov cl, al
    and cl, 0x0F                    ; CL = ones digit
    shr al, 4                       ; AL = tens digit
    mov ah, 10
    mul ah                          ; AX = tens * 10
    add al, cl                      ; AL = binary
    pop cx
    ret

; format_time: Build "HH:MM:SS" string from BCD values
.format_time:
    push ax
    mov al, [cs:rtc_hours]
    call .bcd_to_ascii_pair
    mov [cs:time_str], ah
    mov [cs:time_str+1], al
    mov al, [cs:rtc_mins]
    call .bcd_to_ascii_pair
    mov [cs:time_str+3], ah
    mov [cs:time_str+4], al
    mov al, [cs:rtc_secs]
    call .bcd_to_ascii_pair
    mov [cs:time_str+6], ah
    mov [cs:time_str+7], al
    pop ax
    ret

; bcd_to_ascii_pair: Convert BCD byte to two ASCII chars
; Input:  AL = BCD byte
; Output: AH = tens digit char, AL = ones digit char
.bcd_to_ascii_pair:
    mov ah, al
    and al, 0x0F
    shr ah, 4
    add al, '0'
    add ah, '0'
    ret

.exit_ok:
    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80
    xor ax, ax
    jmp .exit

.exit_fail:
    mov ax, 1

.exit:
    pop es
    pop ds
    popa
    retf

; ---- Data Section ----
window_title:   db 'Clock', 0
win_handle:     db 0
time_str:       db '00:00:00', 0
rtc_hours:      db 0
rtc_mins:       db 0
rtc_secs:       db 0
last_secs:      db 0xFF
bin_hours:      db 0
bin_mins:       db 0
bin_secs:       db 0
sec_pos:        db 0
min_pos:        db 0
hr_pos:         db 0
temp_x:         dw 0
temp_y:         dw 0

; Sine lookup table: 60 entries (signed bytes, scale factor 32)
; sin_table[i] = round(sin(i * 6 degrees) * 32) for i = 0..59
; cos(pos) = sin_table[(pos + 15) % 60]
sin_table:
    db   0,  3,  7, 10, 13, 16, 19, 21, 24, 26, 28, 29, 30, 31, 32, 32
    db  32, 31, 30, 29, 28, 26, 24, 21, 19, 16, 13, 10,  7,  3,  0
    db  -3, -7,-10,-13,-16,-19,-21,-24,-26,-28,-29,-30,-31,-32,-32
    db -32,-31,-30,-29,-28,-26,-24,-21,-19,-16,-13,-10, -7, -3
