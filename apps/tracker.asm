; TRACKER.BIN - pattern music editor for UnoDOS (Amiga-parity app)
;
; The 32-row x 4-channel tracker shared with the 68K ports: the pattern
; format is byte-identical (note 1..24 = C-2..B-3, instrument 0-3), so
; SONG.TRK files round-trip across every UnoDOS platform.
;
; The PC speaker is monophonic, so playback voices the LEFTMOST
; non-empty channel of each row (the 68K ports play all four).
;
;   arrows      move the cursor          q / w   note down / up
;   e           cycle instrument         x       clear cell
;   d           demo song                space   play / stop
;   s / l       save / load SONG.TRK     ESC     quit
;
; Build: nasm -f bin -o tracker.bin tracker.asm

[BITS 16]
[ORG 0x0000]
cpu 8086
%include "kernel/cpu8086.inc"

; --- Icon Header (80 bytes) ---
    db 0xEB, 0x4E
    db 'UI'
    db 'Tracker', 0
    times (0x04 + 12) - ($ - $$) db 0

    ; 16x16 icon, 2bpp: a pattern grid with bars (cyan=01, mag=10, wh=11)
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 0  frame
    db 0x30, 0x00, 0x00, 0x0C      ; Row 1
    db 0x33, 0x0C, 0xC0, 0x0C      ; Row 2  bars
    db 0x33, 0x0C, 0xC3, 0x0C      ; Row 3
    db 0x33, 0x2C, 0xC3, 0x0C      ; Row 4
    db 0x33, 0x2C, 0xF3, 0x0C      ; Row 5
    db 0x33, 0x2C, 0xF3, 0xCC      ; Row 6
    db 0x33, 0xAC, 0xF3, 0xCC      ; Row 7
    db 0x33, 0xAC, 0xF3, 0xCC      ; Row 8
    db 0x33, 0xAF, 0xF3, 0xCC      ; Row 9
    db 0x33, 0xAF, 0xF3, 0xCC      ; Row 10
    db 0x30, 0x00, 0x00, 0x0C      ; Row 11
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 12
    db 0x00, 0x00, 0x00, 0x00      ; Row 13
    db 0x00, 0x00, 0x00, 0x00      ; Row 14
    db 0x00, 0x00, 0x00, 0x00      ; Row 15

    times 0x50 - ($ - $$) db 0

; --- API indices ---
API_GFX_DRAW_FILLED_RECT equ 2
API_GFX_DRAW_STRING     equ 4
API_GFX_DRAW_STRING_INV equ 6
API_FILLED_RECT_COLOR   equ 67
API_EVENT_GET           equ 9
API_FS_MOUNT            equ 13
API_FS_OPEN             equ 14
API_FS_READ             equ 15
API_FS_CLOSE            equ 16
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34
API_SPEAKER_TONE        equ 41
API_SPEAKER_OFF         equ 42
API_GET_BOOT_DRIVE      equ 43
API_FS_CREATE           equ 45
API_FS_WRITE            equ 46
API_FS_DELETE           equ 47
API_GET_TICK            equ 63

EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

TK_ROWS                 equ 32
TK_CHANS                equ 4
TK_VIEW                 equ 12
TK_PATLEN               equ TK_ROWS*TK_CHANS*2
ROW_TICKS               equ 2       ; 18.2Hz ticks per row (~the 68K tempo)

KEY_UP                  equ 128
KEY_DOWN                equ 129
KEY_LEFT                equ 130
KEY_RIGHT               equ 131

; --- Entry ---
entry:
    PUSHA86
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov es, ax

%ifdef PROBE_ALIVE
    mov bx, 60
    mov cx, 96
    mov si, window_title
    mov ah, API_GFX_DRAW_STRING
    int 0x80
.hang:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    jmp .hang
%endif

    ; Create window
    mov bx, 24
    mov cx, 16
    mov dx, 272
    mov si, 168
    mov di, window_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [win_handle], al

    mov al, [win_handle]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    call draw_all

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    call play_tick

    mov ah, API_EVENT_GET
    int 0x80
    jc .main_loop
    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    call draw_all
    jmp .main_loop
.not_redraw:
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    call on_key
    cmp byte [quit_flag], 0
    je .main_loop

    mov ah, API_SPEAKER_OFF
    int 0x80
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

