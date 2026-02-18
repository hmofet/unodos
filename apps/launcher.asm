; LAUNCHER.BIN - Desktop for UnoDOS v3.14.0
; Fullscreen desktop with app icons, double-click to launch
;
; Build: nasm -f bin -o launcher.bin launcher.asm
;
; Loads at segment 0x2000 (shell), launches apps to 0x3000 (user)
; Scans floppy for .BIN files, reads icon headers, displays icon grid

[BITS 16]
[ORG 0x0000]

; API function indices (must match kernel_api_table in kernel.asm)
API_GFX_DRAW_RECT       equ 1
API_GFX_DRAW_FILLED_RECT equ 2
API_GFX_DRAW_CHAR       equ 3
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_GFX_DRAW_STRING_INVERTED equ 6
API_EVENT_GET           equ 9
API_FS_MOUNT            equ 13
API_FS_READDIR          equ 27
API_APP_LOAD            equ 18
API_APP_START           equ 35
API_APP_YIELD           equ 34
API_MOUSE_GET_STATE     equ 28
API_DESKTOP_SET_ICON    equ 37
API_DESKTOP_CLEAR       equ 38
API_GFX_DRAW_ICON       equ 39
API_FS_READ_HEADER      equ 40
API_GFX_TEXT_WIDTH      equ 33
API_WIN_DRAW            equ 22
API_GET_BOOT_DRIVE      equ 43
API_POINT_OVER_WINDOW   equ 64
API_GET_TICK            equ 63
API_DELAY_TICKS         equ 73

; Event types
EVENT_KEY_PRESS         equ 1

; Icon grid constants
GRID_COLS               equ 4
GRID_ROWS               equ 2
MAX_ICONS               equ 8
COL_WIDTH               equ 80
ROW_HEIGHT              equ 80
GRID_START_Y            equ 24
ICON_SIZE               equ 16          ; 16x16 pixels
ICON_X_OFFSET           equ 32          ; Center 16px icon in 80px column
ICON_Y_OFFSET           equ 5           ; Top padding within row
LABEL_Y_GAP             equ 20          ; Below icon top (16px + 4px gap)
HITBOX_HEIGHT           equ 40          ; Clickable area height

; Double-click threshold
DOUBLE_CLICK_TICKS      equ 9           ; ~0.5s at 18.2 Hz BIOS timer

; Floppy poll interval
POLL_INTERVAL           equ 36          ; ~2 seconds

; Background color (CGA palette)
BG_COLOR                equ 1           ; Cyan

; Entry point - called by kernel via far CALL
entry:
    pusha
    push ds
    push es

    ; Set up segment
    mov ax, cs
    mov ds, ax

    ; Initialize state
    mov byte [cs:icon_count], 0
    mov byte [cs:selected_icon], 0xFF
    mov byte [cs:prev_buttons], 0
    mov word [cs:last_click_tick], 0
    mov byte [cs:last_click_icon], 0xFF

    ; Show splash screen with logo and progress bar
    call show_splash

    ; Mount filesystem and scan for apps (updates progress bar)
    call scan_disk

    ; Clear splash and draw full desktop
    call redraw_desktop

    ; Read initial BIOS tick counter for polling
    call read_bios_ticks
    mov [cs:last_poll_tick], ax

    ; Main event loop
.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; --- Mouse polling ---
    mov ah, API_MOUSE_GET_STATE
    int 0x80
    ; BX=X, CX=Y, DL=buttons

    ; Check for left button transition (released -> pressed)
    mov al, dl
    and al, 0x01                    ; Isolate left button
    mov ah, [cs:prev_buttons]
    mov [cs:prev_buttons], dl       ; Save current state
    and ah, 0x01
    ; AH = prev left button, AL = current left button
    cmp al, 1
    jne .no_click
    cmp ah, 0
    jne .no_click                   ; Not a transition

    ; Left button just pressed - check if click is over a window
    mov ah, API_POINT_OVER_WINDOW
    int 0x80                        ; BX=X, CX=Y preserved
    jnc .no_click                   ; CF=0 → over a window, skip desktop click

    call handle_click               ; BX=mouse X, CX=mouse Y

.no_click:
    ; Floppy swap polling removed — caused constant seeking on real hardware
    ; User clicks Refresh icon to rescan disk instead

    ; --- Keyboard events ---
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_KEY_PRESS
    jne .no_event

    ; Handle keyboard
    cmp dl, 13                      ; Enter?
    je .kb_launch
    cmp dl, 128                     ; Up arrow
    je .kb_up
    cmp dl, 129                     ; Down arrow
    je .kb_down
    cmp dl, 130                     ; Left arrow
    je .kb_left
    cmp dl, 131                     ; Right arrow
    je .kb_right
    cmp dl, 'w'
    je .kb_up
    cmp dl, 'W'
    je .kb_up
    cmp dl, 's'
    je .kb_down
    cmp dl, 'S'
    je .kb_down
    cmp dl, 'a'
    je .kb_left
    cmp dl, 'A'
    je .kb_left
    cmp dl, 'd'
    je .kb_right
    cmp dl, 'D'
    je .kb_right
    jmp .no_event

