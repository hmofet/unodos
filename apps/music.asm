; MUSIC.BIN - Fur Elise music player for UnoDOS
; Full Beethoven's Fur Elise with clickable Play/Pause button
;
; Build: nasm -f bin -o music.bin music.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Music', 0                   ; App name (12 bytes)
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    ; Musical note icon: magenta note with white stem
    db 0x00, 0x00, 0x00, 0x00      ; Row 0
    db 0x00, 0x00, 0xAA, 0xA0      ; Row 1:  flag (magenta)
    db 0x00, 0x00, 0xAA, 0xA8      ; Row 2:  flag (magenta)
    db 0x00, 0x00, 0x00, 0x28      ; Row 3:  magenta tip
    db 0x00, 0x00, 0x00, 0x0C      ; Row 4:  white stem
    db 0x00, 0x00, 0x00, 0x0C      ; Row 5:  white stem
    db 0x00, 0x00, 0x00, 0x0C      ; Row 6:  white stem
    db 0x00, 0x00, 0x00, 0x0C      ; Row 7:  white stem
    db 0x00, 0x00, 0x00, 0x0C      ; Row 8:  white stem
    db 0x00, 0x00, 0x00, 0x0C      ; Row 9:  white stem
    db 0x00, 0x02, 0x80, 0x0C      ; Row 10: magenta note head
    db 0x00, 0x0A, 0xA0, 0x0C      ; Row 11: magenta note head
    db 0x00, 0x0A, 0xA0, 0x0C      ; Row 12: magenta note head
    db 0x00, 0x02, 0x80, 0x00      ; Row 13: magenta note head
    db 0x00, 0x00, 0x00, 0x00      ; Row 14
    db 0x00, 0x00, 0x00, 0x00      ; Row 15

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API constants
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34
API_SPEAKER_TONE        equ 41
API_SPEAKER_OFF         equ 42
API_DRAW_BUTTON         equ 51
API_HIT_TEST            equ 53
API_GET_TICK            equ 63

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_MOUSE             equ 4
EVENT_WIN_REDRAW        equ 6

; States
ST_PLAYING              equ 0
ST_PAUSED               equ 1
ST_DONE                 equ 2

; Button position/size (window-relative)
BTN_X                   equ 69
BTN_Y                   equ 50
BTN_W                   equ 60
BTN_H                   equ 12

; Note frequencies (Hz) - equal temperament A4=440
NOTE_REST   equ 0
NOTE_C4     equ 262
NOTE_D4     equ 294
NOTE_E4     equ 330
NOTE_F4     equ 349
NOTE_G4     equ 392
NOTE_GS4    equ 415
NOTE_A4     equ 440
NOTE_B4     equ 494
NOTE_C5     equ 523
NOTE_D5     equ 587
NOTE_DS5    equ 622
NOTE_E5     equ 659
NOTE_F5     equ 698

; Timing (BIOS ticks, ~55ms each at 18.2 Hz)
DUR_EIGHTH  equ 3                   ; ~165ms
DUR_QUARTER equ 6                   ; ~330ms
DUR_HALF    equ 12                  ; ~660ms
DUR_GAP     equ 1                   ; Inter-note gap

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create window
    mov bx, 60                      ; X
    mov cx, 45                      ; Y
    mov dx, 200                     ; Width
    mov si, 100                     ; Height
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:wh], al

    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Init state
    mov byte [cs:state], ST_PLAYING
    mov word [cs:note_idx], 0
    mov byte [cs:prev_btn], 0
    mov byte [cs:quit], 0

    call draw_all

    ; === Main loop ===
.main:
    cmp byte [cs:quit], 1
    je .exit

    cmp byte [cs:state], ST_PLAYING
    jne .idle

    ; === Playing: process current note ===
    mov si, [cs:note_idx]
    shl si, 2                       ; 4 bytes per entry
    add si, notes

    mov bx, [cs:si]
    cmp bx, 0xFFFF
    je .song_done

    mov cx, [cs:si + 2]             ; CX = duration

    ; Play tone or rest
    test bx, bx
    jz .rest
    mov ah, API_SPEAKER_TONE
    int 0x80
    jmp .wait
