; CLOCK.BIN - Clock application for UnoDOS v3.12.0
; First real windowed application - displays time in a window
;
; Build: nasm -f bin -o clock.bin clock.asm
; Copy to FAT12 floppy as CLOCK.BIN

[BITS 16]
[ORG 0x0000]

; API function indices
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 8
API_WIN_CREATE          equ 19
API_WIN_GET_CONTENT     equ 24

; Event types
EVENT_KEY_PRESS         equ 1

; Entry point - called by kernel via far CALL
entry:
    ; Save all registers
    pusha
    push ds
    push es

    ; DEBUG: Write directly to video memory to prove app executes
    ; This avoids INT 0x80 to isolate the problem
    push es
    push di
    mov ax, 0xB800
    mov es, ax
    mov di, 0x0C80 + 10             ; Y=80, X=40
    mov byte [es:di], 0xFF          ; White pixels
    mov byte [es:di+1], 0xFF
    mov byte [es:di+2], 0xFF
    mov byte [es:di+3], 0xFF
    mov di, 0x2000 + 0x0C80 + 10    ; Y=81 (odd line)
    mov byte [es:di], 0xFF
    mov byte [es:di+1], 0xFF
    mov byte [es:di+2], 0xFF
    mov byte [es:di+3], 0xFF
    pop di
    pop es

    ; Save our code segment for data access
    mov ax, cs
    mov [cs:our_seg], ax

    ; DEBUG: Test string access from app segment
    mov ax, cs
    mov ds, ax
    mov si, debug_msg

    ; Read first character and write to Y=84 (even scanline)
    ; Y=84 offset = (84/2)*80 = 42*80 = 3360 = 0x0D20
    push es
    push di
    mov ax, 0xB800
    mov es, ax
    lodsb                           ; AL = first char ('A' = 0x41)
    mov di, 0x0D20 + 10             ; Y=84, X=40
    mov [es:di], al
    mov [es:di+1], al
    mov [es:di+2], al
    mov [es:di+3], al
    ; Also Y=85 (odd scanline) at 0x2000 base
    mov di, 0x2000 + 0x0D20 + 10    ; Y=85
    mov [es:di], al
    mov [es:di+1], al
    mov [es:di+2], al
    mov [es:di+3], al
    pop di
    pop es

    ; Test simpler API: draw single pixel via INT 0x80
    ; API 0 = gfx_draw_pixel_stub: CX=X, BX=Y, AL=color
    mov cx, 160                     ; X = center of screen
    mov bx, 100                     ; Y = center of screen
    mov al, 3                       ; color = white
    mov ah, 0                       ; API_GFX_DRAW_PIXEL
    int 0x80

    ; Draw a few more pixels to make it visible
    mov cx, 161
    int 0x80
    mov cx, 162
    int 0x80
    mov cx, 163
    int 0x80
    mov cx, 164
    int 0x80

    ; DEBUG 2: Write after INT 0x80 to prove it returned
    push es
    push di
    mov ax, 0xB800
    mov es, ax
    mov di, 0x0C80 + 20             ; Y=80, X=80 (right of first bar)
    mov byte [es:di], 0xAA          ; Different pattern
    mov byte [es:di+1], 0xAA
    mov byte [es:di+2], 0xAA
    mov byte [es:di+3], 0xAA
    pop di
    pop es

    ; Create clock window
    mov bx, 100                     ; X position
    mov cx, 60                      ; Y position
    mov dx, 120                     ; Width
    mov si, 50                      ; Height
    mov ax, cs
    mov es, ax
    mov di, window_title            ; Title pointer (ES:DI)
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    call call_kernel_api
    jc .exit_fail
    mov [cs:win_handle], ax         ; Save window handle

    ; Get content area bounds
    mov ah, API_WIN_GET_CONTENT
    call call_kernel_api
    jc .exit_fail
    mov [cs:content_x], bx
    mov [cs:content_y], cx
    mov [cs:content_w], dx
    mov [cs:content_h], si

    ; Main loop