.kb_up:
    ; Move selection up (subtract GRID_COLS)
    cmp byte [cs:icon_count], 0
    je .no_event
    mov al, [cs:selected_icon]
    cmp al, 0xFF
    je .kb_select_first
    cmp al, GRID_COLS
    jb .no_event                    ; Already in top row
    sub al, GRID_COLS
    call select_icon
    jmp .no_event

.kb_down:
    cmp byte [cs:icon_count], 0
    je .no_event
    mov al, [cs:selected_icon]
    cmp al, 0xFF
    je .kb_select_first
    add al, GRID_COLS
    cmp al, [cs:icon_count]
    jae .no_event                   ; Past last icon
    call select_icon
    jmp .no_event

.kb_left:
    cmp byte [cs:icon_count], 0
    je .no_event
    mov al, [cs:selected_icon]
    cmp al, 0xFF
    je .kb_select_first
    or al, al
    jz .no_event                    ; Already at 0
    dec al
    call select_icon
    jmp .no_event

.kb_right:
    cmp byte [cs:icon_count], 0
    je .no_event
    mov al, [cs:selected_icon]
    cmp al, 0xFF
    je .kb_select_first
    inc al
    cmp al, [cs:icon_count]
    jae .no_event                   ; Past last icon
    call select_icon
    jmp .no_event

.kb_select_first:
    xor al, al
    call select_icon
    jmp .no_event

.kb_launch:
    ; Launch selected app
    mov al, [cs:selected_icon]
    cmp al, 0xFF
    je .no_event
    call launch_app
    jmp .no_event

.no_event:
    jmp .main_loop

; ============================================================================
; scan_disk - Mount filesystem and scan for .BIN files
; Populates icon data arrays
; ============================================================================
scan_disk:
    pusha
    push es

    ; Query boot drive from kernel
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov [cs:mounted_drive], al      ; Save boot drive (0x00=floppy, 0x80=HDD)

    ; Mount the boot drive filesystem
    mov ah, API_FS_MOUNT
    int 0x80
    jc .scan_done

    mov [cs:mount_handle], bl       ; Save mount handle (0=FAT12, 1=FAT16)
    mov word [cs:dir_state], 0
    mov word [cs:scan_safety], 0

    ; Clear all kernel desktop icons
    mov ah, API_DESKTOP_CLEAR
    int 0x80

    mov byte [cs:icon_count], 0

    ; Clear is_refresh flags
    mov di, is_refresh
    mov cx, 8
.clear_refresh:
    mov byte [cs:di], 0
    inc di
    loop .clear_refresh

.scan_loop:
    ; Safety check
    inc word [cs:scan_safety]
    cmp word [cs:scan_safety], 500
    jae .scan_done

    ; Check if we have room
    cmp byte [cs:icon_count], MAX_ICONS
    jae .scan_done

    ; Read next directory entry
    mov al, [cs:mount_handle]       ; Mount handle (0=FAT12, 1=FAT16)
    mov cx, [cs:dir_state]
    push cs
    pop es
    mov di, dir_entry_buffer
    mov ah, API_FS_READDIR
    int 0x80
    jc .scan_done

    mov [cs:dir_state], cx

    ; Check if .BIN file (extension at offset 8-10)
    cmp byte [cs:dir_entry_buffer + 8], 'B'
    jne .scan_loop
    cmp byte [cs:dir_entry_buffer + 9], 'I'
    jne .scan_loop
    cmp byte [cs:dir_entry_buffer + 10], 'N'
    jne .scan_loop

    ; Skip LAUNCHER.BIN
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
    jmp .scan_loop                  ; It's LAUNCHER, skip

.not_launcher:
    ; Skip KERNEL.BIN (not an app, would crash if launched)
    mov si, dir_entry_buffer
    mov di, kernel_name
    mov cx, 8
.cmp_kernel:
    mov al, [cs:si]
    cmp al, [cs:di]
    jne .not_kernel
    inc si
    inc di
    loop .cmp_kernel
    jmp .scan_loop                  ; It's KERNEL, skip

.not_kernel:
    ; Convert FAT name to dot format for app_info storage
    mov al, [cs:icon_count]
    call store_app_info

    ; Try to read BIN header to get icon
    mov al, [cs:icon_count]
    call read_bin_header

    ; Calculate grid position and register icon with kernel
    mov al, [cs:icon_count]
    call register_icon

    inc byte [cs:icon_count]
    call update_progress            ; Fill one segment of progress bar
    jmp .scan_loop

.scan_done:
    ; On floppy boot, add a Refresh icon as the last slot
    test byte [cs:mounted_drive], 0x80
    jnz .scan_really_done             ; Skip on HD boot
    cmp byte [cs:icon_count], MAX_ICONS
    jae .scan_really_done             ; No room

    mov al, [cs:icon_count]
    call add_refresh_icon
    inc byte [cs:icon_count]
    call update_progress            ; Fill one segment of progress bar

.scan_really_done:
    pop es
    popa
    ret

