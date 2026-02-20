; MUSIC.BIN - Music player with visual playback for UnoDOS
; 5 classical songs with scrolling staff notation
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
API_HLINE               equ 69
API_GET_TICK            equ 63
API_DRAW_SPRITE         equ 94

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_MOUSE             equ 4
EVENT_WIN_REDRAW        equ 6

; States
ST_PLAYING              equ 0
ST_PAUSED               equ 1
ST_DONE                 equ 2

; Window
WIN_X                   equ 20
WIN_Y                   equ 22
WIN_W                   equ 280
WIN_H                   equ 150

; Layout Y positions
TITLE_Y                 equ 2
COMP_Y                  equ 12
SEP_Y                   equ 22
STAFF_Y                 equ 26
STAFF_H                 equ 54
STATUS_Y                equ 84
BTN_ROW_Y               equ 96
BTN_H                   equ 12
HELP_Y                  equ 114

; Three buttons: Prev | Play/Pause | Next
BTNP_X                  equ 10       ; Prev button
BTNP_W                  equ 60
BTNC_X                  equ 85       ; Center (play/pause) button
BTNC_W                  equ 70
BTNN_X                  equ 170      ; Next button
BTNN_W                  equ 60

; Staff visualization
STAFF_BASE_Y            equ 60      ; Y of bottom staff line (E4)
STAFF_LINE_SPACING      equ 8
NOTES_VISIBLE           equ 21      ; notes shown in viewport
NOTE_SPACING            equ 12      ; pixels between note centers
STAFF_LEFT_X            equ 10      ; left margin for notes

; Note frequencies (Hz) - equal temperament A4=440
NOTE_REST   equ 0
NOTE_C4     equ 262
NOTE_D4     equ 294
NOTE_E4     equ 330
NOTE_F4     equ 349
NOTE_G4     equ 392
NOTE_GS4    equ 415
NOTE_A4     equ 440
NOTE_AS4    equ 466
NOTE_B4     equ 494
NOTE_C5     equ 523
NOTE_D5     equ 587
NOTE_DS5    equ 622
NOTE_E5     equ 659
NOTE_F5     equ 698
NOTE_G5     equ 784

; Timing (BIOS ticks, ~55ms each at 18.2 Hz)
DUR_EIGHTH  equ 3                   ; ~165ms
DUR_QUARTER equ 6                   ; ~330ms
DUR_HALF    equ 12                  ; ~660ms
DUR_DOTQ    equ 9                   ; dotted quarter ~495ms
DUR_GAP     equ 1                   ; Inter-note gap

; Song count
NUM_SONGS   equ 5

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create window
    mov bx, WIN_X
    mov cx, WIN_Y
    mov dx, WIN_W
    mov si, WIN_H
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
    mov byte [cs:state], ST_PAUSED
    mov word [cs:note_idx], 0
    mov byte [cs:cur_song], 0
    mov byte [cs:prev_btn], 0
    mov byte [cs:quit], 0
    call load_song_info

    call draw_all

    ; === Main loop ===
.main:
    cmp byte [cs:quit], 1
    je .exit

    cmp byte [cs:state], ST_PLAYING
    jne .idle

    ; === Playing: process current note ===
    call get_current_note           ; BX=freq, CX=duration
    cmp bx, 0xFFFF
    je .song_done

    ; Update visualization before playing
    call draw_staff

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
    je .main

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
    call draw_staff
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
; Get current note data
; Output: BX=frequency, CX=duration
; ============================================================================
get_current_note:
    push si
    mov si, [cs:cur_notes_ptr]
    mov ax, [cs:note_idx]
    shl ax, 2                       ; 4 bytes per entry
    add si, ax
    mov bx, [cs:si]                 ; freq
    mov cx, [cs:si + 2]             ; duration
    pop si
    ret

