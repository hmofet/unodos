# ============================================================================
# UnoDOS / PowerPC Mac — Tracker. A 32-step x 4-channel pattern grid (the
# Amiga/Genesis pattern shape) played through the port's ONE software square-
# wave PCM voice: each row trigger scans ch0..ch3 and the first non-empty cell
# steals the voice (like the 1-bit-speaker Trackers). Auto-plays on entry.
#
#   d-pad     move the cell cursor (wraps)
#   A         cycle the cell's note  . -> C4 .. C5 -> .   (and preview it)
#   B         back to the launcher
#
# Cell values: 0 empty, 1..8 = C4 D4 E4 F4 G4 A4 B4 C5. The play row is marked
# with a '>' in the margin and advances every TKRATE frames.
# ============================================================================

.equ TKROWS, 32
.equ TKCH,   4
.equ TKRATE, 12                 # frames per row

tracker_init:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LA    r3, tk_pat
    LA    r4, tk_demo
    li    r5, (TKROWS*TKCH)
    mtctr r5
tki_c:
    lbz   r0, 0(r4)
    stb   r0, 0(r3)
    addi  r3, r3, 1
    addi  r4, r4, 1
    bdnz  tki_c
    li    r3, 0
    STWA  r3, tk_row, r4
    STWA  r3, tk_ch, r4
    STWA  r3, tk_orow, r4
    STWA  r3, tk_och, r4
    STWA  r3, tk_prow, r4
    STWA  r3, tk_oprow, r4
    STWA  r3, tk_pf, r4
    STWA  r3, m_freq, r4
    STWA  r3, m_phase, r4
    li    r3, TKRATE
    STWA  r3, tk_ctr, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

tk_savecur:
    LWZA  r3, tk_row
    STWA  r3, tk_orow, r4
    LWZA  r3, tk_ch
    STWA  r3, tk_och, r4
    blr
tk_markmove:
    LWZA  r3, tk_pf
    ori   r3, r3, 1
    STWA  r3, tk_pf, r4
    blr

# tk_setfreq: r3 = note value 0..8 -> m_freq (0 = rest), phase restarted. Leaf.
tk_setfreq:
    cmpwi r3, 0
    beq   tsf_off
    subi  r3, r3, 1
    slwi  r3, r3, 1
    LA    r4, tk_freqs
    lhzx  r3, r4, r3
    STWA  r3, m_freq, r4
    li    r3, 0
    STWA  r3, m_phase, r4
    blr
tsf_off:
    li    r3, 0
    STWA  r3, m_freq, r4
    blr

# tk_trigger: play row tk_prow — first non-empty channel steals the voice.
tk_trigger:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, tk_prow
    slwi  r3, r3, 2
    LA    r4, tk_pat
    add   r4, r4, r3
    li    r5, 0
tkt_l:
    lbz   r3, 0(r4)
    cmpwi r3, 0
    bne   tkt_hit
    addi  r4, r4, 1
    addi  r5, r5, 1
    cmpwi r5, TKCH
    bne   tkt_l
    li    r3, 0                           # all empty -> rest
tkt_hit:
    bl    tk_setfreq
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# tracker_update: one frame — cursor nav, A edit, playback + PCM generation.
tracker_update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_pade
    andi. r0, r3, PAD_U
    beq   tku_1
    bl    tk_savecur
    LWZA  r3, tk_row
    cmpwi r3, 0
    bne   tku_ud
    li    r3, TKROWS
tku_ud:
    subi  r3, r3, 1
    STWA  r3, tk_row, r4
    bl    tk_markmove
tku_1:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_D
    beq   tku_2
    bl    tk_savecur
    LWZA  r3, tk_row
    addi  r3, r3, 1
    cmpwi r3, TKROWS
    blt   tku_ds
    li    r3, 0
tku_ds:
    STWA  r3, tk_row, r4
    bl    tk_markmove
tku_2:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_L
    beq   tku_3
    bl    tk_savecur
    LWZA  r3, tk_ch
    cmpwi r3, 0
    bne   tku_ld
    li    r3, TKCH
tku_ld:
    subi  r3, r3, 1
    STWA  r3, tk_ch, r4
    bl    tk_markmove
tku_3:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_R
    beq   tku_4
    bl    tk_savecur
    LWZA  r3, tk_ch
    addi  r3, r3, 1
    cmpwi r3, TKCH
    blt   tku_rs
    li    r3, 0
tku_rs:
    STWA  r3, tk_ch, r4
    bl    tk_markmove
tku_4:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_A
    beq   tku_play
    bl    tk_savecur
    LWZA  r3, tk_row
    slwi  r3, r3, 2
    LWZA  r4, tk_ch
    add   r3, r3, r4
    LA    r4, tk_pat
    lbzx  r5, r4, r3
    addi  r5, r5, 1
    cmpwi r5, 9
    blt   tku_st
    li    r5, 0
tku_st:
    stbx  r5, r4, r3
    mr    r3, r5
    bl    tk_setfreq                      # preview the edit
    bl    tk_markmove
tku_play:
    LWZA  r3, tk_ctr
    subi  r3, r3, 1
    STWA  r3, tk_ctr, r4
    cmpwi r3, 0
    bne   tku_gen
    li    r3, TKRATE
    STWA  r3, tk_ctr, r4
    LWZA  r3, tk_prow
    STWA  r3, tk_oprow, r4
    addi  r3, r3, 1
    andi. r3, r3, (TKROWS-1)
    STWA  r3, tk_prow, r4
    bl    tk_trigger
    LWZA  r3, tk_pf
    ori   r3, r3, 2
    STWA  r3, tk_pf, r4
