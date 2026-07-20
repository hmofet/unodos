// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — Tracker (app 6). The shared 32-row x
// 4-channel pattern grid (genesis/c64 data model: one byte per cell, 0 = empty,
// 1..8 = C4..C5) with cursor editing and a moving play position. The Pi has ONE
// PWM square voice (the Music tone path), so playback voice-steals: each row the
// HIGHEST channel with a note wins the voice, an all-empty row gates it off —
// the same policy as the 1-voice tracker ports.
//
//   d-pad  move the cursor        A  cycle the cell (.. -> C4 -> ... -> C5 -> ..)
//   B      back (silences the PWM via enter_launcher)
// Playback free-runs from entry; the inverted cell is the cursor, the '>' row
// is the play position.
// ============================================================================

.equ TKR_ROWS, 32
.equ TKR_CH,   4
.equ TKR_RATE, 7                          // frames per row (~8.6 rows/sec)
.equ TKR_Y0,   40                         // first row y
.equ TKR_DY,   13                         // row pitch (8px font + 5)

tracker_init:
    stp   x29, x30, [sp, #-16]!
    // demo pattern -> tk_pat
    ldr   x0, =tkr_demo
    ldr   x1, =tk_pat
    mov   w2, #(TKR_ROWS*TKR_CH)
tki_cp:
    ldrb  w3, [x0], #1
    strb  w3, [x1], #1
    subs  w2, w2, #1
    b.ne  tki_cp
    ldr   x0, =tk_row
    str   wzr, [x0]
    ldr   x0, =tk_col
    str   wzr, [x0]
    ldr   x0, =tk_prow
    mov   w1, #(TKR_ROWS-1)               // first tick wraps to row 0
    str   w1, [x0]
    ldr   x0, =tk_tmr
    mov   w1, #1
    str   w1, [x0]
    // PWM clock up (same bring-up as music_init; the voice itself stays off
    // until the first played note)
    ldr   x0, =CM_PWMCTL
    ldr   w1, =0x5A000000
    str   w1, [x0]
    ldr   x0, =CM_PWMDIV
    ldr   w1, =0x5A002000                 // DIVI = 2 -> PWM_CLK 9.6 MHz
    str   w1, [x0]
    ldr   x0, =CM_PWMCTL
    ldr   w1, =0x5A000211
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// tracker_update: cursor moves / cell edit / playback advance (one frame)
tracker_update:
    stp   x29, x30, [sp, #-16]!
    // snapshot cursor for the partial redraw
    ldr   x0, =tk_row
    ldr   w0, [x0]
    ldr   x1, =tk_orow
    str   w0, [x1]
    ldr   x0, =tk_col
    ldr   w0, [x0]
    ldr   x1, =tk_ocol
    str   w0, [x1]
    // ---- input edges ----
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_U
    b.eq  tku_d
    ldr   x2, =tk_row
    ldr   w0, [x2]
    cbnz  w0, tku_updec
    mov   w0, #TKR_ROWS
tku_updec:
    sub   w0, w0, #1
    str   w0, [x2]
    bl    tkr_mark
tku_d:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_D
    b.eq  tku_l
    ldr   x2, =tk_row
    ldr   w0, [x2]
    add   w0, w0, #1
    cmp   w0, #TKR_ROWS
    csel  w0, wzr, w0, hs
    str   w0, [x2]
    bl    tkr_mark
tku_l:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_L
    b.eq  tku_r
    ldr   x2, =tk_col
    ldr   w0, [x2]
    cbnz  w0, tku_ldec
    mov   w0, #TKR_CH
tku_ldec:
    sub   w0, w0, #1
    str   w0, [x2]
    bl    tkr_mark
tku_r:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_R
    b.eq  tku_a
    ldr   x2, =tk_col
    ldr   w0, [x2]
    add   w0, w0, #1
    cmp   w0, #TKR_CH
    csel  w0, wzr, w0, hs
    str   w0, [x2]
    bl    tkr_mark
tku_a:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  tku_play
    // cycle the cursor cell: 0..8 wrap
    bl    tkr_cur_ptr                     // x0 = cell ptr
    ldrb  w1, [x0]
    add   w1, w1, #1
    cmp   w1, #9
    csel  w1, wzr, w1, hs
    strb  w1, [x0]
    bl    tkr_mark
tku_play:
    // ---- playback: advance one row per TKR_RATE frames ----
    ldr   x2, =tk_tmr
    ldr   w0, [x2]
    sub   w0, w0, #1
    str   w0, [x2]
    cbnz  w0, tku_done
    mov   w0, #TKR_RATE
    str   w0, [x2]
    ldr   x2, =tk_prow
    ldr   w0, [x2]
    ldr   x1, =tk_oprow
    str   w0, [x1]
    add   w0, w0, #1
    cmp   w0, #TKR_ROWS
    csel  w0, wzr, w0, hs
    str   w0, [x2]
    bl    tkr_trigger
    bl    tkr_mark
tku_done:
    ldp   x29, x30, [sp], #16
    ret

// tkr_mark: set the tracker partial-redraw flag. Leaf.
tkr_mark:
    ldr   x0, =pf_tk
    mov   w1, #1
    str   w1, [x0]
    ret

// tkr_cur_ptr: -> x0 = &tk_pat[tk_row*4 + tk_col]. Leaf.
tkr_cur_ptr:
    ldr   x0, =tk_row
    ldr   w0, [x0]
    lsl   w0, w0, #2
    ldr   x1, =tk_col
    ldr   w1, [x1]
    add   w0, w0, w1
    ldr   x1, =tk_pat
    add   x0, x1, w0, uxtw
    ret

// tkr_trigger: play row tk_prow through the single PWM voice. The highest
// channel with a non-zero note steals the voice; all-empty rows gate it off.
tkr_trigger:
    ldr   x0, =tk_prow
    ldr   w0, [x0]
    lsl   w0, w0, #2
    ldr   x1, =tk_pat
    add   x1, x1, w0, uxtw                // row base
    mov   w2, #0                          // winning note
    mov   w3, #0                          // channel
tkt_scan:
    ldrb  w4, [x1, w3, uxtw]
    cmp   w4, #0
    csel  w2, w2, w4, eq                  // keep old if empty, else take (higher
    add   w3, w3, #1                      //  channels scan later -> they win)
    cmp   w3, #TKR_CH
    b.ne  tkt_scan
    cbz   w2, tkt_off
    sub   w2, w2, #1
    ldr   x0, =tkr_freq
    add   x0, x0, w2, uxtw #1
    ldrh  w4, [x0]                        // note frequency (Hz)
    ldr   w5, =PWM_CLK_HZ
    udiv  w5, w5, w4                      // RNG1 = clk / freq
    ldr   x0, =PWM_RNG1
    str   w5, [x0]
    lsr   w5, w5, #1
    ldr   x0, =PWM_DAT1
    str   w5, [x0]
    ldr   x0, =PWM_CTL
    mov   w1, #0x81                       // PWEN1 | MSEN1
    str   w1, [x0]
    ret
tkt_off:
    ldr   x0, =PWM_CTL
    str   wzr, [x0]
    ret

// ============================================================================
// rendering
// ============================================================================
// tkr_draw_row: w0 = row index -> the whole text row (marker, number, 4 cells)
tkr_draw_row:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, w0                         // row
    mov   w1, #TKR_DY
    mul   w20, w0, w1
    add   w20, w20, #TKR_Y0               // y
    // play marker '>' (or the erasing space)
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    ldr   x0, =tk_prow
    ldr   w0, [x0]
    cmp   w0, w19
    mov   w2, #'>'
    mov   w3, #' '
    csel  w2, w2, w3, eq
    mov   w0, #40
    mov   w1, w20
    bl    pchar
    // row number
    mov   w0, w19
    bl    two_digits
    ldr   x2, =str_buf
    strb  w1, [x2, #0]
    strb  w0, [x2, #1]
    strb  wzr, [x2, #2]
    mov   w0, #16
    mov   w1, w20
    bl    pstr
    // 4 channel cells at x = 64 + ch*48
    mov   w21, #0                         // channel
tdr_ch:
    // fg/bg: inverted when this is the cursor cell
    ldr   x0, =tk_row
    ldr   w0, [x0]
    cmp   w0, w19
    b.ne  tdr_norm
    ldr   x0, =tk_col
    ldr   w0, [x0]
    cmp   w0, w21
    b.ne  tdr_norm
    mov   w0, #0
    mov   w1, #1
    b     tdr_set
tdr_norm:
    mov   w0, #1
    mov   w1, #0
tdr_set:
    bl    setfb
    // cell text: ".." or note name
    lsl   w0, w19, #2
    add   w0, w0, w21
    ldr   x1, =tk_pat
    ldrb  w22, [x1, w0, uxtw]             // note value
    ldr   x2, =str_buf
    cbz   w22, tdr_empty
    sub   w22, w22, #1
    ldr   x1, =tkr_names
    add   x1, x1, w22, uxtw #1
    ldrb  w0, [x1]
    strb  w0, [x2, #0]
    ldrb  w0, [x1, #1]
    strb  w0, [x2, #1]
    b     tdr_put
tdr_empty:
    mov   w0, #'.'
    strb  w0, [x2, #0]
    strb  w0, [x2, #1]
tdr_put:
    strb  wzr, [x2, #2]
    mov   w0, #48
    mul   w0, w21, w0
    add   w0, w0, #64
    mov   w1, w20
    bl    pstr
    add   w21, w21, #1
    cmp   w21, #TKR_CH
    b.ne  tdr_ch
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// tracker_partial: repaint just the rows a cursor move / play advance touched
tracker_partial:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =tk_orow
    ldr   w0, [x0]
    bl    tkr_draw_row
    ldr   x0, =tk_row
    ldr   w0, [x0]
    bl    tkr_draw_row
    ldr   x0, =tk_oprow
    ldr   w0, [x0]
    bl    tkr_draw_row
    ldr   x0, =tk_prow
    ldr   w0, [x0]
    bl    tkr_draw_row
    ldp   x29, x30, [sp], #16
    ret

// app_tracker full redraw: chrome + header + all 32 rows
tracker_draw:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #24
    ldr   x2, =s_tk_hdr
    bl    pstr
    mov   w0, #360
    mov   w1, #24
    ldr   x2, =s_tk_help
    bl    pstr
    mov   w19, #0
tkd_rows:
    mov   w0, w19
    bl    tkr_draw_row
    add   w19, w19, #1
    cmp   w19, #TKR_ROWS
    b.ne  tkd_rows
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

.section .rodata
s_tk_hdr:  .asciz "Row   Ch1   Ch2   Ch3   Ch4"
s_tk_help: .asciz "A = edit cell  (1 PWM voice)"
tkr_names: .ascii "C4D4E4F4G4A4B4C5"
.align 1
tkr_freq:  .hword 262, 294, 330, 349, 392, 440, 494, 523
// demo pattern: 32 rows x 4 channels (ch1 bass, ch2 arp, ch3 melody, ch4 top) —
// the genesis/amiga demo riff re-voiced onto the 1..8 (C4..C5) note range.
tkr_demo:
    .byte 1,0,0,0,  0,0,0,0,  0,3,0,0,  0,0,0,0
    .byte 1,0,5,0,  0,0,0,0,  0,3,0,0,  0,0,0,8
    .byte 5,0,0,0,  0,0,3,0,  0,0,0,0,  0,0,0,0
    .byte 5,0,1,0,  0,0,3,0,  0,6,0,0,  0,0,0,8
    .byte 4,0,0,0,  0,0,0,0,  0,6,0,0,  0,0,0,0
    .byte 4,0,6,0,  0,0,0,0,  0,6,0,0,  0,0,0,8
    .byte 5,0,0,0,  0,0,2,0,  0,0,0,0,  0,0,0,0
    .byte 5,0,8,0,  0,0,2,0,  0,3,0,0,  0,0,0,8
.align 2
.section .text