; ============================================================================
; Load song info from song table
; Uses: cur_song → sets cur_notes_ptr, cur_note_count, cur_title, cur_composer
; ============================================================================
load_song_info:
    push ax
    push bx
    push si
    movzx ax, byte [cs:cur_song]
    shl ax, 3                       ; 8 bytes per entry
    add ax, song_table
    mov si, ax
    mov bx, [cs:si]                 ; notes pointer
    mov [cs:cur_notes_ptr], bx
    mov bx, [cs:si + 2]            ; note count
    mov [cs:cur_note_count], bx
    mov bx, [cs:si + 4]            ; title pointer
    mov [cs:cur_title], bx
    mov bx, [cs:si + 6]            ; composer pointer
    mov [cs:cur_composer], bx
    pop si
    pop bx
    pop ax
    ret

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
    cmp dl, ','                     ; < prev song
    je .prev_song
    cmp dl, '.'                     ; > next song
    je .next_song
    ; Number keys 1-5
    cmp dl, '1'
    jb .check_arrows
    cmp dl, '5'
    ja .check_arrows
    sub dl, '1'                     ; 0-4
    mov al, dl
    jmp switch_song
.check_arrows:
    cmp dl, 130                     ; Left arrow (special code)
    je .prev_song
    cmp dl, 131                     ; Right arrow (special code)
    je .next_song
    ret

.esc:
    mov byte [cs:quit], 1
    ret
.toggle:
    jmp toggle_state
.prev_song:
    mov al, [cs:cur_song]
    test al, al
    jz .wrap_last
    dec al
    jmp switch_song
.wrap_last:
    mov al, NUM_SONGS - 1
    jmp switch_song
.next_song:
    mov al, [cs:cur_song]
    inc al
    cmp al, NUM_SONGS
    jb switch_song
    xor al, al
    jmp switch_song

.mouse:
    test dl, 1                      ; Left button?
    jz .btn_up
    cmp byte [cs:prev_btn], 0
    jne .held
    mov byte [cs:prev_btn], 1
    ; Hit test Play/Pause button (center)
    mov bx, BTNC_X
    mov cx, BTN_ROW_Y
    mov dx, BTNC_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .btn_play_hit
    ; Hit test Prev button
    mov bx, BTNP_X
    mov cx, BTN_ROW_Y
    mov dx, BTNP_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .btn_prev_hit
    ; Hit test Next button
    mov bx, BTNN_X
    mov cx, BTN_ROW_Y
    mov dx, BTNN_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .btn_next_hit
    ret
.btn_play_hit:
    jmp toggle_state
.btn_prev_hit:
    jmp .prev_song
.btn_next_hit:
    jmp .next_song
.btn_up:
    mov byte [cs:prev_btn], 0
.held:
    ret

.redraw:
    jmp draw_all

; ============================================================================
; Switch to song AL (0-4)
; ============================================================================
switch_song:
    mov [cs:cur_song], al
    mov ah, API_SPEAKER_OFF
    int 0x80
    mov word [cs:note_idx], 0
    mov byte [cs:state], ST_PAUSED
    call load_song_info
    jmp draw_all

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
    jmp draw_button

; ============================================================================
; Drawing functions
; ============================================================================

; Draw entire window content
draw_all:
    pusha

    ; Clear entire content area
    mov bx, 0
    mov cx, 0
    mov dx, WIN_W - 2
    mov si, WIN_H - 10
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Song title with arrows
    call draw_song_title

    ; Separator line
    mov bx, 0
    mov cx, SEP_Y
    mov dx, WIN_W - 4
    mov al, 3                       ; white
    mov ah, API_HLINE
    int 0x80

    ; Help text
    mov bx, 4
    mov cx, HELP_Y
    mov si, msg_help
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    call draw_staff
    call draw_status
    jmp draw_button

