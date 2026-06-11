; ============================================================================
; UnoDOS/68K portable FAT12 core (read path) over fdd_read_sector.
; Little-endian on-disk structures on a big-endian CPU: all multi-byte
; fields go through the LE helpers.
;
;   fat_mount      -> d0 = 0 ok / -1 (reads the BPB, slurps the FAT)
;   fat_list_root  -> fills fat_tab/fat_count (files only, max 16)
;   fat_read_file  d0 = first cluster, d1 = byte budget, a1 = dest
;                  -> d0 = bytes read or -1
; ============================================================================

FAT_MAXFILES equ 16
FATSECBUF   equ $6D800              ; 512-byte sector scratch (chip ok)

; fat_le16 - (a0,d0.w) -> d0.w little-endian word
fat_le16:
        movem.l d1/a0,-(sp)
        lea     (a0,d0.w),a0
        moveq   #0,d1
        move.b  1(a0),d1
        lsl.w   #8,d1
        move.b  (a0),d1
        move.w  d1,d0
        movem.l (sp)+,d1/a0
        rts

; fat_mount - parse the BPB from LBA 0 and load the whole FAT into RAM
fat_mount:
        movem.l d1-d3/a0-a1/a4,-(sp)
        bsr     fdd_invalidate
        moveq   #0,d0
        lea     FATSECBUF,a1
        bsr     fdd_read_sector
        tst.w   d0
        bmi     .err
        lea     FATSECBUF,a0
        ; bytes/sector must be 512
        moveq   #11,d0
        bsr     fat_le16
        cmp.w   #512,d0
        bne     .err
        lea     vars(pc),a4
        moveq   #0,d1
        move.b  13(a0),d1
        move.w  d1,fat_spc-vars(a4)         ; sectors per cluster
        moveq   #14,d0
        bsr     fat_le16
        move.w  d0,d2                       ; reserved
        moveq   #0,d1
        move.b  16(a0),d1                   ; FAT copies
        moveq   #22,d0
        bsr     fat_le16
        move.w  d0,fat_spf-vars(a4)         ; sectors per FAT
        mulu    d0,d1                       ; nfats * spf
        moveq   #17,d0
        bsr     fat_le16
        move.w  d0,fat_rootents-vars(a4)
        lsl.w   #5,d0                       ; * 32 bytes
        lsr.w   #8,d0
        lsr.w   #1,d0                       ; / 512
        move.w  d0,d3                       ; root sectors
        move.w  d2,fat_fatstart-vars(a4)
        add.w   d1,d2
        move.w  d2,fat_rootstart-vars(a4)
        add.w   d3,d2
        move.w  d2,fat_datastart-vars(a4)
        ; slurp the FAT (first copy) into fat_buf
        move.w  fat_spf(pc),d3
        cmp.w   #3,d3
        ble     .spfok
        moveq   #3,d3                       ; fat_buf holds 3 sectors max
.spfok: move.w  fat_fatstart(pc),d2
        lea     fat_buf(pc),a1
        subq.w  #1,d3
.fatl:  move.w  d2,d0
        and.l   #$FFFF,d0
        bsr     fdd_read_sector
        tst.w   d0
        bmi     .err
        addq.w  #1,d2
        lea     512(a1),a1
        dbra    d3,.fatl
        st      fat_mounted-vars(a4)
        moveq   #0,d0
        bra     .out
.err:   lea     vars(pc),a4
        sf      fat_mounted-vars(a4)
        moveq   #-1,d0
.out:   movem.l (sp)+,d1-d3/a0-a1/a4
        rts

; fat_next_cluster - d0 = cluster -> d0 = next (>= $FF8 = chain end)
fat_next_cluster:
        movem.l d1-d2/a0,-(sp)
        move.w  d0,d1
        move.w  d0,d2
        lsr.w   #1,d2
        add.w   d2,d0               ; offset = cl + cl/2
        lea     fat_buf(pc),a0
        moveq   #0,d2
        move.b  1(a0,d0.w),d2
        lsl.w   #8,d2
        move.b  (a0,d0.w),d2        ; LE 16-bit straddle
        btst    #0,d1
        beq     .even
        lsr.w   #4,d2
        bra     .done
