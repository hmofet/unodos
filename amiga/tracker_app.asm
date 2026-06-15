; ============================================================================
; UnoDOS/68K Tracker - DISK-LOADED app (TRACKER.APP, proc 9). Was tracker.i's
; tracker_draw/key/tick (+ tk_cell/tk_trigger_row/tk_fmt_cell/tk_load_demo)
; built into the kernel; now a separate -Fbin binary loaded off DF1. Links to
; the kernel only via APIVEC (KCALL/KDATA) and drives Paula directly.
;
; The instrument waves are synthesized into chip RAM at boot by the kernel's
; tk_init and live at the fixed TKWAVES address (relocation-proof). The pattern
; (tk_pat) and the cursor/playback state (tk_*) stay in the kernel 'vars' block;
; tk_stop is kernel-resident (close_window also calls it) and is reached via
; APIVEC. npstat is the shared row-string scratch. The period / note-name /
; instrument tables and the demo song are app-private (moved out of the kernel).
; ============================================================================

        mc68000
        include "sysabi.i"
        include "build/kernel_api.inc"

TK_ROWS     equ 32
TK_CHANS    equ 4
TK_VIEW     equ 12                  ; visible rows
TK_WAVELEN  equ 32                  ; bytes per instrument wave
TKWAVES     equ $76B00              ; 4 x 32 bytes, chip RAM (filled by tk_init)

; ---- Paula custom registers (fixed hardware addresses) ----
CUSTOM      equ $DFF000
DMACON      equ $096
AUD0LCH     equ $0A0
AUD0VOL     equ $0A8

        org     APPSLOT0
; ---- JMP table (open/draw/key/tick/click) ----
        jmp     tracker_open(pc)
        jmp     tracker_draw(pc)
        jmp     tracker_key(pc)
        jmp     tracker_tick(pc)
        jmp     tracker_click(pc)

tracker_open:
        rts
tracker_click:
        rts

; tk_cell - d0=row d1=chan, a4=&vars -> a0 = cell ptr (2 bytes: note, instr)
tk_cell:
        movem.l d0-d1,-(sp)
        lsl.w   #2,d0               ; row * 4 chans
        add.w   d1,d0
        add.w   d0,d0               ; * 2 bytes
        lea     VO_tk_pat(a4),a0
        lea     (a0,d0.w),a0
        movem.l (sp)+,d0-d1
        rts

; tk_trigger_row - d0 = row: fire the notes of one row on Paula. a4 = &vars
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
        move.w  d1,d4
        lsl.w   #4,d4               ; chan * $10
        lea     AUD0LCH(a6),a3
        add.w   d4,a3
        move.w  d3,d5
        lsl.w   #5,d5               ; * TK_WAVELEN
        add.l   #TKWAVES,d5
        move.l  d5,(a3)             ; AUDxLCH+LCL
        move.w  #TK_WAVELEN/2,4(a3) ; AUDxLEN
        subq.w  #1,d2
        add.w   d2,d2
        lea     tk_periods(pc),a0
        move.w  (a0,d2.w),6(a3)     ; AUDxPER
        move.w  #44,8(a3)           ; AUDxVOL
        moveq   #1,d5
        lsl.w   d1,d5
        or.w    #$8200,d5           ; SET | DMAEN | AUDxEN
        move.w  d5,DMACON(a6)
.next:  addq.w  #1,d1
        cmp.w   #TK_CHANS,d1
        blt     .ch
        movem.l (sp)+,d0-d5/a0/a3/a6
        rts

; tracker_tick - playback sequencer (focused window via scheduler; 6 ticks/row)
tracker_tick:
        movem.l d0-d2/a2/a4,-(sp)
        KDATA   a4,vars
        move.b  VO_tk_playing(a4),d0
        beq     .out
        move.l  VO_ticks(a4),d0
        sub.l   VO_tk_last(a4),d0
        cmp.l   #6,d0
        blt     .out
        move.l  VO_ticks(a4),d0
        move.l  d0,VO_tk_last(a4)
        move.w  VO_tk_prow(a4),d0
        addq.w  #1,d0
        cmp.w   #TK_ROWS,d0
        blt     .rok
        moveq   #0,d0               ; loop the pattern
