; ============================================================================
; UnoDOS/Genesis SRAM storage (milestone 4) + the Files app (proc 7).
;
; 8KB of battery-backed cartridge SRAM, declared in the ROM header
; ("RA" $F8 $20, odd bytes at $200001-$203FFF) - emulators and
; flashcarts persist it as a .sav. Byte n of the store lives at
; $200001 + n*2.
;
; Mini-filesystem ("USV1"):
;   0..3    magic "USV1"
;   4..5    file count (word, big-endian)
;   6..7    heap top = offset of the next free heap byte
;   16..143 8 directory entries x 16: name[12] (NUL-padded), size.w, off.w
;   144..   heap (8048 bytes)
; Files are contiguous in the heap; delete compacts. Save-by-name
; overwrites (delete + append). All multi-byte fields are big-endian
; (written byte-wise by the 68000; SRAM is not interchange media).
; ============================================================================

SRAM_BASE   equ $200001
SRAM_CTRL   equ $A130F1
SRAM_SIZE   equ 8192
SRD_OFF     equ 16                  ; directory offset
SRD_MAX     equ 8                   ; directory entries
SRD_ENT     equ 16                  ; entry size
SRH_OFF     equ 144                 ; heap offset

; sram_on / sram_off - with a 64KB ROM there is no address overlap, so
; SRAM stays mapped permanently: sram_init writes $A130F1 = 1 once and
; these are no-ops. (Toggling the register per-access broke the mapping
; in BlastEm - subsequent reads returned open-bus $FF. The dance is
; only needed on >2MB ROMs that overlap the SRAM window.)
sram_on:
        rts
sram_off:
        rts

; srd / swr - read/write SRAM byte at offset d0 (-> / <- d1.b).
; Preserve everything else.
srd:
        movem.l d0/a0,-(sp)
        add.w   d0,d0               ; byte offset * 2 (odd-lane SRAM);
        lea     SRAM_BASE,a0        ; max 16382, so .w indexing is safe
        moveq   #0,d1
        move.b  (a0,d0.w),d1
        movem.l (sp)+,d0/a0
        rts
swr:
        movem.l d0/a0,-(sp)
        add.w   d0,d0
        lea     SRAM_BASE,a0
        move.b  d1,(a0,d0.w)
        movem.l (sp)+,d0/a0
        rts

; srd_w / swr_w - word accessors (big-endian byte pair at offset d0)
srd_w:
        movem.l d2,-(sp)
        bsr     srd
        move.w  d1,d2
        lsl.w   #8,d2
        addq.w  #1,d0
        bsr     srd
        subq.w  #1,d0
        or.w    d2,d1
        movem.l (sp)+,d2
        rts
swr_w:
        movem.l d1-d2,-(sp)
        move.w  d1,d2
        lsr.w   #8,d1
        bsr     swr
        addq.w  #1,d0
        move.w  d2,d1
        bsr     swr
        subq.w  #1,d0
        movem.l (sp)+,d1-d2
        rts

; sram_init - map SRAM (once, permanently), validate the magic, format
; an uninitialized store
sram_init:
        movem.l d0-d2/a0,-(sp)
        move.b  #1,SRAM_CTRL        ; map SRAM; never unmapped again
        lea     str_usv1(pc),a0
        moveq   #0,d0
.chk:   bsr     srd
        cmp.b   (a0,d0.w),d1
        bne     .format
        addq.w  #1,d0
        cmp.w   #4,d0
        blt     .chk
        bra     .out
.format:
        lea     str_usv1(pc),a0
        moveq   #0,d0
.put:   move.b  (a0,d0.w),d1
        bsr     swr
        addq.w  #1,d0
        cmp.w   #4,d0
        blt     .put
        moveq   #4,d0
        moveq   #0,d1
        bsr     swr_w               ; count = 0
        moveq   #6,d0
        move.w  #SRH_OFF,d1
        bsr     swr_w               ; heap top
.out:   bsr     sram_off
        movem.l (sp)+,d0-d2/a0
        rts

; sram_count -> d0.w
sram_count:
        move.l  d1,-(sp)
        bsr     sram_on
        moveq   #4,d0
        bsr     srd_w
        move.w  d1,d0
        bsr     sram_off
        move.l  (sp)+,d1
        rts

; sram_entry - d0 = index -> d0 = directory offset of the entry
sram_entry:
        lsl.w   #4,d0
        add.w   #SRD_OFF,d0
        rts

; sram_name - d0 = index, a0 = 13-byte dest buffer (NUL-terminated)
sram_name:
        movem.l d0-d2/a0,-(sp)
        bsr     sram_on
        bsr     sram_entry
        moveq   #11,d2