; --- keys (DL = key) ---
on_key:
    PUSHA86
    cmp dl, 27
    jne .nesc
    mov byte [quit_flag], 1
    jmp .done
.nesc:
    cmp dl, KEY_UP
    jne .ndu
    cmp word [cur_row], 0
    je .redraw
    dec word [cur_row]
    jmp .redraw
.ndu:
    cmp dl, KEY_DOWN
    jne .ndd
    cmp word [cur_row], TK_ROWS-1
    jge .redraw
    inc word [cur_row]
    jmp .redraw
.ndd:
    cmp dl, KEY_LEFT
    jne .ndl
    cmp word [cur_ch], 0
    je .redraw
    dec word [cur_ch]
    jmp .redraw
.ndl:
    cmp dl, KEY_RIGHT
    jne .ndr
    cmp word [cur_ch], TK_CHANS-1
    jge .redraw
    inc word [cur_ch]
    jmp .redraw
.ndr:
    cmp dl, 'q'
    jne .nq
    call cur_cell                   ; BX -> cell
    mov al, [bx]
    test al, al
    jnz .qdec
    mov byte [bx], 1
    jmp .preview
.qdec:
    cmp al, 1
    jbe .redraw
    dec byte [bx]
    jmp .preview
.nq:
    cmp dl, 'w'
    jne .nw
    call cur_cell
    mov al, [bx]
    test al, al
    jnz .winc
    mov byte [bx], 1
    jmp .preview
.winc:
    cmp al, 24
    jae .redraw
    inc byte [bx]
    jmp .preview
.nw:
    cmp dl, 'e'
    jne .ne
    call cur_cell
    cmp byte [bx], 0
    je .redraw
    mov al, [bx+1]
    inc al
    and al, 3
    mov [bx+1], al
    jmp .redraw
.ne:
    cmp dl, 'x'
    jne .nx
    call cur_cell
    mov word [bx], 0
    jmp .redraw
.nx:
    cmp dl, 'd'
    jne .nd
    call load_demo
    jmp .redraw
.nd:
    cmp dl, 's'
    jne .ns
    call song_save
    jmp .redraw
.ns:
    cmp dl, 'l'
    jne .nl
    call song_load
    jmp .redraw
.nl:
    cmp dl, ' '
    jne .done
    cmp byte [playing], 0
    je .start
    mov byte [playing], 0
    mov ah, API_SPEAKER_OFF
    int 0x80
    jmp .redraw
.start:
    mov byte [playing], 1
    mov word [play_row], TK_ROWS-1  ; first tick wraps to row 0
    mov ah, API_GET_TICK
    int 0x80
    sub ax, ROW_TICKS
    mov [last_tick], ax
    jmp .redraw
.preview:
    call cur_cell
    call play_cell                  ; hear the edit
.redraw:
    call draw_all
.done:
    POPA86
    ret

; cur_cell -> BX = &pattern[(row*4+ch)*2]
cur_cell:
    mov bx, [cur_row]
    shl bx, 1
    shl bx, 1                       ; row*4
    add bx, [cur_ch]
    shl bx, 1                       ; *2 bytes
    add bx, pattern
    ret

; play_cell - BX -> cell: voice it on the speaker (with auto-off later)
play_cell:
    push ax
    push bx
    mov al, [bx]
    test al, al
    jz .off
    push bx
    mov bl, al
    xor bh, bh
    dec bx
    shl bx, 1
    mov bx, [note_hz + bx]
    mov ah, API_SPEAKER_TONE
    int 0x80
    pop bx
    jmp .out
.off:
    mov ah, API_SPEAKER_OFF
    int 0x80
.out:
    pop bx
    pop ax
    ret

; play_tick - sequencer: advance a row every ROW_TICKS; voice the
; leftmost non-empty channel (PC speaker = one voice)
play_tick:
    PUSHA86
    cmp byte [playing], 0
    je .out
    mov ah, API_GET_TICK
    int 0x80
    mov bx, ax
    sub ax, [last_tick]
    cmp ax, ROW_TICKS
    jl .out
    mov [last_tick], bx
    mov ax, [play_row]
    inc ax
    cmp ax, TK_ROWS
    jl .rok
    xor ax, ax
.rok:
    mov [play_row], ax
    ; find the leftmost non-empty cell of this row
    mov bx, ax
    shl bx, 1
    shl bx, 1
    shl bx, 1                       ; row*8 bytes
    add bx, pattern
    mov cx, TK_CHANS
