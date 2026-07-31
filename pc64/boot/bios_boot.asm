; ============================================================================
; bios_boot.asm - pc64 boot sector (legacy BIOS, 16-bit real mode)
;
; The BIOS loads this 512-byte sector to 0x7C00 and jumps here with DL = the
; boot drive. It reads stage2 off the disk with INT 13h AH=42h and runs it.
;
; Disk layout (LBA, 512-byte sectors):
;       0          this sector
;       1 .. 16    stage2          -> loaded to physical 0x8000
;       17 ..      the kernel image -> stage2 loads it to 0x100000
;
; NO CHS FALLBACK, deliberately. Every machine this port targets has EDD (see
; docs/BIOS-BOOT-PLAN.md); a CHS path would be untestable code that is wrong on
; the day it is finally needed.
;
; stage2 is loaded at segment 0 offset 0x8000 and entered as 0:0x8000, so it
; runs with CS=DS=ES=SS=0 and every label is a flat physical address.
;
; Derived from the Writer's Unlock loader (boot/boot.asm), which is the same
; author's and is hardware-proven across five machines.
; ============================================================================
bits 16
cpu 686
org 0x7C00

STAGE2_LBA      equ 1
STAGE2_SECTORS  equ 16
STAGE2_OFF      equ 0x8000      ; physical load offset (segment 0)

start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00          ; stack just below us, grows down
    cld
    sti
    mov     [boot_drive], dl

    mov     si, msg_boot
    call    print

    xor     ah, ah              ; reset the disk system
    mov     dl, [boot_drive]
    int     0x13

    mov     si, dap             ; extended read: stage2 -> 0000:8000
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jc      disk_err

    mov     dl, [boot_drive]    ; stage2 wants the drive in DL
    jmp     0x0000:0x8000

disk_err:
    mov     si, msg_err
    call    print
.hang:
    hlt
    jmp     .hang

; print a NUL-terminated string at DS:SI via BIOS teletype
print:
    push    ax
    push    bx
    mov     ah, 0x0E
    mov     bx, 0x0007
.l:
    lodsb
    test    al, al
    jz      .d
    int     0x10
    jmp     .l
.d:
    pop     bx
    pop     ax
    ret

boot_drive: db 0
msg_boot:   db "UnoDOS pc64", 13, 10, 0
msg_err:    db "disk err", 0

align 4
dap:                            ; INT 13h Disk Address Packet
    db  0x10                    ; size
    db  0
    dw  STAGE2_SECTORS
    dw  STAGE2_OFF              ; buffer offset
    dw  0x0000                  ; buffer segment
    dq  STAGE2_LBA

times 510-($-$$) db 0
dw 0xAA55
