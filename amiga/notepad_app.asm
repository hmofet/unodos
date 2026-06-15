; ============================================================================
; UnoDOS/68K Notepad - DISK-LOADED app (NOTEPAD.APP, proc 3). Was apps_m2.i's
; notepad_draw/notepad_key (+ linecol/seek helpers) built into the kernel; now
; a separate -Fbin binary loaded off DF1 into its window slot. Links to the
; kernel only via APIVEC (KCALL/KDATA).
;
; The edit buffer (npbuf), the wrapped line scratch (npline) and all np_* state
; live in the kernel 'vars' block (reached via KDATA vars + VO_*), so the
; kernel-resident notepad_open_file / notepad_open_fat / notepad_set_demo can
; seed them and the Files app can open files into Notepad unchanged. npstat is
; the shared status-line scratch (exported data). fat_save_file/_list_root and
; rd_entry are exported kernel routines used by the F1-save path.
; ============================================================================

        mc68000
        include "sysabi.i"
        include "build/kernel_api.inc"

NBUF        equ 2048                ; notepad buffer (matches kernel.asm)

        org     APPSLOT0
; ---- JMP table (open/draw/key/tick/click) ----
        jmp     np_open(pc)
        jmp     np_draw(pc)
        jmp     np_key(pc)
        jmp     np_tick(pc)
        jmp     np_click(pc)

np_open:
        rts
np_tick:
        rts
np_click:
        rts

; np_linecol - a4 = &vars -> d0 = line, d1 = col of caret (0-based)
np_linecol:
        movem.l d2-d3/a0,-(sp)
        lea     VO_npbuf(a4),a0
        moveq   #0,d0
        moveq   #0,d1
        moveq   #0,d2               ; index
.scan:  cmp.w   VO_np_caret(a4),d2
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
.done:  movem.l (sp)+,d2-d3/a0
        rts

; np_seek_linecol - d0 = target line, d2 = goal col; a4 = &vars
;   -> d0 = caret index (clamped to line end), or -1 if no such line
np_seek_linecol:
        movem.l d3-d5/a0,-(sp)
        lea     VO_npbuf(a4),a0
        moveq   #0,d3               ; index
        moveq   #0,d4               ; line
.fs:    cmp.w   d0,d4
        beq     .at
        cmp.w   VO_np_len(a4),d3
        bge     .nofind
        cmp.b   #13,(a0,d3.w)
        bne     .fnc
        addq.w  #1,d4
.fnc:   addq.w  #1,d3
        bra     .fs
.at:    moveq   #0,d5               ; col
.adv:   cmp.w   d2,d5
        bge     .found
        cmp.w   VO_np_len(a4),d3
        bge     .found
        cmp.b   #13,(a0,d3.w)
        beq     .found
        addq.w  #1,d3
        addq.w  #1,d5
        bra     .adv
.found: move.w  d3,d0
        movem.l (sp)+,d3-d5/a0
        rts
.nofind:
        moveq   #-1,d0
        movem.l (sp)+,d3-d5/a0
        rts

; np_draw - a2 = window
np_draw:
        movem.l d0-d7/a0-a4,-(sp)
        KDATA   a4,vars
        ; clear content area (above the status row)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        sub.w   #TBAR_H+11,d3
        moveq   #0,d4
        KCALL   fill_rect
        ; layout
        move.w  WX(a2),d6
        addq.w  #4,d6               ; text x
        move.w  WY(a2),d5
        add.w   #TBAR_H+2,d5        ; first line y
        move.w  WW(a2),d7
        subq.w  #8,d7
        lsr.w   #3,d7               ; d7 = max chars per line
        cmp.w   #38,d7
        ble     .wok
        moveq   #38,d7
.wok:
        ; rows that fit
        move.w  WH(a2),d4
        sub.w   #TBAR_H+12,d4
        divu    #10,d4
        ; vertical scroll: clamp np_top so the caret line stays visible
        tst.w   d4
        beq     .noscr
        bsr     np_linecol          ; d0 = caret line
        move.w  VO_np_top(a4),d1
        cmp.w   d1,d0
        bge     .ntop
        move.w  d0,d1               ; caret above view: scroll up to it
.ntop:  move.w  d1,d2
        add.w   d4,d2               ; top + rows
        cmp.w   d2,d0
        blt     .nbot
        move.w  d0,d1               ; caret below view: caret-rows+1
        sub.w   d4,d1
        addq.w  #1,d1
.nbot:  move.w  d1,VO_np_top(a4)
.noscr:
        ; line loop: a0 = scan ptr, d3 = line index
        lea     VO_npbuf(a4),a0
        move.w  VO_np_top(a4),d3    ; first visible line
        move.w  d3,d2               ; lines to skip
        moveq   #0,d0
