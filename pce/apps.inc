; ============================================================================
; UnoDOS / PC Engine — apps (M3), clock/theme/music, AUTOTEST, strings. ca65.
; ============================================================================

draw_app:
    lda v_app
    cmp #0
    bne @n0
    jmp app_sysinfo
@n0: cmp #1
    bne @n1
    jmp app_clock
@n1: cmp #2
    bne @n2
    jmp app_notepad
@n2: cmp #3
    bne @n3
    jmp app_music
@n3: cmp #4
    bne @n4
    jmp app_files
@n4: cmp #5
    bne @n5
    jmp app_theme
@n5: cmp #7
    bne @n7
    jmp app_dostris
@n7: jmp app_generic

app_sysinfo:
    lda #<t_sysinfo
    sta ptr
    lda #>t_sysinfo
    sta ptr+1
    jsr draw_chrome
    lda #<c_sysinfo
    sta ptr2
    lda #>c_sysinfo
    sta ptr2+1
    jmp draw_content
app_notepad:
    lda #<t_notepad
    sta ptr
    lda #>t_notepad
    sta ptr+1
    jsr draw_chrome
    lda #<c_notepad
    sta ptr2
    lda #>c_notepad
    sta ptr2+1
    jmp draw_content
app_files:
    lda #<t_files
    sta ptr
    lda #>t_files
    sta ptr+1
    jsr draw_chrome
    lda #<c_files
    sta ptr2
    lda #>c_files
    sta ptr2+1
    jmp draw_content
app_generic:
    lda v_app
    asl
    tay
    lda icon_lbl,y
    sta ptr
    lda icon_lbl+1,y
    sta ptr+1
    jsr draw_chrome
    lda #<c_generic
    sta ptr2
    lda #>c_generic
    sta ptr2+1
    jmp draw_content
app_dostris:
    lda #<t_dostris
    sta ptr
    lda #>t_dostris
    sta ptr+1
    jsr draw_chrome
    jmp dostris_draw
app_clock:
    lda #<t_clock
    sta ptr
    lda #>t_clock
    sta ptr+1
    jsr draw_chrome
    lda #<c_clock
    sta ptr2
    lda #>c_clock
    sta ptr2+1
    jsr draw_content
    jsr clock_format
    jmp draw_clock_time
app_theme:
    lda #<t_theme
    sta ptr
    lda #>t_theme
    sta ptr+1
    jsr draw_chrome
    lda #<c_theme
    sta ptr2
    lda #>c_theme
    sta ptr2+1
    jsr draw_content
    jmp draw_theme_status
app_music:
    lda #<t_music
    sta ptr
    lda #>t_music
    sta ptr+1
    jsr draw_chrome
    lda #<c_music
    sta ptr2
    lda #>c_music
    sta ptr2+1
    jsr draw_content
    jmp draw_music_status

; draw_chrome: ptr=title -> title bar + footer
draw_chrome:
    lda #0
    sta col
    sta row
    jsr vdc_cell
    ldx #SCRCOLS
@tb: lda #T_WHITE
    jsr putcell
    dex
    bne @tb
    lda #1
    sta col
    lda #0
    sta row
    lda #T_FONTINV
    sta tmp
    jsr puts
    lda #<s_back
    sta ptr
    lda #>s_back
    sta ptr+1
    lda #1
    sta col
    lda #SCRROWS-1
    sta row
    lda #T_FONT
    sta tmp
    jmp puts

; ---- Clock ----
clock_advance:
    inc v_frac
    lda v_frac
    cmp #60
    bcc @done
    lda #0
    sta v_frac
    inc v_ss
    lda v_ss
    cmp #60
    bcc @fmt
    lda #0
    sta v_ss
    inc v_mm
    lda v_mm
    cmp #60
    bcc @fmt
    lda #0
    sta v_mm
    inc v_hh
    lda v_hh
    cmp #24
    bcc @fmt
    lda #0
    sta v_hh
@fmt:
    jsr clock_format
    lda #1
    sta pf_clk
@done:
    rts

clock_format:
    lda v_hh
    jsr two_digits
    stx clk_str+0
    sta clk_str+1
    lda #':'
    sta clk_str+2
    lda v_mm
    jsr two_digits
    stx clk_str+3
    sta clk_str+4
    lda #':'
    sta clk_str+5
    lda v_ss
    jsr two_digits
    stx clk_str+6
    sta clk_str+7
    lda #0
    sta clk_str+8
    rts
