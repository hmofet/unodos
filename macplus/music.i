; ============================================================================
; UnoDOS/MacPlus Music app (proc 9) - the square-wave sequencer + staff
; view, port of the Amiga apps_m2.i Music over snd.i instead of Paula.
; Same shared note data (gen_data.i mus_notes: period.w, PAL-dur.w,
; staff-y.w), Canon in D; durations rescaled x6/5 to TICKS_SEC. 1-bit
; colors: black staff/labels on the white content, playing note black,
; others medium dither. Space toggles playback; the song keeps playing
; when the window is not topmost (it is a player); closing any window
; silences audio per PORT-SPEC SS2.
; ============================================================================

MUSIC_PROC  equ 9

; music_start
music_start:
        movem.l d0-d1/a0/a4,-(sp)
        lea     vars(pc),a4
        clr.w   mus_ix-vars(a4)
        st      mus_playing-vars(a4)
        lea     mus_notes(pc),a0
        move.w  (a0),d0             ; first period
        moveq   #0,d1
        move.w  2(a0),d1            ; first duration (PAL ticks)
        mulu    #6,d1
        divu    #5,d1
        and.l   #$FFFF,d1
        add.l   ticks(pc),d1
        move.l  d1,mus_end-vars(a4)
        bsr     snd_tone
        movem.l (sp)+,d0-d1/a0/a4
        rts

; music_stop
music_stop:
        movem.l d0/a4,-(sp)
        lea     vars(pc),a4
        sf      mus_playing-vars(a4)
        bsr     snd_off
        movem.l (sp)+,d0/a4
        rts

; music_tick - advance the sequencer; refresh window when topmost
music_tick:
        move.b  mus_playing(pc),d0
        bne     .on
        rts
.on:
        movem.l d0-d3/a0/a2/a4,-(sp)
        move.l  ticks(pc),d0
        cmp.l   mus_end(pc),d0
        blt     .out
        lea     vars(pc),a4
        move.w  mus_ix(pc),d1
        addq.w  #1,d1
        cmp.w   mus_count(pc),d1
        blt     .ixok
        moveq   #0,d1               ; loop
.ixok:  move.w  d1,mus_ix-vars(a4)
        ; play the note + set the new end time
        move.w  d1,d2
        mulu    #6,d2
        lea     mus_notes(pc),a0
        add.w   d2,a0
        moveq   #0,d2
        move.w  2(a0),d2            ; duration (PAL ticks)
        mulu    #6,d2
        divu    #5,d2
        and.l   #$FFFF,d2
        add.l   d2,d0
        move.l  d0,mus_end-vars(a4)
        move.w  (a0),d0
        bsr     snd_tone
        ; topmost-only visual refresh (PORT-SPEC SS2)
        move.w  zcount(pc),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        moveq   #0,d3
        move.b  WPROC(a2),d3
        cmp.w   #MUSIC_PROC,d3
        bne     .out
        bsr     music_draw
.out:   movem.l (sp)+,d0-d3/a0/a2/a4
        rts

; music_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
music_key:
        cmp.b   #' ',d1
        beq     .toggle
        moveq   #1,d0
        rts
.toggle:
        move.b  mus_playing(pc),d0
        beq     .play
        bsr     music_stop
        bra     .rd
.play:  bsr     music_start
.rd:    bsr     redraw_topmost
        moveq   #0,d0
        rts

; music_draw - a2 = window
music_draw:
        movem.l d0-d7/a0-a3,-(sp)
        ; clear content (white)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        sub.w   #TBAR_H+1,d3
        moveq   #0,d4
        bsr     fill_rect
        ; title
        lea     str_m_title(pc),a0
        move.w  WX(a2),d0
        addq.w  #6,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+3,d1
        moveq   #3,d2
        bsr     draw_string
        ; staff: 5 black hlines
        moveq   #0,d7
.staff: move.w  WX(a2),d0
        addq.w  #8,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+24,d1
        move.w  d7,d2
        lsl.w   #3,d2
        add.w   d2,d1
        move.w  WW(a2),d2
        sub.w   #16,d2
        moveq   #1,d3
        moveq   #3,d4
        bsr     fill_rect
        addq.w  #1,d7
        cmp.w   #5,d7
        blt     .staff
        ; notes
        moveq   #0,d7
.note:  cmp.w   mus_count(pc),d7
        bge     .foot
        ; x = wx + 10 + i*step ; step = (ww-24)/count
        move.w  WW(a2),d0
        sub.w   #24,d0
        ext.l   d0
        divu    mus_count(pc),d0
        mulu    d7,d0
        add.w   WX(a2),d0
        add.w   #10,d0
        ; y = wy + TBAR_H + 56 - staff offset
        move.w  d7,d2
        mulu    #6,d2
        lea     mus_notes(pc),a0
        add.w   d2,a0
        move.w  WY(a2),d1
        add.w   #TBAR_H+56,d1
        sub.w   4(a0),d1            ; staff y offset
        moveq   #4,d2               ; 4x4 note block
        moveq   #4,d3
        ; playing note black, others medium dither
        moveq   #2,d4
        move.b  mus_playing(pc),d5
        beq     .col
        cmp.w   mus_ix(pc),d7
        bne     .col
        moveq   #3,d4
.col:   bsr     fill_rect
        addq.w  #1,d7
        bra     .note
.foot:
        lea     str_m_play(pc),a0
        move.w  WX(a2),d0
        addq.w  #6,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #12,d1
        moveq   #3,d2
        bsr     draw_string
        movem.l (sp)+,d0-d7/a0-a3
        rts

str_m_title:    dc.b    "Canon in D  (square voice)",0
str_m_play:     dc.b    "Space: play/stop",0
str_t_music:    dc.b    "Music",0
name_music:     dc.b    "Music",0
        even
