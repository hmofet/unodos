; MKBOOT.BIN - Boot Floppy Creator for UnoDOS
; Creates bootable 1.44MB floppies from any boot source.
; Floppy boot: pre-reads apps into memory, swaps disk, writes.
; HD boot: reads apps directly from HD during write.
;
; Build: nasm -f bin -o mkboot.bin mkboot.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'MkBoot', 0                  ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    ; White arrow pointing down into cyan disk
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 0:  white arrow shaft
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 1:  white arrow shaft
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 2:  white arrow shaft
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 3:  white arrow shaft
    db 0x03, 0xFF, 0xFF, 0xC0      ; Row 4:  white arrowhead
    db 0x00, 0xFF, 0xFF, 0x00      ; Row 5:  white arrowhead
    db 0x00, 0x3F, 0xFC, 0x00      ; Row 6:  white arrowhead
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 7:  white arrowhead
    db 0x00, 0x03, 0xC0, 0x00      ; Row 8:  white arrowhead tip
    db 0x00, 0x00, 0x00, 0x00      ; Row 9:  gap
    db 0x15, 0x55, 0x55, 0x54      ; Row 10: cyan disk top
    db 0x10, 0x00, 0x00, 0x04      ; Row 11: cyan disk sides
    db 0x10, 0x00, 0x00, 0x04      ; Row 12: cyan disk sides
    db 0x10, 0x00, 0x00, 0x04      ; Row 13: cyan disk sides
    db 0x15, 0x55, 0x55, 0x54      ; Row 14: cyan disk bottom
    db 0x00, 0x00, 0x00, 0x00      ; Row 15

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API constants
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_FS_MOUNT            equ 13
API_FS_OPEN             equ 14
API_FS_READ             equ 15
API_FS_CLOSE            equ 16
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_FS_READDIR          equ 27
API_MOUSE_STATE         equ 28
API_WIN_DRAW            equ 22
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34
API_GET_BOOT_DRIVE      equ 43
API_FS_WRITE_SECTOR     equ 44
API_FS_CREATE           equ 45
API_FS_WRITE            equ 46
API_WIN_GET_CONTENT_SIZE equ 97
API_DRAW_STRING_WRAP    equ 50
API_DRAW_BUTTON         equ 51
API_HIT_TEST            equ 53

EVENT_KEY_PRESS         equ 1
EVENT_MOUSE_CLICK       equ 4
EVENT_WIN_REDRAW        equ 6

; Button positions (window-relative)
BTN_FULL_X      equ 6
BTN_FULL_Y      equ 48
BTN_FULL_W      equ 84
BTN_FULL_H      equ 16
BTN_BARE_X      equ 96
BTN_BARE_Y      equ 48
BTN_BARE_W      equ 120
BTN_BARE_H      equ 16
BTN_CANCEL_X    equ 222
BTN_CANCEL_Y    equ 48
BTN_CANCEL_W    equ 72
BTN_CANCEL_H    equ 16
BTN_WRITE_X     equ 100
BTN_WRITE_Y     equ 66
BTN_WRITE_W     equ 96
BTN_WRITE_H     equ 16

; Floppy layout constants
FLOPPY_STAGE2_START     equ 1
FLOPPY_STAGE2_SECTORS   equ 4
FLOPPY_KERNEL_START     equ 5
FLOPPY_KERNEL_SECTORS   equ 64
FLOPPY_FS_START         equ 70

; FAT12 filesystem parameters
FAT12_RESERVED          equ 1
FAT12_NUM_FATS          equ 2
FAT12_SPF               equ 9
FAT12_ROOT_SECTORS      equ 14

MAX_APPS                equ 8
SCRATCH_SEG             equ 0x9000

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create window
    mov bx, 10
    mov cx, 20
    mov dx, 300
    mov si, 140
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:win_handle], al

    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Get boot drive
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov [cs:boot_drive], al

    ; Mount source
    test al, 0x80
    jnz .hd_boot_mount

    ; === Floppy boot ===
    mov al, 0x00
    mov ah, API_FS_MOUNT
    int 0x80
    jmp .draw_ui