; Draw song title and composer
draw_song_title:
    pusha

    ; Clear title area
    mov bx, 0
    mov cx, 0
    mov dx, WIN_W - 2
    mov si, 21
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw "< " prefix
    mov bx, 4
    mov cx, TITLE_Y
    mov si, str_arrow_l
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw song title
    mov bx, 28
    mov cx, TITLE_Y
    mov si, [cs:cur_title]
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw " >" suffix
    mov bx, 240
    mov cx, TITLE_Y
    mov si, str_arrow_r
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw composer
    mov bx, 28
    mov cx, COMP_Y
    mov si, [cs:cur_composer]
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; Draw status text
draw_status:
    pusha

    ; Clear status area
    mov bx, 0
    mov cx, STATUS_Y
    mov dx, WIN_W - 2
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
    mov bx, 100
    mov cx, STATUS_Y
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; Draw all three buttons: Prev | Play/Pause | Next
draw_button:
    pusha
    mov ax, cs
    mov es, ax

    ; --- Prev button ---
    mov di, btn_prev
    mov bx, BTNP_X
    mov cx, BTN_ROW_Y
    mov dx, BTNP_W
    mov si, BTN_H
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; --- Play/Pause button (center) ---
    cmp byte [cs:state], ST_PLAYING
    je .lbl_pause
    cmp byte [cs:state], ST_DONE
    je .lbl_replay
    mov di, btn_play
    jmp .draw_center
.lbl_pause:
    mov di, btn_pause
    jmp .draw_center
.lbl_replay:
    mov di, btn_replay
.draw_center:
    mov bx, BTNC_X
    mov cx, BTN_ROW_Y
    mov dx, BTNC_W
    mov si, BTN_H
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; --- Next button ---
    mov di, btn_next
    mov bx, BTNN_X
    mov cx, BTN_ROW_Y
    mov dx, BTNN_W
    mov si, BTN_H
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    popa
    ret

; ============================================================================
; Staff visualization
; ============================================================================
draw_staff:
    pusha

    ; Clear staff area
    mov bx, 0
    mov cx, STAFF_Y
    mov dx, WIN_W - 4
    mov si, STAFF_H
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw 5 staff lines (E4, G4, B4, D5, F5 from bottom to top)
    ; E4 = STAFF_BASE_Y, spacing = 8
    mov cx, STAFF_BASE_Y            ; E4 line (bottom)
    mov al, 1                       ; cyan
    mov bx, 4
    mov dx, WIN_W - 8

    mov ah, API_HLINE               ; E4
    int 0x80
    sub cx, STAFF_LINE_SPACING
    mov ah, API_HLINE               ; G4
    int 0x80
    sub cx, STAFF_LINE_SPACING
    mov ah, API_HLINE               ; B4
    int 0x80
    sub cx, STAFF_LINE_SPACING
    mov ah, API_HLINE               ; D5
    int 0x80
    sub cx, STAFF_LINE_SPACING
    mov ah, API_HLINE               ; F5
    int 0x80

    ; Calculate view_start = max(0, note_idx - 10)
    mov ax, [cs:note_idx]
    sub ax, 10
    jns .vs_ok
    xor ax, ax
.vs_ok:
    mov [cs:view_start], ax

    ; Draw visible notes
    ; DI = loop counter (0 to NOTES_VISIBLE-1)
    xor di, di

.note_loop:
    cmp di, NOTES_VISIBLE
    jae .notes_done

    ; note index = view_start + di
    mov ax, [cs:view_start]
    add ax, di
    ; Check if past end of song
    cmp ax, [cs:cur_note_count]
    jae .skip_note

    ; Get note frequency
    push di
    mov si, [cs:cur_notes_ptr]
    mov bx, ax                      ; BX = note index
    push ax
    shl ax, 2                       ; 4 bytes per entry
    add si, ax
    mov bx, [cs:si]                 ; BX = frequency
    pop ax

    ; Skip rests
    test bx, bx
    jz .skip_note_pop
    cmp bx, 0xFFFF
    je .skip_note_pop

    ; Look up Y position from frequency
    call freq_to_y                  ; AL = Y offset from staff area top
    test al, al
    jz .skip_note_pop               ; unknown frequency

    ; Calculate note X position
    push ax
    mov ax, di
    mov bx, NOTE_SPACING
    mul bx                          ; AX = di * NOTE_SPACING
    add ax, STAFF_LEFT_X
    mov bx, ax                      ; BX = X position
    pop ax
    movzx cx, al                    ; CX = Y position

    ; Choose color: current note = magenta(2), others = white(3)
    mov ax, [cs:view_start]
    add ax, di
    cmp ax, [cs:note_idx]
    mov al, 3                       ; white (default)
    jne .draw_note
    mov al, 2                       ; magenta (current)

