; ============================================================================
; UnoDOS/Genesis soft keyboard - kernel overlay on cell rows 22..27.
; Clicking a key posts a normal EV_KEY through the event queue with the
; Amiga-port raw codes (arrows $4C-$4F, F1 $50), so apps can't tell a
; soft key from a real PS/2 keystroke. Toggled with the pad's B button.
;
; Key table entry (10 bytes): x.b, row.b (0-4), w.b, ascii.b, raw.b,
; pad.b, label (4 bytes, NUL-padded). raw $FE = the sticky Shift key.
; ============================================================================

KB_ROW0     equ KBD_TOP+1           ; first key row (22 = panel header)
KB_NKEYS    equ 55
KB_SHIFTIX  equ 39                  ; index of the Shift key (redraws)

; softkbd_show / softkbd_hide / softkbd_draw
softkbd_show:
        movem.l d0/a4,-(sp)
        lea     VARS,a4
        st      v_kb_vis(a4)
        move.w  #-1,v_kb_hover(a4)
        bsr     softkbd_draw
        movem.l (sp)+,d0/a4
        rts

softkbd_hide:
        movem.l d0/a4,-(sp)
        lea     VARS,a4
        sf      v_kb_vis(a4)
        sf      v_kb_shift(a4)
        bsr     repaint_all
        movem.l (sp)+,d0/a4
        rts

softkbd_draw:
        movem.l d0-d7/a0-a1,-(sp)
        ; panel: cyan slab over rows KBD_TOP..27
        moveq   #0,d0
        moveq   #KBD_TOP,d1
        moveq   #SCRW_C,d2
        moveq   #SCRH_C-KBD_TOP,d3
        move.w  #ATTR_KEY+T_SOLBG,d4
        bsr     fill_cells
        ; keys
        moveq   #0,d7
.key:   move.w  d7,d0
        bsr     softkbd_drawkey
        addq.w  #1,d7
        cmp.w   #KB_NKEYS,d7
        blt     .key
        movem.l (sp)+,d0-d7/a0-a1
        rts

; softkbd_keyent - d0 = index -> a0 = table entry. Preserves d0-d7.
softkbd_keyent:
        move.w  d0,-(sp)
        mulu    #10,d0
        lea     kb_keys(pc),a0
        add.w   d0,a0
        move.w  (sp)+,d0
        rts

; softkbd_drawkey - d0 = key index (hover/active shift = inverted)
softkbd_drawkey:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     VARS,a4
        move.w  d0,d7
        bsr     softkbd_keyent      ; a0 = entry
        ; attr: hovered key, or the Shift key while latched -> inverted
        move.w  #ATTR_KEY,d4
        cmp.w   v_kb_hover(a4),d7
        beq     .inv
        cmp.b   #$FE,4(a0)          ; the Shift key?
        bne     .attr
        move.b  v_kb_shift(a4),d1
        beq     .attr
.inv:   move.w  #ATTR_INV,d4
.attr:
        moveq   #0,d0
        move.b  (a0),d0             ; x
        moveq   #0,d1
        move.b  1(a0),d1            ; row
        add.w   #KB_ROW0,d1
        moveq   #0,d2
        move.b  2(a0),d2            ; w
        moveq   #1,d3
        move.w  d4,-(sp)
        add.w   #T_SOLBG,d4
        bsr     fill_cells
        move.w  (sp)+,d4
        ; centered label
        lea     6(a0),a1            ; label
        moveq   #0,d3               ; len
.len:   cmp.w   #4,d3
        bge     .gotlen
        move.b  (a1,d3.w),d5
        beq     .gotlen
        addq.w  #1,d3
        bra     .len
.gotlen:
        move.w  d2,d5
        sub.w   d3,d5
        asr.w   #1,d5
        bpl     .pad
        moveq   #0,d5
.pad:   add.w   d5,d0
        move.l  a1,a0
        bsr     draw_str
        movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; softkbd_key_at - d0 = cx, d1 = cy -> d0 = key index or -1