; two_digits returns X=tens char, A=units char (defined in kernel)

draw_clock_time:
    lda #<clk_str
    sta ptr
    lda #>clk_str
    sta ptr+1
    lda #12
    sta col
    lda #12
    sta row
    jmp cputs

; ---- Theme ----
theme_input:
    lda v_pade
    and #PAD_A
    beq @done
    inc v_theme
    lda v_theme
    cmp #NTHEMES
    bcc @s
    lda #0
    sta v_theme
@s: lda #1
    sta pf_pal
@done:
    rts
draw_theme_status:
    jsr load_palette
    lda v_theme
    clc
    adc #'0'
    sta numstr
    lda #0
    sta numstr+1
    lda #<numstr
    sta ptr
    lda #>numstr
    sta ptr+1
    lda #10
    sta col
    lda #12
    sta row
    jmp cputs

; ---- Music (PSG channel 0) ----
music_init:
    lda #0
    sta PSG_AR
    lda #$FF
    sta PSG_MAIN
    lda #$00
    sta PSG_CR
    lda #$FF
    sta PSG_BAL
    ldx #16
@hi: lda #$1F
    sta PSG_WAV
    dex
    bne @hi
    ldx #16
@lo: lda #$00
    sta PSG_WAV
    dex
    bne @lo
    lda #0
    sta m_idx
    lda #1
    sta m_play
    jsr music_load
    lda #1
    sta pf_score
    rts
music_silence:
    lda #0
    sta PSG_AR
    sta PSG_CR
    sta m_play
    rts
music_load:
    lda m_idx
    sta g_n
    asl
    clc
    adc g_n              ; m_idx*3
    tay
    lda music_song,y
    sta mlo
    iny
    lda music_song,y
    sta mhi
    iny
    lda music_song,y
    sta m_timer
    lda #0
    sta PSG_AR
    lda mlo
    sta PSG_FL
    lda mhi
    sta PSG_FH
    lda #$9F             ; channel on, volume 31
    sta PSG_CR
    rts
music_tick:
    lda m_play
    beq @done
    dec m_timer
    bne @done
    inc m_idx
    lda m_idx
    cmp #MUSIC_COUNT
    bcc @set
    lda #0
    sta m_idx
@set:
    jsr music_load
    lda #1
    sta pf_score
@done:
    rts
draw_music_status:
    lda m_idx
    clc
    adc #1
    jsr two_digits
    stx numstr
    sta numstr+1
    lda #0
    sta numstr+2
    lda #<numstr
    sta ptr
    lda #>numstr
    sta ptr+1
    lda #10
    sta col
    lda #12
    sta row
    jsr cputs
    lda #6
    sta col
    lda #14
    sta row
    jsr vdc_cell
    lda m_idx
    cmp #16
    bcc @w
    lda #16
@w: tax
    inx
@bar: lda #T_SOLC
    jsr putcell
    dex
    bne @bar
    rts

; ============================================================================
; AUTOTEST
; ============================================================================
.ifdef AUTOTEST
auto_input:
    lda a_tmr
    bne @run
    lda a_idx
    asl
    tay
    lda auto_script,y
    bne @have
    lda #1
    sta a_gpause
    lda v_pad
    sta v_padp
    lda #0
    sta v_pad
    sta v_pade
    rts
@have:
    sta a_tmr
    iny
    lda auto_script,y
    sta a_pad
    inc a_idx
@run:
    dec a_tmr
    lda v_pad
    sta v_padp
    lda a_pad
    sta v_pad
    lda v_padp
    eor #$FF
    and v_pad
    sta v_pade
    rts
auto_script:
.ifdef AT_NAV
    .byte 8,0, 2,PAD_R, 4,0, 2,PAD_R, 4,0, 2,PAD_D, 30,0, 0,0
.endif
.ifdef AT_APP
    .byte 8,0, 2,PAD_A, 40,0, 0,0
.endif
.ifdef AT_CLOCK
    .byte 6,0, 2,PAD_R, 4,0, 2,PAD_A, 200,0, 0,0
.endif
.ifdef AT_THEME
    .byte 6,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R, 4,0
    .byte 2,PAD_A, 8,0, 2,PAD_A, 8,0, 2,PAD_A, 30,0, 0,0
