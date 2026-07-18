// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — full-screen apps (M3), clock/theme/music,
// AUTOTEST scripted pad, strings. GNU as / aarch64.
// ============================================================================

// draw_chrome: x2 = title strptr -> apply palette, clear, title bar + footer
draw_chrome:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   x19, x2
    bl    load_palette
    bl    clear_screen
    mov   w0, #1
    bl    set_fg
    mov   w0, #0
    mov   w1, #0
    mov   w2, #SCRW
    mov   w3, #16
    bl    frect
    mov   w0, #0
    mov   w1, #1
    bl    setfb
    mov   w0, #8
    mov   w1, #4
    mov   x2, x19
    bl    pstr
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #8
    mov   w1, #464
    ldr   x2, =s_back
    bl    pstr
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// draw_content: x0 = table of .word x,y,strptr ; end x=0xFFFFFFFF (white on desktop)
draw_content:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   x19, x0
    mov   w0, #1
    mov   w1, #0
    bl    setfb
dc_l:
    ldr   w0, [x19], #4
    cmn   w0, #1
    b.eq  dc_d
    ldr   w1, [x19], #4
    ldr   w2, [x19], #4
    bl    pstr
    b     dc_l
dc_d:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// ---- draw_app dispatch -----------------------------------------------------
draw_app:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #0
    b.eq  app_sysinfo
    cmp   w0, #1
    b.eq  app_clock
    cmp   w0, #2
    b.eq  app_notepad
    cmp   w0, #3
    b.eq  app_music
    cmp   w0, #4
    b.eq  app_files
    cmp   w0, #5
    b.eq  app_theme
    cmp   w0, #7
    b.eq  app_dostris
    cmp   w0, #9
    b.eq  app_pacman
    cmp   w0, #8
    b.eq  app_outlast
    cmp   w0, #6
    b.eq  app_tracker
    cmp   w0, #10
    b.eq  app_paint
    b     app_generic
app_sysinfo:
    ldr   x2, =t_sysinfo
    bl    draw_chrome
    ldr   x0, =c_sysinfo
    bl    draw_content
    ldp   x29, x30, [sp], #16
    ret
app_notepad:
    bl    notepad_draw
    ldp   x29, x30, [sp], #16
    ret
app_files:
    bl    files_draw
    ldp   x29, x30, [sp], #16
    ret
