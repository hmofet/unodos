; PAINT.BIN - MacPaint-style bitmap editor for UnoDOS
;
; The shared UnoDOS Paint design (also on Mac, Amiga, Genesis): a tool
; palette down the left (pencil, brush, eraser, line, rect, filled
; rect, oval, filled oval, flood fill, spray), a color strip along the
; bottom, and a drag-to-draw canvas with a byte-per-pixel backing
; store, repainted as horizontal runs.
;
; Color selector = every color the active mode can show: 4 in CGA,
; the full 256-entry VGA palette in mode 13h ('c' opens an all-colors
; picker; the bottom strip holds 16 quick swatches).
;
;   1..9,0   select tool (also clickable)   c   all-colors picker
;   n        clear canvas                   s/l save/load PAINT.UNO
;   ESC      quit (or close the picker)
;
; Build: nasm -f bin -o paint.bin paint.asm

[BITS 16]
[ORG 0x0000]
cpu 8086
%include "kernel/cpu8086.inc"

; --- Icon Header (80 bytes) ---
    db 0xEB, 0x4E
    db 'UI'
    db 'Paint', 0
    times (0x04 + 12) - ($ - $$) db 0

    ; 16x16 icon, 2bpp: brush over a paint daub (cyan=01,mag=10,wh=11)
    db 0x00, 0x00, 0x0F, 0x00
    db 0x00, 0x00, 0x3F, 0x00
    db 0x00, 0x00, 0xFC, 0x00
    db 0x00, 0x03, 0xF0, 0x00
    db 0x00, 0x0F, 0xC0, 0x00
    db 0x00, 0x57, 0x00, 0x00
    db 0x01, 0x55, 0x00, 0x00
    db 0x01, 0x54, 0x00, 0x00
    db 0x05, 0x50, 0x00, 0x00
    db 0x05, 0x40, 0x00, 0x00
    db 0x28, 0x00, 0xAA, 0x00
    db 0xAA, 0x02, 0xAA, 0xA0
    db 0xAA, 0xAA, 0xAA, 0xA8
    db 0xAA, 0xAA, 0xAA, 0xAA
    db 0x2A, 0xAA, 0xAA, 0xA8
    db 0x02, 0xAA, 0xAA, 0x80

    times 0x50 - ($ - $$) db 0

; --- API indices ---
API_GFX_DRAW_PIXEL      equ 0
API_GFX_DRAW_STRING     equ 4
API_EVENT_GET           equ 9
API_FS_MOUNT            equ 13
API_FS_OPEN             equ 14
API_FS_READ             equ 15
API_FS_CLOSE            equ 16
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_MOUSE_STATE         equ 28
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34
API_GET_BOOT_DRIVE      equ 43
API_FS_CREATE           equ 45
API_FS_WRITE            equ 46
API_FS_DELETE           equ 47
API_HIT_TEST            equ 53
API_FILLED_RECT_COLOR   equ 67
API_HLINE_COLOR         equ 69
API_WIN_GET_INFO        equ 79
API_GET_SCREEN_INFO     equ 82
API_WIN_GET_CONTENT     equ 97

EVENT_KEY_PRESS         equ 1
EVENT_MOUSE             equ 4
EVENT_WIN_REDRAW        equ 6

PT_W                    equ 224
PT_H                    equ 120
PT_BG                   equ 3
CAN_X                   equ 30
CAN_Y                   equ 4
TOOLS                   equ 10

T_PENCIL                equ 0
T_BRUSH                 equ 1
T_ERASER                equ 2
T_LINE                  equ 3
T_RECT                  equ 4
T_FRECT                 equ 5
T_OVAL                  equ 6
T_FOVAL                 equ 7
T_FILL                  equ 8
T_SPRAY                 equ 9

; ===========================================================================
entry:
    PUSHA86
    push ds
    push es
    mov ax, cs
    mov ds, ax
    mov es, ax

    mov ah, API_GET_SCREEN_INFO
    int 0x80
    mov [mode_colors], ah           ; 4 = CGA, else VGA-class

    mov bx, 12
    mov cx, 8
    mov dx, 292
    mov si, 178
    mov di, window_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [win_handle], al

    mov al, [win_handle]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    call update_origin
    call canvas_clear
    call draw_all

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    mov ah, API_EVENT_GET
    int 0x80
    jc .main_loop
    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    call update_origin
    call draw_all
    jmp .main_loop
