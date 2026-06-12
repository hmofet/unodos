; ============================================================================
; UnoDOS/Genesis milestone-1 apps: SysInfo, Clock, Notepad, Music.
; Ported from amiga/apps_m2.i; drawing is cell-based (draw_str/fill_cells
; with an attr in d4), the buffer/caret/sequencer logic is byte-identical.
; Key handlers: in d1 = ascii, d2 = raw; out d0 = 0 consumed / 1 not.
; ============================================================================

; ---------------------------------------------------------------------------
; fmt_dec - d0.w unsigned -> decimal digits + NUL at a0 (a0 unchanged)
; ---------------------------------------------------------------------------
fmt_dec:
        movem.l d0-d1/a0-a1,-(sp)
        and.l   #$FFFF,d0
        lea     8(a0),a1
        clr.b   -(a1)
.dig:   divu    #10,d0
        swap    d0
        add.b   #'0',d0
        move.b  d0,-(a1)
        clr.w   d0
        swap    d0
        tst.w   d0
        bne     .dig
.copy:  move.b  (a1)+,(a0)+
        bne     .copy
        movem.l (sp)+,d0-d1/a0-a1
        rts

; str_append - append NUL string a1 at cursor a0 (a0 -> new NUL)
str_append:
.cp:    move.b  (a1)+,(a0)+
        bne     .cp
        subq.l  #1,a0
        rts

; put2dig - d0.w (0..99) -> two ASCII digits at (a0)+. Trashes d0/d1.
put2dig:
        and.l   #$FFFF,d0
        divu    #10,d0
        move.l  d0,d1
        add.b   #'0',d0
        move.b  d0,(a0)+
        swap    d1
        add.b   #'0',d1
        move.b  d1,(a0)+
        rts

