; ============================================================================
; UnoDOS / WonderSwan — full-screen apps (M3), clock/theme/music, AUTOTEST.
; Included by kernel.asm (same flat ROM segment).
; ============================================================================

; draw_app: chrome (title bar + footer) then dispatch on v_app
draw_app:
    call draw_chrome
    mov ax, [v_app]
    cmp ax, 0
    je app_sysinfo
    cmp ax, 1
    je app_clock
    cmp ax, 2
    je app_notepad
    cmp ax, 3
    je app_files
    cmp ax, 4
    je app_theme
    cmp ax, 5
    je app_music
    cmp ax, 6
    je app_dostris
    jmp app_placeholder

; draw_chrome: white screen, inverted title bar (app name), inverted footer
draw_chrome:
    xor ax, ax
    call fill_map
    mov ch, 0
    mov dl, 1
    call fill_row
    mov bx, [v_app]
    shl bx, 1
    mov si, [cs:app_names+bx]
    mov cl, 1
    mov ch, 0
    mov dl, 1
    call putstr
    mov ch, 17
    mov dl, 1
    call fill_row
    mov cl, 1
    mov ch, 17
    mov dl, 1
    mov si, s_back
    call putstr
    ret

; ---- a small helper: draw a ROM string (SI) at (col CL,row CH) pal0 ---------
line0:
    mov dl, 0
    call putstr
    ret

app_sysinfo:
    mov cl, 2
    mov ch, 3
    mov si, si_l1
    call line0
    mov cl, 2
    mov ch, 5
    mov si, si_l2
    call line0
    mov cl, 2
    mov ch, 7
    mov si, si_l3
    call line0
    mov cl, 2
    mov ch, 9
    mov si, si_l4
    call line0
    mov cl, 2
    mov ch, 11
    mov si, si_l5
    call line0
    ret

app_clock:
    mov cl, 2
    mov ch, 3
    mov si, s_clklbl
    call line0
    call draw_clock_time
    ret

; draw_clock_time: HH:MM:SS at row 6, col 9 (pal0)
draw_clock_time:
    mov cl, 9
    mov ch, 6
    mov ax, [v_hh]
    call put2
    mov al, ':'
    call putc0
    mov ax, [v_mm]
    call put2
    mov al, ':'
    call putc0
    mov ax, [v_ss]
    call put2
    ret

app_notepad:
    mov cl, 2
    mov ch, 3
    mov si, np_l1
    call line0
    mov cl, 2
    mov ch, 5
    mov si, np_l2
    call line0
    mov cl, 2
    mov ch, 7
    mov si, np_l3
    call line0
    ret

app_files:
    mov cl, 2
    mov ch, 3
    mov si, fs_l1
    call line0
    mov cl, 3
    mov ch, 5
    mov si, fs_f1
    call line0
    mov cl, 3
    mov ch, 6
    mov si, fs_f2
    call line0
    mov cl, 3
    mov ch, 7
    mov si, fs_f3
    call line0
    ret

app_theme:
    mov cl, 2
    mov ch, 3
    mov si, th_l1
    call line0
    call draw_theme_status
    ret

draw_theme_status:
    mov cl, 2
    mov ch, 6
    mov si, th_lbl
    call line0               ; "Theme " (advances CL)
    mov ax, [v_theme]
    inc ax
    call put2
    ret

; theme_input: Up/Down cycles the theme -> rewrite the shade pool (recolours all)
theme_input:
    mov ax, [v_pade]
    test ax, PAD_U
    jnz .next
    test ax, PAD_D
    jnz .next
    ret
.next:
    mov ax, [v_theme]
    inc ax
    cmp ax, NTHEMES
    jb .s
    xor ax, ax
.s:
    mov [v_theme], ax
    call set_pool
    mov word [v_dirty], 1    ; refresh the "Theme n" readout
    ret

app_music:
    mov cl, 2
    mov ch, 3
    mov si, mu_l1
    call line0
    call draw_music_status
    ret

draw_music_status:
    mov cl, 2
    mov ch, 6
    mov si, mu_note
    call line0               ; "Note " (advances CL)
    mov ax, [m_idx]
    inc ax
    call put2
    mov al, '/'
    call putc0
    mov ax, MUSIC_COUNT
    call put2
    ret

; music_init: build a square wavetable, program channel 1, start the tune
music_init:
    mov di, WAVE             ; 32 4-bit samples = 16 bytes: half F, half 0
    mov cx, 4
    mov ax, 0xFFFF
    rep stosw
    mov cx, 4
    xor ax, ax
    rep stosw
    mov al, WAVE>>6          ; wavetable base register
    out SND_WAVE, al
    mov al, 0xFF             ; ch1 volume L/R = max
    out SND_CH1VOL, al
    mov al, 0x01             ; enable ch1
    out SND_CTRL, al
    mov al, 0x0F             ; output on, full volume
    out SND_OUT, al
    mov word [m_idx], 0
    mov word [m_play], 1
    call music_load
    ret

