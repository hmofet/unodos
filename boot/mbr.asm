; UnoDOS Master Boot Record (MBR)
; Loaded by BIOS at 0x0000:0x7C00
; Relocates to 0x0000:0x0600, finds bootable partition, loads VBR
;
; CRITICAL: ORG is 0x0600 so all data/code labels are in the relocated
; region. This prevents the VBR load (to 0x7C00) from overwriting MBR
; variables, error strings, and the halt loop.
;
; 8086/8088-clean: the partition LBA is handled as 16-bit word pairs, the
; INT 13h AH=42h (LBA) path needs no 32-bit math, and the CHS fallback converts
; a full 32-bit LBA via two 16-bit DIVs (exact for all 32-bit LBAs).

[BITS 16]
[ORG 0x0600]
cpu 8086            ; Target CPU: Intel 8088/8086 (PC/XT)

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

    ; Get partition start LBA (offset 8 in partition entry) as 16-bit words.
    ; The DAP high dword stays 0 (initialized), covering the LBA28/32 range.
    mov ax, [si + 8]
    mov [dap_lba], ax
    mov ax, [si + 10]
    mov [dap_lba + 2], ax

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

    ; Convert the 32-bit partition LBA to CHS using BIOS-reported geometry.
    ; 8086: a 32-bit / 16-bit division is done as two chained 16-bit DIVs
    ; (high word first, remainder carried into the low word).
    ; --- LBA / SPT -> quotient (32-bit in chs_tmp) + remainder (sector) ---
    xor bx, bx
    mov bl, [bios_spt]              ; BX = sectors per track
    mov ax, [dap_lba + 2]          ; high word
    xor dx, dx
    div bx                          ; AX = hi/SPT, DX = hi%SPT
    mov [chs_tmp + 2], ax          ; quotient high
    mov ax, [dap_lba]              ; low word (DX = carry-in remainder)
    div bx                          ; AX = lo quotient, DX = LBA mod SPT
    mov [chs_tmp], ax              ; quotient low
    inc dx                          ; sector = remainder + 1
    mov cl, dl                      ; CL[5:0] = sector (<=63)

    ; --- quotient / heads -> cylinder (AX) + head (DX) ---
    xor bx, bx
    mov bl, [bios_heads]           ; BX = number of heads
    mov ax, [chs_tmp + 2]          ; quotient high
    xor dx, dx
    div bx                          ; AX = qhi/heads, DX = qhi%heads
    mov ax, [chs_tmp]              ; quotient low (DX = carry-in)
    div bx                          ; AX = cylinder, DX = head
    mov dh, dl                      ; DH = head
    mov ch, al                      ; CH = cylinder low 8 bits
    mov al, ah                      ; AL = cylinder high byte
    and al, 0x03                    ; keep bits 8-9 of the cylinder
    ror al, 1
    ror al, 1                       ; bits 0-1 -> bits 6-7 (== shl al,6 here)
    or cl, al                       ; CL[7:6] = cylinder high bits

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
chs_tmp:         dd 0              ; 32-bit quotient between the two CHS divisions

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