.endif
.ifdef AT_MUSIC
    .byte 6,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R, 4,0, 2,PAD_A, 200,0, 0,0
.endif
.ifdef AT_DOSTRIS
    .byte 6,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_D, 4,0, 2,PAD_A, 6,0
    .byte 2,PAD_L,2,0, 2,PAD_L,2,0, 2,PAD_L,2,0, 2,PAD_L,2,0, 16,PAD_D, 34,0
    .byte 2,PAD_L,2,0, 2,PAD_L,2,0, 16,PAD_D, 34,0
    .byte 2,PAD_A,2,0, 16,PAD_D, 34,0
    .byte 2,PAD_R,2,0, 2,PAD_R,2,0, 16,PAD_D, 34,0
    .byte 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 16,PAD_D, 34,0
    .byte 2,PAD_L,2,0, 6,PAD_D, 2,0, 0,0
.endif
.endif

; ============================================================================
; strings + data
; ============================================================================
s_title:   .byte "UnoDOS 3",0
s_back:    .byte "B = Back",0
icon_lbl:
    .word l_sysinfo, l_clock, l_notepad, l_music, l_files, l_theme
    .word l_tracker, l_dostris, l_outlast, l_pacman, l_paint
l_sysinfo: .byte "SysInfo",0
l_clock:   .byte "Clock",0
l_notepad: .byte "Notepad",0
l_music:   .byte "Music",0
l_files:   .byte "Files",0
l_theme:   .byte "Theme",0
l_tracker: .byte "Tracker",0
l_dostris: .byte "Dostris",0
l_outlast: .byte "OutLast",0
l_pacman:  .byte "Pac-Man",0
l_paint:   .byte "Paint",0
t_sysinfo: .byte "SysInfo",0
t_clock:   .byte "Clock",0
t_notepad: .byte "Notepad",0
t_music:   .byte "Music",0
t_files:   .byte "Files",0
t_theme:   .byte "Theme",0
t_dostris: .byte "Dostris",0
c_sysinfo:
    .byte 2,3
    .word m_si0
    .byte 2,5
    .word m_si1
    .byte 2,6
    .word m_si2
    .byte 2,7
    .word m_si3
    .byte 2,9
    .word m_si4
    .byte $FF
m_si0: .byte "UnoDOS 3 - PC Engine",0
m_si1: .byte "CPU  HuC6280",0
m_si2: .byte "     7.16 MHz",0
m_si3: .byte "VDC  256x224",0
m_si4: .byte "minimal profile",0
c_clock:
    .byte 2,3
    .word m_cl0
    .byte 2,10
    .word m_cl1
    .byte $FF
m_cl0: .byte "System clock",0
m_cl1: .byte "Time:",0
c_notepad:
    .byte 2,3
    .word m_np0
    .byte 2,5
    .word m_np1
    .byte 2,6
    .word m_np2
    .byte $FF
m_np0: .byte "Notepad",0
m_np1: .byte "Sixth fresh port",0
m_np2: .byte "first HuC6280",0
c_files:
    .byte 2,3
    .word m_fi0
    .byte 2,5
    .word m_fi1
    .byte 2,6
    .word m_fi2
    .byte 2,8
    .word m_fi3
    .byte $FF
m_fi0: .byte "Files - apps:",0
m_fi1: .byte "SYSINFO CLOCK NOTEPAD",0
m_fi2: .byte "THEME MUSIC FILES",0
m_fi3: .byte "(no disk - HuCard)",0
c_theme:
    .byte 2,3
    .word m_th0
    .byte 2,5
    .word m_th1
    .byte 2,12
    .word m_th2
    .byte $FF
m_th0: .byte "Theme presets",0
m_th1: .byte "A = cycle palette",0
m_th2: .byte "Preset:",0
c_music:
    .byte 2,3
    .word m_mu0
    .byte 2,5
    .word m_mu1
    .byte 2,12
    .word m_mu2
    .byte $FF
m_mu0: .byte "Music player",0
m_mu1: .byte "Ode to Joy",0
m_mu2: .byte "Note:",0
c_generic:
    .byte 2,3
    .word m_ge0
    .byte 2,5
    .word m_ge1
    .byte $FF
m_ge0: .byte "UnoDOS 3 / PCE",0
m_ge1: .byte "Coming soon.",0
