; MKBOOT.BIN - Boot Floppy Creator for UnoDOS
; Creates a bootable 1.44MB floppy disk from HD
; Embeds boot sector and stage2, copies kernel from memory,
; builds FAT12 filesystem, optionally copies apps from HD
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
    ; Arrow pointing down into a disk shape
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 0:  ....####....
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 1:  ....####....
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 2:  ....####....
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 3:  ....####....
    db 0x03, 0xFF, 0xFF, 0xC0      ; Row 4:  ##..####..##
    db 0x00, 0xFF, 0xFF, 0x00      ; Row 5:  ..########..
    db 0x00, 0x3F, 0xFC, 0x00      ; Row 6:  ....######..
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 7:  ....####....
    db 0x00, 0x03, 0xC0, 0x00      ; Row 8:  ......##....
    db 0x00, 0x00, 0x00, 0x00      ; Row 9:  ............
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 10: ##############
    db 0x30, 0x00, 0x00, 0x0C      ; Row 11: ##..........##
    db 0x30, 0x00, 0x00, 0x0C      ; Row 12: ##..........##
    db 0x30, 0x00, 0x00, 0x0C      ; Row 13: ##..........##
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 14: ##############
    db 0x00, 0x00, 0x00, 0x00      ; Row 15: ............

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API constants
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_GFX_DRAW_STRING_INV equ 6
API_EVENT_GET           equ 9
API_FS_MOUNT            equ 13
API_FS_OPEN             equ 14
API_FS_READ             equ 15
API_FS_CLOSE            equ 16
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34
API_GET_BOOT_DRIVE      equ 43
API_FS_WRITE_SECTOR     equ 44
API_FS_CREATE           equ 45
API_FS_WRITE            equ 46
API_FS_READDIR          equ 27

EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Floppy layout constants
FLOPPY_STAGE2_START     equ 1       ; Sectors 1-4
FLOPPY_STAGE2_SECTORS   equ 4
FLOPPY_KERNEL_START     equ 5       ; Sectors 5-68
FLOPPY_KERNEL_SECTORS   equ 64
FLOPPY_FS_START         equ 70      ; Sector 70

; FAT12 filesystem parameters
FAT12_RESERVED          equ 1
FAT12_NUM_FATS          equ 2
FAT12_SPF               equ 9
FAT12_ROOT_SECTORS      equ 14

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create window - use moderate size
    mov bx, 40                      ; X
    mov cx, 35                      ; Y
    mov dx, 240                     ; Width
    mov si, 120                     ; Height
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

    ; Draw initial UI
    call draw_ui

    ; Check boot drive
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov [cs:boot_drive], al
    test al, 0x80
    jnz .hd_boot

    ; Booting from floppy - can't create boot floppies
    mov si, msg_need_hd
    mov bx, 4
    mov cx, 60
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .wait_exit

.hd_boot:
    ; Mount HD filesystem
    mov al, 0x80
    mov ah, API_FS_MOUNT
    int 0x80

    ; Show options
    mov si, msg_option_f
    mov bx, 4
    mov cx, 48
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov si, msg_option_b
    mov bx, 4
    mov cx, 60
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov si, msg_option_esc
    mov bx, 4
    mov cx, 76
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.wait_choice:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    mov ah, API_EVENT_GET
    int 0x80
    jc .wait_choice
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
    cmp dl, 27                      ; ESC
    je .exit_ok
    jmp .wait_choice
.check_redraw1:
    cmp al, EVENT_WIN_REDRAW
    jne .wait_choice
    call draw_ui
    jmp .wait_choice

.start_full:
    mov byte [cs:copy_apps], 1
    jmp .do_write

.start_barebones:
    mov byte [cs:copy_apps], 0

.do_write:
    ; Clear options area
    mov bx, 0
    mov cx, 44
    mov dx, 236
    mov si, 100
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    mov word [cs:status_y], 46

    ; === Write boot sector ===
    mov si, msg_writing_boot
    call show_status

    mov ax, cs
    mov es, ax
    mov bx, embedded_boot
    mov si, 0                       ; LBA 0
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
    mov ax, 0x1000                  ; Kernel segment
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

    ; === Write FAT12 filesystem ===
    mov si, msg_writing_fs
    call show_status

    ; FS BPB at sector 70
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

    ; FAT2 first sector (media byte)
    call build_fat_first
    mov ax, cs
    mov es, ax
    mov bx, secbuf
    mov si, FLOPPY_FS_START + FAT12_RESERVED + FAT12_SPF
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error

    ; FAT2 remaining sectors (zero)
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

    ; Root directory first sector (volume label)
    call build_rootdir
    mov ax, cs
    mov es, ax
    mov bx, secbuf
    mov si, FLOPPY_FS_START + FAT12_RESERVED + (FAT12_NUM_FATS * FAT12_SPF)
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error

    ; Root dir remaining sectors (zero)
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

    ; === Optionally copy apps ===
    cmp byte [cs:copy_apps], 0
    je .write_done

    mov si, msg_copying
    call show_status

    ; Mount the new floppy filesystem
    mov al, 0x00
    mov ah, API_FS_MOUNT
    int 0x80

    ; Scan HD directory and copy BIN files
    mov word [cs:dir_idx], 0
    mov byte [cs:n_copied], 0

