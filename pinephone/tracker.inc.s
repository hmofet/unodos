// ============================================================================
// UnoDOS / PinePhone (AArch64) — Tracker. The 32-row x 4-channel pattern
// sequencer (genesis/tracker.i pattern shape, c64/tracker.s cell semantics:
// a cell is 0 empty or 1..8 = C4..C5). The PinePhone has ONE software PCM
// voice (the square synth feeding the I2S TX FIFO), so playback VOICE-STEALS:
// each row is scanned ch1..ch4 and the last non-empty cell wins the voice —
// the platform-honest take on the 3-voice SID / 4-channel PSG trackers.
//
// Portrait layout: all 32 rows visible at once (13 px row pitch).
// D-pad = cursor, A = cycle the cell's note (audible preview), SELECT(o) =
// clear the cell, START(p) = play/stop, B = back.
// ============================================================================

.equ TK_RATE, 8                  // frames per row (~7.5 rows/s)
.equ TK_GATE, 6                  // tone length (frames)

tracker_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =tk_demo                    // load the demo pattern
    ldr   x1, =tk_pat
    mov   w2, #(TK_ROWS*TK_CHANS)
tki_cp:
    ldrb  w3, [x0], #1
    strb  w3, [x1], #1
    subs  w2, w2, #1
    b.ne  tki_cp
    ldr   x0, =tk_row
    str   wzr, [x0]
    ldr   x0, =tk_chan
    str   wzr, [x0]
    ldr   x0, =tk_orow
    str   wzr, [x0]
    ldr   x0, =tk_ochan
    str   wzr, [x0]
    ldr   x0, =tk_play
    str   wzr, [x0]
    ldr   x0, =tk_prow
    str   wzr, [x0]
    ldr   x0, =tk_gate
    str   wzr, [x0]
    mov   w1, #TK_RATE
    ldr   x0, =tk_ctr
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// tk_draw_cell: w0 = row, w1 = chan — note text, inverted when it's the cursor
tk_draw_cell:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, w0
    mov   w20, w1
    ldr   x0, =tk_row
    ldr   w0, [x0]
    cmp   w0, w19
    b.ne  tdc_n
    ldr   x0, =tk_chan
    ldr   w0, [x0]
    cmp   w0, w20
    b.ne  tdc_n
    mov   w0, #0                          // cursor cell: inverted
    mov   w1, #1
    b     tdc_s
tdc_n:
    mov   w0, #1
    mov   w1, #0
tdc_s:
    bl    setfb
    lsl   w1, w19, #2                     // value = pat[row*4+chan]
    add   w1, w1, w20
    ldr   x0, =tk_pat
    ldrb  w21, [x0, w1, uxtw]
    ldr   x2, =numstr
    cbz   w21, tdc_e
    sub   w1, w21, #1                     // note letter + octave
    ldr   x0, =tk_letters
    ldrb  w3, [x0, w1, uxtw]
    strb  w3, [x2]
    mov   w3, #'4'
    cmp   w21, #8
    b.ne  tdc_o
    mov   w3, #'5'