tku_gen:
    bl    music_gen                       # synthesize this frame's PCM
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ============================================================================
# rendering
# ============================================================================
# tk_cell_xy: r3 = row, r4 = ch -> r3 = px, r4 = py. Leaf.
tk_cell_xy:
    mulli r5, r4, 56
    addi  r5, r5, 72
    mulli r4, r3, 13
    addi  r4, r4, 40
    mr    r3, r5
    blr

# tk_cell_draw: r3 = row, r4 = ch — note text, inverted when it's the cursor.
tk_cell_draw:
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    stw   r18, 24(r1)
    mr    r14, r3                         # row
    mr    r15, r4                         # ch
    bl    tk_cell_xy
    mr    r16, r3                         # px
    mr    r17, r4                         # py
    LWZA  r3, tk_row
    cmpw  r3, r14
    bne   tcd_norm
    LWZA  r3, tk_ch
    cmpw  r3, r15
    bne   tcd_norm
    li    r3, 0
    li    r4, 1
    b     tcd_set
tcd_norm:
    li    r3, 1
    li    r4, 0
tcd_set:
    bl    setfb
    slwi  r3, r14, 2
    add   r3, r3, r15
    LA    r4, tk_pat
    lbzx  r18, r4, r3                     # cell value
    cmpwi r18, 0
    bne   tcd_note
    li    r5, '.'
    mr    r3, r16
    mr    r4, r17
    bl    pchar
    li    r5, '.'
    addi  r3, r16, 8
    mr    r4, r17
    bl    pchar
    b     tcd_done
tcd_note:
    LA    r4, tk_names
    subi  r3, r18, 1
    lbzx  r5, r4, r3                      # note letter
    mr    r3, r16
    mr    r4, r17
    bl    pchar
    li    r5, '4'
    cmpwi r18, 8
    bne   tcd_oct
    li    r5, '5'
tcd_oct:
    addi  r3, r16, 8
    mr    r4, r17
    bl    pchar
tcd_done:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r18, 24(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

# tk_marker_draw: r3 = row, r4 = char (' ' erase / '>' draw)
tk_marker_draw:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    mr    r14, r4
    mulli r4, r3, 13
    addi  r4, r4, 40
    stw   r4, 12(r1)
    li    r3, 2
    li    r4, 0
    bl    setfb
    li    r3, 48
    lwz   r4, 12(r1)
    mr    r5, r14
    bl    pchar
    lwz   r14, 8(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

tracker_partial:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, tk_pf
    andi. r0, r3, 1
    beq   tp_2
    LWZA  r3, tk_orow
    LWZA  r4, tk_och
    bl    tk_cell_draw
    LWZA  r3, tk_row
    LWZA  r4, tk_ch
    bl    tk_cell_draw
tp_2:
    LWZA  r3, tk_pf
    andi. r0, r3, 2
    beq   tp_d
    LWZA  r3, tk_oprow
    li    r4, ' '
    bl    tk_marker_draw
    LWZA  r3, tk_prow
    li    r4, '>'
    bl    tk_marker_draw
tp_d:
    li    r3, 0
    STWA  r3, tk_pf, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# app_tracker: the full-screen draw (chrome + header + all 32 rows).
app_tracker_draw:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 16
    li    r4, 24
    LA    r5, s_tk_hdr0
    bl    pstr
    li    r3, 72
    li    r4, 24
    LA    r5, s_tk_hdr
    bl    pstr
    li    r3, 200
    li    r4, 464
    LA    r5, s_tk_help
    bl    pstr
    li    r14, 0                          # row
atk_row:
    mr    r3, r14
    bl    two_digits
    LA    r5, numstr
    stb   r4, 0(r5)
    stb   r3, 1(r5)
    li    r3, 0
    stb   r3, 2(r5)
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 16
    mulli r4, r14, 13
    addi  r4, r4, 40
    LA    r5, numstr
    bl    pstr
    li    r15, 0                          # ch
atk_ch:
    mr    r3, r14
    mr    r4, r15
    bl    tk_cell_draw
    addi  r15, r15, 1
    cmpwi r15, TKCH
    bne   atk_ch
    addi  r14, r14, 1
    cmpwi r14, TKROWS
    bne   atk_row
    LWZA  r3, tk_prow
    li    r4, '>'
    bl    tk_marker_draw
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

.section .rodata
tk_names:  .ascii "CDEFGABC"
s_tk_hdr0: .asciz "Row"
s_tk_hdr:  .asciz "Ch1    Ch2    Ch3    Ch4"
s_tk_help: .asciz "A=edit cell   auto-play"
.align 1
tk_freqs:  .short 262, 294, 330, 349, 392, 440, 494, 523
# demo pattern: 32 rows x 4 channels, 0 empty / 1..8 = C4..C5
# ch0 bass, ch1 arp, ch2 melody, ch3 sparse accents
tk_demo:
    .byte 1,0,0,0,  0,3,0,0,  0,5,0,0,  0,3,0,8
    .byte 5,0,0,0,  0,3,0,0,  0,5,0,0,  0,0,3,0
    .byte 1,0,0,0,  0,3,0,0,  0,5,0,0,  0,3,0,8
    .byte 6,0,0,0,  0,4,0,0,  0,6,0,0,  0,0,4,0
    .byte 4,0,0,0,  0,6,0,0,  0,8,0,0,  0,6,0,8
    .byte 5,0,0,0,  0,7,0,0,  0,2,0,0,  0,0,7,0
    .byte 1,0,0,0,  0,3,0,0,  0,5,0,0,  0,3,0,8
    .byte 5,0,3,0,  0,3,0,0,  1,0,0,0,  0,0,0,8
.align 2
.section .text