app_generic:
    ldr   x0, =icon_lbl
    ldr   x1, =v_app
    ldr   w1, [x1]
    ldr   w2, [x0, w1, uxtw #2]
    bl    draw_chrome
    ldr   x0, =c_generic
    bl    draw_content
    ldp   x29, x30, [sp], #16
    ret
app_dostris:
    ldr   x2, =t_dostris
    bl    draw_chrome
    bl    dostris_draw
    ldp   x29, x30, [sp], #16
    ret
app_clock:
    ldr   x2, =t_clock
    bl    draw_chrome
    ldr   x0, =c_clock
    bl    draw_content
    bl    clock_format
    bl    draw_clock_time
    ldp   x29, x30, [sp], #16
    ret
app_theme:
    ldr   x2, =t_theme
    bl    draw_chrome
    ldr   x0, =c_theme
    bl    draw_content
    bl    draw_theme_status
    ldp   x29, x30, [sp], #16
    ret
app_music:
    ldr   x2, =t_music
    bl    draw_chrome
    ldr   x0, =c_music
    bl    draw_content
    bl    draw_music_status
    ldp   x29, x30, [sp], #16
    ret

// ---- Clock -----------------------------------------------------------------
clock_advance:
    stp   x29, x30, [sp, #-16]!
    ldr   x3, =v_frac
    ldr   w0, [x3]
    add   w0, w0, #1
    cmp   w0, #60
    b.lo  ca_store_frac
    str   wzr, [x3]
    ldr   x3, =v_ss
    ldr   w0, [x3]
    add   w0, w0, #1
    cmp   w0, #60
    b.lo  ca_store_ss
    str   wzr, [x3]
    ldr   x3, =v_mm
    ldr   w0, [x3]
    add   w0, w0, #1
    cmp   w0, #60
    b.lo  ca_store_mm
    str   wzr, [x3]
    ldr   x3, =v_hh
    ldr   w0, [x3]
    add   w0, w0, #1
    cmp   w0, #24
    csel  w0, wzr, w0, hs
    str   w0, [x3]
    b     ca_fmt
ca_store_mm:
    str   w0, [x3]
    b     ca_fmt
ca_store_ss:
    str   w0, [x3]
ca_fmt:
    bl    clock_format
    ldr   x0, =pf_clk
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret
ca_store_frac:
    str   w0, [x3]
    ldp   x29, x30, [sp], #16
    ret

clock_format:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    ldr   x19, =clk_str
    ldr   x0, =v_hh
    ldr   w0, [x0]
    bl    two_digits
    strb  w1, [x19, #0]
    strb  w0, [x19, #1]
    mov   w0, #':'
    strb  w0, [x19, #2]
    ldr   x0, =v_mm
    ldr   w0, [x0]
    bl    two_digits
    strb  w1, [x19, #3]
    strb  w0, [x19, #4]
    mov   w0, #':'
    strb  w0, [x19, #5]
    ldr   x0, =v_ss
    ldr   w0, [x0]
    bl    two_digits
    strb  w1, [x19, #6]
    strb  w0, [x19, #7]
    strb  wzr, [x19, #8]
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

draw_clock_time:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #260
    mov   w1, #240
    ldr   x2, =clk_str
    bl    pstr
    ldp   x29, x30, [sp], #16
    ret

// ---- Theme -----------------------------------------------------------------
theme_input:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  ti_done
    ldr   x2, =v_theme
    ldr   w0, [x2]
    add   w0, w0, #1
    cmp   w0, #NTHEMES
    csel  w0, wzr, w0, hs
    str   w0, [x2]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
ti_done:
    ldp   x29, x30, [sp], #16
    ret

draw_theme_status:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_theme
    ldr   w0, [x0]
    add   w0, w0, #'0'
    ldr   x1, =numstr
    strb  w0, [x1]
    strb  wzr, [x1, #1]
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #136
    mov   w1, #240
    ldr   x2, =numstr
    bl    pstr
    ldp   x29, x30, [sp], #16
    ret

// ---- Music (Pi PWM headphone-jack tone path) -------------------------------
// The 3.5mm jack on the Pi is driven by PWM0/1 in mark/space mode: a square wave
// at freq = PWM_CLK / RNG1 with DAT1 = RNG1/2. PWM_CLK is set ~9.6 MHz here. The
// harness sinks these MMIO writes (no audio in Unicorn); the tone is hardware,
// by-ear. The UI/timing path below IS verified.
.equ PWM_CLK_HZ, 9600000
music_init:
    stp   x29, x30, [sp, #-16]!
    // PWM clock: disable, set divisor, enable (source = 19.2MHz oscillator)
    ldr   x0, =CM_PWMCTL
    ldr   w1, =0x5A000000
    str   w1, [x0]
    ldr   x0, =CM_PWMDIV
    ldr   w1, =0x5A002000                 // DIVI = 2
    str   w1, [x0]
    ldr   x0, =CM_PWMCTL
    ldr   w1, =0x5A000211                 // ENAB | src=osc
    str   w1, [x0]
    ldr   x0, =m_idx
    str   wzr, [x0]
    ldr   x0, =m_play
    mov   w1, #1
    str   w1, [x0]
    bl    music_load
    ldr   x0, =pf_score
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret
music_silence:
    ldr   x0, =PWM_CTL
    str   wzr, [x0]
    ldr   x0, =m_play
    str   wzr, [x0]
    ret
music_load:
    ldr   x0, =m_idx
    ldr   w0, [x0]
    ldr   x1, =music_song
    add   x1, x1, w0, uxtw #2
    ldrh  w2, [x1]                        // note frequency (Hz)
    ldrb  w3, [x1, #2]                    // duration (frames)
    ldr   x0, =m_timer
    str   w3, [x0]
    cbz   w2, ml_rest
    ldr   w4, =PWM_CLK_HZ
    udiv  w4, w4, w2                       // RNG1 = clk / freq
    ldr   x0, =PWM_RNG1
    str   w4, [x0]
    lsr   w4, w4, #1
    ldr   x0, =PWM_DAT1
    str   w4, [x0]
    ldr   x0, =PWM_CTL
    mov   w1, #0x81                        // PWEN1 | MSEN1
    str   w1, [x0]
    ret
ml_rest:
    ldr   x0, =PWM_CTL
    str   wzr, [x0]
    ret
music_tick:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =m_play
    ldr   w0, [x0]
    cbz   w0, mt_done
    ldr   x3, =m_timer
    ldr   w0, [x3]
    sub   w0, w0, #1
    str   w0, [x3]
    cbnz  w0, mt_done
    ldr   x3, =m_idx
    ldr   w0, [x3]
    add   w0, w0, #1
    cmp   w0, #MUSIC_COUNT
    csel  w0, wzr, w0, hs
    str   w0, [x3]
    bl    music_load
    ldr   x0, =pf_score
    mov   w1, #1
    str   w1, [x0]
mt_done:
    ldp   x29, x30, [sp], #16
    ret

draw_music_status:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    ldr   x0, =m_idx
    ldr   w0, [x0]
    add   w0, w0, #1
    bl    two_digits
    ldr   x2, =numstr
    strb  w1, [x2, #0]
    strb  w0, [x2, #1]
    strb  wzr, [x2, #2]
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #120
    mov   w1, #130
    ldr   x2, =numstr
    bl    pstr
    // progress bar: (m_idx cap 24)+1 cyan cells at (40,240)
    ldr   x0, =m_idx
    ldr   w0, [x0]
    cmp   w0, #24
    mov   w1, #24
    csel  w0, w1, w0, hs
    add   w0, w0, #1
    lsl   w19, w0, #4                      // *16 px
    mov   w0, #2
    bl    set_fg
    mov   w0, #40
    mov   w1, #240
    mov   w2, w19
    mov   w3, #14
    bl    frect
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// AUTOTEST: drive a scripted pad into the same input path. .byte frames, pad.
// ============================================================================
.ifdef AUTOTEST
auto_input:
    ldr   x3, =a_tmr
    ldr   w1, [x3]
    cbnz  w1, ai_run
    ldr   x0, =a_idx
    ldr   w0, [x0]
    ldr   x1, =auto_script
    add   x1, x1, w0, uxtw #1
    ldrb  w2, [x1]
    cbnz  w2, ai_have
    ldr   x0, =a_gpause
    mov   w2, #1
    str   w2, [x0]
    ldr   x0, =v_pad
    str   wzr, [x0]
    ldr   x0, =v_pade
    str   wzr, [x0]
    ret
ai_have:
    ldr   x0, =a_tmr
    str   w2, [x0]
    ldrb  w2, [x1, #1]
    ldr   x0, =a_pad
    str   w2, [x0]
    ldr   x0, =a_idx
    ldr   w2, [x0]
    add   w2, w2, #1
    str   w2, [x0]
ai_run:
    ldr   x0, =v_pad
    ldr   w1, [x0]
    ldr   x2, =v_padp
    str   w1, [x2]
    ldr   x3, =a_tmr
    ldr   w1, [x3]
    sub   w1, w1, #1
    str   w1, [x3]
    ldr   x0, =a_pad
    ldr   w1, [x0]
    ldr   x0, =v_pad
    str   w1, [x0]
    ldr   x0, =v_padp
    ldr   w0, [x0]
    mvn   w0, w0
    and   w1, w1, w0
    ldr   x0, =v_pade
    str   w1, [x0]
    ret

.section .rodata
.align 1
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
.ifdef AT_FS
    .byte 6,0
    .byte 2,PAD_R,2,0, 2,PAD_R,2,0       // launcher sel 0->2 (Notepad)
    .byte 2,PAD_A, 6,0                    // launch Notepad
    .byte 2,PAD_A, 6,0                    // A = save NOTES.TXT
    .byte 2,PAD_B, 6,0                    // back to launcher
    .byte 2,PAD_L,2,0, 2,PAD_L,2,0       // sel 2->0
    .byte 2,PAD_D,2,0                     // 0->4 (Files)
    .byte 2,PAD_A, 12,0                   // launch Files (list shows 3 files)
    .byte 0,0
.endif
.ifdef AT_FS2
    .byte 6,0
    .byte 2,PAD_D,2,0                     // sel 0->4 (Files) - no write this run
    .byte 2,PAD_A, 12,0                   // list should show persisted NOTES.TXT
    .byte 0,0
.endif
.ifdef AT_FSVIEW
    .byte 6,0
    .byte 2,PAD_D,2,0                     // sel 0->4 (Files)
    .byte 2,PAD_A, 6,0                    // launch Files (list)
    .byte 2,PAD_A, 12,0                   // open README.TXT (view its contents)
    .byte 0,0
.endif
.ifdef AT_TRACKER
    .byte 6,0
    .byte 2,PAD_D,2,0                     // launcher sel 0->4
    .byte 2,PAD_R,2,0, 2,PAD_R,2,0       // 4->5->6 (Tracker)
    .byte 2,PAD_A, 40,0                   // launch + play the seeded scale
    .byte 2,PAD_D,2,0, 2,PAD_D,2,0       // move cursor down
    .byte 2,PAD_R,2,0                     // to channel 1
    .byte 2,PAD_A,2,0, 2,PAD_A,2,0, 2,PAD_A,2,0   // cycle a note in
    .byte 30,0
    .byte 0,0
.endif
.ifdef AT_OUTLAST
    .byte 6,0
    .byte 2,PAD_D,2,0, 2,PAD_D,2,0       // launcher sel 0->4->8 (OutLast)
    .byte 2,PAD_A, 30,0                   // launch + drive
    .byte 2,PAD_L,4,0, 2,PAD_L,4,0, 30,0 // steer left
    .byte 2,PAD_R,4,0, 2,PAD_R,4,0, 30,0 // steer right
    .byte 0,0
.endif
.ifdef AT_PACMAN
    .byte 6,0
    .byte 2,PAD_D,2,0, 2,PAD_D,2,0       // launcher sel 0->4->8
    .byte 2,PAD_R,2,0                     // 8->9 (Pac-Man)
    .byte 2,PAD_A, 40,0                   // launch; pac runs left eating dots
    .byte 2,PAD_U, 40,0                   // roam up
    .byte 2,PAD_L, 40,0                   // left
    .byte 2,PAD_D, 40,0                   // down
    .byte 2,PAD_R, 40,0                   // right
    .byte 0,0
.endif
.ifdef AT_PAINT
    .byte 6,0
    .byte 2,PAD_D,2,0, 2,PAD_D,2,0       // launcher sel 0->4->8
    .byte 2,PAD_R,2,0, 2,PAD_R,2,0       // 8->9->10 (Paint)
    .byte 2,PAD_A, 6,0                    // launch Paint (cursor at 0,0, white)
    .byte 2,PAD_A,2,0                     // paint (0,0)
    .byte 2,PAD_R,2,0, 2,PAD_A,2,0       // (1,0)
    .byte 2,PAD_R,2,0, 2,PAD_A,2,0       // (2,0)
    .byte 2,PAD_R,2,0, 2,PAD_A,2,0       // (3,0)
    .byte 2,PAD_R,2,0, 2,PAD_A,2,0       // (4,0)
    .byte 2,PAD_U,2,0                     // row0 -> palette row (cursor carries x)
    .byte 2,PAD_L,2,0, 2,PAD_L,2,0, 2,PAD_L,2,0, 2,PAD_L,2,0   // clamp to swatch 0
    .byte 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0   // -> swatch 5 (red)
    .byte 2,PAD_A,2,0                     // pick red
    .byte 2,PAD_D,2,0                     // palette -> canvas row0 (cx=5)
    .byte 2,PAD_A,2,0                     // paint (5,0) red
    .byte 2,PAD_R,2,0, 2,PAD_A,2,0       // (6,0)
    .byte 2,PAD_D,2,0, 2,PAD_A,2,0       // (6,1)
    .byte 2,PAD_D,2,0, 2,PAD_A,2,0       // (6,2)
    .byte 0,0
.endif
.align 3
.section .text
.endif

// ============================================================================
// data: strings + content tables
// ============================================================================
.section .rodata
.align 2
icon_lbl:
    .word l_sysinfo, l_clock, l_notepad, l_music, l_files, l_theme
    .word l_tracker, l_dostris, l_outlast, l_pacman, l_paint
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
s_title:   .asciz "UnoDOS 3 - Raspberry Pi (AArch64)"
s_back:    .asciz "B = Back"
s_usb:     .asciz "USB:"
t_sysinfo: .asciz "SysInfo"
t_clock:   .asciz "Clock"
t_notepad: .asciz "Notepad"
t_music:   .asciz "Music"
t_files:   .asciz "Files"
t_theme:   .asciz "Theme"
t_dostris: .asciz "Dostris"
t_paint:   .asciz "Paint"
t_pacman:  .asciz "Pac-Man"
t_outlast: .asciz "OutLast"
t_tracker: .asciz "Tracker"
.align 2
c_sysinfo:
    .word 16, 48, m_si0
    .word 16, 96, m_si1
    .word 16, 116, m_si2
    .word 16, 136, m_si3
    .word 16, 172, m_si4
    .word 0xFFFFFFFF
m_si0: .asciz "UnoDOS 3 - Raspberry Pi"
m_si1: .asciz "CPU  ARM Cortex-A (AArch64)"
m_si2: .asciz "64-bit, GPU mailbox framebuffer"
m_si3: .asciz "Display  640x480x32 XRGB"
m_si4: .asciz "minimal profile"
.align 2
c_clock:
    .word 16, 48, m_cl0
    .word 16, 240, m_cl1
    .word 0xFFFFFFFF
m_cl0: .asciz "System clock"
m_cl1: .asciz "Time:"
.align 2
c_notepad:
    .word 16, 48, m_np0
    .word 16, 96, m_np1
    .word 16, 116, m_np2
    .word 0xFFFFFFFF
m_np0: .asciz "Notepad"
m_np1: .asciz "Ninth fresh port"
m_np2: .asciz "first AArch64 (64-bit) world"
.align 2
c_files:
    .word 16, 48, m_fi0
    .word 16, 96, m_fi1
    .word 16, 116, m_fi2
    .word 16, 136, m_fi3
    .word 0xFFFFFFFF
m_fi0: .asciz "Files - apps:"
m_fi1: .asciz "SYSINFO CLOCK NOTEPAD"
m_fi2: .asciz "FILES THEME MUSIC"
m_fi3: .asciz "(no disk - flat image)"
.align 2
c_theme:
    .word 16, 48, m_th0
    .word 16, 96, m_th1
    .word 16, 240, m_th2
    .word 0xFFFFFFFF
m_th0: .asciz "Theme presets"
m_th1: .asciz "A = cycle palette"
m_th2: .asciz "Preset:"
.align 2
c_music:
    .word 16, 48, m_mu0
    .word 16, 96, m_mu1
    .word 16, 130, m_mu2
    .word 0xFFFFFFFF
m_mu0: .asciz "Music player (PWM jack)"
m_mu1: .asciz "Ode to Joy"
m_mu2: .asciz "Note:"
.align 2
c_generic:
    .word 16, 48, m_ge0
    .word 16, 96, m_ge1
    .word 0xFFFFFFFF
m_ge0: .asciz "UnoDOS 3 / Raspberry Pi"
m_ge1: .asciz "Coming soon."
.section .text