tdc_o:
    strb  w3, [x2, #1]
    b     tdc_p
tdc_e:
    mov   w3, #'.'
    strb  w3, [x2]
    strb  w3, [x2, #1]
tdc_p:
    strb  wzr, [x2, #2]
    mov   w0, #56                         // x = 48 + chan*56, y = 40 + row*13
    mul   w0, w20, w0
    add   w0, w0, #48
    mov   w1, #13
    mul   w1, w19, w1
    add   w1, w1, #40
    bl    pstr
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// tk_draw_mark: w0 = row, w1 = char ('>' or ' ') — the playback marker column
tk_draw_mark:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, w0
    mov   w20, w1
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w2, w20
    mov   w0, #28
    mov   w1, #13
    mul   w1, w19, w1
    add   w1, w1, #40
    bl    pchar
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// tk_move_redraw: repaint the old cursor cell plainly + the new one inverted
tk_move_redraw:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =tk_orow
    ldr   w0, [x0]
    ldr   x1, =tk_ochan
    ldr   w1, [x1]
    bl    tk_draw_cell
    ldr   x0, =tk_row
    ldr   w0, [x0]
    ldr   x1, =tk_chan
    ldr   w1, [x1]
    bl    tk_draw_cell
    ldr   x0, =tk_row                     // old = new (chained moves in a frame)
    ldr   w0, [x0]
    ldr   x1, =tk_orow
    str   w0, [x1]
    ldr   x0, =tk_chan
    ldr   w0, [x0]
    ldr   x1, =tk_ochan
    str   w0, [x1]
    ldp   x29, x30, [sp], #16
    ret

// tk_trigger_row: scan tk_prow's channels; the LAST non-empty cell steals the
// single PCM voice. Leaf.
tk_trigger_row:
    ldr   x0, =tk_prow
    ldr   w0, [x0]
    lsl   w0, w0, #2
    ldr   x1, =tk_pat
    add   x1, x1, w0, uxtw
    mov   w2, #0
    mov   w3, #0
ttr_l:
    ldrb  w4, [x1, w2, uxtw]
    cbz   w4, ttr_n
    sub   w4, w4, #1
    ldr   x5, =tk_freqs
    ldrh  w3, [x5, w4, uxtw #1]           // later channels override = steal
ttr_n:
    add   w2, w2, #1
    cmp   w2, #TK_CHANS
    b.ne  ttr_l
    cbz   w3, ttr_d
    ldr   x0, =m_freq
    str   w3, [x0]
    ldr   x0, =m_phase
    str   wzr, [x0]
    mov   w4, #TK_GATE
    ldr   x0, =tk_gate
    str   w4, [x0]
ttr_d:
    ret

// tk_cycle: A — cell = (cell+1) mod 9, with an audible preview
tk_cycle:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =tk_row
    ldr   w0, [x0]
    lsl   w0, w0, #2
    ldr   x1, =tk_chan
    ldr   w1, [x1]
    add   w0, w0, w1
    ldr   x1, =tk_pat
    ldrb  w2, [x1, w0, uxtw]
    add   w2, w2, #1
    cmp   w2, #9
    csel  w2, wzr, w2, hs
    strb  w2, [x1, w0, uxtw]
    cbz   w2, tkc_nod
    sub   w3, w2, #1                      // preview the new note
    ldr   x4, =tk_freqs
    ldrh  w3, [x4, w3, uxtw #1]
    ldr   x0, =m_freq
    str   w3, [x0]
    ldr   x0, =m_phase
    str   wzr, [x0]
    mov   w3, #TK_GATE
    ldr   x0, =tk_gate
    str   w3, [x0]
tkc_nod:
    ldr   x0, =tk_row
    ldr   w0, [x0]
    ldr   x1, =tk_chan
    ldr   w1, [x1]
    bl    tk_draw_cell
    ldp   x29, x30, [sp], #16
    ret

// tk_clearcell: SELECT — empty the cursor cell
tk_clearcell:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =tk_row
    ldr   w0, [x0]
    lsl   w0, w0, #2
    ldr   x1, =tk_chan
    ldr   w1, [x1]
    add   w0, w0, w1
    ldr   x1, =tk_pat
    strb  wzr, [x1, w0, uxtw]
    ldr   x0, =tk_row
    ldr   w0, [x0]
    ldr   x1, =tk_chan
    ldr   w1, [x1]
    bl    tk_draw_cell
    ldp   x29, x30, [sp], #16
    ret

// tk_toggle: START — play/stop (full redraw refreshes marker + status line)
tk_toggle:
    stp   x29, x30, [sp, #-16]!
    ldr   x2, =tk_play
    ldr   w0, [x2]
    eor   w0, w0, #1
    str   w0, [x2]
    cbz   w0, tkt_stop
    ldr   x1, =tk_prow
    str   wzr, [x1]
    mov   w1, #TK_RATE
    ldr   x3, =tk_ctr
    str   w1, [x3]
    bl    tk_trigger_row                  // row 0 sounds immediately
    b     tkt_d
tkt_stop:
    ldr   x0, =m_freq
    str   wzr, [x0]
tkt_d:
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// tracker_update: one frame — input edges, the row sequencer, the PCM voice
tracker_update:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    ldr   x0, =tk_row                     // snapshot the cursor for redraw
    ldr   w0, [x0]
    ldr   x1, =tk_orow
    str   w0, [x1]
    ldr   x0, =tk_chan
    ldr   w0, [x0]
    ldr   x1, =tk_ochan
    str   w0, [x1]
    ldr   x19, =v_pade
    ldr   w19, [x19]
    tst   w19, #PAD_U
    b.eq  tku_1
    ldr   x2, =tk_row
    ldr   w0, [x2]
    cbnz  w0, tku_ud
    mov   w0, #TK_ROWS
tku_ud:
    sub   w0, w0, #1
    str   w0, [x2]
    bl    tk_move_redraw
tku_1:
    tst   w19, #PAD_D
    b.eq  tku_2
    ldr   x2, =tk_row
    ldr   w0, [x2]
    add   w0, w0, #1
    cmp   w0, #TK_ROWS
    csel  w0, wzr, w0, hs
    str   w0, [x2]
    bl    tk_move_redraw
tku_2:
    tst   w19, #PAD_L
    b.eq  tku_3
    ldr   x2, =tk_chan
    ldr   w0, [x2]
    cbnz  w0, tku_ld
    mov   w0, #TK_CHANS
tku_ld:
    sub   w0, w0, #1
    str   w0, [x2]
    bl    tk_move_redraw
tku_3:
    tst   w19, #PAD_R
    b.eq  tku_4
    ldr   x2, =tk_chan
    ldr   w0, [x2]
    add   w0, w0, #1
    cmp   w0, #TK_CHANS
    csel  w0, wzr, w0, hs
    str   w0, [x2]
    bl    tk_move_redraw
tku_4:
    tst   w19, #PAD_A
    b.eq  tku_5
    bl    tk_cycle
tku_5:
    tst   w19, #PAD_SEL
    b.eq  tku_6
    bl    tk_clearcell
tku_6:
    tst   w19, #PAD_ST
    b.eq  tku_7
    bl    tk_toggle
tku_7:
    ldr   x0, =tk_play                    // sequencer
    ldr   w0, [x0]
    cbz   w0, tku_gate
    ldr   x2, =tk_ctr
    ldr   w0, [x2]
    subs  w0, w0, #1
    str   w0, [x2]
    b.ne  tku_gate
    mov   w0, #TK_RATE
    str   w0, [x2]
    ldr   x0, =tk_prow                    // erase the old marker
    ldr   w0, [x0]
    mov   w1, #' '
    bl    tk_draw_mark
    ldr   x2, =tk_prow
    ldr   w0, [x2]
    add   w0, w0, #1
    cmp   w0, #TK_ROWS
    csel  w0, wzr, w0, hs
    str   w0, [x2]
    bl    tk_trigger_row
    ldr   x0, =tk_prow
    ldr   w0, [x0]
    mov   w1, #'>'
    bl    tk_draw_mark
tku_gate:
    ldr   x2, =tk_gate                    // note-length gate -> silence
    ldr   w0, [x2]
    cbz   w0, tku_gen
    subs  w0, w0, #1
    str   w0, [x2]
    b.ne  tku_gen
    ldr   x0, =m_freq
    str   wzr, [x0]
tku_gen:
    bl    music_gen                       // synthesise this frame's PCM
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// app_tracker: full-screen draw (chrome + header + all 32 rows + status)
app_tracker:
    ldr   x2, =l_tracker
    bl    draw_chrome
    stp   x19, x20, [sp, #-16]!
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #8
    mov   w1, #24
    ldr   x2, =s_tk_hdr
    bl    pstr
    mov   w19, #0
atk_r:
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, w19
    bl    fmt_dec
    mov   w0, #8
    mov   w1, #13
    mul   w1, w19, w1
    add   w1, w1, #40
    ldr   x2, =numstr
    bl    pstr
    mov   w20, #0
atk_c:
    mov   w0, w19
    mov   w1, w20
    bl    tk_draw_cell
    add   w20, w20, #1
    cmp   w20, #TK_CHANS
    b.ne  atk_c
    add   w19, w19, #1
    cmp   w19, #TK_ROWS
    b.ne  atk_r
    ldr   x0, =tk_play
    ldr   w0, [x0]
    cbz   w0, atk_st
    ldr   x0, =tk_prow
    ldr   w0, [x0]
    mov   w1, #'>'
    bl    tk_draw_mark
atk_st:
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    ldr   x0, =tk_play
    ldr   w0, [x0]
    cbz   w0, atk_h
    ldr   x2, =s_tk_playing
    b     atk_p
atk_h:
    ldr   x2, =s_tk_help
atk_p:
    mov   w0, #8
    mov   w1, #452
    bl    pstr
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

.section .rodata
tk_letters:   .ascii "CDEFGABC"
s_tk_hdr:     .asciz "Row  Ch1    Ch2    Ch3    Ch4"
s_tk_help:    .asciz "A=note o=clear p=play"
s_tk_playing: .asciz "PLAYING (voice-steal)  p=stop"
.section .text
