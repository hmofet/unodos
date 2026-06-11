; ============================================================================
; UnoDOS/68K trackdisk driver - raw MFM track reads from DF1 (the data
; drive; DF0 holds the boot disk). Standard Amiga DD format: 11 sectors
; x 512 bytes per track side, $4489 word-sync, odd/even MFM longwords.
;
;   fdd_read_sector  d0 = LBA (0-1759), a1 = 512-byte dest -> d0 = 0 ok
;   fdd_invalidate   drop the track cache (after disk change)
;
; A full side (11 sectors) is decoded per physical read; repeat hits on
; the same track are served from the cache.
; ============================================================================

DSKPTH      equ $020
DSKLEN      equ $024
DSKSYNC     equ $07E
ADKCON      equ $09E
INTREQW     equ $09C
CIAB_PRB    equ $BFD100

FDD_SPT     equ 11
RAWBUF      equ $6A000              ; raw MFM DMA target (chip RAM)
RAWWORDS    equ $1900               ; read: 12800 raw bytes (track + slack)
WRWORDS     equ 6200                ; write: must fit one revolution (~6250)
TRKCACHE    equ $6E000              ; 11 x 512 decoded sectors

FDD_SELBIT  equ 4                   ; CIA-B PRB: SEL1 = DF1 (active low)
; CIA-B PRB: 7 MTR, 6-3 SEL3-0, 2 SIDE, 1 DIR, 0 STEP (all active low)
; CIA-A PRA: 5 RDY, 4 TK0, 3 WPRO, 2 CHNG (active low)

; fdd_delay - d0 = vblank ticks to busy-wait (vblank ISR is running)
fdd_delay:
        movem.l d0-d1,-(sp)
        move.l  ticks(pc),d1
        add.l   d0,d1
.w:     move.l  ticks(pc),d0
        cmp.l   d1,d0
        blt     .w
        movem.l (sp)+,d0-d1
        rts

; fdd_init - the boot loader leaves DF0 selected with its motor on;
; latch every drive's motor off and deselect them all, or DF0 answers
; our DF1 DMA alongside it and corrupts reads/writes.
fdd_init:
        movem.l d0-d1,-(sp)
        bset    #7,CIAB_PRB         ; MTR = off level
        moveq   #3,d0               ; SEL0..SEL3 = bits 3-6
.drv:   bclr    d0,CIAB_PRB         ; select: latches motor off
        moveq   #20,d1
.dly:   nop
        dbra    d1,.dly
        bset    d0,CIAB_PRB         ; deselect
        addq.w  #1,d0
        cmp.w   #7,d0
        blt     .drv
        movem.l (sp)+,d0-d1
        rts

fdd_select:
        bclr    #FDD_SELBIT,CIAB_PRB
        rts
fdd_deselect:
        bset    #FDD_SELBIT,CIAB_PRB
        rts

; fdd_motor_on -> d0 = 0 ok / -1 not ready (no disk / no drive)
fdd_motor_on:
        move.l  d1,-(sp)
        bsr     fdd_deselect
        bclr    #7,CIAB_PRB         ; MTR on (latched on the select edge)
        bsr     fdd_select
        moveq   #100,d1             ; ~2s spin-up timeout
.rdy:   btst    #5,CIAA_PRA
        beq     .ok                 ; RDY is active low
        moveq   #2,d0
        bsr     fdd_delay
        subq.w  #1,d1
        bne     .rdy
        moveq   #-1,d0
        bra     .out
.ok:    moveq   #0,d0
.out:   move.l  (sp)+,d1
        rts

fdd_motor_off:
        bsr     fdd_deselect
        bset    #7,CIAB_PRB
        bsr     fdd_select
        bsr     fdd_deselect
        rts

; fdd_step - one head step in the direction already set on DIR
fdd_step:
        move.l  d0,-(sp)
        bclr    #0,CIAB_PRB
        moveq   #1,d0
        bsr     fdd_delay
        bset    #0,CIAB_PRB
        moveq   #1,d0
        bsr     fdd_delay
        move.l  (sp)+,d0
        rts

; fdd_seek - d0 = target cylinder (0-79) -> d0 = 0 ok / -1 fail
fdd_seek:
        movem.l d1-d2/a4,-(sp)
        move.w  d0,d2               ; target
        move.w  fdd_cyl(pc),d1
        bpl     .havepos
        ; recalibrate: step out until TK0 (active low)
        bset    #1,CIAB_PRB         ; DIR = out (towards track 0)
        moveq   #90,d1