.hd_boot_mount:
    ; === HD boot ===
    mov al, 0x80
    mov ah, API_FS_MOUNT
    int 0x80

.draw_ui:
    call draw_main_ui

.wait_choice:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    mov ah, API_EVENT_GET
    int 0x80
    jc .check_mouse
    cmp al, EVENT_KEY_PRESS
    jne .check_redraw1
    cmp dl, 'f'
    je .start_full
    cmp dl, 'F'
    je .start_full
    cmp dl, 'b'
    je .start_barebones
    cmp dl, 'B'
    je .start_barebones
    cmp dl, 27
    je .exit_ok
    jmp .wait_choice
.check_redraw1:
    cmp al, EVENT_WIN_REDRAW
    jne .wait_choice
    call draw_main_ui
    jmp .wait_choice

.check_mouse:
    ; Poll mouse for button clicks
    mov ah, API_MOUSE_STATE
    int 0x80
    ; DL = buttons (bit 0 = left)
    test dl, 1
    jz .mouse_up
    ; Left button pressed - check if just pressed (edge detect)
    cmp byte [cs:prev_btn], 0
    jne .wait_choice              ; Already held down
    mov byte [cs:prev_btn], 1
    ; Hit test each button
    mov bx, BTN_FULL_X
    mov cx, BTN_FULL_Y
    mov dx, BTN_FULL_W
    mov si, BTN_FULL_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .start_full
    mov bx, BTN_BARE_X
    mov cx, BTN_BARE_Y
    mov dx, BTN_BARE_W
    mov si, BTN_BARE_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .start_barebones
    mov bx, BTN_CANCEL_X
    mov cx, BTN_CANCEL_Y
    mov dx, BTN_CANCEL_W
    mov si, BTN_CANCEL_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .exit_ok
    jmp .wait_choice
.mouse_up:
    mov byte [cs:prev_btn], 0
    jmp .wait_choice

.start_full:
    mov byte [cs:copy_apps], 1
    jmp .do_write

.start_barebones:
    mov byte [cs:copy_apps], 0

.do_write:
    ; Clear button area + status area for write messages
    mov bx, 0
    mov cx, 44
    mov dx, 296
    mov si, 76
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    mov word [cs:status_y], 46

    ; === Pre-read apps from floppy if needed ===
    cmp byte [cs:copy_apps], 1
    jne .skip_preread
    test byte [cs:boot_drive], 0x80
    jnz .skip_preread
    ; Floppy full: read all BIN files into scratch buffer
    mov si, msg_reading
    call show_status
    call preread_apps
.skip_preread:

    ; === Swap prompt if floppy boot ===
    test byte [cs:boot_drive], 0x80
    jnz .no_swap
    mov si, msg_swap1
    call show_status
    ; Draw "Write" button below the swap message
    call draw_write_btn
    call wait_write_btn
    ; Clear swap message area for write status
    mov bx, 0
    mov cx, 44
    mov dx, 296
    mov si, 76
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    mov word [cs:status_y], 46
.no_swap:

    ; === Write boot sector ===
    mov si, msg_writing_boot
    call show_status

    mov ax, cs
    mov es, ax
    mov bx, embedded_boot
    mov si, 0
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error

    ; === Write stage2 (4 sectors) ===
    mov si, msg_writing_stage2
    call show_status

    mov cx, FLOPPY_STAGE2_SECTORS
    mov word [cs:cur_lba], FLOPPY_STAGE2_START
    mov word [cs:buf_off], embedded_stage2
.wr_stage2:
    push cx
    mov ax, cs
    mov es, ax
    mov bx, [cs:buf_off]
    mov si, [cs:cur_lba]
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    pop cx
    jc .error
    add word [cs:buf_off], 512
    inc word [cs:cur_lba]
    loop .wr_stage2

    ; === Write kernel from memory (64 sectors) ===
    mov si, msg_writing_kernel
    call show_status

    mov cx, FLOPPY_KERNEL_SECTORS
    mov word [cs:cur_lba], FLOPPY_KERNEL_START
    mov word [cs:buf_off], 0