.not_redraw:
    cmp al, EVENT_MOUSE
    jne .not_mouse
    test dl, 1
    jz .main_loop
    call on_click
    jmp .main_loop
.not_mouse:
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    call on_key
    cmp byte [quit_flag], 0
    je .main_loop

    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80
    xor ax, ax
    jmp .exit
.exit_fail:
    mov ax, 1
.exit:
    pop es
    pop ds
    POPA86
    retf

; update_origin - content origin on screen: (winX+1, winY+winH-contentH-2)
update_origin:
    PUSHA86
    mov al, [win_handle]
    mov ah, API_WIN_GET_INFO
    int 0x80
    inc bx
    mov [org_x], bx
    add cx, si
    mov [org_y], cx
    mov al, [win_handle]
    mov ah, API_WIN_GET_CONTENT
    int 0x80                        ; SI = content height
    mov ax, [org_y]
    sub ax, si
    sub ax, 2
    mov [org_y], ax
    POPA86
    ret

; ===========================================================================
; keys
; ===========================================================================
on_key:
    PUSHA86
    cmp dl, 27
    jne .nesc
    cmp byte [picker_on], 0
    je .quit
    mov byte [picker_on], 0
    call draw_all
    jmp .done
.quit:
    mov byte [quit_flag], 1
    jmp .done
.nesc:
    cmp dl, '1'
    jb .nd
    cmp dl, '9'
    ja .nd
    sub dl, '1'
    mov [cur_tool], dl
    call draw_all
    jmp .done
.nd:
    cmp dl, '0'
    jne .nz
    mov byte [cur_tool], T_SPRAY
    call draw_all
    jmp .done
.nz:
    cmp dl, 'c'
    jne .nc
    xor byte [picker_on], 1
    call draw_all
    jmp .done
.nc:
    cmp dl, 'n'
    jne .nn
    call canvas_clear
    call draw_all
    jmp .done
.nn:
    cmp dl, 's'
    jne .ns
    call canvas_save
    jmp .done
.ns:
    cmp dl, 'l'
    jne .done
    call canvas_load
    call draw_all
.done:
    POPA86
    ret

; ===========================================================================
; clicks
; ===========================================================================
on_click:
    PUSHA86
    ; tool cells: content (4, 4+i*15, 20, 13)
    mov word [tmp_i], 0
.tt:
    mov ax, [tmp_i]
    cmp ax, TOOLS
    jge .tpens
    mov cx, 15
    mul cx
    add ax, 4
    mov cx, ax
    mov bx, 4
    mov dx, 20
    mov si, 13
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jz .ttn
    mov ax, [tmp_i]
    mov [cur_tool], al
    call draw_all
    jmp .done
.ttn:
    inc word [tmp_i]
    jmp .tt
.tpens:
    ; quick strip: content (CAN_X+i*14, CAN_Y+PT_H+6, 12, 10)
    mov word [tmp_i], 0
.tp:
    mov ax, [tmp_i]
    cmp ax, 16
    jge .tcv
    mov cx, 14
    mul cx
    add ax, CAN_X
    mov bx, ax
    mov cx, CAN_Y+PT_H+6
    mov dx, 12
    mov si, 10
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jz .tpn
    mov bx, [tmp_i]
    mov al, [quick_colors + bx]
    mov [cur_color], al
    call draw_all
    jmp .done
.tpn:
    inc word [tmp_i]
    jmp .tp
.tcv:
    cmp byte [picker_on], 0
    je .ncv
    call picker_click
    jmp .done
.ncv:
    mov bx, CAN_X
    mov cx, CAN_Y
    mov dx, PT_W
    mov si, PT_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jz .done
    call mouse_canvas
    mov al, [cur_tool]
    cmp al, T_FILL
    jne .nfill
    call flood_fill
    call repaint_canvas
    jmp .done
.nfill:
    cmp al, T_LINE
    jb .free
    cmp al, T_FOVAL
    ja .free
    call drag_shape
    jmp .done
.free:
    call drag_free
