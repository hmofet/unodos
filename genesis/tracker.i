; ============================================================================
; UnoDOS/Genesis Tracker (proc 9) - milestone 3. Port of amiga/tracker.i
; onto the PSG: channels 1-3 are the three square-wave tone generators,
; channel 4 is the noise generator. The pattern format is byte-identical
; to the Amiga port (32 rows x 4 channels x (note, instr)), so songs
; round-trip between platforms via tape.
;
; The PSG has no waveforms, so the instrument column maps to timbre the
; PSG can express: on tone channels it picks the note's volume (SQ
; bright / SW medium / TR soft / NZ accent); on the noise channel the
; note picks the noise rate (3 pitches of white noise) and instruments
; are ignored. Same cell semantics, platform-appropriate voice.
;
;   arrows      move the cursor (row / channel)
;   q / w       note down / up a semitone (empty cell starts at C-2)
;   e           cycle the cell's instrument
;   x           clear the cell
;   d           load the demo song
;   space (Y)   play / stop (loops the pattern)
;   s / l       save / load SONG.TRK in cartridge SRAM
;   t / y       write / read the pattern to/from tape
; ============================================================================

TK_ROWS     equ 32
TK_CHANS    equ 4
TK_VIEW     equ 12                  ; visible rows
TK_PATLEN   equ TK_ROWS*TK_CHANS*2

; PSG tone values for C-2..B-3 (notes 1..24): the Amiga ProTracker
; periods scaled by the clock ratio (3579545 / 3546895) - the same
; pitches the Amiga tracker plays, within a cent or two.
tkg_psgval:
        dc.w    432,408,385,363,342,323,305,288,271,256,242,228
        dc.w    216,204,192,182,172,161,152,144,136,128,121,114
tkg_notenames:                      ; 2 chars per semitone
        dc.b    "C-C#D-D#E-F-F#G-G#A-A#B-"
tkg_instname:
        dc.b    "SQSWTRNZ"
tkg_instatt:                        ; tone-channel attenuation per instr
        dc.b    2,4,6,3
        even

; tk_cell - d0 = row, d1 = chan -> a0 = cell (2 bytes: note, instr)
tk_cell:
        movem.l d0-d1,-(sp)
        lsl.w   #2,d0               ; row * 4 chans
        add.w   d1,d0
        add.w   d0,d0               ; * 2 bytes
        lea     VARS+v_tkpat,a0
        lea     (a0,d0.w),a0
        movem.l (sp)+,d0-d1
        rts

; tk_silence - all four PSG channels off
tk_silence:
        move.b  #$9F,PSG
        move.b  #$BF,PSG
        move.b  #$DF,PSG
        move.b  #$FF,PSG
        rts

; tk_stop
tk_stop:
        movem.l a4,-(sp)
        lea     VARS,a4
        sf      v_tk_playing(a4)
        sf      v_tk_nzatt(a4)
        bsr     tk_silence
        movem.l (sp)+,a4
        rts

; tk_trigger_row - d0 = row: fire the row's notes on the PSG
tk_trigger_row:
        movem.l d0-d5/a0-a1/a4,-(sp)
        lea     VARS,a4
        moveq   #0,d1               ; channel
.ch:    bsr     tk_cell             ; a0 = cell
        moveq   #0,d2
        move.b  (a0),d2             ; note (0 = keep playing)
        beq     .next
        moveq   #0,d3
        move.b  1(a0),d3            ; instrument 0-3
        cmp.w   #3,d1
        beq     .noise
        ; tone channel d1: latch tone + volume
        subq.w  #1,d2
        add.w   d2,d2
        lea     tkg_psgval(pc),a1
        move.w  (a1,d2.w),d4        ; PSG tone value
        move.w  d1,d5
        lsl.w   #5,d5               ; chan << 5
        move.w  d4,d2
        and.b   #$0F,d2
        or.b    #$80,d2
        or.b    d5,d2
        move.b  d2,PSG              ; latch: tone low nibble
        lsr.w   #4,d4
        and.b   #$3F,d4
        move.b  d4,PSG              ; data: high 6 bits
        lea     tkg_instatt(pc),a1
        move.b  (a1,d3.w),d2
        or.b    #$90,d2
        or.b    d5,d2
        move.b  d2,PSG              ; volume
        bra     .next
.noise: ; noise channel: note picks the rate, retrigger the decay
        moveq   #3,d4
        and.w   d2,d4
        cmp.w   #3,d4
        bne     .nz
        moveq   #2,d4               ; rate 3 = ch2-coupled: avoid
.nz:    or.b    #$E4,d4             ; white noise + rate
        move.b  d4,PSG
        move.b  #$F2,PSG            ; volume on
        move.b  #2,v_tk_nzatt(a4)   ; decay restarts
.next:  addq.w  #1,d1
        cmp.w   #TK_CHANS,d1
        blt     .ch
        movem.l (sp)+,d0-d5/a0-a1/a4
        rts

