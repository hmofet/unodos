; CLOCK.BIN - Clock application for UnoDOS
; Displays RTC time in a draggable window, updates once per second
;
; Build: nasm -f bin -o clock.bin clock.asm

[BITS 16]
[ORG 0x0000]

; API function indices (must match kernel_api_table in kernel.asm)
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32

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

    ; Create clock window at X=100, Y=60, W=110, H=40
    mov bx, 100                     ; X position
    mov cx, 60                      ; Y position
    mov dx, 110                     ; Width (content=108, fits "00:00:00"=64px centered)
    mov si, 40                      ; Height
    mov ax, cs
    mov es, ax
    mov di, window_title            ; Title pointer (ES:DI)
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:win_handle], al         ; Save handle

    ; Set window drawing context - coordinates now relative to content area
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Initialize last_secs to impossible value to force first draw
    mov byte [cs:last_secs], 0xFF

    ; Main loop - update time only when seconds change
.main_loop:
    sti

    ; Read RTC time using BIOS INT 1Ah, AH=02h
    ; Returns: CH=hours (BCD), CL=minutes (BCD), DH=seconds (BCD)
    mov ah, 02h
    int 1Ah

    ; Only redraw if seconds changed
    cmp dh, [cs:last_secs]
    je .check_event
    mov [cs:last_secs], dh

    ; Save RTC values (before any calls clobber them)
    mov [cs:rtc_hours], ch
    mov [cs:rtc_mins], cl
    mov [cs:rtc_secs], dh

    ; Convert BCD time to ASCII string
    mov al, [cs:rtc_hours]
    call .bcd_to_ascii
    mov [cs:time_str], ah
    mov [cs:time_str+1], al

    mov al, [cs:rtc_mins]
    call .bcd_to_ascii
    mov [cs:time_str+3], ah
    mov [cs:time_str+4], al

    mov al, [cs:rtc_secs]
    call .bcd_to_ascii
    mov [cs:time_str+6], ah
    mov [cs:time_str+7], al

    ; Clear time area (window-relative, centered in 108px content)
    ; "00:00:00" = 8 chars × 12px advance = 96px wide
    mov bx, 6
    mov cx, 8
    mov dx, 98
    mov si, 10
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw time string (window-relative, centered: (108-96)/2 = 6)
    mov bx, 6
    mov cx, 8
    mov si, time_str
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.check_event:
    ; Check for keypress (non-blocking)
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_KEY_PRESS
    jne .no_event
    cmp dl, 27                      ; ESC key?
    je .exit_ok

.no_event:
    ; Short delay to avoid busy-spinning
    mov cx, 0x2000
.delay:
    loop .delay

    jmp .main_loop

; Convert BCD byte in AL to two ASCII digits
; Input: AL = BCD byte (e.g., 0x23 for 23)
; Output: AH = tens digit ASCII, AL = ones digit ASCII
.bcd_to_ascii:
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

; Data Section
window_title:   db 'Clock', 0
win_handle:     db 0
time_str:       db '00:00:00', 0
rtc_hours:      db 0
rtc_mins:       db 0
rtc_secs:       db 0
last_secs:      db 0xFF             ; Last displayed seconds (0xFF forces first draw)