.done:
    POPA86
    ret

; picker_click - pick from the all-colors grid (canvas area overlay)
picker_click:
    PUSHA86
    call mouse_canvas               ; [mx],[my] within the canvas
    mov al, [mode_colors]
    cmp al, 4
    je .cga
    ; VGA: 16x16 grid of 13x7 cells starting (4,4)
    mov ax, [my]
    sub ax, 4
    js .out
    mov cx, 7
    xor dx, dx
    div cx                          ; AX = grid row
    cmp ax, 16
    jae .out
    mov [tmp_i], ax
    mov ax, [mx]
    sub ax, 4
    js .out
    mov cx, 13
    xor dx, dx
    div cx                          ; AX = grid col
    cmp ax, 16
    jae .out
    mov cx, [tmp_i]
    push ax
    mov ax, cx
    mov cl, 4
    SHL_N ax, 4
    pop cx
    add ax, cx                      ; row*16 + col
    mov [cur_color], al
    jmp .pick
.cga:
    ; 4 big swatches of 48x24 at (8,8)
    mov ax, [mx]
    sub ax, 8
    js .out
    mov cx, 50
    xor dx, dx
    div cx
    cmp ax, 4
    jae .out
    mov [cur_color], al
.pick:
    mov byte [picker_on], 0
    call draw_all
.out:
    POPA86
    ret

; mouse_canvas - live mouse -> canvas coords in [mx],[my] (clamped)
mouse_canvas:
    PUSHA86
    mov ah, API_MOUSE_STATE
    int 0x80                        ; BX = X, CX = Y
    sub bx, [org_x]
    sub bx, CAN_X
    sub cx, [org_y]
    sub cx, CAN_Y
    or bx, bx
    jns .x0
    xor bx, bx
.x0:
    cmp bx, PT_W-1
    jle .x1
    mov bx, PT_W-1
.x1:
    or cx, cx
    jns .y0
    xor cx, cx
.y0:
    cmp cx, PT_H-1
    jle .y1
    mov cx, PT_H-1
.y1:
    mov [mx], bx
    mov [my], cx
    POPA86
    ret

; mouse_held -> AL = 1 while the left button is down
; (post-audit API 28: live button state is in DL, not AL)
mouse_held:
    push bx
    push cx
    push dx
    mov ah, API_MOUSE_STATE
    int 0x80
    mov al, dl
    and al, 1
    pop dx
    pop cx
    pop bx
    ret

; ===========================================================================
; canvas primitives - args in named vars: [px_x],[px_y],[px_c]
; ===========================================================================
px_set:
    PUSHA86
    mov bx, [px_x]
    mov cx, [px_y]
    cmp bx, PT_W
    jae .out
    cmp cx, PT_H
    jae .out
    mov ax, cx
    mov si, PT_W
    mul si
    mov si, ax
    add si, bx
    mov al, [px_c]
    mov [canvas + si], al
    add bx, CAN_X
    add cx, CAN_Y
    mov ah, API_GFX_DRAW_PIXEL
    int 0x80
.out:
    POPA86
    ret

; px_get - [px_x],[px_y] -> [px_c]
px_get:
    PUSHA86
    mov byte [px_c], PT_BG
    mov bx, [px_x]
    mov cx, [px_y]
    cmp bx, PT_W
    jae .out
    cmp cx, PT_H
    jae .out
    mov ax, cx
    mov si, PT_W
    mul si
    mov si, ax
    add si, bx
    mov al, [canvas + si]
    mov [px_c], al
.out:
    POPA86
    ret

; dot - square of [dot_size] at ([px_x],[px_y]); [px_c] preset by caller
dot:
    PUSHA86
    mov ax, [px_x]
    mov [tmp_x], ax
    mov ax, [px_y]
    mov [tmp_y], ax
    mov ax, [dot_size]
    shr ax, 1
    mov bx, [tmp_x]
    sub bx, ax
    mov [tmp_x], bx
    mov bx, [tmp_y]
    sub bx, ax
    mov [tmp_y], bx
    mov word [tmp_j], 0
.row:
    mov ax, [tmp_j]
    cmp ax, [dot_size]
    jge .done
    mov word [tmp_i], 0
