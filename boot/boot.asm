; UnoDOS Boot Sector
; 512-byte boot sector for IBM PC XT compatible BIOS
; Loads second stage loader from floppy and transfers control

[BITS 16]
[ORG 0x7C00]

; ============================================================================
; BPB - BIOS Parameter Block (required for correct floppy geometry)
; Many BIOSes read sectors/track and heads from here for INT 0x13
; ============================================================================

    jmp short start         ; 2-byte jump (offset 0)
    nop                     ; 1-byte NOP  (offset 2)

bpb_oem:        db 'UNODOS  '  ; offset  3: OEM name (8 bytes)
bpb_bps:        dw 512          ; offset 11: bytes per sector
bpb_spc:        db 1            ; offset 13: sectors per cluster
bpb_rsvd:       dw 86           ; offset 14: reserved sectors (OS area, expanded Build 311)
bpb_fats:       db 2            ; offset 16: number of FATs
bpb_rootent:    dw 224          ; offset 17: root directory entries
bpb_sectors:    dw 2880         ; offset 19: total sectors (1.44MB)
bpb_media:      db 0xF0         ; offset 21: media descriptor (1.44MB floppy)
bpb_fpf:        dw 9            ; offset 22: sectors per FAT
bpb_spt:        dw 18           ; offset 24: sectors per track
bpb_heads:      dw 2            ; offset 26: number of heads
bpb_hidden:     dd 0            ; offset 28: hidden sectors
bpb_total32:    dd 0            ; offset 32: total sectors (32-bit, 0 = use 16-bit)
bpb_drive:      db 0            ; offset 36: drive number
bpb_rsv:        db 0            ; offset 37: reserved
bpb_bootsig:    db 0x29         ; offset 38: extended boot signature
bpb_volid:      dd 0x554E4F53  ; offset 39: volume serial ('UNOS')
bpb_vollabel:   db 'UNODOS     '; offset 43: volume label (11 bytes)
bpb_fstype:     db 'FAT12   '  ; offset 54: filesystem type (8 bytes)

; ============================================================================
; Boot Sector Entry Point (offset 62)
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

msg_boot:       db 'UnoDOS Boot v3.18', 0x0D, 0x0A, 0
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
