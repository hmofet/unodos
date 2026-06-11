; ============================================================================
; UnoDOS/68K Tracker (proc 9) - write and play 4-channel Paula music,
; MOD-style: ProTracker note periods, 32-row pattern, 4 chip-synthesized
; instruments (square / sawtooth / triangle / noise) generated into chip
; RAM at boot. The pattern lives in RAM (file save lands with FAT12).
;
;   arrows      move the cursor (row / channel)
;   q / w       note down / up a semitone (empty cell starts at C-2)
;   e           cycle the cell's instrument
;   x           clear the cell
;   d           load the demo song
;   space       play / stop (loops the pattern)
; ============================================================================

TK_ROWS     equ 32
TK_CHANS    equ 4
TK_VIEW     equ 12                  ; visible rows
TK_WAVELEN  equ 32                  ; bytes per instrument wave
TKWAVES     equ $76B00              ; 4 x 32 bytes, chip RAM

; ProTracker periods, C-2..B-3 (notes 1..24)
tk_periods:
        dc.w    428,404,381,360,339,320,302,285,269,254,240,226
        dc.w    214,202,190,180,170,160,151,143,135,127,120,113
tk_notenames:                       ; 2 chars per semitone
        dc.b    "C-C#D-D#E-F-F#G-G#A-A#B-"
tk_instname:
        dc.b    "SQ","SW","TR","NZ"
        even

; tk_init - synthesize the 4 instrument waves into chip RAM (boot)
tk_init:
        movem.l d0-d3/a0,-(sp)
        lea     TKWAVES,a0
        ; 0: square
        moveq   #TK_WAVELEN/2-1,d0
.sq1:   move.b  #$6F,(a0)+
        dbra    d0,.sq1
        moveq   #TK_WAVELEN/2-1,d0
.sq2:   move.b  #$91,(a0)+
        dbra    d0,.sq2
        ; 1: sawtooth -64..60
        moveq   #-64,d1
        moveq   #TK_WAVELEN-1,d0
.saw:   move.b  d1,(a0)+
        addq.b  #4,d1
        dbra    d0,.saw
        ; 2: triangle
        moveq   #-64,d1
        moveq   #TK_WAVELEN/2-1,d0
.tr1:   move.b  d1,(a0)+
        addq.b  #8,d1
        dbra    d0,.tr1
        moveq   #63,d1
        moveq   #TK_WAVELEN/2-1,d0
.tr2:   move.b  d1,(a0)+
        subq.b  #8,d1
        dbra    d0,.tr2
        ; 3: noise (LCG)
        move.l  #$2F6E1349,d2
        moveq   #TK_WAVELEN-1,d0
.nz:    move.l  d2,d3
        swap    d3
        move.b  d3,(a0)+
        mulu    #$C13F,d2
        add.l   #$0BAD,d2
        dbra    d0,.nz
        movem.l (sp)+,d0-d3/a0
        rts

; tk_cell - d0=row d1=chan -> a0 = cell ptr (2 bytes: note, instr)
tk_cell:
        movem.l d0-d1,-(sp)
        lsl.w   #2,d0               ; row * 4 chans
        add.w   d1,d0
        add.w   d0,d0               ; * 2 bytes
        lea     tk_pat(pc),a0
        lea     (a0,d0.w),a0
        movem.l (sp)+,d0-d1
        rts

; tk_silence - all four channels off
tk_silence:
        movem.l d0/a6,-(sp)
        lea     CUSTOM,a6
        moveq   #0,d0
        move.w  d0,AUD0VOL(a6)
        move.w  d0,AUD0VOL+$10(a6)
        move.w  d0,AUD0VOL+$20(a6)
        move.w  d0,AUD0VOL+$30(a6)
        movem.l (sp)+,d0/a6
        rts

; tk_stop
tk_stop:
        movem.l a4,-(sp)
        lea     vars(pc),a4
        sf      tk_playing-vars(a4)
        bsr     tk_silence
        movem.l (sp)+,a4
        rts

; tk_trigger_row - d0 = row: fire the notes of one row on Paula
tk_trigger_row:
        movem.l d0-d5/a0/a3/a6,-(sp)
        lea     CUSTOM,a6
        moveq   #0,d1               ; channel