.recal: btst    #4,CIAA_PRA
        beq     .at0
        bsr     fdd_step
        subq.w  #1,d1
        bne     .recal
        moveq   #-1,d0
        bra     .out
.at0:   lea     vars(pc),a4
        clr.w   fdd_cyl-vars(a4)
        moveq   #0,d1
.havepos:
        cmp.w   d1,d2
        beq     .settle
        bgt     .stepin
        ; step out (towards 0)
        bset    #1,CIAB_PRB
.outl:  bsr     fdd_step
        subq.w  #1,d1
        cmp.w   d1,d2
        bne     .outl
        bra     .store
.stepin:
        bclr    #1,CIAB_PRB         ; DIR = in (higher cylinders)
.inl:   bsr     fdd_step
        addq.w  #1,d1
        cmp.w   d1,d2
        bne     .inl
.store: lea     vars(pc),a4
        move.w  d2,fdd_cyl-vars(a4)
.settle:
        moveq   #2,d0
        bsr     fdd_delay           ; head settle
        moveq   #0,d0
.out:   movem.l (sp)+,d1-d2/a4
        rts

; fdd_dma_track - raw-read the currently selected track side into RAWBUF
; -> d0 = 0 ok / -1 timeout
fdd_dma_track:
        movem.l d1-d2/a6,-(sp)
        lea     CUSTOM,a6
        move.w  #$4000,DSKLEN(a6)   ; DMA off
        move.w  #$7F00,ADKCON(a6)   ; clear disk ADK bits
        move.w  #$9500,ADKCON(a6)   ; SETCLR | FAST | MFMPREC | WORDSYNC
        move.w  #$4489,DSKSYNC(a6)
        move.w  #2,INTREQW(a6)      ; clear DSKBLK
        move.w  #$8210,DMACON(a6)   ; SETCLR | DMAEN | DSKEN
        move.l  #RAWBUF,DSKPTH(a6)
        move.w  #$8000+RAWWORDS,DSKLEN(a6)
        move.w  #$8000+RAWWORDS,DSKLEN(a6)  ; second write starts DMA
        ; wait for DSKBLK, ~2s timeout
        move.l  ticks(pc),d2
        add.l   #100,d2
.wait:  move.w  INTREQR(a6),d1
        btst    #1,d1
        bne     .done
        move.l  ticks(pc),d1
        cmp.l   d2,d1
        blt     .wait
        move.w  #$4000,DSKLEN(a6)
        moveq   #-1,d0
        bra     .out
.done:  move.w  #$4000,DSKLEN(a6)
        move.w  #2,INTREQW(a6)
        moveq   #0,d0
.out:   movem.l (sp)+,d1-d2/a6
        rts

; fdd_decode_track - d2 = expected track number (0-159): decode the 11
; sectors from RAWBUF into TRKCACHE -> d0 = 0 ok / -1 incomplete
fdd_decode_track:
        movem.l d1-d7/a0-a2,-(sp)
        moveq   #0,d7               ; found-sector bitmask
        lea     RAWBUF,a0
        lea     RAWBUF+RAWWORDS*2-1200,a2   ; scan limit (room for a sector)
.scan:  cmp.l   a2,a0
        bge     .donescan
        cmp.w   #$4489,(a0)+
        bne     .scan
        ; skip any further sync words
.syncs: cmp.w   #$4489,(a0)
        bne     .info
        addq.l  #2,a0
        bra     .syncs
.info:  ; info longword: odd then even
        move.l  (a0)+,d0
        move.l  (a0)+,d1
        and.l   #$55555555,d0
        and.l   #$55555555,d1
        add.l   d0,d0
        or.l    d1,d0               ; $FF TT SS SG (fmt/track/sector/gap)
        rol.l   #8,d0
        cmp.b   #$FF,d0
        bne     .scan               ; not an Amiga sector header
        rol.l   #8,d0
        moveq   #0,d4
        move.b  d0,d4               ; d4 = track number
        rol.l   #8,d0
        moveq   #0,d5
        move.b  d0,d5               ; d5 = sector number (0-10)
        cmp.w   d2,d4
        bne     .scan               ; wrong track: stale read, keep scanning
        cmp.w   #FDD_SPT,d5
        bge     .scan
        ; skip label (8 longs) + header checksum (2) + data checksum (2)
        lea     48(a0),a0
        ; decode 512 data bytes: 128 odd longs, then 128 even longs
        move.w  d5,d6
        lsl.w   #8,d6
        add.w   d6,d6               ; * 512
        lea     TRKCACHE,a1
        add.w   d6,a1
        moveq   #127,d6
