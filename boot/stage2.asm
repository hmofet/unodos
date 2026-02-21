; UnoDOS Stage 2 Loader
; Loaded at 0x0800:0000 (linear 0x8000)
; Minimal loader that loads kernel and transfers control
; Size: ~2KB (4 sectors)

[BITS 16]
[ORG 0x0000]

; ============================================================================
; Configuration
; ============================================================================

KERNEL_SEGMENT  equ 0x1000          ; Kernel loads at 0x1000:0000 (64KB mark)
KERNEL_SECTORS  equ 74              ; 37KB kernel (expanded for resize support - Build 301)
KERNEL_START    equ 6               ; Kernel starts at sector 6 (after 4-sector stage2 + 1 reserved)
KERNEL_SIG      equ 0x4B55          ; 'UK' signature for kernel

; ============================================================================
; Signature and Entry Point
; ============================================================================

signature:
    dw 0x4E55                       ; 'UN' signature for boot sector verification

entry:
    ; Set up segment registers
    mov ax, 0x0800
    mov ds, ax

    ; Save boot drive (passed from boot sector in DL)
    mov [boot_drive], dl

    ; Print loading message
    mov si, msg_loading
    call print_string

    ; Load kernel from disk with progress indicator
    call load_kernel

    ; Verify kernel signature
    mov ax, KERNEL_SEGMENT
    mov es, ax
    mov ax, [es:0]
    cmp ax, KERNEL_SIG
    jne kernel_error

    ; Progress complete
    mov si, msg_done
    call print_string

    ; Pass boot info to kernel
    ; DL = boot drive
    ; DS:SI = pointer to boot info structure
    mov dl, [boot_drive]

    ; Jump to kernel (past signature)
    jmp KERNEL_SEGMENT:0x0002

; ============================================================================
; Load Kernel with Progress Bar
; ============================================================================

load_kernel:
    pusha

    ; Set up for disk read
    mov ax, KERNEL_SEGMENT
    mov es, ax
    xor bx, bx                      ; Start at offset 0

    mov byte [sectors_left], KERNEL_SECTORS
    mov byte [current_sector], KERNEL_START
    mov byte [current_head], 0
    mov byte [current_cyl], 0

.load_loop:
    ; Read one sector with retry (real floppy drives need this)
    mov byte [retry_count], 3       ; Try 3 times

.retry:
    mov ah, 0x02                    ; BIOS read sectors
    mov al, 1                       ; Read 1 sector at a time
    mov ch, [current_cyl]
    mov cl, [current_sector]
    mov dh, [current_head]
    mov dl, [boot_drive]
    int 0x13
    jnc .read_ok

    ; Read failed - reset drive and retry
    dec byte [retry_count]
    jz .disk_error                  ; All retries exhausted
    xor ah, ah                      ; AH=0 reset disk
    mov dl, [boot_drive]
    int 0x13
    jmp .retry

.read_ok:

    ; Print progress dot
    ; IMPORTANT: Preserve BX since BIOS int 0x10 may modify it
    push bx
    mov ah, 0x0E
    mov al, '.'                     ; ASCII 0x2E
    xor bh, bh
    int 0x10
    pop bx

    ; Advance buffer pointer
    add bx, 512
    jnc .no_segment_wrap
    ; Buffer wrapped, advance segment
    mov ax, es
    add ax, 0x1000                  ; Add 64KB
    mov es, ax
    xor bx, bx
.no_segment_wrap:

    ; Advance to next sector
    inc byte [current_sector]
    cmp byte [current_sector], 19   ; Sectors 1-18 per track
    jb .sector_ok

    ; Move to next head
    mov byte [current_sector], 1
    inc byte [current_head]
    cmp byte [current_head], 2      ; 2 heads
    jb .sector_ok

    ; Move to next cylinder
    mov byte [current_head], 0
    inc byte [current_cyl]

.sector_ok:
    dec byte [sectors_left]
    jnz .load_loop

    popa
    ret

.disk_error:
    mov si, msg_disk_err
    call print_string

    ; Print diagnostic: CHS and error code
    ; AH still has BIOS error code from last int 0x13
    mov si, msg_diag_cyl
    call print_string
    mov al, [current_cyl]
    call print_hex_byte
    mov si, msg_diag_head
    call print_string
    mov al, [current_head]
    call print_hex_byte
    mov si, msg_diag_sec
    call print_string
    mov al, [current_sector]
    call print_hex_byte
    mov si, msg_diag_left
    call print_string
    mov al, [sectors_left]
    call print_hex_byte
    mov si, msg_crlf
    call print_string

    jmp halt

; ============================================================================
; Error Handlers
; ============================================================================

kernel_error:
    mov si, msg_kern_err
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
; Print String (BIOS teletype)
; ============================================================================

print_string:
    push ax
    push bx
    push si
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    jmp .loop
.done:
    pop si
    pop bx
    pop ax
    ret

; ============================================================================
; Print Hex Byte (AL = value to print)
; ============================================================================

print_hex_byte:
    push ax
    push bx
    push cx
    mov cl, al                      ; Save value
    ; High nibble
    shr al, 4
    call .print_nibble
    ; Low nibble
    mov al, cl
    and al, 0x0F
    call .print_nibble
    pop cx
    pop bx
    pop ax
    ret
.print_nibble:
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .print_it
.digit:
    add al, '0'
.print_it:
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    ret

; ============================================================================
; Data
; ============================================================================

boot_drive:     db 0
sectors_left:   db 0
current_sector: db 0
current_head:   db 0
current_cyl:    db 0
retry_count:    db 0

msg_loading:    db 'Loading kernel', 0
msg_done:       db ' OK', 0x0D, 0x0A, 0
msg_disk_err:   db 0x0D, 0x0A, 'Disk error!', 0x0D, 0x0A, 0
msg_kern_err:   db 0x0D, 0x0A, 'Bad kernel!', 0x0D, 0x0A, 0
msg_halt:       db 'Halted.', 0x0D, 0x0A, 0
msg_diag_cyl:   db 'C:', 0
msg_diag_head:  db ' H:', 0
msg_diag_sec:   db ' S:', 0
msg_diag_left:  db ' Left:', 0
msg_crlf:       db 0x0D, 0x0A, 0

; ============================================================================
; Padding
; ============================================================================

; Pad to 2KB (4 sectors) - minimal loader
times 2048 - ($ - $$) db 0
