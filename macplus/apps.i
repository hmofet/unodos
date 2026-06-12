; ============================================================================
; UnoDOS/MacPlus milestone 2 apps: Files (proc 2) and Notepad (proc 3).
; Ported from the Amiga port's apps_m2.i, tailored for the Mac:
;   - one source only: the FAT12 volume on the boot floppy (no ROM-disk),
;     lazily mounted the first time Files is drawn;
;   - 1-bit colors for the white content background (0=white .. 3=black);
;   - Music is an M3 item, omitted here.
; Conventions match the kernel: PC-relative reads, writes via lea vars(pc),a4
; + offset; key handlers take d1=ascii d2=raw, return d0=0 consumed / 1 not.
; ============================================================================

; fmt_dec - d0.w unsigned -> decimal digits + NUL at a0 (no suffix)
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

; str_append - append NUL string a1 to cursor a0 (a0 advances to new NUL)
str_append:
.cp:    move.b  (a1)+,(a0)+
        bne     .cp
        subq.l  #1,a0
        rts

; redraw_topmost - repaint just the topmost window (frame + content)
redraw_topmost:
        movem.l d2/a2,-(sp)
        move.w  zcount(pc),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        bsr     draw_window
.out:   movem.l (sp)+,d2/a2
        rts

; ============================================================================
; Files app (proc 2) - lists the FAT12 root directory off the boot floppy
; ============================================================================

; files_mount - mount the volume and cache the root listing (idempotent)
files_mount:
        movem.l d0-d7/a0-a3,-(sp)
        lea     vars(pc),a4
        bsr     fat_mount
        tst.w   d0
        bmi     .fail
        bsr     fat_list_root
        st      fat_mounted-vars(a4)
        bra     .out
.fail:  sf      fat_mounted-vars(a4)
        clr.w   fat_count-vars(a4)
.out:   movem.l (sp)+,d0-d7/a0-a3
        rts

; files_draw - a2 = window
files_draw:
        movem.l d0-d7/a0-a3,-(sp)
        ; lazy mount the first time we paint the window
        move.b  fat_mounted(pc),d0
        bne     .mounted
        bsr     files_mount