; ============================================================================
; add_refresh_icon - Add the floppy refresh icon to a slot
; Input: AL = slot number
; ============================================================================
add_refresh_icon:
    pusha

    mov [cs:.ari_slot], al

    ; Mark this slot as refresh
    xor ah, ah
    mov di, ax
    mov byte [cs:is_refresh + di], 1

    ; Copy bitmap: icon_bitmaps + slot*64
    mov al, [cs:.ari_slot]
    xor ah, ah
    shl ax, 6
    add ax, icon_bitmaps
    mov di, ax
    mov si, refresh_icon
    mov cx, 64
.ari_bmp:
    mov al, [cs:si]
    mov [cs:di], al
    inc si
    inc di
    loop .ari_bmp

    ; Copy name: icon_names + slot*12
    mov al, [cs:.ari_slot]
    xor ah, ah
    mov cl, 12
    mul cl
    add ax, icon_names
    mov di, ax
    mov si, refresh_name
    mov cx, 12
.ari_name:
    mov al, [cs:si]
    mov [cs:di], al
    inc si
    inc di
    loop .ari_name

    ; Register with kernel for desktop repaint
    mov al, [cs:.ari_slot]
    call register_icon

    popa
    ret
.ari_slot: db 0

; ============================================================================
; store_app_info - Store app filename info
; Input: AL = icon slot, dir_entry_buffer has FAT entry
; ============================================================================
store_app_info:
    push ax
    push cx
    push si
    push di

    ; Calculate destination: app_info + (slot * 16)
    xor ah, ah
    shl ax, 4                       ; * 16
    add ax, app_info
    mov di, ax

    ; Convert FAT "CLOCK   BIN" to "CLOCK.BIN\0"
    mov si, dir_entry_buffer
    mov cx, 8
.sai_name:
    mov al, [cs:si]
    cmp al, ' '
    je .sai_dot
    mov [cs:di], al
    inc si
    inc di
    loop .sai_name

.sai_dot:
    mov byte [cs:di], '.'
    inc di

    ; Copy extension
    mov si, dir_entry_buffer
    add si, 8
    mov cx, 3
.sai_ext:
    mov al, [cs:si]
    cmp al, ' '
    je .sai_null
    mov [cs:di], al
    inc si
    inc di
    loop .sai_ext

.sai_null:
    mov byte [cs:di], 0

    pop di
    pop si
    pop cx
    pop ax
    ret

; ============================================================================
; read_bin_header - Read first 80 bytes of a BIN file for icon data
; Input: AL = icon slot (app_info already populated)
; Sets icon_bitmaps[slot] and icon_names[slot] from BIN header
; ============================================================================
read_bin_header:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es

    mov [cs:.rbh_slot], al

    ; Get filename pointer: app_info + (slot * 16)
    xor ah, ah
    shl ax, 4
    add ax, app_info
    mov si, ax                      ; SI = filename in our segment

    ; Read first 80 bytes of the file
    mov bl, [cs:mount_handle]       ; Mount handle (0=FAT12, 1=FAT16)
    xor bh, bh
    push cs
    pop es
    mov di, header_buffer           ; ES:DI = buffer in our segment
    mov cx, 80                      ; Read 80 bytes
    mov ah, API_FS_READ_HEADER
    int 0x80
    jc .rbh_default                 ; Read failed, use default icon

    ; Check for icon header magic: byte[0]=0xEB, byte[2]='U', byte[3]='I'
    cmp byte [cs:header_buffer], 0xEB
    jne .rbh_default
    cmp byte [cs:header_buffer + 2], 'U'
    jne .rbh_default
    cmp byte [cs:header_buffer + 3], 'I'
    jne .rbh_default

    ; Has icon header - copy bitmap (64 bytes at offset 0x10)
    mov al, [cs:.rbh_slot]
    xor ah, ah
    shl ax, 6                       ; * 64
    add ax, icon_bitmaps
    mov di, ax
    mov si, header_buffer + 0x10    ; Source: bitmap at offset 0x10
    mov cx, 64
.rbh_copy_bmp:
    mov al, [cs:si]
    mov [cs:di], al
    inc si
    inc di
    loop .rbh_copy_bmp

    ; Copy name (12 bytes at offset 0x04)
    mov al, [cs:.rbh_slot]
    xor ah, ah
    mov cl, 12
    mul cl                          ; AX = slot * 12
    add ax, icon_names
    mov di, ax
    mov si, header_buffer + 0x04
    mov cx, 12
.rbh_copy_name:
    mov al, [cs:si]
    mov [cs:di], al
    inc si
    inc di
    loop .rbh_copy_name
    jmp .rbh_done

.rbh_default:
    ; No icon header - use default icon and derive name from FAT filename
    mov al, [cs:.rbh_slot]
    xor ah, ah
    shl ax, 6                       ; * 64
    add ax, icon_bitmaps
    mov di, ax
    mov si, default_icon
    mov cx, 64
.rbh_def_bmp:
    mov al, [cs:si]
    mov [cs:di], al
    inc si
    inc di
    loop .rbh_def_bmp

    ; Derive name from FAT filename (first 8 chars, strip trailing spaces)
    mov al, [cs:.rbh_slot]
    xor ah, ah
    mov cl, 12
    mul cl
    add ax, icon_names
    mov di, ax
    mov si, dir_entry_buffer        ; FAT name
    mov cx, 8