.col:
    mov ax, [tmp_i]
    cmp ax, [dot_size]
    jge .nrow
    mov ax, [tmp_x]
    add ax, [tmp_i]
    mov [px_x], ax
    mov ax, [tmp_y]
    add ax, [tmp_j]
    mov [px_y], ax
    call px_set
    inc word [tmp_i]
    jmp .col
.nrow:
    inc word [tmp_j]
    jmp .row
.done:
    POPA86
    ret

; line_seg - ([lx0],[ly0]) -> ([lx1],[ly1]) with dot ([dot_size],[px_c])
; Canonical Bresenham; e2 computed once per step.
line_seg:
    PUSHA86
    ; dx = abs(x1-x0), sx
    mov ax, [lx1]
    sub ax, [lx0]
    mov word [l_sx], 1
    jns .dxok
    neg ax
    mov word [l_sx], -1
.dxok:
    mov [l_dx], ax
    mov ax, [ly1]
    sub ax, [ly0]
    mov word [l_sy], 1
    jns .dyok
    neg ax
    mov word [l_sy], -1
.dyok:
    neg ax
    mov [l_dy], ax                  ; dy = -abs
    mov ax, [l_dx]
    add ax, [l_dy]
    mov [l_err], ax
.loop:
    mov ax, [lx0]
    mov [px_x], ax
    mov ax, [ly0]
    mov [px_y], ax
    call dot
    mov ax, [lx0]
    cmp ax, [lx1]
    jne .cont
    mov ax, [ly0]
    cmp ax, [ly1]
    je .done
.cont:
    mov ax, [l_err]
    add ax, ax                      ; e2 (one value for both tests)
    cmp ax, [l_dy]
    jl .ny
    mov bx, [l_dy]
    add [l_err], bx
    mov bx, [l_sx]
    add [lx0], bx
.ny:
    cmp ax, [l_dx]
    jg .loop
    mov bx, [l_dx]
    add [l_err], bx
    mov bx, [l_sy]
    add [ly0], bx
    jmp .loop
.done:
    POPA86
    ret

; rect_shape - ([lx0],[ly0])-([lx1],[ly1]) frame/[shape_fill]
rect_shape:
    PUSHA86
    call norm_box
    mov ax, [ly0]
    mov [tmp_j], ax
.row:
    mov ax, [tmp_j]
    cmp ax, [ly1]
    jg .done
    ; full row when filled or top/bottom edge
    cmp byte [shape_fill], 0
    jne .full
    mov bx, [ly0]
    cmp ax, bx
    je .full
    mov bx, [ly1]
    cmp ax, bx
    je .full
    ; sides
    mov ax, [lx0]
    mov [px_x], ax
    mov ax, [tmp_j]
    mov [px_y], ax
    call px_set
    mov ax, [lx1]
    mov [px_x], ax
    call px_set
    jmp .next
.full:
    mov ax, [lx0]
    mov [tmp_i], ax
.px:
    mov ax, [tmp_i]
    cmp ax, [lx1]
    jg .next
    mov [px_x], ax
    mov ax, [tmp_j]
    mov [px_y], ax
    call px_set
    inc word [tmp_i]
    jmp .px
.next:
    inc word [tmp_j]
    jmp .row
.done:
    POPA86
    ret

; oval_shape - bounding box rows: h = isqrt(a^2 - a^2*dy^2/b^2)
oval_shape:
    PUSHA86
    call norm_box
    mov ax, [lx1]
    sub ax, [lx0]
    shr ax, 1
    mov [o_a], ax                   ; a
    mov ax, [ly1]
    sub ax, [ly0]
    shr ax, 1
    mov [o_b], ax                   ; b
    cmp word [o_a], 0
    je .degen
    cmp word [o_b], 0
    jne .ok
.degen:
    call rect_shape
    jmp .out
.ok:
    mov ax, [lx0]
    add ax, [o_a]
    mov [o_cx], ax
    mov ax, [ly0]
    add ax, [o_b]
    mov [o_cy], ax
    mov ax, [ly0]
    mov [tmp_j], ax