.rok:   move.w  d0,VO_tk_prow(a4)
        bsr     tk_trigger_row
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
        ; header: channel names, selected channel in accent
        move.w  WX(a2),d6
        addq.w  #6,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+2,d5
        lea     str_tk_hdr(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #1,d2
        KCALL   draw_string
        ; channel cursor marker: caret under the selected channel column
        move.w  VO_tk_ch(a4),d0
        mulu    #56,d0
        add.w   d6,d0
        add.w   #24,d0
        move.w  d5,d1
        lea     str_tk_mark(pc),a0
        moveq   #2,d2
        KCALL   draw_string
        ; keep the cursor row in view
        move.w  VO_tk_row(a4),d0
        move.w  VO_tk_top(a4),d1
        cmp.w   d1,d0
        bge     .t1
        move.w  d0,d1
.t1:    move.w  d1,d2
        add.w   #TK_VIEW,d2
        cmp.w   d2,d0
        blt     .t2
        move.w  d0,d1
        sub.w   #TK_VIEW-1,d1
.t2:    move.w  d1,VO_tk_top(a4)
        ; rows
        add.w   #12,d5
        move.w  VO_tk_top(a4),d7    ; row index
.row:   ; build "NN  C-2 S  ..." into npstat
        KDATA   a1,npstat
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
        cmp.w   VO_tk_row(a4),d7
        bne     .ncur
        moveq   #0,d2
        moveq   #1,d3
        bra     .draw
.ncur:  move.b  VO_tk_playing(a4),d0
        beq     .draw
        cmp.w   VO_tk_prow(a4),d7
        bne     .draw
        moveq   #3,d2
        moveq   #2,d3
.draw:  KDATA   a0,npstat
        move.w  d6,d0
        move.w  d5,d1
        KCALL   draw_string_bg
        add.w   #10,d5
        addq.w  #1,d7
        move.w  VO_tk_top(a4),d0
        add.w   #TK_VIEW,d0
        cmp.w   d0,d7
        blt     .row
        ; footer
        addq.w  #4,d5
        lea     str_tk_foot1(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #1,d2
        KCALL   draw_string
        add.w   #10,d5
        lea     str_tk_foot2(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #1,d2
        KCALL   draw_string
        movem.l (sp)+,d0-d7/a0-a4
        rts

; tracker_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
tracker_key:
        KDATA   a4,vars
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
.save:  move.b  VO_fat_mounted(a4),d0
        beq     .redraw
        lea     str_songname(pc),a0
        lea     VO_tk_pat(a4),a1
        move.l  #TK_ROWS*TK_CHANS*2,d1
        KCALL   fat_save_file
        KCALL   fat_list_root
        bra     .redraw
.load:  move.b  VO_fat_mounted(a4),d0
        beq     .redraw
        lea     str_songname(pc),a0
        KCALL   fat_find_file
        tst.w   d0
        bmi     .redraw
        move.l  #TK_ROWS*TK_CHANS*2,d1
        lea     VO_tk_pat(a4),a1
        KCALL   fat_read_file
        bra     .redraw
.up:    move.w  VO_tk_row(a4),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,VO_tk_row(a4)
        bra     .redraw
.down:  move.w  VO_tk_row(a4),d0
        cmp.w   #TK_ROWS-1,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,VO_tk_row(a4)
        bra     .redraw
.left:  move.w  VO_tk_ch(a4),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,VO_tk_ch(a4)
        bra     .redraw
.right: move.w  VO_tk_ch(a4),d0
        cmp.w   #TK_CHANS-1,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,VO_tk_ch(a4)
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
        move.b  VO_tk_playing(a4),d0
        beq     .start
        KCALL   tk_stop             ; kernel-resident (silences Paula)
        bra     .redraw
.start: st      VO_tk_playing(a4)
        move.w  #TK_ROWS-1,VO_tk_prow(a4)  ; first tick wraps to row 0
        move.l  VO_ticks(a4),d0
        subq.l  #6,d0
        move.l  d0,VO_tk_last(a4)
        bra     .redraw
.preview:
        ; hear the edit immediately: trigger just this cell's row on its chan
        move.w  VO_tk_row(a4),d0
        bsr     tk_trigger_row
.redraw:
        KCALL   redraw_topmost
        moveq   #0,d0
        rts

; tk_curcell - a4 = &vars -> a0 = cell at the cursor
tk_curcell:
        movem.l d0-d1,-(sp)
        move.w  VO_tk_row(a4),d0
        move.w  VO_tk_ch(a4),d1
        bsr     tk_cell
        movem.l (sp)+,d0-d1
        rts

; tk_load_demo - copy the built-in demo song into the pattern. a4 = &vars
tk_load_demo:
        movem.l d0/a0-a1,-(sp)
        lea     tk_demo(pc),a0
        lea     VO_tk_pat(a4),a1
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
        even
; ProTracker periods, C-2..B-3 (notes 1..24)
tk_periods:
        dc.w    428,404,381,360,339,320,302,285,269,254,240,226
        dc.w    214,202,190,180,170,160,151,143,135,127,120,113
tk_notenames:                       ; 2 chars per semitone
        dc.b    "C-C#D-D#E-F-F#G-G#A-A#B-"
tk_instname:
        dc.b    "SQ","SW","TR","NZ"
        even
; demo song: 32 rows x 4 channels x (note, instr). Notes: 1=C-2 .. 24=B-3.
tk_demo:
        dc.b    1,1,  13,0,  0,0,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  17,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    1,1,  20,0,  0,0,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  17,0, 13,2,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  13,0, 17,2,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  15,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  20,0, 15,2,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  15,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    6,1,  10,0, 13,2,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  13,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    6,1,  17,0,  0,0,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  13,0, 10,2,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  11,0, 15,2,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  15,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  20,0, 19,2,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  23,0,  0,0,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        even
str_songname:   dc.b    "SONG    UNO"
        even
        end