; ----------------------------------------------------------------------------
; tracker_tick - playback sequencer + noise decay (main loop; 6 ticks
; per row, same tempo as the Amiga port)
; ----------------------------------------------------------------------------
tracker_tick:
        movem.l d0-d3/a2/a4,-(sp)
        lea     VARS,a4
        ; noise hit decay: fade the noise channel a step per frame
        move.b  v_tk_nzatt(a4),d0
        beq     .seq
        addq.b  #1,d0
        cmp.b   #15,d0
        bls     .att
        sf      v_tk_nzatt(a4)
        move.b  #$FF,PSG            ; off
        bra     .seq
.att:   move.b  d0,v_tk_nzatt(a4)
        or.b    #$F0,d0
        move.b  d0,PSG
.seq:   move.b  v_tk_playing(a4),d0
        beq     .out
        move.l  v_ticks(a4),d0
        move.l  d0,d1
        sub.l   v_tk_last(a4),d1
        cmp.l   #6,d1
        blt     .out
        move.l  d0,v_tk_last(a4)
        move.w  v_tk_prow(a4),d0
        addq.w  #1,d0
        cmp.w   #TK_ROWS,d0
        blt     .rok
        moveq   #0,d0               ; loop the pattern
.rok:   move.w  d0,v_tk_prow(a4)
        bsr     tk_trigger_row
        ; redraw when we are the topmost window
        move.w  v_zcount(a4),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        cmp.b   #9,WPROC(a2)
        bne     .out
        bsr     tracker_draw
.out:   movem.l (sp)+,d0-d3/a2/a4
        rts

; tk_fmt_cell - a0 = cell: write 5 chars ("C#2 S" / "--- -") at a1+
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
        move.w  d0,d1
        and.l   #$FFFF,d1
        divu    #12,d1
        move.w  d1,d2               ; octave 0/1
        swap    d1                  ; semitone
        add.w   d1,d1
        lea     tkg_notenames(pc),a2
        move.b  (a2,d1.w),(a1)+
        move.b  1(a2,d1.w),(a1)+
        add.w   #'2',d2
        move.b  d2,(a1)+
        move.b  #' ',(a1)+
        moveq   #0,d0
        move.b  1(a0),d0
        add.w   d0,d0
        lea     tkg_instname(pc),a2
        move.b  (a2,d0.w),(a1)+     ; first letter of the instrument
.done:  movem.l (sp)+,d0-d2/a0/a2
        rts