.rbh_def_name:
    mov al, [cs:si]
    cmp al, ' '
    je .rbh_def_name_end
    mov [cs:di], al
    inc si
    inc di
    loop .rbh_def_name
.rbh_def_name_end:
    mov byte [cs:di], 0

.rbh_done:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.rbh_slot: db 0

; ============================================================================
; register_icon - Register icon with kernel for desktop repaint
; Input: AL = icon slot
; ============================================================================
register_icon:
    push ax
    push bx
    push cx
    push si

    mov [cs:.ri_slot], al

    ; Calculate grid position
    xor ah, ah
    mov bl, GRID_COLS
    div bl                          ; AL = row, AH = col

    ; Icon X = col * COL_WIDTH + ICON_X_OFFSET
    push ax
    mov al, ah                      ; AL = col
    xor ah, ah
    mov bl, COL_WIDTH
    mul bl                          ; AX = col * 80
    add ax, ICON_X_OFFSET
    mov [cs:.ri_x], ax
    pop ax

    ; Icon Y = GRID_START_Y + row * ROW_HEIGHT + ICON_Y_OFFSET
    xor ah, ah                      ; AL = row
    mov bl, ROW_HEIGHT
    mul bl                          ; AX = row * 80
    add ax, GRID_START_Y
    add ax, ICON_Y_OFFSET
    mov [cs:.ri_y], ax

    ; Build 76-byte data block: 64B bitmap + 12B name
    ; Point SI to bitmap for this slot
    mov al, [cs:.ri_slot]
    xor ah, ah
    shl ax, 6                       ; * 64
    add ax, icon_bitmaps
    mov si, ax                      ; SI = bitmap source

    ; Copy bitmap to register_buffer
    mov di, register_buffer
    mov cx, 64
.ri_copy_bmp:
    mov al, [cs:si]
    mov [cs:di], al
    inc si
    inc di
    loop .ri_copy_bmp

    ; Copy name
    mov al, [cs:.ri_slot]
    xor ah, ah
    mov cl, 12
    mul cl
    add ax, icon_names
    mov si, ax
    mov cx, 12
.ri_copy_name:
    mov al, [cs:si]
    mov [cs:di], al
    inc si
    inc di
    loop .ri_copy_name

    ; Register with kernel API 37
    mov al, [cs:.ri_slot]
    mov bx, [cs:.ri_x]
    mov cx, [cs:.ri_y]
    mov si, register_buffer
    mov ah, API_DESKTOP_SET_ICON
    int 0x80

    pop si
    pop cx
    pop bx
    pop ax
    ret

.ri_slot: db 0
.ri_x:    dw 0
.ri_y:    dw 0

; ============================================================================
; show_splash - Display splash screen with logo and progress bar
; ============================================================================
show_splash:
    pusha

    ; Fast clear CGA screen (direct memory write — instant vs pixel-by-pixel API)
    call fast_clear_screen

    ; Draw "U" logo — 3 filled white rectangles
    ; Left pillar: (140, 40) w=10, h=36
    mov bx, 140
    mov cx, 40
    mov dx, 10
    mov si, 36
    mov ah, API_GFX_DRAW_FILLED_RECT
    int 0x80

    ; Right pillar: (170, 40) w=10, h=36
    mov bx, 170
    mov cx, 40
    mov dx, 10
    mov si, 36
    mov ah, API_GFX_DRAW_FILLED_RECT
    int 0x80

    ; Bottom bar: (140, 68) w=40, h=8
    mov bx, 140
    mov cx, 68
    mov dx, 40
    mov si, 8
    mov ah, API_GFX_DRAW_FILLED_RECT
    int 0x80

    ; "UnoDOS 3" centered below logo (white text, black bg = transparent on black screen)
    mov bx, 112
    mov cx, 90
    mov si, splash_name
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; "Loading..." centered
    mov bx, 100
    mov cx, 120
    mov si, splash_loading
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Progress bar outline: (90, 140) w=140, h=10
    mov bx, 90
    mov cx, 140
    mov dx, 140
    mov si, 10
    mov ah, API_GFX_DRAW_RECT
    int 0x80

    popa
    ret

; ============================================================================
; update_progress - Fill one segment of the splash progress bar
; Called after icon_count is incremented
; ============================================================================
update_progress:
    pusha
    ; Fill segment N (0-based): X = 91 + N*17, width=17, height=8
    mov al, [cs:icon_count]
    dec al                          ; Just incremented, use N-1
    xor ah, ah
    mov bl, 17
    mul bl
    add ax, 91
    mov bx, ax
    mov cx, 141
    mov dx, 17
    mov si, 8
    mov ah, API_GFX_DRAW_FILLED_RECT
    int 0x80
    popa
    ret

; ============================================================================
; draw_background - Fill screen with background color
; ============================================================================
draw_background:
    pusha

    ; Fast clear CGA screen (direct memory write — instant vs pixel-by-pixel API)
    call fast_clear_screen

    popa
    ret

