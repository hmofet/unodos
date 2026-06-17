; ============================================================================
; UnoDOS / Bandai WonderSwan (NEC V30MZ) — milestones 1-3.
; ============================================================================
; The EIGHTH fresh contract-driven port and the FIRST x86 handheld. The V30MZ is
; an 80186-class CPU — the same instruction family as the reference kernel — so
; this is built with nasm (16-bit real mode) and verified on a Unicorn x86 core
; (ws/harness.py), exactly as the GBA port is verified on a Unicorn ARM core.
;
; MINIMAL profile (CONTRACT-ARCH §9): one full-screen app at a time, directional
; nav via the X-pad. The picture is a hardware tile display — an 8x8 2bpp planar
; tile set in the 16 KB internal RAM (0x2000) plus the 32x32 SCR1 tilemap (0x0800)
; whose visible window is the top-left 28x18 tiles (224x144). "Drawing" is a write
; of a tile entry (tile# | palette<<9) into the map; colour is the per-tile palette
; resolving through the mono shade-pool, so the Theme app recolours everything by
; rewriting the pool (ports 0x1C-0x1F) with no tile-data change.
;
; M1: boot to a rendered launcher (inverted title bar + an 11-item icon list).
; M2: read the keypad (port 0xB5 group-select), a line-counter-polled frame loop,
;     an Up/Down selection highlight, A launches the app full-screen, B returns.
; M3: full-screen apps — SysInfo, live Clock, Notepad, Files, Theme (pool cycle),
;     Music (sound channel 1), and Dostris (the falling-blocks game).
;
; Contract-owned (Phase 4): the visible geometry comes from unogen
; ([world.ws] -> unodef/gen/ws/sys_gen.inc : SCRCOLS/SCRROWS).
; ============================================================================

%include "build/ws_equ.inc"                 ; NICONS/NTHEMES/MUSIC_COUNT/TILE_BLOCK/ICON_BASE/NTILES
%include "../unodef/gen/ws/sys_gen.inc"      ; SCRCOLS (28) / SCRROWS (18)

bits 16
cpu 186
org 0x0000                                   ; ROM maps to segment 0xF000 (top 64 KB)

; ---- I/O ports -------------------------------------------------------------
DISP_CTRL   equ 0x00        ; bit0 SCR1 enable, bit1 SCR2, bit2 sprites
MAP_BASE    equ 0x07        ; bits0-3 = SCR1 map base in 2 KB units
SCR1_X      equ 0x10        ; scroll
SCR1_Y      equ 0x11
LCD_CTRL    equ 0x14        ; bit0 = LCD on
LINE_CUR    equ 0x03        ; current scanline (>=144 = vblank)
POOL0       equ 0x1C        ; mono shade pool (4 ports, 2 nibbles each = 8 greys)
PAL0        equ 0x20        ; 16 palettes x 2 bytes (4 colours of 4 bits = pool index)
SND_CH1FL   equ 0x80        ; ch1 period low / high
SND_CH1FH   equ 0x81
SND_CH1VOL  equ 0x88
SND_WAVE    equ 0x8F        ; wavetable base (>>6)
SND_CTRL    equ 0x90        ; bit0 = ch1 enable
SND_OUT     equ 0x91        ; output enable + volume
KEYPAD      equ 0xB5        ; group-select keypad

; ---- memory map (16 KB internal RAM) ---------------------------------------
WAVE        equ 0x0040      ; 16-byte ch1 wavetable (64-byte aligned -> SND_WAVE=1)
MAP1        equ 0x0800      ; SCR1 32x32 tilemap (2 KB)
TILES       equ 0x2000      ; tile patterns (NTILES*16 bytes)

; ---- keypad bits (active-high, as assembled by read_keys) ------------------
PAD_U       equ 0x01        ; X1
PAD_R       equ 0x02        ; X2
PAD_D       equ 0x04        ; X3
PAD_L       equ 0x08        ; X4
PAD_START   equ 0x20
PAD_A       equ 0x40
PAD_B       equ 0x80

; ---- Dostris geometry (tile cells) -----------------------------------------
BW          equ 10
BHT          equ 15
BORG_COL    equ 9           ; board left column (centres 10 in 28)
BORG_ROW    equ 1           ; board top row (row 0 = title bar)
FALLRATE    equ 30

; ---- variables (RAM words) -------------------------------------------------
VARS        equ 0x0100
v_pad       equ VARS+0
v_padp      equ VARS+2
v_pade      equ VARS+4
v_inapp     equ VARS+6
v_sel       equ VARS+8
v_selp      equ VARS+10
v_app       equ VARS+12
v_dirty     equ VARS+14
v_frac      equ VARS+16
v_ss        equ VARS+18
v_mm        equ VARS+20
v_hh        equ VARS+22
v_theme     equ VARS+24
m_idx       equ VARS+26
m_timer     equ VARS+28
m_play      equ VARS+30
pf_hl       equ VARS+32
pf_clk      equ VARS+34
pf_pc       equ VARS+36
pf_score    equ VARS+38
g_type      equ VARS+40
g_rot       equ VARS+42
g_px        equ VARS+44
g_py        equ VARS+46
g_state     equ VARS+48
g_fall      equ VARS+50
g_lines     equ VARS+52
g_seed      equ VARS+54
g_tx        equ VARS+56
g_ty        equ VARS+58
g_srot      equ VARS+60
g_oldpx     equ VARS+62
g_oldpy     equ VARS+64
g_oldrot    equ VARS+66
mlo         equ VARS+68
g_pt        equ VARS+70
g_lt        equ VARS+72
g_row       equ VARS+74
a_idx       equ VARS+76
a_tmr       equ VARS+78
a_pad       equ VARS+80
a_gpause    equ VARS+82
g_board     equ 0x0300      ; BW*BHT bytes

; ============================================================================
start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x2000
    ; clear the variable area (0x0100..0x02FF)
    mov di, VARS
    mov cx, 0x100
    xor ax, ax
    rep stosw
    call load_tiles
    call init_palettes
    call set_pool
    mov al, 0x01            ; SCR1 map base = 0x0800
    out MAP_BASE, al
    xor al, al
    out SCR1_X, al
    out SCR1_Y, al
    mov al, 0x01            ; LCD on
    out LCD_CTRL, al
    mov al, 0x01            ; display: SCR1 enable
    out DISP_CTRL, al
    mov word [g_seed], 0x1234
    call draw_launcher
mainloop:
    call wait_vblank
    call render_partials
    call read_keys
    call clock_advance
    call update
    mov ax, [v_dirty]
    test ax, ax
    jz mainloop
    call full_redraw
    jmp mainloop

; ---- wait for vblank (line counter >= 144) ---------------------------------
wait_vblank:
.w:
    in al, LINE_CUR
    cmp al, 144
    jb .w
    ret

; ---- copy the generated tile patterns ROM -> RAM 0x2000 --------------------
load_tiles:
    push ds
    mov ax, cs
    mov ds, ax
    mov si, tiles_data
    xor ax, ax
    mov es, ax
    mov di, TILES
    mov cx, (NTILES*16)/2
    rep movsw
    pop ds
    ret

; ---- palettes 0-7 (colours are shade-pool indices) -------------------------
;  0 normal  : c0=pool0 (white)  c1=pool7 (black)
;  1 inverted: c0=pool7 (black)  c1=pool0 (white)   -> title bars / selection
;  2-7       : c0=pool0          c1=pool[n]          -> Dostris piece shades
init_palettes:
    mov al, 0x70            ; pal0 byte0: (c1=7<<4)|(c0=0)
    out PAL0+0, al
    xor al, al
    out PAL0+1, al
    mov al, 0x07            ; pal1 byte0: (c1=0)|(c0=7)
    out PAL0+2, al
    xor al, al
    out PAL0+3, al
    mov bx, 2               ; palettes 2..7
.pl:
    mov ax, bx
    shl ax, 4              ; c1 = pool index = palette number (c0=0)
    mov dx, PAL0
    add dx, bx
    add dx, bx             ; port = PAL0 + n*2
    out dx, al
    inc dx
    xor al, al
    out dx, al
    inc bx
    cmp bx, 8
    jb .pl
    ret

; ---- program the shade pool from theme_pools[v_theme] (ports 0x1C-0x1F) -----
set_pool:
    mov ax, [v_theme]
    shl ax, 2             ; theme*4 bytes
    mov bx, ax
    add bx, theme_pools
    mov al, [cs:bx+0]
    out POOL0+0, al
    mov al, [cs:bx+1]
    out POOL0+1, al
    mov al, [cs:bx+2]
    out POOL0+2, al
    mov al, [cs:bx+3]
    out POOL0+3, al
    ret

; ============================================================================
; tile-map helpers
; ============================================================================
; mapaddr: CL=col, CH=row -> DI = MAP1 + (row*32+col)*2  (clobbers AX,BX)
mapaddr:
    mov al, ch
    xor ah, ah
    mov bx, ax
    shl bx, 5             ; row*32
    mov al, cl
    xor ah, ah
    add bx, ax            ; +col
    shl bx, 1             ; *2 bytes
    add bx, MAP1
    mov di, bx
    ret

; puttile: CL=col, CH=row, AX=entry word -> write to the map (clobbers BX,DI)
puttile:
    push ax
    call mapaddr
    pop ax
    mov [di], ax
    ret

; putstr: CL=col, CH=row, SI=string offset (ROM), DL=palette -> draw, advance CL
putstr:
.l:
    mov al, [cs:si]
    inc si
    test al, al
    jz .d
    mov ah, dl
    shl ah, 1            ; AX = char | (pal<<9)
    push si
    call puttile
    pop si
    inc cl
    jmp .l
.d:
    ret

; put2: AX=value(0..99) -> 2 decimal digit tiles (pal0) at CL,CH; CL+=2
put2:
    xor dx, dx
    mov bx, 10
    div bx              ; AX=tens, DX=units
    add al, '0'
    xor ah, ah
    call puttile        ; pal0 (AH=0)
    inc cl
    mov al, dl
    add al, '0'
    xor ah, ah
    call puttile
    inc cl
    ret

; putc0: AL=char -> one tile (pal0) at CL,CH; CL++
putc0:
    xor ah, ah
    call puttile
    inc cl
    ret

; fill_map: AX=entry -> fill the whole 32x32 map
fill_map:
    push di
    mov di, MAP1
    mov cx, 32*32
    rep stosw
    pop di
    ret

; fill_row: CH=row, DL=pal -> fill cols 0..27 with the blank tile
fill_row:
    xor cl, cl
.f:
    xor al, al
    mov ah, dl
    shl ah, 1
    call puttile
    inc cl
    cmp cl, 28
    jb .f
    ret

; ============================================================================
; launcher (M1) — inverted title bar + an 11-item icon list
; ============================================================================
draw_launcher:
    xor ax, ax
    call fill_map           ; white desktop
    mov ch, 0               ; title bar
    mov dl, 1
    call fill_row
    mov cl, 1
    mov ch, 0
    mov dl, 1
    mov si, s_title
    call putstr
    xor bp, bp
.it:
    call draw_item
    inc bp
    cmp bp, NICONS
    jb .it
    ret

; draw_item: BP=index -> draw its list row (inverted palette if selected)
draw_item:
    xor dl, dl              ; palette
    mov ax, [v_sel]
    cmp ax, bp
    jne .np
    mov dl, 1
.np:
    mov ax, bp
    add ax, 2
    mov ch, al              ; row = index + 2
    call fill_row           ; row background (selection bar when inverted)
    mov cl, 2               ; icon at col 2
    mov ax, bp
    add ax, ICON_BASE
    mov ah, dl
    shl ah, 1
    call puttile
    mov cl, 4               ; label at col 4
    mov bx, bp
    shl bx, 1
    mov si, [cs:app_names+bx]
    call putstr
    ret

draw_highlight:
    mov bp, [v_selp]
    call draw_item
    mov bp, [v_sel]
    call draw_item
    ret

; ============================================================================
; input (M2)
; ============================================================================
read_keys:
%if AUTOTEST
    jmp auto_input
%else
    mov ax, [v_pad]
    mov [v_padp], ax
    mov al, 0x20            ; select X group (the d-pad)
    out KEYPAD, al
    in al, KEYPAD
    and al, 0x0F            ; bits0-3 = X1..X4 = U R D L
    mov bl, al
    mov al, 0x40            ; select button group
    out KEYPAD, al
    in al, KEYPAD
    and al, 0x0E            ; bits1-3 = Start,A,B
    shl al, 4              ; -> 0x20/0x40/0x80
    or bl, al
    xor bh, bh
    mov [v_pad], bx
    mov ax, [v_pad]
    mov cx, [v_padp]
    not cx
    and ax, cx
    mov [v_pade], ax
    ret
%endif

update:
    mov ax, [v_inapp]
    test ax, ax
    jnz .inapp
    call nav_input
    ret
.inapp:
    mov ax, [v_pade]
    test ax, PAD_B
    jz .disp
    call enter_launcher
    ret
.disp:
    mov ax, [v_app]
    cmp ax, 4
    je .theme
    cmp ax, 5
    je .music
    cmp ax, 6
    je .dostris
    ret
.theme:
    call theme_input
    ret
.music:
    call music_tick
    ret
.dostris:
    call dostris_update
    ret

nav_input:
    mov ax, [v_pade]
    test ax, PAD_A
    jnz .launch
    test ax, PAD_U
    jnz .up
    test ax, PAD_D
    jnz .down
    ret
.up:
    call sel_up
    ret
.down:
    call sel_down
    ret
.launch:
    mov ax, [v_sel]
    mov [v_app], ax
    mov word [v_inapp], 1
    call enter_app
    ret

sel_up:
    mov ax, [v_sel]
    mov [v_selp], ax
    test ax, ax
    jnz .dec
    mov ax, NICONS
.dec:
    dec ax
    mov [v_sel], ax
    mov word [pf_hl], 1
    ret

sel_down:
    mov ax, [v_sel]
    mov [v_selp], ax
    inc ax
    cmp ax, NICONS
    jb .ok
    xor ax, ax
.ok:
    mov [v_sel], ax
    mov word [pf_hl], 1
    ret

enter_app:
    mov ax, [v_app]
    cmp ax, 6
    jne .n6
    call dostris_init
.n6:
    mov ax, [v_app]
    cmp ax, 5
    jne .n5
    call music_init
.n5:
    mov word [v_dirty], 1
    ret

enter_launcher:
    mov word [v_inapp], 0
    call music_silence
    mov word [v_dirty], 1
    ret

; ============================================================================
; per-frame partial redraws (small map writes)
; ============================================================================
render_partials:
    mov ax, [v_inapp]
    test ax, ax
    jnz .app
    mov ax, [pf_hl]
    test ax, ax
    jz .ret
    call draw_highlight
    mov word [pf_hl], 0
.ret:
    ret
.app:
    mov ax, [v_app]
    cmp ax, 1
    je .clk
    cmp ax, 5
    je .ms
    cmp ax, 6
    je .pc
    ret
.clk:
    mov ax, [pf_clk]
    test ax, ax
    jz .ret
    call draw_clock_time
    mov word [pf_clk], 0
    ret
.ms:
    mov ax, [pf_score]
    test ax, ax
    jz .ret
    call draw_music_status
    mov word [pf_score], 0
    ret
.pc:
    mov ax, [pf_pc]
    test ax, ax
    jz .ret
    call draw_piece_partial
    mov word [pf_pc], 0
    ret

full_redraw:
    mov word [v_dirty], 0
    mov word [pf_hl], 0
    mov word [pf_pc], 0
    mov ax, [v_inapp]
    test ax, ax
    jnz .app
    call draw_launcher
    ret
.app:
    call draw_app
    ret

clock_advance:
    inc word [v_frac]
    cmp word [v_frac], 60
    jb .done
    mov word [v_frac], 0
    inc word [v_ss]
    cmp word [v_ss], 60
    jb .flag
    mov word [v_ss], 0
    inc word [v_mm]
    cmp word [v_mm], 60
    jb .flag
    mov word [v_mm], 0
    inc word [v_hh]
    cmp word [v_hh], 24
    jb .flag
    mov word [v_hh], 0
.flag:
    mov ax, [v_inapp]
    test ax, ax
    jz .done
    cmp word [v_app], 1
    jne .done
    mov word [pf_clk], 1
.done:
    ret

%include "apps.inc"
%include "dostris.inc"

; ============================================================================
; generated assets (in ROM, CS-addressable)
; ============================================================================
%include "build/ws_data.inc"

; ---- ROM header / reset vector (last 16 bytes of the image) ----------------
    times 0xFFF0 - ($ - $$) db 0
header:
    db 0xEA                  ; JMP FAR
    dw start
    dw 0xF000
    db 0x00                  ; maintenance
    db 0x00                  ; publisher
    db 0x00                  ; 0 = monochrome
    db 0x00                  ; game id
    db 0x00                  ; version
    db 0x02                  ; ROM size code
    db 0x00                  ; save type (none)
    db 0x04                  ; flags: horizontal orientation, 16-bit ROM bus
    db 0x00                  ; mapper
    dw 0x0000                ; checksum (patched by build.sh)
