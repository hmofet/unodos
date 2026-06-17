; ============================================================================
; UnoDOS / VIC-20 — apps (M3), clock/theme/music, AUTOTEST, strings. dasm/6502.
; ============================================================================

draw_app:
        lda v_app
        cmp #0
        bne da1
        jmp app_sysinfo
da1:    cmp #1
        bne da2
        jmp app_clock
da2:    cmp #2
        bne da3
        jmp app_notepad
da3:    cmp #3
        bne da4
        jmp app_music
da4:    cmp #4
        bne da5
        jmp app_files
da5:    cmp #5
        bne da6
        jmp app_theme
da6:    cmp #7
        bne da7
        jmp app_dostris
da7:    jmp app_generic

; chrome: title bar (row 0 inverted) + title (strptr already set) + footer
draw_chrome:
        jsr clear_screen
        ; save title ptr
        lda strptr
        pha
        lda strptr+1
        pha
        lda #0
        sta col
        sta row
        jsr cellptr
        ldy #0
.cctb:  lda #T_FONTINV
        sta (scrptr),y
        lda #1
        sta (colptr),y
        iny
        cpy #SCRCOLS
        bne .cctb
        pla
        sta strptr+1
        pla
        sta strptr
        lda #1
        sta col
        lda #0
        sta row
        lda #T_FONTINV
        sta tmp
        lda #1
        sta pcol
        jsr puts
        ; footer
        lda #<s_back
        sta strptr
        lda #>s_back
        sta strptr+1
        lda #1
        sta col
        lda #SCRROWS-1
        sta row
        lda #T_FONT
        sta tmp
        lda #1
        sta pcol
        jmp puts

app_sysinfo:
        lda #<t_sysinfo
        sta strptr
        lda #>t_sysinfo
        sta strptr+1
        jsr draw_chrome
        lda #<c_sysinfo
        sta dcptr
        lda #>c_sysinfo
        sta dcptr+1
        jmp draw_content
app_notepad:
        lda #<t_notepad
        sta strptr
        lda #>t_notepad
        sta strptr+1
        jsr draw_chrome
        lda #<c_notepad
        sta dcptr
        lda #>c_notepad
        sta dcptr+1
        jmp draw_content
app_files:
        lda #<t_files
        sta strptr
        lda #>t_files
        sta strptr+1
        jsr draw_chrome
        lda #<c_files
        sta dcptr
        lda #>c_files
        sta dcptr+1
        jmp draw_content
app_generic:
        lda v_app
        asl
        tay
        lda icon_lbl,y
        sta strptr
        lda icon_lbl+1,y
        sta strptr+1
        jsr draw_chrome
        lda #<c_generic
        sta dcptr
        lda #>c_generic
        sta dcptr+1
        jmp draw_content
app_dostris:
        lda #<t_dostris
        sta strptr
        lda #>t_dostris
        sta strptr+1
        jsr draw_chrome
        jmp dostris_draw
app_clock:
        lda #<t_clock
        sta strptr
        lda #>t_clock
        sta strptr+1
        jsr draw_chrome
        lda #<c_clock
        sta dcptr
        lda #>c_clock
        sta dcptr+1
        jsr draw_content
        jsr clock_format
        jmp draw_clock_time
app_theme:
        lda #<t_theme
        sta strptr
        lda #>t_theme
        sta strptr+1
        jsr draw_chrome
        lda #<c_theme
        sta dcptr
        lda #>c_theme
        sta dcptr+1
        jsr draw_content
        jmp draw_theme_status
app_music:
        lda #<t_music
        sta strptr
        lda #>t_music
        sta strptr+1
        jsr draw_chrome
        lda #<c_music
        sta dcptr
        lda #>c_music
        sta dcptr+1
        jsr draw_content
        jmp draw_music_status

clock_format:
        lda v_hh
        jsr two_digits
        stx clk_str+0
        sta clk_str+1
        lda #$3A
        sta clk_str+2
        lda v_mm
        jsr two_digits
        stx clk_str+3
        sta clk_str+4
        lda #$3A
        sta clk_str+5
        lda v_ss
        jsr two_digits
        stx clk_str+6
        sta clk_str+7
        lda #0
        sta clk_str+8
        rts
