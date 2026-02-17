; UnoDOS Stage2 Hard Drive Loader
; Loaded by VBR at 0x0800:0x0000
; Loads KERNEL.BIN from FAT16 filesystem
;
; NOTE: This version requires 386+ CPU (uses EAX, EBX, ECX, EDX, movzx, etc.)
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

    ; ── Parse BPB from VBR FIRST (while 0x7C00 is pristine) ────────────
    ; CRITICAL: Read BPB before any INT 13h calls. Some old BIOSes
    ; may use 0x7C00 area as scratch during INT 13h.

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

    ; Restore ES to stage2 segment
    mov ax, 0x0800
    mov es, ax

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

    ; ── Probe BIOS capabilities (BPB already saved) ─────────────────────

    ; Check INT 13h extensions (AH=41h)
    mov byte [lba_supported], 0
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .no_ext
    cmp bx, 0xAA55
    jne .no_ext
    mov byte [lba_supported], 1
.no_ext:

    ; Query BIOS drive geometry for CHS fallback
    push es
    xor ax, ax
    mov es, ax
    xor di, di
    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    pop es
    jc .default_geo

    ; Parse geometry: CL[5:0] = max sector, DH = max head
    mov al, cl
    and al, 0x3F
    test al, al
    jz .default_geo
    mov [bios_spt], al
    inc dh
    test dh, dh
    jz .default_geo
    mov [bios_heads], dh
    jmp .geo_done

.default_geo:
    mov byte [bios_spt], 63
    mov byte [bios_heads], 16

.geo_done:
    ; ── Diagnostic: show read method and root LBA ───────────────────────

    ; Show read method: L=LBA, C=CHS
    mov al, 'L'
    cmp byte [lba_supported], 0
    jne .show_method
    mov al, 'C'
.show_method:
    call print_char

    ; Show root_start_lba as hex (expected: 0144 = 324 decimal)
    mov ax, word [root_start_lba]
    call print_hex_word

    mov al, ' '
    call print_char

    ; ── Display loading message ───────────────────────────────────────────

    mov si, loading_msg
    call print_string

    ; ── Search root directory for KERNEL.BIN ──────────────────────────────

    mov eax, [root_start_lba]
    movzx ecx, word [root_entries]
    shr ecx, 4                      ; Root directory sectors

.search_root:
    push ecx
    push eax

    ; Read root directory sector
    call read_sector_lba
    jc near .disk_error

    ; Progress dot for each root dir sector
    push ax
    mov al, '.'
    call print_char
    pop ax

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
;
; Uses static DAP for LBA reads (avoids push dword stack issues on some
; BIOSes) and pre-queried BIOS geometry for CHS fallback.
; ============================================================================

read_sector_to_esbx:
    push eax
    push cx
    push dx

    ; Fill static DAP with current read parameters
    mov [dap_buf_off], bx
    mov [dap_buf_seg], es
    mov [dap_lba], eax
    mov dword [dap_lba + 4], 0

    ; Try LBA if BIOS supports extensions
    cmp byte [lba_supported], 0
    je .chs_read

    ; CRITICAL: Use DS=0 for INT 13h AH=42h DAP pointer.
    ; MBR and VBR both use DS=0 and work. Some BIOSes have bugs
    ; reading the DAP from non-zero segments (e.g. CF-to-IDE on
    ; Omnibook 600C). Convert DAP address to linear for DS=0.
    mov dl, [boot_drive]            ; Load before switching DS
    push ds
    push si
    mov si, dap
    add si, 0x8000                  ; Linear: 0x0800*16 + offset
    xor ax, ax
    mov ds, ax                      ; DS = 0
    mov ah, 0x42
    int 0x13
    pop si
    pop ds
    jnc .read_ok

    ; LBA failed - fall through to CHS

.chs_read:
    ; Convert LBA to CHS using pre-queried BIOS geometry
    ; CRITICAL: Use EBX for divisor (not ECX!) to avoid clobbering CL
    ; which holds the sector number between the two divisions.
    push ebx
    mov eax, [dap_lba]
    xor edx, edx
    movzx ebx, byte [bios_spt]
    div ebx                         ; EAX = LBA / SPT, EDX = LBA mod SPT
    inc dl                          ; Sector (1-based)
    mov cl, dl                      ; CL[5:0] = sector

    xor edx, edx
    movzx ebx, byte [bios_heads]
    div ebx                         ; EAX = cylinder, EDX = head
    mov dh, dl                      ; DH = head
    mov ch, al                      ; CH = cylinder low
    shl ah, 6
    or cl, ah                       ; CL[7:6] = cylinder high
    pop ebx

    mov ah, 0x02                    ; Read sectors
    mov al, 1
    mov es, [dap_buf_seg]
    mov bx, [dap_buf_off]
    mov dl, [boot_drive]
    int 0x13
    jc .read_err

.read_ok:
    clc
    jmp .read_done

.read_err:
    stc

.read_done:
    pop dx
    pop cx
    pop eax
    ret

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
lba_supported:      db 0
bios_spt:           db 63
bios_heads:         db 16
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

; Static Disk Address Packet for INT 13h extended read
; (avoids push dword on stack which fails on some BIOSes)
dap:
    db 0x10                         ; Packet size
    db 0                            ; Reserved
    dw 1                            ; Number of sectors
dap_buf_off:
    dw 0                            ; Buffer offset (filled per read)
dap_buf_seg:
    dw 0                            ; Buffer segment (filled per read)
dap_lba:
    dd 0                            ; LBA low (filled per read)
    dd 0                            ; LBA high

kernel_name:        db 'KERNEL  BIN'    ; 8.3 format (no dot)

loading_msg:        db 'Loading kernel', 0
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
