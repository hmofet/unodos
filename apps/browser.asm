; BROWSER.BIN - File browser for UnoDOS v3.12.0
; Build 047 - Debug: loop counter + event detection
;
; Build: nasm -f bin -o browser.bin browser.asm

[BITS 16]
[ORG 0x0000]

; API function indices
API_GFX_DRAW_CHAR       equ 3
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 8
API_FS_MOUNT            equ 12
API_FS_READDIR          equ 26
API_WIN_CREATE          equ 19
API_WIN_DESTROY         equ 20
API_WIN_GET_CONTENT     equ 24

; Event types
EVENT_KEY_PRESS         equ 1

; Display constants
LINE_HEIGHT             equ 10          ; Pixels per line
LEFT_PADDING            equ 4           ; Left margin
TOP_PADDING             equ 2           ; Top margin
MAX_DISPLAY_FILES       equ 8           ; Max files to show

; Entry point
entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create browser window (wider for filename + size)
    call create_window
    jc .exit_fail

    ; Drain pending events
.drain_events:
    mov ah, API_EVENT_GET
    int 0x80
    test al, al
    jnz .drain_events

    ; Scan and display files
    call scan_and_display

    ; DEBUG: Show marker that we reached main loop
    mov bx, 300                     ; X position (right side)
    mov cx, 10                      ; Y position (top)
    mov al, '!'                     ; Marker character
    mov ah, API_GFX_DRAW_CHAR
    int 0x80

    ; DEBUG: Show loop counter to prove loop runs
    mov byte [cs:loop_counter], 0

    ; Event loop - wait for ESC
.main_loop:
    ; Increment and show loop counter (proves loop is running)
    inc byte [cs:loop_counter]
    mov al, [cs:loop_counter]
    and al, 0x0F                    ; Keep low nibble
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .show_count
.digit:
    add al, '0'
.show_count:
    mov bx, 305
    mov cx, 10
    mov ah, API_GFX_DRAW_CHAR
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    ; Got an event! Show '+' at position 315
    push ax
    push dx
    mov bx, 315
    mov cx, 10
    mov al, '+'
    mov ah, API_GFX_DRAW_CHAR
    int 0x80
    pop dx
    pop ax

    cmp al, EVENT_KEY_PRESS
    jne .no_event

    ; Show key code at position 320
    push dx
    mov bx, 320                     ; X position (off screen on 320-wide, but try)
    mov cx, 10
    mov al, dl
    mov ah, API_GFX_DRAW_CHAR
    int 0x80
    pop dx

    cmp dl, 27                      ; ESC?
    je .exit_ok

.no_event:
    mov cx, 0x1000
.delay:
    loop .delay
    jmp .main_loop

.exit_ok:
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