; ============================================================================
; redraw_desktop - Full desktop repaint (background + title + version + icons)
; ============================================================================
redraw_desktop:
    pusha

    call draw_background

    ; Title at top-left
    mov bx, 4
    mov cx, 4
    mov si, title_str
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Version at bottom-left
    mov bx, 4
    mov cx, 190
    mov si, VERSION_STR
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Build number at bottom-right area
    mov bx, 200
    mov cx, 190
    mov si, BUILD_NUMBER_STR
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    call draw_all_icons

    popa
    ret

; ============================================================================
; draw_all_icons - Draw all discovered icons on the desktop
; ============================================================================
draw_all_icons:
    pusha

    mov byte [cs:.dai_idx], 0

.dai_loop:
    mov al, [cs:.dai_idx]
    cmp al, [cs:icon_count]
    jae .dai_done

    call draw_single_icon

    inc byte [cs:.dai_idx]
    jmp .dai_loop

.dai_done:
    ; If no icons, show message
    cmp byte [cs:icon_count], 0
    jne .dai_ret
    mov bx, 80
    mov cx, 80
    mov si, no_apps_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.dai_ret:
    popa
    ret

.dai_idx: db 0

; ============================================================================
; draw_single_icon - Draw one icon at its grid position
; Input: AL = icon slot
; ============================================================================
draw_single_icon:
    pusha

    mov [cs:.dsi_slot], al

    ; Calculate grid position
    xor ah, ah
    mov bl, GRID_COLS
    div bl                          ; AL = row, AH = col

    ; Icon X = col * COL_WIDTH + ICON_X_OFFSET
    push ax
    mov al, ah
    xor ah, ah
    mov bl, COL_WIDTH
    mul bl
    add ax, ICON_X_OFFSET
    mov [cs:.dsi_x], ax
    pop ax

    ; Icon Y = GRID_START_Y + row * ROW_HEIGHT + ICON_Y_OFFSET
    xor ah, ah
    mov bl, ROW_HEIGHT
    mul bl
    add ax, GRID_START_Y
    add ax, ICON_Y_OFFSET
    mov [cs:.dsi_y], ax

    ; Draw icon bitmap using API 39
    mov bx, [cs:.dsi_x]
    mov cx, [cs:.dsi_y]
    ; Point SI to bitmap data
    mov al, [cs:.dsi_slot]
    xor ah, ah
    shl ax, 6                       ; * 64
    add ax, icon_bitmaps
    mov si, ax
    mov ah, API_GFX_DRAW_ICON
    int 0x80

    ; Draw name label below icon
    mov bx, [cs:.dsi_x]
    sub bx, 8                       ; Shift left a bit for longer names
    mov cx, [cs:.dsi_y]
    add cx, LABEL_Y_GAP            ; Below icon
    ; Point SI to name
    mov al, [cs:.dsi_slot]
    xor ah, ah
    mov cl, 12
    mul cl
    add ax, icon_names
    mov si, ax
    mov cx, [cs:.dsi_y]
    add cx, LABEL_Y_GAP
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw selection highlight if this is the selected icon
    mov al, [cs:.dsi_slot]
    cmp al, [cs:selected_icon]
    jne .dsi_done

    call draw_highlight

.dsi_done:
    popa
    ret

.dsi_slot: db 0
.dsi_x:    dw 0
.dsi_y:    dw 0

; ============================================================================
; draw_highlight - Draw selection rectangle around selected icon
; Uses draw_single_icon's .dsi_x/.dsi_y
; ============================================================================
draw_highlight:
    push ax
    push bx
    push cx
    push dx
    push si

    ; Draw a white rectangle border around the icon area
    ; Top line
    mov bx, [cs:draw_single_icon.dsi_x]
    sub bx, 2
    mov cx, [cs:draw_single_icon.dsi_y]
    sub cx, 2
    mov dx, 20                      ; 16 + 4
    mov si, 1
    mov ah, API_GFX_DRAW_FILLED_RECT
    int 0x80

    ; Bottom line
    mov bx, [cs:draw_single_icon.dsi_x]
    sub bx, 2
    mov cx, [cs:draw_single_icon.dsi_y]
    add cx, ICON_SIZE
    add cx, 1
    mov dx, 20
    mov si, 1
    mov ah, API_GFX_DRAW_FILLED_RECT
    int 0x80

    ; Left line
    mov bx, [cs:draw_single_icon.dsi_x]
    sub bx, 2
    mov cx, [cs:draw_single_icon.dsi_y]
    sub cx, 1
    mov dx, 1
    mov si, 18                      ; 16 + 2
    mov ah, API_GFX_DRAW_FILLED_RECT
    int 0x80

    ; Right line
    mov bx, [cs:draw_single_icon.dsi_x]
    add bx, ICON_SIZE
    add bx, 1
    mov cx, [cs:draw_single_icon.dsi_y]
    sub cx, 1
    mov dx, 1
    mov si, 18
    mov ah, API_GFX_DRAW_FILLED_RECT
    int 0x80

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; handle_click - Process a mouse click
; Input: BX = mouse X, CX = mouse Y
; ============================================================================
handle_click:
    pusha

    ; Hit test: which icon was clicked?
    mov [cs:.hc_mx], bx
    mov [cs:.hc_my], cx
    mov byte [cs:.hc_hit], 0xFF     ; No hit

    xor dx, dx                      ; Icon counter
