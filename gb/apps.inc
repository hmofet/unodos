; ============================================================================
; UnoDOS / Game Boy — full-screen apps (M3), the clock/theme/music engines, the
; AUTOTEST input script, and shared strings/data. Included by kernel.asm.
; Screen is 20x18 cells, so content strings are kept short.
; ============================================================================

SECTION "apps", ROM0

; ============================================================================
; draw_app: dispatch the resident app's full-screen content (LCD off)
; ============================================================================
draw_app:
    ld a, [v_app]
    cp 0
    jr nz, .n0
    jp app_sysinfo
.n0:
    cp 1
    jr nz, .n1
    jp app_clock
.n1:
    cp 2
    jr nz, .n2
    jp app_notepad
.n2:
    cp 3
    jr nz, .n3
    jp app_music
.n3:
    cp 4
    jr nz, .n4
    jp app_files
.n4:
    cp 5
    jr nz, .n5
    jp app_theme
.n5:
    cp 7
    jr nz, .n7
    jp app_dostris
.n7:
    jp app_generic

app_sysinfo:
    ld de, t_sysinfo
    call draw_chrome
    ld hl, c_sysinfo
    jp draw_content

app_notepad:
    ld de, t_notepad
    call draw_chrome
    ld hl, c_notepad
    jp draw_content

app_files:
    ld de, t_files
    call draw_chrome
    ld hl, c_files
    jp draw_content

app_generic:
    ; title = the launched icon's own label
    ld a, [v_app]
    add a, a
    ld e, a
    ld d, 0
    ld hl, icon_lbl
    add hl, de
    ld a, [hl+]
    ld e, a
    ld a, [hl]
    ld d, a
    call draw_chrome
    ld hl, c_generic
    jp draw_content

app_dostris:
    ld de, t_dostris
    call draw_chrome
    jp dostris_draw

; ---- Clock ----------------------------------------------------------------
app_clock:
    ld de, t_clock
    call draw_chrome
    ld hl, c_clock
    call draw_content
    call clock_format
    jp draw_clock_time

; clock_advance: tick the global clock; format + flag when the second changes
clock_advance:
    ld hl, v_frac
    inc [hl]
    ld a, [hl]
    cp 60
    jr c, .done
    xor a
    ld [v_frac], a
    ld hl, v_ss
    inc [hl]
    ld a, [hl]
    cp 60
    jr c, .fmt
    xor a
    ld [v_ss], a
    ld hl, v_mm
    inc [hl]
    ld a, [hl]
    cp 60
    jr c, .fmt
    xor a
    ld [v_mm], a
    ld hl, v_hh
    inc [hl]
    ld a, [hl]
    cp 24
    jr c, .fmt
    xor a
    ld [v_hh], a
.fmt:
    call clock_format
    ld a, 1
    ld [pf_clk], a
.done:
    ret

clock_format:
    ; two_digits -> D = tens char, A = units char; store A (units) before D
    ld a, [v_hh]
    call two_digits
    ld [clk_str+1], a
    ld a, d
    ld [clk_str+0], a
    ld a, $3A
    ld [clk_str+2], a
    ld a, [v_mm]
    call two_digits
    ld [clk_str+4], a
    ld a, d
    ld [clk_str+3], a
    ld a, $3A
    ld [clk_str+5], a
    ld a, [v_ss]
    call two_digits
    ld [clk_str+7], a
    ld a, d
    ld [clk_str+6], a
    xor a
    ld [clk_str+8], a
    ret

draw_clock_time:
    ld a, 6
    ld [col], a
    ld a, 8
    ld [row], a
    call xy_to_hl
    ld de, clk_str
    ld b, T_FONT
    jp wstr

; ---- Theme (palette cycling) ----------------------------------------------
app_theme:
    ld de, t_theme
    call draw_chrome
    ld hl, c_theme
    call draw_content
    jp draw_theme_status

theme_input:
    ld a, [v_pade]
    and PAD_A
    ret z
    ld a, [v_theme]
    inc a
    cp NTHEMES
    jr c, .s
    xor a
.s:
    ld [v_theme], a
    ld a, 1
    ld [pf_pal], a
    ret

draw_theme_status:
    call setup_palettes                     ; apply the current preset
    ld a, [v_theme]
    add a, $30
    ld [numstr], a
    xor a
    ld [numstr+1], a
    ld a, 8
    ld [col], a
    ld a, 8
    ld [row], a
    call xy_to_hl
    ld de, numstr
    ld b, T_FONT
    jp wstr

; ---- Music (GB APU pulse channel 1) ---------------------------------------
app_music:
    ld de, t_music
    call draw_chrome
    ld hl, c_music
    call draw_content
    jp draw_music_status

