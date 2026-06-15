; UnoDOS Stage2 Hard Drive Loader
; Loaded by VBR at 0x0800:0x0000
; Loads KERNEL.BIN from FAT16 filesystem
;
; 8086/8088-clean: all 32-bit LBAs are held as 16-bit word pairs, products use
; MUL, and the CHS fallback divides a 32-bit LBA via two chained 16-bit DIVs.
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
cpu 8086            ; Target CPU: Intel 8088/8086 (PC/XT)

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

    ; Hidden sectors (partition start LBA) — 32-bit as words
    mov ax, [es:0x7C00 + 0x1C]
    mov [partition_lba], ax
    mov ax, [es:0x7C00 + 0x1E]
    mov [partition_lba + 2], ax

    ; Restore ES to stage2 segment
    mov ax, 0x0800
    mov es, ax

    ; All LBAs are 32-bit, held as word pairs (low at +0, high at +2).
    ; fat_start = partition_lba + reserved_sectors
    mov ax, [reserved_sects]
    xor dx, dx
    add ax, [partition_lba]
    adc dx, [partition_lba + 2]
    mov [fat_start_lba], ax
    mov [fat_start_lba + 2], dx

    ; root_start = fat_start + (num_fats * sectors_per_fat)
    mov al, [num_fats]
    xor ah, ah
    mov bx, [sects_per_fat]
    mul bx                           ; DX:AX = num_fats * sects_per_fat (32-bit)
    add ax, [fat_start_lba]
    adc dx, [fat_start_lba + 2]
    mov [root_start_lba], ax
    mov [root_start_lba + 2], dx

    ; data_start = root_start + (root_entries * 32 / 512) = root_start + ents/16
    mov bx, [root_entries]
    shr bx, 1
    shr bx, 1
    shr bx, 1
    shr bx, 1                        ; BX = root directory sectors
    add ax, bx                       ; AX:DX still = root_start
    adc dx, 0
    mov [data_start_lba], ax
    mov [data_start_lba + 2], dx

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

    mov ax, [root_start_lba]
    mov dx, [root_start_lba + 2]
    mov [cur_lba], ax
    mov [cur_lba + 2], dx
    mov cx, [root_entries]
    shr cx, 1
    shr cx, 1
    shr cx, 1
    shr cx, 1                        ; CX = root directory sectors
    mov [root_sects_left], cx

.search_root:
    ; Read root directory sector (LBA in DX:AX)
    mov ax, [cur_lba]
    mov dx, [cur_lba + 2]
    call read_sector_lba
    jc near .disk_error

    ; Progress dot for each root dir sector
    mov al, '.'
    call print_char

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

    ; Next root directory sector (32-bit LBA increment)
    add word [cur_lba], 1
    adc word [cur_lba + 2], 0
    dec word [root_sects_left]
    jnz .search_root
    jmp .not_found

.found_kernel:
    ; Get starting cluster (offset 26) and file size (offset 28)
    mov ax, [si + 26]
    mov [kernel_cluster], ax
    mov ax, [si + 28]
    mov [kernel_size], ax
    mov ax, [si + 30]
    mov [kernel_size + 2], ax

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

    ; cluster LBA = data_start + (cluster - 2) * sects_per_cluster
    mov ax, [kernel_cluster]
    sub ax, 2
    mov bl, [sects_per_cluster]
    xor bh, bh
    mul bx                           ; DX:AX = (cluster-2) * spc (32-bit)
    add ax, [data_start_lba]
    adc dx, [data_start_lba + 2]
    mov [cur_lba], ax
    mov [cur_lba + 2], dx

    ; Read all sectors in cluster
    mov cl, [sects_per_cluster]
    xor ch, ch                       ; CX = sectors in this cluster

.read_cluster_sector:
    push cx

    ; Read sector (LBA in DX:AX) to ES:DI
    mov ax, [cur_lba]
    mov dx, [cur_lba + 2]
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

    ; Advance LBA (32-bit)
    add word [cur_lba], 1
    adc word [cur_lba + 2], 0
    pop cx
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

