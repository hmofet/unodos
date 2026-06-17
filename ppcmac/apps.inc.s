# ============================================================================
# UnoDOS / PowerPC Mac — full-screen apps (M3), clock/theme/music, AUTOTEST,
# strings (incl. the Open Firmware service names). GNU as / PowerPC.
# ============================================================================

# draw_chrome: r5 = title strptr -> apply palette, clear, title bar + footer
draw_chrome:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    mr    r14, r5
    bl    load_palette
    bl    clear_screen
    li    r3, 1
    bl    set_fg
    li    r3, 0
    li    r4, 0
    li    r5, SCRW
    li    r6, 16
    bl    frect
    li    r3, 0
    li    r4, 1
    bl    setfb
    li    r3, 8
    li    r4, 4
    mr    r5, r14
    bl    pstr
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 8
    li    r4, 464
    LA    r5, s_back
    bl    pstr
    lwz   r14, 8(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

# draw_content: r3 = table of .long x,y,strptr ; end x=0xFFFFFFFF
draw_content:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    mr    r14, r3
    li    r3, 1
    li    r4, 0
    bl    setfb
dc_l:
    lwz   r3, 0(r14)
    cmpwi r3, -1
    beq   dc_d
    lwz   r4, 4(r14)
    lwz   r5, 8(r14)
    bl    pstr
    addi  r14, r14, 12
    b     dc_l
dc_d:
    lwz   r14, 8(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

# ---- draw_app dispatch -----------------------------------------------------
draw_app:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_app
    cmpwi r3, 0
    beq   app_sysinfo
    cmpwi r3, 1
    beq   app_clock
    cmpwi r3, 2
    beq   app_notepad
    cmpwi r3, 3
    beq   app_music
    cmpwi r3, 4
    beq   app_files
    cmpwi r3, 5
    beq   app_theme
    cmpwi r3, 7
    beq   app_dostris
    b     app_generic
app_sysinfo:
    LA    r5, t_sysinfo
    bl    draw_chrome
    LA    r3, c_sysinfo
    bl    draw_content
    b     da_ret
app_notepad:
    LA    r5, t_notepad
    bl    draw_chrome
    LA    r3, c_notepad
    bl    draw_content
    b     da_ret
app_files:
    LA    r5, t_files
    bl    draw_chrome
    LA    r3, c_files
    bl    draw_content
    b     da_ret
app_generic:
    LA    r4, icon_lbl
    LWZA  r3, v_app
    slwi  r3, r3, 2
    lwzx  r5, r4, r3
    bl    draw_chrome
    LA    r3, c_generic
    bl    draw_content
    b     da_ret
app_dostris:
    LA    r5, t_dostris
    bl    draw_chrome
    bl    dostris_draw
    b     da_ret
app_clock:
    LA    r5, t_clock
    bl    draw_chrome
    LA    r3, c_clock
    bl    draw_content
    bl    clock_format
    bl    draw_clock_time
    b     da_ret
app_theme:
    LA    r5, t_theme
    bl    draw_chrome
    LA    r3, c_theme
    bl    draw_content
    bl    draw_theme_status
    b     da_ret
app_music:
    LA    r5, t_music
    bl    draw_chrome
    LA    r3, c_music
    bl    draw_content
    bl    draw_music_status
da_ret:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ---- Clock -----------------------------------------------------------------
clock_advance:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_frac
    addi  r3, r3, 1
    cmpwi r3, 60
    blt   ca_store_frac
    li    r3, 0
    STWA  r3, v_frac, r4
    LWZA  r3, v_ss
    addi  r3, r3, 1
    cmpwi r3, 60
    blt   ca_store_ss
    li    r3, 0
    STWA  r3, v_ss, r4
    LWZA  r3, v_mm
    addi  r3, r3, 1
    cmpwi r3, 60
    blt   ca_store_mm
    li    r3, 0
    STWA  r3, v_mm, r4
    LWZA  r3, v_hh
    addi  r3, r3, 1
    cmpwi r3, 24
    blt   ca_store_hh
    li    r3, 0
ca_store_hh:
    STWA  r3, v_hh, r4
    b     ca_fmt
ca_store_mm:
    STWA  r3, v_mm, r4
    b     ca_fmt
ca_store_ss:
    STWA  r3, v_ss, r4
ca_fmt:
    bl    clock_format
    li    r3, 1
    STWA  r3, pf_clk, r4
    b     ca_ret
ca_store_frac:
    STWA  r3, v_frac, r4
ca_ret:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

clock_format:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    stw   r14, 8(r1)
    LA    r14, clk_str
    LWZA  r3, v_hh
    bl    two_digits
    stb   r4, 0(r14)
    stb   r3, 1(r14)
    li    r3, ':'
    stb   r3, 2(r14)
    LWZA  r3, v_mm
    bl    two_digits
    stb   r4, 3(r14)
    stb   r3, 4(r14)
    li    r3, ':'
    stb   r3, 5(r14)
    LWZA  r3, v_ss
    bl    two_digits
    stb   r4, 6(r14)
    stb   r3, 7(r14)
    li    r3, 0
    stb   r3, 8(r14)
    lwz   r14, 8(r1)
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

draw_clock_time:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 260
    li    r4, 240
    LA    r5, clk_str
    bl    pstr
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ---- Theme -----------------------------------------------------------------
theme_input:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_pade
    andi. r0, r3, PAD_A
    beq   ti_done
    LWZA  r3, v_theme
    addi  r3, r3, 1
    cmpwi r3, NTHEMES
    blt   ti_st
    li    r3, 0
ti_st:
    STWA  r3, v_theme, r4
    li    r3, 1
    STWA  r3, v_dirty, r4
ti_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

draw_theme_status:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_theme
    addi  r3, r3, '0'
    LA    r4, numstr
    stb   r3, 0(r4)
    li    r3, 0
    stb   r3, 1(r4)
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 136
    li    r4, 240
    LA    r5, numstr
    bl    pstr
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ---- Music (square-wave PCM tone synthesis -> the Mac codec data port) -----
# The kernel synthesises a square wave for the current note in software (a phase
# accumulator) and feeds PCM samples to the codec data port each frame. The harness
# captures those writes as the actual audio stream (-> WAV). On real hardware the
# codec + DBDMA bring-up is best-effort / by-ear; the PCM synthesis is verified.
music_init:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 0
    STWA  r3, m_idx, r4
    li    r3, 1
    STWA  r3, m_play, r4
    bl    music_load
    li    r3, 1
    STWA  r3, pf_score, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr
music_silence:
    li    r3, 0
    STWA  r3, m_play, r4
    li    r3, 0
    STWA  r3, m_freq, r4
    blr
music_load:
    LWZA  r3, m_idx
    slwi  r3, r3, 2
    LA    r4, music_song
    add   r4, r4, r3
    lhz   r5, 0(r4)                       # note frequency (Hz)
    STWA  r5, m_freq, r6
    li    r5, 0
    STWA  r5, m_phase, r6                 # restart the phase for a clean tone
    lbz   r3, 2(r4)                       # duration (frames)
    STWA  r3, m_timer, r4
    blr
# music_gen: synthesise one frame of square-wave PCM for the current note. Leaf.
music_gen:
    LWZA  r3, m_freq
    cmpwi r3, 0
    beq   mg_done                        # rest -> no samples
    LWZA  r4, m_phase
    LA    r5, SOUND_FIFO
    li    r6, AUD_PERF
mg_loop:
    cmpwi r4, (AUD_RATE/2)
    blt   mg_hi
    li    r7, -AUD_AMP                    # second half -amp
    b     mg_w
mg_hi:
    li    r7, AUD_AMP                     # first half +amp
mg_w:
    sth   r7, 0(r5)                       # write 16-bit sample to the codec port
    add   r4, r4, r3                      # phase += freq
    cmpwi r4, AUD_RATE
    blt   mg_now
    subi  r4, r4, AUD_RATE
mg_now:
    subic. r6, r6, 1
    bne   mg_loop
    STWA  r4, m_phase, r5
mg_done:
    blr
music_tick:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, m_play
    cmpwi r3, 0
    beq   mt_done
    bl    music_gen
    LWZA  r3, m_timer
    subi  r3, r3, 1
    STWA  r3, m_timer, r4
    cmpwi r3, 0
    bne   mt_done
    LWZA  r3, m_idx
    addi  r3, r3, 1
    cmpwi r3, MUSIC_COUNT
    blt   mt_st
    li    r3, 0
mt_st:
    STWA  r3, m_idx, r4
    bl    music_load
    li    r3, 1
    STWA  r3, pf_score, r4
mt_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

draw_music_status:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    LWZA  r3, m_idx
    addi  r3, r3, 1
    bl    two_digits
    LA    r5, numstr
    stb   r4, 0(r5)
    stb   r3, 1(r5)
    li    r3, 0
    stb   r3, 2(r5)
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 120
    li    r4, 130
    LA    r5, numstr
    bl    pstr
    LWZA  r3, m_idx
    cmpwi r3, 24
    blt   dms_ok
    li    r3, 24
dms_ok:
    addi  r3, r3, 1
    slwi  r14, r3, 4                      # bar width
    li    r3, 2
    bl    set_fg
    li    r3, 40
    li    r4, 240
    mr    r5, r14
    li    r6, 14
    bl    frect
    lwz   r14, 8(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

# ============================================================================
# AUTOTEST: scripted pad into the same input path. .byte frames, pad.
# ============================================================================
.ifdef AUTOTEST
auto_input:
    LWZA  r3, a_tmr
    cmpwi r3, 0
    bne   ai_run
    LWZA  r3, a_idx
    slwi  r3, r3, 1
    LA    r4, auto_script
    add   r4, r4, r3                      # &script[a_idx*2]
    lbz   r5, 0(r4)                       # frames
    cmpwi r5, 0
    bne   ai_have
    li    r3, 1
    STWA  r3, a_gpause, r6
    li    r3, 0
    STWA  r3, v_pad, r6
    STWA  r3, v_pade, r6
    blr
ai_have:
    STWA  r5, a_tmr, r6
    lbz   r5, 1(r4)                       # pad
    STWA  r5, a_pad, r6
    LWZA  r3, a_idx
    addi  r3, r3, 1
    STWA  r3, a_idx, r6
ai_run:
    LWZA  r3, v_pad
    STWA  r3, v_padp, r4
    LWZA  r3, a_tmr
    subi  r3, r3, 1
    STWA  r3, a_tmr, r4
    LWZA  r5, a_pad
    STWA  r5, v_pad, r4
    LWZA  r3, v_padp
    not   r3, r3
    and   r3, r5, r3                      # edges = new & ~prev
    STWA  r3, v_pade, r4
    blr

.section .rodata
.align 0
auto_script:
.ifdef AT_NAV
    .byte 8,0,  2,PAD_R, 4,0, 2,PAD_R, 4,0, 2,PAD_D, 30,0, 0,0
.endif
.ifdef AT_APP
    .byte 8,0,  2,PAD_A, 40,0, 0,0
.endif
.ifdef AT_CLOCK
    .byte 6,0,  2,PAD_R, 4,0, 2,PAD_A, 200,0, 0,0
.endif
.ifdef AT_THEME
    .byte 6,0,  2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R, 4,0, 2,PAD_A, 8,0, 2,PAD_A, 8,0, 2,PAD_A, 30,0, 0,0
.endif
.ifdef AT_MUSIC
    .byte 6,0,  2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R, 4,0, 2,PAD_A, 200,0, 0,0
.endif
.ifdef AT_DOSTRIS
    .byte 6,0,  2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_D, 4,0, 2,PAD_A, 6,0
    .byte 2,PAD_L,2,0, 2,PAD_L,2,0, 2,PAD_L,2,0, 2,PAD_L,2,0, 16,PAD_D, 34,0
    .byte 2,PAD_L,2,0, 2,PAD_L,2,0, 16,PAD_D, 34,0
    .byte 2,PAD_A,2,0, 16,PAD_D, 34,0
    .byte 2,PAD_R,2,0, 2,PAD_R,2,0, 16,PAD_D, 34,0
    .byte 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 16,PAD_D, 34,0
    .byte 2,PAD_L,2,0, 6,PAD_D, 2,0, 0,0
.endif
.align 2
.section .text
.endif

# ============================================================================
# data: strings + content tables
# ============================================================================
.section .rodata
# Open Firmware client-interface service + device/property names
s_finddevice: .asciz "finddevice"
s_getprop:    .asciz "getprop"
s_read:       .asciz "read"
s_screen:     .asciz "screen"
s_chosen:     .asciz "/chosen"
s_address:    .asciz "address"
s_linebytes:  .asciz "linebytes"
s_stdin:      .asciz "stdin"
.align 2
icon_lbl:
    .long l_sysinfo, l_clock, l_notepad, l_music, l_files, l_theme
    .long l_tracker, l_dostris, l_outlast, l_pacman, l_paint
l_sysinfo: .asciz "SysInfo"
l_clock:   .asciz "Clock"
l_notepad: .asciz "Notepad"
l_music:   .asciz "Music"
l_files:   .asciz "Files"
l_theme:   .asciz "Theme"
l_tracker: .asciz "Tracker"
l_dostris: .asciz "Dostris"
l_outlast: .asciz "OutLast"
l_pacman:  .asciz "Pac-Man"
l_paint:   .asciz "Paint"
s_title:   .asciz "UnoDOS 3 - PowerPC Mac (Open Firmware)"
s_back:    .asciz "B = Back"
t_sysinfo: .asciz "SysInfo"
t_clock:   .asciz "Clock"
t_notepad: .asciz "Notepad"
t_music:   .asciz "Music"
t_files:   .asciz "Files"
t_theme:   .asciz "Theme"
t_dostris: .asciz "Dostris"
.align 2
c_sysinfo:
    .long 16, 48, m_si0
    .long 16, 96, m_si1
    .long 16, 116, m_si2
    .long 16, 136, m_si3
    .long 16, 172, m_si4
    .long 0xFFFFFFFF
m_si0: .asciz "UnoDOS 3 - PowerPC Mac"
m_si1: .asciz "CPU  PowerPC (32-bit, BE)"
m_si2: .asciz "Boot  Open Firmware client"
m_si3: .asciz "Display  640x480x32 XRGB"
m_si4: .asciz "minimal profile"
.align 2
c_clock:
    .long 16, 48, m_cl0
    .long 16, 240, m_cl1
    .long 0xFFFFFFFF
m_cl0: .asciz "System clock"
m_cl1: .asciz "Time:"
.align 2
c_notepad:
    .long 16, 48, m_np0
    .long 16, 96, m_np1
    .long 16, 116, m_np2
    .long 0xFFFFFFFF
m_np0: .asciz "Notepad"
m_np1: .asciz "Eleventh fresh port"
m_np2: .asciz "first PowerPC (big-endian) world"
.align 2
c_files:
    .long 16, 48, m_fi0
    .long 16, 96, m_fi1
    .long 16, 116, m_fi2
    .long 16, 136, m_fi3
    .long 0xFFFFFFFF
m_fi0: .asciz "Files - apps:"
m_fi1: .asciz "SYSINFO CLOCK NOTEPAD"
m_fi2: .asciz "FILES THEME MUSIC"
m_fi3: .asciz "(no disk - OF payload)"
.align 2
c_theme:
    .long 16, 48, m_th0
    .long 16, 96, m_th1
    .long 16, 240, m_th2
    .long 0xFFFFFFFF
m_th0: .asciz "Theme presets"
m_th1: .asciz "A = cycle palette"
m_th2: .asciz "Preset:"
.align 2
c_music:
    .long 16, 48, m_mu0
    .long 16, 96, m_mu1
    .long 16, 130, m_mu2
    .long 0xFFFFFFFF
m_mu0: .asciz "Music player (square)"
m_mu1: .asciz "Ode to Joy"
m_mu2: .asciz "Note:"
.align 2
c_generic:
    .long 16, 48, m_ge0
    .long 16, 96, m_ge1
    .long 0xFFFFFFFF
m_ge0: .asciz "UnoDOS 3 / PowerPC Mac"
m_ge1: .asciz "Coming soon."
.section .text
