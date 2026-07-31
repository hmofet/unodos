; ============================================================================
; bios_stage2.asm - pc64 second-stage loader: real mode -> LONG mode
;
; Loaded by bios_boot.asm at physical 0x8000, entered with CS=0, IP=0x8000,
; DL=drive. ORG is 0x8000 and we run with DS=ES=SS=0, so every label is a flat
; physical address and segment juggling disappears.
;
; Everything here is work that NEEDS the BIOS or real mode, because none of it
; is reachable afterwards:
;   1. check the CPU can run long mode AT ALL, and refuse readably if not
;   2. enable A20
;   3. load the kernel image to physical 0x100000
;   4. set a 32bpp linear-framebuffer video mode via VBE
;   5. capture the E820 memory map
;   6. fill the bootinfo block
;   7. build page tables, enter long mode, jump to the kernel
;
; STEP 4 IS ONE-WAY AND THAT GOVERNS THE WHOLE DESIGN. Mode setting is INT 10h,
; a real-mode call, and long mode has no v8086 - after step 7 the video BIOS is
; unreachable short of leaving long mode or emulating an x86. The mode chosen
; here is the mode for the life of the boot. See docs/BIOS-BOOT-PLAN.md.
;
; The kernel is read in <=32 KB chunks into a low staging buffer and copied
; above 1 MB with INT 15h AH=87h (Big Memory Move), which does the mode
; transition for us and so avoids needing unreal mode at all.
;
; Fixed physical addresses (kept in sync with pc64/bootinfo.h):
;   0x01000  bootinfo block
;   0x02000  E820 entry array
;   0x03000  VBE controller info / 0x03200 VBE mode info
;   0x08000  this loader
;   0x10000  kernel staging buffer (boot only; free afterwards)
;   0x20000  page tables (6 pages: PML4, PDPT, 4x PD)
;   0x90000  stack top on entry to the kernel
;   0x100000 the kernel, where it runs
;
; Derived from the Writer's Unlock loader (boot/stage2.asm), same author,
; hardware-proven on five machines. The real-mode half is substantially his;
; the long-mode transition and the CPUID gate are new here, because that loader
; targets a 32-bit kernel.
; ============================================================================
bits 16
cpu 686
org 0x8000

BOOTINFO    equ 0x1000
E820BUF     equ 0x2000
VBECTRL     equ 0x3000
VBEMODE     equ 0x3200
PML4        equ 0x20000
PDPT        equ 0x21000
PD0         equ 0x22000         ; four PDs: 0x22000..0x25000, 4 GiB identity
KERNEL_LBA  equ 17
STAGE_SEG   equ 0x1000          ; staging buffer 0x10000 as a real-mode segment
STAGE_LIN   equ 0x10000         ; ... as a linear address (INT15/87 source base)
KERNEL_LIN  equ 0x100000        ; final kernel base (1 MB)
STACK_TOP   equ 0x90000
MAX_W       equ 1920
MAX_H       equ 1200

stage2_start:
    mov     [drive], dl
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    cld

    mov     si, s_hi
    call    rprint

    call    check_long          ; FIRST: refuse unusable CPUs while we can talk
    call    enable_a20          ; before any access above 1 MB
    call    load_kernel
    call    do_e820
    call    do_vbe              ; ONE-WAY, and the last thing that needs the BIOS
    call    fill_bootinfo
    jmp     go_long

; --------------------------------------------------------------------------
; check_long: does this CPU have long mode?
;
; This runs before ANY page table is touched, and it prints, because the
; machines that fail it are exactly the ones the plan admits are out of scope:
; pre-Prescott Pentium 4 (no EM64T) and Core Solo/Duo (Yonah, 32-bit). A user
; on one of those must be told; a triple fault would tell them nothing, and
; "the machine reboots instantly" is the single least diagnosable symptom a
; bootloader can produce.
; --------------------------------------------------------------------------
check_long:
    mov     eax, 0x80000000
    cpuid
    cmp     eax, 0x80000001     ; is the extended leaf even implemented?
    jb      .no
    mov     eax, 0x80000001
    cpuid
    test    edx, 1 << 29        ; LM
    jz      .no
    ret