.even:  and.w   #$0FFF,d2
.done:  move.w  d2,d0
        movem.l (sp)+,d1-d2/a0
        rts

; fat_list_root - root directory -> fat_tab (name 11 + attr 1 + size.l
; + cluster.w = 18 bytes/entry), fat_count
fat_list_root:
        movem.l d0-d7/a0-a2/a4,-(sp)
        lea     vars(pc),a4
        clr.w   fat_count-vars(a4)
        move.w  fat_rootstart(pc),d6        ; current sector
        move.w  fat_datastart(pc),d7        ; end of root region
.sect:  cmp.w   d7,d6
        bge     .done
        move.w  d6,d0
        and.l   #$FFFF,d0
        lea     FATSECBUF,a1
        bsr     fdd_read_sector
        tst.w   d0
        bmi     .done
        lea     FATSECBUF,a0
        moveq   #15,d5              ; 16 entries per sector
.ent:   move.b  (a0),d0
        beq     .done               ; first byte 0 = end of directory
        cmp.b   #$E5,d0
        beq     .skip               ; deleted
        move.b  11(a0),d0
        and.b   #$18,d0             ; skip volume labels + subdirs (v1)
        bne     .skip
        move.w  fat_count(pc),d1
        cmp.w   #FAT_MAXFILES,d1
        bge     .done
        ; dest entry
        mulu    #18,d1
        lea     fat_tab(pc),a2
        lea     (a2,d1.w),a2
        moveq   #10,d1              ; copy the 8.3 name
.nm:    move.b  (a0,d1.w),(a2,d1.w)
        dbra    d1,.nm
        move.b  11(a0),11(a2)       ; attr
        ; size (LE32) -> big-endian long at +12
        moveq   #0,d2
        move.b  31(a0),d2
        lsl.l   #8,d2
        move.b  30(a0),d2
        lsl.l   #8,d2
        move.b  29(a0),d2
        lsl.l   #8,d2
        move.b  28(a0),d2
        move.l  d2,12(a2)
        ; first cluster (LE16) -> word at +16
        moveq   #0,d2
        move.b  27(a0),d2
        lsl.w   #8,d2
        move.b  26(a0),d2
        move.w  d2,16(a2)
        move.w  fat_count(pc),d1
        addq.w  #1,d1
        move.w  d1,fat_count-vars(a4)
.skip:  lea     32(a0),a0
        dbra    d5,.ent
        addq.w  #1,d6
        bra     .sect
.done:  movem.l (sp)+,d0-d7/a0-a2/a4
        rts

; fat_read_file - d0 = first cluster, d1 = byte budget, a1 = dest
; -> d0 = bytes read / -1 error
fat_read_file:
        movem.l d1-d7/a0-a2,-(sp)
        move.w  d0,d4               ; current cluster
        move.l  d1,d5               ; budget remaining
        moveq   #0,d7               ; bytes read
.clust: cmp.w   #2,d4
        blt     .done
        cmp.w   #$0FF8,d4
        bge     .done
        tst.l   d5
        ble     .done
        ; lba = datastart + (cl-2) * spc
        move.w  d4,d0
        subq.w  #2,d0
        mulu    fat_spc(pc),d0
        add.w   fat_datastart(pc),d0
        move.w  fat_spc(pc),d6
.sec:   movem.l d0,-(sp)
        and.l   #$FFFF,d0
        lea     FATSECBUF,a2
        exg     a1,a2
        bsr     fdd_read_sector     ; -> FATSECBUF (a1 right now)
        exg     a1,a2
        tst.w   d0
        bmi     .errpop
        ; copy min(512, budget) into the destination
        move.l  d5,d1
        cmp.l   #512,d1
        ble     .part
        moveq   #0,d1
        move.w  #512,d1
.part:  tst.l   d1
        ble     .nopop
        move.l  d1,d2
        subq.w  #1,d2
        lea     FATSECBUF,a0