.cp:    bsr     srd
        move.b  d1,(a0)+
        addq.w  #1,d0
        dbra    d2,.cp
        clr.b   (a0)
        bsr     sram_off
        movem.l (sp)+,d0-d2/a0
        rts

; sram_size - d0 = index -> d0.w = size
sram_size:
        move.l  d1,-(sp)
        bsr     sram_on
        bsr     sram_entry
        add.w   #12,d0
        bsr     srd_w
        move.w  d1,d0
        bsr     sram_off
        move.l  (sp)+,d1
        rts

; sram_read - d0 = index, a1 = dest, d2 = max -> d0.w = bytes read
sram_read:
        movem.l d1-d5/a1,-(sp)
        bsr     sram_on
        move.w  d0,d3
        bsr     sram_entry
        move.w  d0,d4               ; entry offset
        add.w   #12,d0
        bsr     srd_w
        move.w  d1,d5               ; size
        cmp.w   d2,d5
        ble     .szok
        move.w  d2,d5
.szok:  move.w  d4,d0
        add.w   #14,d0
        bsr     srd_w
        move.w  d1,d0               ; data offset
        move.w  d5,d2
        beq     .done
        subq.w  #1,d2
.cp:    bsr     srd
        move.b  d1,(a1)+
        addq.w  #1,d0
        dbra    d2,.cp
.done:  move.w  d5,d0
        bsr     sram_off
        movem.l (sp)+,d1-d5/a1
        rts

; sram_find - a0 = 12-byte name -> d0 = index or -1
sram_find:
        movem.l d1-d4/a0-a1,-(sp)
        bsr     sram_on
        moveq   #4,d0
        bsr     srd_w
        move.w  d1,d4               ; count
        moveq   #0,d3               ; index
.ent:   cmp.w   d4,d3
        bge     .miss
        move.w  d3,d0
        bsr     sram_entry
        move.l  a0,a1
        moveq   #0,d2
.ch:    bsr     srd
        cmp.b   (a1,d2.w),d1
        bne     .next
        addq.w  #1,d0
        addq.w  #1,d2
        cmp.w   #12,d2
        blt     .ch
        move.w  d3,d0
        bsr     sram_off
        movem.l (sp)+,d1-d4/a0-a1
        rts
.next:  addq.w  #1,d3
        bra     .ent
.miss:  moveq   #-1,d0
        bsr     sram_off
        movem.l (sp)+,d1-d4/a0-a1
        rts

; sram_delete - d0 = index: compact the heap, shift the directory
sram_delete:
        movem.l d0-d7/a0,-(sp)
        bsr     sram_on
        move.w  d0,d7               ; victim index
        bsr     sram_entry
        move.w  d0,d6               ; victim entry offset
        add.w   #12,d0
        bsr     srd_w
        move.w  d1,d4               ; victim size
        move.w  d6,d0
        add.w   #14,d0
        bsr     srd_w
        move.w  d1,d5               ; victim data offset
        ; heap compaction: move [off+size, heaptop) down by size
        moveq   #6,d0
        bsr     srd_w
        move.w  d1,d3               ; heap top
        move.w  d5,d0
        add.w   d4,d0               ; src
        move.w  d5,d2               ; dst
.mv:    cmp.w   d3,d0
        bge     .mvdone
        bsr     srd                 ; d1 = [src]
        exg     d0,d2
        bsr     swr                 ; [dst] = d1
        exg     d0,d2
        addq.w  #1,d0
        addq.w  #1,d2
        bra     .mv
.mvdone:
        ; heap top -= size
        move.w  d3,d1
        sub.w   d4,d1
        moveq   #6,d0
        bsr     swr_w
        ; fix offsets of entries above the victim's data
        moveq   #4,d0
        bsr     srd_w
        move.w  d1,d3               ; count
        moveq   #0,d2
.fix:   cmp.w   d3,d2
        bge     .fixdone
        move.w  d2,d0
        bsr     sram_entry
        add.w   #14,d0
        bsr     srd_w
        cmp.w   d5,d1
        ble     .fnext
        sub.w   d4,d1
        bsr     swr_w
.fnext: addq.w  #1,d2
        bra     .fix
.fixdone:
        ; shift directory entries [victim+1..count) down one
        move.w  d7,d2
.shift: addq.w  #1,d2
        cmp.w   d3,d2
        bge     .shdone
        move.w  d2,d0
        bsr     sram_entry
        move.w  d0,d4               ; src entry
        moveq   #0,d6
.shb:   move.w  d4,d0
        add.w   d6,d0
        bsr     srd
        sub.w   #SRD_ENT,d0
        bsr     swr
        addq.w  #1,d6
        cmp.w   #SRD_ENT,d6
        blt     .shb
        bra     .shift
