; ============================================================================
; UnoDOS / NES — full-screen apps (M3), the clock/theme/music engines, the
; AUTOTEST input script, and all shared strings/data. Included by kernel.s
; (dasm resolves labels across the whole assembly).
; ============================================================================

; ============================================================================
; draw_app: dispatch the resident app's full-screen content (rendering off)
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
da6:    cmp #6
        bne da7
        jmp app_tracker
da7:    cmp #7
        bne da8
        jmp app_dostris
da8:    cmp #8
        bne da9
        jmp app_outlast
da9:    cmp #9
        bne da10
        jmp app_pacman
da10:   cmp #10
        bne da11
        jmp app_paint
da11:   jmp app_generic

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
        ; title = the launched icon's own label
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

; ---- Clock ----------------------------------------------------------------
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

; clock_advance: tick the global clock; format + flag when the second changes
clock_advance:
        inc v_frac
        lda v_frac
        cmp #60
        bcc ca_done
        lda #0
        sta v_frac
        inc v_ss
        lda v_ss
        cmp #60
        bcc ca_fmt
        lda #0
        sta v_ss
        inc v_mm
        lda v_mm
        cmp #60
        bcc ca_fmt
        lda #0
        sta v_mm
        inc v_hh
        lda v_hh
        cmp #24
        bcc ca_fmt
        lda #0
        sta v_hh
ca_fmt:
        jsr clock_format
        lda #1
        sta pf_clk
ca_done:
        rts

clock_format:
        lda v_hh
        jsr two_digits
        stx clk_str+0
        sta clk_str+1
        lda #$3A             ; ':'
        sta clk_str+2
        lda v_mm
        jsr two_digits
        stx clk_str+3
        sta clk_str+4
        lda #$3A             ; ':'
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
        sta ptr
        lda #>clk_str
        sta ptr+1
        lda #12
        sta col
        lda #12
        sta row
        jmp cputs

; ---- Theme (palette cycling) ----------------------------------------------
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
        jsr draw_theme_status   ; apply the current preset + show its digit
        rts

theme_input:
        lda v_pade
        and #PAD_A
        beq ti_done
        inc v_theme
        lda v_theme
        cmp #NTHEMES
        bcc ti_s
        lda #0
        sta v_theme
ti_s:   lda #1
        sta pf_pal
ti_done:
        rts

; apply_theme: write the current preset's 4 colours to $3F00
apply_theme:
        lda v_theme
        asl
        asl
        tay                     ; v_theme*4
        bit PPUSTATUS
        lda #$3F
        sta PPUADDR
        lda #$00
        sta PPUADDR
        lda theme_presets,y
        sta PPUDATA
        iny
        lda theme_presets,y
        sta PPUDATA
        iny
        lda theme_presets,y
        sta PPUDATA
        iny
        lda theme_presets,y
        sta PPUDATA
        rts

draw_theme_status:
        jsr apply_theme
        lda v_theme
        clc
        adc #$30             ; '0'
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

; ---- Music (2A03 APU pulse channel 1) -------------------------------------
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

music_init:
        lda #$01
        sta APUSTATUS           ; enable pulse 1
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
        sta APUSTATUS
        sta m_play
        rts

; music_load: program pulse 1 with note m_idx, set m_timer
music_load:
        lda m_idx
        sta g_n
        asl                     ; *2
        clc
        adc g_n                 ; *3 (dw period + db frames)
        tay
        lda music_song,y
        sta mlo                 ; period low
        iny
        lda music_song,y
        sta mhi                 ; period high
        iny
        lda music_song,y
        sta m_timer             ; frames
        lda #$BF                ; duty 50%, length halt, constant vol 15
        sta $4000
        lda #$00
        sta $4001               ; sweep off
        lda mlo
        sta $4002               ; timer low
        lda mhi
        and #$07
        ora #$08                ; timer high 3 bits + length-counter load
        sta $4003
        rts

music_tick:
        lda m_play
        beq mt_done
        dec m_timer
        bne mt_done
        inc m_idx
        lda m_idx
        cmp #MUSIC_COUNT
        bcc mt_load
        lda #0
        sta m_idx               ; loop the tune
mt_load:
        jsr music_load
        lda #1
        sta pf_score
mt_done:
        rts

draw_music_status:
        ; note number (1-based)
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
        ; progress bar at (6,14): m_idx cyan cells (cap 16, min 1)
        lda #6
        sta col
        lda #14
        sta row
        jsr set_nt_addr
        lda m_idx
        cmp #16
        bcc mb_w
        lda #16
