; LAUNCHER.BIN - Desktop launcher for UnoDOS v3.12.0
; Build 043 - Fix filename format conversion (FAT -> dot format)
;
; Build: nasm -f bin -o launcher.bin launcher.asm
;
; Loads at segment 0x2000 (shell), launches apps to 0x3000 (user)
; Scans floppy for .BIN files and displays them dynamically

[BITS 16]
[ORG 0x0000]

; API function indices
API_GFX_DRAW_CHAR       equ 3
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 8
API_FS_MOUNT            equ 12
API_FS_READDIR          equ 26
API_APP_LOAD            equ 17
API_APP_RUN             equ 18
API_WIN_CREATE          equ 19
API_WIN_DESTROY         equ 20
API_WIN_GET_CONTENT     equ 24

; Event types
EVENT_KEY_PRESS         equ 1

; Menu constants
MENU_ITEM_HEIGHT        equ 12          ; Pixels per menu item
MENU_LEFT_PADDING       equ 10          ; Left margin for text
MENU_TOP_PADDING        equ 4           ; Top margin
MAX_MENU_ITEMS          equ 8           ; Maximum apps to display

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

    ; Drain any pending events (leftover from disk swap confirmation)
.drain_events:
    mov ah, API_EVENT_GET
    int 0x80
    test al, al                     ; AL=0 means no more events
    jnz .drain_events

    ; Scan for .BIN files on the floppy
    call scan_for_apps

    ; Draw initial menu
    call draw_menu

    ; Main event loop
.main_loop:
    ; Get event (non-blocking)
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    ; Check if key press
    cmp al, EVENT_KEY_PRESS
    jne .no_event

    ; Handle key
    cmp dl, 27                      ; ESC?
    je .exit_ok

    ; Only handle navigation if we have apps
    cmp byte [cs:discovered_count], 0
    je .no_event

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
    mov al, [cs:discovered_count]
    dec al
    mov [cs:selected], al
    jmp .redraw

.move_down:
    ; Increment selection with wrap
    mov al, [cs:selected]
    inc al
    cmp al, [cs:discovered_count]
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
    mov dl, [cs:mounted_drive]      ; Use the drive we successfully mounted
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
; scan_for_apps - Scan for .BIN files on available drives
; Populates discovered_apps and discovered_count
; Tries HDD (0x80) first, then floppy (0x00)
; ============================================================================
scan_for_apps:
    pusha

    ; Initialize
    mov byte [cs:discovered_count], 0
    mov word [cs:dir_state], 0

    ; Try HDD first (0x80)
    mov al, 0x80                    ; Drive 0x80 (HDD/IDE)
    xor ah, ah                      ; Auto-detect
    mov ah, API_FS_MOUNT
    int 0x80
    jnc .mounted_hdd                ; Success! Save drive and scan

    ; HDD failed, try floppy (0x00)
    mov al, 0                       ; Drive A: (floppy)
    xor ah, ah                      ; Auto-detect
    mov ah, API_FS_MOUNT
    int 0x80
    jc .scan_done                   ; Both failed, leave empty list

    ; Save floppy mount info (mount returned handle in BX)
    mov byte [cs:mounted_drive], 0  ; Drive A:
    mov byte [cs:mount_handle], bl  ; Mount handle (should be 0 for FAT12)
    jmp .scan_loop

.mounted_hdd:
    ; Save HDD mount info (mount returned handle in BX)
    mov byte [cs:mounted_drive], 0x80    ; Drive 0x80
    mov byte [cs:mount_handle], bl       ; Mount handle (should be 1 for FAT16)