.draw_note:
    ; Draw note head sprite: BX=X, CX=Y, DL=5, DH=6, AL=color, SI=bitmap
    mov si, note_head_bmp
    mov dh, 6                       ; width
    mov dl, 5                       ; height
    mov ah, API_DRAW_SPRITE
    int 0x80
    pop di
    jmp .next_note

.skip_note_pop:
    pop di
.skip_note:
.next_note:
    inc di
    jmp .note_loop

.notes_done:
    popa
    ret

; ============================================================================
; Frequency to Y position lookup
; Input: BX = frequency (Hz)
; Output: AL = Y position in content area (0 = unknown)
; ============================================================================
freq_to_y:
    push si
    push cx
    mov si, freq_y_table
.lookup:
    mov cx, [cs:si]                 ; frequency
    test cx, cx
    jz .not_found                   ; end of table
    cmp bx, cx
    je .found
    add si, 4                       ; next entry (freq word + y byte + pad)
    jmp .lookup
.found:
    mov al, [cs:si + 2]             ; Y position
    pop cx
    pop si
    ret
.not_found:
    xor al, al
    pop cx
    pop si
    ret

; ============================================================================
; Helpers
; ============================================================================

read_tick:
    mov ah, API_GET_TICK
    int 0x80
    ret

; ============================================================================
; Data
; ============================================================================

win_title:  db 'Music', 0
wh:         db 0
state:      db ST_PAUSED
note_idx:   dw 0
t0:         dw 0
prev_btn:   db 0
quit:       db 0
cur_song:   db 0
view_start: dw 0

; Current song info (loaded from song_table)
cur_notes_ptr:   dw 0
cur_note_count:  dw 0
cur_title:       dw 0
cur_composer:    dw 0

; Strings
str_arrow_l:     db '< ', 0
str_arrow_r:     db ' >', 0
msg_playing:     db 'Playing...', 0
msg_paused:      db 'Paused', 0
msg_finished:    db 'Song Complete', 0
msg_help:        db '1-5:Song  SPC:Play  ESC:Exit', 0

; Button labels
btn_prev:   db ' Prev ', 0
btn_play:   db ' Play ', 0
btn_pause:  db 'Pause ', 0
btn_replay: db 'Replay', 0
btn_next:   db ' Next ', 0

; Note head bitmap (6 wide x 5 tall, 1 byte per row, MSB first)
; .XXXX. / XXXXXX / XXXXXX / XXXXXX / .XXXX.
note_head_bmp: db 0x78, 0xFC, 0xFC, 0xFC, 0x78

; Frequency → Y position table (freq_word, y_byte, pad_byte)
; Staff positions: E4=60, G4=52, B4=44, D5=36, F5=28 (lines)
; Spaces/ledger: C4=68, D4=64, F4=56, A4=48, C5=40, E5=32
; Accidentals: GS4=50, AS4=46, DS5=34
freq_y_table:
    dw NOTE_C4
    db 68, 0                        ; below staff (ledger)
    dw NOTE_D4
    db 64, 0                        ; below staff
    dw NOTE_E4
    db 60, 0                        ; bottom line
    dw NOTE_F4
    db 56, 0                        ; first space
    dw NOTE_G4
    db 52, 0                        ; second line
    dw NOTE_GS4
    db 50, 0                        ; between G4 and A4
    dw NOTE_A4
    db 48, 0                        ; second space
    dw NOTE_AS4
    db 46, 0                        ; between A4 and B4
    dw NOTE_B4
    db 44, 0                        ; middle line
    dw NOTE_C5
    db 40, 0                        ; third space
    dw NOTE_D5
    db 36, 0                        ; fourth line
    dw NOTE_DS5
    db 34, 0                        ; between D5 and E5
    dw NOTE_E5
    db 32, 0                        ; fourth space
    dw NOTE_F5
    db 28, 0                        ; top line
    dw NOTE_G5
    db 26, 0                        ; above staff
    dw 0                            ; end marker

