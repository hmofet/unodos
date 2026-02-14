; MUSIC.BIN - Fur Elise music player for UnoDOS
; Plays Beethoven's Fur Elise opening using the PC speaker
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
    ; Musical note icon: eighth note
    db 0x00, 0x00, 0x00, 0x00      ; Row 0:  ................
    db 0x00, 0x00, 0xFF, 0xF0      ; Row 1:  ..........######
    db 0x00, 0x00, 0xFF, 0xFC      ; Row 2:  ..........########
    db 0x00, 0x00, 0x00, 0x3C      ; Row 3:  ..............####
    db 0x00, 0x00, 0x00, 0x0C      ; Row 4:  ................##
    db 0x00, 0x00, 0x00, 0x0C      ; Row 5:  ................##
    db 0x00, 0x00, 0x00, 0x0C      ; Row 6:  ................##
    db 0x00, 0x00, 0x00, 0x0C      ; Row 7:  ................##
    db 0x00, 0x00, 0x00, 0x0C      ; Row 8:  ................##
    db 0x00, 0x00, 0x00, 0x0C      ; Row 9:  ................##
    db 0x00, 0x03, 0xC0, 0x0C      ; Row 10: ......####......##
    db 0x00, 0x0F, 0xF0, 0x0C      ; Row 11: ....########....##
    db 0x00, 0x0F, 0xF0, 0x0C      ; Row 12: ....########....##
    db 0x00, 0x03, 0xC0, 0x00      ; Row 13: ......####........
    db 0x00, 0x00, 0x00, 0x00      ; Row 14: ................
    db 0x00, 0x00, 0x00, 0x00      ; Row 15: ................

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API function indices
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

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Note frequencies (Hz) - equal temperament A4=440
NOTE_REST   equ 0
NOTE_C4     equ 262
NOTE_D4     equ 294
NOTE_E4     equ 330
NOTE_F4     equ 349
NOTE_GS4    equ 415
NOTE_A4     equ 440
NOTE_B4     equ 494
NOTE_C5     equ 523
NOTE_D5     equ 587
NOTE_DS5    equ 622
NOTE_E5     equ 659

; Timing (BIOS ticks, ~55ms each at 18.2 Hz)
DUR_SIXTEENTH equ 2                ; ~110ms
DUR_EIGHTH  equ 3                  ; ~165ms
DUR_QUARTER equ 6                  ; ~330ms
DUR_HALF    equ 12                 ; ~660ms
DUR_GAP     equ 1                  ; Inter-note gap

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create window
    mov bx, 60                      ; X
    mov cx, 50                      ; Y
    mov dx, 200                     ; Width
    mov si, 80                      ; Height
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:win_handle], al

    ; Set window drawing context
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Draw initial content
    call draw_content

    ; Initialize note pointer
    mov word [cs:note_index], 0

    ; === Main playback loop ===
.play_loop:
    ; Get current note from table
    mov si, [cs:note_index]
    shl si, 2                       ; Each entry = 4 bytes (freq + duration)
    add si, note_data

    ; Check for end marker
    mov bx, [cs:si]
    cmp bx, 0xFFFF
    je .playback_done

    mov cx, [cs:si + 2]            ; CX = duration in ticks

    ; Play tone or rest
    test bx, bx
    jz .play_rest
    mov ah, API_SPEAKER_TONE
    int 0x80
    jmp .wait_duration
.play_rest:
    mov ah, API_SPEAKER_OFF
    int 0x80

.wait_duration:
    ; Record start tick
    call read_bios_tick
    mov [cs:start_tick], ax

.wait_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; Check for ESC key
    push cx
    mov ah, API_EVENT_GET
    int 0x80
    cmp al, EVENT_KEY_PRESS
    jne .check_redraw
    cmp dl, 27                      ; ESC?
    je .esc_pop_exit
    jmp .event_done
.check_redraw:
    cmp al, EVENT_WIN_REDRAW
    jne .event_done
    call draw_content
.event_done:
    pop cx

    ; Check elapsed time
    call read_bios_tick
    sub ax, [cs:start_tick]
    cmp ax, cx
    jb .wait_loop                   ; Keep waiting

    ; Brief silence gap between notes
    mov ah, API_SPEAKER_OFF
    int 0x80
    call read_bios_tick
    mov [cs:start_tick], ax
.gap_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    call read_bios_tick
    sub ax, [cs:start_tick]
    cmp ax, DUR_GAP
    jb .gap_loop

    ; Next note
    inc word [cs:note_index]
    jmp .play_loop

.esc_pop_exit:
    pop cx                          ; Balance the push cx
    jmp .stop_and_exit

.playback_done:
    ; Turn off speaker
    mov ah, API_SPEAKER_OFF
    int 0x80

    ; Show "Done" message
    mov bx, 4
    mov cx, 40
    mov dx, 190
    mov si, 18
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    mov bx, 40
    mov cx, 40
    mov si, msg_done
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Wait for ESC to exit
.wait_exit:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    mov ah, API_EVENT_GET
    int 0x80
    cmp al, EVENT_KEY_PRESS
    jne .wait_exit_redraw
    cmp dl, 27
    je .stop_and_exit
    jmp .wait_exit
.wait_exit_redraw:
    cmp al, EVENT_WIN_REDRAW
    jne .wait_exit
    call draw_content
    jmp .wait_exit

.stop_and_exit:
    mov ah, API_SPEAKER_OFF
    int 0x80

    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80

.exit_fail:
    pop es
    pop ds
    popa
    retf

; === Helper: read BIOS tick counter ===
; Output: AX = current tick count (low word)
read_bios_tick:
    push ds
    push bx
    mov ax, 0x0040
    mov ds, ax
    mov ax, [0x006C]
    pop bx
    pop ds
    ret

; === Helper: draw window content ===
draw_content:
    pusha

    mov bx, 36
    mov cx, 4
    mov si, msg_title
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 4
    mov cx, 18
    mov si, msg_subtitle
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 40
    mov cx, 40
    mov si, msg_playing
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 28
    mov cx, 54
    mov si, msg_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; === Data ===
window_title:   db 'Music', 0
win_handle:     db 0
note_index:     dw 0
start_tick:     dw 0

msg_title:      db 'Fur Elise', 0
msg_subtitle:   db 'L. van Beethoven', 0
msg_playing:    db 'Playing...', 0
msg_done:       db 'Done! ESC=exit', 0
msg_esc:        db 'ESC to stop', 0

; === Note Data: (frequency, duration_ticks) pairs ===
; Beethoven's Fur Elise - opening theme (two phrases)
; End marker: 0xFFFF
note_data:
    ; Phrase 1: E5 D#5 E5 D#5 E5 B4 D5 C5 A4
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_A4,  DUR_QUARTER

    ; Phrase 2: (rest) C4 E4 A4 B4
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_QUARTER

    ; Phrase 3: (rest) E4 G#4 B4 C5
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_GS4, DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_QUARTER

    ; Phrase 4 (repeat): (rest) E4 E5 D#5 E5 D#5 E5 B4 D5 C5 A4
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

    ; Phrase 5: (rest) C4 E4 A4 B4
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_QUARTER

    ; Phrase 6: (rest) E4 C5 B4 A4
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_HALF

    ; End marker
    dw 0xFFFF, 0