music_init:
    ld a, $80
    ld [rNR52], a                           ; APU power on
    ld a, $FF
    ld [rNR51], a                           ; all channels both sides
    ld a, $77
    ld [rNR50], a                           ; max volume
    xor a
    ld [m_idx], a
    ld a, 1
    ld [m_play], a
    call music_load
    ld a, 1
    ld [pf_score], a
    ret

music_silence:
    xor a
    ld [rNR52], a                           ; APU off -> silence
    ld [m_play], a
    ret

music_load:
    ld a, [m_idx]
    ld b, a
    add a, a
    add a, b                                ; m_idx*3
    ld e, a
    ld d, 0
    ld hl, music_song
    add hl, de
    ld a, [hl+]
    ld c, a                                 ; freq lo
    ld a, [hl+]
    ld b, a                                 ; freq hi
    ld a, [hl]
    ld [m_timer], a                         ; frames
    ld a, $80
    ld [rNR11], a                           ; duty 50%
    ld a, $F0
    ld [rNR12], a                           ; envelope: vol 15, no decay
    ld a, c
    ld [rNR13], a                           ; freq lo
    ld a, b
    or $80                                  ; trigger
    ld [rNR14], a
    ret

music_tick:
    ld a, [m_play]
    and a
    ret z
    ld hl, m_timer
    dec [hl]
    ret nz
    ld a, [m_idx]
    inc a
    cp MUSIC_COUNT
    jr c, .set
    xor a
.set:
    ld [m_idx], a
    call music_load
    ld a, 1
    ld [pf_score], a
    ret

draw_music_status:
    ld a, [m_idx]
    inc a
    call two_digits                         ; D = tens, A = units
    ld [numstr+1], a
    ld a, d
    ld [numstr], a
    xor a
    ld [numstr+2], a
    ld a, 7
    ld [col], a
    ld a, 8
    ld [row], a
    call xy_to_hl
    ld de, numstr
    ld b, T_FONT
    call wstr
    ; progress bar at (2,10): m_idx cells (cap 16, min 1)
    ld a, 2
    ld [col], a
    ld a, 10
    ld [row], a
    call xy_to_hl
    ld a, [m_idx]
    cp 16
    jr c, .w
    ld a, 16
.w:
    inc a
    ld b, a
.bar:
    ld a, T_SOLC
    ld [hl+], a
    dec b
    jr nz, .bar
    ret

; ============================================================================
; AUTOTEST: drive a scripted pad into the same input path (one screenshot
; proves nav / launch / an app). Each step = db frames, pad. frames=0 ends.
; ============================================================================
  IF AUTOTEST
auto_input:
    ld a, [a_tmr]
    and a
    jr nz, .run
    ld a, [a_idx]
    add a, a
    ld e, a
    ld d, 0
    ld hl, auto_script
    add hl, de
    ld a, [hl]
    and a
    jr nz, .have
    ; script done: idle the pad + freeze Dostris gravity
    ld a, 1
    ld [a_gpause], a
    ld a, [v_pad]
    ld [v_padp], a
    xor a
    ld [v_pad], a
    ld [v_pade], a
    ret
.have:
    ld [a_tmr], a
    inc hl
    ld a, [hl]
    ld [a_pad], a
    ld hl, a_idx
    inc [hl]
.run:
    ld hl, a_tmr
    dec [hl]
    ld a, [v_pad]
    ld [v_padp], a
    ld a, [a_pad]
    ld [v_pad], a
    ld a, [v_padp]
    cpl
    ld c, a
    ld a, [v_pad]
    and c
    ld [v_pade], a
    ret