.scan:
    cmp byte [bx], 0
    jne .voice
    add bx, 2
    loop .scan
    jmp .row_done                   ; all rests: keep the last tone
.voice:
    call play_cell
.row_done:
    call draw_rows                  ; play-bar update (rows only)
.out:
    POPA86
    ret

; --- drawing ------------------------------------------------------------
draw_all:
    PUSHA86
    ; header
    mov bx, 6
    mov cx, 4
    mov si, hdr_text
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    call draw_rows
    ; footers
    mov bx, 6
    mov cx, 140
    mov si, foot1
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 6
    mov cx, 150
    mov si, foot2
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    POPA86
    ret

; draw_rows - the visible TK_VIEW rows with cursor / play bars
draw_rows:
    PUSHA86
    ; keep the cursor row in view
    mov ax, [cur_row]
    cmp ax, [top_row]
    jge .t1
    mov [top_row], ax
.t1:
    mov bx, [top_row]
    add bx, TK_VIEW
    cmp ax, bx
    jl .t2
    sub ax, TK_VIEW-1
    mov [top_row], ax
.t2:
    mov word [draw_i], 0
.row_loop:
    mov ax, [draw_i]
    cmp ax, TK_VIEW
    jge .done
    mov bx, [top_row]
    add bx, ax                      ; row index
    mov [draw_row], bx

    ; row band: clear to black; the play row gets a magenta band and
    ; the cursor row renders its text inverted (white-on-black APIs)
    mov ax, [draw_i]
    mov cx, 10
    mul cx                          ; row pitch 10px
    add ax, 16
    mov cx, ax                      ; y
    push cx
    mov bx, 4                       ; x
    mov dx, 256                     ; w
    mov si, 9                       ; h
    mov al, 0                       ; clear (background color)
    mov di, [draw_row]
    cmp byte [playing], 0
    je .nb
    cmp di, [play_row]
    jne .nb
    mov al, 2                       ; play bar: magenta
.nb:
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop cx

    ; row label
    mov ax, [draw_row]
    call fmt2
    mov bx, 6
    inc cx
    mov si, numbuf
    call draw_row_string

    ; cells
    mov word [draw_ch], 0
.cell_loop:
    mov ax, [draw_ch]
    cmp ax, TK_CHANS
    jge .next_row
    ; cell -> text
    mov bx, [draw_row]
    shl bx, 1
    shl bx, 1
    add bx, [draw_ch]
    shl bx, 1
    add bx, pattern
    call fmt_cell                   ; -> cellbuf
    mov ax, [draw_ch]
    mov dx, 56
    mul dx
    add ax, 30
    mov bx, ax
    mov si, cellbuf
    call draw_row_string
    inc word [draw_ch]
    jmp .cell_loop
.next_row:
    inc word [draw_i]
    jmp .row_loop
.done:
    POPA86
    ret

; draw_row_string - BX/CX/SI like API 4; inverted on the cursor row
draw_row_string:
    push ax
    push di
    mov di, [draw_row]
    cmp di, [cur_row]
    jne .plain
    mov ah, API_GFX_DRAW_STRING_INV
    int 0x80
    jmp .out
.plain:
    mov ah, API_GFX_DRAW_STRING
    int 0x80
.out:
    pop di
    pop ax
    ret

; fmt2 - AX (0-99) -> numbuf "NN",0
fmt2:
    push ax
    push bx
    push dx
    xor dx, dx
    mov bx, 10
    div bx
    add al, '0'
    mov [numbuf], al
    add dl, '0'
    mov [numbuf+1], dl
    mov byte [numbuf+2], 0
    pop dx
    pop bx
    pop ax
    ret

; fmt_cell - BX -> cell: "C#2 S" / "--- -" into cellbuf
fmt_cell:
    push ax
    push bx
    push si
    mov al, [bx]
    test al, al
    jnz .note
    mov word [cellbuf], '--'
    mov word [cellbuf+2], ' -'
    mov byte [cellbuf+2], '-'
    mov byte [cellbuf+3], ' '
    mov byte [cellbuf+4], '-'
    mov byte [cellbuf+5], 0
    jmp .out
