; LAUNCHER.BIN - Desktop launcher for UnoDOS v3.12.0
; Build 037 - Debug: show error code on load failure
;
; Build: nasm -f bin -o launcher.bin launcher.asm
;
; Loads at segment 0x2000 (shell), launches apps to 0x3000 (user)

[BITS 16]
[ORG 0x0000]

; API function indices
API_GFX_DRAW_CHAR       equ 3
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 8
API_APP_LOAD            equ 17
API_APP_RUN             equ 18
API_WIN_CREATE          equ 19
API_WIN_DESTROY         equ 20
API_WIN_GET_CONTENT     equ 24
API_REGISTER_SHELL      equ 25

; Event types
EVENT_KEY_PRESS         equ 1

; Menu constants
MENU_ITEM_HEIGHT        equ 12          ; Pixels per menu item
MENU_LEFT_PADDING       equ 10          ; Left margin for text
MENU_TOP_PADDING        equ 4           ; Top margin

; Entry point - called by kernel via far CALL
entry:
    pusha
    push ds
    push es

    ; Save our code segment
    mov ax, cs
    mov ds, ax

    ; Create launcher window
    call create_window
    jc .exit_fail

    ; Draw initial menu
    call draw_menu

    ; Main event loop
.main_loop:
    ; Get event (non-blocking)
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    ; DEBUG: Show event type and key code at top of window
    push ax
    push dx
    ; Show event type (AL) as hex digit at position (content_x, content_y-10)
    mov bx, [cs:content_x]
    add bx, 120                     ; Right side of window
    mov cx, [cs:content_y]
    add cx, 4
    ; Convert AL to hex character
    push ax
    and al, 0x0F
    cmp al, 10
    jb .digit1
    add al, 'A' - 10
    jmp .show1
.digit1:
    add al, '0'
.show1:
    mov ah, API_GFX_DRAW_CHAR
    int 0x80
    pop ax
    pop dx
    pop ax

    ; Check if key press
    cmp al, EVENT_KEY_PRESS
    jne .no_event

    ; DEBUG: Show key character
    push ax
    push dx
    mov bx, [cs:content_x]
    add bx, 130
    mov cx, [cs:content_y]
    add cx, 4
    mov al, dl                      ; Key character
    mov ah, API_GFX_DRAW_CHAR
    int 0x80
    pop dx
    pop ax

    ; Handle key
    cmp dl, 27                      ; ESC?
    je .exit_ok

    cmp dl, 'w'
    je .move_up
    cmp dl, 'W'
    je .move_up

    cmp dl, 's'
    je .move_down
    cmp dl, 'S'
    je .move_down

    cmp dl, 13                      ; Enter?
    je .launch_app

    jmp .no_event

.move_up:
    ; Decrement selection with wrap
    mov al, [cs:selected]
    or al, al
    jz .wrap_to_bottom
    dec al
    mov [cs:selected], al
    jmp .redraw

.wrap_to_bottom:
    mov al, [cs:menu_count]
    dec al
    mov [cs:selected], al
    jmp .redraw

.move_down:
    ; Increment selection with wrap
    mov al, [cs:selected]
    inc al
    cmp al, [cs:menu_count]
    jb .no_wrap
    xor al, al                      ; Wrap to 0
.no_wrap:
    mov [cs:selected], al

.redraw:
    call draw_menu
    jmp .no_event

