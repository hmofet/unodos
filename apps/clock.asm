; CLOCK.BIN - Clock application for UnoDOS v3.12.0
; Build 028 - Clear before drawing time
;
; Build: nasm -f bin -o clock.bin clock.asm

[BITS 16]
[ORG 0x0000]

; API function indices (must match kernel_api_table in kernel.asm)
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_GET_CONTENT     equ 25

; Event types
EVENT_KEY_PRESS         equ 1

; Entry point - called by kernel via far CALL
entry:
    pusha
    push ds
    push es

    ; Save our code segment
    mov ax, cs
    mov ds, ax

    ; Create clock window at X=100, Y=60, W=120, H=50
    mov bx, 100                     ; X position
    mov cx, 60                      ; Y position
    mov dx, 120                     ; Width
    mov si, 50                      ; Height
    mov ax, cs
    mov es, ax
    mov di, window_title            ; Title pointer (ES:DI)
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:win_handle], al         ; Save handle

    ; Get content area coordinates
    mov al, [cs:win_handle]
    mov ah, API_WIN_GET_CONTENT
    int 0x80
    jc .exit_fail

    ; Save content area (BX=X, CX=Y, DX=Width, SI=Height)
    mov [cs:content_x], bx
    mov [cs:content_y], cx

    ; Main loop - update time and check for ESC
.main_loop:
    ; Read RTC time using BIOS INT 1Ah, AH=02h
    ; Returns: CH=hours (BCD), CL=minutes (BCD), DH=seconds (BCD)
    mov ah, 02h
    int 1Ah
    ; Ignore CF - some BIOSes set it during RTC update, values still valid

    ; Save RTC values immediately (before any calls clobber them)
    mov [cs:rtc_hours], ch
    mov [cs:rtc_mins], cl
    mov [cs:rtc_secs], dh

    ; Convert BCD time to ASCII string
    ; Hours
    mov al, [cs:rtc_hours]
    call .bcd_to_ascii
    mov [cs:time_str], ah           ; Tens digit
    mov [cs:time_str+1], al         ; Ones digit

    ; Minutes
    mov al, [cs:rtc_mins]
    call .bcd_to_ascii
    mov [cs:time_str+3], ah         ; Tens digit
    mov [cs:time_str+4], al         ; Ones digit

    ; Seconds
    mov al, [cs:rtc_secs]
    call .bcd_to_ascii
    mov [cs:time_str+6], ah         ; Tens digit
    mov [cs:time_str+7], al         ; Ones digit

    ; Clear time area before drawing (prevents ghosting)
    ; gfx_clear_area: BX=X, CX=Y, DX=Width, SI=Height
    mov bx, [cs:content_x]
    add bx, 27                      ; Same X offset as text
    mov cx, [cs:content_y]
    add cx, 10                      ; Same Y offset as text
    mov dx, 100                     ; Width (8 chars * ~12 pixels)
    mov si, 10                      ; Height (8 pixel font + margin)
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw time string at content area + offset for centering
    mov bx, [cs:content_x]
    add bx, 27                      ; Center horizontally
    mov cx, [cs:content_y]
    add cx, 10                      ; Down a bit from top
    mov si, time_str
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Check for keypress (non-blocking)
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_KEY_PRESS
    jne .no_event
    cmp dl, 27                      ; ESC key?
    je .exit_ok

.no_event:
    ; Short delay - just enough to not spin too fast
    mov cx, 0x4000
.delay:
    loop .delay

    jmp .main_loop

; Convert BCD byte in AL to two ASCII digits
; Input: AL = BCD byte (e.g., 0x23 for 23)
; Output: AH = tens digit ASCII, AL = ones digit ASCII
.bcd_to_ascii:
    mov ah, al
    and al, 0x0F                    ; Low nibble (ones)
    shr ah, 4                       ; High nibble (tens)
    add al, '0'
    add ah, '0'
    ret

.exit_ok:
    xor ax, ax
    jmp .exit

.exit_fail:
    mov ax, 1

.exit:
    pop es
    pop ds
    popa
    retf

; Data Section
window_title:   db 'Clock', 0
win_handle:     dw 0
content_x:      dw 0
content_y:      dw 0
time_str:       db '00:00:00', 0
rtc_hours:      db 0
rtc_mins:       db 0
rtc_secs:       db 0