.rw:
    mov ax, [tmp_j]
    cmp ax, [ly1]
    jg .out
    ; t = a*a*dy*dy / (b*b)  (a,b <= 112: products fit 16x16->32)
    mov ax, [tmp_j]
    sub ax, [o_cy]
    push ax
    imul ax                         ; DX:AX = dy^2 (fits 16 bits)
    pop bx
    mov bx, ax                      ; dy^2
    mov ax, [o_a]
    mul ax                          ; a^2 (<= 12544)
    mov [o_a2], ax
    mul bx                          ; DX:AX = a^2 * dy^2
    mov bx, [o_b]
    push ax
    mov ax, bx
    mul bx                          ; b^2
    mov bx, ax
    pop ax
    div bx                          ; AX = t
    mov bx, [o_a2]
    sub bx, ax                      ; h^2
    jns .h2ok
    xor bx, bx
.h2ok:
    ; h = isqrt(h2) by increment (h <= 112)
    xor cx, cx
.sq:
    mov ax, cx
    inc ax
    mul ax
    cmp ax, bx
    ja .goth
    inc cx
    jmp .sq
.goth:
    ; row span cx-h .. cx+h on row tmp_j
    mov ax, [o_cx]
    sub ax, cx
    mov [o_xl], ax
    mov ax, [o_cx]
    add ax, cx
    mov [o_xr], ax
    cmp byte [shape_fill], 0
    je .frame
    mov ax, [o_xl]
    mov [tmp_i], ax
.fp:
    mov ax, [tmp_i]
    cmp ax, [o_xr]
    jg .nx
    mov [px_x], ax
    mov ax, [tmp_j]
    mov [px_y], ax
    call px_set
    inc word [tmp_i]
    jmp .fp
.frame:
    mov ax, [o_xl]
    mov [px_x], ax
    mov ax, [tmp_j]
    mov [px_y], ax
    call px_set
    mov ax, [o_xr]
    mov [px_x], ax
    call px_set
.nx:
    inc word [tmp_j]
    jmp .rw
.out:
    POPA86
    ret

; norm_box - order lx0/ly0 <= lx1/ly1
norm_box:
    push ax
    push bx
    mov ax, [lx0]
    mov bx, [lx1]
    cmp ax, bx
    jle .x
    mov [lx0], bx
    mov [lx1], ax
.x:
    mov ax, [ly0]
    mov bx, [ly1]
    cmp ax, bx
    jle .y
    mov [ly0], bx
    mov [ly1], ax
.y:
    pop bx
    pop ax
    ret

; flood_fill - scanline fill from [mx],[my] with [cur_color]
FF_STK equ 400
flood_fill:
    PUSHA86
    mov ax, [mx]
    mov [px_x], ax
    mov ax, [my]
    mov [px_y], ax
    call px_get
    mov al, [px_c]
    mov [ff_from], al
    cmp al, [cur_color]
    je .done
    ; push the seed
    mov ax, [mx]
    mov [ff_stack], ax
    mov ax, [my]
    mov [ff_stack+2], ax
    mov word [ff_n], 1
.pop:
    cmp word [ff_n], 0
    je .done
    dec word [ff_n]
    mov bx, [ff_n]
    SHL_N bx, 2
    mov ax, [ff_stack + bx]
    mov [ff_x], ax
    mov ax, [ff_stack + bx + 2]
    mov [ff_y], ax
    ; still target?
    mov ax, [ff_x]
    mov [px_x], ax
    mov ax, [ff_y]
    mov [px_y], ax
    call px_get
    mov al, [px_c]
    cmp al, [ff_from]
    jne .pop
    ; run left
.left:
    cmp word [ff_x], 0
    je .lend
    mov ax, [ff_x]
    dec ax
    mov [px_x], ax
    call px_get
    mov al, [px_c]
    cmp al, [ff_from]
    jne .lend
    dec word [ff_x]
    jmp .left
.lend:
    ; sweep right