.data:  move.l  (a0),d0
        move.l  512(a0),d1
        addq.l  #4,a0
        and.l   #$55555555,d0
        and.l   #$55555555,d1
        add.l   d0,d0
        or.l    d1,d0
        move.l  d0,(a1)+
        dbra    d6,.data
        lea     512(a0),a0          ; skip the even half
        bset    d5,d7
        cmp.w   #$07FF,d7
        bne     .scan
.donescan:
        cmp.w   #$07FF,d7           ; all 11 sectors?
        beq     .ok
        moveq   #-1,d0
        bra     .out
.ok:    moveq   #0,d0
.out:   movem.l (sp)+,d1-d7/a0-a2
        rts

; fdd_read_track - d0 = track (0-159): DMA + decode into TRKCACHE
; -> d0 = 0 ok / -1 error
fdd_read_track:
        movem.l d1-d3/a4,-(sp)
        move.w  d0,d3               ; track
        bsr     fdd_motor_on
        tst.w   d0
        bmi     .out
        ; side: even track = lower head (SIDE high), odd = upper (SIDE low)
        btst    #0,d3
        beq     .side0
        bclr    #2,CIAB_PRB
        bra     .seek
.side0: bset    #2,CIAB_PRB
.seek:  move.w  d3,d0
        lsr.w   #1,d0               ; cylinder
        bsr     fdd_seek
        tst.w   d0
        bmi     .out
        ; two attempts at DMA+decode (d1 = retry counter)
        moveq   #1,d1
.try:   bsr     fdd_dma_track
        tst.w   d0
        bmi     .retry
        move.w  d3,d2               ; expected track for the decoder
        bsr     fdd_decode_track
        tst.w   d0
        bpl     .gotit
.retry: dbra    d1,.try
        moveq   #-1,d0
        bra     .out
.gotit: lea     vars(pc),a4
        move.w  d3,fdd_ctrack-vars(a4)
        moveq   #0,d0
.out:   movem.l (sp)+,d1-d3/a4
        rts

; fdd_read_sector - d0 = LBA (0-1759), a1 = dest (512 bytes)
; -> d0 = 0 ok / -1 error
fdd_read_sector:
        movem.l d1-d3/a0-a1,-(sp)
        and.l   #$FFFF,d0
        divu    #FDD_SPT,d0
        move.l  d0,d3
        swap    d3                  ; d3 = sector within track
        ; d0.w = track
        cmp.w   fdd_ctrack(pc),d0
        beq     .cached
        bsr     fdd_read_track
        tst.w   d0
        bmi     .out
.cached:
        and.l   #$FFFF,d3
        lsl.w   #8,d3
        add.w   d3,d3               ; * 512
        lea     TRKCACHE,a0
        add.w   d3,a0
        moveq   #127,d1
.cp:    move.l  (a0)+,(a1)+
        dbra    d1,.cp
        moveq   #0,d0
.out:   movem.l (sp)+,d1-d3/a0-a1
        rts

; ============================================================================
; Write path: MFM-encode the 11 cached sectors and write the whole track.
; ============================================================================

; fdd_enc_long - d0 = 32 data bits -> writes odd then even MFM longs at a1+
; d6 = last data bit of the previous encoded long (0/1), updated on exit.
; MFM: data in the $55555555 positions, clock c(n)=1 iff both neighbour
; data bits are 0. Internal clocks from the shift formula; the boundary
; clock (bit31) is patched from d6.
fdd_enc_long:
        movem.l d0-d3,-(sp)
        move.l  d0,d2
        lsr.l   #1,d2
        and.l   #$55555555,d2       ; odd data bits -> data positions
        bsr     fdd_enc_half
        move.l  (sp),d0             ; reload original
        move.l  d0,d2
        and.l   #$55555555,d2       ; even data bits
        bsr     fdd_enc_half
        movem.l (sp)+,d0-d3
        rts

; fdd_enc_half - d2 = data in $55555555 positions: emit one encoded long
; at (a1)+, maintaining d6 = last data bit (bit0 of d2 afterwards)
fdd_enc_half:
        movem.l d1/d3,-(sp)         ; callers use d1/d3 as loop counters
        move.l  d2,d1
        lsl.l   #1,d1               ; left-neighbour data bits
        move.l  d2,d3
        lsr.l   #1,d3               ; right-neighbour data bits
        or.l    d2,d1
        or.l    d3,d1               ; any neighbouring data set?
        not.l   d1
        and.l   #$AAAAAAAA,d1       ; clock bits
        or.l    d2,d1               ; encoded long
        ; boundary clock at bit31: 1 iff prev data bit == 0 and bit30 == 0
        tst.w   d6
        beq     .prev0
        bclr    #31,d1              ; previous data bit set: no clock
        bra     .store
