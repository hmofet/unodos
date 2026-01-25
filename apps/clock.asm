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

; Kernel segment
KERNEL_SEG              equ 0x1000

; Entry point - called by kernel via far CALL
entry:
    ; Save all registers
    pusha
    push ds
    push es

    ; Save our code segment for data access
    mov ax, cs
    mov [cs:our_seg], ax

    ; Discover kernel API via INT 0x80
    xor ax, ax
    int 0x80                        ; ES:BX = API table pointer
    mov [cs:api_off], bx            ; Save API table offset

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
; Call Kernel API Function
; Input: AH = API function index
;        Other registers = function parameters
; Output: Depends on function called
; ============================================================================
call_kernel_api:
    push bp
    mov bp, sp
    push di

    ; Get function offset from API table
    ; API table is at KERNEL_SEG:api_off
    ; Function pointer is at table + 8 + (index * 2)
    push es
    push bx
    mov bx, KERNEL_SEG
    mov es, bx
    mov bx, [cs:api_off]            ; BX = API table offset
    add bx, 8                       ; Skip header
    mov al, ah                      ; AL = function index
    xor ah, ah
    shl ax, 1                       ; AX = index * 2
    add bx, ax                      ; BX = offset to function pointer
    mov di, [es:bx]                 ; DI = function offset in kernel
    mov [cs:call_off], di           ; Save for far call
    pop bx                          ; Restore original BX
    pop es                          ; Restore original ES

    ; Do far call to KERNEL_SEG:function_offset
    call far [cs:call_ptr]

    pop di
    pop bp
    ret

call_ptr:
call_off:   dw 0                    ; Function offset (filled in)
call_seg:   dw KERNEL_SEG           ; Kernel segment (constant)

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

window_title:   db 'Clock', 0
time_string:    db '00:00:00', 0

our_seg:        dw 0
api_off:        dw 0
win_handle:     dw 0
content_x:      dw 0
content_y:      dw 0
content_w:      dw 0
content_h:      dw 0

rtc_hours:      db 0
rtc_minutes:    db 0
rtc_seconds:    db 0
