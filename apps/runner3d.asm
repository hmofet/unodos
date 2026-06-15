; ============================================================================
; RUN3D.BIN - "UnoDOS Runner" 3D, a native UnoDOS application.
;
; A 3D obstacle-dodger that runs ON UnoDOS (not DOS): it talks only to the
; UnoDOS kernel via INT 0x80 - set VGA mode, draw, read events, restore. The
; same game ships on PS2/Dreamcast via the uno3d library; this is the native
; UnoDOS-x86 version.
;
; Real perspective projection (sx = cx + x*FOCAL/z). The camera never rotates
; and walls face the camera, so each wall block projects to an axis-aligned
; rectangle - the whole solid 3D corridor is drawn with the kernel's filled-rect
; API in painter's order (far->near). 16-bit fixed point (positions x16), no FPU;
; assembles for the 8088 so it runs on every UnoDOS machine (snappy on a 386+).
; ============================================================================
cpu 8086
%include "kernel/cpu8086.inc"

; --- BIN header (80 bytes) ---
    db 0xEB, 0x4E                    ; jmp short entry (offset 0x50)
    db 'UI'                          ; magic
    db '3D Runner', 0               ; name (padded to 12)
    times (0x04 + 12) - ($ - $$) db 0
    ; 16x16 icon, 2bpp CGA (4 bytes/row)
    db 0xFF,0xFF,0xFF,0xFF
    db 0xC0,0x00,0x00,0x03
    db 0xC3,0xFC,0x3F,0x03
    db 0xC3,0x0C,0x30,0x03
    db 0xC3,0x0C,0x30,0x03
    db 0xC3,0xFC,0x3F,0x03
    db 0xC0,0x00,0x00,0x03
    db 0xC0,0xF0,0x0F,0x03
    db 0xC0,0x00,0x00,0x03
    db 0xC3,0xC0,0x03,0xC3
    db 0xC0,0x00,0x00,0x03
    db 0xC0,0x05,0x50,0x03
    db 0xC0,0x15,0x54,0x03
    db 0xC0,0x15,0x54,0x03
    db 0xC0,0x00,0x00,0x03
    db 0xFF,0xFF,0xFF,0xFF
    times 0x50 - ($ - $$) db 0

; --- API constants ---
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_APP_YIELD           equ 34
API_GET_TICK            equ 63
API_FILLED_RECT_COLOR   equ 67
API_SET_VIDEO_MODE      equ 95
API_GET_VIDEO_MODE      equ 100
API_MOUSE_SET_VISIBLE   equ 101
EVENT_KEY_PRESS         equ 1

; --- world / projection constants ---
FOCAL   equ 160
CXC     equ 160
CYC     equ 100
NWALLS  equ 6
SLOTS   equ 9
GAPW    equ 2
HALFBW  equ 14                       ; block half-width (.4)
WTOP    equ 44                       ; block top y (.4)
WBOT    equ -44
PZ      equ 48                       ; player plane z (.4)
SPAWNZ  equ 768
RECYZ   equ 24
PMAX    equ (8*16-8)                 ; player_x clamp

; pick wall colour 1..5 from z in AX (smaller z = brighter); result in AL
%macro ZCOLOR 0
    mov cl, 7
    shr ax, cl                       ; z >> 7  (0..6)
    mov bl, al
    mov al, 5
    sub al, bl
    cmp al, 1
    jge %%lo
    mov al, 1
%%lo:
    cmp al, 5
    jle %%hi
    mov al, 5
%%hi:
%endmacro

; ============================================================================
entry:
    PUSHA86
    push ds
    push es
    mov ax, cs
    mov ds, ax

    mov ah, API_GET_VIDEO_MODE
    int 0x80
    mov [cs:saved_mode], al

    mov al, 0x13
    mov ah, API_SET_VIDEO_MODE
    int 0x80

    mov al, 0
    mov ah, API_MOUSE_SET_VISIBLE
    int 0x80

    xor bx, bx
    xor cx, cx
    mov dx, 320
    mov si, 200
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x04
    mov ah, API_WIN_CREATE
    int 0x80
    mov [cs:win_handle], al

    call set_palette
    mov ah, API_GET_TICK
    int 0x80
    mov [cs:rng], ax
    call reset_run

.loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov byte [cs:in_left], 0
    mov byte [cs:in_right], 0
    mov byte [cs:in_fire], 0
.drain:
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_KEY_PRESS
    jne .drain
    cmp dl, 27
    je .quit
    cmp dl, 130
    je .kl
    cmp dl, 131
    je .kr
    cmp dl, ' '
    je .kf
    jmp .drain
.kl:
    mov byte [cs:in_left], 1
    jmp .drain
.kr:
    mov byte [cs:in_right], 1
    jmp .drain
.kf:
    mov byte [cs:in_fire], 1
    jmp .drain
.no_event:

    call update
    call render
    jmp .loop

.quit:
    mov al, [cs:win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80
    mov al, [cs:saved_mode]
    mov ah, API_SET_VIDEO_MODE
    int 0x80
    mov al, 1
    mov ah, API_MOUSE_SET_VISIBLE
    int 0x80
    pop es
    pop ds
    POPA86
    retf

; rnd_mod: BX = modulus, returns AX = 0..BX-1
rnd_mod:
    mov ax, [cs:rng]
    mov dx, 25173
    mul dx
    add ax, 13849
    mov [cs:rng], ax
    xor dx, dx
    div bx
    mov ax, dx
    ret

; new_gap: AX = gap within +/-2 of last_gap, clamped; updates last_gap
new_gap:
    mov bx, 5
    call rnd_mod
    sub ax, 2
    add ax, [cs:last_gap]
    cmp ax, 0
    jge .lo
    xor ax, ax
.lo:
    cmp ax, SLOTS-GAPW
    jle .hi
    mov ax, SLOTS-GAPW
.hi:
    mov [cs:last_gap], ax
    ret

reset_run:
    mov word [cs:player_x], 0
    mov word [cs:speed], 7
    mov word [cs:score], 0
    mov byte [cs:dead], 0
    mov byte [cs:attract], 1
    mov byte [cs:flash], 0
    mov word [cs:last_gap], (SLOTS-GAPW)/2
    xor si, si
    mov cx, NWALLS
.rs:
    push cx
    mov ax, NWALLS
    sub ax, cx
    inc ax                           ; i+1
    mov dx, SPAWNZ/NWALLS
    mul dx                           ; AX = (i+1)*step
    mov bx, si
    mov [cs:wall_z + bx], ax
    call new_gap
    mov bx, si
    mov [cs:wall_gap + bx], ax
    add si, 2
    pop cx
    loop .rs
    ret

; ai_target: AX = gap centre x (.4) of nearest wall ahead of the player
ai_target:
    xor si, si
    mov cx, NWALLS
    mov di, 0xFFFF                    ; best index*2 (none)
    mov bp, 0x7FFF                    ; smallest z that is >= PZ
.at:
    push cx
    mov bx, si
    mov ax, [cs:wall_z + bx]
    cmp ax, PZ
    jl .skip
    cmp ax, bp
    jg .skip
    mov bp, ax
    mov di, si
.skip:
    add si, 2
    pop cx
    loop .at
    cmp di, 0xFFFF
    jne .have
    mov ax, [cs:player_x]
    ret
.have:
    mov bx, di
    mov ax, [cs:wall_gap + bx]
    shl ax, 1                         ; 2*gap
    add ax, GAPW-1
    sub ax, 8
    mov cx, 16
    imul cx                          ; *16 -> .4
    ret

update:
    cmp byte [cs:flash], 0
    je .nf
    dec byte [cs:flash]
.nf:
    cmp byte [cs:dead], 0
    je .alive
    cmp byte [cs:in_fire], 0
    je .done
    call reset_run
    jmp .done
.alive:
    mov al, [cs:in_left]
    or  al, [cs:in_right]
    jz .nin
    mov byte [cs:attract], 0
.nin:
    cmp byte [cs:attract], 0
    je .manual
    call ai_target
    mov bx, [cs:player_x]
    mov cx, ax
    add cx, 32
    cmp bx, cx
    jg .aleft
    mov cx, ax
    sub cx, 32
    cmp bx, cx
    jl .aright
    jmp .clamp
.aleft:
    sub word [cs:player_x], 7
    jmp .clamp
.aright:
    add word [cs:player_x], 7
    jmp .clamp
.manual:
    cmp byte [cs:in_left], 0
    je .m1
    sub word [cs:player_x], 7
.m1:
    cmp byte [cs:in_right], 0
    je .clamp
    add word [cs:player_x], 7
.clamp:
    mov ax, [cs:player_x]
    cmp ax, PMAX
    jle .c1
    mov ax, PMAX
.c1:
    cmp ax, -PMAX
    jge .c2
    mov ax, -PMAX
.c2:
    mov [cs:player_x], ax

    xor si, si
    mov cx, NWALLS
.uw:
    push cx
    mov bx, si
    mov ax, [cs:wall_z + bx]
    mov dx, ax                       ; prev z
    sub ax, [cs:speed]
    mov [cs:wall_z + bx], ax
    cmp dx, PZ
    jl .nocross
    cmp ax, PZ
    jge .nocross
    call check_pass                  ; BX = wall index*2
.nocross:
    mov bx, si
    mov ax, [cs:wall_z + bx]
    cmp ax, RECYZ
    jge .norec
    mov ax, SPAWNZ
    mov [cs:wall_z + bx], ax
    call new_gap
    mov bx, si
    mov [cs:wall_gap + bx], ax
.norec:
    add si, 2
    pop cx
    loop .uw
.done:
    ret

; check_pass: BX = wall index*2
check_pass:
    push si
    push di
    mov ax, [cs:wall_gap + bx]
    ; gap left (.4) = (2*gap - 8)*16 - 16
    mov si, ax
    shl ax, 1
    sub ax, 8
    mov cx, 16
    imul cx
    sub ax, 16
    mov di, ax                       ; di = gap left
    ; gap right = (2*(gap+GAPW-1) - 8)*16 + 16
    mov ax, si
    add ax, GAPW-1
    shl ax, 1
    sub ax, 8
    mov cx, 16
    imul cx
    add ax, 16                       ; ax = gap right
    mov cx, [cs:player_x]
    cmp cx, di
    jle .crash
    cmp cx, ax
    jge .crash
    inc word [cs:score]
    add word [cs:speed], 1
    cmp word [cs:speed], 13
    jle .ok
    mov word [cs:speed], 13
.ok:
    pop di
    pop si
    ret
.crash:
    mov byte [cs:dead], 1
    mov byte [cs:flash], 8
    pop di
    pop si
    ret

; ============================================================================
render:
    ; clear background
    xor bx, bx
    xor cx, cx
    mov dx, 320
    mov si, 200
    mov al, 0
    cmp byte [cs:flash], 0
    je .bg
    mov al, 7
.bg:
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; reset drawn flags
    mov cx, NWALLS
    mov di, 0
.clr:
    mov byte [cs:drawn + di], 0
    inc di
    loop .clr

    ; painter's: NWALLS passes, draw max-z undrawn each time
    mov cx, NWALLS
.pass:
    push cx
    mov bp, 0xFFFF                    ; best z (use -1 sentinel below)
    mov word [cs:best_z], 0
    mov di, 0xFFFF                    ; best index*2
    xor si, si
.find:
    mov bx, si
    shr bx, 1
    cmp byte [cs:drawn + bx], 0
    jne .fn
    mov ax, [cs:wall_z + si]
    cmp ax, [cs:best_z]
    jle .fn
    mov [cs:best_z], ax
    mov di, si
.fn:
    add si, 2
    cmp si, NWALLS*2
    jl .find
    cmp di, 0xFFFF
    je .pdone
    mov bx, di
    shr bx, 1
    mov byte [cs:drawn + bx], 1
    mov si, di
    call draw_wall
.pdone:
    pop cx
    loop .pass

    call draw_ship
    ret

; draw_wall: SI = wall index*2
draw_wall:
    mov bx, si
    mov ax, [cs:wall_z + bx]
    mov [cs:cur_z], ax
    cmp ax, RECYZ
    jl .done
    ZCOLOR                           ; AL = colour from z (AX clobbered)
    mov [cs:cur_col], al
    mov bx, si
    mov ax, [cs:wall_gap + bx]
    mov [cs:cur_gap], ax
    xor di, di                       ; slot
.col:
    mov ax, di
    cmp ax, [cs:cur_gap]
    jl .solid
    mov bx, [cs:cur_gap]
    add bx, GAPW
    cmp ax, bx
    jl .skip
.solid:
    call draw_block
.skip:
    inc di
    cmp di, SLOTS
    jl .col
.done:
    ret

; draw_block: DI = slot index, uses cur_z + cur_col
draw_block:
    push di
    ; centre x (.4) = (2*di - 8) * 16
    mov ax, di
    shl ax, 1
    sub ax, 8
    mov cx, 16
    imul cx
    mov bx, ax                       ; centre .4
    mov dx, bx
    sub dx, HALFBW                   ; left
    add bx, HALFBW                   ; right
    mov [cs:t_r], bx
    mov ax, dx                       ; left
    mov cx, [cs:cur_z]
    call project
    add ax, CXC
    mov [cs:r_l], ax
    mov ax, [cs:t_r]
    mov cx, [cs:cur_z]
    call project
    add ax, CXC
    mov [cs:r_r], ax
    mov ax, WTOP
    mov cx, [cs:cur_z]
    call project
    neg ax
    add ax, CYC
    mov [cs:r_t], ax
    mov ax, WBOT
    mov cx, [cs:cur_z]
    call project
    neg ax
    add ax, CYC
    mov [cs:r_b], ax
    mov al, [cs:cur_col]
    call fill_rect
    pop di
    ret

draw_ship:
    mov ax, [cs:player_x]
    sub ax, 20
    mov cx, PZ
    call project
    add ax, CXC
    mov [cs:r_l], ax
    mov ax, [cs:player_x]
    add ax, 20
    mov cx, PZ
    call project
    add ax, CXC
    mov [cs:r_r], ax
    mov ax, -8
    mov cx, PZ
    call project
    neg ax
    add ax, CYC
    mov [cs:r_t], ax
    mov ax, -40
    mov cx, PZ
    call project
    neg ax
    add ax, CYC
    mov [cs:r_b], ax
    mov al, 6
    cmp byte [cs:dead], 0
    je .c
    mov al, 7
.c:
    call fill_rect
    ret

; fill_rect: AL=colour, rect in [r_l,r_t,r_r,r_b]; clamps to 0..319/0..199
fill_rect:
    mov [cs:f_col], al
    mov bx, [cs:r_l]
    mov dx, [cs:r_r]
    cmp bx, 0
    jge .lx
    xor bx, bx
.lx:
    cmp dx, 320
    jle .rx
    mov dx, 320
.rx:
    sub dx, bx                       ; width
    jle .none
    mov cx, [cs:r_t]
    mov si, [cs:r_b]
    cmp cx, 0
    jge .ty
    xor cx, cx
.ty:
    cmp si, 200
    jle .by
    mov si, 200
.by:
    sub si, cx                       ; height
    jle .none
    mov al, [cs:f_col]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
.none:
    ret

; project: AX = coord (.4 signed), CX = z (.4 > 0) -> AX = coord*FOCAL/z
project:
    mov bx, FOCAL
    imul bx                          ; DX:AX = coord*160
    idiv cx                          ; AX = quotient
    ret

set_palette:
    mov si, pal_data
    mov cx, 8
    xor bx, bx
.sp:
    push cx
    mov dh, [cs:si]
    mov ch, [cs:si+1]
    mov cl, [cs:si+2]
    mov ax, 0x1010
    int 0x10
    add si, 3
    inc bx
    pop cx
    loop .sp
    ret

; ============================================================================
win_title:  db '3D Runner', 0
saved_mode: db 0
win_handle: db 0
rng:        dw 0
player_x:   dw 0
speed:      dw 7
score:      dw 0
last_gap:   dw 0
dead:       db 0
attract:    db 1
flash:      db 0
in_left:    db 0
in_right:   db 0
in_fire:    db 0
cur_z:      dw 0
cur_col:    db 0
cur_gap:    dw 0
best_z:     dw 0
t_r:        dw 0
r_l:        dw 0
r_r:        dw 0
r_t:        dw 0
r_b:        dw 0
f_col:      db 0
drawn:      times NWALLS db 0
wall_z:     times NWALLS dw 0
wall_gap:   times NWALLS dw 0
pal_data:
    db 0,  0,  16
    db 24, 8,  40
    db 34, 10, 52
    db 44, 14, 60
    db 52, 18, 62
    db 60, 24, 63
    db 0,  60, 60
    db 60, 8,  8