softkbd_key_at:
        movem.l d1-d4/a0,-(sp)
        sub.w   #KB_ROW0,d1
        blt     .miss
        cmp.w   #5,d1
        bge     .miss
        move.w  d0,d2               ; cx
        lea     kb_keys(pc),a0
        moveq   #0,d3               ; index
.scan:  moveq   #0,d4
        move.b  1(a0),d4            ; row
        cmp.w   d1,d4
        bne     .next
        moveq   #0,d4
        move.b  (a0),d4             ; x
        cmp.w   d4,d2
        blt     .next
        add.b   2(a0),d4            ; + w
        cmp.w   d4,d2
        bge     .next
        move.w  d3,d0
        movem.l (sp)+,d1-d4/a0
        rts
.next:  lea     10(a0),a0
        addq.w  #1,d3
        cmp.w   #KB_NKEYS,d3
        blt     .scan
.miss:  moveq   #-1,d0
        movem.l (sp)+,d1-d4/a0
        rts

; softkbd_click - d0 = cx, d1 = cy (cell coords of a press)
softkbd_click:
        movem.l d0-d3/a0/a4,-(sp)
        bsr     softkbd_key_at
        bmi     .out
        lea     VARS,a4
        move.w  d0,d3               ; key index
        bsr     softkbd_keyent      ; (d0 = index) -> a0
        cmp.b   #$FE,4(a0)          ; Shift: toggle the latch
        bne     .normal
        not.b   v_kb_shift(a4)
        move.w  d3,d0
        bsr     softkbd_drawkey
        bra     .out
.normal:
        moveq   #0,d1
        move.b  3(a0),d1            ; ascii
        moveq   #0,d2
        move.b  4(a0),d2            ; raw
        ; sticky shift: uppercase a letter, then release the latch
        move.b  v_kb_shift(a4),d0
        beq     .post
        cmp.b   #'a',d1
        blt     .unlatch
        cmp.b   #'z',d1
        bgt     .unlatch
        sub.b   #32,d1
.unlatch:
        sf      v_kb_shift(a4)
        move.w  d3,-(sp)
        move.w  #KB_SHIFTIX,d0
        bsr     softkbd_drawkey
        move.w  (sp)+,d3
.post:  lsl.w   #8,d2
        or.w    d2,d1
        moveq   #EV_KEY,d0
        bsr     ev_post
.out:   movem.l (sp)+,d0-d3/a0/a4
        rts

; softkbd_hover - track the cursor, repaint the (un)hovered keys
softkbd_hover:
        movem.l d0-d2/a4,-(sp)
        lea     VARS,a4
        move.b  v_kb_vis(a4),d0
        beq     .out
        move.w  v_mouse_x(a4),d0
        lsr.w   #3,d0
        move.w  v_mouse_y(a4),d1
        lsr.w   #3,d1
        bsr     softkbd_key_at      ; d0 = index or -1
        cmp.w   v_kb_hover(a4),d0
        beq     .out
        move.w  v_kb_hover(a4),d2
        move.w  d0,v_kb_hover(a4)
        tst.w   d2
        bmi     .new
        move.w  d2,d0
        bsr     softkbd_drawkey     ; un-highlight the old key
.new:   move.w  v_kb_hover(a4),d0
        bmi     .out
        bsr     softkbd_drawkey
.out:   movem.l (sp)+,d0-d2/a4
        rts

; kbtest_click - d0 = key index: click its center through the real path
; (AUTOTEST_KBD)
kbtest_click:
        movem.l d0-d1/a0,-(sp)
        bsr     softkbd_keyent
        moveq   #0,d1
        move.b  2(a0),d1            ; w
        lsr.w   #1,d1
        moveq   #0,d0
        move.b  (a0),d0             ; x
        add.w   d1,d0
        moveq   #0,d1
        move.b  1(a0),d1
        add.w   #KB_ROW0,d1
        bsr     softkbd_click
        movem.l (sp)+,d0-d1/a0
        rts