.wr_kernel:
    push cx
    mov ax, 0x1000
    mov es, ax
    mov bx, [cs:buf_off]
    mov si, [cs:cur_lba]
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    pop cx
    jc .error
    add word [cs:buf_off], 512
    inc word [cs:cur_lba]
    loop .wr_kernel

    ; === Write FAT12 filesystem structure ===
    mov si, msg_writing_fs
    call show_status

    ; BPB at sector 70
    call build_fs_bpb
    mov ax, cs
    mov es, ax
    mov bx, secbuf
    mov si, FLOPPY_FS_START
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error

    ; FAT1 first sector (media byte)
    call build_fat_first
    mov ax, cs
    mov es, ax
    mov bx, secbuf
    mov si, FLOPPY_FS_START + FAT12_RESERVED
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error

    ; FAT1 remaining sectors (zero)
    call clear_secbuf
    mov cx, FAT12_SPF - 1
    mov word [cs:cur_lba], FLOPPY_FS_START + FAT12_RESERVED + 1
.wr_fat1:
    push cx
    mov ax, cs
    mov es, ax
    mov bx, secbuf
    mov si, [cs:cur_lba]
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    pop cx
    jc .error
    inc word [cs:cur_lba]
    loop .wr_fat1

    ; FAT2 first sector
    call build_fat_first
    mov ax, cs
    mov es, ax
    mov bx, secbuf
    mov si, FLOPPY_FS_START + FAT12_RESERVED + FAT12_SPF
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error

    ; FAT2 remaining sectors
    call clear_secbuf
    mov cx, FAT12_SPF - 1
    mov word [cs:cur_lba], FLOPPY_FS_START + FAT12_RESERVED + FAT12_SPF + 1
.wr_fat2:
    push cx
    mov ax, cs
    mov es, ax
    mov bx, secbuf
    mov si, [cs:cur_lba]
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    pop cx
    jc .error
    inc word [cs:cur_lba]
    loop .wr_fat2

    ; Root dir first sector (volume label)
    call build_rootdir
    mov ax, cs
    mov es, ax
    mov bx, secbuf
    mov si, FLOPPY_FS_START + FAT12_RESERVED + (FAT12_NUM_FATS * FAT12_SPF)
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error

    ; Root dir remaining sectors
    call clear_secbuf
    mov cx, FAT12_ROOT_SECTORS - 1
    mov word [cs:cur_lba], FLOPPY_FS_START + FAT12_RESERVED + (FAT12_NUM_FATS * FAT12_SPF) + 1
.wr_rootdir:
    push cx
    mov ax, cs
    mov es, ax
    mov bx, secbuf
    mov si, [cs:cur_lba]
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    pop cx
    jc .error
    inc word [cs:cur_lba]
    loop .wr_rootdir

    ; === Copy apps if requested ===
    cmp byte [cs:copy_apps], 0
    je .write_done

    mov si, msg_copying
    call show_status

    ; Mount the new floppy filesystem
    mov al, 0x00
    mov ah, API_FS_MOUNT
    int 0x80

    ; Choose write path based on boot source
    test byte [cs:boot_drive], 0x80
    jnz .copy_from_hd

    ; === Write pre-read apps from buffer ===
    call write_buffered_apps
    jmp .show_count

.copy_from_hd:
    ; === Copy apps from HD (stream copy) ===
    mov word [cs:dir_state], 0
    mov byte [cs:n_copied], 0