.no:
    mov     si, s_nolong
    call    rprint
    jmp     halt16

; --------------------------------------------------------------------------
; load_kernel: read [kern_count] sectors from LBA KERNEL_LBA into the low
; staging buffer in <=64-sector (32 KB) chunks; copy each chunk above 1 MB with
; INT 15h AH=87h. r_* are memory vars because the BIOS calls clobber registers.
; --------------------------------------------------------------------------
load_kernel:
    mov     dword [r_lba], KERNEL_LBA
    mov     eax, [kern_count]
    mov     [r_rem], eax
    mov     dword [r_dest], KERNEL_LIN
.next:
    mov     eax, [r_rem]
    test    eax, eax
    jz      .done
    cmp     eax, 64
    jbe     .have
    mov     eax, 64
.have:
    mov     [r_sec], eax                ; sectors this chunk

    ; --- INT 13h AH=42h: read chunk into the staging buffer --------------
    mov     cx, ax
    mov     word [kdap+2], cx
    mov     word [kdap+4], 0
    mov     word [kdap+6], STAGE_SEG
    mov     eax, [r_lba]
    mov     dword [kdap+8], eax
    mov     dword [kdap+12], 0
    mov     si, kdap
    mov     ah, 0x42
    mov     dl, [drive]
    int     0x13
    jc      .err

    ; --- INT 15h AH=87h: copy the chunk to [r_dest] ----------------------
    mov     eax, [r_sec]
    shl     eax, 9                      ; bytes = sectors * 512
    dec     eax                         ; descriptor limit = bytes - 1
    mov     [gmm_src], ax
    mov     [gmm_dst], ax
    mov     word [gmm_src+2], (STAGE_LIN & 0xFFFF)
    mov     byte [gmm_src+4], (STAGE_LIN >> 16) & 0xFF
    mov     eax, [r_dest]
    mov     [gmm_dst+2], ax
    shr     eax, 16
    mov     [gmm_dst+4], al
    mov     eax, [r_sec]
    shl     eax, 8                      ; CX = words = sectors * 256
    mov     cx, ax
    mov     si, gmm_table
    mov     ah, 0x87
    int     0x15
    jc      .err
    sti                                 ; 87h returns with interrupts off

    mov     eax, [r_sec]
    add     [r_lba], eax
    sub     [r_rem], eax
    shl     eax, 9
    add     [r_dest], eax
    mov     al, '.'
    call    rputc
    jmp     .next
.done:
    ret
.err:
    mov     si, s_derr
    call    rprint
    jmp     halt16

; --------------------------------------------------------------------------
; do_e820: capture the BIOS memory map into E820BUF (24-byte entries).
; --------------------------------------------------------------------------
do_e820:
    mov     di, E820BUF
    xor     ebx, ebx
.loop:
    mov     eax, 0xE820
    mov     edx, 0x534D4150             ; 'SMAP'
    mov     ecx, 24
    mov     dword [di+20], 1            ; ask for a valid ACPI 3.0 attr field
    int     0x15
    jc      .done
    cmp     eax, 0x534D4150
    jne     .done
    add     di, 24
    test    ebx, ebx
    jz      .done
    cmp     di, E820BUF + 24*64
    jb      .loop
.done:
    mov     ax, di
    sub     ax, E820BUF
    xor     dx, dx
    mov     bx, 24
    div     bx                          ; AX = entry count
    movzx   eax, ax
    mov     [mmap_count], eax
    ret