.prev0: btst    #30,d2
        bne     .noclk
        bset    #31,d1
        bra     .store
.noclk: bclr    #31,d1
.store: move.l  d1,(a1)+
        moveq   #0,d6
        btst    #0,d2
        beq     .out
        moveq   #1,d6
.out:   movem.l (sp)+,d1/d3
        rts

; fdd_encode_track - d2 = track number: build the raw MFM track image
; from TRKCACHE into RAWBUF (gap + 11 sectors, Amiga trackdisk format)
fdd_encode_track:
        movem.l d0-d7/a0-a2,-(sp)
        lea     RAWBUF,a1
        ; lead-in gap
        move.w  #64-1,d0
.gap:   move.w  #$AAAA,(a1)+
        dbra    d0,.gap
        moveq   #0,d6               ; previous data bit
        moveq   #0,d7               ; sector number
.sect:  ; pre-sync zeros + two sync words
        move.w  #$AAAA,(a1)+
        move.w  #$AAAA,(a1)+
        move.w  #$4489,(a1)+
        move.w  #$4489,(a1)+
        moveq   #1,d6               ; sync word ends in a data 1 bit
        ; info longword: $FF, track, sector, sectors-until-gap
        move.l  #$FF000000,d0
        moveq   #0,d1
        move.w  d2,d1
        lsl.l   #8,d1
        or.w    d7,d1               ; 00 00 TT SS
        lsl.l   #8,d1
        moveq   #FDD_SPT,d3
        sub.w   d7,d3
        or.b    d3,d1               ; 00 TT SS SG
        or.l    d1,d0               ; FF TT SS SG
        move.l  a1,a2               ; remember: info starts here
        bsr     fdd_enc_long
        ; label: 4 zero longs -> 8 encoded
        moveq   #3,d3
.lbl:   moveq   #0,d0
        bsr     fdd_enc_long
        dbra    d3,.lbl
        ; header checksum: XOR of the 10 encoded longs so far, & $55555555
        movem.l d2/a1,-(sp)
        moveq   #0,d0
        moveq   #9,d3
.hck:   move.l  (a2)+,d1
        eor.l   d1,d0
        dbra    d3,.hck
        and.l   #$55555555,d0
        movem.l (sp)+,d2/a1
        bsr     fdd_enc_long
        ; data checksum: computed over the encoded data - encode the data
        ; AFTER the checksum slot, so reserve it and backfill.
        move.l  a1,a2               ; a2 = checksum slot
        moveq   #0,d0
        bsr     fdd_enc_long        ; placeholder (updates d6 as if zero)
        ; encode 512 data bytes: odd halves then even halves
        move.w  d2,-(sp)            ; track number (the loops reuse d2)
        move.l  a1,-(sp)            ; start of encoded data
        ; source: TRKCACHE + sector*512
        move.w  d7,d0
        lsl.w   #8,d0
        add.w   d0,d0
        lea     TRKCACHE,a0
        add.w   d0,a0
        ; pass 1: odd bits of each long
        movem.l a0,-(sp)
        moveq   #127,d3
.dod:   move.l  (a0)+,d0
        lsr.l   #1,d0
        and.l   #$55555555,d0
        move.l  d0,d2
        bsr     fdd_enc_half
        dbra    d3,.dod
        movem.l (sp)+,a0
        ; pass 2: even bits
        moveq   #127,d3
.dev:   move.l  (a0)+,d0
        and.l   #$55555555,d0
        move.l  d0,d2
        bsr     fdd_enc_half
        dbra    d3,.dev
        ; data checksum = XOR of the 256 encoded longs & $55555555
        move.l  (sp)+,a0            ; encoded data start
        moveq   #0,d0
        move.w  #255,d3
.dck:   move.l  (a0)+,d1
        eor.l   d1,d0
        dbra    d3,.dck
        and.l   #$55555555,d0
        ; backfill the checksum slot (re-encode with the pre-slot bit
        ; context; safe because the slot's neighbours are already fixed:
        ; clock fixup below repairs the single boundary bit)
        movem.l a1/d6,-(sp)
        move.l  a2,a1
        moveq   #1,d6               ; boundary repaired by the fixup pass
        bsr     fdd_enc_long
        movem.l (sp)+,a1/d6
        move.w  (sp)+,d2            ; track number back
        addq.w  #1,d7
        cmp.w   #FDD_SPT,d7
        blt     .sect
        ; trailing gap to the end of the WRITE window (one revolution)
