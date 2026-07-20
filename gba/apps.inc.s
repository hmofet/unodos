@ ============================================================================
@ UnoDOS / GBA — full-screen apps (M3), clock/theme/music, strings. ARM / GNU as.
@ ============================================================================

@ draw_chrome: r0 = title strptr -> apply palette, clear, title bar + footer
draw_chrome:
    push {r4, lr}
    mov r4, r0
    bl load_palette
    bl clear_screen
    mov r0, #1
    bl set_fg
    mov r0, #0
    mov r1, #0
    mov r2, #240
    mov r3, #14
    bl frect
    mov r0, #0
    mov r1, #1
    bl setfb
    mov r0, #4
    mov r1, #3
    mov r2, r4
    bl pstr
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #4
    mov r1, #150
    ldr r2, =s_back
    bl pstr
    pop {r4, pc}
.ltorg

@ draw_content: r0 = table of .word x,y,strptr ; end x=0xFFFFFFFF (white on desktop)
draw_content:
    push {r4, lr}
    mov r4, r0
    mov r0, #1
    mov r1, #0
    bl setfb
dc_l:
    ldr r0, [r4], #4
    cmn r0, #1
    beq dc_d
    ldr r1, [r4], #4
    ldr r2, [r4], #4
    bl pstr
    b dc_l
dc_d:
    pop {r4, pc}
.ltorg

@ ---- draw_app dispatch -----------------------------------------------------
draw_app:
    push {lr}
    ldr r0, =v_app
    ldr r0, [r0]
    cmp r0, #0
    beq app_sysinfo
    cmp r0, #1
    beq app_clock
    cmp r0, #2
    beq app_notepad
    cmp r0, #3
    beq app_music
    cmp r0, #4
    beq app_files
    cmp r0, #5
    beq app_theme
    cmp r0, #6
    beq app_tracker
    cmp r0, #7
    beq app_dostris
    cmp r0, #8
    beq app_outlast
    cmp r0, #9
    beq app_pacman
    cmp r0, #10
    beq app_paint
    b app_generic
app_sysinfo:
    ldr r0, =t_sysinfo
    bl draw_chrome
    ldr r0, =c_sysinfo
    bl draw_content
    pop {pc}
app_notepad:
    ldr r0, =t_notepad
    bl draw_chrome
    bl notepad_draw
    pop {pc}
app_files:
    ldr r0, =t_files
    bl draw_chrome
    bl files_draw
    pop {pc}
app_tracker:
    ldr r0, =l_tracker
    bl draw_chrome
    bl tracker_draw
    pop {pc}
app_outlast:
    bl outlast_fulldraw
    pop {pc}
app_pacman:
    bl pacman_draw
    pop {pc}
app_paint:
    bl paint_fulldraw
    pop {pc}