; --------------------------------------------------------------------------
; do_vbe: find the largest 32bpp linear-framebuffer mode within MAX_W x MAX_H
; and set it, recording the framebuffer parameters.
;
; PhysBasePtr is a 32-bit VBE field, which is why identity-mapping the low
; 4 GiB below is sufficient BY CONSTRUCTION for the framebuffer: a VBE LFB
; cannot live above 4 GiB.
; --------------------------------------------------------------------------
do_vbe:
    mov     di, VBECTRL
    mov     dword [di], 0x32454256      ; 'VBE2' - request VBE 2+ info
    mov     ax, 0x4F00
    int     0x10
    cmp     ax, 0x004F
    jne     .fail

    mov     ax, [VBECTRL+0x10]          ; VideoModePtr segment
    mov     [modeseg], ax
    mov     ax, [VBECTRL+0x0E]          ; VideoModePtr offset
    mov     [modeoff], ax
    mov     dword [best_area], 0
    mov     word [best_mode], 0xFFFF

.iter:
    mov     fs, [modeseg]
    mov     bx, [modeoff]
    mov     dx, [fs:bx]                 ; next mode number
    cmp     dx, 0xFFFF
    je      .choose
    add     word [modeoff], 2
    mov     [cur_mode], dx

    mov     cx, dx
    mov     di, VBEMODE
    mov     ax, 0x4F01
    int     0x10
    cmp     ax, 0x004F
    jne     .iter

    mov     ax, [VBEMODE+0x00]          ; ModeAttributes
    test    ax, 0x0001                  ; supported?
    jz      .iter
    test    ax, 0x0080                  ; linear framebuffer available?
    jz      .iter
    cmp     byte [VBEMODE+0x19], 32     ; bits per pixel
    jne     .iter

    movzx   eax, word [VBEMODE+0x12]    ; XResolution
    cmp     eax, MAX_W
    ja      .iter
    movzx   ecx, word [VBEMODE+0x14]    ; YResolution
    cmp     ecx, MAX_H
    ja      .iter
    mul     ecx                         ; EAX = width * height
    cmp     eax, [best_area]
    jbe     .iter
    mov     [best_area], eax
    mov     ax, [cur_mode]
    mov     [best_mode], ax
    jmp     .iter

.choose:
    mov     ax, [best_mode]
    cmp     ax, 0xFFFF
    je      .fail
    mov     cx, ax                      ; re-read info for the chosen mode
    mov     di, VBEMODE
    mov     ax, 0x4F01
    int     0x10
    mov     bx, [best_mode]
    or      bx, 0x4000                  ; request the linear framebuffer
    mov     ax, 0x4F02
    int     0x10
    cmp     ax, 0x004F
    jne     .fail

    mov     ax, [VBEMODE+0x12]
    mov     [fb_w], ax
    mov     ax, [VBEMODE+0x14]
    mov     [fb_h], ax
    movzx   ax, byte [VBEMODE+0x19]
    mov     [fb_bpp], ax
    mov     ax, [VBEMODE+0x10]          ; BytesPerScanLine
    mov     [fb_pitch], ax
    mov     eax, [VBEMODE+0x28]         ; PhysBasePtr
    mov     [fb_addr], eax
    ret
.fail:
    mov     si, s_vbe
    call    rprint
    jmp     halt16

; --------------------------------------------------------------------------
; fill_bootinfo: see pc64/bootinfo.h. The magic and size come FIRST so a kernel
; can reject a stale loader instead of reading a wild pointer.
; --------------------------------------------------------------------------
fill_bootinfo:
    mov     dword [BOOTINFO+0], 0x36364F42      ; 'BO66'
    mov     dword [BOOTINFO+4], 48              ; sizeof(struct), version gate
    mov     eax, [fb_addr]
    mov     [BOOTINFO+8], eax
    movzx   eax, word [fb_pitch]
    mov     [BOOTINFO+12], eax
    movzx   eax, word [fb_w]
    mov     [BOOTINFO+16], eax
    movzx   eax, word [fb_h]
    mov     [BOOTINFO+20], eax
    movzx   eax, word [fb_bpp]
    mov     [BOOTINFO+24], eax
    mov     eax, [mmap_count]
    mov     [BOOTINFO+28], eax
    mov     dword [BOOTINFO+32], E820BUF
    mov     eax, [kern_count]
    mov     [BOOTINFO+36], eax
    movzx   eax, byte [drive]
    mov     [BOOTINFO+40], eax
    mov     dword [BOOTINFO+44], KERNEL_LIN
    ret