mb_w:   tax
        inx
mb_loop:
        lda #T_SOLC
        sta PPUDATA
        dex
        bne mb_loop
        rts

; ============================================================================
; AUTOTEST: drive a scripted pad into the same input path so one screenshot
; proves nav / launch / an app — the ROM simulates the pad, nothing is faked.
; Each step = db frames, pad. frames=0 ends (then the pad idles + gravity freezes).
; ============================================================================
  IF AUTOTEST
auto_input:
        lda a_tmr
        bne ai_run
        lda a_idx
        asl                     ; *2 (2 bytes/step)
        tay
        lda auto_script,y
        bne ai_have             ; frames != 0
        ; script done: idle the pad, freeze game gravity for a stable shot
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
        ; move the highlight across the grid, ending on Theme (sel 5) — a
        ; fully-visible icon. The screenshot shows directional selection (no launch).
        dc.b 8, 0
        dc.b 2, PAD_R           ; sel 0 -> 1
        dc.b 4, 0
        dc.b 2, PAD_R           ; -> 2
        dc.b 4, 0
        dc.b 2, PAD_D           ; -> 6 (Tracker, row 2)
        dc.b 4, 0
        dc.b 2, PAD_U           ; -> 2
        dc.b 4, 0
        dc.b 2, PAD_R           ; -> 3
        dc.b 4, 0
        dc.b 2, PAD_R           ; -> 4 (Files)
        dc.b 4, 0
        dc.b 2, PAD_R           ; -> 5 (Theme)
        dc.b 30, 0
        dc.b 0, 0
  ENDIF
  IF AT_APP
        ; launch SysInfo (sel 0) full-screen and settle
        dc.b 8, 0
        dc.b 2, PAD_A
        dc.b 40, 0
        dc.b 0, 0
  ENDIF
  IF AT_CLOCK
        ; navigate to Clock (sel 1), launch, let ~3 s of time tick by
        dc.b 6, 0
        dc.b 2, PAD_R           ; -> 1 (Clock)
        dc.b 4, 0
        dc.b 2, PAD_A           ; launch
        dc.b 200, 0             ; ~3.3 s -> HH:MM:SS advances live
        dc.b 0, 0
  ENDIF
  IF AT_THEME
        ; navigate to Theme (sel 5), launch, cycle the palette twice
        dc.b 6, 0
        dc.b 2, PAD_R
        dc.b 2, 0
        dc.b 2, PAD_R
        dc.b 2, 0
        dc.b 2, PAD_R
        dc.b 2, 0
        dc.b 2, PAD_R
        dc.b 2, 0
        dc.b 2, PAD_R           ; -> 5 (Theme)
        dc.b 4, 0
        dc.b 2, PAD_A           ; launch
        dc.b 8, 0
        dc.b 2, PAD_A           ; cycle preset
        dc.b 8, 0
        dc.b 2, PAD_A           ; cycle again
        dc.b 30, 0
        dc.b 0, 0
  ENDIF
  IF AT_MUSIC
        ; navigate to Music (sel 3), launch, let several notes play
        dc.b 6, 0
        dc.b 2, PAD_R
        dc.b 2, 0
        dc.b 2, PAD_R
        dc.b 2, 0
        dc.b 2, PAD_R           ; -> 3 (Music)
        dc.b 4, 0
        dc.b 2, PAD_A           ; launch + play
        dc.b 200, 0
        dc.b 0, 0
  ENDIF
  IF AT_DOSTRIS
        ; launch Dostris (navigate to sel 7), then spread a few pieces across
        ; the board (each L/R toggles back to 0 to register a fresh edge; PAD_D
        ; soft-drops, then idle so gravity locks + spawns the next).
        dc.b 6, 0
        dc.b 2, PAD_R           ; -> 1
        dc.b 2, 0
        dc.b 2, PAD_R           ; -> 2
        dc.b 2, 0
        dc.b 2, PAD_R           ; -> 3
        dc.b 2, 0
        dc.b 2, PAD_D           ; -> 7 (Dostris)
        dc.b 4, 0
        dc.b 2, PAD_A           ; launch
        dc.b 6, 0
        ; piece 1 -> hard left
        dc.b 2, PAD_L
        dc.b 2, 0
        dc.b 2, PAD_L
        dc.b 2, 0
        dc.b 2, PAD_L
        dc.b 2, 0
        dc.b 2, PAD_L
        dc.b 2, 0
        dc.b 16, PAD_D
        dc.b 34, 0
        ; piece 2 -> left of centre
        dc.b 2, PAD_L
        dc.b 2, 0
        dc.b 2, PAD_L
        dc.b 2, 0
        dc.b 16, PAD_D
        dc.b 34, 0
        ; piece 3 -> centre, rotated
        dc.b 2, PAD_A
        dc.b 2, 0
        dc.b 16, PAD_D
        dc.b 34, 0
        ; piece 4 -> right of centre
        dc.b 2, PAD_R
        dc.b 2, 0
        dc.b 2, PAD_R
        dc.b 2, 0
        dc.b 16, PAD_D
        dc.b 34, 0
        ; piece 5 -> hard right
        dc.b 2, PAD_R
        dc.b 2, 0
        dc.b 2, PAD_R
        dc.b 2, 0
        dc.b 2, PAD_R
        dc.b 2, 0
        dc.b 16, PAD_D
        dc.b 34, 0
        ; piece 6 -> nudge, leave it falling for the shot
        dc.b 2, PAD_L
        dc.b 2, 0
        dc.b 6, PAD_D
        dc.b 2, 0
        dc.b 0, 0
  ENDIF
  IF AT_TRACKER
        ; navigate to Tracker (sel 6: Right Right Down), launch, edit a couple
        ; of notes, let the playhead run, then idle for a stable shot.
        dc.b 8, 0
        dc.b 2, PAD_R           ; -> 1
        dc.b 4, 0
        dc.b 2, PAD_R           ; -> 2
        dc.b 4, 0
        dc.b 2, PAD_D           ; -> 6 (Tracker)
        dc.b 4, 0
        dc.b 2, PAD_A           ; launch
        dc.b 8, 0
        dc.b 2, PAD_D           ; cursor down a row
        dc.b 4, 0
        dc.b 2, PAD_R           ; cursor to channel 1
        dc.b 4, 0
        dc.b 2, PAD_A           ; cycle a note in
        dc.b 4, 0
        dc.b 2, PAD_A           ; cycle again
        dc.b 60, 0              ; let the playhead step down the rows
        dc.b 0, 0
  ENDIF
  IF AT_PAINT
        ; navigate to Paint (sel 10: Right Right Down Down), launch, paint a
        ; little row of cells, then idle for a stable shot.
        dc.b 8, 0
        dc.b 2, PAD_R           ; -> 1
        dc.b 4, 0
        dc.b 2, PAD_R           ; -> 2
        dc.b 4, 0
        dc.b 2, PAD_D           ; -> 6
        dc.b 4, 0
        dc.b 2, PAD_D           ; -> 10 (Paint)
        dc.b 4, 0
        dc.b 2, PAD_A           ; launch
        dc.b 8, 0
        dc.b 2, PAD_D           ; (0,1)
        dc.b 2, 0
        dc.b 2, PAD_A           ; paint
        dc.b 2, 0
        dc.b 2, PAD_R           ; (1,1)
        dc.b 2, 0
        dc.b 2, PAD_A
        dc.b 2, 0
        dc.b 2, PAD_R           ; (2,1)
        dc.b 2, 0
        dc.b 2, PAD_A
        dc.b 2, 0
        dc.b 2, PAD_D           ; (2,2)
        dc.b 2, 0
        dc.b 2, PAD_A
        dc.b 8, 0
        dc.b 0, 0
  ENDIF
  IF AT_OUTLAST
        ; navigate to OutLast (sel 8: Down Down), launch, drive straight so the
        ; road scrolls, then idle for a stable shot.
        dc.b 8, 0
        dc.b 2, PAD_D           ; -> 4
        dc.b 4, 0
        dc.b 2, PAD_D           ; -> 8 (OutLast)
        dc.b 4, 0
        dc.b 2, PAD_A           ; launch
        dc.b 90, 0              ; drive straight (road scrolls past)
        dc.b 0, 0
  ENDIF
  IF AT_PACMAN
        ; navigate to Pac-Man (sel 9: Down Down Right), launch, let Pac chomp
        ; left then turn up while the ghosts close in, then idle for the shot.
        dc.b 8, 0
        dc.b 2, PAD_D           ; -> 4
        dc.b 4, 0
        dc.b 2, PAD_D           ; -> 8
        dc.b 4, 0
        dc.b 2, PAD_R           ; -> 9 (Pac-Man)
        dc.b 4, 0
        dc.b 2, PAD_A           ; launch
        dc.b 22, 0              ; Pac chomps left (default heading)
        dc.b 2, PAD_U           ; queue a turn up
        dc.b 8, 0               ; freeze mid-chase (ghosts closing, still playing)
        dc.b 0, 0
  ENDIF
  ENDIF ; AUTOTEST