.sktop: tst.w   d2
        beq     .skdone
        cmp.w   VO_np_len(a4),d0
        bge     .skdone
        cmp.b   #13,(a0,d0.w)
        bne     .sknc
        subq.w  #1,d2
.sknc:  addq.w  #1,d0
        bra     .sktop
.skdone:
        lea     (a0,d0.w),a0
.line:  tst.w   d4
        beq     .status
        ; find line end
        move.l  a0,a1               ; a1 = line start
        moveq   #0,d2               ; len
.find:  move.l  a1,d0
        lea     VO_npbuf(a4),a3
        sub.l   a3,d0
        add.w   d2,d0
        cmp.w   VO_np_len(a4),d0
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
.lenok: lea     VO_npline(a4),a3
        move.w  d0,d1
        beq     .zt
        subq.w  #1,d1
        move.l  a1,a4               ; (a4 temporarily reused - restored below)
.cpl:   move.b  (a4)+,(a3)+
        dbra    d1,.cpl
.zt:    clr.b   (a3)
        KDATA   a4,vars             ; restore a4 = &vars
        ; draw the line
        movem.l d2-d4/a0-a1,-(sp)
        lea     VO_npline(a4),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        KCALL   draw_string
        movem.l (sp)+,d2-d4/a0-a1
        ; caret on this line?
        movem.l d2-d4/a0-a1,-(sp)
        bsr     np_linecol          ; d0=line d1=col
        cmp.w   d3,d0
        bne     .nocaret
        cmp.w   d7,d1
        ble     .colok
        move.w  d7,d1
.colok: lsl.w   #3,d1
        add.w   d6,d1
        move.w  d1,d0               ; caret x
        move.w  d5,d1               ; caret y
        moveq   #2,d2
        moveq   #8,d3
        moveq   #1,d4               ; cyan
        KCALL   fill_rect
.nocaret:
        movem.l (sp)+,d2-d4/a0-a1
        ; advance to next line
        lea     1(a1,d2.w),a0       ; skip CR
        move.l  a0,d0
        lea     VO_npbuf(a4),a3
        sub.l   a3,d0
        cmp.w   VO_np_len(a4),d0
        bgt     .status
        add.w   #10,d5
        addq.w  #1,d3
        subq.w  #1,d4
        bra     .line
.status:
        ; status row: white bar + "Ln x Co y  NNN B [*] F1 save"
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #11,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        moveq   #10,d3
        moveq   #3,d4
        KCALL   fill_rect
        ; build the string
        KDATA   a0,npstat
        lea     str_n_ln(pc),a1
        KCALL   str_append
        bsr     np_linecol          ; d0=line d1=col
        move.w  d1,-(sp)
        addq.w  #1,d0
        KCALL   fmt_dec
.skip1: tst.b   (a0)+
        bne     .skip1
        subq.l  #1,a0
        lea     str_n_co(pc),a1
        KCALL   str_append
        move.w  (sp)+,d1
        move.w  d1,d0
        addq.w  #1,d0
        KCALL   fmt_dec
.skip2: tst.b   (a0)+
        bne     .skip2
        subq.l  #1,a0
        move.b  #' ',(a0)+
        move.w  VO_np_len(a4),d0
        KCALL   fmt_dec
.skip3: tst.b   (a0)+
        bne     .skip3
        subq.l  #1,a0
        lea     str_n_b(pc),a1
        KCALL   str_append
        move.b  VO_np_dirty(a4),d0
        beq     .nodirty
        lea     str_n_dirty(pc),a1
        KCALL   str_append
.nodirty:
        lea     str_n_save(pc),a1
        KCALL   str_append
        ; draw it (blue on white, glyph TOP at bar top + 1)
        KDATA   a0,npstat
        move.w  WX(a2),d0
        addq.w  #4,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #10,d1
        moveq   #0,d2
        moveq   #3,d3
        KCALL   draw_string_bg
        movem.l (sp)+,d0-d7/a0-a4
        rts

; np_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
np_key:
        KDATA   a4,vars
        cmp.b   #$4F,d2             ; left
        beq     .left
        cmp.b   #$4E,d2             ; right
        beq     .right
        cmp.b   #$4C,d2             ; up
        beq     .up
        cmp.b   #$4D,d2             ; down
        beq     .down
        cmp.b   #$50,d2             ; F1 = save
        beq     .save
        cmp.b   #8,d1               ; backspace
        beq     .bs
        cmp.b   #13,d1              ; return inserts CR
        beq     .ins
        cmp.b   #32,d1
        bge     .ins
        moveq   #1,d0               ; not consumed (incl. ESC)
        rts
.left:  move.w  #-1,VO_np_goal(a4)
        move.w  VO_np_caret(a4),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,VO_np_caret(a4)
        bra     .redraw
.right: move.w  #-1,VO_np_goal(a4)
        move.w  VO_np_caret(a4),d0
        cmp.w   VO_np_len(a4),d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,VO_np_caret(a4)
        bra     .redraw