app_generic:
    ldr r0, =icon_lbl
    ldr r1, =v_app
    ldr r1, [r1]
    ldr r0, [r0, r1, lsl #2]
    bl draw_chrome
    ldr r0, =c_generic
    bl draw_content
    pop {pc}
app_dostris:
    ldr r0, =t_dostris
    bl draw_chrome
    bl dostris_draw
    pop {pc}
app_clock:
    ldr r0, =t_clock
    bl draw_chrome
    ldr r0, =c_clock
    bl draw_content
    bl clock_format
    bl draw_clock_time
    pop {pc}
app_theme:
    ldr r0, =t_theme
    bl draw_chrome
    ldr r0, =c_theme
    bl draw_content
    bl draw_theme_status
    pop {pc}
app_music:
    ldr r0, =t_music
    bl draw_chrome
    ldr r0, =c_music
    bl draw_content
    bl draw_music_status
    pop {pc}
.ltorg

@ ---- Clock -----------------------------------------------------------------
clock_advance:
    push {lr}
    ldr r3, =v_frac
    ldr r0, [r3]
    add r0, r0, #1
    cmp r0, #60
    blo ca_store_frac
    mov r0, #0
    str r0, [r3]
    ldr r3, =v_ss
    ldr r0, [r3]
    add r0, r0, #1
    cmp r0, #60
    blo ca_store_ss
    mov r0, #0
    str r0, [r3]
    ldr r3, =v_mm
    ldr r0, [r3]
    add r0, r0, #1
    cmp r0, #60
    blo ca_store_mm
    mov r0, #0
    str r0, [r3]
    ldr r3, =v_hh
    ldr r0, [r3]
    add r0, r0, #1
    cmp r0, #24
    movhs r0, #0
    str r0, [r3]
    b ca_fmt
ca_store_mm:
    str r0, [r3]
    b ca_fmt
ca_store_ss:
    str r0, [r3]
ca_fmt:
    bl clock_format
    ldr r0, =pf_clk
    mov r1, #1
    str r1, [r0]
    pop {pc}
ca_store_frac:
    str r0, [r3]
    pop {pc}
.ltorg

clock_format:
    push {r4, lr}
    ldr r4, =clk_str
    ldr r0, =v_hh
    ldr r0, [r0]
    bl two_digits
    strb r1, [r4, #0]
    strb r0, [r4, #1]
    mov r0, #':'
    strb r0, [r4, #2]
    ldr r0, =v_mm
    ldr r0, [r0]
    bl two_digits
    strb r1, [r4, #3]
    strb r0, [r4, #4]
    mov r0, #':'
    strb r0, [r4, #5]
    ldr r0, =v_ss
    ldr r0, [r0]
    bl two_digits
    strb r1, [r4, #6]
    strb r0, [r4, #7]
    mov r0, #0
    strb r0, [r4, #8]
    pop {r4, pc}
.ltorg

draw_clock_time:
    push {lr}
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #88
    mov r1, #84
    ldr r2, =clk_str
    bl pstr
    pop {pc}
.ltorg

@ ---- Theme -----------------------------------------------------------------
theme_input:
    push {lr}
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_A
    popeq {pc}
    ldr r2, =v_theme
    ldr r0, [r2]
    add r0, r0, #1
    cmp r0, #NTHEMES
    movhs r0, #0
    str r0, [r2]
    ldr r0, =v_dirty
    mov r1, #1
    str r1, [r0]
    pop {pc}

draw_theme_status:
    push {lr}
    ldr r0, =v_theme
    ldr r0, [r0]
    add r0, r0, #'0'
    ldr r1, =numstr
    strb r0, [r1]
    mov r0, #0
    strb r0, [r1, #1]
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #80
    mov r1, #84
    ldr r2, =numstr
    bl pstr
    pop {pc}
.ltorg

@ ---- Music (GBA square channel 1) ------------------------------------------
music_init:
    push {lr}
    ldr r0, =SNDCNT_X
    mov r1, #0x80
    strh r1, [r0]
    ldr r0, =SNDCNT_L
    ldr r1, =0x1177
    strh r1, [r0]
    ldr r0, =SNDCNT_H
    mov r1, #2
    strh r1, [r0]
    ldr r0, =m_idx
    mov r1, #0
    str r1, [r0]
    ldr r0, =m_play
    mov r1, #1
    str r1, [r0]
    bl music_load
    ldr r0, =pf_score
    mov r1, #1
    str r1, [r0]
    pop {pc}
music_silence:
    ldr r0, =SNDCNT_X
    mov r1, #0
    strh r1, [r0]
    ldr r0, =m_play
    str r1, [r0]
    bx lr
music_load:
    push {lr}
    ldr r0, =m_idx
    ldr r0, [r0]
    ldr r1, =music_song
    add r1, r1, r0, lsl #2
    ldrh r2, [r1]
    ldrb r3, [r1, #2]
    ldr r0, =m_timer
    str r3, [r0]
    ldr r0, =SND1CNT_H
    ldr r12, =0xF080
    strh r12, [r0]
    ldr r0, =SND1CNT_X
    orr r2, r2, #0x8000
    strh r2, [r0]
    pop {pc}
music_tick:
    push {lr}
    ldr r0, =m_play
    ldr r0, [r0]
    cmp r0, #0
    popeq {pc}
    ldr r3, =m_timer
    ldr r0, [r3]
    subs r0, r0, #1
    str r0, [r3]
    bne mt_done
    ldr r3, =m_idx
    ldr r0, [r3]
    add r0, r0, #1
    cmp r0, #MUSIC_COUNT
    movhs r0, #0
    str r0, [r3]
    bl music_load
    ldr r0, =pf_score
    mov r1, #1
    str r1, [r0]
mt_done:
    pop {pc}
.ltorg

draw_music_status:
    push {r4, lr}
    ldr r0, =m_idx
    ldr r0, [r0]
    add r0, r0, #1
    bl two_digits
    ldr r2, =numstr
    strb r1, [r2, #0]
    strb r0, [r2, #1]
    mov r0, #0
    strb r0, [r2, #2]
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #56
    mov r1, #60
    ldr r2, =numstr
    bl pstr
    @ progress bar: (m_idx cap 16)+1 cyan cells at (16,84)
    ldr r0, =m_idx
    ldr r0, [r0]
    cmp r0, #16
    movhs r0, #16
    add r0, r0, #1
    mov r4, r0, lsl #3
    mov r0, #2
    bl set_fg
    mov r0, #16
    mov r1, #84
    mov r2, r4
    mov r3, #8
    bl frect
    pop {r4, pc}
.ltorg

@ num5: r0 = 0..65535 -> numstr = 5 zero-padded decimal digits + NUL
num5:
    push {r4-r6, lr}
    ldr r4, =numstr
    ldr r5, =num5_pows
    mov r6, #5
n5_l:
    ldr r2, [r5], #4
    mov r1, #'0'
n5_d:
    cmp r0, r2
    blo n5_s
    sub r0, r0, r2
    add r1, r1, #1
    b n5_d
n5_s:
    strb r1, [r4], #1
    subs r6, r6, #1
    bne n5_l
    mov r1, #0
    strb r1, [r4]
    pop {r4-r6, pc}
.ltorg

@ ============================================================================
@ AUTOTEST: drive a scripted pad into the same input path. .hword frames, pad
@ (hwords so the shoulder buttons 0x100/0x200 fit in a script entry).
@ ============================================================================
.ifdef AUTOTEST
auto_input:
    ldr r3, =a_tmr
    ldr r1, [r3]
    cmp r1, #0
    bne ai_run
    ldr r0, =a_idx
    ldr r0, [r0]
    ldr r1, =auto_script
    add r1, r1, r0, lsl #2
    ldrh r2, [r1]
    cmp r2, #0
    bne ai_have
    ldr r0, =a_gpause
    mov r2, #1
    str r2, [r0]
    ldr r0, =v_pad
    mov r2, #0
    str r2, [r0]
    ldr r0, =v_pade
    str r2, [r0]
    bx lr
ai_have:
    ldr r0, =a_tmr
    str r2, [r0]
    ldrh r2, [r1, #2]
    ldr r0, =a_pad
    str r2, [r0]
    ldr r0, =a_idx
    ldr r2, [r0]
    add r2, r2, #1
    str r2, [r0]
ai_run:
    ldr r0, =v_pad
    ldr r1, [r0]
    ldr r2, =v_padp
    str r1, [r2]
    ldr r3, =a_tmr
    ldr r1, [r3]
    sub r1, r1, #1
    str r1, [r3]
    ldr r0, =a_pad
    ldr r1, [r0]
    ldr r0, =v_pad
    str r1, [r0]
    ldr r0, =v_padp
    ldr r0, [r0]
    mvn r0, r0
    and r1, r1, r0
    ldr r0, =v_pade
    str r1, [r0]
    bx lr
.ltorg

.section .rodata
.align 2
auto_script:
.ifdef AT_NAV
    .hword 8,0,  2,PAD_R, 4,0, 2,PAD_R, 4,0, 2,PAD_D, 30,0, 0,0
.endif
.ifdef AT_APP
    .hword 8,0,  2,PAD_A, 40,0, 0,0
.endif
.ifdef AT_CLOCK
    .hword 6,0,  2,PAD_R, 4,0, 2,PAD_A, 200,0, 0,0
.endif
.ifdef AT_THEME
    .hword 6,0,  2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R, 4,0, 2,PAD_A, 8,0, 2,PAD_A, 8,0, 2,PAD_A, 30,0, 0,0
.endif
.ifdef AT_MUSIC
    .hword 6,0,  2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R, 4,0, 2,PAD_A, 200,0, 0,0
.endif
.ifdef AT_DOSTRIS
    .hword 6,0,  2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_D, 4,0, 2,PAD_A, 6,0
    .hword 2,PAD_L,2,0, 2,PAD_L,2,0, 2,PAD_L,2,0, 2,PAD_L,2,0, 16,PAD_D, 34,0
    .hword 2,PAD_L,2,0, 2,PAD_L,2,0, 16,PAD_D, 34,0
    .hword 2,PAD_A,2,0, 16,PAD_D, 34,0
    .hword 2,PAD_R,2,0, 2,PAD_R,2,0, 16,PAD_D, 34,0
    .hword 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R,2,0, 16,PAD_D, 34,0
    .hword 2,PAD_L,2,0, 6,PAD_D, 2,0, 0,0
.endif
.ifdef AT_TRACKER
    @ launch Tracker (icon 6), cursor down 2, enter 2 notes, chan right, note,
    @ Start playback, let the play marker run
    .hword 8,0, 2,PAD_R,4,0, 2,PAD_R,4,0, 2,PAD_D,4,0, 2,PAD_A, 30,0
    .hword 2,PAD_D,4,0, 2,PAD_D,4,0, 2,PAD_A,4,0, 2,PAD_A,4,0
    .hword 2,PAD_R,4,0, 2,PAD_A,4,0, 2,PAD_ST, 90,0, 0,0
.endif
.ifdef AT_OUTLAST
    @ launch OutLast (icon 8), steer left twice, drive a while
    .hword 8,0, 2,PAD_D,4,0, 2,PAD_D,4,0, 2,PAD_A, 30,0
    .hword 2,PAD_L,2,0, 2,PAD_L,2,0, 120,0, 0,0
.endif
.ifdef AT_PACMAN
    @ launch Pac-Man (icon 9), Start, wait out READY, munch left then down
    .hword 8,0, 2,PAD_R,4,0, 2,PAD_D,4,0, 2,PAD_D,4,0, 2,PAD_A, 20,0
    .hword 2,PAD_ST, 70,0, 40,PAD_L, 20,0, 30,PAD_D, 20,0, 60,PAD_L, 30,0, 0,0
.endif
.ifdef AT_PAINT
    @ launch Paint (icon 10), stroke right, down, colour change, stroke left
    .hword 8,0, 2,PAD_R,4,0, 2,PAD_R,4,0, 2,PAD_D,4,0, 2,PAD_D,4,0, 2,PAD_A, 30,0
    .hword 24,(PAD_A|PAD_R), 4,0, 12,PAD_D, 4,0, 2,PAD_SR, 2,0
    .hword 24,(PAD_A|PAD_L), 4,0, 2,PAD_SR, 2,0, 8,PAD_A, 20,0, 0,0
.endif
.ifdef AT_FILES
    @ launch Files (icon 4): the USV1 listing
    .hword 8,0, 2,PAD_D,4,0, 2,PAD_A, 40,0, 0,0
.endif
.ifdef AT_NOTE
    @ launch Notepad (icon 2), append '+' three times (each press saves)
    .hword 8,0, 2,PAD_R,4,0, 2,PAD_R,4,0, 2,PAD_A, 20,0
    .hword 2,PAD_A,6,0, 2,PAD_A,6,0, 2,PAD_A,6,0, 20,0, 0,0
.endif
.align 2
.section .text
.endif

@ ============================================================================
@ data: strings + content tables
@ ============================================================================
.section .rodata
.align 2
@ per-app dispatch tables (indexed by v_app; up_none = no handler)
update_tab:
    .word up_none, up_none, notepad_update, music_tick, up_none, theme_input
    .word tracker_update, dostris_update, outlast_update, pacman_update, paint_update
init_tab:
    .word up_none, up_none, notepad_init, music_init, up_none, up_none
    .word tracker_init, dostris_init, outlast_init, pacman_init, paint_init
rp_tab:
    .word up_none, rp_clock, up_none, rp_music, up_none, up_none
    .word rp_tracker, rp_dostris, rp_outlast, rp_pacman, rp_paint
num5_pows:
    .word 10000, 1000, 100, 10, 1
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
s_title:   .asciz "UnoDOS 3"
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
    .word 8, 24, m_si0
    .word 8, 44, m_si1
    .word 8, 56, m_si2
    .word 8, 68, m_si3
    .word 8, 88, m_si4
    .word 0xFFFFFFFF
m_si0: .asciz "UnoDOS 3 - GBA"
m_si1: .asciz "CPU  ARM7TDMI"
m_si2: .asciz "     16.78 MHz"
m_si3: .asciz "LCD  240x160 Mode 3"
m_si4: .asciz "minimal profile"
.align 2
c_clock:
    .word 8, 24, m_cl0
    .word 8, 84, m_cl1
    .word 0xFFFFFFFF
m_cl0: .asciz "System clock"
m_cl1: .asciz "Time:"
.align 2
c_theme:
    .word 8, 24, m_th0
    .word 8, 44, m_th1
    .word 8, 84, m_th2
    .word 0xFFFFFFFF
m_th0: .asciz "Theme presets"
m_th1: .asciz "A = cycle palette"
m_th2: .asciz "Preset:"
.align 2
c_music:
    .word 8, 24, m_mu0
    .word 8, 44, m_mu1
    .word 8, 60, m_mu2
    .word 0xFFFFFFFF
m_mu0: .asciz "Music player"
m_mu1: .asciz "Ode to Joy"
m_mu2: .asciz "Note:"
.align 2
c_generic:
    .word 8, 24, m_ge0
    .word 8, 44, m_ge1
    .word 0xFFFFFFFF
m_ge0: .asciz "UnoDOS 3 / GBA"
m_ge1: .asciz "Coming soon."
.section .text