; ============================================================================
; strings + data
; ============================================================================
s_title:   dc.b "UnoDOS 3",0
s_back:    dc.b "B = Back",0

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

; app titles
t_sysinfo: dc.b "SysInfo",0
t_clock:   dc.b "Clock",0
t_notepad: dc.b "Notepad",0
t_music:   dc.b "Music",0
t_files:   dc.b "Files",0
t_theme:   dc.b "Theme",0
t_dostris: dc.b "Dostris",0

; ---- app content tables: per line {db col,row} then {dw str}; end col=$FF ----
c_sysinfo:
        dc.b 2, 3
        dc.w m_si0
        dc.b 2, 5
        dc.w m_si1
        dc.b 2, 6
        dc.w m_si2
        dc.b 2, 7
        dc.w m_si3
        dc.b 2, 8
        dc.w m_si4
        dc.b 2,10
        dc.w m_si5
        dc.b $FF
m_si0: dc.b "UnoDOS 3 - NES",0
m_si1: dc.b "CPU  6502/2A03",0
m_si2: dc.b "     1.79 MHz",0
m_si3: dc.b "PPU  2C02 256x240",0
m_si4: dc.b "RAM  2 KB",0
m_si5: dc.b "minimal profile",0

c_clock:
        dc.b 2, 3
        dc.w m_cl0
        dc.b 2,10
        dc.w m_cl1
        dc.b $FF