.copy_loop:
    mov ax, cs
    mov es, ax
    mov di, dirent                  ; ES:DI = dir entry buffer
    mov ax, [cs:dir_idx]
    mov bl, 1                       ; Mount handle 1 = HD
    mov ah, API_FS_READDIR
    int 0x80
    jc .write_done                  ; End of directory

    inc word [cs:dir_idx]

    ; Check for .BIN extension
    cmp byte [cs:dirent + 8], 'B'
    jne .copy_loop
    cmp byte [cs:dirent + 9], 'I'
    jne .copy_loop
    cmp byte [cs:dirent + 10], 'N'
    jne .copy_loop

    ; Skip KERNEL.BIN
    cmp byte [cs:dirent], 'K'
    je .copy_loop

    ; Convert 8.3 to dot format
    call convert_83_to_dot

    ; Open on HD
    mov si, dotname
    mov bx, 1
    mov ah, API_FS_OPEN
    int 0x80
    jc .copy_loop
    mov [cs:hd_fh], al

    ; Create on floppy
    mov si, dotname
    mov bl, 0
    mov ah, API_FS_CREATE
    int 0x80
    jc .close_hd

    mov [cs:fl_fh], al

    ; Copy data
.copy_data:
    mov al, [cs:hd_fh]
    mov ax, cs
    mov es, ax
    mov di, cpybuf
    mov cx, 512
    mov al, [cs:hd_fh]
    mov ah, API_FS_READ
    int 0x80
    jc .close_both
    test ax, ax
    jz .close_both
    mov [cs:nbytes], ax

    mov ax, cs
    mov es, ax
    mov bx, cpybuf
    mov cx, [cs:nbytes]
    mov al, [cs:fl_fh]
    mov ah, API_FS_WRITE
    int 0x80
    jc .close_both

    cmp word [cs:nbytes], 512
    je .copy_data

.close_both:
    mov al, [cs:fl_fh]
    mov ah, API_FS_CLOSE
    int 0x80
.close_hd:
    mov al, [cs:hd_fh]
    mov ah, API_FS_CLOSE
    int 0x80
    inc byte [cs:n_copied]
    jmp .copy_loop

.write_done:
    ; Show success
    mov si, msg_done
    call show_status

    cmp byte [cs:copy_apps], 0
    je .wait_exit
    ; Show file count
    mov al, [cs:n_copied]
    add al, '0'
    mov [cs:cnt_ch], al
    mov si, msg_files
    mov bx, 4
    mov cx, [cs:status_y]
    mov ah, API_GFX_DRAW_STRING
    int 0x80

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
    call draw_ui
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
; Helpers
; ============================================================================

draw_ui:
    push ax
    push bx
    push cx
    push si
    mov si, msg_title
    mov bx, 4
    mov cx, 4
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov si, msg_insert
    mov bx, 4
    mov cx, 20
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov si, msg_then
    mov bx, 4
    mov cx, 32
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    pop si
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
msg_option_f:   db 'F = Full (OS + all apps)', 0
msg_option_b:   db 'B = Barebones (OS only)', 0
msg_option_esc: db 'ESC = Cancel', 0
msg_need_hd:    db 'Must boot from HD!', 0
msg_writing_boot:  db 'Boot sector...', 0
msg_writing_stage2: db 'Stage2 loader...', 0
msg_writing_kernel: db 'Kernel (32KB)...', 0
msg_writing_fs: db 'FAT12 filesystem...', 0
msg_copying:    db 'Copying apps...', 0
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
cur_lba:        dw 0
buf_off:        dw 0
status_y:       dw 46
dir_idx:        dw 0
n_copied:       db 0
hd_fh:          db 0
fl_fh:          db 0
nbytes:         dw 0

dirent:         times 32 db 0
dotname:        times 13 db 0

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