.up:    bsr     np_linecol          ; d0=line d1=col
        tst.w   d0
        beq     .redraw             ; already on first line
        move.w  VO_np_goal(a4),d2
        bpl     .upg                ; keep existing goal column
        move.w  d1,d2
        move.w  d2,VO_np_goal(a4)
.upg:   subq.w  #1,d0
        bsr     np_seek_linecol
        tst.w   d0
        bmi     .redraw
        move.w  d0,VO_np_caret(a4)
        bra     .redraw
.down:  bsr     np_linecol          ; d0=line d1=col
        move.w  VO_np_goal(a4),d2
        bpl     .dng
        move.w  d1,d2
        move.w  d2,VO_np_goal(a4)
.dng:   addq.w  #1,d0
        bsr     np_seek_linecol
        tst.w   d0
        bmi     .redraw             ; no next line
        move.w  d0,VO_np_caret(a4)
        bra     .redraw
.bs:    move.w  #-1,VO_np_goal(a4)
        move.w  VO_np_caret(a4),d0
        beq     .redraw
        ; shift [caret..len) left by one
        lea     VO_npbuf(a4),a0
        move.w  VO_np_caret(a4),d1
        move.w  VO_np_len(a4),d2
        sub.w   d1,d2               ; bytes after caret
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
        move.w  d0,VO_np_caret(a4)
        move.w  VO_np_len(a4),d0
        subq.w  #1,d0
        move.w  d0,VO_np_len(a4)
        st      VO_np_dirty(a4)
        bra     .redraw
.ins:   move.w  #-1,VO_np_goal(a4)
        move.w  VO_np_len(a4),d0
        cmp.w   #NBUF-1,d0
        bge     .redraw
        ; shift [caret..len) right by one (backwards copy)
        lea     VO_npbuf(a4),a0
        move.w  VO_np_len(a4),d2
        sub.w   VO_np_caret(a4),d2  ; count
        lea     (a0,d0.w),a1        ; a1 = npbuf+len (one past last)
.insloop:
        tst.w   d2
        beq     .insdone
        move.b  -(a1),d3
        move.b  d3,1(a1)
        subq.w  #1,d2
        bra     .insloop
.insdone:
        move.w  VO_np_caret(a4),d0
        lea     VO_npbuf(a4),a0
        move.b  d1,(a0,d0.w)        ; ascii (13 for return)
        addq.w  #1,d0
        move.w  d0,VO_np_caret(a4)
        move.w  VO_np_len(a4),d0
        addq.w  #1,d0
        move.w  d0,VO_np_len(a4)
        st      VO_np_dirty(a4)
        bra     .redraw
.save:
        move.w  VO_np_file(a4),d0
        cmp.w   #-2,d0
        beq     .savefat            ; FAT origin: write back to DF1
        cmp.w   #-1,d0
        beq     .saveunt            ; untitled: create UNTITLED.TXT
        tst.w   d0
        bmi     .redraw
        KCALL   rd_entry            ; a3 = entry
        move.w  VO_np_len(a4),d1
        cmp.w   18(a3),d1           ; capacity
        bgt     .redraw             ; too big: refuse (status keeps *)
        move.w  d1,16(a3)           ; new size
        move.l  12(a3),a1           ; dest
        lea     VO_npbuf(a4),a0
        move.w  d1,d2
        beq     .savedone
        subq.w  #1,d2
.svloop:
        move.b  (a0)+,(a1)+
        dbra    d2,.svloop
.savedone:
        sf      VO_np_dirty(a4)
        bra     .redraw
.savefat:
        move.w  VO_np_fatidx(a4),d0
        mulu    #18,d0
        KDATA   a0,fat_tab
        lea     (a0,d0.w),a0        ; 11-char name
        bra     .dofat
.saveunt:
        move.b  VO_fat_mounted(a4),d0
        beq     .redraw             ; no data disk: stay RAM-only
        lea     str_untitled(pc),a0
.dofat: lea     VO_npbuf(a4),a1
        moveq   #0,d1
        move.w  VO_np_len(a4),d1
        KCALL   fat_save_file
        tst.w   d0
        bmi     .redraw             ; failed (write-protect / full)
        KDATA   a4,vars
        sf      VO_np_dirty(a4)
        KCALL   fat_list_root       ; refresh the listing
        bra     .redraw
.redraw:
        KCALL   redraw_topmost
        moveq   #0,d0
        rts

; ---- app-private strings (moved out of the kernel) ----
str_n_save:     dc.b    " F1 save",0
str_n_ln:       dc.b    "Ln ",0
str_n_co:       dc.b    " Co ",0
str_n_b:        dc.b    " B",0
str_n_dirty:    dc.b    " *",0
str_untitled:   dc.b    "UNTITLEDTXT",0
        even
        end