.note:
    push bx
    dec al
    xor ah, ah
    mov bl, 12
    div bl                          ; AL = octave, AH = semitone
    push ax
    mov al, ah
    xor ah, ah
    shl ax, 1
    mov si, ax
    mov al, [note_names + si]
    mov [cellbuf], al
    mov al, [note_names + si + 1]
    mov [cellbuf+1], al
    pop ax
    add al, '2'
    mov [cellbuf+2], al
    mov byte [cellbuf+3], ' '
    pop bx
    mov al, [bx+1]
    and al, 3
    xor ah, ah
    mov si, ax
    mov al, [inst_names + si]
    mov [cellbuf+4], al
    mov byte [cellbuf+5], 0
.out:
    pop si
    pop bx
    pop ax
    ret

; --- demo + disk ---------------------------------------------------------
load_demo:
    push cx
    push si
    push di
    mov si, demo_song
    mov di, pattern
    mov cx, TK_PATLEN
    rep movsb
    pop di
    pop si
    pop cx
    ret

song_save:
    PUSHA86
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov ah, API_FS_MOUNT
    int 0x80
    mov [mountb], bl
    mov si, song_name
    mov ah, API_FS_DELETE
    int 0x80
    mov si, song_name
    mov bl, [mountb]
    mov ah, API_FS_CREATE
    int 0x80
    jc .out
    mov [fileh], al
    mov ax, cs
    mov es, ax
    mov bx, pattern
    mov cx, TK_PATLEN
    mov al, [fileh]
    mov ah, API_FS_WRITE
    int 0x80
    mov al, [fileh]
    mov ah, API_FS_CLOSE
    int 0x80
.out:
    POPA86
    ret

song_load:
    PUSHA86
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov ah, API_FS_MOUNT
    int 0x80
    mov [mountb], bl
    mov si, song_name
    mov ah, API_FS_OPEN
    int 0x80
    jc .out
    mov [fileh], al
    mov ax, cs
    mov es, ax
    mov bx, pattern
    mov cx, TK_PATLEN
    mov al, [fileh]
    mov ah, API_FS_READ
    int 0x80
    mov al, [fileh]
    mov ah, API_FS_CLOSE
    int 0x80
.out:
    POPA86
    ret

; --- data ----------------------------------------------------------------
window_title:   db 'Tracker', 0
hdr_text:       db 'Row  Ch1    Ch2    Ch3    Ch4', 0
foot1:          db 'q/w:note e:inst x:clr d:demo s/l:disk', 0
foot2:          db 'Space:play/stop  (PC spkr: 1 voice)', 0
song_name:      db 'SONG.TRK', 0
note_names:     db 'C-C#D-D#E-F-F#G-G#A-A#B-'
inst_names:     db 'SSTN'

; note 1..24 -> Hz (C4..B5: the same pitches the 68K ports play)
note_hz:
    dw 262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494
    dw 523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988

; demo song - byte-identical to the 68K trackers
demo_song:
    db 1,1, 13,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0
    db 0,0, 17,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0
    db 1,1, 20,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0
    db 0,0, 17,0, 13,2,  0,0,  0,0, 0,0, 0,0, 0,0
    db 8,1, 13,0, 17,2, 20,3,  0,0, 0,0, 0,0, 0,0
    db 0,0, 15,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0
    db 8,1, 20,0, 15,2, 20,3,  0,0, 0,0, 0,0, 0,0
    db 0,0, 15,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0
    db 6,1, 10,0, 13,2, 20,3,  0,0, 0,0, 0,0, 0,0
    db 0,0, 13,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0
    db 6,1, 17,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0
    db 0,0, 13,0, 10,2,  0,0,  0,0, 0,0, 0,0, 0,0
    db 8,1, 11,0, 15,2, 20,3,  0,0, 0,0, 0,0, 0,0
    db 0,0, 15,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0
    db 8,1, 20,0, 19,2, 20,3,  0,0, 0,0, 0,0, 0,0
    db 0,0, 23,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0

win_handle:     db 0
quit_flag:      db 0
playing:        db 0
mountb:         db 0
fileh:          db 0
cur_row:        dw 0
cur_ch:         dw 0
top_row:        dw 0
play_row:       dw 0
last_tick:      dw 0
draw_i:         dw 0
draw_row:       dw 0
draw_ch:        dw 0
numbuf:         times 4 db 0
cellbuf:        times 8 db 0
pattern:        times TK_PATLEN db 0