.rest:
    mov ah, API_SPEAKER_OFF
    int 0x80

.wait:
    call read_tick
    mov [cs:t0], ax

.wait_lp:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    push cx
    call poll_events
    pop cx

    cmp byte [cs:quit], 1
    je .exit

    cmp byte [cs:state], ST_PAUSED
    je .main                         ; Paused during wait

    ; Check elapsed time
    call read_tick
    sub ax, [cs:t0]
    cmp ax, cx
    jb .wait_lp

    ; Inter-note gap
    mov ah, API_SPEAKER_OFF
    int 0x80
    call read_tick
    mov [cs:t0], ax
.gap_lp:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    call read_tick
    sub ax, [cs:t0]
    cmp ax, DUR_GAP
    jb .gap_lp

    inc word [cs:note_idx]
    jmp .main

.song_done:
    mov ah, API_SPEAKER_OFF
    int 0x80
    mov byte [cs:state], ST_DONE
    call draw_status
    call draw_button
    jmp .idle

.idle:
    cmp byte [cs:quit], 1
    je .exit
    sti
    mov ah, API_APP_YIELD
    int 0x80
    call poll_events
    jmp .main

.exit:
    mov ah, API_SPEAKER_OFF
    int 0x80
    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:wh]
    mov ah, API_WIN_DESTROY
    int 0x80

.exit_fail:
    pop es
    pop ds
    popa
    retf

; ============================================================================
; Event handler
; ============================================================================
poll_events:
    mov ah, API_EVENT_GET
    int 0x80
    cmp al, EVENT_KEY_PRESS
    je .key
    cmp al, EVENT_MOUSE
    je .mouse
    cmp al, EVENT_WIN_REDRAW
    je .redraw
    ret

.key:
    cmp dl, 27                      ; ESC
    je .esc
    cmp dl, ' '                     ; Space = toggle
    je .toggle
    ret
.esc:
    mov byte [cs:quit], 1
    ret
.toggle:
    jmp toggle_state                ; tail call

.mouse:
    test dl, 1                      ; Left button?
    jz .btn_up
    cmp byte [cs:prev_btn], 0
    jne .held                       ; Already held
    mov byte [cs:prev_btn], 1
    ; Hit test button using widget API
    mov bx, BTN_X
    mov cx, BTN_Y
    mov dx, BTN_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .btn_hit
    ret
.btn_hit:
    jmp toggle_state                ; tail call
.btn_up:
    mov byte [cs:prev_btn], 0
.held:
    ret

.redraw:
    jmp draw_all                    ; tail call

; ============================================================================
; Toggle play/pause state
; ============================================================================
toggle_state:
    cmp byte [cs:state], ST_PLAYING
    je .to_pause

    cmp byte [cs:state], ST_DONE
    je .restart

    ; Paused → resume
    mov byte [cs:state], ST_PLAYING
    jmp .update

.to_pause:
    mov byte [cs:state], ST_PAUSED
    mov ah, API_SPEAKER_OFF
    int 0x80
    jmp .update

.restart:
    mov word [cs:note_idx], 0
    mov byte [cs:state], ST_PLAYING

.update:
    call draw_status
    jmp draw_button                 ; tail call


; ============================================================================
; Drawing functions
; ============================================================================

; Draw entire window content
draw_all:
    pusha

    ; Title
    mov bx, 40
    mov cx, 4
    mov si, msg_title
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Subtitle
    mov bx, 12
    mov cx, 16
    mov si, msg_sub
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Help text
    mov bx, 4
    mov cx, 74
    mov si, msg_help
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    call draw_status
    jmp draw_button                 ; tail call