; ----------------------------------------------------------------------------
; tracker_draw - a2 = window
; ----------------------------------------------------------------------------
tracker_draw:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     VARS,a4
        ; clear content
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        subq.w  #2,d3
        move.w  #ATTR_NORM+T_SOLBG,d4
        bsr     fill_cells
        move.w  WX(a2),d6
        addq.w  #2,d6
        move.w  WY(a2),d5
        addq.w  #1,d5
        ; header: "Row" + channel names, the cursor channel inverted
        lea     str_tk_row(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        moveq   #0,d7
.hdr:   move.w  d7,d0
        lsl.w   #2,d0
        lea     tk_chname_tab(pc),a0
        move.l  (a0,d0.w),a0
        move.w  d7,d0
        mulu    #7,d0
        add.w   d6,d0
        addq.w  #4,d0
        move.w  d5,d1
        move.w  #ATTR_ACC,d4
        cmp.w   v_tk_ch(a4),d7
        bne     .hat
        move.w  #ATTR_INV,d4
.hat:   bsr     draw_str
        addq.w  #1,d7
        cmp.w   #TK_CHANS,d7
        blt     .hdr
        ; keep the cursor row in view
        move.w  v_tk_row(a4),d0
        move.w  v_tk_top(a4),d1
        cmp.w   d1,d0
        bge     .t1
        move.w  d0,d1
.t1:    move.w  d1,d2
        add.w   #TK_VIEW,d2
        cmp.w   d2,d0
        blt     .t2
        move.w  d0,d1
        sub.w   #TK_VIEW-1,d1
.t2:    move.w  d1,v_tk_top(a4)
        ; rows
        addq.w  #1,d5
        move.w  v_tk_top(a4),d7     ; row index
.row:   ; build "NN  C#2 S  ..." into npstat
        lea     v_npstat(a4),a1
        move.w  d7,d0
        moveq   #0,d1
        and.l   #$FFFF,d0
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
        ; attrs: cursor row inverted, playing row accent
        move.w  #ATTR_NORM,d4
        cmp.w   v_tk_row(a4),d7
        bne     .ncur
        move.w  #ATTR_INV,d4
        ; cursor bar
        movem.l d1/d3,-(sp)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  d5,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        moveq   #1,d3
        movem.l d4,-(sp)
        add.w   #T_SOLBG,d4
        bsr     fill_cells
        movem.l (sp)+,d4
        movem.l (sp)+,d1/d3
        bra     .draw
.ncur:  move.b  v_tk_playing(a4),d0
        beq     .draw
        cmp.w   v_tk_prow(a4),d7
        bne     .draw
        move.w  #ATTR_ACC,d4
.draw:  lea     v_npstat(a4),a0
        move.w  d6,d0
        move.w  d5,d1
        bsr     draw_str
        addq.w  #1,d5
        addq.w  #1,d7
        move.w  v_tk_top(a4),d0
        add.w   #TK_VIEW,d0
        cmp.w   d0,d7
        blt     .row
        ; footers
        lea     str_tk_foot1(pc),a0
        move.w  d6,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #3,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        lea     str_tk_foot2(pc),a0
        move.w  d6,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #2,d1
        bsr     draw_str
        movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; ----------------------------------------------------------------------------
; tracker_key - d1 = ascii, d2 = raw -> d0 = 0 consumed / 1 not
; ----------------------------------------------------------------------------
tracker_key:
        lea     VARS,a4
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
        cmp.b   #'t',d1
        beq     .tapew
        cmp.b   #'y',d1
        beq     .taper
        cmp.b   #32,d1              ; space (pad Y) = play/stop
        beq     .playstop
        moveq   #1,d0
        rts
.up:    move.w  v_tk_row(a4),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,v_tk_row(a4)
        bra     .redraw
.down:  move.w  v_tk_row(a4),d0
        cmp.w   #TK_ROWS-1,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,v_tk_row(a4)
        bra     .redraw
.left:  move.w  v_tk_ch(a4),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,v_tk_ch(a4)
        bra     .redraw
.right: move.w  v_tk_ch(a4),d0
        cmp.w   #TK_CHANS-1,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,v_tk_ch(a4)
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
.save:  lea     str_tk_song(pc),a0  ; SONG.TRK -> cartridge SRAM
        lea     VARS+v_tkpat,a1
        move.w  #TK_PATLEN,d1
        bsr     sram_save
        bra     .redraw
.load:  lea     str_tk_song(pc),a0
        bsr     sram_find
        tst.w   d0
        bmi     .redraw
        lea     VARS+v_tkpat,a1
        move.w  #TK_PATLEN,d2
        bsr     sram_read
        bra     .redraw
.tapew: lea     str_tk_song(pc),a0  ; pattern -> PSG AFSK (tape.i)
        lea     VARS+v_tkpat,a1
        move.w  #TK_PATLEN,d0
        bsr     tape_save_blk
        bra     .redraw
.taper: lea     v_tkpat(a4),a0      ; tape -> pattern
        move.l  a0,v_tp_dst(a4)
        lea     v_numbuf(a4),a0     ; landing spot for the block name
        move.l  a0,v_tp_nmd(a4)
        move.w  #TK_PATLEN,v_tp_max(a4)
        bsr     tape_load_core
        bra     .redraw
.playstop:
        move.b  v_tk_playing(a4),d0
        beq     .start
        bsr     tk_stop
        bra     .redraw
.start: bsr     music_stop          ; the Tracker owns the PSG now
        bsr     gm_stop
        st      v_tk_playing(a4)
        move.w  #TK_ROWS-1,v_tk_prow(a4) ; first tick wraps to row 0
        move.l  v_ticks(a4),d0
        subq.l  #6,d0
        move.l  d0,v_tk_last(a4)
        bra     .redraw
.preview:
        ; hear the edit: trigger the cursor row immediately
        move.w  v_tk_row(a4),d0
        bsr     tk_trigger_row
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

; tk_curcell -> a0 = the cell under the cursor
tk_curcell:
        movem.l d0-d1,-(sp)
        move.w  v_tk_row(a4),d0
        move.w  v_tk_ch(a4),d1
        bsr     tk_cell
        movem.l (sp)+,d0-d1
        rts

; tk_load_demo - copy the built-in demo song into the pattern
tk_load_demo:
        movem.l d0/a0-a1,-(sp)
        lea     tk_demo(pc),a0
        lea     VARS+v_tkpat,a1
        move.w  #TK_PATLEN/4-1,d0
.cp:    move.l  (a0)+,(a1)+
        dbra    d0,.cp
        movem.l (sp)+,d0/a0-a1
        rts

; ---------------------------------------------------------------- data
str_t_tracker:  dc.b    "Tracker",0
name_tracker:   dc.b    "Tracker",0
str_tk_row:     dc.b    "Row",0
str_tk_ch1:     dc.b    "Ch1",0
str_tk_ch2:     dc.b    "Ch2",0
str_tk_ch3:     dc.b    "Ch3",0
str_tk_ch4:     dc.b    "Nz",0
str_tk_foot1:   dc.b    "q/w:note e:inst x:clr d:demo",0
str_tk_foot2:   dc.b    "Y:play s/l:sram t/y:tape",0
str_tk_song:    dc.b    "SONG.TRK",0,0,0,0
        even
tk_chname_tab:
        dc.l    str_tk_ch1
        dc.l    str_tk_ch2
        dc.l    str_tk_ch3
        dc.l    str_tk_ch4

; demo song: 32 rows x 4 channels x (note, instr) - byte-identical to
; the Amiga tracker's demo (notes 1 = C-2 .. 24 = B-3).
; ch1 bass, ch2 arp, ch3 melody, ch4 noise hits.
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