; ============================================================================
; create_window - Create the browser window
; ============================================================================
create_window:
    push bx
    push cx
    push dx
    push si
    push di

    ; Create window: X=40, Y=30, W=240, H=120
    mov bx, 40                      ; X position
    mov cx, 30                      ; Y position
    mov dx, 240                     ; Width (wider for size column)
    mov si, 120                     ; Height
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .done

    mov [cs:win_handle], al

    mov ah, API_WIN_GET_CONTENT
    int 0x80
    jc .done

    mov [cs:content_x], bx
    mov [cs:content_y], cx
    mov [cs:content_w], dx
    mov [cs:content_h], si

    clc

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; ============================================================================
; scan_and_display - Mount FS, scan directory, display files
; ============================================================================
scan_and_display:
    pusha

    ; Clear content area
    mov bx, [cs:content_x]
    mov cx, [cs:content_y]
    mov dx, [cs:content_w]
    mov si, [cs:content_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Mount filesystem
    mov al, 0                       ; Drive A:
    xor ah, ah
    mov ah, API_FS_MOUNT
    int 0x80
    jc .mount_failed

    ; Initialize
    mov word [cs:dir_state], 0
    mov byte [cs:file_count], 0
    mov word [cs:current_y], 0

    ; Set initial Y position
    mov ax, [cs:content_y]
    add ax, TOP_PADDING
    mov [cs:current_y], ax

.scan_loop:
    ; Check if we have room for more
    cmp byte [cs:file_count], MAX_DISPLAY_FILES
    jae .scan_done

    ; Read next directory entry
    mov al, 0
    mov cx, [cs:dir_state]
    push cs
    pop es
    mov di, dir_entry_buffer
    mov ah, API_FS_READDIR
    int 0x80
    jc .scan_done

    mov [cs:dir_state], cx

    ; Display this file
    call display_file_entry

    inc byte [cs:file_count]

    ; Move to next line
    mov ax, [cs:current_y]
    add ax, LINE_HEIGHT
    mov [cs:current_y], ax

    jmp .scan_loop

.mount_failed:
    ; Display error
    mov bx, [cs:content_x]
    add bx, LEFT_PADDING
    mov cx, [cs:content_y]
    add cx, TOP_PADDING
    mov si, mount_error_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .done

.scan_done:
    ; Check if no files
    cmp byte [cs:file_count], 0
    jne .done

    mov bx, [cs:content_x]
    add bx, LEFT_PADDING
    mov cx, [cs:content_y]
    add cx, TOP_PADDING
    mov si, no_files_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.done:
    popa
    ret

; ============================================================================
; display_file_entry - Display one directory entry
; Format: "FILENAME.EXT  12345"
; ============================================================================
display_file_entry:
    pusha

    ; Build display string in display_buffer
    mov di, display_buffer

    ; Copy filename (8 chars, skip trailing spaces)
    mov si, dir_entry_buffer
    mov cx, 8
.copy_name:
    mov al, [cs:si]
    cmp al, ' '
    je .name_done
    mov [cs:di], al
    inc si
    inc di
    loop .copy_name

.name_done:
    ; Add dot
    mov byte [cs:di], '.'
    inc di

    ; Copy extension (3 chars, skip trailing spaces)
    mov si, dir_entry_buffer
    add si, 8                       ; Extension at offset 8
    mov cx, 3
.copy_ext:
    mov al, [cs:si]
    cmp al, ' '
    je .ext_done
    mov [cs:di], al
    inc si
    inc di
    loop .copy_ext

.ext_done:
    ; Pad with spaces to column 14 for alignment
    mov ax, di
    sub ax, display_buffer
.pad_loop:
    cmp ax, 14
    jae .show_size
    mov byte [cs:di], ' '
    inc di
    inc ax
    jmp .pad_loop

.show_size:
    ; Get file size from directory entry (offset 0x1C = 28, 32-bit)
    ; For simplicity, just show lower 16 bits (up to 65535)
    mov ax, [cs:dir_entry_buffer + 28]

    ; Convert to decimal string
    call word_to_decimal

    ; Null terminate
    mov byte [cs:di], 0

    ; Draw the string
    mov bx, [cs:content_x]
    add bx, LEFT_PADDING
    mov cx, [cs:current_y]
    mov si, display_buffer
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; word_to_decimal - Convert AX to decimal string at CS:DI
; Input: AX = number, DI = destination
; Output: DI points past the last digit
; ============================================================================
word_to_decimal:
    push ax
    push bx
    push cx
    push dx

    ; Handle zero specially
    test ax, ax
    jnz .not_zero
    mov byte [cs:di], '0'
    inc di
    jmp .done

.not_zero:
    ; We'll build digits in reverse, then reverse them
    mov cx, 0                       ; Digit count
    mov bx, 10

.div_loop:
    xor dx, dx
    div bx                          ; AX = quotient, DX = remainder
    push dx                         ; Save digit
    inc cx
    test ax, ax
    jnz .div_loop

    ; Pop digits and store (now in correct order)
.store_loop:
    pop ax
    add al, '0'
    mov [cs:di], al
    inc di
    loop .store_loop

.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Data Section
; ============================================================================

window_title:   db 'Files', 0
win_handle:     db 0
content_x:      dw 0
content_y:      dw 0
content_w:      dw 0
content_h:      dw 0

dir_entry_buffer: times 32 db 0
dir_state:      dw 0
file_count:     db 0
current_y:      dw 0

display_buffer: times 32 db 0       ; For formatted output

mount_error_msg: db 'Mount failed', 0
no_files_msg:   db 'No files', 0
loop_counter:   db 0