.hd_copy_loop:
    mov ax, cs
    mov es, ax
    mov di, dirent
    mov al, 1                       ; Mount handle 1 = HD
    mov cx, [cs:dir_state]
    mov ah, API_FS_READDIR
    int 0x80
    jc .show_count
    mov [cs:dir_state], cx

    ; Check .BIN extension
    cmp byte [cs:dirent + 8], 'B'
    jne .hd_copy_loop
    cmp byte [cs:dirent + 9], 'I'
    jne .hd_copy_loop
    cmp byte [cs:dirent + 10], 'N'
    jne .hd_copy_loop

    ; Skip KERNEL.BIN
    cmp byte [cs:dirent], 'K'
    je .hd_copy_loop

    call convert_83_to_dot

    ; Open on HD
    mov si, dotname
    mov bl, 1
    mov ah, API_FS_OPEN
    int 0x80
    jc .hd_copy_loop
    mov [cs:hd_fh], al

    ; Create on floppy
    mov si, dotname
    mov bl, 0
    mov ah, API_FS_CREATE
    int 0x80
    jc .hd_close_src

    mov [cs:fl_fh], al

    ; Stream copy
.hd_stream:
    mov ax, cs
    mov es, ax
    mov di, cpybuf
    mov cx, 512
    mov al, [cs:hd_fh]
    mov ah, API_FS_READ
    int 0x80
    jc .hd_close_both
    test ax, ax
    jz .hd_close_both
    mov [cs:nbytes], ax

    mov ax, cs
    mov es, ax
    mov bx, cpybuf
    mov cx, [cs:nbytes]
    mov al, [cs:fl_fh]
    mov ah, API_FS_WRITE
    int 0x80
    jc .hd_close_both

    cmp word [cs:nbytes], 512
    je .hd_stream

.hd_close_both:
    mov al, [cs:fl_fh]
    mov ah, API_FS_CLOSE
    int 0x80
.hd_close_src:
    mov al, [cs:hd_fh]
    mov ah, API_FS_CLOSE
    int 0x80
    inc byte [cs:n_copied]
    jmp .hd_copy_loop

.show_count:
    ; Redraw window frame (may have been overdrawn during write ops)
    mov al, [cs:win_handle]
    mov ah, API_WIN_DRAW
    int 0x80

    ; Show file count
    mov al, [cs:n_copied]
    add al, '0'
    mov [cs:cnt_ch], al
    mov si, msg_files
    mov bx, 4
    mov cx, [cs:status_y]
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    add word [cs:status_y], 10

.write_done:
    mov si, msg_done
    call show_status

.wait_exit:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    mov ah, API_EVENT_GET
    int 0x80
    jc .wait_exit
    cmp al, EVENT_KEY_PRESS
    jne .check_redraw2
    cmp dl, 27
    je .exit_ok
    jmp .wait_exit
.check_redraw2:
    cmp al, EVENT_WIN_REDRAW
    jne .wait_exit
    ; On redraw, show the done message
    mov si, msg_title
    mov bx, 4
    mov cx, 4
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .wait_exit

.error:
    mov si, msg_err
    call show_status
    jmp .wait_exit

.exit_ok:
    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80

.exit_fail:
    pop es
    pop ds
    popa
    retf

; ============================================================================
; Pre-read apps from source floppy into scratch buffer (0x9000)
; ============================================================================
preread_apps:
    mov byte [cs:n_apps], 0
    mov word [cs:buf_pos], 0
    mov word [cs:dir_state], 0

.pr_loop:
    mov ax, cs
    mov es, ax
    mov di, dirent
    mov al, 0                       ; Mount handle 0 = floppy
    mov cx, [cs:dir_state]
    mov ah, API_FS_READDIR
    int 0x80
    jc .pr_done
    mov [cs:dir_state], cx

    ; Check .BIN extension
    cmp byte [cs:dirent + 8], 'B'
    jne .pr_loop
    cmp byte [cs:dirent + 9], 'I'
    jne .pr_loop
    cmp byte [cs:dirent + 10], 'N'
    jne .pr_loop

    ; Skip KERNEL.BIN
    cmp byte [cs:dirent], 'K'
    je .pr_loop

    ; Check capacity
    cmp byte [cs:n_apps], MAX_APPS
    jae .pr_done

    call convert_83_to_dot

    ; Save filename to app_names table
    mov al, [cs:n_apps]
    xor ah, ah
    mov cx, 13
    mul cx
    add ax, app_names
    mov di, ax
    mov si, dotname
    mov cx, 13
