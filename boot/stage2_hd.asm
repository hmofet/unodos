; UnoDOS Stage2 Hard Drive Loader
; Loaded by VBR at 0x0800:0x0000
; Loads KERNEL.BIN from FAT16 filesystem
;
; NOTE: This version requires 386+ CPU (uses EAX, EBX, ECX, EDX, movzx, etc.)
; TODO: Create 8086-compatible version for HP 200LX, Sharp PC-3100, etc.
;
; Memory layout during boot:
;   0x0600:0x0000 - Relocated MBR
;   0x0000:0x7C00 - VBR (contains BPB)
;   0x0800:0x0000 - This stage2 loader
;   0x0900:0x0000 - Sector buffer (512 bytes)
;   0x0920:0x0000 - FAT cache (512 bytes)
;   0x1000:0x0000 - Kernel (loads here)

[BITS 16]
[ORG 0x0000]

; Signature (checked by VBR)
signature:
    dw 0x5355                       ; 'US' = UnoDOS Stage2

entry:
    ; Set up segments
    mov ax, 0x0800
    mov ds, ax
    mov es, ax

    ; Save boot drive
    mov [boot_drive], dl

    ; Display loading message
    mov si, loading_msg
    call print_string

    ; Parse BPB from VBR (still in memory at 0x7C00)
    ; Store key values for calculations
    xor ax, ax
    mov es, ax                      ; ES = 0 for accessing 0x7C00

    ; Reserved sectors
    mov ax, [es:0x7C00 + 0x0E]
    mov [reserved_sects], ax

    ; Sectors per FAT
    mov ax, [es:0x7C00 + 0x16]
    mov [sects_per_fat], ax

    ; Number of FATs
    xor ah, ah
    mov al, [es:0x7C00 + 0x10]
    mov [num_fats], al

    ; Root directory entries
    mov ax, [es:0x7C00 + 0x11]
    mov [root_entries], ax

    ; Sectors per cluster
    mov al, [es:0x7C00 + 0x0D]
    mov [sects_per_cluster], al

    ; Hidden sectors (partition start LBA)
    mov eax, [es:0x7C00 + 0x1C]
    mov [partition_lba], eax

    ; Calculate FAT start sector
    ; fat_start = hidden_sectors + reserved_sectors
    movzx eax, word [reserved_sects]
    add eax, [partition_lba]
    mov [fat_start_lba], eax

    ; Calculate root directory start sector
    ; root_start = fat_start + (num_fats * sectors_per_fat)
    movzx eax, byte [num_fats]
    movzx ebx, word [sects_per_fat]
    imul eax, ebx
    add eax, [fat_start_lba]
    mov [root_start_lba], eax

    ; Calculate data area start sector
    ; data_start = root_start + (root_entries * 32 / 512)
    movzx ebx, word [root_entries]
    shr ebx, 4                      ; Divide by 16 (32/512 = 1/16)
    add eax, ebx
    mov [data_start_lba], eax

    ; Restore ES to stage2 segment (was set to 0 for BPB read)
    mov ax, 0x0800
    mov es, ax

    ; Search root directory for KERNEL.BIN
    mov eax, [root_start_lba]
    movzx ecx, word [root_entries]
    shr ecx, 4                      ; Root directory sectors

.search_root:
    push ecx
    push eax

    ; Read root directory sector
    call read_sector_lba
    jc near .disk_error

    ; Search 16 entries in this sector
    mov si, sector_buffer
    mov cx, 16

.search_entry:
    ; Check if entry is used
    mov al, [si]
    test al, al                     ; End of directory?
    jz near .not_found
    cmp al, 0xE5                    ; Deleted entry?
    je .next_entry

    ; Compare with "KERNEL  BIN"
    push si
    push di
    mov di, kernel_name
    push cx
    mov cx, 11
    repe cmpsb
    pop cx
    pop di
    pop si
    je .found_kernel

.next_entry:
    add si, 32
    loop .search_entry

    ; Next sector
    pop eax
    pop ecx
    inc eax
    loop .search_root
    jmp .not_found

.found_kernel:
    ; Clean up stack
    add sp, 8                       ; Pop saved eax and ecx

    ; Get starting cluster (offset 26) and file size (offset 28)
    mov ax, [si + 26]
    mov [kernel_cluster], ax
    mov eax, [si + 28]
    mov [kernel_size], eax

    ; Print dot to show progress
    mov al, '.'
    call print_char

    ; Load kernel to 0x1000:0x0000
    mov ax, 0x1000
    mov es, ax
    xor di, di                      ; ES:DI = load address

    ; Load cluster by cluster