.cp:    move.b  (a0)+,(a1)+
        dbra    d2,.cp
        add.l   d1,d7
        sub.l   d1,d5
.nopop: movem.l (sp)+,d0
        addq.w  #1,d0
        subq.w  #1,d6
        bne     .sec
        ; next cluster
        move.w  d4,d0
        bsr     fat_next_cluster
        move.w  d0,d4
        bra     .clust
.errpop:
        movem.l (sp)+,d0
        moveq   #-1,d0
        bra     .out
.done:  move.l  d7,d0
.out:   movem.l (sp)+,d1-d7/a0-a2
        rts

; ============================================================================
; Write path
; ============================================================================

; fat_set_entry - d0 = cluster, d1 = 12-bit value (into fat_buf)
fat_set_entry:
        movem.l d0-d3/a0,-(sp)
        move.w  d0,d2
        move.w  d0,d3
        lsr.w   #1,d3
        add.w   d3,d0               ; offset = cl + cl/2
        lea     fat_buf(pc),a0
        btst    #0,d2
        bne     .odd
        ; even: low 8 bits -> [off], high 4 -> low nibble of [off+1]
        move.b  d1,(a0,d0.w)
        move.b  1(a0,d0.w),d3
        and.b   #$F0,d3
        lsr.w   #8,d1
        and.b   #$0F,d1
        or.b    d1,d3
        move.b  d3,1(a0,d0.w)
        bra     .out
.odd:   ; odd: low 4 bits -> high nibble of [off], high 8 -> [off+1]
        move.b  (a0,d0.w),d3
        and.b   #$0F,d3
        move.w  d1,d2
        lsl.b   #4,d2
        and.b   #$F0,d2
        or.b    d2,d3
        move.b  d3,(a0,d0.w)
        move.w  d1,d2
        lsr.w   #4,d2
        move.b  d2,1(a0,d0.w)
.out:   movem.l (sp)+,d0-d3/a0
        rts

; fat_free_chain - d0 = first cluster: zero the chain in fat_buf
fat_free_chain:
        movem.l d0-d2,-(sp)
.loop:  cmp.w   #2,d0
        blt     .out
        cmp.w   #$0FF8,d0
        bge     .out
        move.w  d0,d2
        bsr     fat_next_cluster    ; d0 = next
        exg     d0,d2               ; d0 = current, d2 = next
        moveq   #0,d1
        bsr     fat_set_entry
        move.w  d2,d0
        bra     .loop
.out:   movem.l (sp)+,d0-d2
        rts

; fat_alloc_chain - d0 = clusters needed -> d0 = first cluster / -1
fat_alloc_chain:
        movem.l d1-d6,-(sp)
        move.w  d0,d4               ; remaining
        moveq   #0,d5               ; previous cluster (0 = none)
        moveq   #0,d6               ; first cluster
        moveq   #2,d3               ; scan cursor
.scan:  cmp.w   #872,d3             ; cluster count ceiling for 880KB
        bge     .fail
        move.w  d3,d0
        bsr     fat_next_cluster
        tst.w   d0
        bne     .next               ; in use
        ; claim it: mark end-of-chain, link the previous one
        move.w  d3,d0
        move.w  #$0FFF,d1
        bsr     fat_set_entry
        tst.w   d5
        beq     .first
        move.w  d5,d0
        move.w  d3,d1
        bsr     fat_set_entry
        bra     .claimed
.first: move.w  d3,d6
.claimed:
        move.w  d3,d5
        subq.w  #1,d4
        beq     .done
.next:  addq.w  #1,d3
        bra     .scan
.done:  move.w  d6,d0
        bra     .out
.fail:  tst.w   d6
        beq     .nofree
        move.w  d6,d0
        bsr     fat_free_chain      ; roll back a partial allocation
.nofree:
        moveq   #-1,d0
.out:   movem.l (sp)+,d1-d6
        rts

; fat_flush - write fat_buf back to every FAT copy -> d0 = 0 / -1
fat_flush:
        movem.l d1-d4/a0-a1,-(sp)
        move.w  fat_fatstart(pc),d2
        moveq   #2-1,d4             ; both FAT copies
