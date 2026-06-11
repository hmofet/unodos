; ============================================================================
; UnoDOS/68K milestone 2 apps: Files, Notepad, Music
; Included by kernel.asm. Same conventions: PC-relative reads, writes via
; lea vars(pc),a4 + offset; key handlers in: d1=ascii d2=raw,
; out: d0=0 consumed / d0=1 not consumed.
; ============================================================================

; ---------------------------------------------------------------------------
; fmt_dec - d0.w unsigned -> decimal digits + NUL at a0 (no suffix)
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

; str_append - append NUL string a1 to cursor a0 (a0 advances to new NUL)
str_append:
.cp:    move.b  (a1)+,(a0)+
        bne     .cp
        subq.l  #1,a0
        rts

; rd_entry - d0.w = index -> a3 = romdisk entry. Preserves d0-d7/a0-a2.
rd_entry:
        move.w  d0,-(sp)
        mulu    #RD_ENT,d0
        lea     romdisk_tab(pc),a3
        add.w   d0,a3
        move.w  (sp)+,d0
        rts

; ============================================================================
; Files app (proc 2)
; ============================================================================

; files_draw - a2 = window
files_draw:
        movem.l d0-d7/a0-a3,-(sp)
        move.w  WX(a2),d6
        addq.w  #6,d6               ; content x
        move.w  WY(a2),d5
        add.w   #TBAR_H+4,d5        ; content y
        ; header
        lea     str_f_hdr(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #1,d2               ; cyan
        bsr     draw_string
        ; rows
        moveq   #0,d7               ; index
.row:   move.b  files_src(pc),d0
        beq     .romcnt
        cmp.w   fat_count(pc),d7
        bge     .foot
        bra     .haveidx
.romcnt:
        cmp.w   romdisk_count(pc),d7
        bge     .foot
.haveidx:
        move.b  files_src(pc),d0
        beq     .romsrc
        move.w  d7,d0
        mulu    #18,d0
        lea     fat_tab(pc),a3
        lea     (a3,d0.w),a3        ; a3 = FAT entry
        bra     .gotentry
.romsrc:
        move.w  d7,d0
        bsr     rd_entry            ; a3 = entry
.gotentry:
        ; row position
        move.w  d5,d1
        add.w   #12,d1
        move.w  d7,d0
        mulu    #11,d0
        add.w   d0,d1               ; d1 = row y
        ; selection bar
        cmp.w   files_sel(pc),d7
        bne     .name
        movem.l d1/a3,-(sp)
        move.w  WX(a2),d0
        addq.w  #2,d0
        subq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #4,d2
        moveq   #10,d3
        moveq   #3,d4               ; white bar
        bsr     fill_rect
        movem.l (sp)+,d1/a3
.name:
        ; name: ROM-disk entries are NUL-padded C strings; FAT entries are
        ; space-padded 8.3 - copy 11 chars + NUL into numbuf for those
        move.b  files_src(pc),d0
        beq     .nmrom
        movem.l d1-d2/a1,-(sp)
        lea     numbuf(pc),a0
        moveq   #10,d2
        move.l  a3,a1
.nmcp:  move.b  (a1)+,(a0)+
        dbra    d2,.nmcp
        clr.b   (a0)
        movem.l (sp)+,d1-d2/a1
        lea     numbuf(pc),a0
        bra     .nmgo
.nmrom: move.l  a3,a0
.nmgo:  move.w  d6,d0
        movem.l d1/a3,-(sp)
        cmp.w   files_sel(pc),d7
        bne     .fgw
        moveq   #0,d2               ; selected: blue on the white bar
        bra     .dN
.fgw:   moveq   #3,d2               ; normal: white
.dN:    bsr     draw_string
        movem.l (sp)+,d1/a3
        ; size, right-ish column
        move.b  files_src(pc),d0
        beq     .szrom
        move.l  12(a3),d0           ; FAT size (long); display low word
        cmp.l   #65535,d0
        ble     .szok
        move.l  #65535,d0
.szok:  bra     .szgo
.szrom: move.w  12+4(a3),d0         ; ROM-disk size word
.szgo:
        lea     numbuf(pc),a0
        bsr     fmt_dec
        lea     numbuf(pc),a0
        move.w  d6,d0
        add.w   #112,d0
        movem.l d1/a3,-(sp)
        cmp.w   files_sel(pc),d7
        bne     .fgw2
        moveq   #0,d2
        bra     .dS
.fgw2:  moveq   #1,d2               ; cyan
.dS:    bsr     draw_string
        movem.l (sp)+,d1/a3
        addq.w  #1,d7
        bra     .row
.foot:
        lea     str_f_foot(pc),a0
        move.b  files_src(pc),d0
        beq     .footgo
        lea     str_f_footf(pc),a0
.footgo:
        move.w  d6,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #12,d1
        moveq   #1,d2
        bsr     draw_string
        movem.l (sp)+,d0-d7/a0-a3
        rts

; files_key - d1=ascii d2=raw -> d0=0 consumed
files_key:
        cmp.b   #$4D,d2             ; down
        beq     .down
        cmp.b   #$4C,d2             ; up
        beq     .up
        cmp.b   #13,d1              ; Enter: open
        beq     .open
        cmp.b   #'r',d1
        beq     .remount            ; refresh (re-mounts when on FAT)
        cmp.b   #'m',d1
        beq     .mount              ; mount the DF1 FAT12 data disk
        moveq   #1,d0
        rts
.mount:
.remount:
        move.b  files_src(pc),d0
        bne     .domount
        cmp.b   #'m',d1
        bne     .redraw             ; 'r' on the ROM-disk: just repaint
.domount:
        lea     vars(pc),a4
        bsr     fat_mount
        tst.w   d0
        bmi     .mfail
        bsr     fat_list_root
        st      files_src-vars(a4)
        clr.w   files_sel-vars(a4)
        bra     .redraw
.mfail: sf      files_src-vars(a4)  ; fall back to the ROM-disk listing
        clr.w   files_sel-vars(a4)
        bra     .redraw
.down:  move.w  files_sel(pc),d0
        addq.w  #1,d0
        move.b  files_src(pc),d1
        beq     .cntrom
        cmp.w   fat_count(pc),d0
        bge     .redraw
        bra     .selok
.cntrom:
        cmp.w   romdisk_count(pc),d0
        bge     .redraw
.selok:
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
        move.b  files_src(pc),d0
        bne     .openfat
        move.w  files_sel(pc),d0
        bsr     notepad_open_file
        bra     .launch
.openfat:
        bsr     notepad_open_fat
.launch:
        moveq   #3,d0
        bsr     launch_app          ; raises + repaints if already open
        bsr     redraw_topmost
        moveq   #0,d0
        rts
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
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
; Notepad app (proc 3)
; ============================================================================

; notepad_open_file - d0 = romdisk index: copy file into the edit buffer
notepad_open_file:
        movem.l d0-d2/a0-a1/a3-a4,-(sp)
        bsr     rd_entry            ; a3 = entry
        lea     vars(pc),a4
        move.w  d0,np_file-vars(a4)
        move.l  12(a3),a0           ; data
        move.w  16(a3),d1           ; size
        cmp.w   #NBUF-1,d1
        ble     .szok
        move.w  #NBUF-1,d1
.szok:  move.w  d1,np_len-vars(a4)
        clr.w   np_caret-vars(a4)
        clr.w   np_top-vars(a4)
        move.w  #-1,np_goal-vars(a4)
        sf      np_dirty-vars(a4)
        lea     npbuf(pc),a1
        move.w  d1,d2
        beq     .done
        subq.w  #1,d2
.cp:    move.b  (a0)+,(a1)+
        dbra    d2,.cp
.done:  movem.l (sp)+,d0-d2/a0-a1/a3-a4
        rts

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
        move.w  #-2,np_file-vars(a4) ; FAT origin: F1 save disabled (read-only v1)
        sf      np_dirty-vars(a4)
        movem.l (sp)+,d0-d2/a0-a1/a3-a4
        rts

; notepad_set_demo - load demo_text (AUTOTEST)
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
        move.w  #-1,np_file-vars(a4)
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
        moveq   #0,d4
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
        ; find line end
        move.l  a0,a1               ; a1 = line start
        moveq   #0,d2               ; len
.find:  move.l  a1,d0
        sub.l   a0,d0
        add.w   d2,d0               ; (a1-a0)+len ... simpler: index check below
        ; compute absolute index = (a1 - npbuf) + d2
        move.l  a1,d0
        lea     npbuf(pc),a3
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
        ; draw the line
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
        move.w  d0,d2
        moveq   #2,d2
        moveq   #8,d3
        moveq   #1,d4               ; cyan
        ; fill_rect(d0=x d1=y d2=w d3=h d4=col)
        bsr     fill_rect
.nocaret:
        movem.l (sp)+,d2-d4/a0-a1
        ; advance to next line
        lea     1(a1,d2.w),a0       ; skip CR
        ; stop if past end
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
        ; status row: white bar + "Ln x Co y  NNN B [*] F1 save"
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   d1,d1               ; (scratch zero - recompute below)
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #11,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        moveq   #10,d3
        moveq   #3,d4
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
        ; draw it (blue on white, glyph TOP at bar top + 1)
        lea     npstat(pc),a0
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
        beq     .bsdone
        bra     .bsloop
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
        move.w  np_file(pc),d0
        bmi     .redraw             ; untitled: RAM only
        bsr     rd_entry            ; a3 = entry
        move.w  np_len(pc),d1
        cmp.w   18(a3),d1           ; capacity
        bgt     .redraw             ; too big: refuse (status keeps *)
        move.w  d1,16(a3)           ; new size
        move.l  12(a3),a1           ; dest
        lea     npbuf(pc),a0
        move.w  d1,d2
        beq     .savedone
        subq.w  #1,d2
.svloop:
        move.b  (a0)+,(a1)+
        dbra    d2,.svloop
.savedone:
        sf      np_dirty-vars(a4)
        bra     .redraw
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

; ============================================================================
; Music app (proc 4) - Paula square wave sequencer
; ============================================================================

; music_start
music_start:
        movem.l d0-d1/a0/a4/a6,-(sp)
        lea     vars(pc),a4
        clr.w   mus_ix-vars(a4)
        st      mus_playing-vars(a4)
        move.l  ticks(pc),d0
        lea     mus_notes(pc),a0
        add.l   #0,d0
        moveq   #0,d1
        move.w  2(a0),d1            ; first duration
        add.l   d1,d0
        move.l  d0,mus_end-vars(a4)
        lea     CUSTOM,a6
        move.w  (a0),AUD0PER(a6)    ; first period
        move.w  #48,AUD0VOL(a6)
        move.w  #$8201,DMACON(a6)   ; DMAEN|AUD0EN
        movem.l (sp)+,d0-d1/a0/a4/a6
        rts

; music_stop
music_stop:
        movem.l a4/a6,-(sp)
        lea     vars(pc),a4
        sf      mus_playing-vars(a4)
        lea     CUSTOM,a6
        move.w  #0,AUD0VOL(a6)
        move.w  #$0001,DMACON(a6)   ; AUD0 DMA off
        movem.l (sp)+,a4/a6
        rts

; music_tick - advance the sequencer; refresh window when topmost
music_tick:
        move.b  mus_playing(pc),d0
        bne     .on
        rts
.on:
        movem.l d0-d3/a0/a2/a4/a6,-(sp)
        move.l  ticks(pc),d0
        cmp.l   mus_end(pc),d0
        blt     .out
        lea     vars(pc),a4
        move.w  mus_ix(pc),d1
        addq.w  #1,d1
        cmp.w   mus_count(pc),d1
        blt     .ixok
        moveq   #0,d1               ; loop
.ixok:  move.w  d1,mus_ix-vars(a4)
        ; set period + new end time
        move.w  d1,d2
        mulu    #6,d2
        lea     mus_notes(pc),a0
        add.w   d2,a0
        lea     CUSTOM,a6
        move.w  (a0),AUD0PER(a6)
        moveq   #0,d2
        move.w  2(a0),d2
        add.l   d2,d0
        move.l  d0,mus_end-vars(a4)
        ; topmost-only visual refresh (PORT-SPEC SS2)
        move.w  zcount(pc),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        moveq   #0,d3
        move.b  WPROC(a2),d3
        cmp.w   #4,d3
        bne     .out
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
        move.b  mus_playing(pc),d0
        beq     .play
        bsr     music_stop
        bra     .rd
.play:  bsr     music_start
.rd:    bsr     redraw_topmost
        moveq   #0,d0
        rts

; music_draw - a2 = window
music_draw:
        movem.l d0-d7/a0-a3,-(sp)
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
        ; title
        lea     str_m_title(pc),a0
        move.w  WX(a2),d0
        addq.w  #6,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+3,d1
        moveq   #3,d2
        bsr     draw_string
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
        ; y = wy + TBAR_H + 56 - yoff
        move.w  d7,d2
        mulu    #6,d2
        lea     mus_notes(pc),a0
        add.w   d2,a0
        move.w  WY(a2),d1
        add.w   #TBAR_H+56,d1
        sub.w   4(a0),d1            ; staff y offset
        moveq   #4,d2               ; 4x4 note block
        moveq   #4,d3
        ; color: playing note magenta, others cyan
        moveq   #1,d4
        move.b  mus_playing(pc),d5
        beq     .col
        cmp.w   mus_ix(pc),d7
        bne     .col
        moveq   #2,d4
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
        moveq   #1,d2
        bsr     draw_string
        movem.l (sp)+,d0-d7/a0-a3
        rts