; ============================================================================
; Song table: 5 entries x 8 bytes
; Format: notes_ptr (word), note_count (word), title_ptr (word), composer_ptr (word)
; ============================================================================
song_table:
    dw notes_furelise,   FURELISE_COUNT,  str_t_furelise,  str_c_beethoven
    dw notes_ode,        ODE_COUNT,       str_t_ode,       str_c_beethoven
    dw notes_twinkle,    TWINKLE_COUNT,   str_t_twinkle,   str_c_mozart
    dw notes_lullaby,    LULLABY_COUNT,   str_t_lullaby,   str_c_brahms
    dw notes_canon,      CANON_COUNT,     str_t_canon,     str_c_pachelbel

; Song titles
str_t_furelise:  db 'Fur Elise', 0
str_t_ode:       db 'Ode to Joy', 0
str_t_twinkle:   db 'Twinkle Twinkle', 0
str_t_lullaby:   db "Brahms' Lullaby", 0
str_t_canon:     db 'Canon in D', 0

; Composers
str_c_beethoven: db 'L. van Beethoven', 0
str_c_mozart:    db 'W.A. Mozart', 0
str_c_brahms:    db 'J. Brahms', 0
str_c_pachelbel: db 'J. Pachelbel', 0

; ============================================================================
; Song 1: Fur Elise (Beethoven) - A-B-A form
; ============================================================================
notes_furelise:
    ; === A SECTION (first time) ===
    ; Phrase 1: Trill -> A minor
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_A4,  DUR_QUARTER

    ; Phrase 2: C-E-A -> B
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_QUARTER

    ; Phrase 3: E-G#-B -> C5
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

    ; Phrase 5: C-E-A -> B
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_QUARTER

    ; Phrase 6: E-C5-B -> A (first ending)
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_QUARTER

    ; === B SECTION ===
    ; F major arpeggio
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

    ; Pickup
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH

    ; === A SECTION (final) ===
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_DS5, DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_A4,  DUR_QUARTER

    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_QUARTER

    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_GS4, DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_QUARTER

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

    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_QUARTER

    ; Final resolution
    dw NOTE_REST, DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_HALF

    dw 0xFFFF, 0
FURELISE_COUNT equ ($ - notes_furelise - 4) / 4  ; exclude end marker

; ============================================================================
; Song 2: Ode to Joy (Beethoven, 9th Symphony)
; ============================================================================
notes_ode:
    ; Line 1: E E F G | G F E D
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER

    ; Line 2: C C D E | E. D D
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_E4,  DUR_DOTQ
    dw NOTE_D4,  DUR_EIGHTH
    dw NOTE_D4,  DUR_HALF

    ; Line 3: E E F G | G F E D  (repeat)
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER

    ; Line 4: C C D E | D. C C
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_D4,  DUR_DOTQ
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_C4,  DUR_HALF

    ; Bridge: D D E C | D EF E C | D EF E D | C D G
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_F4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_F4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_G4,  DUR_HALF

    ; Final: E E F G | G F E D | C C D E | D. C C
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_D4,  DUR_DOTQ
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_C4,  DUR_HALF

    dw 0xFFFF, 0
ODE_COUNT equ ($ - notes_ode - 4) / 4

; ============================================================================
; Song 3: Twinkle Twinkle Little Star
; ============================================================================
notes_twinkle:
    ; Verse 1: C C G G A A G-
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_A4,  DUR_QUARTER
    dw NOTE_A4,  DUR_QUARTER
    dw NOTE_G4,  DUR_HALF

    ; F F E E D D C-
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_C4,  DUR_HALF

    ; Verse 2: G G F F E E D-
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_D4,  DUR_HALF

    ; G G F F E E D-
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_D4,  DUR_HALF

    ; Verse 3 (repeat verse 1): C C G G A A G-
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_C4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_A4,  DUR_QUARTER
    dw NOTE_A4,  DUR_QUARTER
    dw NOTE_G4,  DUR_HALF

    ; F F E E D D C-
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_C4,  DUR_HALF

    dw 0xFFFF, 0
