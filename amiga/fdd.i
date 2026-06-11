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
RAWWORDS    equ $1900               ; 12800 raw bytes: track + slack
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

; fdd_invalidate - drop the cache (disk change / refresh)
fdd_invalidate:
        move.l  a4,-(sp)
        lea     vars(pc),a4
        move.w  #-1,fdd_ctrack-vars(a4)
        move.w  #-1,fdd_cyl-vars(a4)
        move.l  (sp)+,a4
        rts
