; UnoDOS Master Boot Record (MBR)
; Loaded by BIOS at 0x0000:0x7C00
; Relocates to 0x0000:0x0600, finds bootable partition, loads VBR
;
; CRITICAL: ORG is 0x0600 so all data/code labels are in the relocated
; region. This prevents the VBR load (to 0x7C00) from overwriting MBR
; variables, error strings, and the halt loop.
;
; NOTE: This version requires 386+ CPU (uses EAX, movzx, etc.)

[BITS 16]
[ORG 0x0600]

start:
    ; BIOS loads MBR at 0x7C00 - set up segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00                  ; Stack below MBR

    ; Debug: print 'M' to show MBR is running
    mov ah, 0x0E
    mov al, 'M'
    xor bx, bx
    int 0x10

    ; Relocate MBR to 0x0600 to free 0x7C00 for VBR
    ; DL (boot drive from BIOS) is preserved through rep movsw
    cld
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256                     ; 512 bytes = 256 words
    rep movsw

    ; Jump to relocated code (labels now match actual addresses)
    jmp 0x0000:relocated

; ============================================================================
; Relocated code (runs from 0x0600, safe from VBR overwrite at 0x7C00)
; ============================================================================

relocated:
    ; Save boot drive (DL preserved through relocation)
    mov [boot_drive], dl

    ; Find bootable partition in partition table
    mov si, partition_table         ; Points to 0x07BE (relocated copy)
    mov cx, 4

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
    ; Save partition entry pointer (for VBR, some need it in SI)
    mov [partition_entry], si

    ; Get partition start LBA (offset 8 in partition entry)
    mov eax, [si + 8]
    mov [dap_lba], eax              ; Store in static DAP

    ; Check if INT 13h extensions are supported (AH=41h)
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .try_chs                     ; Extensions not supported
    cmp bx, 0xAA55
    jne .try_chs                    ; Extensions not supported

    ; LBA read via static DAP (avoids push dword on stack)
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jnc .verify_vbr                 ; LBA succeeded

    ; LBA read failed even though extensions exist - try CHS

.try_chs:
    ; Query BIOS drive geometry (don't trust partition table CHS values)
    push es
    xor di, di
    mov es, di
    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    pop es
    jc .disk_error

    ; Parse geometry: CL[5:0] = max sector, DH = max head
    mov al, cl
    and al, 0x3F                    ; Sectors per track (1-63)
    test al, al
    jz .disk_error                  ; Invalid geometry
    mov [bios_spt], al
    inc dh                          ; Max head -> number of heads
    test dh, dh
    jz .disk_error                  ; Invalid (wrapped from 0xFF)
    mov [bios_heads], dh

    ; Convert partition LBA to CHS using BIOS-reported geometry
    ; (partition table CHS may not match BIOS translation for CF cards)
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
    or cl, ah                       ; CL[7:6] = cylinder high bits

    ; Read VBR (1 sector) to 0x7C00
    mov ah, 0x02
    mov al, 1
    mov bx, 0x7C00
    mov dl, [boot_drive]
    int 0x13
    jc .disk_error

.verify_vbr:
    ; Verify VBR boot signature
    cmp word [0x7C00 + 510], 0xAA55
    jne .invalid_vbr

    ; Jump to VBR, passing drive number in DL and partition entry in SI
    ; boot_drive is safe at 0x0600+N (not overwritten by VBR load)
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
    xor bx, bx                     ; Page 0
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
    cli                             ; Disable interrupts - prevents timer wakeup
    hlt
    jmp halt                        ; Safety: catch NMI

; ============================================================================
; Data (all in relocated region 0x0600-0x07FF, safe from VBR overwrite)
; ============================================================================

boot_drive:      db 0
partition_entry: dw 0
bios_spt:        db 63              ; Default, overwritten by INT 13h/08h
bios_heads:      db 16              ; Default, overwritten by INT 13h/08h

; Static Disk Address Packet for INT 13h extended read
; (avoids push dword which can be problematic on some BIOSes)
dap:
    db 0x10                         ; Packet size (16 bytes)
    db 0                            ; Reserved
    dw 1                            ; Number of sectors to read
    dw 0x7C00                       ; Buffer offset
    dw 0x0000                       ; Buffer segment
dap_lba:
    dd 0                            ; Starting LBA (low 32 bits, filled at runtime)
    dd 0                            ; Starting LBA (high 32 bits)

no_boot_msg:     db 'No boot partition', 13, 10, 0
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