m_cl0: dc.b "System clock",0
m_cl1: dc.b "Time:",0

c_notepad:
        dc.b 2, 3
        dc.w m_np0
        dc.b 2, 5
        dc.w m_np1
        dc.b 2, 6
        dc.w m_np2
        dc.b 2, 8
        dc.w m_np3
        dc.b $FF
m_np0: dc.b "Notepad",0
m_np1: dc.b "Second fresh port",0
m_np2: dc.b "on the 6502/2A03.",0
m_np3: dc.b "- contract-driven",0

c_files:
        dc.b 2, 3
        dc.w m_fi0
        dc.b 2, 5
        dc.w m_fi1
        dc.b 2, 6
        dc.w m_fi2
        dc.b 2, 7
        dc.w m_fi3
        dc.b 2, 9
        dc.w m_fi4
        dc.b $FF
m_fi0: dc.b "Files - apps:",0
m_fi1: dc.b "SYSINFO CLOCK",0
m_fi2: dc.b "NOTEPAD FILES",0
m_fi3: dc.b "THEME   MUSIC",0
m_fi4: dc.b "(no disk - NROM)",0

c_theme:
        dc.b 2, 3
        dc.w m_th0
        dc.b 2, 5
        dc.w m_th1
        dc.b 2,12
        dc.w m_th2
        dc.b $FF
m_th0: dc.b "Theme presets",0
m_th1: dc.b "A = cycle palette",0
m_th2: dc.b "Preset:",0

c_music:
        dc.b 2, 3
        dc.w m_mu0
        dc.b 2, 5
        dc.w m_mu1
        dc.b 2,12
        dc.w m_mu2
        dc.b 2,14
        dc.w m_mu3
        dc.b $FF
m_mu0: dc.b "Music player",0
m_mu1: dc.b "Ode to Joy",0
m_mu2: dc.b "Note:",0
m_mu3: dc.b "APU pulse 1",0

c_generic:
        dc.b 2, 3
        dc.w m_ge0
        dc.b 2, 5
        dc.w m_ge1
        dc.b 2, 6
        dc.w m_ge2
        dc.b $FF
m_ge0: dc.b "UnoDOS 3 / NES",0
m_ge1: dc.b "Coming soon.",0
m_ge2: dc.b "(minimal profile)",0

; theme presets: 4 NES master-palette bytes each (backdrop, white, accent1/2)
NTHEMES = 4
theme_presets:
        dc.b $11,$30,$2C,$24    ; 0 blue (default)
        dc.b $1A,$30,$2A,$28    ; 1 green
        dc.b $06,$30,$16,$27    ; 2 red/orange
        dc.b $0F,$30,$00,$10    ; 3 mono
