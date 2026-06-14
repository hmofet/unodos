; ============================================================================
; UnoDOS/AppleII kernel RWTS (milestone 2) - read AND write a single logical
; 256-byte sector via the Disk II controller's GCR data latch. Ported from
; boot.s's seek/readsec (read side; boot.s's RWTS lives in the now-reclaimed
; $0800-$17FF boot image, so the kernel needs its own copy) plus a new
; 6-and-2 encoder for the write side (the mathematical inverse of the read
; decode - see harness.py's nibblize_data, the executable spec).
;
; Calling convention:
;   rwts_init                 - call once at boot; builds DECTAB from WRTAB
;   zpTrkWant = track, A = logical sector (0-15), zpBuf/zpBuf+1 = buffer ptr
;   jsr rwts_read              - read into the buffer
;   jsr rwts_write             - write from the buffer
;
; Checksums are not verified on read, matching boot.s (trusted FloppyEmu/
; AppleWin media). $F1 (zpTrkCur) stays the harness's DiskII-track-select ABI
; (HANDOFF.md SS6 / HANDOFF-M2 SS1) - do not move it.
; ============================================================================

; ---- zero page (inherits boot.s's $F0-$FF; kernel.s renderer owns $00-$2D) ----
zpSlot   equ $F0   ; slot*16, set by boot.s entry: and preserved across jmp $4000
zpTrkCur equ $F1   ; ABI: harness DiskII.read_data selects the track from here
zpTrkWant equ $F2  ; rwts_seek target track
zpSec    equ $F3   ; physical sector under search
zpBuf    equ $F4   ; (2) caller's 256-byte sector buffer
zpRTmp    equ $F6   ; running XOR ("prev") / general scratch
zpRTmp2   equ $F7   ; rd44 scratch
zpRetry  equ $F8   ; address-field hunt retry counter
zpRI      equ $F9   ; loop index
zpTrkRd  equ $FD   ; track read from the address field (validation)

; ---- BSS (above KBSS, not part of the assembled image - see kernel.s) ----
; KBSS = $9000 (M3); offsets below are unchanged, addresses just shift up.
DECTAB   equ KBSS        ; $9000-$90FF: 256-byte 6-and-2 decode table
RDBUF2   equ KBSS+$100   ; $9100-$9155: 86-byte "twos" buffer
SECBUF   equ KBSS+$200   ; $9200-$92FF: general-purpose sector I/O buffer
SECBUF2  equ KBSS+$300   ; $9300-$93FF: second buffer (AUTOTEST compare)

; ---------------------------------------------------------------- rwts_init
; rwts_init - build DECTAB[WRTAB[y]] = y for y=0..63 (boot.s's bdt loop).
rwts_init:
        ldy #0
ri_loop:
        lda WRTAB,y
        tax
        tya
        sta DECTAB,x
        iny
        cpy #64
        bne ri_loop
        rts

; ----------------------------------------------------------------- rwts_seek
; rwts_seek - step the head from zpTrkCur to zpTrkWant (2 half-steps/track).
rwts_seek:
        lda zpTrkCur
        cmp zpTrkWant
        beq rs_done
        bcc rs_up
        jsr rs_phasedn
        dec zpTrkCur
        jmp rwts_seek
rs_up:  jsr rs_phaseup
        inc zpTrkCur
        jmp rwts_seek
rs_done:
        rts

rs_phaseup:
        lda zpTrkCur
        asl
        jsr rs_phon1
        lda zpTrkCur
        asl
        clc
        adc #2
        jsr rs_phon1
        rts
rs_phasedn:
        lda zpTrkCur
        asl
        sec
        sbc #1
        jsr rs_phon1
        lda zpTrkCur
        asl
        sec
        sbc #2
        jsr rs_phon1
        rts
; rs_phon1 - A = half-track position: pulse that stepper phase on, delay, off
rs_phon1:
        and #3
        asl
        ora zpSlot
        tax
        lda $C081,x
        jsr rs_stepdel
        lda $C080,x
        rts
; rs_stepdel - ~20ms at 1MHz (the conservative per-half-step settle)
rs_stepdel:
        ldy #40
rsd1:   ldx #100
rsd2:   dex
        bne rsd2
        dey
        bne rsd1
        rts

; ----------------------------------------------------------------- rwts_rd44
; rwts_rd44 - read an odd-even 4-and-4 encoded byte -> A (boot.s's rd44).
rwts_rd44:
        ldx zpSlot
rd44_1: lda $C08C,x
        bpl rd44_1
        sec
        rol                     ; (b<<1)|1
        sta zpRTmp2
rd44_2: lda $C08C,x
        bpl rd44_2
        and zpRTmp2
        rts

; ----------------------------------------------------------------- rwts_read
; rwts_read - A = logical sector (0-15). Seeks to zpTrkWant, hunts for the
; address field of the corresponding physical sector (SKEW_INV[A]), then
; hunts the data field and 6-and-2 decodes 256 bytes into zpBuf/zpBuf+1.
rwts_read:
        tax
        lda SKEW_INV,x
        sta zpSec               ; physical sector to hunt for
        jsr rwts_seek
rwr_retry:
        ldx zpSlot
        lda $C089,x             ; motor on (idempotent)
        lda $C08A,x             ; drive 1 select
        lda $C08E,x             ; Q7 off: read mode
        lda $C08C,x
        lda #48
        sta zpRetry             ; ~48 field attempts then re-init and retry
rwr_huntaddr:
        dec zpRetry
        beq rwr_retry
rwr_a1: lda $C08C,x
        bpl rwr_a1
        cmp #$D5
        bne rwr_huntaddr
rwr_a2: lda $C08C,x
        bpl rwr_a2
        cmp #$AA
        bne rwr_huntaddr
rwr_a3: lda $C08C,x
        bpl rwr_a3
        cmp #$96
        bne rwr_huntaddr
        jsr rwts_rd44           ; volume (ignored)
        jsr rwts_rd44           ; track
        sta zpTrkRd
        jsr rwts_rd44           ; sector
        sta zpRTmp
        jsr rwts_rd44           ; checksum (not verified)
        lda zpTrkRd
        cmp zpTrkWant
        bne rwr_huntaddr
        lda zpRTmp
        cmp zpSec
        bne rwr_huntaddr

; ---- hunt the data field: D5 AA AD (must follow within ~32 nibbles) ----
        ldy #32
rwr_hd0: dey
        beq rwr_huntaddr
rwr_hd1: lda $C08C,x
        bpl rwr_hd1
        cmp #$D5
        bne rwr_hd0
rwr_hd2: lda $C08C,x
        bpl rwr_hd2
        cmp #$AA
        bne rwr_hd0
rwr_hd3: lda $C08C,x
        bpl rwr_hd3
        cmp #$AD
        bne rwr_hd0

; ---- 342 nibbles: 86 "twos" into RDBUF2 (descending), 256 "sixes" into
; zpBuf (ascending), running EOR chain (zpRTmp) ----
        ldy #86
        lda #0
        sta zpRTmp
rwr_t1: sty zpRI
rwr_tw1: lda $C08C,x
        bpl rwr_tw1
        tay
        lda DECTAB,y
        eor zpRTmp
        sta zpRTmp
        ldy zpRI
        sta RDBUF2-1,y
        dey
        bne rwr_t1

        ldy #0
rwr_s1: lda $C08C,x
        bpl rwr_s1
        sty zpRI
        tay
        lda DECTAB,y
        ldy zpRI
        eor zpRTmp
        sta zpRTmp
        sta (zpBuf),y
        iny
        bne rwr_s1

; data checksum nibble (not verified)
rwr_ck1: lda $C08C,x
        bpl rwr_ck1

; ---- combine: byte = sixes<<2 | two-bits (per-position group from RDBUF2) ----
        ldy #0
rwr_fix1:
        sty zpRI
        tya
        cmp #172
        bcs rwr_g2
        cmp #86
        bcs rwr_g1
        tax
        lda RDBUF2,x
        and #3
        tax
        lda FLIP2,x
        jmp rwr_fxc
rwr_g1: sec
        sbc #86
        tax
        lda RDBUF2,x
        lsr
        lsr
        and #3
        tax
        lda FLIP2,x
        jmp rwr_fxc
rwr_g2: sec
        sbc #172
        tax
        lda RDBUF2,x
        lsr
        lsr
        lsr
        lsr
        and #3
        tax
        lda FLIP2,x
rwr_fxc:
        sta zpRTmp
        ldy zpRI
        lda (zpBuf),y
        asl
        asl
        ora zpRTmp
        sta (zpBuf),y
        iny
        bne rwr_fix1
        rts

; ---------------------------------------------------------------- rwts_write
; rwts_write - A = logical sector (0-15). Seeks, builds the "twos" table
; from zpBuf, hunts for the target physical sector's address field (read
; mode), then switches to write mode (Q7 on via $C08F) and emits 5 self-sync
; $FF nibbles + D5 AA AD + 343 6-and-2 data nibbles (encoder: inverse of
; rwts_read's decode, see harness.py nibblize_data) + DE AA EB, then back to
; read mode (Q7 off via $C08E).
;
; Real-hardware note (HANDOFF-M2 SS1): every nibble must land ~32 cycles
; apart and self-sync nibbles use an extended ~40-cycle bit-cell; this loop
; is not yet cycle-counted for that. The harness validates the logical
; protocol only - AppleWin is the timing oracle before metal.
rwts_write:
        tax
        lda SKEW_INV,x
        sta zpSec
        jsr rwts_seek
        jsr rwts_buildrdbuf2

rww_retry:
        ldx zpSlot
        lda $C089,x             ; motor on
        lda $C08A,x             ; drive 1 select
        lda $C08E,x             ; Q7 off: read mode (for the address hunt)
        lda $C08C,x
        lda #48
        sta zpRetry
rww_huntaddr:
        dec zpRetry
        beq rww_retry
rww_a1: lda $C08C,x
        bpl rww_a1
        cmp #$D5
        bne rww_huntaddr
rww_a2: lda $C08C,x
        bpl rww_a2
        cmp #$AA
        bne rww_huntaddr
rww_a3: lda $C08C,x
        bpl rww_a3
        cmp #$96
        bne rww_huntaddr
        jsr rwts_rd44           ; volume (ignored)
        jsr rwts_rd44           ; track
        sta zpTrkRd
        jsr rwts_rd44           ; sector
        sta zpRTmp
        jsr rwts_rd44           ; checksum (not verified)
        lda zpTrkRd
        cmp zpTrkWant
        bne rww_huntaddr
        lda zpRTmp
        cmp zpSec
        bne rww_huntaddr

; ---- found target address field; switch to write mode ----
        lda $C08F,x             ; Q7 on -> write mode (harness begins capture)

        lda #$FF
        ldy #5
rww_sync:
        sta $C08D,x
        dey
        bne rww_sync

        lda #$D5
        sta $C08D,x
        lda #$AA
        sta $C08D,x
        lda #$AD
        sta $C08D,x

; ---- 86 "twos" nibbles, descending RDBUF2[85..0] ----
        lda #0
        sta zpRTmp               ; running "prev"
        ldy #85
rww_twos:
        lda RDBUF2,y
        eor zpRTmp
        tax
        lda WRTAB,x
        ldx zpSlot
        sta $C08D,x
        lda RDBUF2,y
        sta zpRTmp
        dey
        bpl rww_twos

; ---- 256 "sixes" nibbles, ascending zpBuf[0..255] ----
        ldy #0
rww_sixes:
        lda (zpBuf),y
        lsr
        lsr                     ; six = byte >> 2
        eor zpRTmp
        tax
        lda WRTAB,x
        ldx zpSlot
        sta $C08D,x
        lda (zpBuf),y
        lsr
        lsr
        sta zpRTmp
        iny
        bne rww_sixes

; ---- checksum nibble = WRTAB[prev] ----
        lda zpRTmp
        tax
        lda WRTAB,x
        ldx zpSlot
        sta $C08D,x

        lda #$DE
        sta $C08D,x
        lda #$AA
        sta $C08D,x
        lda #$EB
        sta $C08D,x

        lda $C08E,x             ; Q7 off -> read mode (harness commits capture)
        rts

; rwts_buildrdbuf2 - fill RDBUF2[0..85] from zpBuf per harness.py's
; nibblize_data: RDBUF2[j] = FLIP2[buf[j+172]&3]<<4 | FLIP2[buf[j+86]&3]<<2 |
; FLIP2[buf[j]&3], with the j+172 term 0 for j>=84 (buf[256]/[257] don't exist).
rwts_buildrdbuf2:
        ldx #0
rb_loop:
        stx zpRI
        ldy zpRI
        lda (zpBuf),y
        and #3
        tay
        lda FLIP2,y
        sta zpRTmp

        lda zpRI
        clc
        adc #86
        tay
        lda (zpBuf),y
        and #3
        tay
        lda FLIP2,y
        asl
        asl
        ora zpRTmp
        sta zpRTmp

        lda zpRI
        cmp #84
        bcs rb_nob54
        clc
        adc #172
        tay
        lda (zpBuf),y
        and #3
        tay
        lda FLIP2,y
        asl
        asl
        asl
        asl
        ora zpRTmp
        sta zpRTmp
rb_nob54:
        lda zpRTmp
        sta RDBUF2,x
        inx
        cpx #86
        bne rb_loop
        rts

; ---------------------------------------------------------------- tables
; SKEW_INV - logical sector -> physical sector (inverse of boot.s's skew,
; the DOS 3.3 sector-translation table: physical P holds logical SKEW[P]).
SKEW_INV:
        dc.b $0,$D,$B,$9,$7,$5,$3,$1,$E,$C,$A,$8,$6,$4,$2,$F

FLIP2:  dc.b 0,2,1,3                ; 2-bit groups are bit-reversed (boot.s)

; WRTAB - the 6-and-2 write translate table (62+2 valid disk nibbles, from
; boot.s; DECTAB is its inverse, built by rwts_init).
WRTAB:  dc.b $96,$97,$9A,$9B,$9D,$9E,$9F,$A6
        dc.b $A7,$AB,$AC,$AD,$AE,$AF,$B2,$B3
        dc.b $B4,$B5,$B6,$B7,$B9,$BA,$BB,$BC
        dc.b $BD,$BE,$BF,$CB,$CD,$CE,$CF,$D3
        dc.b $D6,$D7,$D9,$DA,$DB,$DC,$DD,$DE
        dc.b $DF,$E5,$E6,$E7,$E9,$EA,$EB,$EC
        dc.b $ED,$EE,$EF,$F2,$F3,$F4,$F5,$F6
        dc.b $F7,$F9,$FA,$FB,$FC,$FD,$FE,$FF