.shdone:
        ; count--
        subq.w  #1,d3
        move.w  d3,d1
        moveq   #4,d0
        bsr     swr_w
        bsr     sram_off
        movem.l (sp)+,d0-d7/a0
        rts

; sram_save - a0 = 12-byte name, a1 = src, d1.w = len
;             -> d0 = 0 ok / -1 full
sram_save:
        movem.l d1-d7/a0-a1,-(sp)
        move.w  d1,d7               ; len
        ; overwrite: delete an existing file of this name
        bsr     sram_find
        bmi     .fresh
        bsr     sram_delete
.fresh: bsr     sram_on
        moveq   #4,d0
        bsr     srd_w
        move.w  d1,d4               ; count
        cmp.w   #SRD_MAX,d4
        bge     .full
        moveq   #6,d0
        bsr     srd_w
        move.w  d1,d5               ; heap top
        move.w  d5,d0
        add.w   d7,d0
        cmp.w   #SRAM_SIZE,d0
        bgt     .full
        ; directory entry
        move.w  d4,d0
        bsr     sram_entry
        move.w  d0,d6
        moveq   #0,d2
.nm:    move.b  (a0,d2.w),d1
        bsr     swr
        addq.w  #1,d0
        addq.w  #1,d2
        cmp.w   #12,d2
        blt     .nm
        move.w  d6,d0
        add.w   #12,d0
        move.w  d7,d1
        bsr     swr_w               ; size
        move.w  d6,d0
        add.w   #14,d0
        move.w  d5,d1
        bsr     swr_w               ; offset
        ; data
        move.w  d5,d0
        move.w  d7,d2
        beq     .zlen
        subq.w  #1,d2
.cp:    move.b  (a1)+,d1
        bsr     swr
        addq.w  #1,d0
        dbra    d2,.cp
.zlen:  ; heap top + count
        move.w  d5,d1
        add.w   d7,d1
        moveq   #6,d0
        bsr     swr_w
        addq.w  #1,d4
        move.w  d4,d1
        moveq   #4,d0
        bsr     swr_w
        bsr     sram_off
        moveq   #0,d0
        movem.l (sp)+,d1-d7/a0-a1
        rts
.full:  bsr     sram_off
        moveq   #-1,d0
        movem.l (sp)+,d1-d7/a0-a1
        rts

; ============================================================================
; Files app (proc 7) - storage listing; Enter/C opens into Notepad, d
; deletes, w/r = tape, v cycles the volume (SRAM <-> Sega CD BRAM when
; a CD attachment was detected). All storage access goes through the
; store_* volume dispatch in bram.i.
; ============================================================================

; files_draw - a2 = window
files_draw:
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
        subq.w  #2,d3
        move.w  #ATTR_NORM+T_SOLBG,d4
        bsr     fill_cells
        ; header (names the active volume)
        lea     str_f_hdr(pc),a0
        tst.w   v_files_vol(a4)
        beq     .hdr
        lea     str_f_hdrb(pc),a0
.hdr:   move.w  WX(a2),d6
        addq.w  #2,d6
        move.w  WY(a2),d5
        addq.w  #2,d5
        move.w  d6,d0
        move.w  d5,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        ; rows
        bsr     store_count
        move.w  d0,d3
        moveq   #0,d7
.row:   cmp.w   d3,d7
        bge     .foot
        move.w  d5,d1
        addq.w  #1,d1
        add.w   d7,d1               ; row y
        move.w  #ATTR_NORM,d4
        cmp.w   v_files_sel(a4),d7
        bne     .attr
        move.w  #ATTR_INV,d4
        ; selection bar
        movem.l d1/d3,-(sp)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WW(a2),d2
        subq.w  #2,d2
        moveq   #1,d3
        movem.l d4,-(sp)
        add.w   #T_SOLBG,d4
        bsr     fill_cells
        movem.l (sp)+,d4
        movem.l (sp)+,d1/d3
.attr:  move.w  d7,d0
        lea     v_numbuf(a4),a0
        bsr     store_name
        move.w  d6,d0
        bsr     draw_str
        ; size
        movem.l d1/d3-d4,-(sp)
        move.w  d7,d0
        bsr     store_size
        lea     v_npstat(a4),a0
        bsr     fmt_dec
        movem.l (sp)+,d1/d3-d4
        move.w  d6,d0
        add.w   #14,d0
        bsr     draw_str
        addq.w  #1,d7
        bra     .row
