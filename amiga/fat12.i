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
