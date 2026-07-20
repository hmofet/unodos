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
    cmp   w0, #6
    b.eq  app_tracker
    cmp   w0, #7
    b.eq  app_dostris
    cmp   w0, #8
    b.eq  app_outlast
    cmp   w0, #9
    b.eq  app_pacman
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
    ldr   x2, =t_notepad
    bl    draw_chrome
    bl    notepad_draw
    ldp   x29, x30, [sp], #16
    ret
app_files:
    ldr   x2, =t_files
    bl    draw_chrome
    bl    files_draw
    ldp   x29, x30, [sp], #16
    ret
app_tracker:
    ldr   x2, =t_tracker
    bl    draw_chrome
    bl    tracker_draw
    ldp   x29, x30, [sp], #16
    ret
app_outlast:
    ldr   x2, =t_outlast
    bl    draw_chrome
    bl    ol_draw_scene
    ldp   x29, x30, [sp], #16
    ret
app_pacman:
    ldr   x2, =t_pacman
    bl    draw_chrome
    bl    pacman_draw
    ldp   x29, x30, [sp], #16
    ret
app_paint:
    ldr   x2, =t_paint
    bl    draw_chrome
    bl    paint_draw
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
// Notepad — USV1-backed (fs.inc.s). Loads NOTE.TXT on entry; A appends the
// next letter (A, B, C, ... by length) and saves, so the note is a real file
// Files lists and the harness --storage sidecar persists across runs.
// ============================================================================
notepad_enter:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =fn_note
    bl    fs_find
    cmn   w0, #1
    b.eq  np_seed
    ldr   x1, =np_buf
    mov   w2, #NP_MAX
    bl    fs_read                         // -> w0 = size
    ldr   x1, =np_len
    str   w0, [x1]
    ldr   x0, =np_saved
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret
np_seed:
    // no NOTE.TXT yet: seed the buffer, save nothing until the first edit
    ldr   x0, =np_seed_txt
    ldr   x1, =np_buf
    mov   w2, #(np_seed_end - np_seed_txt)
    ldr   x3, =np_len
    str   w2, [x3]
np_sc:
    ldrb  w3, [x0], #1
    strb  w3, [x1], #1
    subs  w2, w2, #1
    b.ne  np_sc
    ldr   x0, =np_saved
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret

// notepad_input: A = append one letter + save NOTE.TXT
notepad_input:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  npi_done
    ldr   x2, =np_len
    ldr   w0, [x2]
    cmp   w0, #NP_MAX
    b.hs  npi_done
    // append 'A' + (len mod 26)
    mov   w3, #26
    udiv  w4, w0, w3
    msub  w4, w4, w3, w0
    add   w4, w4, #'A'
    ldr   x1, =np_buf
    strb  w4, [x1, w0, uxtw]
    add   w0, w0, #1
    str   w0, [x2]
    // save
    ldr   x0, =fn_note
    ldr   x1, =np_buf
    ldr   x2, =np_len
    ldr   w2, [x2]
    bl    fs_save
    ldr   x0, =np_saved
    mov   w1, #1
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
npi_done:
    ldp   x29, x30, [sp], #16
    ret

// notepad_draw: status line + the note text wrapped at 48 columns
notepad_draw:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    // status: "NOTE.TXT NNN bytes" (or "(unsaved)" before the first save)
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #40
    ldr   x2, =s_np_file
    bl    pstr
    ldr   x0, =np_len
    ldr   w0, [x0]
    ldr   x1, =str_buf
    bl    fmt_dec
    mov   w0, #96
    mov   w1, #40
    ldr   x2, =str_buf
    bl    pstr
    mov   w0, #136
    mov   w1, #40
    ldr   x2, =s_np_bytes
    bl    pstr
    ldr   x0, =np_saved
    ldr   w0, [x0]
    cbnz  w0, npd_text
    mov   w0, #192
    mov   w1, #40
    ldr   x2, =s_np_unsaved
    bl    pstr
npd_text:
    // body: np_buf in 48-char lines from y=80
    ldr   x19, =np_buf
    ldr   x0, =np_len
    ldr   w20, [x0]                       // remaining
    mov   w21, #80                        // y
npd_line:
    cbz   w20, npd_help
    cmp   w21, #420
    b.hs  npd_help
    mov   w22, #48
    cmp   w20, w22
    csel  w22, w20, w22, lo               // this line's length
    ldr   x2, =str_buf
    mov   w0, #0
npd_cp:
    ldrb  w1, [x19], #1
    strb  w1, [x2, w0, uxtw]
    add   w0, w0, #1
    cmp   w0, w22
    b.ne  npd_cp
    strb  wzr, [x2, w0, uxtw]
    mov   w0, #16
    mov   w1, w21
    bl    pstr
    sub   w20, w20, w22
    add   w21, w21, #16
    b     npd_line