.fat:   move.w  fat_spf(pc),d3
        cmp.w   #3,d3
        ble     .n
        moveq   #3,d3
.n:     subq.w  #1,d3
        lea     fat_buf(pc),a1
.sec:   move.w  d2,d0
        and.l   #$FFFF,d0
        bsr     fdd_write_sector
        tst.w   d0
        bmi     .err
        addq.w  #1,d2
        lea     512(a1),a1
        dbra    d3,.sec
        dbra    d4,.fat
        moveq   #0,d0
        bra     .out
.err:   moveq   #-1,d0
.out:   movem.l (sp)+,d1-d4/a0-a1
        rts

; fat_write_data - d0 = first cluster, d1 = length, a1 = source
; -> d0 = 0 / -1 (zero-pads the final sector)
fat_write_data:
        movem.l d1-d7/a0-a2,-(sp)
        move.w  d0,d4               ; cluster
        move.l  d1,d5               ; remaining
.clust: cmp.w   #2,d4
        blt     .done
        cmp.w   #$0FF8,d4
        bge     .done
        move.w  d4,d0
        subq.w  #2,d0
        mulu    fat_spc(pc),d0
        add.w   fat_datastart(pc),d0
        move.w  fat_spc(pc),d6
.sec:   ; build a 512-byte sector in FATSECBUF (handles the short tail)
        movem.l d0,-(sp)
        lea     FATSECBUF,a2
        move.w  #511,d2
.fill:  tst.l   d5
        ble     .pad
        move.b  (a1)+,(a2)+
        subq.l  #1,d5
        bra     .nf
.pad:   clr.b   (a2)+
.nf:    dbra    d2,.fill
        movem.l (sp)+,d0
        movem.l d0/a1,-(sp)
        and.l   #$FFFF,d0
        lea     FATSECBUF,a1
        bsr     fdd_write_sector
        move.l  d0,d7
        movem.l (sp)+,d0/a1
        tst.w   d7
        bmi     .err
        addq.w  #1,d0
        subq.w  #1,d6
        bne     .sec
        move.w  d4,d0
        bsr     fat_next_cluster
        move.w  d0,d4
        tst.l   d5
        bgt     .clust
.done:  moveq   #0,d0
        bra     .out
.err:   moveq   #-1,d0
.out:   movem.l (sp)+,d1-d7/a0-a2
        rts

; fat_save_file - a0 = 11-char 8.3 name, a1 = data, d1 = length
; -> d0 = 0 ok / -1. Overwrites an existing entry or claims a free slot.
fat_save_file:
        movem.l d1-d7/a0-a3,-(sp)
        move.l  a0,a3               ; name
        move.l  a1,-(sp)            ; data
        move.l  d1,-(sp)            ; length
        ; scan the root directory for the name or the first free slot
        move.w  fat_rootstart(pc),d6
        moveq   #-1,d7              ; free-slot sector (none yet)
        moveq   #0,d5               ; free-slot offset
.sect:  cmp.w   fat_datastart(pc),d6
        bge     .nomatch
        move.w  d6,d0
        and.l   #$FFFF,d0
        lea     FATSECBUF,a1
        bsr     fdd_read_sector
        tst.w   d0
        bmi     .errpop2
        lea     FATSECBUF,a0
        moveq   #0,d4               ; offset within sector
.ent:   move.b  (a0,d4.w),d0
        beq     .freeslot           ; end-of-dir: usable as a free slot
        cmp.b   #$E5,d0
        beq     .freeslot2
        ; name match?
        movem.l d4/a0,-(sp)
        lea     (a0,d4.w),a0
        moveq   #10,d1
        moveq   #0,d2
.cmp:   move.b  (a0,d1.w),d0
        cmp.b   (a3,d1.w),d0
        bne     .nem
        dbra    d1,.cmp
        moveq   #1,d2
.nem:   movem.l (sp)+,d4/a0
        tst.w   d2
        bne     .found
        bra     .nextent