.pr_copy_name:
    mov al, [cs:si]
    mov [cs:di], al
    inc si
    inc di
    loop .pr_copy_name

    ; Open file on floppy
    mov si, dotname
    mov bl, 0
    mov ah, API_FS_OPEN
    int 0x80
    jc .pr_loop
    mov [cs:hd_fh], al

    ; Read entire file into 0x9000:buf_pos
    mov ax, SCRATCH_SEG
    mov es, ax
    mov di, [cs:buf_pos]
    mov cx, 0xFFFF
    mov al, [cs:hd_fh]
    mov ah, API_FS_READ
    int 0x80
    jc .pr_close

    ; Save size
    push ax
    mov al, [cs:n_apps]
    xor ah, ah
    shl ax, 1
    mov si, ax
    add si, app_sizes
    pop ax
    mov [cs:si], ax

    ; Advance buffer
    add [cs:buf_pos], ax
    inc byte [cs:n_apps]

.pr_close:
    mov al, [cs:hd_fh]
    mov ah, API_FS_CLOSE
    int 0x80
    jmp .pr_loop

.pr_done:
    ret

; ============================================================================
; Write pre-read apps from scratch buffer to new floppy
; ============================================================================
write_buffered_apps:
    mov byte [cs:n_copied], 0
    mov word [cs:wr_off], 0
    mov byte [cs:app_idx], 0

.wb_loop:
    mov al, [cs:app_idx]
    cmp al, [cs:n_apps]
    jae .wb_done

    ; Get filename: app_names + idx * 13
    xor ah, ah
    mov cx, 13
    mul cx
    add ax, app_names
    mov si, ax

    ; Create file on new floppy
    mov bl, 0
    mov ah, API_FS_CREATE
    int 0x80
    jc .wb_next
    mov [cs:fl_fh], al

    ; Get size: app_sizes + idx * 2
    mov al, [cs:app_idx]
    xor ah, ah
    shl ax, 1
    mov si, ax
    add si, app_sizes
    mov cx, [cs:si]

    ; Write from 0x9000:wr_off
    mov ax, SCRATCH_SEG
    mov es, ax
    mov bx, [cs:wr_off]
    mov al, [cs:fl_fh]
    mov ah, API_FS_WRITE
    int 0x80

    ; Close
    mov al, [cs:fl_fh]
    mov ah, API_FS_CLOSE
    int 0x80

    ; Advance: wr_off += size
    mov al, [cs:app_idx]
    xor ah, ah
    shl ax, 1
    mov si, ax
    add si, app_sizes
    mov ax, [cs:si]
    add [cs:wr_off], ax

    inc byte [cs:n_copied]
.wb_next:
    inc byte [cs:app_idx]
    jmp .wb_loop

.wb_done:
    ret

; ============================================================================
; Helpers
; ============================================================================

; ============================================================================
; Draw the main UI (title, source info, buttons)
; ============================================================================
draw_main_ui:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    ; Clear content area (use API 97 for correct dimensions)
    mov al, 0xFF                    ; Current draw context
    mov ah, API_WIN_GET_CONTENT_SIZE
    int 0x80                        ; DX = content_w, SI = content_h
    mov bx, 0
    mov cx, 0
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw title
    mov si, msg_title
    mov bx, 4
    mov cx, 4
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Show source info
    test byte [cs:boot_drive], 0x80
    jnz .ui_hd_info

    ; Floppy source
    mov si, msg_floppy_src
    mov bx, 4
    mov cx, 18
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .ui_buttons