npd_help:
    mov   w0, #16
    mov   w1, #440
    ldr   x2, =s_np_help
    bl    pstr
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// Files — a real USV1 directory listing (name + size per entry)
// ============================================================================
files_draw:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #40
    ldr   x2, =s_fi_hdr
    bl    pstr
    ldr   x19, =FS_BASE
    ldr   w20, [x19, #4]                  // count
    mov   w21, #0                         // index
    mov   w22, #64                        // y
fid_ent:
    cmp   w21, w20
    b.hs  fid_sum
    add   x1, x19, #16
    add   x1, x1, w21, uxtw #4            // entry
    // name (12 bytes, NUL-padded) -> str_buf
    ldr   x2, =str_buf
    mov   w0, #0
fid_nm:
    ldrb  w3, [x1, w0, uxtw]
    strb  w3, [x2, w0, uxtw]
    add   w0, w0, #1
    cmp   w0, #12
    b.ne  fid_nm
    strb  wzr, [x2, w0, uxtw]
    mov   w0, #16
    mov   w1, w22
    bl    pstr
    // size
    add   x1, x19, #16
    add   x1, x1, w21, uxtw #4
    ldrh  w0, [x1, #12]
    ldr   x1, =str_buf
    bl    fmt_dec
    mov   w0, #200
    mov   w1, w22
    ldr   x2, =str_buf
    bl    pstr
    add   w21, w21, #1
    add   w22, w22, #16
    b     fid_ent
fid_sum:
    // "Files: N   Used: NNNN / 3840"
    mov   w0, #16
    mov   w1, #416
    ldr   x2, =s_fi_cnt
    bl    pstr
    mov   w0, w20
    ldr   x1, =str_buf
    bl    fmt_dec
    mov   w0, #72
    mov   w1, #416
    ldr   x2, =str_buf
    bl    pstr
    mov   w0, #16
    mov   w1, #432
    ldr   x2, =s_fi_used
    bl    pstr
    ldr   w0, [x19, #8]
    ldr   x1, =str_buf
    bl    fmt_dec
    mov   w0, #64
    mov   w1, #432
    ldr   x2, =str_buf
    bl    pstr
    mov   w0, #112
    mov   w1, #432
    ldr   x2, =s_fi_cap
    bl    pstr
    ldp   x21, x22, [sp], #16
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
.ifdef AT_TRACKER
    // launch icon 6 (R,R,D), cursor to row 2 / ch 1, cycle the cell twice
    // (.. -> C4 -> D4), then let playback run
    .byte 6,0,  2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_D, 4,0, 2,PAD_A, 8,0
    .byte 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_R,2,0, 2,PAD_A,2,0, 2,PAD_A, 4,0
    .byte 150,0, 0,0
.endif
.ifdef AT_OUTLAST
    // launch icon 8 (D,D), steer right three columns, then drive
    .byte 6,0,  2,PAD_D,2,0, 2,PAD_D, 4,0, 2,PAD_A, 8,0
    .byte 2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_R, 4,0, 130,0, 0,0
.endif
.ifdef AT_PACMAN
    // launch icon 9 (R,D,D); READY runs out, pac eats leftward, then turns
    .byte 6,0,  2,PAD_R,2,0, 2,PAD_D,2,0, 2,PAD_D, 4,0, 2,PAD_A, 8,0
    .byte 120,0, 2,PAD_U,2,0, 60,0, 2,PAD_L,2,0, 60,0, 0,0
.endif
.ifdef AT_PAINT
    // launch icon 10 (R,R,D,D); plot three red cells, dive to the palette row,
    // pick the 5th swatch (green), come back up and plot two more
    .byte 6,0,  2,PAD_R,2,0, 2,PAD_R,2,0, 2,PAD_D,2,0, 2,PAD_D, 4,0, 2,PAD_A, 8,0
    .byte 2,PAD_A,2,0, 2,PAD_R,2,0, 2,PAD_A,2,0, 2,PAD_R,2,0, 2,PAD_A, 4,0
    .byte 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D,2,0
    .byte 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D,2,0, 2,PAD_D, 4,0
    .byte 2,PAD_A, 4,0, 2,PAD_U,2,0, 2,PAD_A,2,0, 2,PAD_L,2,0, 2,PAD_A, 8,0, 0,0
.endif
.ifdef AT_STORE
    // Notepad (icon 2): append+save three letters, back out, open Files
    .byte 6,0,  2,PAD_R,2,0, 2,PAD_R, 4,0, 2,PAD_A, 8,0
    .byte 2,PAD_A,4,0, 2,PAD_A,4,0, 2,PAD_A, 8,0
    .byte 2,PAD_B,4,0, 2,PAD_L,2,0, 2,PAD_L,2,0, 2,PAD_D, 4,0, 2,PAD_A, 30,0, 0,0
.endif
.ifdef AT_FILES
    // straight to Files (icon 4): the listing shows what the store holds
    .byte 6,0,  2,PAD_D, 4,0, 2,PAD_A, 30,0, 0,0
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
t_sysinfo: .asciz "SysInfo"
t_clock:   .asciz "Clock"
t_notepad: .asciz "Notepad"
t_music:   .asciz "Music"
t_files:   .asciz "Files"
t_theme:   .asciz "Theme"
t_dostris: .asciz "Dostris"
t_tracker: .asciz "Tracker"
t_outlast: .asciz "OutLast"
t_pacman:  .asciz "Pac-Man"
t_paint:   .asciz "Paint"
s_np_file:    .asciz "NOTE.TXT"
s_np_bytes:   .asciz "bytes"
s_np_unsaved: .asciz "(unsaved)"
s_np_help:    .asciz "A = append + save   B = back"
np_seed_txt:  .ascii "RPI:"
np_seed_end:
s_fi_hdr:  .asciz "Name          Size   (USV1 store)"
s_fi_cnt:  .asciz "Files:"
s_fi_used: .asciz "Used:"
s_fi_cap:  .asciz "/ 3840 bytes"
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