.foot:  tst.w   d3
        bne     .help
        lea     str_f_empty(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        addq.w  #2,d1
        move.w  #ATTR_NORM,d4
        bsr     draw_str
.help:  lea     str_f_foot(pc),a0
        move.b  v_bram_pres(a4),d0
        beq     .ft
        lea     str_f_footv(pc),a0
.ft:    move.w  d6,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #2,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        ; tape status line (filled in by the tape ops)
        move.b  v_tp_msg(a4),d0
        beq     .done
        lea     str_tp_ok(pc),a0
        cmp.b   #1,d0
        beq     .msg
        lea     str_tp_err(pc),a0
.msg:   move.w  d6,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        subq.w  #3,d1
        move.w  #ATTR_INV,d4
        bsr     draw_str
.done:  movem.l (sp)+,d0-d7/a0/a4
        rts

; files_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
files_key:
        lea     VARS,a4
        cmp.b   #$4D,d2             ; down
        beq     .down
        cmp.b   #$4C,d2             ; up
        beq     .up
        cmp.b   #13,d1              ; Enter: open
        beq     .open
        cmp.b   #'d',d1
        beq     .del
        cmp.b   #8,d1               ; X button (Backspace) = delete
        beq     .del
        cmp.b   #'w',d1             ; write Notepad buffer to tape
        beq     .tapew
        cmp.b   #'r',d1             ; read a tape block into Notepad
        beq     .taper
        cmp.b   #'v',d1             ; cycle the volume (when BRAM exists)
        beq     .vol
        moveq   #1,d0
        rts
.vol:   move.b  v_bram_pres(a4),d0
        beq     .redraw
        move.w  v_files_vol(a4),d0
        eor.w   #1,d0
        move.w  d0,v_files_vol(a4)
        beq     .vsel
        bsr     bram_refresh        ; entering the BRAM volume: re-list
.vsel:  clr.w   v_files_sel(a4)
        bra     .redraw
.down:  move.w  v_files_sel(a4),d0
        addq.w  #1,d0
        move.w  d0,-(sp)
        bsr     store_count
        move.w  (sp)+,d1
        cmp.w   d0,d1
        bge     .redraw
        move.w  d1,v_files_sel(a4)
        bra     .redraw
.up:    move.w  v_files_sel(a4),d0
        subq.w  #1,d0
        blt     .redraw
        move.w  d0,v_files_sel(a4)
        bra     .redraw
.open:  bsr     store_count
        tst.w   d0
        beq     .redraw
        ; name -> np_name (listing name first; a BRAM payload header
        ; restores the original 8.3 name), data -> npbuf
        move.w  v_files_sel(a4),d0
        lea     v_np_name(a4),a0
        bsr     store_name
        move.w  v_files_sel(a4),d0
        lea     v_np_name(a4),a0
        lea     v_npbuf(a4),a1
        move.w  #NBUF-1,d2
        bsr     store_open_read
        move.w  d0,v_np_len(a4)
        clr.w   v_np_caret(a4)
        clr.w   v_np_top(a4)
        move.w  #-1,v_np_goal(a4)
        sf      v_np_dirty(a4)
        sf      v_tp_msg(a4)
        moveq   #2,d0
        bsr     launch_app          ; Notepad (raises if open)
        bsr     redraw_topmost
        moveq   #0,d0
        rts
.del:   bsr     store_count
        tst.w   d0
        beq     .redraw
        move.w  v_files_sel(a4),d0
        bsr     store_delete
        bsr     store_count
        move.w  d0,d1
        beq     .selz
        subq.w  #1,d1
        cmp.w   v_files_sel(a4),d1
        bge     .redraw
        move.w  d1,v_files_sel(a4)
        bra     .redraw
.selz:  clr.w   v_files_sel(a4)
        bra     .redraw
.tapew: bsr     tape_save_buf       ; (tape.i) notepad buffer -> PSG
        bra     .redraw
.taper: bsr     tape_load_buf       ; (tape.i) port 2 D0 -> notepad
        bra     .redraw
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

; np_save_sram - Notepad F1: save the buffer under np_name to the
; active volume (SRAM, or Sega CD BRAM when selected in Files)
np_save_sram:
        movem.l d0-d1/a0-a1/a4,-(sp)
        lea     VARS,a4
        lea     v_np_name(a4),a0
        lea     v_npbuf(a4),a1
        move.w  v_np_len(a4),d1
        bsr     store_save
        tst.w   d0
        bmi     .out                ; full/error: keep the dirty flag
        sf      v_np_dirty(a4)
.out:   movem.l (sp)+,d0-d1/a0-a1/a4
        rts

str_usv1:       dc.b    "USV1"
str_f_hdr:      dc.b    "Name          Size  (SRAM)",0
str_f_hdrb:     dc.b    "Name          Size  (BRAM)",0
str_f_foot:     dc.b    "C:open X:del w/r:tape",0
str_f_footv:    dc.b    "C:open X:del w/r:tp v:vol",0
str_f_empty:    dc.b    "no files - F1 in Notepad saves",0
str_tp_ok:      dc.b    "TAPE OK         ",0
str_tp_err:     dc.b    "TAPE ERR/NO SIG ",0
        even