.tgap:  cmp.l   #RAWBUF+WRWORDS*2,a1
        bge     .fix
        move.w  #$AAAA,(a1)+
        bra     .tgap
.fix:   ; clock-fixup pass: bit31 of every long after the first, from the
        ; previous long's bit0 (covers all the spots stitched above)
        lea     RAWBUF,a0
        move.w  #WRWORDS/2-2,d3
        move.l  (a0)+,d1            ; previous long
.fx:    move.l  (a0),d0
        btst    #0,d1
        beq     .p0
        bclr    #31,d0
        bra     .fs
.p0:    btst    #30,d0
        bne     .fc
        bset    #31,d0
        bra     .fs
.fc:    bclr    #31,d0
.fs:    move.l  d0,(a0)             ; sync words keep bit31=0 naturally
        move.l  (a0)+,d1
        dbra    d3,.fx
        movem.l (sp)+,d0-d7/a0-a2
        rts

; fdd_write_track - d0 = track: encode TRKCACHE and write the full track
; -> d0 = 0 ok / -1 error (incl. write-protected)
fdd_write_track:
        movem.l d1-d3/a6,-(sp)
        move.w  d0,d3
        bsr     fdd_motor_on
        tst.w   d0
        bmi     .out
        btst    #3,CIAA_PRA         ; WPRO (active low)
        beq     .wprot
        btst    #0,d3
        beq     .side0
        bclr    #2,CIAB_PRB
        bra     .seek
.side0: bset    #2,CIAB_PRB
.seek:  move.w  d3,d0
        lsr.w   #1,d0
        bsr     fdd_seek
        tst.w   d0
        bmi     .out
        move.w  d3,d2
        bsr     fdd_encode_track
        ; write DMA
        lea     CUSTOM,a6
        move.w  #$4000,DSKLEN(a6)
        move.w  #$7F00,ADKCON(a6)
        move.w  #$9100,ADKCON(a6)   ; SETCLR | FAST | PRECOMP 140ns
        move.w  #2,INTREQW(a6)
        move.w  #$8210,DMACON(a6)
        move.l  #RAWBUF,DSKPTH(a6)
        move.w  #$C000+WRWORDS,DSKLEN(a6)
        move.w  #$C000+WRWORDS,DSKLEN(a6)   ; bit14 = WRITE
        move.l  ticks(pc),d2
        add.l   #150,d2
.wait:  move.w  INTREQR(a6),d1
        btst    #1,d1
        bne     .done
        move.l  ticks(pc),d1
        cmp.l   d2,d1
        blt     .wait
        move.w  #$4000,DSKLEN(a6)
        moveq   #-1,d0
        bra     .out
.done:  move.w  #$4000,DSKLEN(a6)
        move.w  #2,INTREQW(a6)
        moveq   #0,d0
        bra     .out
.wprot: moveq   #-1,d0
.out:   movem.l (sp)+,d1-d3/a6
        rts

; fdd_write_sector - d0 = LBA, a1 = 512-byte source -> d0 = 0 ok / -1
; (read-modify-write at track granularity through the cache)
fdd_write_sector:
        movem.l d1-d3/a0-a1,-(sp)
        move.l  a1,-(sp)
        and.l   #$FFFF,d0
        divu    #FDD_SPT,d0
        move.l  d0,d3
        swap    d3                  ; sector within track
        move.w  d0,d2               ; track
        cmp.w   fdd_ctrack(pc),d0
        beq     .have
        bsr     fdd_read_track
        tst.w   d0
        bmi     .errpop
.have:  ; copy source into the cache
        move.l  (sp)+,a1
        and.l   #$FFFF,d3
        lsl.w   #8,d3
        add.w   d3,d3
        lea     TRKCACHE,a0
        add.w   d3,a0
        moveq   #127,d1
.cp:    move.l  (a1)+,(a0)+
        dbra    d1,.cp
        move.w  d2,d0
        bsr     fdd_write_track
        bra     .out
.errpop:
        move.l  (sp)+,a1
        moveq   #-1,d0
.out:   movem.l (sp)+,d1-d3/a0-a1
        rts

; fdd_invalidate - drop the cache (disk change / refresh)
fdd_invalidate:
        move.l  a4,-(sp)
        lea     vars(pc),a4
        move.w  #-1,fdd_ctrack-vars(a4)
        move.w  #-1,fdd_cyl-vars(a4)
        move.l  (sp)+,a4
        rts