; Draw status text
draw_status:
    pusha

    ; Clear status area
    mov bx, 0
    mov cx, 34
    mov dx, 198
    mov si, 10
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Pick status text
    cmp byte [cs:state], ST_PLAYING
    je .playing
    cmp byte [cs:state], ST_DONE
    je .done
    mov si, msg_paused
    jmp .draw
.playing:
    mov si, msg_playing
    jmp .draw
.done:
    mov si, msg_finished
.draw:
    mov bx, 52
    mov cx, 35
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; Draw play/pause button using widget API
draw_button:
    pusha

    ; Pick label
    cmp byte [cs:state], ST_PLAYING
    je .lbl_pause
    cmp byte [cs:state], ST_DONE
    je .lbl_replay
    mov di, btn_play
    jmp .draw_btn
.lbl_pause:
    mov di, btn_pause
    jmp .draw_btn
.lbl_replay:
    mov di, btn_replay
.draw_btn:
    mov ax, cs
    mov es, ax
    mov bx, BTN_X
    mov cx, BTN_Y
    mov dx, BTN_W
    mov si, BTN_H
    xor al, al                      ; Not pressed
    mov ah, API_DRAW_BUTTON
    int 0x80

    popa
    ret

; ============================================================================
; Helpers
; ============================================================================

; Read tick counter via kernel API
; Output: AX = tick count (low word)
read_tick:
    mov ah, API_GET_TICK
    int 0x80
    ret

; ============================================================================
; Data
; ============================================================================

win_title:  db 'Music', 0
wh:         db 0
state:      db 0
note_idx:   dw 0
t0:         dw 0
prev_btn:   db 0
quit:       db 0

msg_title:  db 'Fur Elise', 0
msg_sub:    db 'L. van Beethoven', 0
msg_playing: db 'Now Playing...', 0
msg_paused: db 'Paused', 0
msg_finished: db 'Song Complete', 0
msg_help:   db 'SPC/Click  ESC=Exit', 0

; Button labels (6 chars each for uniform width)
btn_play:   db ' Play ', 0
btn_pause:  db 'Pause ', 0
btn_replay: db 'Replay', 0

; ============================================================================
; Note Data: (frequency, duration_ticks) pairs
; Beethoven's Fur Elise, WoO 59 - Complete A-B-A form
; End marker: 0xFFFF
; ============================================================================
notes:
    ; === A SECTION (first time) ===

    ; Phrase 1: Trill → A minor
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_A4,  DUR_QUARTER

    ; Phrase 2: C-E-A → B
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_QUARTER

    ; Phrase 3: E-G#-B → C5
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_GS4, DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_QUARTER

    ; Phrase 4: Repeat trill
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_A4,  DUR_QUARTER

    ; Phrase 5: C-E-A → B (same as phrase 2)
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_QUARTER

    ; Phrase 6: E-C5-B → A (first ending)
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_QUARTER

    ; === B SECTION ===

    ; F major arpeggio ascending
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_F4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_F5,  DUR_QUARTER

    ; Descending from F5
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_F5,  DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_QUARTER

    ; Rising then falling
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_QUARTER

    ; Ascending to E5
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_E5,  DUR_QUARTER

    ; G4 passage
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_G4,  DUR_EIGHTH
    dw NOTE_F5,  DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_D5,  DUR_QUARTER

    ; Lead back to trill
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_QUARTER

    ; Pickup into A section
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH

    ; === A SECTION (final) ===

    ; Trill → A minor
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_A4,  DUR_QUARTER

    ; C-E-A → B
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_QUARTER

    ; E-G#-B → C5
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_GS4, DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_QUARTER

    ; Repeat trill
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_A4,  DUR_QUARTER

    ; C-E-A → B
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_QUARTER

    ; Final resolution: E-C5-B → A (hold)
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_HALF

    ; End marker
    dw 0xFFFF, 0
