; ============================================================================
; UnoDOS/MacPlus Music - DISK-LOADED app (MUSIC.APP, proc 9, slot 5).
; Square-wave sequencer staff view. The sequencer itself (advance + snd_tone)
; runs kernel-side in appsvc.i (music_tick) so the song keeps playing when the
; window is not topmost; this app is the UI (draw + space toggles play/stop via
; the exported music_start/music_stop). State (mus_*) lives in the kernel vars,
; shared with the kernel sequencer. ABI: JMP draw/key/tick/click.
; ============================================================================

        mc68000
        include "sysequ.i"
        include "build/kernel_api.inc"

MUSIC_PROC  equ 9

        org     APP_LOAD+APPSLOT_MUSIC*APP_SLOT_SZ
        dc.l     music_draw          ; +0  draw
        dc.l     music_key           ; +4  key
        dc.l     music_tickv         ; +8  tick (audio runs kernel-side)
        dc.l     music_click         ; +12 click

music_tickv:
        rts
music_click:
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
        bsr     music_stop          ; -> jsr (kernel) via mkapp
        bra     .rd
.play:  bsr     music_start
.rd:    moveq   #0,d0               ; consumed; the kernel redraws topmost
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
        lea     mus_notes,a0
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
        even
        end