.ch:    bsr     tk_cell             ; a0 = cell
        moveq   #0,d2
        move.b  (a0),d2             ; note 0 = keep playing
        beq     .next
        moveq   #0,d3
        move.b  1(a0),d3            ; instrument 0-3
        ; a3 = this channel's register block (AUDxLCH base)
        move.w  d1,d4
        lsl.w   #4,d4               ; chan * $10
        lea     AUD0LCH(a6),a3
        add.w   d4,a3
        ; wave address
        move.w  d3,d5
        lsl.w   #5,d5               ; * TK_WAVELEN
        add.l   #TKWAVES,d5
        move.l  d5,(a3)             ; AUDxLCH+LCL
        move.w  #TK_WAVELEN/2,4(a3) ; AUDxLEN
        ; period
        subq.w  #1,d2
        add.w   d2,d2
        lea     tk_periods(pc),a0
        move.w  (a0,d2.w),6(a3)     ; AUDxPER
        move.w  #44,8(a3)           ; AUDxVOL
        ; enable this channel's DMA
        moveq   #1,d5
        lsl.w   d1,d5
        or.w    #$8200,d5           ; SET | DMAEN | AUDxEN
        move.w  d5,DMACON(a6)
.next:  addq.w  #1,d1
        cmp.w   #TK_CHANS,d1
        blt     .ch
        movem.l (sp)+,d0-d5/a0/a3/a6
        rts

; tracker_tick - playback sequencer (main loop; 6 PAL ticks per row)
tracker_tick:
        movem.l d0-d2/a2/a4,-(sp)
        move.b  tk_playing(pc),d0
        beq     .out
        move.l  ticks(pc),d0
        sub.l   tk_last(pc),d0
        cmp.l   #6,d0
        blt     .out
        lea     vars(pc),a4
        move.l  ticks(pc),d0
        move.l  d0,tk_last-vars(a4)
        move.w  tk_prow(pc),d0
        addq.w  #1,d0
        cmp.w   #TK_ROWS,d0
        blt     .rok
        moveq   #0,d0               ; loop the pattern
.rok:   move.w  d0,tk_prow-vars(a4)
        bsr     tk_trigger_row
        ; redraw if we are the topmost window
        move.w  zcount(pc),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        cmp.b   #9,WPROC(a2)
        bne     .out
        bsr     tracker_draw
.out:   movem.l (sp)+,d0-d2/a2/a4
        rts

; tk_fmt_cell - a0=cell -> writes 5 chars ("C#2 1" / "--- -") at a1, advances
tk_fmt_cell:
        movem.l d0-d2/a0/a2,-(sp)
        moveq   #0,d0
        move.b  (a0),d0
        bne     .note
        move.b  #'-',(a1)+
        move.b  #'-',(a1)+
        move.b  #'-',(a1)+
        move.b  #' ',(a1)+
        move.b  #'-',(a1)+
        bra     .done
.note:  subq.w  #1,d0               ; 0-23
        ; semitone d1, octave d2
        move.w  d0,d1
        and.l   #$FFFF,d1
        divu    #12,d1
        move.w  d1,d2               ; octave 0/1
        swap    d1                  ; semitone
        add.w   d1,d1
        lea     tk_notenames(pc),a2
        move.b  (a2,d1.w),(a1)+
        move.b  1(a2,d1.w),(a1)+
        add.w   #'2',d2
        move.b  d2,(a1)+
        move.b  #' ',(a1)+
        moveq   #0,d0
        move.b  1(a0),d0
        add.w   d0,d0
        lea     tk_instname(pc),a2
        move.b  (a2,d0.w),(a1)+     ; first letter of instrument
.done:  movem.l (sp)+,d0-d2/a0/a2
        rts