.ui_hd_info:
    ; HD source - show insert message
    mov si, msg_insert
    mov bx, 4
    mov cx, 18
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov si, msg_then
    mov bx, 4
    mov cx, 30
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.ui_buttons:
    ; Draw "Full" button
    mov ax, cs
    mov es, ax
    mov bx, BTN_FULL_X
    mov cx, BTN_FULL_Y
    mov dx, BTN_FULL_W
    mov si, BTN_FULL_H
    mov di, btn_full_lbl
    xor al, al                     ; Not pressed
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; Draw "Barebones" button
    mov ax, cs
    mov es, ax
    mov bx, BTN_BARE_X
    mov cx, BTN_BARE_Y
    mov dx, BTN_BARE_W
    mov si, BTN_BARE_H
    mov di, btn_bare_lbl
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; Draw "Cancel" button
    mov ax, cs
    mov es, ax
    mov bx, BTN_CANCEL_X
    mov cx, BTN_CANCEL_Y
    mov dx, BTN_CANCEL_W
    mov si, BTN_CANCEL_H
    mov di, btn_cancel_lbl
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

show_status:
    push ax
    push bx
    push cx
    mov cx, [cs:status_y]
    mov bx, 4
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    add word [cs:status_y], 10
    pop cx
    pop bx
    pop ax
    ret

draw_write_btn:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    mov ax, cs
    mov es, ax
    mov bx, BTN_WRITE_X
    mov cx, BTN_WRITE_Y
    mov dx, BTN_WRITE_W
    mov si, BTN_WRITE_H
    mov di, lbl_write_btn
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

wait_write_btn:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    ; Check keyboard events
    mov ah, API_EVENT_GET
    int 0x80
    jc .wb_check_mouse
    cmp al, EVENT_KEY_PRESS
    je .wb_done
    cmp al, EVENT_WIN_REDRAW
    jne wait_write_btn
    call draw_write_btn
    jmp wait_write_btn
.wb_check_mouse:
    mov ah, API_MOUSE_STATE
    int 0x80
    test dl, 1
    jz .wb_mouse_up
    cmp byte [cs:wb_prev], 0
    jne wait_write_btn
    mov byte [cs:wb_prev], 1
    ; Hit test Write button
    mov bx, BTN_WRITE_X
    mov cx, BTN_WRITE_Y
    mov dx, BTN_WRITE_W
    mov si, BTN_WRITE_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .wb_done
    jmp wait_write_btn
.wb_mouse_up:
    mov byte [cs:wb_prev], 0
    jmp wait_write_btn
.wb_done:
    ret

build_fs_bpb:
    call clear_secbuf
    mov byte [cs:secbuf + 0], 0xEB
    mov byte [cs:secbuf + 1], 0x3C
    mov byte [cs:secbuf + 2], 0x90
    mov dword [cs:secbuf + 3], 'UNOD'
    mov dword [cs:secbuf + 7], 'OS  '
    mov word [cs:secbuf + 11], 512
    mov byte [cs:secbuf + 13], 1
    mov word [cs:secbuf + 14], 1
    mov byte [cs:secbuf + 16], 2
    mov word [cs:secbuf + 17], 224
    mov word [cs:secbuf + 19], 2810
    mov byte [cs:secbuf + 21], 0xF0
    mov word [cs:secbuf + 22], 9
    mov word [cs:secbuf + 24], 18
    mov word [cs:secbuf + 26], 2
    mov byte [cs:secbuf + 38], 0x29
    mov dword [cs:secbuf + 39], 0x12345678
    mov dword [cs:secbuf + 43], 'UNOD'
    mov dword [cs:secbuf + 47], 'OS  '
    mov word [cs:secbuf + 51], '  '
    mov byte [cs:secbuf + 53], ' '
    mov dword [cs:secbuf + 54], 'FAT1'
    mov dword [cs:secbuf + 58], '2   '
    mov byte [cs:secbuf + 510], 0x55
    mov byte [cs:secbuf + 511], 0xAA
    ret