.launch_app:
    ; Destroy launcher window before launching
    mov al, [cs:win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80

    ; Get filename pointer for selected app
    call get_selected_filename      ; Returns SI = filename pointer

    ; Load app to segment 0x3000 (user segment)
    mov dl, 0                       ; Drive A:
    mov dh, 0x30                    ; Target segment 0x3000
    mov ah, API_APP_LOAD
    int 0x80
    jc .load_error

    ; Save app handle
    mov [cs:app_handle], al

    ; Run the app
    xor ah, ah                      ; AX = app handle
    mov ah, API_APP_RUN
    int 0x80

    ; App returned - recreate our window
    call create_window
    call draw_menu
    jmp .main_loop

.load_error:
    ; Save error code
    mov [cs:last_error], al

    ; Recreate window and show error
    call create_window
    call draw_menu
    call draw_error
    jmp .main_loop

.no_event:
    ; Small delay to avoid CPU spin
    mov cx, 0x1000
.delay:
    loop .delay
    jmp .main_loop

.exit_ok:
    ; Destroy window
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
; create_window - Create the launcher window
; Output: CF=0 success, CF=1 error
; ============================================================================
create_window:
    push bx
    push cx
    push dx
    push si
    push di

    ; Create window: X=80, Y=40, W=160, H=100
    mov bx, 80                      ; X position
    mov cx, 40                      ; Y position
    mov dx, 160                     ; Width
    mov si, 100                     ; Height
    mov ax, cs
    mov es, ax
    mov di, window_title            ; ES:DI = title
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .done

    ; Save window handle
    mov [cs:win_handle], al

    ; Get content area
    mov ah, API_WIN_GET_CONTENT
    int 0x80
    jc .done

    ; Save content area bounds
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
; draw_menu - Draw all menu items
; ============================================================================
draw_menu:
    pusha

    ; Clear content area first
    mov bx, [cs:content_x]
    mov cx, [cs:content_y]
    mov dx, [cs:content_w]
    mov si, [cs:content_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw each menu item
    mov byte [cs:draw_index], 0     ; Item index

.draw_loop:
    mov al, [cs:draw_index]
    cmp al, [cs:menu_count]
    jae .draw_help

    ; Calculate Y position: content_y + MENU_TOP_PADDING + (index * MENU_ITEM_HEIGHT)
    mov bl, MENU_ITEM_HEIGHT
    mul bl                          ; AX = index * MENU_ITEM_HEIGHT
    add ax, [cs:content_y]
    add ax, MENU_TOP_PADDING
    mov [cs:draw_y], ax             ; Save Y position

    ; Calculate X position
    mov ax, [cs:content_x]
    add ax, MENU_LEFT_PADDING
    mov [cs:draw_x], ax             ; Save X position

    ; Check if this item is selected
    mov al, [cs:draw_index]
    cmp al, [cs:selected]
    jne .draw_name

    ; Draw selection indicator "> "
    mov bx, [cs:draw_x]
    mov cx, [cs:draw_y]
    mov si, indicator
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    add word [cs:draw_x], 16        ; Move X past indicator

.draw_name:
    ; Get display name pointer: menu_data + (index * 24) + 12
    mov al, [cs:draw_index]
    mov cl, 24
    mul cl                          ; AX = index * 24
    add ax, menu_data + 12          ; Point to display name
    mov si, ax

    ; Draw the display name
    mov bx, [cs:draw_x]
    mov cx, [cs:draw_y]
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    inc byte [cs:draw_index]
    jmp .draw_loop

.draw_help:
    ; Draw help text at bottom of content area
    mov bx, [cs:content_x]
    add bx, MENU_LEFT_PADDING
    mov cx, [cs:content_y]
    add cx, [cs:content_h]
    sub cx, 24                      ; Two lines from bottom
    mov si, help_line1
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    add cx, 10                      ; Next line
    mov si, help_line2
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; Temporary variables for draw_menu
draw_index: db 0
draw_x:     dw 0
draw_y:     dw 0

; ============================================================================
; draw_error - Draw error message with error code
; Error codes: 1=no slot, 2=mount fail, 3=file not found, 4=read fail
; ============================================================================
draw_error:
    pusha

    ; Draw error message
    mov bx, [cs:content_x]
    add bx, MENU_LEFT_PADDING
    mov cx, [cs:content_y]
    add cx, [cs:content_h]
    sub cx, 36                      ; Above help text
    mov si, error_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw error code as digit after message
    add bx, 104                     ; After "Load failed! "
    mov al, [cs:last_error]
    add al, '0'                     ; Convert to ASCII digit
    mov ah, API_GFX_DRAW_CHAR
    int 0x80

    ; Wait a moment so user sees it
    mov cx, 0xFFFF
.wait:
    loop .wait
    mov cx, 0xFFFF
.wait2:
    loop .wait2

    ; Redraw menu to clear error
    popa
    ret

; ============================================================================
; get_selected_filename - Get pointer to selected app's FAT filename
; Output: SI = pointer to 11-byte FAT filename
; ============================================================================
get_selected_filename:
    ; Calculate: menu_data + (selected * 24)
    mov al, [cs:selected]
    mov cl, 24
    mul cl                          ; AX = selected * 24
    add ax, menu_data
    mov si, ax                      ; SI = pointer to FAT filename
    ret

; ============================================================================
; Data Section
; ============================================================================

window_title:   db 'Launcher', 0
win_handle:     db 0
content_x:      dw 0
content_y:      dw 0
content_w:      dw 0
content_h:      dw 0
selected:       db 0
app_handle:     dw 0
last_error:     db 0

; Menu data: 12-byte filename + 12-byte display name (null-terminated)
; Each entry is 24 bytes
; Filename format: "NAME.EXT", 0 (fat12_open expects dot-separated, null-terminated)
menu_data:
    db 'CLOCK.BIN', 0               ; Filename (10 bytes with null)
    db 0, 0                         ; Padding to 12 bytes
    db 'Clock', 0                   ; Display name (null-terminated)
    times 6 db 0                    ; Padding to 24 bytes

    db 'HELLO.BIN', 0               ; Filename (10 bytes with null)
    db 0, 0                         ; Padding to 12 bytes
    db 'Hello Test', 0              ; Display name
    db 0                            ; Padding to 24 bytes

menu_count:     db 2

; UI strings
indicator:      db '> ', 0
help_line1:     db 'W/S: Select', 0
help_line2:     db 'Enter: Run', 0
error_msg:      db 'Load failed!', 0
