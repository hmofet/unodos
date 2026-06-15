; ============================================================================
; UnoDOS/MacPlus Files + Notepad - DISK-LOADED app (FILES.APP, slot 0).
; Files (proc 2) and Notepad (proc 3) ship as ONE binary: Files opens the
; selected file INTO Notepad (files_key -> notepad_open_fat + launch_app #3),
; so they are coupled and share this slot. The kernel dispatches both procs
; here; the draw/key entry stubs branch on the proc index (d3).
;
; State: the edit buffer npbuf is a KBSS equate; np_*/files_sel/fat_count are
; kernel vars; the FAT12 routines + files_mount + fmt_dec/str_append are kernel
; entry points. ABI: JMP draw/key/tick/click (draw/key branch on d3).
; ============================================================================

        mc68000
        include "sysequ.i"
        include "build/kernel_api.inc"

        org     APP_LOAD+APPSLOT_FILES*APP_SLOT_SZ
        dc.l     fn_draw             ; +0  d3 = proc (2 Files / 3 Notepad)
        dc.l     fn_key              ; +4  d3 = proc
        dc.l     fn_tickv            ; +8
        dc.l     fn_clickv           ; +12
fn_tickv:
        rts
fn_clickv:
        rts

; fn_draw / fn_key - route to Files (proc 2) or Notepad (proc 3) by d3.
fn_draw:
        cmp.w   #2,d3
        bne     .np
        bra     files_draw
.np:    bra     notepad_draw
fn_key:
        cmp.w   #2,d3
        bne     .np
        bra     files_key
.np:    bra     notepad_key

; ============================================================================
; Files app (proc 2)
; ============================================================================
files_draw:
        movem.l d0-d7/a0-a3,-(sp)
        move.b  fat_mounted(pc),d0
        bne     .mounted
        bsr     files_mount
.mounted:
        move.w  WX(a2),d6
        addq.w  #6,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+4,d5
        lea     str_f_hdr(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        move.w  fat_count(pc),d0
        bne     .rows
        lea     str_f_none(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   #14,d1
        moveq   #3,d2
        bsr     draw_string
        bra     .foot
.rows:
        moveq   #0,d7
.row:   cmp.w   fat_count(pc),d7
        bge     .foot
        move.w  d7,d0
        mulu    #18,d0
        lea     fat_tab,a3
        lea     (a3,d0.w),a3
        move.w  d5,d1
        add.w   #12,d1
        move.w  d7,d0
        mulu    #11,d0
        add.w   d0,d1
        cmp.w   files_sel(pc),d7
        bne     .name
        movem.l d1/a3,-(sp)
        move.w  WX(a2),d0
        addq.w  #2,d0
        subq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #4,d2
        moveq   #10,d3
        moveq   #3,d4
        bsr     fill_rect
        movem.l (sp)+,d1/a3
.name:  movem.l d1-d2/a1,-(sp)
        lea     numbuf,a0
        moveq   #10,d2
        move.l  a3,a1
.nmcp:  move.b  (a1)+,(a0)+
        dbra    d2,.nmcp
        clr.b   (a0)
        movem.l (sp)+,d1-d2/a1
        lea     numbuf,a0
        move.w  d6,d0
        movem.l d1/a3,-(sp)
        cmp.w   files_sel(pc),d7
        bne     .fgn
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        bra     .dNd
.fgn:   moveq   #3,d2
        bsr     draw_string
.dNd:   movem.l (sp)+,d1/a3
        move.l  12(a3),d0
        cmp.l   #65535,d0
        ble     .szok
        move.l  #65535,d0
.szok:  lea     numbuf,a0
        bsr     fmt_dec
        lea     numbuf,a0
        move.w  d6,d0
        add.w   #112,d0
        movem.l d1/a3,-(sp)
        cmp.w   files_sel(pc),d7
        bne     .fgn2
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        bra     .dSd
.fgn2:  moveq   #3,d2
        bsr     draw_string
.dSd:   movem.l (sp)+,d1/a3
        addq.w  #1,d7
        bra     .row
.foot:
        lea     str_f_foot(pc),a0
        move.w  d6,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #12,d1
        moveq   #3,d2
        bsr     draw_string
        movem.l (sp)+,d0-d7/a0-a3
        rts

files_key:
        cmp.b   #$4D,d2             ; down
        beq     .down
        cmp.b   #$4C,d2             ; up
        beq     .up
        cmp.b   #13,d1              ; Enter: open
        beq     .open
        cmp.b   #'r',d1
        beq     .refresh
        moveq   #1,d0
        rts
.refresh:
        lea     vars(pc),a4
        sf      fat_mounted-vars(a4)
        clr.w   files_sel-vars(a4)
        bsr     files_mount
        bra     .redraw
.down:  move.w  files_sel(pc),d0
        addq.w  #1,d0
        cmp.w   fat_count(pc),d0
        bge     .redraw
        lea     vars(pc),a4
        move.w  d0,files_sel-vars(a4)
        bra     .redraw
.up:    move.w  files_sel(pc),d0
        subq.w  #1,d0
        blt     .redraw
        lea     vars(pc),a4
        move.w  d0,files_sel-vars(a4)
        bra     .redraw
.open:
        move.w  fat_count(pc),d0
        beq     .redraw
        bsr     notepad_open_fat
        moveq   #3,d0
        bsr     launch_app          ; opens/raises Notepad (proc 3)
        moveq   #0,d0               ; consumed
        rts
.redraw:
        moveq   #0,d0               ; consumed; kernel redraws topmost
        rts

; ============================================================================
; Notepad app (proc 3)
; ============================================================================
notepad_open_fat:
        movem.l d0-d2/a0-a1/a3-a4,-(sp)
        move.w  files_sel(pc),d0
        mulu    #18,d0
        lea     fat_tab,a3
        lea     (a3,d0.w),a3
        lea     vars(pc),a4
        moveq   #0,d0
        move.w  16(a3),d0
        move.l  12(a3),d1
        cmp.l   #NBUF-1,d1
        ble     .szok
        move.l  #NBUF-1,d1
.szok:  lea     npbuf,a1
        bsr     fat_read_file
        tst.l   d0
        bpl     .ok
        moveq   #0,d0
.ok:    move.w  d0,np_len-vars(a4)
        clr.w   np_caret-vars(a4)
        clr.w   np_top-vars(a4)
        move.w  #-1,np_goal-vars(a4)
        move.w  files_sel(pc),d0
        move.w  d0,np_fatidx-vars(a4)
        sf      np_dirty-vars(a4)
        movem.l (sp)+,d0-d2/a0-a1/a3-a4
        rts

notepad_linecol:
        movem.l d2-d3/a0,-(sp)
        lea     npbuf,a0
        moveq   #0,d0
        moveq   #0,d1
        moveq   #0,d2
.scan:  cmp.w   np_caret(pc),d2
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

notepad_seek_linecol:
        movem.l d3-d5/a0,-(sp)
        lea     npbuf,a0
        moveq   #0,d3
        moveq   #0,d4
.fs:    cmp.w   d0,d4
        beq     .at
        cmp.w   np_len(pc),d3
        bge     .nofind
        cmp.b   #13,(a0,d3.w)
        bne     .fnc
        addq.w  #1,d4
.fnc:   addq.w  #1,d3
        bra     .fs
.at:    moveq   #0,d5
.adv:   cmp.w   d2,d5
        bge     .found
        cmp.w   np_len(pc),d3
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

notepad_draw:
        movem.l d0-d7/a0-a4,-(sp)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        sub.w   #TBAR_H+11,d3
        moveq   #0,d4
        bsr     fill_rect
        move.w  WX(a2),d6
        addq.w  #4,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+2,d5
        move.w  WW(a2),d7
        subq.w  #8,d7
        lsr.w   #3,d7
        cmp.w   #38,d7
        ble     .wok
        moveq   #38,d7
.wok:
        move.w  WH(a2),d4
        sub.w   #TBAR_H+12,d4
        divu    #10,d4
        tst.w   d4
        beq     .noscr
        bsr     notepad_linecol
        move.w  np_top(pc),d1
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
.nbot:  lea     vars(pc),a4
        move.w  d1,np_top-vars(a4)
.noscr:
        lea     npbuf,a0
        move.w  np_top(pc),d3
        move.w  d3,d2
        moveq   #0,d0
.sktop: tst.w   d2
        beq     .skdone
        cmp.w   np_len(pc),d0
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
        move.l  a0,a1
        moveq   #0,d2
.find:  move.l  a1,d0
        lea     npbuf,a3
        sub.l   a3,d0
        add.w   d2,d0
        cmp.w   np_len(pc),d0
        bge     .eol
        move.b  (a1,d2.w),d1
        cmp.b   #13,d1
        beq     .eol
        addq.w  #1,d2
        bra     .find
.eol:
        move.w  d2,d0
        cmp.w   d7,d0
        ble     .lenok
        move.w  d7,d0
.lenok: lea     npline,a3
        move.w  d0,d1
        beq     .zt
        subq.w  #1,d1
        move.l  a1,a4
.cpl:   move.b  (a4)+,(a3)+
        dbra    d1,.cpl
.zt:    clr.b   (a3)
        movem.l d2-d4/a0-a1,-(sp)
        lea     npline,a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        movem.l (sp)+,d2-d4/a0-a1
        movem.l d2-d4/a0-a1,-(sp)
        bsr     notepad_linecol
        cmp.w   d3,d0
        bne     .nocaret
        cmp.w   d7,d1
        ble     .colok
        move.w  d7,d1
.colok: lsl.w   #3,d1
        add.w   d6,d1
        move.w  d1,d0
        move.w  d5,d1
        moveq   #2,d2
        moveq   #8,d3
        moveq   #3,d4
        bsr     fill_rect
.nocaret:
        movem.l (sp)+,d2-d4/a0-a1
        lea     1(a1,d2.w),a0
        move.l  a0,d0
        lea     npbuf,a3
        sub.l   a3,d0
        cmp.w   np_len(pc),d0
        bgt     .status
        add.w   #10,d5
        addq.w  #1,d3
        subq.w  #1,d4
        bra     .line
.status:
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #11,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        moveq   #10,d3
        moveq   #3,d4
        bsr     fill_rect
        lea     npstat,a0
        lea     str_n_ln(pc),a1
        bsr     str_append
        bsr     notepad_linecol
        move.w  d1,-(sp)
        addq.w  #1,d0
        bsr     fmt_dec
.skip1: tst.b   (a0)+
        bne     .skip1
        subq.l  #1,a0
        lea     str_n_co(pc),a1
        bsr     str_append
        move.w  (sp)+,d1
        move.w  d1,d0
        addq.w  #1,d0
        bsr     fmt_dec
.skip2: tst.b   (a0)+
        bne     .skip2
        subq.l  #1,a0
        move.b  #' ',(a0)+
        move.w  np_len(pc),d0
        bsr     fmt_dec
.skip3: tst.b   (a0)+
        bne     .skip3
        subq.l  #1,a0
        lea     str_n_b(pc),a1
        bsr     str_append
        move.b  np_dirty(pc),d0
        beq     .nodirty
        lea     str_n_dirty(pc),a1
        bsr     str_append
.nodirty:
        lea     str_n_save(pc),a1
        bsr     str_append
        lea     npstat,a0
        move.w  WX(a2),d0
        addq.w  #4,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #10,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        movem.l (sp)+,d0-d7/a0-a4
        rts

notepad_key:
        lea     vars(pc),a4
        cmp.b   #$4F,d2
        beq     .left
        cmp.b   #$4E,d2
        beq     .right
        cmp.b   #$4C,d2
        beq     .up
        cmp.b   #$4D,d2
        beq     .down
        cmp.b   #$50,d2             ; F1 (Clr) = save
        beq     .save
        cmp.b   #8,d1
        beq     .bs
        cmp.b   #13,d1
        beq     .ins
        cmp.b   #32,d1
        bge     .ins
        moveq   #1,d0
        rts
.left:  move.w  #-1,np_goal-vars(a4)
        move.w  np_caret(pc),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,np_caret-vars(a4)
        bra     .redraw
.right: move.w  #-1,np_goal-vars(a4)
        move.w  np_caret(pc),d0
        cmp.w   np_len(pc),d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,np_caret-vars(a4)
        bra     .redraw
.up:    bsr     notepad_linecol
        tst.w   d0
        beq     .redraw
        move.w  np_goal(pc),d2
        bpl     .upg
        move.w  d1,d2
        move.w  d2,np_goal-vars(a4)
.upg:   subq.w  #1,d0
        bsr     notepad_seek_linecol
        tst.w   d0
        bmi     .redraw
        move.w  d0,np_caret-vars(a4)
        bra     .redraw
.down:  bsr     notepad_linecol
        move.w  np_goal(pc),d2
        bpl     .dng
        move.w  d1,d2
        move.w  d2,np_goal-vars(a4)
.dng:   addq.w  #1,d0
        bsr     notepad_seek_linecol
        tst.w   d0
        bmi     .redraw
        move.w  d0,np_caret-vars(a4)
        bra     .redraw
.bs:    move.w  #-1,np_goal-vars(a4)
        move.w  np_caret(pc),d0
        beq     .redraw
        lea     npbuf,a0
        move.w  np_caret(pc),d1
        move.w  np_len(pc),d2
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
        move.w  d0,np_caret-vars(a4)
        move.w  np_len(pc),d0
        subq.w  #1,d0
        move.w  d0,np_len-vars(a4)
        st      np_dirty-vars(a4)
        bra     .redraw
.ins:   move.w  #-1,np_goal-vars(a4)
        move.w  np_len(pc),d0
        cmp.w   #NBUF-1,d0
        bge     .redraw
        lea     npbuf,a0
        move.w  np_len(pc),d2
        sub.w   np_caret(pc),d2
        lea     (a0,d0.w),a1
.insloop:
        tst.w   d2
        beq     .insdone
        move.b  -(a1),d3
        move.b  d3,1(a1)
        subq.w  #1,d2
        bra     .insloop
.insdone:
        move.w  np_caret(pc),d0
        lea     npbuf,a0
        move.b  d1,(a0,d0.w)
        addq.w  #1,d0
        move.w  d0,np_caret-vars(a4)
        move.w  np_len(pc),d0
        addq.w  #1,d0
        move.w  d0,np_len-vars(a4)
        st      np_dirty-vars(a4)
        bra     .redraw
.save:
        move.b  fat_mounted(pc),d0
        beq     .redraw
        move.w  np_fatidx(pc),d0
        bmi     .saveunt
        mulu    #18,d0
        lea     fat_tab,a0
        lea     (a0,d0.w),a0
        bra     .dofat
.saveunt:
        lea     str_untitled(pc),a0
.dofat: lea     npbuf,a1
        moveq   #0,d1
        move.w  np_len(pc),d1
        bsr     fat_save_file
        tst.w   d0
        bmi     .redraw
        sf      np_dirty-vars(a4)
        bsr     fat_list_root
        bra     .redraw
.redraw:
        moveq   #0,d0               ; consumed; kernel redraws topmost
        rts

; ---------------------------------------------------------------- data
str_f_hdr:      dc.b    "Name          Size",0
str_f_foot:     dc.b    "Enter:open  r:refresh",0
str_f_none:     dc.b    "(no files on disk)",0
str_n_save:     dc.b    " Clr save",0
str_n_ln:       dc.b    "Ln ",0
str_n_co:       dc.b    " Co ",0
str_n_b:        dc.b    " B",0
str_n_dirty:    dc.b    " *",0
str_untitled:   dc.b    "UNTITLEDTXT",0
        even
        end