.mounted:
        move.w  WX(a2),d6
        addq.w  #6,d6               ; content x
        move.w  WY(a2),d5
        add.w   #TBAR_H+4,d5        ; content y
        ; header
        lea     str_f_hdr(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2              ; black
        bsr     draw_string
        ; no files? say so
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
        moveq   #0,d7               ; index
.row:   cmp.w   fat_count(pc),d7
        bge     .foot
        move.w  d7,d0
        mulu    #18,d0
        lea     fat_tab(pc),a3
        lea     (a3,d0.w),a3        ; a3 = FAT entry
        ; row y
        move.w  d5,d1
        add.w   #12,d1
        move.w  d7,d0
        mulu    #11,d0
        add.w   d0,d1               ; d1 = row y
        ; selection bar (black) when selected
        cmp.w   files_sel(pc),d7
        bne     .name
        movem.l d1/a3,-(sp)
        move.w  WX(a2),d0
        addq.w  #2,d0
        subq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #4,d2
        moveq   #10,d3
        moveq   #3,d4              ; black bar
        bsr     fill_rect
        movem.l (sp)+,d1/a3
.name:  ; FAT names are space-padded 8.3 - copy 11 chars + NUL into numbuf
        movem.l d1-d2/a1,-(sp)
        lea     numbuf(pc),a0
        moveq   #10,d2
        move.l  a3,a1
.nmcp:  move.b  (a1)+,(a0)+
        dbra    d2,.nmcp
        clr.b   (a0)
        movem.l (sp)+,d1-d2/a1
        lea     numbuf(pc),a0
        move.w  d6,d0
        movem.l d1/a3,-(sp)
        cmp.w   files_sel(pc),d7
        bne     .fgn
        moveq   #0,d2              ; selected: white text (RMW) on the
        moveq   #3,d3              ; black bar - OR-only white draws nothing
        bsr     draw_string_bg
        bra     .dNd
.fgn:   moveq   #3,d2              ; normal: black on white (transparent OR)
        bsr     draw_string
.dNd:   movem.l (sp)+,d1/a3
        ; size, right column
        move.l  12(a3),d0           ; FAT size (long); clamp display to 65535
        cmp.l   #65535,d0
        ble     .szok
        move.l  #65535,d0
.szok:  lea     numbuf(pc),a0
        bsr     fmt_dec
        lea     numbuf(pc),a0
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

; files_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
files_key:
        cmp.b   #$4D,d2             ; down
        beq     .down
        cmp.b   #$4C,d2             ; up
        beq     .up
        cmp.b   #13,d1              ; Enter: open
        beq     .open
        cmp.b   #'r',d1             ; refresh: re-mount + re-list
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
        bsr     launch_app          ; raises + repaints if already open
        bsr     redraw_topmost
        moveq   #0,d0
        rts
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

; ============================================================================
; Notepad app (proc 3) - text viewer/editor; F1 (Clr key) saves to disk
; ============================================================================

; notepad_open_fat - load the selected FAT12 file into the edit buffer
notepad_open_fat:
        movem.l d0-d2/a0-a1/a3-a4,-(sp)
        move.w  files_sel(pc),d0
        mulu    #18,d0
        lea     fat_tab(pc),a3
        lea     (a3,d0.w),a3
        lea     vars(pc),a4
        moveq   #0,d0
        move.w  16(a3),d0           ; first cluster
        move.l  12(a3),d1           ; size
        cmp.l   #NBUF-1,d1
        ble     .szok
        move.l  #NBUF-1,d1
.szok:  lea     npbuf(pc),a1
        bsr     fat_read_file
        tst.l   d0
        bpl     .ok
        moveq   #0,d0               ; read failed: empty buffer
.ok:    move.w  d0,np_len-vars(a4)
        clr.w   np_caret-vars(a4)
        clr.w   np_top-vars(a4)
        move.w  #-1,np_goal-vars(a4)
        move.w  files_sel(pc),d0
        move.w  d0,np_fatidx-vars(a4)
        sf      np_dirty-vars(a4)
        movem.l (sp)+,d0-d2/a0-a1/a3-a4
        rts

; notepad_set_demo - load demo_text into the buffer (AUTOTEST)
notepad_set_demo:
        movem.l d1/a0-a1/a4,-(sp)
        lea     vars(pc),a4
        lea     demo_text(pc),a0
        lea     npbuf(pc),a1
        moveq   #0,d1
.cp:    move.b  (a0)+,d0
        beq     .done
        move.b  d0,(a1)+
        addq.w  #1,d1
        bra     .cp
.done:  move.w  d1,np_len-vars(a4)
        move.w  d1,np_caret-vars(a4)
        clr.w   np_top-vars(a4)
        move.w  #-1,np_goal-vars(a4)
        move.w  #-1,np_fatidx-vars(a4)
        st      np_dirty-vars(a4)
        movem.l (sp)+,d1/a0-a1/a4
        rts

; notepad_linecol - -> d0 = line, d1 = col of caret (0-based)
notepad_linecol:
        movem.l d2-d3/a0,-(sp)
        lea     npbuf(pc),a0
        moveq   #0,d0
        moveq   #0,d1
        moveq   #0,d2               ; index
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

; notepad_seek_linecol - d0 = target line, d2 = goal col
;   -> d0 = caret index (clamped to line end), or -1 if no such line
notepad_seek_linecol:
        movem.l d3-d5/a0,-(sp)
        lea     npbuf(pc),a0
        moveq   #0,d3               ; index
        moveq   #0,d4               ; line
.fs:    cmp.w   d0,d4
        beq     .at
        cmp.w   np_len(pc),d3
        bge     .nofind
        cmp.b   #13,(a0,d3.w)
        bne     .fnc
        addq.w  #1,d4
.fnc:   addq.w  #1,d3
        bra     .fs
.at:    moveq   #0,d5               ; col
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

; notepad_draw - a2 = window
notepad_draw:
        movem.l d0-d7/a0-a4,-(sp)
        ; clear content area (above the status row)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        sub.w   #TBAR_H+11,d3
        moveq   #0,d4              ; white
        bsr     fill_rect
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
        bsr     notepad_linecol     ; d0 = caret line
        move.w  np_top(pc),d1
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
.nbot:  lea     vars(pc),a4
        move.w  d1,np_top-vars(a4)
.noscr:
        ; line loop: a0 = scan ptr, d3 = line index
        lea     npbuf(pc),a0
        move.w  np_top(pc),d3       ; first visible line
        move.w  d3,d2               ; lines to skip
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
        ; find line end (absolute index check)
        move.l  a0,a1               ; a1 = line start
        moveq   #0,d2               ; len
.find:  move.l  a1,d0
        lea     npbuf(pc),a3
        sub.l   a3,d0
        add.w   d2,d0               ; absolute index = (a1-npbuf)+d2
        cmp.w   np_len(pc),d0
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
.lenok: lea     npline(pc),a3
        move.w  d0,d1
        beq     .zt
        subq.w  #1,d1
        move.l  a1,a4
.cpl:   move.b  (a4)+,(a3)+
        dbra    d1,.cpl
.zt:    clr.b   (a3)
        ; draw the line (black)
        movem.l d2-d4/a0-a1,-(sp)
        lea     npline(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        movem.l (sp)+,d2-d4/a0-a1
        ; caret on this line?
        movem.l d2-d4/a0-a1,-(sp)
        bsr     notepad_linecol     ; d0=line d1=col
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
        moveq   #3,d4              ; black caret bar
        bsr     fill_rect
.nocaret:
        movem.l (sp)+,d2-d4/a0-a1
        ; advance to next line
        lea     1(a1,d2.w),a0       ; skip CR
        move.l  a0,d0
        lea     npbuf(pc),a3
        sub.l   a3,d0
        cmp.w   np_len(pc),d0
        bgt     .status
        add.w   #10,d5
        addq.w  #1,d3
        subq.w  #1,d4
        bra     .line
.status:
        ; status row: black bar + "Ln x Co y  NNN B [*] F1 save" in white
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #11,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        moveq   #10,d3
        moveq   #3,d4              ; black bar
        bsr     fill_rect
        ; build the string
        lea     npstat(pc),a0
        lea     str_n_ln(pc),a1
        bsr     str_append
        bsr     notepad_linecol     ; d0=line d1=col
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
        ; draw it (white on the black bar)
        lea     npstat(pc),a0
        move.w  WX(a2),d0
        addq.w  #4,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #10,d1
        moveq   #0,d2              ; white fg
        moveq   #3,d3              ; black bg
        bsr     draw_string_bg
        movem.l (sp)+,d0-d7/a0-a4
        rts

; notepad_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
notepad_key:
        lea     vars(pc),a4
        cmp.b   #$4F,d2             ; left
        beq     .left
        cmp.b   #$4E,d2             ; right
        beq     .right
        cmp.b   #$4C,d2             ; up
        beq     .up
        cmp.b   #$4D,d2             ; down
        beq     .down
        cmp.b   #$50,d2             ; F1 (Clr) = save
        beq     .save
        cmp.b   #8,d1               ; backspace
        beq     .bs
        cmp.b   #13,d1              ; return inserts CR
        beq     .ins
        cmp.b   #32,d1
        bge     .ins
        moveq   #1,d0               ; not consumed (incl. ESC)
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
.up:    bsr     notepad_linecol     ; d0=line d1=col
        tst.w   d0
        beq     .redraw             ; already on first line
        move.w  np_goal(pc),d2
        bpl     .upg                ; keep existing goal column
        move.w  d1,d2
        move.w  d2,np_goal-vars(a4)
.upg:   subq.w  #1,d0
        bsr     notepad_seek_linecol
        tst.w   d0
        bmi     .redraw
        move.w  d0,np_caret-vars(a4)
        bra     .redraw
.down:  bsr     notepad_linecol     ; d0=line d1=col
        move.w  np_goal(pc),d2
        bpl     .dng
        move.w  d1,d2
        move.w  d2,np_goal-vars(a4)
.dng:   addq.w  #1,d0
        bsr     notepad_seek_linecol
        tst.w   d0
        bmi     .redraw             ; no next line
        move.w  d0,np_caret-vars(a4)
        bra     .redraw
.bs:    move.w  #-1,np_goal-vars(a4)
        move.w  np_caret(pc),d0
        beq     .redraw
        ; shift [caret..len) left by one
        lea     npbuf(pc),a0
        move.w  np_caret(pc),d1
        move.w  np_len(pc),d2
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
        ; shift [caret..len) right by one (backwards copy)
        lea     npbuf(pc),a0
        move.w  np_len(pc),d2
        sub.w   np_caret(pc),d2     ; count
        lea     (a0,d0.w),a1        ; a1 = npbuf+len (one past last)
.insloop:
        tst.w   d2
        beq     .insdone
        move.b  -(a1),d3
        move.b  d3,1(a1)
        subq.w  #1,d2
        bra     .insloop
.insdone:
        move.w  np_caret(pc),d0
        lea     npbuf(pc),a0
        move.b  d1,(a0,d0.w)        ; ascii (13 for return)
        addq.w  #1,d0
        move.w  d0,np_caret-vars(a4)
        move.w  np_len(pc),d0
        addq.w  #1,d0
        move.w  d0,np_len-vars(a4)
        st      np_dirty-vars(a4)
        bra     .redraw
.save:
        move.b  fat_mounted(pc),d0
        beq     .redraw             ; no disk mounted: stay RAM-only
        move.w  np_fatidx(pc),d0
        bmi     .saveunt            ; -1 = untitled (demo): UNTITLED.TXT
        mulu    #18,d0
        lea     fat_tab(pc),a0
        lea     (a0,d0.w),a0        ; 11-char name of the open file
        bra     .dofat
.saveunt:
        lea     str_untitled(pc),a0
.dofat: lea     npbuf(pc),a1
        moveq   #0,d1
        move.w  np_len(pc),d1
        bsr     fat_save_file
        tst.w   d0
        bmi     .redraw             ; failed (write-protect / disk full)
        sf      np_dirty-vars(a4)
        bsr     fat_list_root       ; refresh listing (new file may appear)
        bra     .redraw
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts
