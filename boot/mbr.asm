; UnoDOS Master Boot Record (MBR)
; Loaded by BIOS at 0x0000:0x7C00
; Relocates to 0x0000:0x0600, finds bootable partition, loads VBR
;
; MBR Layout:
;   0x000-0x1BD: Boot code (446 bytes)
;   0x1BE-0x1FD: Partition table (4 × 16 bytes)
;   0x1FE-0x1FF: Boot signature (0x55AA)

[BITS 16]
[ORG 0x7C00]

start:
    ; Set up segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00                  ; Stack below MBR

    ; Save boot drive number (BIOS passes in DL)
    mov [boot_drive], dl

    ; Relocate MBR to 0x0600 to make room for VBR at 0x7C00
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256                     ; 512 bytes = 256 words
    cld
    rep movsw

    ; Jump to relocated code
    jmp 0x0000:relocated

; ============================================================================
; Relocated code (runs from 0x0600)
; ============================================================================

relocated:
    ; Find bootable partition in partition table
    mov si, 0x0600 + 0x1BE          ; Partition table in relocated MBR
    mov cx, 4                       ; 4 partition entries

.find_boot:
    test byte [si], 0x80            ; Bootable flag set?
    jnz .found_partition
    add si, 16                      ; Next partition entry
    loop .find_boot

    ; No bootable partition found
    mov si, no_boot_msg
    call print_string
    jmp halt

.found_partition:
    ; Save partition entry pointer for later
    mov [partition_entry], si

    ; Get partition start LBA (offset 8 in partition entry)
    mov eax, [si + 8]
    mov [partition_lba], eax

    ; Read VBR from partition start
    ; Try INT 13h extended read first (LBA mode)

    ; Build disk address packet on stack
    push dword 0                    ; High 32 bits of LBA (0)
    push dword [partition_lba]      ; Low 32 bits of LBA
    push word 0x0000                ; Buffer segment
    push word 0x7C00                ; Buffer offset
    push word 1                     ; Number of sectors
    push word 0x0010                ; Packet size (16 bytes)
    mov si, sp                      ; DS:SI = packet

    mov ah, 0x42                    ; Extended read
    mov dl, [boot_drive]
    int 0x13
    add sp, 16                      ; Clean up stack
    jnc .verify_vbr

    ; Extended read failed - try CHS mode
    mov si, [partition_entry]

    ; CHS values from partition entry
    mov dh, [si + 1]                ; Starting head
    mov cx, [si + 2]                ; Starting cyl/sector
    mov dl, [boot_drive]

    mov ax, 0x0201                  ; Read 1 sector
    mov bx, 0x7C00                  ; Load to 0x7C00
    int 0x13
    jc .disk_error

.verify_vbr:
    ; Verify VBR signature
    cmp word [0x7C00 + 510], 0xAA55
    jne .invalid_vbr

    ; Jump to VBR, passing drive number in DL
    mov dl, [boot_drive]
    mov si, [partition_entry]       ; SI = partition entry (some VBRs need this)
    jmp 0x0000:0x7C00

.disk_error:
    mov si, disk_error_msg
    call print_string
    jmp halt

.invalid_vbr:
    mov si, invalid_vbr_msg
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
    mov ah, 0x0E                    ; Teletype output
    xor bx, bx                      ; Page 0
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

; ============================================================================
; Data
; ============================================================================

boot_drive:      db 0
partition_entry: dw 0
partition_lba:   dd 0

no_boot_msg:     db 'No bootable partition', 13, 10, 0
disk_error_msg:  db 'Disk error', 13, 10, 0
invalid_vbr_msg: db 'Invalid VBR', 13, 10, 0

; ============================================================================
; Padding to partition table
; ============================================================================

; Pad to offset 0x1BE (446 bytes of boot code)
times 0x1BE - ($ - $$) db 0

; Partition table (64 bytes) - filled by image creation tool
partition_table:
    times 64 db 0

; Boot signature
    dw 0xAA55