enable_a20:
    in      al, 0x92
    test    al, 2
    jnz     .ok
    or      al, 2
    and     al, 0xFE                    ; never write the reset bit
    out     0x92, al
.ok:
    ret

; --- tiny real-mode console helpers ---------------------------------------
rprint:
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
rputc:
    push    ax
    push    bx
    mov     ah, 0x0E
    mov     bx, 0x0007
    int     0x10
    pop     bx
    pop     ax
    ret
halt16:
    cli
    hlt
    jmp     halt16

; ==========================================================================
; go_long: build page tables, enter long mode, hand off.
;
; Identity-maps the low 4 GiB with 2 MiB pages: PML4[0] -> PDPT, PDPT[0..3] ->
; four PDs, each PD filling 512 entries of 2 MiB. Six pages of tables at
; 0x20000, in conventional memory the kernel never reclaims (its heap is a
; .bss array above 1 MiB), because NOTHING REPLACES THESE LATER - pc64 never
; writes CR3, so the mapping this loader installs is the mapping the OS runs on
; for its whole life.
;
; 4 GiB is enough by construction for the framebuffer (VBE PhysBasePtr is a
; 32-bit field). A device whose MMIO the firmware placed above 4 GiB would need
; more, which is phase C's problem and not reachable from here.
; ==========================================================================
go_long:
    cli

    ; --- zero the six table pages ----------------------------------------
    mov     edi, PML4
    mov     ecx, 6 * 4096 / 4
    xor     eax, eax
.zero:
    mov     [dword edi], eax            ; a20-safe: 32-bit addressing in rmode
    add     edi, 4
    dec     ecx
    jnz     .zero

    ; --- PML4[0] -> PDPT --------------------------------------------------
    mov     dword [dword PML4], PDPT | 3            ; present | rw
    mov     dword [dword PML4 + 4], 0

    ; --- PDPT[0..3] -> PD0..PD3 ------------------------------------------
    mov     edi, PDPT
    mov     eax, PD0 | 3
    mov     ecx, 4
.pdpt:
    mov     [dword edi], eax
    mov     dword [dword edi + 4], 0
    add     eax, 4096
    add     edi, 8
    dec     ecx
    jnz     .pdpt

    ; --- the PDs: 2048 entries of 2 MiB, identity ------------------------
    mov     edi, PD0
    xor     eax, eax                    ; physical address, low 32 bits
    xor     edx, edx                    ; ... high 32 bits
    mov     ecx, 2048
.pd:
    mov     ebx, eax
    or      ebx, 0x83                   ; present | rw | PS (2 MiB page)
    mov     [dword edi], ebx
    mov     [dword edi + 4], edx
    add     eax, 0x200000
    adc     edx, 0
    add     edi, 8
    dec     ecx
    jnz     .pd

    ; --- CR4.PAE, CR3, EFER.LME, CR0.PG ----------------------------------
    mov     eax, cr4
    or      eax, 1 << 5                 ; PAE
    mov     cr4, eax

    mov     eax, PML4
    mov     cr3, eax

    mov     ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or      eax, 1 << 8                 ; LME
    wrmsr

    lgdt    [gdt64_desc]

    mov     eax, cr0
    or      eax, 0x80000001             ; PG | PE - paging and protection together
    mov     cr0, eax

    ; ptr16:32 explicitly: this far jump is encoded by 16-bit code but lands
    ; in a 64-bit segment, and the 16-bit form would zero-extend a 16-bit
    ; offset - correct only by accident of where this loader sits.
    jmp     dword 0x08:long_entry