auto_script:
  IF AT_NAV
    ; move the selection down the list, ending on Files (sel 4)
    db 8, 0
    db 2, PAD_D
    db 4, 0
    db 2, PAD_D
    db 4, 0
    db 2, PAD_D
    db 4, 0
    db 2, PAD_D
    db 30, 0
    db 0, 0
  ENDC
  IF AT_APP
    ; launch SysInfo (sel 0)
    db 8, 0
    db 2, PAD_A
    db 40, 0
    db 0, 0
  ENDC
  IF AT_CLOCK
    ; down to Clock (sel 1), launch, let ~3 s tick by
    db 6, 0
    db 2, PAD_D
    db 4, 0
    db 2, PAD_A
    db 200, 0
    db 0, 0
  ENDC
  IF AT_THEME
    ; down to Theme (sel 5), launch, cycle the palette twice
    db 6, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 4, 0
    db 2, PAD_A
    db 8, 0
    db 2, PAD_A
    db 8, 0
    db 2, PAD_A
    db 30, 0
    db 0, 0
  ENDC
  IF AT_MUSIC
    ; down to Music (sel 3), launch, let several notes play
    db 6, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 4, 0
    db 2, PAD_A
    db 200, 0
    db 0, 0
  ENDC
  IF AT_DOSTRIS
    ; down to Dostris (sel 7), launch, then spread a few pieces
    db 6, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 2, 0
    db 2, PAD_D
    db 4, 0
    db 2, PAD_A
    db 6, 0
    db 2, PAD_L
    db 2, 0
    db 2, PAD_L
    db 2, 0
    db 2, PAD_L
    db 2, 0
    db 2, PAD_L
    db 2, 0
    db 16, PAD_D
    db 34, 0
    db 2, PAD_L
    db 2, 0
    db 2, PAD_L
    db 2, 0
    db 16, PAD_D
    db 34, 0
    db 2, PAD_A
    db 2, 0
    db 16, PAD_D
    db 34, 0
    db 2, PAD_R
    db 2, 0
    db 2, PAD_R
    db 2, 0
    db 16, PAD_D
    db 34, 0
    db 2, PAD_R
    db 2, 0
    db 2, PAD_R
    db 2, 0
    db 2, PAD_R
    db 2, 0
    db 16, PAD_D
    db 34, 0
    db 2, PAD_L
    db 2, 0
    db 6, PAD_D
    db 2, 0
    db 0, 0
  ENDC
  ENDC

; ============================================================================
; strings + data
; ============================================================================
s_title:   db "UnoDOS 3", 0
s_back:    db "B=Back", 0

icon_lbl:
    dw l_sysinfo, l_clock, l_notepad, l_music, l_files, l_theme
    dw l_tracker, l_dostris, l_outlast, l_pacman, l_paint
l_sysinfo: db "SysInfo", 0
l_clock:   db "Clock", 0
l_notepad: db "Notepad", 0
l_music:   db "Music", 0
l_files:   db "Files", 0
l_theme:   db "Theme", 0
l_tracker: db "Tracker", 0
l_dostris: db "Dostris", 0
l_outlast: db "OutLast", 0
l_pacman:  db "Pac-Man", 0
l_paint:   db "Paint", 0

t_sysinfo: db "SysInfo", 0
t_clock:   db "Clock", 0
t_notepad: db "Notepad", 0
t_music:   db "Music", 0
t_files:   db "Files", 0
t_theme:   db "Theme", 0
t_dostris: db "Dostris", 0

; ---- app content tables: per line {db col,row} then {dw str}; end col=$FF ----
c_sysinfo:
    db 2, 2
    dw m_si0
    db 2, 4
    dw m_si1
    db 2, 5
    dw m_si2
    db 2, 6
    dw m_si3
    db 2, 8
    dw m_si4
    db $FF
m_si0: db "UnoDOS 3 GB", 0
m_si1: db "CPU  SM83", 0
m_si2: db "LCD  160x144", 0
m_si3: db "RAM  8 KB", 0
m_si4: db "minimal", 0

c_clock:
    db 2, 2
    dw m_cl0
    db 2, 8
    dw m_cl1
    db $FF
m_cl0: db "System clock", 0
m_cl1: db "Time:", 0

c_notepad:
    db 2, 2
    dw m_np0
    db 2, 4
    dw m_np1
    db 2, 5
    dw m_np2
    db $FF
m_np0: db "Notepad", 0
m_np1: db "Third port", 0
m_np2: db "first gbz80", 0

c_files:
    db 2, 2
    dw m_fi0
    db 4, 4
    dw m_fi1
    db 4, 5
    dw m_fi2
    db 4, 6
    dw m_fi3
    db 2, 8
    dw m_fi4
    db $FF
m_fi0: db "Files:", 0
m_fi1: db "SYSINFO", 0
m_fi2: db "CLOCK", 0
m_fi3: db "THEME MUSIC", 0
m_fi4: db "(ROM only)", 0

c_theme:
    db 2, 2
    dw m_th0
    db 2, 4
    dw m_th1
    db 2, 8
    dw m_th2
    db $FF
m_th0: db "Theme presets", 0
m_th1: db "A = cycle", 0
m_th2: db "Preset:", 0

c_music:
    db 2, 2
    dw m_mu0
    db 2, 4
    dw m_mu1
    db 2, 8
    dw m_mu2
    db $FF
m_mu0: db "Music player", 0
m_mu1: db "Ode to Joy", 0
m_mu2: db "Note:", 0

c_generic:
    db 2, 2
    dw m_ge0
    db 2, 4
    dw m_ge1
    db $FF
m_ge0: db "UnoDOS 3 / GB", 0
m_ge1: db "Coming soon.", 0
