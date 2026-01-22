; UnoDOS Boot Sector
; 512-byte boot sector for IBM PC XT compatible BIOS
; Loads second stage loader from floppy and transfers control

[BITS 16]
[ORG 0x7C00]

; ============================================================================
; Boot Sector Entry Point
; ============================================================================

start:
    ; Set up segment registers
    cli                     ; Disable interrupts during setup
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00          ; Stack grows down from boot sector
    sti                     ; Re-enable interrupts

    ; Save boot drive number (BIOS passes it in DL)
    mov [boot_drive], dl

    ; Print boot message
    mov si, msg_boot
    call print_string

    ; Reset floppy disk system
    mov si, msg_reset
    call print_string

    xor ax, ax
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    mov si, msg_ok
    call print_string

    ; Load second stage loader
    ; Load 4 sectors (2KB) starting from sector 2 to 0x0800:0000 (0x8000)
    mov si, msg_loading
    call print_string

    mov ax, 0x0800          ; Segment to load to (0x0800:0000 = 0x8000)
    mov es, ax
    xor bx, bx              ; Offset 0

    mov ah, 0x02            ; BIOS read sectors function
    mov al, 4               ; Number of sectors to read (2KB)
    mov ch, 0               ; Cylinder 0
    mov cl, 2               ; Start from sector 2 (sector 1 is boot sector)
    mov dh, 0               ; Head 0
    mov dl, [boot_drive]    ; Drive number
    int 0x13
    jc disk_error

    mov si, msg_ok
    call print_string

    ; Verify signature at start of second stage
    mov ax, 0x0800
    mov es, ax
    mov ax, [es:0]
    cmp ax, 0x4E55          ; 'UN' signature (little-endian)
    jne sig_error

    mov si, msg_jump
    call print_string

    ; Jump to second stage loader
    jmp 0x0800:0x0002       ; Jump past signature to code

; ============================================================================
; Error Handlers
; ============================================================================

disk_error:
    mov si, msg_disk_err
    call print_string
    jmp halt

sig_error:
    mov si, msg_sig_err
    call print_string
    jmp halt

halt:
    mov si, msg_halt
    call print_string
.loop:
    cli
    hlt
    jmp .loop

; ============================================================================
; Subroutines
; ============================================================================

; Print null-terminated string
; Input: SI = pointer to string
print_string:
    push ax
    push bx
    mov ah, 0x0E            ; BIOS teletype function
    mov bh, 0               ; Page 0
.loop:
    lodsb                   ; Load byte from SI into AL
    test al, al             ; Check for null terminator
    jz .done
    int 0x10                ; Print character
    jmp .loop
.done:
    pop bx
    pop ax
    ret

; ============================================================================
; Data
; ============================================================================

boot_drive:     db 0

msg_boot:       db 'UnoDOS Boot v0.2', 0x0D, 0x0A, 0
msg_reset:      db 'Reset disk... ', 0
msg_loading:    db 'Load stage2... ', 0
msg_ok:         db 'OK', 0x0D, 0x0A, 0
msg_jump:       db 'Jump to stage2', 0x0D, 0x0A, 0
msg_disk_err:   db 'DISK ERROR!', 0x0D, 0x0A, 0
msg_sig_err:    db 'BAD SIGNATURE!', 0x0D, 0x0A, 0
msg_halt:       db 'System halted.', 0x0D, 0x0A, 0

; ============================================================================
; Boot Sector Padding and Signature
; ============================================================================

times 510 - ($ - $$) db 0   ; Pad to 510 bytes
dw 0xAA55                   ; Boot signature