.hc_test:
    cmp dl, [cs:icon_count]
    jae .hc_tested

    ; Calculate icon hitbox for slot DL
    mov al, dl
    xor ah, ah
    push dx
    mov bl, GRID_COLS
    div bl                          ; AL = row, AH = col

    ; Hitbox X = col * COL_WIDTH + ICON_X_OFFSET - 4 (centered on icon area)
    push ax
    mov al, ah
    xor ah, ah
    mov bl, COL_WIDTH
    mul bl
    add ax, ICON_X_OFFSET
    sub ax, 4                       ; Slightly wider than 16px icon
    mov [cs:.hc_hx], ax
    pop ax

    ; Hitbox Y = GRID_START_Y + row * ROW_HEIGHT + ICON_Y_OFFSET
    xor ah, ah
    mov bl, ROW_HEIGHT
    mul bl
    add ax, GRID_START_Y
    add ax, ICON_Y_OFFSET
    mov [cs:.hc_hy], ax
    pop dx

    ; Check: hx <= mx < hx + 24 (icon width + padding)
    mov ax, [cs:.hc_mx]
    cmp ax, [cs:.hc_hx]
    jb .hc_next
    mov bx, [cs:.hc_hx]
    add bx, 24                     ; 16px icon + 4px padding each side
    cmp ax, bx
    jae .hc_next

    ; Check: hy <= my < hy + HITBOX_HEIGHT
    mov ax, [cs:.hc_my]
    cmp ax, [cs:.hc_hy]
    jb .hc_next
    mov bx, [cs:.hc_hy]
    add bx, HITBOX_HEIGHT
    cmp ax, bx
    jae .hc_next

    ; Hit!
    mov [cs:.hc_hit], dl
    jmp .hc_tested

.hc_next:
    inc dl
    jmp .hc_test

.hc_tested:
    ; Check if we hit an icon
    mov al, [cs:.hc_hit]
    cmp al, 0xFF
    je .hc_deselect

    ; Check for double-click
    cmp al, [cs:last_click_icon]
    jne .hc_single_click

    ; Same icon - check timing
    call read_bios_ticks
    mov bx, ax
    sub bx, [cs:last_click_tick]
    cmp bx, DOUBLE_CLICK_TICKS
    jae .hc_single_click

    ; Double click! Launch the app
    mov al, [cs:.hc_hit]
    call launch_app
    mov byte [cs:last_click_icon], 0xFF
    jmp .hc_done

.hc_single_click:
    ; Select this icon
    mov al, [cs:.hc_hit]
    call select_icon

    ; Record click for double-click detection
    mov al, [cs:.hc_hit]
    mov [cs:last_click_icon], al
    call read_bios_ticks
    mov [cs:last_click_tick], ax
    jmp .hc_done

.hc_deselect:
    ; Clicked on empty space - deselect
    cmp byte [cs:selected_icon], 0xFF
    je .hc_done
    mov byte [cs:selected_icon], 0xFF
    mov byte [cs:last_click_icon], 0xFF
    ; Full desktop redraw to guarantee highlight removal
    call redraw_desktop
    call repaint_all_windows

.hc_done:
    popa
    ret

.hc_mx: dw 0
.hc_my: dw 0
.hc_hx: dw 0
.hc_hy: dw 0
.hc_hit: db 0xFF

; ============================================================================
; select_icon - Select an icon (highlight it)
; Input: AL = icon slot to select
; ============================================================================
select_icon:
    push ax
    push bx
    push cx
    push si

    ; If same icon already selected, nothing to do
    cmp al, [cs:selected_icon]
    je .si_done

    ; Deselect old icon (redraw without highlight)
    push ax
    mov al, [cs:selected_icon]
    cmp al, 0xFF
    je .si_no_old
    ; Clear old highlight area and redraw old icon
    call clear_icon_area
    call draw_single_icon
.si_no_old:
    pop ax

    ; Set new selection
    mov [cs:selected_icon], al

    ; Draw new icon with highlight
    call draw_single_icon

.si_done:
    pop si
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; clear_icon_area - Clear the area around an icon (for removing highlight)
; Input: AL = icon slot
; ============================================================================
clear_icon_area:
    push ax
    push bx
    push cx
    push dx
    push si

    ; Calculate position
    xor ah, ah
    push ax
    mov bl, GRID_COLS
    div bl

    push ax
    mov al, ah
    xor ah, ah
    mov bl, COL_WIDTH
    mul bl
    add ax, ICON_X_OFFSET
    sub ax, 4
    mov [cs:.cia_x], ax
    pop ax

    xor ah, ah
    mov bl, ROW_HEIGHT
    mul bl
    add ax, GRID_START_Y
    add ax, ICON_Y_OFFSET
    sub ax, 4
    mov [cs:.cia_y], ax
    pop ax

    ; Clear area
    mov bx, [cs:.cia_x]
    mov cx, [cs:.cia_y]
    mov dx, 24                      ; 16 + 8
    mov si, 24                      ; 16 + 8
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.cia_x: dw 0
.cia_y: dw 0

