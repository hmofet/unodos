; MKBOOT.BIN - Boot Floppy Creator for UnoDOS
; Creates a bootable 1.44MB floppy disk
; Embeds boot sector and stage2, copies kernel from memory,
; builds FAT12 filesystem, and copies apps from HD
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
    ; Floppy disk icon
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 0:  top edge
    db 0x30, 0x3F, 0xFC, 0x0C      ; Row 1:  label area
    db 0x30, 0x3F, 0xFC, 0x0C      ; Row 2:  label
    db 0x30, 0x3F, 0xFC, 0x0C      ; Row 3:  label
    db 0x30, 0x00, 0x00, 0x0C      ; Row 4:  body
    db 0x30, 0x00, 0x00, 0x0C      ; Row 5:  body
    db 0x30, 0x00, 0x00, 0x0C      ; Row 6:  body
    db 0x30, 0x0F, 0xF0, 0x0C      ; Row 7:  hub top
    db 0x30, 0x0C, 0x30, 0x0C      ; Row 8:  hub
    db 0x30, 0x0C, 0x30, 0x0C      ; Row 9:  hub
    db 0x30, 0x0F, 0xF0, 0x0C      ; Row 10: hub bottom
    db 0x30, 0x00, 0x00, 0x0C      ; Row 11: body
    db 0x30, 0x00, 0x00, 0x0C      ; Row 12: body
    db 0x30, 0x00, 0x00, 0x0C      ; Row 13: body
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 14: bottom edge
    db 0x00, 0x00, 0x00, 0x00      ; Row 15: empty

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
FLOPPY_BOOT_SECTORS     equ 1       ; Sector 0
FLOPPY_STAGE2_START     equ 1       ; Sectors 1-4
FLOPPY_STAGE2_SECTORS   equ 4
FLOPPY_KERNEL_START     equ 5       ; Sectors 5-68
FLOPPY_KERNEL_SECTORS   equ 64
FLOPPY_FS_START         equ 70      ; Sector 70
FLOPPY_TOTAL_SECTORS    equ 2880