draw_clock_time:
        lda #<clk_str
        sta strptr
        lda #>clk_str
        sta strptr+1
        lda #2
        sta col
        lda #10
        sta row
        lda #T_FONT
        sta tmp
        lda #1
        sta pcol
        jmp puts

; ---- Theme: Up cycles the background colour ($900F) ----
theme_input:
        lda v_pade
        and #PAD_U
        beq ti_done
        inc v_theme
        lda v_theme
        cmp #NTHEMES
        bcc ti_s
        lda #0
        sta v_theme
ti_s:   lda #1
        sta v_dirty            ; full redraw with the new background
ti_done:
        rts
draw_theme_status:
        ; apply background colour
        ldx v_theme
        lda theme_bg,x
        sta $900F
        ; show preset digit
        lda v_theme
        clc
        adc #$30
        sta numstr
        lda #0
        sta numstr+1
        lda #<numstr
        sta strptr
        lda #>numstr
        sta strptr+1
        lda #2
        sta col
        lda #10
        sta row
        lda #T_FONT
        sta tmp
        lda #1
        sta pcol
        jmp puts
theme_bg:
        dc.b $66, $55, $22, $00   ; blue, green, red, black backgrounds

; ---- Music (VIC alto oscillator $900B) ----
music_init:
        lda #15
        sta $900E              ; volume
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
        sta $900B
        sta $900E
        sta m_play
        rts
music_load:
        lda m_idx
        asl                    ; *2 (db value, frames)
        tay
        lda music_song,y
        sta $900B              ; oscillator value
        lda music_song+1,y
        sta m_timer
        lda #15
        sta $900E
        rts
music_tick:
        lda m_play
        beq mt_done
        dec m_timer
        bne mt_done
        inc m_idx
        lda m_idx
        cmp #MUSIC_COUNT
        bcc mt_set
        lda #0
        sta m_idx
mt_set: jsr music_load
        lda #1
        sta pf_score
mt_done:
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
        sta strptr
        lda #>numstr
        sta strptr+1
        lda #7
        sta col
        lda #6
        sta row
        lda #T_FONT
        sta tmp
        lda #1
        sta pcol
        jsr puts
        ; progress bar at (2,8): m_idx cyan blocks (cap 16)
        lda #2
        sta col
        lda #8
        sta row
        jsr cellptr
        lda m_idx
        cmp #16
        bcc mb
        lda #16
mb:     tay
        iny
        ldx #0
mbl:    lda #T_BLOCK
        sta (scrptr),y
        lda #3
        sta (colptr),y
        dey
        bpl mbl
        rts

; ============================================================================
; AUTOTEST
; ============================================================================
  IF AUTOTEST
auto_input:
        lda a_tmr
        bne ai_run
        lda a_idx
        asl
        tay
        lda auto_script,y
        bne ai_have
        lda #1
        sta a_gpause
        lda v_pad
        sta v_padp
        lda #0
        sta v_pad
        sta v_pade
        rts
ai_have:
        sta a_tmr
        iny
        lda auto_script,y
        sta a_pad
        inc a_idx
ai_run:
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
  IF AT_NAV
        dc.b 8,0, 2,PAD_D, 4,0, 2,PAD_D, 4,0, 2,PAD_D, 4,0, 2,PAD_D, 30,0, 0,0
  ENDIF
  IF AT_APP
        dc.b 8,0, 2,PAD_FIRE, 40,0, 0,0
  ENDIF
  IF AT_CLOCK
        dc.b 6,0, 2,PAD_D, 4,0, 2,PAD_FIRE, 200,0, 0,0
  ENDIF
  IF AT_THEME
        dc.b 6,0, 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D, 4,0
        dc.b 2,PAD_FIRE, 8,0, 2,PAD_U, 8,0, 2,PAD_U, 30,0, 0,0
  ENDIF
  IF AT_MUSIC
        dc.b 6,0, 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D, 4,0, 2,PAD_FIRE, 200,0, 0,0
  ENDIF
  IF AT_DOSTRIS
        dc.b 6,0, 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D, 4,0, 2,PAD_FIRE, 6,0
        dc.b 2,PAD_L,2,0, 2,PAD_L,2,0, 2,PAD_L,2,0, 16,PAD_D, 34,0
        dc.b 2,PAD_L,2,0, 2,PAD_L,2,0, 16,PAD_D, 34,0
        dc.b 2,PAD_U,2,0, 16,PAD_D, 34,0
        dc.b 2,PAD_R,2,0, 2,PAD_R,2,0, 16,PAD_D, 34,0
        dc.b 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 16,PAD_D, 34,0
        dc.b 2,PAD_L,2,0, 6,PAD_D, 2,0, 0,0
  ENDIF
  ENDIF