; ============================================================================
; launch_app - Launch the selected app
; Input: AL = icon slot
; ============================================================================
launch_app:
    pusha

    ; Check if this is the refresh icon
    xor ah, ah
    mov di, ax
    cmp byte [cs:is_refresh + di], 1
    jne .la_normal_launch

    ; Refresh icon — rescan disk
    mov byte [cs:icon_count], 0
    mov byte [cs:selected_icon], 0xFF
    call scan_disk
    call redraw_desktop
    call repaint_all_windows
    jmp .la_done

.la_normal_launch:
    ; Get filename for this slot: app_info + (slot * 16)
    shl ax, 4
    add ax, app_info
    mov si, ax

    ; Load app to auto-allocated user segment (DH>0x20 triggers pool alloc)
    mov dl, [cs:mounted_drive]
    mov dh, 0x30                    ; User segment (auto-allocated by kernel)
    mov ah, API_APP_LOAD
    int 0x80
    jc .la_error

    ; Start app (non-blocking)
    mov ah, API_APP_START
    int 0x80

    jmp .la_done

.la_error:
    ; Save error code before it gets clobbered
    mov [cs:la_errcode], al

    ; Draw black background bar for message
    mov bx, 0
    mov cx, 68
    mov dx, 176
    mov si, 240
    mov di, 18
    mov ah, API_GFX_DRAW_FILLED_RECT
    int 0x80

    ; Error 2 = mount failed, Error 3 = file not found → "Insert app disk"
    mov al, [cs:la_errcode]
    cmp al, 2
    je .la_show_insert
    cmp al, 3
    je .la_show_insert

    ; Other errors: show generic "Load err: X"
    mov bx, 72
    mov cx, 180
    mov si, load_error_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov al, [cs:la_errcode]
    add al, '0'
    mov bx, 192
    mov cx, 180
    mov ah, API_GFX_DRAW_CHAR
    int 0x80
    jmp .la_after_msg

.la_show_insert:
    mov bx, 72
    mov cx, 180
    mov si, insert_disk_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.la_after_msg:
    ; Brief delay so user can see error
    call .la_delay
    call .la_delay
    ; Redraw desktop to clear error message
    call redraw_desktop
    call repaint_all_windows

.la_done:
    popa
    ret

.la_delay:
    ; Wait ~1 second using kernel delay API
    push cx
    mov cx, 18                      ; ~1 second at 18.2 Hz
    mov ah, API_DELAY_TICKS
    int 0x80
    pop cx
    ret


; ============================================================================
; repaint_all_windows - Redraw all window frames on top of the desktop
; Called after a full desktop repaint to prevent desktop from obscuring apps
; ============================================================================
repaint_all_windows:
    pusha
    mov byte [cs:.raw_idx], 0
.raw_loop:
    cmp byte [cs:.raw_idx], 16
    jae .raw_done
    mov al, [cs:.raw_idx]
    mov ah, API_WIN_DRAW
    int 0x80
    inc byte [cs:.raw_idx]
    jmp .raw_loop
.raw_done:
    popa
    ret
.raw_idx: db 0


; ============================================================================
; check_floppy_swap - Check if floppy disk was swapped
; ============================================================================
check_floppy_swap:
    pusha

    ; Skip floppy polling if booted from hard drive
    test byte [cs:mounted_drive], 0x80
    jnz .cfs_no_change

    ; INT 13h AH=16h: Check disk change status
    mov ah, 16h
    mov dl, 0                       ; Drive A:
    int 13h
    jnc .cfs_no_change              ; CF=0: no change

    ; Disk changed - rescan
    mov byte [cs:icon_count], 0
    mov byte [cs:selected_icon], 0xFF

    call scan_disk

    ; Redraw desktop
    call redraw_desktop

    ; Repaint any open app windows on top of the desktop
    call repaint_all_windows

.cfs_no_change:
    popa
    ret

; ============================================================================
; read_bios_ticks - Read tick counter via kernel API
; Output: AX = tick count (low word)
; ============================================================================
read_bios_ticks:
    mov ah, API_GET_TICK
    int 0x80
    ret

; ============================================================================
; fast_clear_screen - Clear entire CGA screen using REP STOSW
; Clears both interlace banks (16KB at 0xB800) in microseconds.
; Much faster than pixel-by-pixel API_GFX_CLEAR_AREA on real hardware.
; ============================================================================
fast_clear_screen:
    ; Use API 5 (gfx_clear_area) instead of direct CGA writes.
    ; Direct CGA writes break the XOR cursor invariant, leaving ghost cursors.
    ; The kernel's fast byte-fill path handles aligned full-screen clears efficiently.
    push bx
    push cx
    push dx
    push si
    xor bx, bx                     ; X = 0
    xor cx, cx                     ; Y = 0
    mov dx, 320                     ; Width = 320
    mov si, 200                     ; Height = 200
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    pop si
    pop dx
    pop cx
    pop bx
    ret