.main_loop:
    ; Check for ESC key (non-blocking)
    mov ah, API_EVENT_GET
    call call_kernel_api
    jc .no_event                    ; No event available
    cmp al, EVENT_KEY_PRESS
    jne .no_event
    cmp dl, 27                      ; ESC key?
    je .exit_ok

.no_event:
    ; Read RTC time
    call read_rtc_time
    jc .skip_update                 ; RTC error, skip

    ; Format time string
    call format_time

    ; Clear content area for time display
    mov bx, [cs:content_x]
    add bx, 20                      ; Padding from left
    mov cx, [cs:content_y]
    add cx, 10                      ; Padding from top
    mov dx, 80                      ; Width to clear
    mov si, 12                      ; Height to clear
    mov ah, API_GFX_CLEAR_AREA
    call call_kernel_api

    ; Draw time string
    mov bx, [cs:content_x]
    add bx, 24                      ; Center the time
    mov cx, [cs:content_y]
    add cx, 12                      ; Vertical center
    mov ax, cs
    mov ds, ax
    mov si, time_string             ; DS:SI = string
    mov ah, API_GFX_DRAW_STRING
    call call_kernel_api

.skip_update:
    ; Simple delay loop
    mov cx, 0xFFFF
.delay:
    nop
    loop .delay

    jmp .main_loop

.exit_ok:
    xor ax, ax                      ; Return 0 = success
    jmp .exit

.exit_fail:
    mov ax, 1                       ; Return 1 = error

.exit:
    pop es
    pop ds
    popa
    retf                            ; Far return to kernel

; ============================================================================
; Call Kernel API Function via INT 0x80
; Input: AH = API function index
;        Other registers = function parameters
; Output: Depends on function called
; ============================================================================
call_kernel_api:
    int 0x80                        ; Kernel dispatches based on AH
    ret

; ============================================================================
; Read RTC time via BIOS
; Output: BCD values stored, CF=1 on error
; ============================================================================
read_rtc_time:
    push ax
    push cx
    push dx

    mov ah, 0x02                    ; BIOS: Read RTC time
    int 0x1A
    jc .rtc_error

    mov [cs:rtc_hours], ch
    mov [cs:rtc_minutes], cl
    mov [cs:rtc_seconds], dh
    clc
    jmp .rtc_done

.rtc_error:
    stc

.rtc_done:
    pop dx
    pop cx
    pop ax
    ret

; ============================================================================
; Format time as "HH:MM:SS" string
; ============================================================================
format_time:
    push ax
    push bx
    push di
    push es

    mov ax, cs
    mov es, ax
    mov di, time_string

    ; Hours
    mov al, [cs:rtc_hours]
    call bcd_to_ascii
    stosw                           ; Store both digits

    ; Colon
    mov al, ':'
    stosb

    ; Minutes
    mov al, [cs:rtc_minutes]
    call bcd_to_ascii
    stosw

    ; Colon
    mov al, ':'
    stosb

    ; Seconds
    mov al, [cs:rtc_seconds]
    call bcd_to_ascii
    stosw

    ; Null terminator
    xor al, al
    stosb

    pop es
    pop di
    pop bx
    pop ax
    ret

; ============================================================================
; Convert BCD byte to two ASCII digits
; Input: AL = BCD value
; Output: AX = ASCII digits (AH=tens, AL=ones) for stosw
; ============================================================================
bcd_to_ascii:
    push bx
    mov bl, al
    shr al, 4                       ; Tens digit
    add al, '0'
    mov ah, al                      ; AH = tens
    mov al, bl
    and al, 0x0F                    ; Ones digit
    add al, '0'                     ; AL = ones
    xchg ah, al                     ; For stosw: first char in AL, second in AH
    pop bx
    ret

; ============================================================================
; Data Section
; ============================================================================

debug_msg:      db 'APP RUNNING', 0
window_title:   db 'Clock', 0
time_string:    db '00:00:00', 0

our_seg:        dw 0
win_handle:     dw 0
content_x:      dw 0
content_y:      dw 0
content_w:      dw 0
content_h:      dw 0

rtc_hours:      db 0
rtc_minutes:    db 0
rtc_seconds:    db 0