.right:
    mov ax, [ff_x]
    cmp ax, PT_W
    jge .pop
    mov [px_x], ax
    mov ax, [ff_y]
    mov [px_y], ax
    call px_get
    mov al, [px_c]
    cmp al, [ff_from]
    jne .pop
    mov al, [cur_color]
    mov [px_c], al
    call px_set
    ; queue up
    cmp word [ff_y], 0
    je .ndn
    mov ax, [ff_y]
    dec ax
    mov [px_y], ax
    call px_get
    mov al, [px_c]
    cmp al, [ff_from]
    jne .nup
    cmp word [ff_n], FF_STK
    jge .nup
    mov bx, [ff_n]
    SHL_N bx, 2
    mov ax, [ff_x]
    mov [ff_stack + bx], ax
    mov ax, [ff_y]
    dec ax
    mov [ff_stack + bx + 2], ax
    inc word [ff_n]
.nup:
.ndn:
    mov ax, [ff_y]
    inc ax
    cmp ax, PT_H
    jge .ndn2
    mov [px_y], ax
    mov ax, [ff_x]
    mov [px_x], ax
    call px_get
    mov al, [px_c]
    cmp al, [ff_from]
    jne .ndn2
    cmp word [ff_n], FF_STK
    jge .ndn2
    mov bx, [ff_n]
    SHL_N bx, 2
    mov ax, [ff_x]
    mov [ff_stack + bx], ax
    mov ax, [ff_y]
    inc ax
    mov [ff_stack + bx + 2], ax
    inc word [ff_n]
.ndn2:
    inc word [ff_x]
    jmp .right
.done:
    POPA86
    ret

; lfsr -> AX
lfsr:
    mov ax, [rng]
    shl ax, 1
    jnc .nx
    xor ax, 0x1D87
.nx:
    jnz .ok
    mov ax, 0xACE1
.ok:
    mov [rng], ax
    ret

; ===========================================================================
; drag loops (synchronous - the classic one-app-paints model)
; ===========================================================================
drag_free:
    PUSHA86
    mov ax, [mx]
    mov [lx0], ax
    mov ax, [my]
    mov [ly0], ax
.lp:
    mov al, [cur_tool]
    cmp al, T_SPRAY
    je .spray
    ; size + color by tool
    mov word [dot_size], 1
    cmp al, T_BRUSH
    jne .nb
    mov word [dot_size], 3
.nb:
    cmp al, T_ERASER
    jne .ne
    mov word [dot_size], 6
.ne:
    mov al, [cur_color]
    cmp byte [cur_tool], T_ERASER
    jne .ink
    mov al, PT_BG
.ink:
    mov [px_c], al
    ; segment last -> current
    mov ax, [mx]
    mov [lx1], ax
    mov ax, [my]
    mov [ly1], ax
    call line_seg                   ; consumes lx0/ly0 (walks them)
    mov ax, [mx]
    mov [lx0], ax
    mov ax, [my]
    mov [ly0], ax
    jmp .next
.spray:
    mov al, [cur_color]
    mov [px_c], al
    mov word [dot_size], 1
    mov cx, 6
.sp:
    push cx
    call lfsr
    and ax, 15
    sub ax, 8
    add ax, [mx]
    mov [px_x], ax
    call lfsr
    and ax, 15
    sub ax, 8
    add ax, [my]
    mov [px_y], ax
    call px_set
    pop cx
    loop .sp
.next:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    call mouse_canvas
    call mouse_held
    test al, al
    jnz .lp
    POPA86
    ret

drag_shape:
    PUSHA86
    mov ax, [mx]
    mov [sh_x0], ax
    mov ax, [my]
    mov [sh_y0], ax
.lp:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    call mouse_canvas
    call mouse_held
    test al, al
    jnz .lp
    ; release: commit anchor -> [mx],[my]
    mov ax, [sh_x0]
    mov [lx0], ax
    mov ax, [sh_y0]
    mov [ly0], ax
    mov ax, [mx]
    mov [lx1], ax
    mov ax, [my]
    mov [ly1], ax
    mov al, [cur_color]
    mov [px_c], al
    mov word [dot_size], 1
    mov byte [shape_fill], 0
    mov al, [cur_tool]
    cmp al, T_LINE
    jne .nl
    call line_seg
    jmp .done
.nl:
    cmp al, T_RECT
    jne .nr
    call rect_shape
    jmp .done
.nr:
    cmp al, T_FRECT
    jne .no
    mov byte [shape_fill], 1
    call rect_shape
    jmp .done
.no:
    cmp al, T_OVAL
    jne .nfo
    call oval_shape
    jmp .done