TWINKLE_COUNT equ ($ - notes_twinkle - 4) / 4

; ============================================================================
; Song 4: Brahms' Lullaby (Wiegenlied)
; ============================================================================
notes_lullaby:
    ; Phrase 1: E E G. E E G.
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_G4,  DUR_DOTQ
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_G4,  DUR_DOTQ

    ; Phrase 2: E G C5 B A A
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_G4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_QUARTER
    dw NOTE_B4,  DUR_QUARTER
    dw NOTE_A4,  DUR_QUARTER
    dw NOTE_A4,  DUR_QUARTER

    ; Phrase 3: D D B. D D B.
    dw NOTE_D4,  DUR_EIGHTH
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_B4,  DUR_DOTQ
    dw NOTE_D4,  DUR_EIGHTH
    dw NOTE_D4,  DUR_QUARTER
    dw NOTE_B4,  DUR_DOTQ

    ; Phrase 4: D B D5 C5 A F
    dw NOTE_D4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_D5,  DUR_QUARTER
    dw NOTE_C5,  DUR_QUARTER
    dw NOTE_A4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER

    ; Phrase 5: E C5 C5 A. F A G
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_QUARTER
    dw NOTE_A4,  DUR_DOTQ
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_G4,  DUR_HALF

    ; Phrase 6: E C5 C5 B. D5 C5 A F
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_C5,  DUR_QUARTER
    dw NOTE_B4,  DUR_DOTQ
    dw NOTE_D5,  DUR_QUARTER
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_A4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER

    ; Ending: E F G A B C5-
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_A4,  DUR_QUARTER
    dw NOTE_B4,  DUR_QUARTER
    dw NOTE_C5,  DUR_HALF

    dw 0xFFFF, 0
LULLABY_COUNT equ ($ - notes_lullaby - 4) / 4

; ============================================================================
; Song 5: Canon in D (Pachelbel) - melody over the famous progression
; Transposed to C major for available note range
; ============================================================================
notes_canon:
    ; Main melody (the famous descending theme)
    ; Phrase 1: C5 B4 A4 G4
    dw NOTE_C5,  DUR_QUARTER
    dw NOTE_B4,  DUR_QUARTER
    dw NOTE_A4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER

    ; Phrase 2: F4 E4 F4 G4
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER

    ; Variation 1: eighth note arpeggios
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_E5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_D5,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_G4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH

    dw NOTE_F4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_G4,  DUR_EIGHTH
    dw NOTE_F4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_G4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH

    ; Variation 2: ascending scale passages
    dw NOTE_C4,  DUR_EIGHTH
    dw NOTE_D4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_EIGHTH
    dw NOTE_F4,  DUR_EIGHTH
    dw NOTE_G4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH

    dw NOTE_D5,  DUR_QUARTER
    dw NOTE_C5,  DUR_QUARTER
    dw NOTE_B4,  DUR_QUARTER
    dw NOTE_A4,  DUR_QUARTER

    ; Variation 3: the iconic repeated theme
    dw NOTE_G4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_G4,  DUR_EIGHTH
    dw NOTE_F4,  DUR_EIGHTH
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_C5,  DUR_QUARTER

    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_C5,  DUR_EIGHTH
    dw NOTE_B4,  DUR_EIGHTH
    dw NOTE_A4,  DUR_EIGHTH
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_E5,  DUR_QUARTER

    ; Final: descending resolution
    dw NOTE_D5,  DUR_QUARTER
    dw NOTE_C5,  DUR_QUARTER
    dw NOTE_B4,  DUR_QUARTER
    dw NOTE_A4,  DUR_QUARTER
    dw NOTE_G4,  DUR_QUARTER
    dw NOTE_F4,  DUR_QUARTER
    dw NOTE_E4,  DUR_QUARTER
    dw NOTE_C5,  DUR_HALF

    dw 0xFFFF, 0
CANON_COUNT equ ($ - notes_canon - 4) / 4