; Input: DX:AX = LBA (DX high, AX low), ES:BX = buffer.
read_sector_to_esbx:
    push ax
    push bx
    push cx
    push dx

    ; Fill static DAP with current read parameters (LBA as two words;
    ; dap_lba+4/+6 stay 0 from initialization, covering the LBA28/32 range).
    mov [dap_buf_off], bx
    mov [dap_buf_seg], es
    mov [dap_lba], ax
    mov [dap_lba + 2], dx

    ; Try LBA if BIOS supports extensions
    cmp byte [lba_supported], 0
    je .chs_read

    ; CRITICAL: Use DS=0 for INT 13h AH=42h DAP pointer (some BIOSes read the
    ; DAP only from segment 0). Convert DAP address to linear for DS=0.
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
    ; Convert the 32-bit LBA to CHS (two chained 16-bit DIVs). BX is the
    ; divisor; CL holds the sector between the divisions.
    xor bx, bx
    mov bl, [bios_spt]
    mov ax, [dap_lba + 2]           ; high word
    xor dx, dx
    div bx                          ; AX = hi/SPT, DX = hi%SPT
    mov [chs_q_hi], ax
    mov ax, [dap_lba]               ; low word (DX = carry-in)
    div bx                          ; AX = lo quotient, DX = LBA mod SPT
    mov [chs_q_lo], ax
    inc dx                          ; sector = remainder + 1
    mov cl, dl                      ; CL[5:0] = sector

    xor bx, bx
    mov bl, [bios_heads]
    mov ax, [chs_q_hi]
    xor dx, dx
    div bx                          ; AX = qhi/heads, DX = qhi%heads
    mov ax, [chs_q_lo]              ; DX = carry-in
    div bx                          ; AX = cylinder, DX = head
    mov dh, dl                      ; DH = head
    mov ch, al                      ; CH = cylinder low 8 bits
    mov al, ah                      ; AL = cylinder high byte
    and al, 0x03
    ror al, 1
    ror al, 1                       ; bits 0-1 -> bits 6-7 (== shl al,6 here)
    or cl, al                       ; CL[7:6] = cylinder high

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
    pop bx
    pop ax
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

    mov bx, ax                      ; BX = cluster
    mov al, ah                      ; AL = cluster >> 8 (FAT sector index 0..255)
    xor ah, ah                      ; AX = FAT sector index
    add ax, [fat_start_lba]
    mov dx, 0
    adc dx, [fat_start_lba + 2]     ; DX:AX = FAT sector LBA (32-bit)

    ; Check if this FAT sector is cached (32-bit compare)
    cmp ax, [cached_fat_sector]
    jne .load_fat
    cmp dx, [cached_fat_sector + 2]
    je .use_cache

.load_fat:
    mov [cached_fat_sector], ax
    mov [cached_fat_sector + 2], dx
    push bx
    mov bx, 0x0800
    mov es, bx
    mov bx, fat_cache
    call read_sector_to_esbx        ; LBA in DX:AX
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
    shr al, 1
    shr al, 1
    shr al, 1
    shr al, 1
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
cur_lba:            dd 0            ; current 32-bit LBA for the read loops
root_sects_left:    dw 0            ; remaining root directory sectors to scan
chs_q_hi:           dw 0            ; CHS division: quotient high word
chs_q_lo:           dw 0            ; CHS division: quotient low word

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
; Pad the loaded image to exactly 4 sectors (2KB).
; ============================================================================
times 2048 - ($ - $$) db 0

; ============================================================================
; Scratch buffers — placed in RAM immediately after the loaded image (within
; the 0x0800 segment) so they are NOT stored in the 4-sector stage2 binary.
; ============================================================================
sector_buffer       equ 2048            ; 0x0800:0x0800
fat_cache           equ 2048 + 512      ; 0x0800:0x0A00