build_fat_first:
    call clear_secbuf
    mov byte [cs:secbuf], 0xF0
    mov byte [cs:secbuf + 1], 0xFF
    mov byte [cs:secbuf + 2], 0xFF
    ret

build_rootdir:
    call clear_secbuf
    mov dword [cs:secbuf + 0], 'UNOD'
    mov dword [cs:secbuf + 4], 'OS  '
    mov word [cs:secbuf + 8], '  '
    mov byte [cs:secbuf + 10], ' '
    mov byte [cs:secbuf + 11], 0x08
    ret

clear_secbuf:
    push ax
    push cx
    push di
    push es
    mov ax, cs
    mov es, ax
    mov di, secbuf
    xor ax, ax
    mov cx, 256
    rep stosw
    pop es
    pop di
    pop cx
    pop ax
    ret

convert_83_to_dot:
    push ax
    push cx
    push si
    push di
    mov si, dirent
    mov di, dotname
    mov cx, 8
.cn:
    mov al, [cs:si]
    cmp al, ' '
    je .cn_done
    mov [cs:di], al
    inc si
    inc di
    loop .cn
    jmp .dot
.cn_done:
    mov si, dirent
    add si, 8
    jmp .ext
.dot:
    mov si, dirent
    add si, 8
.ext:
    cmp byte [cs:si], ' '
    je .ext_done
    mov byte [cs:di], '.'
    inc di
    mov cx, 3
.ce:
    mov al, [cs:si]
    cmp al, ' '
    je .ext_done
    mov [cs:di], al
    inc si
    inc di
    loop .ce
.ext_done:
    mov byte [cs:di], 0
    pop di
    pop si
    pop cx
    pop ax
    ret

; ============================================================================
; Strings
; ============================================================================

window_title:   db 'Make Boot Floppy', 0
msg_title:      db 'Boot Floppy Creator', 0
msg_insert:     db 'Insert blank floppy in A:', 0
msg_then:       db 'then choose an option.', 0
msg_floppy_src: db 'Source floppy in drive.', 0
btn_full_lbl:   db 'Full', 0
btn_bare_lbl:   db 'Barebones', 0
btn_cancel_lbl: db 'Cancel', 0
msg_reading:    db 'Reading apps...', 0
msg_swap1:      db 'Insert blank floppy:', 0
lbl_write_btn:  db 'Write Disk', 0
msg_writing_boot:  db 'Boot sector...', 0
msg_writing_stage2: db 'Stage2 loader...', 0
msg_writing_kernel: db 'Kernel (32KB)...', 0
msg_writing_fs: db 'FAT12 filesystem...', 0
msg_copying:    db 'Writing apps...', 0
msg_done:       db 'Done! Floppy is bootable.', 0
msg_err:        db 'ERROR: Write failed!', 0
msg_files:      db 'Copied '
cnt_ch:         db '0'
                db ' files.', 0

; ============================================================================
; Variables
; ============================================================================

win_handle:     db 0
boot_drive:     db 0
copy_apps:      db 0
prev_btn:       db 0
wb_prev:        db 0
cur_lba:        dw 0
buf_off:        dw 0
status_y:       dw 94
dir_state:      dw 0
n_copied:       db 0
hd_fh:          db 0
fl_fh:          db 0
nbytes:         dw 0

; Pre-read state
n_apps:         db 0
buf_pos:        dw 0
app_idx:        db 0
wr_off:         dw 0

dirent:         times 32 db 0
dotname:        times 13 db 0

; App info tables (for floppy-to-floppy pre-read)
app_names:      times (MAX_APPS * 13) db 0
app_sizes:      times (MAX_APPS * 2) db 0

; ============================================================================
; Embedded boot sector (512 bytes) and stage2 (2048 bytes)
; ============================================================================
embedded_boot:
    incbin 'build/boot.bin'

embedded_stage2:
    incbin 'build/stage2.bin'

; ============================================================================
; Buffers
; ============================================================================
secbuf:     times 512 db 0
cpybuf:     times 512 db 0