; ============================================================================
; Data Section
; ============================================================================

title_str:      db 'UnoDOS', 0
splash_name:    db 'UnoDOS 3', 0
splash_loading: db 'Loading...', 0
no_apps_msg:    db 'No apps found', 0
load_error_msg: db 'Load err: ', 0
insert_disk_msg: db 'Insert app disk', 0
la_errcode:     db 0

; Drive and scan state
mounted_drive:  db 0
mount_handle:   db 0                ; 0=FAT12, 1=FAT16
dir_state:      dw 0
scan_safety:    dw 0

; Icon tracking
icon_count:     db 0
selected_icon:  db 0xFF

; Mouse state
prev_buttons:   db 0
last_click_tick: dw 0
last_click_icon: db 0xFF

; Floppy polling
last_poll_tick: dw 0

; Per-app info: 8 slots x 16 bytes (13B filename + 1B drive + 2B reserved)
app_info:       times (MAX_ICONS * 16) db 0

; Icon bitmaps: 8 slots x 64 bytes
icon_bitmaps:   times (MAX_ICONS * 64) db 0

; Icon names: 8 slots x 12 bytes
icon_names:     times (MAX_ICONS * 12) db 0

; Buffer for kernel icon registration (76 bytes: 64B bitmap + 12B name)
register_buffer: times 76 db 0

; Buffer for reading BIN file headers
header_buffer:  times 80 db 0

; Directory entry buffer (32 bytes for fs_readdir)
dir_entry_buffer: times 32 db 0

; FAT names for skipping non-app files
launcher_name:  db 'LAUNCHER'
kernel_name:    db 'KERNEL  '

; Default icon for apps without headers (simple square/app shape)
; White outline rectangle with inner dot
default_icon:
    db 0xFF, 0xFF, 0xFF, 0xFF      ; Row 0:  ################
    db 0xC0, 0x00, 0x00, 0x03      ; Row 1:  #..............#
    db 0xC0, 0x00, 0x00, 0x03      ; Row 2:  #..............#
    db 0xC0, 0x00, 0x00, 0x03      ; Row 3:  #..............#
    db 0xC0, 0x00, 0x00, 0x03      ; Row 4:  #..............#
    db 0xC0, 0x00, 0x00, 0x03      ; Row 5:  #..............#
    db 0xC0, 0x03, 0xC0, 0x03      ; Row 6:  #.....##.......#
    db 0xC0, 0x03, 0xC0, 0x03      ; Row 7:  #.....##.......#
    db 0xC0, 0x03, 0xC0, 0x03      ; Row 8:  #.....##.......#
    db 0xC0, 0x03, 0xC0, 0x03      ; Row 9:  #.....##.......#
    db 0xC0, 0x00, 0x00, 0x03      ; Row 10: #..............#
    db 0xC0, 0x00, 0x00, 0x03      ; Row 11: #..............#
    db 0xC0, 0x00, 0x00, 0x03      ; Row 12: #..............#
    db 0xC0, 0x00, 0x00, 0x03      ; Row 13: #..............#
    db 0xC0, 0x00, 0x00, 0x03      ; Row 14: #..............#
    db 0xFF, 0xFF, 0xFF, 0xFF      ; Row 15: ################

; Refresh/floppy icon (3.5" floppy shape: metal slider top, label bottom)
refresh_icon:
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 0:  .##############.
    db 0xC0, 0x00, 0x00, 0x0F      ; Row 1:  ##..............##
    db 0xCF, 0x0C, 0xF0, 0x0F      ; Row 2:  ##..####..####..##
    db 0xCF, 0x0C, 0xF0, 0x0F      ; Row 3:  ##..####..####..##
    db 0xCF, 0x0C, 0xF0, 0x0F      ; Row 4:  ##..####..####..##
    db 0xC0, 0x00, 0x00, 0x0F      ; Row 5:  ##..............##
    db 0xC0, 0x00, 0x00, 0x0C      ; Row 6:  ##..............#.
    db 0xC0, 0x00, 0x00, 0x0C      ; Row 7:  ##..............#.
    db 0xC0, 0x00, 0x00, 0x0C      ; Row 8:  ##..............#.
    db 0xC3, 0xFF, 0xFF, 0x0C      ; Row 9:  ##..############.#.
    db 0xC3, 0x00, 0x03, 0x0C      ; Row 10: ##..#.........#.#.
    db 0xC3, 0x00, 0x03, 0x0C      ; Row 11: ##..#.........#.#.
    db 0xC3, 0xFF, 0xFF, 0x0C      ; Row 12: ##..############.#.
    db 0xC0, 0x00, 0x00, 0x0C      ; Row 13: ##..............#.
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 14: .################.
    db 0x00, 0x00, 0x00, 0x00      ; Row 15: ..................

refresh_name:   db 'Refresh', 0, 0, 0, 0, 0   ; 12 bytes padded

; Per-slot flag: 0=app, 1=refresh icon
is_refresh:     times 8 db 0

; App handle for launched app
app_handle:     dw 0

; Build info strings (auto-generated from BUILD_NUMBER and VERSION)
%include "kernel/build_info.inc"