.nfo:
    mov byte [shape_fill], 1
    call oval_shape
.done:
    POPA86
    ret

; ===========================================================================
; drawing
; ===========================================================================
draw_all:
    PUSHA86
    ; tool cells
    mov word [tmp_i], 0
.tool:
    mov ax, [tmp_i]
    cmp ax, TOOLS
    jge .strip
    mov cx, 15
    mul cx
    add ax, 4
    mov cx, ax
    mov bx, 4
    mov dx, 20
    mov si, 13
    mov al, 0
    cmp byte [picker_on], 0
    jne .tbg
    mov ax, [tmp_i]
    cmp al, [cur_tool]
    jne .tbg2
    mov al, 2                       ; selected: magenta
    jmp .tbg
.tbg2:
    mov al, 1                       ; cyan
.tbg:
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    ; digit label
    mov bx, 11
    mov ax, [tmp_i]
    mov cx, 15
    mul cx
    add ax, 7
    mov cx, ax
    mov ax, [tmp_i]
    inc ax
    cmp ax, 10
    jne .dig
    xor ax, ax
.dig:
    add al, '0'
    mov [numbuf], al
    mov byte [numbuf+1], 0
    mov si, numbuf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    inc word [tmp_i]
    jmp .tool
.strip:
    ; quick swatches
    mov word [tmp_i], 0
.pen:
    mov ax, [tmp_i]
    cmp ax, 16
    jge .canvas
    mov cx, 14
    mul cx
    add ax, CAN_X
    mov bx, ax
    mov cx, CAN_Y+PT_H+6
    mov dx, 12
    mov si, 10
    push bx
    mov bx, [tmp_i]
    mov al, [quick_colors + bx]
    pop bx
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    inc word [tmp_i]
    jmp .pen
.canvas:
    cmp byte [picker_on], 0
    je .cv
    call draw_picker
    jmp .foot
.cv:
    call repaint_canvas
.foot:
    mov bx, 4
    mov cx, CAN_Y+PT_H+22
    mov si, foot1
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    POPA86
    ret

; draw_picker - all colors of the mode over the canvas area
draw_picker:
    PUSHA86
    ; clear the canvas area first
    mov bx, CAN_X
    mov cx, CAN_Y
    mov dx, PT_W
    mov si, PT_H
    mov al, 0
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    cmp byte [mode_colors], 4
    je .cga
    ; 256 colors: 16x16 grid of 13x7 cells at canvas (4,4)
    mov word [tmp_j], 0
.row:
    cmp word [tmp_j], 16
    jge .out
    mov word [tmp_i], 0
.col:
    cmp word [tmp_i], 16
    jge .nrow
    mov ax, [tmp_i]
    mov cx, 13
    mul cx
    add ax, CAN_X+4
    mov bx, ax
    mov ax, [tmp_j]
    mov cx, 7
    mul cx
    add ax, CAN_Y+4
    mov cx, ax
    mov dx, 11
    mov si, 6
    mov ax, [tmp_j]
    SHL_N ax, 4
    add ax, [tmp_i]                 ; color = row*16+col
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    inc word [tmp_i]
    jmp .col
.nrow:
    inc word [tmp_j]
    jmp .row
.cga:
    ; 4 big swatches
    mov word [tmp_i], 0
.cg:
    cmp word [tmp_i], 4
    jge .out
    mov ax, [tmp_i]
    mov cx, 50
    mul cx
    add ax, CAN_X+8
    mov bx, ax
    mov cx, CAN_Y+8
    mov dx, 44
    mov si, 24
    mov ax, [tmp_i]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    inc word [tmp_i]
    jmp .cg
.out:
    POPA86
    ret

; repaint_canvas - run-length rows via the colored hline API
repaint_canvas:
    PUSHA86
    mov word [tmp_j], 0             ; y
.row:
    mov ax, [tmp_j]
    cmp ax, PT_H
    jge .done
    mov si, PT_W
    mul si
    mov [tmp_o], ax                 ; row offset into canvas
    mov word [tmp_i], 0             ; x
.run:
    mov ax, [tmp_i]
    cmp ax, PT_W
    jge .nrow
    mov bx, [tmp_o]
    add bx, ax
    mov dl, [canvas + bx]           ; run color
    mov [tmp_c], dl
    mov [tmp_x], ax                 ; run start
