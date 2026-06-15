; ============================================================================
; UnoDOS/68K Music - DISK-LOADED app (MUSIC.APP, proc 4). Was apps_m2.i's
; music_start/stop/tick/key/draw built into the kernel; now a separate -Fbin
; binary loaded off DF1 into its window slot. Links to the kernel only via
; APIVEC (KCALL/KDATA), and drives Paula channel 0 directly (the audio custom
; registers live at the fixed $DFF000 base, which is relocation-proof).
;
; The score (mus_notes / mus_count - period, duration, staff y-offset triplets)
; is the kernel's generated table, exported via APIVEC. Playback position
; (mus_ix / mus_end / mus_playing) and the frame counter (ticks) stay in the
; kernel 'vars' block. The app's tick is driven on the focused window by the
; scheduler (post_ticks -> app_tick), matching the old topmost-only sequencer.
; ============================================================================

        mc68000
        include "sysabi.i"
        include "build/kernel_api.inc"

; ---- Paula custom registers (fixed hardware addresses) ----
CUSTOM      equ $DFF000
DMACON      equ $096
AUD0PER     equ $0A6
AUD0VOL     equ $0A8

        org     APPSLOT0
; ---- JMP table (open/draw/key/tick/click) ----
        jmp     music_open(pc)
        jmp     music_draw(pc)
        jmp     music_key(pc)
        jmp     music_tick(pc)
        jmp     music_click(pc)

music_open:
        rts
music_click:
        rts

; music_start - a4 = &vars
music_start:
        movem.l d0-d1/a0/a4/a6,-(sp)
        KDATA   a4,vars
        clr.w   VO_mus_ix(a4)
        st      VO_mus_playing(a4)
        move.l  VO_ticks(a4),d0
        KDATA   a0,mus_notes
        moveq   #0,d1
        move.w  2(a0),d1            ; first duration
        add.l   d1,d0
        move.l  d0,VO_mus_end(a4)
        lea     CUSTOM,a6
        move.w  (a0),AUD0PER(a6)    ; first period
        move.w  #48,AUD0VOL(a6)
        move.w  #$8201,DMACON(a6)   ; DMAEN|AUD0EN
        movem.l (sp)+,d0-d1/a0/a4/a6
        rts

; music_stop - a4 = &vars
music_stop:
        movem.l a4/a6,-(sp)
        KDATA   a4,vars
        sf      VO_mus_playing(a4)
        lea     CUSTOM,a6
        move.w  #0,AUD0VOL(a6)
        move.w  #$0001,DMACON(a6)   ; AUD0 DMA off
        movem.l (sp)+,a4/a6
        rts

; music_tick - advance the sequencer; refresh window (called on the focused
; window via the scheduler, so we are the topmost when this fires)
music_tick:
        movem.l d0-d3/a0/a2/a4/a6,-(sp)
        KDATA   a4,vars
        move.b  VO_mus_playing(a4),d0
        beq     .out
        move.l  VO_ticks(a4),d0
        cmp.l   VO_mus_end(a4),d0
        blt     .out
        move.w  VO_mus_ix(a4),d1
        addq.w  #1,d1
        KDATA   a0,mus_count
        cmp.w   (a0),d1
        blt     .ixok
        moveq   #0,d1               ; loop
.ixok:  move.w  d1,VO_mus_ix(a4)
        ; set period + new end time
        move.w  d1,d2
        mulu    #6,d2
        KDATA   a0,mus_notes
        add.w   d2,a0
        lea     CUSTOM,a6
        move.w  (a0),AUD0PER(a6)
        moveq   #0,d2
        move.w  2(a0),d2
        add.l   d2,d0
        move.l  d0,VO_mus_end(a4)
        bsr     music_draw
.out:   movem.l (sp)+,d0-d3/a0/a2/a4/a6
        rts

; music_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
music_key:
        cmp.b   #' ',d1
        beq     .toggle
        moveq   #1,d0
        rts
.toggle:
        KDATA   a4,vars
        move.b  VO_mus_playing(a4),d0
        beq     .play
        bsr     music_stop
        bra     .rd
.play:  bsr     music_start
.rd:    KCALL   redraw_topmost
        moveq   #0,d0
        rts

; music_draw - a2 = window
music_draw:
        movem.l d0-d7/a0-a4,-(sp)
        KDATA   a4,vars
        ; clear content
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        sub.w   #TBAR_H+1,d3
        moveq   #0,d4
        KCALL   fill_rect
        ; title
        lea     str_m_title(pc),a0
        move.w  WX(a2),d0
        addq.w  #6,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+3,d1
        moveq   #3,d2
        KCALL   draw_string
        ; staff: 5 white hlines
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
        KCALL   fill_rect
        addq.w  #1,d7
        cmp.w   #5,d7
        blt     .staff
        ; notes
        KDATA   a3,mus_count
        moveq   #0,d7
.note:  cmp.w   (a3),d7
        bge     .foot
        ; x = wx + 10 + i*step ; step = (ww-24)/count
        move.w  WW(a2),d0
        sub.w   #24,d0
        ext.l   d0
        divu    (a3),d0
        mulu    d7,d0
        add.w   WX(a2),d0
        add.w   #10,d0
        ; y = wy + TBAR_H + 56 - yoff
        move.w  d7,d2
        mulu    #6,d2
        KDATA   a0,mus_notes
        add.w   d2,a0
        move.w  WY(a2),d1
        add.w   #TBAR_H+56,d1
        sub.w   4(a0),d1            ; staff y offset
        moveq   #4,d2               ; 4x4 note block
        moveq   #4,d3
        ; color: playing note magenta, others cyan
        moveq   #1,d4
        move.b  VO_mus_playing(a4),d5
        beq     .col
        cmp.w   VO_mus_ix(a4),d7
        bne     .col
        moveq   #2,d4
.col:   KCALL   fill_rect
        addq.w  #1,d7
        bra     .note
.foot:
        lea     str_m_play(pc),a0
        move.w  WX(a2),d0
        addq.w  #6,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #12,d1
        moveq   #1,d2
        KCALL   draw_string
        movem.l (sp)+,d0-d7/a0-a4
        rts

; ---- app-private strings (moved out of the kernel) ----
str_m_title:    dc.b    "Canon in D  (Pachelbel)",0
str_m_play:     dc.b    "Space: play/stop",0
        even
        end