; --------------------------------------------------------------------------
cpu x64
bits 64
long_entry:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax
    mov     rsp, STACK_TOP

    ; --- ENABLE SSE. NOT OPTIONAL. ---------------------------------------
    ; x86-64 mandates SSE2, so a compiler emits it wherever it likes - and a
    ; 64-bit CPU comes out of reset with CR0.EM set and CR4.OSFXSR clear, which
    ; makes every one of those instructions a #UD. UEFI firmware does this for
    ; us, which is exactly why it is easy to forget here: the kernel that runs
    ; perfectly when the firmware loads it dies in its own prologue when this
    ; loader does.
    mov     rax, cr0
    and     ax, 0xFFFB                  ; CR0.EM = 0 (no FPU emulation)
    or      ax, 0x0002                  ; CR0.MP = 1 (monitor coprocessor)
    mov     cr0, rax
    mov     rax, cr4
    or      ax, 3 << 9                  ; CR4.OSFXSR | CR4.OSXMMEXCPT
    mov     cr4, rax

    ; The kernel is built by the mingw toolchain and so uses the Microsoft x64
    ; ABI (first argument in RCX). RDI is set to the same value so the entry
    ; also works if it is ever built SysV - one register costs nothing and
    ; removes a whole class of "it boots on one toolchain" bug.
    mov     rcx, BOOTINFO
    mov     rdi, rcx
    xor     rbp, rbp
    mov     eax, dword [kernel_entry]   ; KERNEL_LIN + entry RVA, patched in
    jmp     rax

bits 16
cpu 686
; --- patchable parameters; tools/mkbios.py finds patch_magic --------------
align 4
patch_magic:    dd 0x50424F42           ; 'BOBP'
kern_count:     dd 0                    ; sectors of kernel image
kernel_entry:   dd 0                    ; absolute entry address of the kernel

; --- scratch variables ----------------------------------------------------
drive:          db 0
modeseg:        dw 0
modeoff:        dw 0
cur_mode:       dw 0
best_mode:      dw 0xFFFF
best_area:      dd 0
fb_addr:        dd 0
fb_pitch:       dw 0
fb_w:           dw 0
fb_h:           dw 0
fb_bpp:         dw 0
mmap_count:     dd 0
r_lba:          dd 0
r_rem:          dd 0
r_dest:         dd 0
r_sec:          dd 0

align 4
kdap:
    db  0x10
    db  0
    dw  0           ; +2  count
    dw  0           ; +4  offset
    dw  0           ; +6  segment
    dd  0           ; +8  LBA low
    dd  0           ; +12 LBA high

; INT 15h AH=87h Big-Memory-Move descriptor table (6 x 8-byte GDT entries).
; Entries 0/1 and 4/5 are the BIOS's; we fill source (2) and dest (3).
align 4
gmm_table:
    times 16 db 0                  ; 0x00 null, 0x08 GDT (BIOS use)
gmm_src:                           ; 0x10 source
    dw 0
    dw 0
    db 0
    db 0x93
    db 0
    db 0
gmm_dst:                           ; 0x18 destination
    dw 0
    dw 0
    db 0
    db 0x93
    db 0
    db 0
    times 16 db 0                  ; 0x20 BIOS code, 0x28 stack (BIOS fills)

s_hi:     db "pc64 stage2", 13, 10, 0
s_derr:   db 13, 10, "kernel read error", 13, 10, 0
s_vbe:    db 13, 10, "no VBE linear-framebuffer mode", 13, 10, 0
s_nolong: db 13, 10, "This CPU has no long mode (no EM64T/x86-64).", 13, 10
          db "UnoDOS pc64 is 64-bit only and cannot run here.", 13, 10, 0

; --- 64-bit GDT -----------------------------------------------------------
align 8
gdt64_start:
    dq  0x0000000000000000              ; null
    dq  0x00AF9A000000FFFF              ; 0x08 code: L=1, 64-bit
    dq  0x00CF92000000FFFF              ; 0x10 data
gdt64_end:
gdt64_desc:
    dw  gdt64_end - gdt64_start - 1
    dd  gdt64_start                     ; flat: the label IS the linear address