.ext:
    inc word [tmp_i]
    mov ax, [tmp_i]
    cmp ax, PT_W
    jge .flush
    mov bx, [tmp_o]
    add bx, ax
    mov dl, [canvas + bx]
    cmp dl, [tmp_c]
    je .ext
.flush:
    mov bx, [tmp_x]
    add bx, CAN_X
    mov cx, [tmp_j]
    add cx, CAN_Y
    mov dx, [tmp_i]
    sub dx, [tmp_x]                 ; length
    mov al, [tmp_c]
    mov ah, API_HLINE_COLOR
    int 0x80
    jmp .run
.nrow:
    inc word [tmp_j]
    jmp .row
.done:
    POPA86
    ret

canvas_clear:
    push ax
    push cx
    push di
    push es
    mov ax, cs
    mov es, ax
    mov di, canvas
    mov cx, (PT_W*PT_H)/2
    mov ax, (PT_BG << 8) | PT_BG
    rep stosw
    pop es
    pop di
    pop cx
    pop ax
    ret

; ===========================================================================
; disk
; ===========================================================================
canvas_save:
    PUSHA86
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov ah, API_FS_MOUNT
    int 0x80
    mov [mountb], bl
    mov si, file_name
    mov ah, API_FS_DELETE
    int 0x80
    mov si, file_name
    mov bl, [mountb]
    mov ah, API_FS_CREATE
    int 0x80
    jc .out
    mov [fileh], al
    mov ax, cs
    mov es, ax
    mov bx, canvas
    mov cx, PT_W*PT_H
    mov al, [fileh]
    mov ah, API_FS_WRITE
    int 0x80
    mov al, [fileh]
    mov ah, API_FS_CLOSE
    int 0x80
.out:
    POPA86
    ret

canvas_load:
    PUSHA86
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov ah, API_FS_MOUNT
    int 0x80
    mov [mountb], bl
    mov si, file_name
    mov ah, API_FS_OPEN
    int 0x80
    jc .out
    mov [fileh], al
    mov ax, cs
    mov es, ax
    mov bx, canvas
    mov cx, PT_W*PT_H
    mov al, [fileh]
    mov ah, API_FS_READ
    int 0x80
    mov al, [fileh]
    mov ah, API_FS_CLOSE
    int 0x80
.out:
    POPA86
    ret

; ===========================================================================
; data
; ===========================================================================
window_title:   db 'Paint', 0
foot1:          db '1-0:tool c:colors n:new s/l:disk', 0
file_name:      db 'PAINT.UNO', 0
quick_colors:   db 0, 1, 2, 3, 4, 20, 40, 56, 80, 104, 128, 152, 176, 200, 224, 255

win_handle:     db 0
quit_flag:      db 0
picker_on:      db 0
mode_colors:    db 4
cur_tool:       db 0
cur_color:      db 2
mountb:         db 0
fileh:          db 0
tmp_c:          db 0
shape_fill:     db 0
ff_from:        db 0
                db 0                ; (alignment)
org_x:          dw 0
org_y:          dw 0
mx:             dw 0
my:             dw 0
px_x:           dw 0
px_y:           dw 0
px_c:           dw 0
dot_size:       dw 1
tmp_i:          dw 0
tmp_j:          dw 0
tmp_x:          dw 0
tmp_y:          dw 0
tmp_o:          dw 0
lx0:            dw 0
ly0:            dw 0
lx1:            dw 0
ly1:            dw 0
l_dx:           dw 0
l_dy:           dw 0
l_sx:           dw 0
l_sy:           dw 0
l_err:          dw 0
o_a:            dw 0
o_b:            dw 0
o_a2:           dw 0
o_cx:           dw 0
o_cy:           dw 0
o_xl:           dw 0
o_xr:           dw 0
sh_x0:          dw 0
sh_y0:          dw 0
ff_x:           dw 0
ff_y:           dw 0
ff_n:           dw 0
rng:            dw 0xACE1
numbuf:         times 4 db 0
ff_stack:       times FF_STK*4 db 0
canvas:         times PT_W*PT_H db 0