; music_load: program ch1 period + note timer from music_song[m_idx]
music_load:
    mov bx, [m_idx]
    shl bx, 2               ; 4 bytes per note
    mov ax, [cs:music_song+bx]
    out SND_CH1FL, al
    mov al, ah
    out SND_CH1FH, al
    mov al, [cs:music_song+bx+2]
    xor ah, ah
    mov [m_timer], ax
    ret

music_tick:
    mov ax, [m_play]
    test ax, ax
    jz .done
    dec word [m_timer]
    jnz .done
    inc word [m_idx]
    mov ax, [m_idx]
    cmp ax, MUSIC_COUNT
    jb .next
    mov word [m_idx], 0
.next:
    call music_load
    mov word [pf_score], 1
.done:
    ret

music_silence:
    mov word [m_play], 0
    xor al, al
    out SND_CTRL, al
    out SND_OUT, al
    ret

app_placeholder:
    mov cl, 6
    mov ch, 8
    mov si, s_soon
    call line0
    ret

; ============================================================================
; AUTOTEST — a scripted keypad table drives the same input path. The script end
; sets a_gpause so Dostris gravity freezes for a stable screenshot. Each entry is
; (pad, frames); the sentinel frames=0xFFFF ends the script.
; ============================================================================
%if AUTOTEST
auto_input:
    mov ax, [a_tmr]
    test ax, ax
    jnz .have
    mov bx, [a_idx]
    shl bx, 2
    mov ax, [cs:auto_script+bx+2]    ; frames
    cmp ax, 0xFFFF
    je .end
    mov [a_tmr], ax
    mov ax, [cs:auto_script+bx]      ; pad
    mov [a_pad], ax
    inc word [a_idx]
.have:
    dec word [a_tmr]
    mov ax, [v_pad]
    mov [v_padp], ax
    mov ax, [a_pad]
    mov [v_pad], ax
    mov cx, [v_padp]
    not cx
    and ax, cx
    mov [v_pade], ax
    ret
.end:
    mov word [a_gpause], 1
    xor ax, ax
    mov [a_pad], ax
    mov [v_padp], ax
    mov [v_pad], ax
    mov [v_pade], ax
    ret

auto_script:
%if SCENE == 1                       ; nav: Down x3
    dw 0,10
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,30
%elif SCENE == 2                     ; app: launch SysInfo (sel 0)
    dw 0,10
    dw PAD_A,1
    dw 0,40
%elif SCENE == 3                     ; clock: Down->Clock, A, let it tick
    dw 0,8
    dw PAD_D,1
    dw 0,8
    dw PAD_A,1
    dw 0,220
%elif SCENE == 4                     ; theme: Down x4 -> Theme, A, Up x2 cycles
    dw 0,8
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,8
    dw PAD_A,1
    dw 0,8
    dw PAD_U,1
    dw 0,8
    dw PAD_U,1
    dw 0,30
%elif SCENE == 5                     ; music: Down x5 -> Music, A
    dw 0,8
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,8
    dw PAD_A,1
    dw 0,80
%elif SCENE == 6                     ; dostris: Down x6 -> Dostris, A, a few moves
    dw 0,8
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,6
    dw PAD_D,1
    dw 0,8
    dw PAD_A,1
    dw 0,24
    dw PAD_L,1
    dw 0,8
    dw PAD_U,1
    dw 0,8
    dw PAD_R,1
    dw 0,24
%else
    dw 0,30
%endif
    dw 0,0xFFFF                      ; end sentinel
%endif

; ============================================================================
; strings + tables (ROM)
; ============================================================================
app_names:
    dw s_sysinfo, s_clock, s_notepad, s_files, s_theme, s_music
    dw s_dostris, s_tracker, s_outlast, s_pacman, s_paint

s_title    db "UnoDOS 3",0
s_back     db "B=Back",0
s_soon     db "Coming soon",0

s_sysinfo  db "SysInfo",0
s_clock    db "Clock",0
s_notepad  db "Notepad",0
s_files    db "Files",0
s_theme    db "Theme",0
s_music    db "Music",0
s_dostris  db "Dostris",0
s_tracker  db "Tracker",0
s_outlast  db "OutLast",0
s_pacman   db "Pac-Man",0
s_paint    db "Paint",0

si_l1      db "WonderSwan",0
si_l2      db "NEC V30MZ x86",0
si_l3      db "224x144 mono",0
si_l4      db "16 KB RAM",0
si_l5      db "UnoDOS 3.1",0

s_clklbl   db "System clock",0

np_l1      db "Notepad",0
np_l2      db "UnoDOS on the",0
np_l3      db "WonderSwan!",0

fs_l1      db "Files - A:",0
fs_f1      db "CLOCK   BIN",0
fs_f2      db "NOTEPAD BIN",0
fs_f3      db "DOSTRIS BIN",0

th_l1      db "Up/Down: theme",0
th_lbl     db "Theme ",0

mu_l1      db "WonderSwan PSG",0
mu_note    db "Note ",0