; FAT12 filesystem parameters (matching kernel's hard-coded values)
FAT12_RESERVED          equ 1       ; 1 reserved sector (FS BPB)
FAT12_NUM_FATS          equ 2
FAT12_SPF               equ 9       ; Sectors per FAT
FAT12_ROOT_ENTRIES      equ 224
FAT12_ROOT_SECTORS      equ 14      ; (224 * 32) / 512

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create window
    mov bx, 30                      ; X
    mov cx, 30                      ; Y
    mov dx, 260                     ; Width
    mov si, 130                     ; Height
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

    ; Show initial instructions
    call draw_ui

    ; Check boot drive - must be HD for this to make sense
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    test al, 0x80
    jnz .hd_boot_ok

    ; Booting from floppy - warn user
    mov si, msg_floppy_warn
    mov bx, 4
    mov cx, 48
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov si, msg_floppy_warn2
    mov bx, 4
    mov cx, 58
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .wait_key

.hd_boot_ok:
    ; Mount HD filesystem (mount handle 1)
    mov al, 0x80
    mov ah, API_FS_MOUNT
    int 0x80

.wait_key:
    ; Wait for ENTER to start, ESC to cancel
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .wait_key
    cmp al, EVENT_KEY_PRESS
    jne .check_redraw
    cmp dl, 13                      ; ENTER
    je .start_write
    cmp dl, 27                      ; ESC
    je .exit_ok
    jmp .wait_key

.check_redraw:
    cmp al, EVENT_WIN_REDRAW
    jne .wait_key
    call draw_ui
    jmp .wait_key

.start_write:
    ; Clear status area
    call clear_status

    ; === Step 1: Write boot sector ===
    mov si, msg_step_boot
    call show_status

    mov ax, cs
    mov es, ax
    mov bx, embedded_boot          ; ES:BX = boot sector data
    mov si, 0                       ; LBA sector 0
    mov dl, 0x00                    ; Drive A:
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error_write

    ; === Step 2: Write stage2 (4 sectors) ===
    mov si, msg_step_stage2
    call show_status

    mov cx, FLOPPY_STAGE2_SECTORS
    mov word [cs:current_lba], FLOPPY_STAGE2_START
    mov word [cs:buffer_offset], embedded_stage2
.write_stage2_loop:
    push cx
    mov ax, cs
    mov es, ax
    mov bx, [cs:buffer_offset]
    mov si, [cs:current_lba]
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    pop cx
    jc .error_write
    add word [cs:buffer_offset], 512
    inc word [cs:current_lba]
    loop .write_stage2_loop

    ; === Step 3: Write kernel from memory (64 sectors from 0x1000:0000) ===
    mov si, msg_step_kernel
    call show_status

    mov cx, FLOPPY_KERNEL_SECTORS
    mov word [cs:current_lba], FLOPPY_KERNEL_START
    mov word [cs:buffer_offset], 0
.write_kernel_loop:
    push cx
    mov ax, 0x1000                  ; Kernel segment
    mov es, ax
    mov bx, [cs:buffer_offset]
    mov si, [cs:current_lba]
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    pop cx
    jc .error_write
    add word [cs:buffer_offset], 512
    inc word [cs:current_lba]
    loop .write_kernel_loop

    ; === Step 4: Write FAT12 filesystem structure ===
    mov si, msg_step_fat
    call show_status

    ; 4a: Write FS BPB at sector 70
    call build_fs_bpb               ; Builds BPB in sector_buffer
    mov ax, cs
    mov es, ax
    mov bx, sector_buffer
    mov si, FLOPPY_FS_START
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error_write

    ; 4b: Write FAT1 (9 sectors starting at sector 71)
    ; First sector has media byte + 2 reserved entries
    call build_empty_fat_sector     ; Builds first FAT sector in sector_buffer
    mov ax, cs
    mov es, ax
    mov bx, sector_buffer
    mov si, FLOPPY_FS_START + FAT12_RESERVED
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error_write

    ; Remaining FAT1 sectors (8 sectors of zeros)
    call clear_sector_buffer
    mov cx, FAT12_SPF - 1           ; 8 more sectors
    mov word [cs:current_lba], FLOPPY_FS_START + FAT12_RESERVED + 1
.write_fat1_loop:
    push cx
    mov ax, cs
    mov es, ax
    mov bx, sector_buffer
    mov si, [cs:current_lba]
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    pop cx
    jc .error_write
    inc word [cs:current_lba]
    loop .write_fat1_loop

    ; 4c: Write FAT2 (copy of FAT1)
    ; First sector with media byte
    call build_empty_fat_sector
    mov ax, cs
    mov es, ax
    mov bx, sector_buffer
    mov si, FLOPPY_FS_START + FAT12_RESERVED + FAT12_SPF
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error_write

    ; Remaining FAT2 sectors
    call clear_sector_buffer
    mov cx, FAT12_SPF - 1
    mov word [cs:current_lba], FLOPPY_FS_START + FAT12_RESERVED + FAT12_SPF + 1
.write_fat2_loop:
    push cx
    mov ax, cs
    mov es, ax
    mov bx, sector_buffer
    mov si, [cs:current_lba]
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    pop cx
    jc .error_write
    inc word [cs:current_lba]
    loop .write_fat2_loop

    ; 4d: Write empty root directory (14 sectors)
    ; First sector: volume label entry
    call build_root_dir_sector      ; Volume label in first entry
    mov ax, cs
    mov es, ax
    mov bx, sector_buffer
    mov si, FLOPPY_FS_START + FAT12_RESERVED + (FAT12_NUM_FATS * FAT12_SPF)
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    jc .error_write

    ; Remaining root dir sectors (zeros)
    call clear_sector_buffer
    mov cx, FAT12_ROOT_SECTORS - 1
    mov word [cs:current_lba], FLOPPY_FS_START + FAT12_RESERVED + (FAT12_NUM_FATS * FAT12_SPF) + 1
.write_rootdir_loop:
    push cx
    mov ax, cs
    mov es, ax
    mov bx, sector_buffer
    mov si, [cs:current_lba]
    mov dl, 0x00
    mov ah, API_FS_WRITE_SECTOR
    int 0x80
    pop cx
    jc .error_write
    inc word [cs:current_lba]
    loop .write_rootdir_loop

    ; === Step 5: Mount the new floppy filesystem ===
    mov si, msg_step_copy
    call show_status

    ; Mount floppy (mount handle 0) - sets kernel's FAT12 variables
    mov al, 0x00
    mov ah, API_FS_MOUNT
    int 0x80

    ; === Step 6: Copy app files from HD to floppy ===
    ; Scan HD directory and copy each .BIN file
    mov word [cs:dir_index], 0
    mov byte [cs:files_copied], 0

.copy_next_file:
    ; Read next directory entry from HD (mount handle 1)
    mov ax, cs
    mov es, ax
    mov di, dir_entry_buf           ; ES:DI = buffer for dir entry
    mov ax, [cs:dir_index]
    mov bl, 1                       ; Mount handle 1 = HD
    mov ah, API_FS_READDIR
    int 0x80
    jc .copy_done                   ; End of directory or error

    inc word [cs:dir_index]

    ; Check if it's a BIN file (extension at offset 8-10)
    cmp byte [cs:dir_entry_buf + 8], 'B'
    jne .copy_next_file
    cmp byte [cs:dir_entry_buf + 9], 'I'
    jne .copy_next_file
    cmp byte [cs:dir_entry_buf + 10], 'N'
    jne .copy_next_file

    ; Skip KERNEL.BIN (it's already written as raw sectors)
    cmp byte [cs:dir_entry_buf], 'K'
    je .copy_next_file

    ; Convert FAT 8.3 name to dot format for fs_open/fs_create
    call convert_fat_to_dot         ; Converts dir_entry_buf → dot_filename

    ; Show which file we're copying
    mov si, dot_filename
    mov bx, 4
    mov cx, 78
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Open file on HD
    mov si, dot_filename
    mov bx, 1                       ; Mount handle 1 = HD (uses BX not BL)
    mov ah, API_FS_OPEN
    int 0x80
    jc .copy_next_file              ; Skip if can't open

    mov [cs:hd_file_handle], al

    ; Create file on floppy
    mov si, dot_filename
    mov bl, 0                       ; Mount handle 0 = floppy
    mov ah, API_FS_CREATE
    int 0x80
    jc .close_hd_skip

    mov [cs:floppy_file_handle], al

    ; Copy loop: read from HD, write to floppy
.copy_file_loop:
    ; Read 512 bytes from HD file
    mov al, [cs:hd_file_handle]
    mov ax, cs
    mov es, ax
    mov di, copy_buffer             ; ES:DI for fs_read
    mov cx, 512
    mov al, [cs:hd_file_handle]
    mov ah, API_FS_READ
    int 0x80
    jc .copy_file_done              ; Error or EOF
    test ax, ax
    jz .copy_file_done              ; 0 bytes = done

    mov [cs:bytes_read], ax

    ; Write to floppy file
    mov ax, cs
    mov es, ax
    mov bx, copy_buffer             ; ES:BX for fs_write
    mov cx, [cs:bytes_read]
    mov al, [cs:floppy_file_handle]
    mov ah, API_FS_WRITE
    int 0x80
    jc .copy_file_done

    ; Check if we read a full sector (more data might follow)
    cmp word [cs:bytes_read], 512
    je .copy_file_loop

.copy_file_done:
    ; Close floppy file
    mov al, [cs:floppy_file_handle]
    mov ah, API_FS_CLOSE
    int 0x80

.close_hd_skip:
    ; Close HD file
    mov al, [cs:hd_file_handle]
    mov ah, API_FS_CLOSE
    int 0x80

    inc byte [cs:files_copied]

    ; Clear the filename display line
    mov bx, 4
    mov cx, 78
    mov dx, 250
    mov si, 88
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    jmp .copy_next_file

.copy_done:
    ; === Done! ===
    mov si, msg_done
    call show_status

    ; Show file count
    mov al, [cs:files_copied]
    add al, '0'
    mov [cs:count_char], al
    mov si, msg_file_count
    mov bx, 4
    mov cx, 78
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Wait for ESC to exit
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

.error_write:
    mov si, msg_error
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
; Helper: Draw main UI
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

    mov si, msg_press_enter
    mov bx, 4
    mov cx, 32
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    pop si
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Helper: Show status message at line 48
; ============================================================================
show_status:
    push ax
    push bx
    push cx
    push si

    ; Status line Y position advances with each call
    mov cx, [cs:status_y]
    mov bx, 4
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    add word [cs:status_y], 10

    pop si
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Helper: Clear status area
; ============================================================================
clear_status:
    push ax
    push bx
    push cx
    push dx
    push si

    mov bx, 0
    mov cx, 46
    mov dx, 258
    mov si, 120
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    mov word [cs:status_y], 48

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Helper: Build FAT12 BPB in sector_buffer
; ============================================================================
build_fs_bpb:
    push ax
    push cx
    push di
    push es

    ; Clear buffer
    call clear_sector_buffer

    mov ax, cs
    mov es, ax
    mov di, sector_buffer

    ; BPB fields
    mov byte [es:di + 0], 0xEB      ; JMP short
    mov byte [es:di + 1], 0x3C
    mov byte [es:di + 2], 0x90      ; NOP

    ; OEM name
    mov dword [es:di + 3], 'UNOD'
    mov dword [es:di + 7], 'OS  '

    mov word [es:di + 11], 512      ; Bytes per sector
    mov byte [es:di + 13], 1        ; Sectors per cluster
    mov word [es:di + 14], 1        ; Reserved sectors
    mov byte [es:di + 16], 2        ; Number of FATs
    mov word [es:di + 17], 224      ; Root entries
    mov word [es:di + 19], 2810     ; Total sectors in FS area
    mov byte [es:di + 21], 0xF0     ; Media descriptor
    mov word [es:di + 22], 9        ; Sectors per FAT
    mov word [es:di + 24], 18       ; Sectors per track
    mov word [es:di + 26], 2        ; Heads
    mov byte [es:di + 38], 0x29     ; Extended boot sig
    mov dword [es:di + 39], 0x12345678  ; Volume serial
    ; Volume label
    mov dword [es:di + 43], 'UNOD'
    mov dword [es:di + 47], 'OS  '
    mov word [es:di + 51], '  '
    mov byte [es:di + 53], ' '
    ; FS type
    mov dword [es:di + 54], 'FAT1'
    mov dword [es:di + 58], '2   '
    ; Boot signature
    mov byte [es:di + 510], 0x55
    mov byte [es:di + 511], 0xAA

    pop es
    pop di
    pop cx
    pop ax
    ret

; ============================================================================
; Helper: Build first FAT sector (media byte + reserved entries)
; ============================================================================
build_empty_fat_sector:
    call clear_sector_buffer
    ; FAT12 first 3 bytes: media descriptor + EOF markers
    mov byte [cs:sector_buffer], 0xF0
    mov byte [cs:sector_buffer + 1], 0xFF
    mov byte [cs:sector_buffer + 2], 0xFF
    ret

; ============================================================================
; Helper: Build root directory first sector (volume label)
; ============================================================================
build_root_dir_sector:
    call clear_sector_buffer
    ; Volume label entry (32 bytes)
    mov dword [cs:sector_buffer + 0], 'UNOD'
    mov dword [cs:sector_buffer + 4], 'OS  '
    mov word [cs:sector_buffer + 8], '  '
    mov byte [cs:sector_buffer + 10], ' '
    mov byte [cs:sector_buffer + 11], 0x08  ; Volume label attribute
    ret

; ============================================================================
; Helper: Clear sector_buffer (512 bytes of zeros)
; ============================================================================
clear_sector_buffer:
    push ax
    push cx
    push di
    push es
    mov ax, cs
    mov es, ax
    mov di, sector_buffer
    xor ax, ax
    mov cx, 256
    rep stosw
    pop es
    pop di
    pop cx
    pop ax
    ret

; ============================================================================
; Helper: Convert FAT 8.3 name (dir_entry_buf) to dot format (dot_filename)
; Input: dir_entry_buf contains 11-byte FAT name
; Output: dot_filename contains "NAME.EXT",0
; ============================================================================
convert_fat_to_dot:
    push ax
    push cx
    push si
    push di

    mov si, dir_entry_buf
    mov di, dot_filename

    ; Copy name part (8 bytes, strip trailing spaces)
    mov cx, 8
.copy_name:
    mov al, [cs:si]
    cmp al, ' '
    je .name_done
    mov [cs:di], al
    inc si
    inc di
    loop .copy_name
    jmp .add_dot
.name_done:
    ; SI might not have advanced past all 8 name bytes
    mov si, dir_entry_buf
    add si, 8                       ; Point to extension

    jmp .check_ext
.add_dot:
    mov si, dir_entry_buf
    add si, 8
.check_ext:
    ; Check if extension exists (not all spaces)
    cmp byte [cs:si], ' '
    je .no_ext

    mov byte [cs:di], '.'
    inc di

    ; Copy extension (3 bytes, strip trailing spaces)
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
.no_ext:
    mov byte [cs:di], 0             ; Null terminate

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
msg_press_enter: db 'ENTER=Start  ESC=Cancel', 0
msg_floppy_warn: db 'Warning: Boot from HD to', 0
msg_floppy_warn2: db 'create floppies.', 0
msg_step_boot:  db 'Writing boot sector...', 0
msg_step_stage2: db 'Writing stage2 loader...', 0
msg_step_kernel: db 'Writing kernel (32KB)...', 0
msg_step_fat:   db 'Writing FAT12 filesystem...', 0
msg_step_copy:  db 'Copying apps to floppy...', 0
msg_done:       db 'Done! Floppy is bootable.', 0
msg_error:      db 'ERROR: Write failed!', 0
msg_file_count: db 'Copied '
count_char:     db '0'
              db ' files.', 0

; ============================================================================
; Variables
; ============================================================================

win_handle:         db 0
current_lba:        dw 0
buffer_offset:      dw 0
status_y:           dw 48
dir_index:          dw 0
files_copied:       db 0
hd_file_handle:     db 0
floppy_file_handle: db 0
bytes_read:         dw 0

; 8.3 FAT name buffer and dot format buffer
dir_entry_buf:  times 32 db 0       ; Raw directory entry (32 bytes)
dot_filename:   times 13 db 0       ; "FILENAME.EXT",0

; ============================================================================
; Embedded boot sector (512 bytes)
; ============================================================================
embedded_boot:
    incbin 'build/boot.bin'

; ============================================================================
; Embedded stage2 loader (2048 bytes)
; ============================================================================
embedded_stage2:
    incbin 'build/stage2.bin'

; ============================================================================
; Sector buffer (512 bytes for building FAT/root dir sectors)
; ============================================================================
sector_buffer:  times 512 db 0

; ============================================================================
; Copy buffer (512 bytes for file copy operations)
; ============================================================================
copy_buffer:    times 512 db 0