; ----------------------------------------------------------------------------
; key table: x, row, w, ascii, raw, pad, label[4]
; indices:  row0 0-12, row1 13-25, row2 26-38, row3 39-51, row4 52-54
; ----------------------------------------------------------------------------
kb_keys:
        ; row 0: digits + BS                       (0-12)
        dc.b     0,0,3,'1',0,0, "1",0,0,0
        dc.b     3,0,3,'2',0,0, "2",0,0,0
        dc.b     6,0,3,'3',0,0, "3",0,0,0
        dc.b     9,0,3,'4',0,0, "4",0,0,0
        dc.b    12,0,3,'5',0,0, "5",0,0,0
        dc.b    15,0,3,'6',0,0, "6",0,0,0
        dc.b    18,0,3,'7',0,0, "7",0,0,0
        dc.b    21,0,3,'8',0,0, "8",0,0,0
        dc.b    24,0,3,'9',0,0, "9",0,0,0
        dc.b    27,0,3,'0',0,0, "0",0,0,0
        dc.b    30,0,3,'-',0,0, "-",0,0,0
        dc.b    33,0,3,'=',0,0, "=",0,0,0
        dc.b    36,0,4,8,0,0,   "BS",0,0
        ; row 1: qwertyuiop[] + RET                (13-25)
        dc.b     0,1,3,'q',0,0, "q",0,0,0
        dc.b     3,1,3,'w',0,0, "w",0,0,0
        dc.b     6,1,3,'e',0,0, "e",0,0,0
        dc.b     9,1,3,'r',0,0, "r",0,0,0
        dc.b    12,1,3,'t',0,0, "t",0,0,0
        dc.b    15,1,3,'y',0,0, "y",0,0,0
        dc.b    18,1,3,'u',0,0, "u",0,0,0
        dc.b    21,1,3,'i',0,0, "i",0,0,0
        dc.b    24,1,3,'o',0,0, "o",0,0,0
        dc.b    27,1,3,'p',0,0, "p",0,0,0
        dc.b    30,1,3,'[',0,0, "[",0,0,0
        dc.b    33,1,3,']',0,0, "]",0,0,0
        dc.b    36,1,4,13,0,0,  "RET",0
        ; row 2: asdfghjkl;' + F1 + Up             (26-38)
        dc.b     0,2,3,'a',0,0, "a",0,0,0
        dc.b     3,2,3,'s',0,0, "s",0,0,0
        dc.b     6,2,3,'d',0,0, "d",0,0,0
        dc.b     9,2,3,'f',0,0, "f",0,0,0
        dc.b    12,2,3,'g',0,0, "g",0,0,0
        dc.b    15,2,3,'h',0,0, "h",0,0,0
        dc.b    18,2,3,'j',0,0, "j",0,0,0
        dc.b    21,2,3,'k',0,0, "k",0,0,0
        dc.b    24,2,3,'l',0,0, "l",0,0,0
        dc.b    27,2,3,';',0,0, ";",0,0,0
        dc.b    30,2,3,39,0,0,  39,0,0,0
        dc.b    33,2,3,0,$50,0, "F1",0,0
        dc.b    37,2,3,0,$4C,0, "^",0,0,0
        ; row 3: Shift + zxcvbnm,./ + Left + Down  (39-51)
        dc.b     0,3,4,0,$FE,0, "Sh",0,0
        dc.b     4,3,3,'z',0,0, "z",0,0,0
        dc.b     7,3,3,'x',0,0, "x",0,0,0
        dc.b    10,3,3,'c',0,0, "c",0,0,0
        dc.b    13,3,3,'v',0,0, "v",0,0,0
        dc.b    16,3,3,'b',0,0, "b",0,0,0
        dc.b    19,3,3,'n',0,0, "n",0,0,0
        dc.b    22,3,3,'m',0,0, "m",0,0,0
        dc.b    25,3,3,',',0,0, ",",0,0,0
        dc.b    28,3,3,'.',0,0, ".",0,0,0
        dc.b    31,3,3,'/',0,0, "/",0,0,0
        dc.b    34,3,3,0,$4F,0, "<",0,0,0
        dc.b    37,3,3,0,$4D,0, "v",0,0,0
        ; row 4: Esc + Space + Right               (52-54)
        dc.b     0,4,4,27,0,0,  "ESC",0
        dc.b     5,4,26,32,0,0, "SPC",0
        dc.b    37,4,3,0,$4E,0, ">",0,0,0
        even