; ============================================================================
; SysInfo (proc 0)
; ============================================================================
sysinfo_draw:
        movem.l d0-d7/a0/a4,-(sp)
        lea     VARS,a4
        move.w  WX(a2),d6
        addq.w  #2,d6
        move.w  WY(a2),d5
        addq.w  #2,d5
        lea     str_si1(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        move.w  #ATTR_NORM,d4
        bsr     draw_str
        lea     str_si2(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        addq.w  #1,d1
        bsr     draw_str
        lea     str_si3(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        addq.w  #2,d1
        bsr     draw_str
        lea     str_si4(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        addq.w  #4,d1
        bsr     draw_str
        ; uptime seconds (accent, fixed-position overwrite)
        move.l  v_ticks(a4),d0
        divu    #TICKS_SEC,d0
        lea     v_numbuf(a4),a0
        bsr     fmt_dec
        ; append "s "
        move.l  a0,a1
.find:  tst.b   (a1)+
        bne     .find
        subq.l  #1,a1
        move.b  #'s',(a1)+
        move.b  #' ',(a1)+
        clr.b   (a1)
        move.w  d6,d0
        addq.w  #8,d0
        move.w  d5,d1
        addq.w  #4,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        lea     str_si5(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        addq.w  #6,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        movem.l (sp)+,d0-d7/a0/a4
        rts

; ============================================================================
; Clock (proc 1) - uptime as HH:MM:SS
; ============================================================================
clock_draw:
        movem.l d0-d7/a0/a4,-(sp)
        lea     VARS,a4
        move.l  v_ticks(a4),d0
        divu    #TICKS_SEC,d0
        and.l   #$FFFF,d0
        divu    #60,d0
        move.l  d0,d3
        swap    d3                  ; d3.w = seconds
        and.l   #$FFFF,d0
        divu    #60,d0
        move.l  d0,d2
        swap    d2                  ; d2.w = minutes
        and.w   #$FFFF,d0           ; d0.w = hours
        lea     v_clkbuf(a4),a0
        bsr     put2dig
        move.b  #':',(a0)+
        move.w  d2,d0
        bsr     put2dig
        move.b  #':',(a0)+
        move.w  d3,d0
        bsr     put2dig
        clr.b   (a0)
        ; caption
        lea     str_uptime(pc),a0
        move.w  WX(a2),d0
        addq.w  #2,d0
        move.w  WY(a2),d1
        addq.w  #2,d1
        move.w  #ATTR_NORM,d4
        bsr     draw_str
        ; centered time, accent
        move.w  WX(a2),d0
        move.w  WW(a2),d1
        lsr.w   #1,d1
        add.w   d1,d0
        subq.w  #4,d0               ; 8 chars / 2
        move.w  WY(a2),d1
        move.w  WH(a2),d3
        lsr.w   #1,d3
        add.w   d3,d1
        lea     v_clkbuf(a4),a0
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        movem.l (sp)+,d0-d7/a0/a4
        rts

; ============================================================================
; Notepad (proc 2) - 2KB edit buffer, caret, line nav, soft/PS2 input
; ============================================================================

; notepad_set_demo - load demo_text (AUTOTEST + default composite)
notepad_set_demo:
        movem.l d0-d1/a0-a1/a4,-(sp)
        lea     VARS,a4
        lea     demo_text(pc),a0
        lea     v_npbuf(a4),a1
        moveq   #0,d1
.cp:    move.b  (a0)+,d0
        beq     .done
        move.b  d0,(a1)+
        addq.w  #1,d1
        bra     .cp
.done:  move.w  d1,v_np_len(a4)
        move.w  d1,v_np_caret(a4)
        clr.w   v_np_top(a4)
        move.w  #-1,v_np_goal(a4)
        st      v_np_dirty(a4)
        lea     str_np_demo(pc),a0
        lea     v_np_name(a4),a1
        moveq   #11,d1
.nmcp:  move.b  (a0)+,(a1)+
        dbra    d1,.nmcp
        movem.l (sp)+,d0-d1/a0-a1/a4
        rts
str_np_demo:    dc.b    "DEMO.TXT",0,0,0,0
        even

; notepad_linecol - -> d0 = line, d1 = col of the caret (0-based)
notepad_linecol:
        movem.l d2-d3/a0/a4,-(sp)
        lea     VARS,a4
        lea     v_npbuf(a4),a0
        moveq   #0,d0
        moveq   #0,d1
        moveq   #0,d2
.scan:  cmp.w   v_np_caret(a4),d2
        bge     .done
        move.b  (a0)+,d3
        cmp.b   #13,d3
        bne     .ncr
        addq.w  #1,d0
        moveq   #0,d1
        bra     .nx
.ncr:   addq.w  #1,d1
.nx:    addq.w  #1,d2
        bra     .scan
.done:  movem.l (sp)+,d2-d3/a0/a4
        rts

; notepad_seek_linecol - d0 = target line, d2 = goal col
;   -> d0 = caret index (clamped to line end), or -1 if no such line
notepad_seek_linecol:
        movem.l d3-d5/a0/a4,-(sp)
        lea     VARS,a4
        lea     v_npbuf(a4),a0
        moveq   #0,d3               ; index
        moveq   #0,d4               ; line
.fs:    cmp.w   d0,d4
        beq     .at
        cmp.w   v_np_len(a4),d3
        bge     .nofind
        cmp.b   #13,(a0,d3.w)
        bne     .fnc
        addq.w  #1,d4
.fnc:   addq.w  #1,d3
        bra     .fs
.at:    moveq   #0,d5               ; col
.adv:   cmp.w   d2,d5
        bge     .found
        cmp.w   v_np_len(a4),d3
        bge     .found
        cmp.b   #13,(a0,d3.w)
        beq     .found
        addq.w  #1,d3
        addq.w  #1,d5
        bra     .adv
.found: move.w  d3,d0
        movem.l (sp)+,d3-d5/a0/a4
        rts
.nofind:
        moveq   #-1,d0
        movem.l (sp)+,d3-d5/a0/a4
        rts

; notepad_draw - a2 = window
notepad_draw:
        movem.l d0-d7/a0-a4,-(sp)
        lea     VARS,a4
        ; clear the content area (rows 1..h-3; h-2 is the status row)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        subq.w  #3,d3
        move.w  #ATTR_NORM+T_SOLBG,d4
        bsr     fill_cells
        ; layout
        move.w  WX(a2),d6
        addq.w  #1,d6               ; text x
        move.w  WY(a2),d5
        addq.w  #1,d5               ; first line y
        move.w  WW(a2),d7
        subq.w  #2,d7               ; cols
        cmp.w   #42,d7
        ble     .wok
        moveq   #42,d7
.wok:   move.w  WH(a2),d4
        subq.w  #3,d4               ; visible rows
        ; vertical scroll: clamp np_top so the caret line stays visible
        tst.w   d4
        ble     .nsdone
        bsr     notepad_linecol     ; d0 = caret line
        move.w  v_np_top(a4),d1
        cmp.w   d1,d0
        bge     .ntop
        move.w  d0,d1
.ntop:  move.w  d1,d2
        add.w   d4,d2
        cmp.w   d2,d0
        blt     .nbot
        move.w  d0,d1
        sub.w   d4,d1
        addq.w  #1,d1
.nbot:  move.w  d1,v_np_top(a4)
.nsdone:
        ; skip np_top lines
        lea     v_npbuf(a4),a0
        move.w  v_np_top(a4),d2
        moveq   #0,d0
.sktop: tst.w   d2
        beq     .skdone
        cmp.w   v_np_len(a4),d0
        bge     .skdone
        cmp.b   #13,(a0,d0.w)
        bne     .sknc
        subq.w  #1,d2
.sknc:  addq.w  #1,d0
        bra     .sktop
.skdone:
        lea     (a0,d0.w),a0
        move.w  v_np_top(a4),d3     ; d3 = line index
.line:  tst.w   d4
        ble     .status
        ; find line end: a1 = line start, d2 = len
        move.l  a0,a1
        moveq   #0,d2
.find:  move.l  a1,d0
        lea     v_npbuf(a4),a3
        sub.l   a3,d0
        add.w   d2,d0
        cmp.w   v_np_len(a4),d0
        bge     .eol
        move.b  (a1,d2.w),d1
        cmp.b   #13,d1
        beq     .eol
        addq.w  #1,d2
        bra     .find
.eol:
        ; copy min(d2,d7) chars into npline + NUL
        move.w  d2,d0
        cmp.w   d7,d0
        ble     .lenok
        move.w  d7,d0
.lenok: lea     v_npline(a4),a3
        move.w  d0,d1
        beq     .zt
        subq.w  #1,d1
        movem.l a1,-(sp)
.cpl:   move.b  (a1)+,(a3)+
        dbra    d1,.cpl
        movem.l (sp)+,a1
.zt:    clr.b   (a3)
        ; draw the line
        movem.l d2-d4,-(sp)
        lea     v_npline(a4),a0
        move.w  d6,d0
        move.w  d5,d1
        move.w  #ATTR_NORM,d4
        bsr     draw_str
        movem.l (sp)+,d2-d4
        ; caret on this line? (inverted cell, PORT-SPEC caret analogue)
        movem.l d2-d4/a1,-(sp)
        bsr     notepad_linecol     ; d0 = line, d1 = col
        cmp.w   d3,d0
        bne     .nocaret
        move.w  d7,d2
        subq.w  #1,d2
        cmp.w   d2,d1
        ble     .colok
        move.w  d2,d1
.colok:
        move.w  d1,d0
        add.w   d6,d0               ; caret cell x
        move.w  d5,d1               ; caret cell y
        ; char under the caret (or space at EOL/EOF)
        moveq   #32,d2
        move.w  v_np_caret(a4),d3
        cmp.w   v_np_len(a4),d3
        bge     .blank
        lea     v_npbuf(a4),a3
        move.b  (a3,d3.w),d2
        cmp.b   #13,d2
        bne     .blank
        moveq   #32,d2
.blank: move.w  #ATTR_INV,d4
        bsr     draw_char
.nocaret:
        movem.l (sp)+,d2-d4/a1
        ; advance past the CR
        lea     1(a1,d2.w),a0
        move.l  a0,d0
        lea     v_npbuf(a4),a3
        sub.l   a3,d0
        cmp.w   v_np_len(a4),d0
        bgt     .status
        addq.w  #1,d5
        addq.w  #1,d3
        subq.w  #1,d4
        bra     .line
.status:
        ; status row (h-2): white bar + "Ln x Co y  NNN B [*]"
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #2,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        moveq   #1,d3
        move.w  #ATTR_INV+T_SOLBG,d4
        bsr     fill_cells
        ; build the string
        lea     v_npstat(a4),a0
        lea     str_n_ln(pc),a1
        bsr     str_append
        bsr     notepad_linecol     ; d0 = line, d1 = col
        move.w  d1,-(sp)
        addq.w  #1,d0
        bsr     fmt_dec
.skip1: tst.b   (a0)+
        bne     .skip1
        subq.l  #1,a0
        lea     str_n_co(pc),a1
        bsr     str_append
        move.w  (sp)+,d0
        addq.w  #1,d0
        bsr     fmt_dec
.skip2: tst.b   (a0)+
        bne     .skip2
        subq.l  #1,a0
        move.b  #' ',(a0)+
        move.w  v_np_len(a4),d0
        bsr     fmt_dec
.skip3: tst.b   (a0)+
        bne     .skip3
        subq.l  #1,a0
        lea     str_n_b(pc),a1
        bsr     str_append
        move.b  v_np_dirty(a4),d0
        beq     .nodirty
        lea     str_n_dirty(pc),a1
        bsr     str_append
.nodirty:
        lea     str_n_save(pc),a1
        bsr     str_append
        lea     v_npstat(a4),a0
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #2,d1
        move.w  #ATTR_INV,d4
        bsr     draw_str
        movem.l (sp)+,d0-d7/a0-a4
        rts

; notepad_key - d1 = ascii, d2 = raw -> d0 = 0 consumed / 1 not
notepad_key:
        lea     VARS,a4
        cmp.b   #$4F,d2             ; left
        beq     .left
        cmp.b   #$4E,d2             ; right
        beq     .right
        cmp.b   #$4C,d2             ; up
        beq     .up
        cmp.b   #$4D,d2             ; down
        beq     .down
        cmp.b   #$50,d2             ; F1 = save to SRAM under np_name
        beq     .save
        cmp.b   #8,d1               ; backspace
        beq     .bs
        cmp.b   #13,d1              ; return inserts CR
        beq     .ins
        cmp.b   #32,d1
        bge     .ins
        moveq   #1,d0               ; not consumed (incl. ESC)
        rts
.left:  move.w  #-1,v_np_goal(a4)
        move.w  v_np_caret(a4),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,v_np_caret(a4)
        bra     .redraw
.right: move.w  #-1,v_np_goal(a4)
        move.w  v_np_caret(a4),d0
        cmp.w   v_np_len(a4),d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,v_np_caret(a4)
        bra     .redraw
.up:    bsr     notepad_linecol     ; d0 = line, d1 = col
        tst.w   d0
        beq     .redraw
        move.w  v_np_goal(a4),d2
        bpl     .upg
        move.w  d1,d2
        move.w  d2,v_np_goal(a4)
.upg:   subq.w  #1,d0
        bsr     notepad_seek_linecol
        tst.w   d0
        bmi     .redraw
        move.w  d0,v_np_caret(a4)
        bra     .redraw
.down:  bsr     notepad_linecol
        move.w  v_np_goal(a4),d2
        bpl     .dng
        move.w  d1,d2
        move.w  d2,v_np_goal(a4)
.dng:   addq.w  #1,d0
        bsr     notepad_seek_linecol
        tst.w   d0
        bmi     .redraw
        move.w  d0,v_np_caret(a4)
        bra     .redraw
.bs:    move.w  #-1,v_np_goal(a4)
        move.w  v_np_caret(a4),d0
        beq     .redraw
        ; shift [caret..len) left by one
        lea     v_npbuf(a4),a0
        move.w  v_np_caret(a4),d1
        move.w  v_np_len(a4),d2
        sub.w   d1,d2
        lea     (a0,d1.w),a1
.bsloop:
        tst.w   d2
        beq     .bsdone
        move.b  (a1),-1(a1)
        addq.l  #1,a1
        subq.w  #1,d2
        bra     .bsloop
.bsdone:
        subq.w  #1,d0
        move.w  d0,v_np_caret(a4)
        move.w  v_np_len(a4),d0
        subq.w  #1,d0
        move.w  d0,v_np_len(a4)
        st      v_np_dirty(a4)
        bra     .redraw
.ins:   move.w  #-1,v_np_goal(a4)
        move.w  v_np_len(a4),d0
        cmp.w   #NBUF-1,d0
        bge     .redraw
        ; shift [caret..len) right by one (backwards copy)
        lea     v_npbuf(a4),a0
        move.w  v_np_len(a4),d2
        sub.w   v_np_caret(a4),d2
        lea     (a0,d0.w),a1        ; one past the last byte
.insloop:
        tst.w   d2
        beq     .insdone
        move.b  -(a1),d3
        move.b  d3,1(a1)
        subq.w  #1,d2
        bra     .insloop
.insdone:
        move.w  v_np_caret(a4),d0
        move.b  d1,(a0,d0.w)        ; ascii (13 for return)
        addq.w  #1,d0
        move.w  d0,v_np_caret(a4)
        move.w  v_np_len(a4),d0
        addq.w  #1,d0
        move.w  d0,v_np_len(a4)
        st      v_np_dirty(a4)
        bra     .redraw
.save:  bsr     np_save_sram        ; clears the dirty flag on success
        bra     .redraw
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

; ============================================================================
; Music (proc 3) - PSG channel-0 square-wave sequencer, Canon in D
; ============================================================================

; psg_tone - d1.w = 10-bit PSG tone value -> channel 0
psg_tone:
        movem.l d0-d1,-(sp)
        move.w  d1,d0
        and.b   #$0F,d0
        or.b    #$80,d0             ; latch: ch0 tone low nibble
        move.b  d0,PSG
        lsr.w   #4,d1
        and.b   #$3F,d1             ; data: high 6 bits
        move.b  d1,PSG
        movem.l (sp)+,d0-d1
        rts

; music_start
music_start:
        movem.l d0-d1/a0/a4,-(sp)
        lea     VARS,a4
        clr.w   v_mus_ix(a4)
        st      v_mus_playing(a4)
        lea     mus_notes(pc),a0
        move.w  (a0),d1             ; first tone
        bsr     psg_tone
        move.b  #$92,PSG            ; ch0 volume on (2 of 15 attenuation)
        move.l  v_ticks(a4),d0
        moveq   #0,d1
        move.w  2(a0),d1            ; first duration
        add.l   d1,d0
        move.l  d0,v_mus_end(a4)
        movem.l (sp)+,d0-d1/a0/a4
        rts

; music_stop
music_stop:
        movem.l a4,-(sp)
        lea     VARS,a4
        sf      v_mus_playing(a4)
        move.b  #$9F,PSG            ; ch0 volume off
        movem.l (sp)+,a4
        rts

; music_tick - advance the sequencer; refresh the window when topmost
music_tick:
        movem.l d0-d3/a0/a2/a4,-(sp)
        lea     VARS,a4
        move.b  v_mus_playing(a4),d0
        beq     .out
        move.l  v_ticks(a4),d0
        cmp.l   v_mus_end(a4),d0
        blt     .out
        move.w  v_mus_ix(a4),d1
        addq.w  #1,d1
        cmp.w   mus_count(pc),d1
        blt     .ixok
        moveq   #0,d1               ; loop
.ixok:  move.w  d1,v_mus_ix(a4)
        move.w  d1,d2
        mulu    #6,d2
        lea     mus_notes(pc),a0
        add.w   d2,a0
        move.w  (a0),d1
        bsr     psg_tone
        moveq   #0,d2
        move.w  2(a0),d2
        add.l   d2,d0
        move.l  d0,v_mus_end(a4)
        ; topmost-only visual refresh (PORT-SPEC SS2)
        move.w  v_zcount(a4),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        moveq   #0,d3
        move.b  WPROC(a2),d3
        cmp.w   #3,d3
        bne     .out
        bsr     music_draw
.out:   movem.l (sp)+,d0-d3/a0/a2/a4
        rts

; music_key - d1 = ascii -> d0 = 0 consumed / 1 not
music_key:
        cmp.b   #' ',d1
        beq     .toggle
        moveq   #1,d0
        rts
.toggle:
        move.b  VARS+v_mus_playing,d0
        beq     .play
        bsr     music_stop
        bra     .rd
.play:  bsr     music_start
.rd:    bsr     redraw_topmost
        moveq   #0,d0
        rts

; music_draw - a2 = window (title, staff, note blocks, footer)
music_draw:
        movem.l d0-d7/a0/a4,-(sp)
        lea     VARS,a4
        ; clear content
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        subq.w  #3,d3
        move.w  #ATTR_NORM+T_SOLBG,d4
        bsr     fill_cells
        ; title
        lea     str_m_title(pc),a0
        move.w  WX(a2),d0
        addq.w  #2,d0
        move.w  WY(a2),d1
        addq.w  #2,d1
        move.w  #ATTR_NORM,d4
        bsr     draw_str
        ; staff: 5 hline rows (y+4 .. y+8)
        move.w  WX(a2),d0
        addq.w  #2,d0
        move.w  WY(a2),d1
        addq.w  #4,d1
        move.w  WW(a2),d2
        subq.w  #4,d2
        moveq   #5,d3
        move.w  #ATTR_NORM+T_HLINE,d4
        bsr     fill_cells
        ; notes: solid blocks on the staff, the playing one in magenta
        moveq   #0,d7
.note:  cmp.w   mus_count(pc),d7
        bge     .foot
        ; x = wx + 2 + i*(ww-5)/count
        move.w  WW(a2),d0
        subq.w  #5,d0
        mulu    d7,d0
        divu    mus_count(pc),d0
        add.w   WX(a2),d0
        addq.w  #2,d0
        ; y = wy + 8 - yoff/8
        move.w  d7,d2
        mulu    #6,d2
        lea     mus_notes(pc),a0
        add.w   d2,a0
        move.w  4(a0),d1            ; staff y offset (pixels)
        lsr.w   #3,d1
        neg.w   d1
        add.w   WY(a2),d1
        addq.w  #8,d1
        move.w  #ATTR_NORM+T_SOLCY,d4
        move.b  v_mus_playing(a4),d5
        beq     .col
        cmp.w   v_mus_ix(a4),d7
        bne     .col
        move.w  #ATTR_NORM+T_SOLMG,d4
.col:   moveq   #1,d2
        moveq   #1,d3
        bsr     fill_cells
        addq.w  #1,d7
        bra     .note
.foot:
        lea     str_m_play(pc),a0
        move.w  WX(a2),d0
        addq.w  #2,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #2,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        movem.l (sp)+,d0-d7/a0/a4
        rts

; ---------------------------------------------------------------- strings
        even
str_si1:        dc.b    "Video   320x224 VDP",0
str_si2:        dc.b    "Machine Sega Genesis - MD",0
str_si3:        dc.b    "RAM     64 KB",0
str_si4:        dc.b    "Uptime",0
str_si5:        dc.b    "UnoDOS/Genesis  Milestone 6",0
str_uptime:     dc.b    "Uptime",0
str_n_ln:       dc.b    "Ln ",0
str_n_co:       dc.b    " Co ",0
str_n_b:        dc.b    " B",0
str_n_dirty:    dc.b    " *",0
str_n_save:     dc.b    "  F1:save",0
str_m_title:    dc.b    "Canon in D  (Pachelbel)",0
str_m_play:     dc.b    "Y:play/stop  (PSG ch0)",0
demo_text:      dc.b    "UnoDOS/Genesis milestone 6",13
                dc.b    "The quick brown fox",13
                dc.b    "jumps over the lazy dog.",13
                dc.b    "L04 scroll test",13
                dc.b    "L05 scroll test",13
                dc.b    "L06 scroll test",13
                dc.b    "L07 scroll test",13
                dc.b    "L08 scroll test",13
                dc.b    "L09 scroll test",13
                dc.b    "L10 scroll test",13
                dc.b    "L11 scroll test",13
                dc.b    "L12 scroll test",13
                dc.b    "L13 scroll test",13
                dc.b    "L14 scroll test",13
                dc.b    "L15 scroll test",13
                dc.b    "L16 scroll test",13
                dc.b    "L17 scroll test",13
                dc.b    "L18 last line",0
        even