.load_cluster:
    ; Check if done
    cmp word [kernel_cluster], 0xFFF8
    jae .kernel_loaded
    cmp word [kernel_cluster], 0
    je .kernel_loaded

    ; Calculate sector for this cluster
    ; sector = data_start + (cluster - 2) * sects_per_cluster
    movzx eax, word [kernel_cluster]
    sub eax, 2
    movzx ebx, byte [sects_per_cluster]
    imul eax, ebx
    add eax, [data_start_lba]

    ; Read all sectors in cluster
    movzx cx, byte [sects_per_cluster]

.read_cluster_sector:
    push cx
    push eax

    ; Read sector to ES:DI
    call read_sector_to_esdi
    jc near .disk_error

    ; Advance destination pointer
    add di, 512
    jnc .no_segment_wrap
    ; Handle segment wrap (every 64KB)
    mov ax, es
    add ax, 0x1000
    mov es, ax
.no_segment_wrap:

    ; Print dot
    mov al, '.'
    call print_char

    pop eax
    pop cx
    inc eax
    loop .read_cluster_sector

    ; Get next cluster from FAT
    mov ax, [kernel_cluster]
    call get_next_cluster
    mov [kernel_cluster], ax
    jmp .load_cluster

.kernel_loaded:
    ; Print success message
    mov ax, 0x0800
    mov ds, ax
    mov si, ok_msg
    call print_string

    ; Verify kernel signature
    mov ax, 0x1000
    mov es, ax
    cmp word [es:0x0000], 0x4B55    ; 'UK' = UnoDOS Kernel
    jne .invalid_kernel

    ; Jump to kernel
    ; Pass drive number in DL
    mov dl, [boot_drive]
    jmp 0x1000:0x0002               ; Jump past signature to entry

.disk_error:
    mov ax, 0x0800
    mov ds, ax
    mov si, disk_err_msg
    call print_string
    jmp halt

.not_found:
    mov ax, 0x0800
    mov ds, ax
    mov si, not_found_msg
    call print_string
    jmp halt

.invalid_kernel:
    mov ax, 0x0800
    mov ds, ax
    mov si, invalid_kernel_msg
    call print_string
    jmp halt

; ============================================================================
; read_sector_lba - Read sector to sector_buffer
; Input: EAX = LBA
; Output: CF = 0 success, data in sector_buffer
; ============================================================================

read_sector_lba:
    push es
    push bx
    mov bx, 0x0800
    mov es, bx
    mov bx, sector_buffer
    call read_sector_to_esbx
    pop bx
    pop es
    ret

; ============================================================================
; read_sector_to_esbx - Read sector to ES:BX
; Input: EAX = LBA, ES:BX = buffer
; Output: CF = 0 success
; ============================================================================

read_sector_to_esbx:
    push eax
    push cx
    push dx
    push si

    ; Save buffer location for verification
    mov [temp_seg], es
    mov [temp_off], bx

    ; Try INT 13h extended read first
    push dword 0                    ; High LBA
    push eax                        ; Low LBA
    push es                         ; Buffer segment
    push bx                         ; Buffer offset
    push word 1                     ; Sectors
    push word 0x0010                ; Packet size
    mov si, sp

    mov ah, 0x42
    mov dl, [boot_drive]
    push ds
    push ss
    pop ds
    int 0x13
    pop ds
    add sp, 16
    jnc .success                    ; LBA succeeded, trust it
    ; LBA failed, try CHS

.try_chs:
    ; Get drive geometry
    push es
    xor di, di
    mov es, di
    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    pop es
    jc .use_defaults

    ; Parse and validate geometry
    mov al, cl
    and al, 0x3F                    ; Sectors per track
    test al, al                     ; Check for 0 (invalid)
    jz .use_defaults
    mov [saved_spt], al
    inc dh                          ; Max head -> number of heads
    test dh, dh                     ; Check for 0 (wrapped)
    jz .use_defaults
    mov [saved_heads], dh
    jmp .do_chs_read

.use_defaults:
    mov byte [saved_spt], 63
    mov byte [saved_heads], 16

.do_chs_read:
    ; Convert LBA to CHS
    mov eax, [esp + 6]              ; Get original EAX from stack (after si,dx,cx pushes)
    xor edx, edx
    movzx ecx, byte [saved_spt]
    div ecx                         ; EAX = LBA / SPT, EDX = LBA mod SPT
    inc dl                          ; Sector (1-based)
    mov cl, dl                      ; CL = sector

    xor edx, edx
    movzx ecx, byte [saved_heads]
    div ecx                         ; EAX = cylinder, EDX = head
    mov dh, dl                      ; DH = head
    mov ch, al                      ; CH = cylinder low
    shl ah, 6
    or cl, ah                       ; CL[7:6] = cylinder high

    mov ah, 0x02                    ; Read sectors
    mov al, 1
    push es
    mov es, [temp_seg]
    mov bx, [temp_off]
    mov dl, [boot_drive]
    int 0x13
    pop es
    jc .error