.freeslot:
.freeslot2:
        tst.w   d7
        bpl     .nextent            ; already have one
        move.w  d6,d7
        move.w  d4,d5
.nextent:
        add.w   #32,d4
        cmp.w   #512,d4
        blt     .ent
        addq.w  #1,d6
        bra     .sect
.found: ; existing entry at sector d6 offset d4: free its old chain
        moveq   #0,d0
        move.b  27(a0,d4.w),d0
        lsl.w   #8,d0
        move.b  26(a0,d4.w),d0
        bsr     fat_free_chain
        bra     .write
.nomatch:
        tst.w   d7
        bmi     .errpop2            ; directory full
        move.w  d7,d6               ; use the recorded free slot
        move.w  d5,d4
        ; reload that sector
        move.w  d6,d0
        and.l   #$FFFF,d0
        lea     FATSECBUF,a1
        bsr     fdd_read_sector
        tst.w   d0
        bmi     .errpop2
        lea     FATSECBUF,a0
.write: ; allocate the new chain
        move.l  (sp),d1             ; length
        move.l  d1,d0
        add.l   #1023,d0
        moveq   #10,d2
        lsr.l   d2,d0               ; / 1024 (spc=2 fixed for our media)
        tst.l   d0
        bne     .n0
        moveq   #1,d0
.n0:    bsr     fat_alloc_chain
        tst.w   d0
        bmi     .errpop2
        move.w  d0,d3               ; first cluster
        ; build the directory entry
        lea     (a0,d4.w),a2
        moveq   #10,d1
.nm:    move.b  (a3,d1.w),(a2,d1.w)
        dbra    d1,.nm
        move.b  #$20,11(a2)         ; archive
        moveq   #12,d1
.z:     clr.b   11(a2,d1.w)
        addq.w  #1,d1
        cmp.w   #15,d1
        blt     .z
        clr.l   12(a2)
        clr.l   16(a2)
        clr.l   20(a2)
        clr.l   24(a2)
        clr.l   28(a2)
        ; first cluster (LE)
        move.b  d3,26(a2)
        move.w  d3,d1
        lsr.w   #8,d1
        move.b  d1,27(a2)
        ; size (LE32)
        move.l  (sp),d1
        move.b  d1,28(a2)
        lsr.l   #8,d1
        move.b  d1,29(a2)
        lsr.l   #8,d1
        move.b  d1,30(a2)
        lsr.l   #8,d1
        move.b  d1,31(a2)
        ; write the directory sector back
        move.w  d6,d0
        and.l   #$FFFF,d0
        lea     FATSECBUF,a1
        bsr     fdd_write_sector
        tst.w   d0
        bmi     .errpop2
        ; write the data and flush the FAT
        move.l  (sp)+,d1            ; length
        move.l  (sp)+,a1            ; data
        move.w  d3,d0
        bsr     fat_write_data
        tst.w   d0
        bmi     .err
        bsr     fat_flush
        tst.w   d0
        bmi     .err
        moveq   #0,d0
        bra     .out
.errpop2:
        addq.l  #8,sp
.err:   moveq   #-1,d0
.out:   movem.l (sp)+,d1-d7/a0-a3
        rts

; fat_find_file - a0 = 11-char name -> d0 = first cluster / -1, d1 = size
fat_find_file:
        movem.l d2-d3/a1-a2,-(sp)
        move.w  fat_count(pc),d2
        beq     .nf
        subq.w  #1,d2
        lea     fat_tab(pc),a1
.e:     moveq   #10,d3
.c:     move.b  (a1,d3.w),d0
        cmp.b   (a0,d3.w),d0
        bne     .ne
        dbra    d3,.c
        ; match
        moveq   #0,d0
        move.w  16(a1),d0
        move.l  12(a1),d1
        bra     .out
.ne:    lea     18(a1),a1
        dbra    d2,.e
.nf:    moveq   #-1,d0
        moveq   #0,d1
.out:   movem.l (sp)+,d2-d3/a1-a2
        rts
