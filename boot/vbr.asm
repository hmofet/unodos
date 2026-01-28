; UnoDOS Volume Boot Record (VBR)
; Placed at the start of FAT16 partition by image creation tool
; Loaded by MBR at 0x0000:0x7C00
; Loads stage2_hd from reserved sectors, jumps to it
;
; NOTE: This version requires 386+ CPU (uses EAX, push dword).
; TODO: Create 8086-compatible version for HP 200LX, Sharp PC-3100, etc.
;
; VBR Layout:
;   0x000-0x002: Jump instruction
;   0x003-0x00A: OEM name
;   0x00B-0x023: BPB (BIOS Parameter Block)
;   0x024-0x03D: Extended BPB
;   0x03E-0x1FD: Boot code
;   0x1FE-0x1FF: Boot signature (0x55AA)

[BITS 16]
[ORG 0x7C00]

; Jump instruction (must be at offset 0)
    jmp short boot_code
    nop

; ============================================================================
; BIOS Parameter Block (BPB) - values set by image creation tool
; ============================================================================

oem_name:           db 'UNODOS  '   ; OEM name (8 bytes)

; Standard BPB (DOS 2.0)
bytes_per_sector:   dw 512          ; Bytes per sector
sects_per_cluster:  db 16           ; Sectors per cluster (8KB clusters)
reserved_sectors:   dw 4            ; Reserved sectors (VBR + stage2)
num_fats:           db 2            ; Number of FATs
root_entries:       dw 512          ; Root directory entries
total_sectors_16:   dw 0            ; Total sectors (16-bit, 0 if > 65535)
media_type:         db 0xF8         ; Media type (0xF8 = hard disk)
fat_size_16:        dw 0            ; Sectors per FAT (filled by tool)
sectors_per_track:  dw 63           ; Sectors per track
num_heads:          dw 16           ; Number of heads
hidden_sectors:     dd 0            ; Hidden sectors (partition offset)
total_sectors_32:   dd 0            ; Total sectors (32-bit)

; Extended BPB (DOS 3.31+)
drive_number:       db 0x80         ; Drive number (0x80 = first HD)
reserved1:          db 0            ; Reserved
boot_signature:     db 0x29         ; Extended boot signature
volume_serial:      dd 0x12345678   ; Volume serial number
volume_label:       db 'UNODOS     '; Volume label (11 bytes)
fs_type:            db 'FAT16   '   ; File system type (8 bytes)

; ============================================================================
; Boot code (starts at offset 0x3E)
; ============================================================================

boot_code:
    ; Set up segments
    xor ax, ax
    mov ds, ax
    mov es, ax

    ; Debug: print 'V' to show VBR is running
    mov ah, 0x0E
    mov al, 'V'
    xor bx, bx
    int 0x10

    ; Save boot drive number (MBR passes in DL)
    mov [drive_number], dl

    ; Display loading message
    mov si, loading_msg
    call print_string

    ; Load stage2_hd from reserved sectors
    ; Stage2 is 2KB (4 sectors) starting at partition sector 1
    ; Load to 0x0800:0x0000 (linear 0x8000)
    ;
    ; NOTE: Always use CHS mode here because some USB BIOSes have
    ; buggy INT 13h extensions that return success but load zeros.
    ; Stage2 is at a known fixed location so CHS is reliable.

    ; Stage2 is at LBA 64 (partition at 63 + 1) = CHS 0/1/2
    mov ax, 0x0800
    mov es, ax
    xor bx, bx                      ; ES:BX = 0x0800:0x0000

    mov ah, 0x02                    ; Read sectors
    mov al, 4                       ; 4 sectors (stage2 = 2KB)
    mov ch, 0                       ; Cylinder 0
    mov cl, 2                       ; Sector 2 (LBA 64 mod 63 + 1 = 2)
    mov dh, 1                       ; Head 1 (LBA 64 / 63 = 1)
    mov dl, [drive_number]
    int 0x13
    jc .disk_error

.verify_stage2:
    ; Verify stage2 signature
    mov ax, 0x0800
    mov es, ax

    ; Debug: print first byte loaded
    mov ah, 0x0E
    mov al, '['
    xor bx, bx
    int 0x10
    mov al, [es:0x0000]             ; First byte
    call print_hex_byte
    mov ah, 0x0E
    mov al, ']'
    xor bx, bx
    int 0x10

    cmp word [es:0x0000], 0x5355    ; 'US' (UnoDOS Stage2)
    jne .invalid_stage2

    ; Jump to stage2_hd
    ; Pass drive number in DL, partition info in SI
    mov dl, [drive_number]
    jmp 0x0800:0x0002               ; Jump past signature to entry

.disk_error:
    mov si, disk_err_msg
    call print_string
    jmp halt

.invalid_stage2:
    mov si, invalid_stage2_msg
    call print_string
    jmp halt

; ============================================================================
; Helper functions
; ============================================================================

; print_string - Print null-terminated string
; Input: DS:SI = string pointer
print_string:
    push ax
    push bx
    mov ah, 0x0E
    xor bx, bx
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    pop bx
    pop ax
    ret

halt:
    hlt
    jmp halt

; print_hex_byte - Print AL as hex
print_hex_byte:
    push ax
    push bx
    mov bx, ax
    shr al, 4                       ; High nibble
    call .print_nibble
    mov al, bl
    and al, 0x0F                    ; Low nibble
    call .print_nibble
    pop bx
    pop ax
    ret
.print_nibble:
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .print
.digit:
    add al, '0'
.print:
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    ret

; ============================================================================
; Data
; ============================================================================

loading_msg:        db 'Loading stage2...', 13, 10, 0
disk_err_msg:       db 'Disk error', 13, 10, 0
invalid_stage2_msg: db 'Invalid stage2', 13, 10, 0

; ============================================================================
; Padding and signature
; ============================================================================

; Pad to 510 bytes
times 510 - ($ - $$) db 0

; Boot signature
    dw 0xAA55