; tracker_draw - a2 = window
tracker_draw:
        movem.l d0-d7/a0-a4,-(sp)
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
        bsr     fill_rect
        ; header: channel names, selected channel in accent
        move.w  WX(a2),d6
        addq.w  #6,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+2,d5
        lea     str_tk_hdr(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #1,d2
        bsr     draw_string
        ; channel cursor marker: caret under the selected channel column
        move.w  tk_ch(pc),d0
        mulu    #56,d0
        add.w   d6,d0
        add.w   #24,d0
        move.w  d5,d1
        lea     str_tk_mark(pc),a0
        moveq   #2,d2
        bsr     draw_string
        ; keep the cursor row in view
        lea     vars(pc),a4
        move.w  tk_row(pc),d0
        move.w  tk_top(pc),d1
        cmp.w   d1,d0
        bge     .t1
        move.w  d0,d1
.t1:    move.w  d1,d2
        add.w   #TK_VIEW,d2
        cmp.w   d2,d0
        blt     .t2
        move.w  d0,d1
        sub.w   #TK_VIEW-1,d1
.t2:    move.w  d1,tk_top-vars(a4)
        ; rows
        add.w   #12,d5
        move.w  tk_top(pc),d7       ; row index
.row:   ; build "NN  C-2 S  ..." into npstat
        lea     npstat(pc),a1
        moveq   #0,d0
        move.w  d7,d0
        divu    #10,d0
        add.b   #'0',d0
        move.b  d0,(a1)+
        swap    d0
        add.b   #'0',d0
        move.b  d0,(a1)+
        move.b  #' ',(a1)+
        move.b  #' ',(a1)+
        moveq   #0,d1               ; channel
.cell:  move.w  d7,d0
        bsr     tk_cell
        bsr     tk_fmt_cell
        move.b  #' ',(a1)+
        move.b  #' ',(a1)+
        addq.w  #1,d1
        cmp.w   #TK_CHANS,d1
        blt     .cell
        clr.b   (a1)
        ; colors: cursor row = accent bar, playing row = accent2
        moveq   #3,d2               ; fg
        moveq   #0,d3               ; bg
        cmp.w   tk_row(pc),d7
        bne     .ncur
        moveq   #0,d2
        moveq   #1,d3
        bra     .draw
.ncur:  move.b  tk_playing(pc),d0
        beq     .draw
        cmp.w   tk_prow(pc),d7
        bne     .draw
        moveq   #3,d2
        moveq   #2,d3
.draw:  lea     npstat(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        bsr     draw_string_bg
        add.w   #10,d5
        addq.w  #1,d7
        move.w  tk_top(pc),d0
        add.w   #TK_VIEW,d0
        cmp.w   d0,d7
        blt     .row
        ; footer
        addq.w  #4,d5
        lea     str_tk_foot1(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #1,d2
        bsr     draw_string
        add.w   #10,d5
        lea     str_tk_foot2(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #1,d2
        bsr     draw_string
        movem.l (sp)+,d0-d7/a0-a4
        rts

; tracker_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
tracker_key:
        lea     vars(pc),a4
        cmp.b   #$4C,d2             ; up
        beq     .up
        cmp.b   #$4D,d2             ; down
        beq     .down
        cmp.b   #$4F,d2             ; left
        beq     .left
        cmp.b   #$4E,d2             ; right
        beq     .right
        cmp.b   #'q',d1
        beq     .ndown
        cmp.b   #'w',d1
        beq     .nup
        cmp.b   #'e',d1
        beq     .instr
        cmp.b   #'x',d1
        beq     .clear
        cmp.b   #'d',d1
        beq     .demo
        cmp.b   #'s',d1
        beq     .save
        cmp.b   #'l',d1
        beq     .load
        cmp.b   #32,d1
        beq     .playstop
        moveq   #1,d0
        rts
.save:  move.b  fat_mounted(pc),d0
        beq     .redraw
        lea     str_songname(pc),a0
        lea     tk_pat(pc),a1
        move.l  #TK_ROWS*TK_CHANS*2,d1
        bsr     fat_save_file
        bsr     fat_list_root
        bra     .redraw
.load:  move.b  fat_mounted(pc),d0
        beq     .redraw
        lea     str_songname(pc),a0
        bsr     fat_find_file
        tst.w   d0
        bmi     .redraw
        move.l  #TK_ROWS*TK_CHANS*2,d1
        lea     tk_pat(pc),a1
        bsr     fat_read_file
        bra     .redraw
.up:    move.w  tk_row(pc),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,tk_row-vars(a4)
        bra     .redraw
.down:  move.w  tk_row(pc),d0
        cmp.w   #TK_ROWS-1,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,tk_row-vars(a4)
        bra     .redraw
.left:  move.w  tk_ch(pc),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,tk_ch-vars(a4)
        bra     .redraw
.right: move.w  tk_ch(pc),d0
        cmp.w   #TK_CHANS-1,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,tk_ch-vars(a4)
        bra     .redraw
.ndown: bsr     tk_curcell          ; a0 = cell
        move.b  (a0),d0
        beq     .startc
        cmp.b   #1,d0
        ble     .redraw
        subq.b  #1,(a0)
        bra     .preview
.nup:   bsr     tk_curcell
        move.b  (a0),d0
        beq     .startc
        cmp.b   #24,d0
        bge     .redraw
        addq.b  #1,(a0)
        bra     .preview
.startc:
        move.b  #1,(a0)             ; C-2
        bra     .preview
.instr: bsr     tk_curcell
        tst.b   (a0)
        beq     .redraw
        move.b  1(a0),d0
        addq.b  #1,d0
        and.b   #3,d0
        move.b  d0,1(a0)
        bra     .preview
.clear: bsr     tk_curcell
        clr.w   (a0)
        bra     .redraw
.demo:  bsr     tk_load_demo
        bra     .redraw
.playstop:
        move.b  tk_playing(pc),d0
        beq     .start
        bsr     tk_stop
        bra     .redraw
.start: st      tk_playing-vars(a4)
        move.w  #TK_ROWS-1,tk_prow-vars(a4)  ; first tick wraps to row 0
        move.l  ticks(pc),d0
        subq.l  #6,d0
        move.l  d0,tk_last-vars(a4)
        bra     .redraw
.preview:
        ; hear the edit immediately: trigger just this cell's row on its chan
        move.w  tk_row(pc),d0
        bsr     tk_trigger_row
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

; tk_curcell -> a0 = cell at the cursor
tk_curcell:
        movem.l d0-d1,-(sp)
        move.w  tk_row(pc),d0
        move.w  tk_ch(pc),d1
        bsr     tk_cell
        movem.l (sp)+,d0-d1
        rts

; tk_load_demo - copy the built-in demo song into the pattern
tk_load_demo:
        movem.l d0/a0-a1,-(sp)
        lea     tk_demo(pc),a0
        lea     tk_pat(pc),a1
        move.w  #TK_ROWS*TK_CHANS*2/4-1,d0
.cp:    move.l  (a0)+,(a1)+
        dbra    d0,.cp
        movem.l (sp)+,d0/a0-a1
        rts

; ---------------------------------------------------------------- data
str_tk_hdr:     dc.b    "Row Chan1  Chan2  Chan3  Chan4",0
str_tk_mark:    dc.b    "^",0
str_tk_foot1:   dc.b    "q/w:note e:inst x:clr d:demo s/l:disk",0
str_tk_foot2:   dc.b    "Space: play/stop   arrows: move",0
str_t_tracker:  dc.b    "Tracker",0
name_tracker:   dc.b    "Tracker",0

        even
; demo song: 32 rows x 4 channels x (note, instr). Notes: 1=C-2 .. 24=B-3.
; ch1 saw bass, ch2 square arp, ch3 triangle melody, ch4 noise hits.
tk_demo:
        dc.b    1,1,  13,0,  0,0,  20,3    ; row 00  C-2 | C-3 |     | hit
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  17,0,  0,0,   0,0    ;        | E-3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    1,1,  20,0,  0,0,  20,3    ;  C-2 | G-3 |     | hit
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  17,0, 13,2,   0,0    ;        | E-3 | C-3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  13,0, 17,2,  20,3    ;  G-2 | C-3 | E-3 | hit
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  15,0,  0,0,   0,0    ;        | D-3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  20,0, 15,2,  20,3    ;  G-2 | G-3 | D-3 | hit
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  15,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    6,1,  10,0, 13,2,  20,3    ;  F-2 | A-2 | C-3 | hit
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  13,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    6,1,  17,0,  0,0,  20,3    ;  F-2 | E-3 |     | hit
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  13,0, 10,2,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  11,0, 15,2,  20,3    ;  G-2 | A#2 | D-3 | hit
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  15,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  20,0, 19,2,  20,3    ;  G-2 | G-3 | F#3 | hit
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  23,0,  0,0,  20,3    ;        | A#3 |     | hit
        dc.b    0,0,   0,0,  0,0,   0,0
        even

str_songname:   dc.b    "SONG    UNO"
        even