.scan_loop:
    ; Check if we have room for more
    cmp byte [cs:discovered_count], MAX_MENU_ITEMS
    jae .scan_done

    ; Read next directory entry
    mov al, [cs:mount_handle]       ; Use saved mount handle (0=FAT12, 1=FAT16)
    mov cx, [cs:dir_state]          ; Iteration state
    push cs
    pop es
    mov di, dir_entry_buffer        ; ES:DI = buffer
    mov ah, API_FS_READDIR
    int 0x80
    jc .scan_done                   ; End of directory or error

    ; Save new state
    mov [cs:dir_state], cx

    ; Check if this is a .BIN file (extension at offset 8-10)
    cmp byte [cs:dir_entry_buffer + 8], 'B'
    jne .scan_loop
    cmp byte [cs:dir_entry_buffer + 9], 'I'
    jne .scan_loop
    cmp byte [cs:dir_entry_buffer + 10], 'N'
    jne .scan_loop

    ; Skip LAUNCHER.BIN (don't show ourselves)
    ; Compare first 8 characters with "LAUNCHER"
    mov si, dir_entry_buffer
    mov di, launcher_name
    mov cx, 8
.cmp_launcher:
    mov al, [cs:si]
    cmp al, [cs:di]
    jne .not_launcher
    inc si
    inc di
    loop .cmp_launcher
    jmp .scan_loop                  ; It's LAUNCHER, skip it

.not_launcher:
    ; Add to discovered_apps list
    ; Calculate destination: discovered_apps + (count * 12)
    mov al, [cs:discovered_count]
    mov cl, 12
    mul cl                          ; AX = count * 12
    mov di, ax
    add di, discovered_apps         ; DI = destination offset

    ; Copy 11 bytes (8.3 FAT name) and add null terminator
    mov si, dir_entry_buffer
    mov cx, 11
.copy_name:
    mov al, [cs:si]
    mov [cs:di], al
    inc si
    inc di
    loop .copy_name
    mov byte [cs:di], 0             ; Null terminator

    inc byte [cs:discovered_count]
    jmp .scan_loop

.scan_done:
    popa
    ret

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

    ; Check if we have any apps
    cmp byte [cs:discovered_count], 0
    je .no_apps

    ; Draw each menu item
    mov byte [cs:draw_index], 0     ; Item index

.draw_loop:
    mov al, [cs:draw_index]
    cmp al, [cs:discovered_count]
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
    ; Get filename pointer: discovered_apps + (index * 12)
    mov al, [cs:draw_index]
    mov cl, 12
    mul cl                          ; AX = index * 12
    add ax, discovered_apps
    mov si, ax

    ; Format and draw: convert "CLOCK   BIN" to displayable name
    ; For simplicity, just draw the raw 8.3 name
    mov bx, [cs:draw_x]
    mov cx, [cs:draw_y]
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    inc byte [cs:draw_index]
    jmp .draw_loop

.no_apps:
    ; Display "No apps found" message
    mov bx, [cs:content_x]
    add bx, MENU_LEFT_PADDING
    mov cx, [cs:content_y]
    add cx, MENU_TOP_PADDING
    mov si, no_apps_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; DEBUG: Show mount info
    mov bx, [cs:content_x]
    add bx, MENU_LEFT_PADDING
    mov cx, [cs:content_y]
    add cx, MENU_TOP_PADDING
    add cx, 10
    mov si, debug_drive_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Show mounted drive number
    mov al, [cs:mounted_drive]
    call show_hex_byte

    ; Show mount handle
    mov bx, [cs:content_x]
    add bx, MENU_LEFT_PADDING
    mov cx, [cs:content_y]
    add cx, MENU_TOP_PADDING
    add cx, 20
    mov si, debug_handle_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov al, [cs:mount_handle]
    call show_hex_byte

    jmp .draw_help_only

.draw_help:
.draw_help_only:
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
; ============================================================================
draw_error:
    pusha

    mov bx, [cs:content_x]
    add bx, MENU_LEFT_PADDING
    mov cx, [cs:content_y]
    add cx, [cs:content_h]
    sub cx, 36                      ; Above help text
    mov si, error_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw error code on next line
    mov bx, [cs:content_x]
    add bx, MENU_LEFT_PADDING
    add cx, 10
    mov si, error_code_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw the error digit
    mov bx, [cs:content_x]
    add bx, MENU_LEFT_PADDING
    add bx, 48
    mov al, [cs:last_error]
    add al, '0'
    mov ah, API_GFX_DRAW_CHAR
    int 0x80

    ; Wait a moment
    mov cx, 0xFFFF
.wait:
    loop .wait
    mov cx, 0xFFFF
.wait2:
    loop .wait2

    popa
    ret

; ============================================================================
; get_selected_filename - Get pointer to selected app's filename
; Converts FAT format "CLOCK   BIN" to dot format "CLOCK.BIN"
; Output: SI = pointer to null-terminated filename in dot format
; ============================================================================
get_selected_filename:
    push di
    push cx
    push ax

    ; Get source: discovered_apps + (selected * 12)
    mov al, [cs:selected]
    mov cl, 12
    mul cl                          ; AX = selected * 12
    add ax, discovered_apps
    mov si, ax                      ; SI = source (FAT format)
    mov di, filename_buffer         ; DI = destination

    ; Copy filename part (up to 8 chars, stop at space)
    mov cx, 8
.copy_name:
    mov al, [cs:si]
    cmp al, ' '                     ; Stop at space
    je .add_dot
    mov [cs:di], al
    inc si
    inc di
    loop .copy_name

.add_dot:
    ; Skip remaining spaces in filename part
    mov ax, si
    sub ax, discovered_apps
    ; Calculate how many chars we copied
    push di
    mov di, filename_buffer
    mov ax, di
    pop di
    ; SI should point to extension (offset 8 from start of entry)
    mov al, [cs:selected]
    mov cl, 12
    mul cl
    add ax, discovered_apps
    add ax, 8                       ; Extension starts at offset 8
    mov si, ax

    ; Add dot
    mov byte [cs:di], '.'
    inc di

    ; Copy extension (3 chars)
    mov cx, 3
.copy_ext:
    mov al, [cs:si]
    cmp al, ' '                     ; Stop at space
    je .add_null
    mov [cs:di], al
    inc si
    inc di
    loop .copy_ext

.add_null:
    mov byte [cs:di], 0             ; Null terminator

    ; Return pointer to converted filename
    mov si, filename_buffer

    pop ax
    pop cx
    pop di
    ret

; ============================================================================
; Show hex byte - displays AL as 2 hex digits at current BX,CX position
; ============================================================================
show_hex_byte:
    push ax
    push bx
    push cx
    push si

    ; Save position
    add bx, 48                      ; Move X right for hex display

    ; Convert high nibble
    mov cl, al
    shr cl, 4
    and cl, 0x0F
    add cl, '0'
    cmp cl, '9'
    jbe .high_ok
    add cl, 7                       ; 'A'-'9'-1
.high_ok:
    mov [cs:hex_buffer], cl

    ; Convert low nibble
    mov cl, al
    and cl, 0x0F
    add cl, '0'
    cmp cl, '9'
    jbe .low_ok
    add cl, 7
.low_ok:
    mov [cs:hex_buffer+1], cl

    ; Draw it
    mov si, hex_buffer
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    pop si
    pop cx
    pop bx
    pop ax
    ret

hex_buffer: db 0, 0, 0              ; 2 hex chars + null

; Buffer for converted filename (8 + 1 + 3 + 1 = 13 bytes)
filename_buffer: times 13 db 0

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
mounted_drive:  db 0                ; Drive number that was successfully mounted
mount_handle:   db 0                ; Mount handle (0=FAT12, 1=FAT16)

; Dynamic app discovery data
discovered_apps: times (MAX_MENU_ITEMS * 12) db 0   ; 8 apps × 12 bytes each
discovered_count: db 0
dir_entry_buffer: times 32 db 0                     ; For fs_readdir
dir_state:       dw 0                               ; Iteration state

; String to match against for skipping ourselves
launcher_name:  db 'LAUNCHER'                       ; 8 chars, no null needed

; UI strings
indicator:      db '> ', 0
help_line1:     db 'W/S: Select', 0
help_line2:     db 'Enter: Run', 0
no_apps_msg:    db 'No apps found', 0
debug_drive_msg: db 'Drive: 0x', 0
debug_handle_msg: db 'Handle: 0x', 0
error_msg:      db 'Load failed!', 0
error_code_label: db 'Code: ', 0