; ============================================================================
; strings + data
; ============================================================================
s_title:   dc.b "UnoDOS 3",0
s_back:    dc.b "FIRE=Back",0
icon_lbl:
        dc.w l_sysinfo, l_clock, l_notepad, l_music, l_files, l_theme
        dc.w l_tracker, l_dostris, l_outlast, l_pacman, l_paint
l_sysinfo: dc.b "SysInfo",0
l_clock:   dc.b "Clock",0
l_notepad: dc.b "Notepad",0
l_music:   dc.b "Music",0
l_files:   dc.b "Files",0
l_theme:   dc.b "Theme",0
l_tracker: dc.b "Tracker",0
l_dostris: dc.b "Dostris",0
l_outlast: dc.b "OutLast",0
l_pacman:  dc.b "Pac-Man",0
l_paint:   dc.b "Paint",0
t_sysinfo: dc.b "SysInfo",0
t_clock:   dc.b "Clock",0
t_notepad: dc.b "Notepad",0
t_music:   dc.b "Music",0
t_files:   dc.b "Files",0
t_theme:   dc.b "Theme",0
t_dostris: dc.b "Dostris",0
c_sysinfo:
        dc.b 2,2
        dc.w m_si0
        dc.b 2,4
        dc.w m_si1
        dc.b 2,5
        dc.w m_si2
        dc.b 2,7
        dc.w m_si3
        dc.b $FF
m_si0: dc.b "UnoDOS 3 VIC-20",0
m_si1: dc.b "CPU 6502",0
m_si2: dc.b "VIC 22x23",0
m_si3: dc.b "minimal",0
c_clock:
        dc.b 2,2
        dc.w m_cl0
        dc.b 2,8
        dc.w m_cl1
        dc.b $FF
m_cl0: dc.b "System clock",0
m_cl1: dc.b "Time:",0
c_notepad:
        dc.b 2,2
        dc.w m_np0
        dc.b 2,4
        dc.w m_np1
        dc.b $FF
m_np0: dc.b "Seventh port",0
m_np1: dc.b "6502 again",0
c_files:
        dc.b 2,2
        dc.w m_fi0
        dc.b 2,4
        dc.w m_fi1
        dc.b 2,5
        dc.w m_fi2
        dc.b $FF
m_fi0: dc.b "Files:",0
m_fi1: dc.b "SYSINFO CLOCK",0
m_fi2: dc.b "(no disk)",0
c_theme:
        dc.b 2,2
        dc.w m_th0
        dc.b 2,4
        dc.w m_th1
        dc.b 2,8
        dc.w m_th2
        dc.b $FF
m_th0: dc.b "Theme presets",0
m_th1: dc.b "UP=cycle",0
m_th2: dc.b "Preset:",0
c_music:
        dc.b 2,2
        dc.w m_mu0
        dc.b 2,4
        dc.w m_mu1
        dc.b 2,6
        dc.w m_mu2
        dc.b $FF
m_mu0: dc.b "Music player",0
m_mu1: dc.b "Ode to Joy",0
m_mu2: dc.b "Note:",0
c_generic:
        dc.b 2,2
        dc.w m_ge0
        dc.b 2,4
        dc.w m_ge1
        dc.b $FF
m_ge0: dc.b "UnoDOS 3 VIC",0
m_ge1: dc.b "Coming soon",0