.success:
    clc
    jmp .done

.error:
    stc

.done:
    pop si
    pop dx
    pop cx
    pop eax
    ret

; Saved geometry for CHS conversion
saved_spt:   db 63
saved_heads: db 16
temp_seg:    dw 0
temp_off:    dw 0

; ============================================================================
; read_sector_to_esdi - Read sector to ES:DI
; Input: EAX = LBA, ES:DI = buffer
; Output: CF = 0 success
; ============================================================================

read_sector_to_esdi:
    push bx
    mov bx, di
    call read_sector_to_esbx
    pop bx
    ret

; ============================================================================
; get_next_cluster - Get next cluster from FAT
; Input: AX = current cluster
; Output: AX = next cluster
; ============================================================================

get_next_cluster:
    push bx
    push cx
    push dx
    push es

    ; FAT16: each entry is 2 bytes
    ; FAT sector = cluster / 256
    ; Offset in sector = (cluster mod 256) * 2

    mov bx, ax                      ; Save cluster
    shr ax, 8                       ; Sector index within FAT
    movzx eax, ax
    add eax, [fat_start_lba]

    ; Check if this FAT sector is cached
    cmp eax, [cached_fat_sector]
    je .use_cache

    ; Load FAT sector
    mov [cached_fat_sector], eax
    push bx
    mov bx, 0x0800
    mov es, bx
    mov bx, fat_cache
    call read_sector_to_esbx
    pop bx
    jc .fat_error

.use_cache:
    ; Get entry from cache
    mov ax, bx
    and ax, 0x00FF                  ; cluster mod 256
    shl ax, 1                       ; * 2 bytes per entry
    mov bx, ax
    mov ax, [fat_cache + bx]

    pop es
    pop dx
    pop cx
    pop bx
    ret

.fat_error:
    mov ax, 0xFFFF                  ; Return end-of-chain on error
    pop es
    pop dx
    pop cx
    pop bx
    ret

; ============================================================================
; print_string - Print null-terminated string
; ============================================================================

print_string:
    push ax
    push bx
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    jmp .loop
.done:
    pop bx
    pop ax
    ret

; ============================================================================
; print_char - Print single character
; ============================================================================

print_char:
    push ax
    push bx
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    pop bx
    pop ax
    ret

; print_hex_byte - Print AL as hex
print_hex_byte:
    push ax
    push bx
    push cx
    mov cl, al
    shr al, 4
    call .nibble
    mov al, cl
    and al, 0x0F
    call .nibble
    pop cx
    pop bx
    pop ax
    ret
.nibble:
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .out
.digit:
    add al, '0'
.out:
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    ret

; print_hex_word - Print AX as hex
print_hex_word:
    push ax
    mov al, ah
    call print_hex_byte
    pop ax
    push ax
    call print_hex_byte
    pop ax
    ret

halt:
    cli                             ; Disable interrupts to prevent wakeup
    hlt
    jmp halt

; ============================================================================
; Data
; ============================================================================

boot_drive:         db 0x80
reserved_sects:     dw 0
sects_per_fat:      dw 0
num_fats:           db 0
root_entries:       dw 0
sects_per_cluster:  db 0
partition_lba:      dd 0
fat_start_lba:      dd 0
root_start_lba:     dd 0
data_start_lba:     dd 0
kernel_cluster:     dw 0
kernel_size:        dd 0
cached_fat_sector:  dd 0xFFFFFFFF

kernel_name:        db 'KERNEL  BIN'    ; 8.3 format (no dot)

loading_msg:        db 'Loading UnoDOS kernel', 0
ok_msg:             db ' OK', 13, 10, 0
disk_err_msg:       db 13, 10, 'Disk error!', 13, 10, 0
not_found_msg:      db 13, 10, 'KERNEL.BIN not found!', 13, 10, 0
invalid_kernel_msg: db 13, 10, 'Invalid kernel!', 13, 10, 0

; ============================================================================
; Buffers (must be within first segment for easy access)
; ============================================================================

; Align to 512 bytes for sector buffer
align 512
sector_buffer:      times 512 db 0
fat_cache:          times 512 db 0

; Pad to ensure total stage2 fits in 4 sectors (2KB)
times 2048 - ($ - $$) db 0
