; ============================================================================
; UnoDOS/AppleII boot chain - track 0 of the 140K Disk II floppy.
;
; The Disk II controller ROM (the 256-byte 16-sector P5A at $Cs00) loads
; ONLY boot sector T0S0 to $0800 and jumps to $0801 with X = slot*16.
; Unlike the Mac ROM there are NO further firmware disk services - so this
; track carries our own read-RWTS. Protocol: byte 0 of this sector tells
; the controller ROM how many track-0 sectors to autoload ($10 = all 16,
; $0800-$17FF, the standard multi-sector boot used since DOS 3.2) before
; jumping to $0801. (If a clone ROM only honors 1 sector, this is the
; first thing to check in AppleWin - documented in the README.)
;
; Layout of the loaded 4KB:
;   $0800  boot0: entry, denibble-table build, track loader
;   $0A00+ rwts: GCR 6-and-2 sector read + head stepping
; The loader reads tracks 1..KTRACKS into $4000+ (16 sectors per track,
; logical order via the DOS skew table) and jumps to the kernel at $4000.
; mkdsk.py patches KTRACKS into the 'KT' placeholder.
;
; Build: dasm boot.s -f3 -oboot.bin   (raw 4096 bytes, track 0 image)
; ============================================================================

        processor 6502

KERNLOAD equ $4000                  ; kernel load target
RDBUF2   equ $1800                  ; 86-byte twos-buffer for denibbling
DECTAB   equ $1900                  ; 256-byte nibble decode table

; ---- zero page ----
zpSlot   equ $F0                    ; slot*16
zpTrkCur equ $F1                    ; head position (current track)
zpTrkWant equ $F2                   ; target track
zpSecMask equ $F3                   ; bitmask... (lo)
zpSecMaskH equ $F4                  ; (hi) sectors of this track still wanted
zpDst    equ $F6                    ; current page base (hi byte) of track dst
zpPtr    equ $F8                    ; data pointer (2)
zpTmp    equ $FA
zpSec    equ $FB                    ; physical sector from the address field
zpTrkRd  equ $FC                    ; track from the address field
zpRetry  equ $FD

        org $0800

; ---------------------------------------------------------------- boot0
        dc.b $10                    ; ROM autoload count: all 16 T0 sectors
entry:
        ; X = slot*16 (from the controller ROM)
        stx zpSlot
        ; build the 6-and-2 decode table: DECTAB[nibble] = 6-bit value
        ldy #0
bdt:    lda wrtab,y
        tax
        tya
        sta DECTAB,x
        iny
        cpy #64
        bne bdt
        lda #0
        sta zpTrkCur                ; ROM left the head on track 0
        ; load tracks 1..KTRACKS to KERNLOAD
        lda #>KERNLOAD
        sta zpDst
        lda #1
        sta zpTrkWant
nexttrk:
        jsr seek                    ; step the head to zpTrkWant
        lda #$FF
        sta zpSecMask
        sta zpSecMaskH              ; 16 sectors wanted
secloop:
        jsr readsec                 ; one (any) sector of this track ->
        ; page zpDst + skew[phys]; clears its bit in the mask
        lda zpSecMask
        ora zpSecMaskH
        bne secloop
        ; track done: dst += 16 pages, next track
        lda zpDst
        clc
        adc #16
        sta zpDst
        inc zpTrkWant
        lda zpTrkWant
ktpatch:
        cmp #$4B                    ; 'K' placeholder: tracks+1 (mkdsk patches)
        bcc nexttrk
        ; motor off, into the kernel
        ldx zpSlot
        lda $C088,x
        jmp KERNLOAD

; ---------------------------------------------------------------- seek
; seek - step the head from zpTrkCur to zpTrkWant (2 half-steps/track)
seek:
        lda zpTrkCur
        cmp zpTrkWant
        beq seekdone
        bcc seekup
        ; step down
        jsr phasedn
        dec zpTrkCur
        jmp seek
seekup: jsr phaseup
        inc zpTrkCur
        jmp seek
seekdone:
        rts

; phaseup/phasedn - one full track = two half-steps through the stepper
; phases. Phase for half-track h = h & 3; energize, wait, de-energize.
phaseup:
        lda zpTrkCur
        asl                         ; half-track = track*2
        jsr phon1                   ; ht+1
        lda zpTrkCur
        asl
        clc
        adc #2                      ; ht+2
        jsr phon1
        rts
phasedn:
        lda zpTrkCur
        asl
        sec
        sbc #1                      ; ht-1
        jsr phon1
        lda zpTrkCur
        asl
        sec
        sbc #2                      ; ht-2
        jsr phon1
        rts
; phon1 - A = half-track position: pulse that phase on, delay, off
phon1:
        and #3
        asl                         ; phase*2
        ora zpSlot
        tax
        lda $C081,x                 ; phase N on
        jsr stepdel
        lda $C080,x                 ; phase N off
        rts
; stepdel - ~20ms at 1MHz (the conservative per-half-step settle)
stepdel:
        ldy #40
sd1:    ldx #100
sd2:    dex
        bne sd2
        dey
        bne sd1
        rts

; ---------------------------------------------------------------- readsec
; readsec - read whichever sector passes under the head next on the
; current track. If its bit in zpSecMask/H is still set, denibble it to
; page (zpDst + skew[sector]) and clear the bit. GCR 6-and-2 per
; Beneath Apple DOS.
readsec:
        ldx zpSlot
        lda $C089,x                 ; motor on (idempotent)
        lda $C08A,x                 ; select drive 1
        lda $C08E,x                 ; Q7L: read mode
        lda $C08C,x                 ; Q6L
        lda #48
        sta zpRetry                 ; ~48 field attempts then give up (spin)
; ---- hunt for an address field: D5 AA 96
huntaddr:
        dec zpRetry
        beq readsec                 ; restart (keeps motor alive)
ha1:    lda $C08C,x
        bpl ha1
        cmp #$D5
        bne huntaddr
ha2:    lda $C08C,x
        bpl ha2
        cmp #$AA
        bne huntaddr
ha3:    lda $C08C,x
        bpl ha3
        cmp #$96
        bne huntaddr
        ; volume, track, sector, checksum as odd-even 4-and-4 pairs
        jsr rd44                    ; volume (ignored)
        jsr rd44
        sta zpTrkRd
        jsr rd44
        sta zpSec
        jsr rd44                    ; checksum (trusted media: skip verify)
        ; right track?
        lda zpTrkRd
        cmp zpTrkWant
        bne huntaddr
        ; sector still wanted?
        ldy zpSec
        cpy #8
        bcs maskhi
        lda bits,y
        and zpSecMask
        beq huntaddr                ; already have it
        bne huntdata
maskhi: lda bits-8,y
        and zpSecMaskH
        beq huntaddr
; ---- hunt for the data field: D5 AA AD (must follow within ~32 nibbles)
huntdata:
        ldy #32
hd0:    dey
        beq huntaddr
hd1:    lda $C08C,x
        bpl hd1
        cmp #$D5
        bne hd0
hd2:    lda $C08C,x
        bpl hd2
        cmp #$AA
        bne hd0
hd3:    lda $C08C,x
        bpl hd3
        cmp #$AD
        bne hd0
        ; ---- 342 nibbles: 86 "twos" + 256 "sixes", running EOR chain
        ; twos, descending into RDBUF2
        ldy #86
        lda #0
        sta zpTmp                   ; running checksum
t1:     sty zpPtr                   ; (y index juggling)
tw1:    lda $C08C,x
        bpl tw1
        tay
        lda DECTAB,y
        eor zpTmp
        sta zpTmp
        ldy zpPtr
        sta RDBUF2-1,y
        dey
        bne t1
        ; sixes, ascending into the destination page
        lda zpDst
        clc
        ldy zpSec
        sty secsave                 ; zpSec gets reused as a byte counter
        adc skew,y                  ; page = dst + skew[phys sector]
        sta zpPtr+1
        lda #0
        sta zpPtr
        ldy #0
s1:     sty zpSec                   ; reuse: byte index
sx1:    lda $C08C,x
        bpl sx1
        sty zpTmp+1
        tay
        lda DECTAB,y
        ldy zpTmp+1
        eor zpTmp
        sta zpTmp
        sta (zpPtr),y
        iny
        bne s1
        ; data checksum nibble (skip verify - FloppyEmu/AppleWin media)
ck1:    lda $C08C,x
        bpl ck1
        ; ---- combine: byte = sixes<<2 | two-bits (per position rules)
        ; twos buffer holds, per entry, 2-bit groups for bytes i, i+86, i+172
        ldy #0
fix1:   sty zpTmp+1
        lda (zpPtr),y               ; six bits in 7..2 position (<<2 below)
        ; index into twos: i mod 86, group = i / 86
        ; (predivided tables would cost ROM; compute cheaply)
        tya
        cmp #172
        bcs g2
        cmp #86
        bcs g1
        ; group 0: twos bits 1,0 (reversed pairs per the encoding)
        tax
        lda RDBUF2,x
        and #3
        tax
        lda flip2,x
        jmp fxc
g1:     sec
        sbc #86
        tax
        lda RDBUF2,x
        lsr
        lsr
        and #3
        tax
        lda flip2,x
        jmp fxc
g2:     sec
        sbc #172
        tax
        lda RDBUF2,x
        lsr
        lsr
        lsr
        lsr
        and #3
        tax
        lda flip2,x
fxc:    sta zpTmp
        ldy zpTmp+1
        lda (zpPtr),y
        asl
        asl
        ora zpTmp
        sta (zpPtr),y
        iny
        bne fix1
        ; clear this sector's wanted bit (zpSec was reused; use the stash)
        ldy secsave
        cpy #8
        bcs clrhi
        lda bits,y
        eor #$FF
        and zpSecMask
        sta zpSecMask
        rts
clrhi:  lda bits-8,y
        eor #$FF
        and zpSecMaskH
        sta zpSecMaskH
        rts

; rd44 - read an odd-even 4-and-4 encoded byte -> A
rd44:
        ldx zpSlot                  ; (x preserved convention: reload)
r41:    lda $C08C,x
        bpl r41
        sec
        rol                         ; (b<<1)|1
        sta zpTmp+1
r42:    lda $C08C,x
        bpl r42
        and zpTmp+1
        rts

secsave: dc.b 0

; physical sector -> logical page offset (DOS 3.3 software skew)
skew:   dc.b $0,$7,$E,$6,$D,$5,$C,$4,$B,$3,$A,$2,$9,$1,$8,$F
bits:   dc.b 1,2,4,8,16,32,64,128
flip2:  dc.b 0,2,1,3                ; the 2-bit groups are bit-reversed
; the 6-and-2 write translate table (62+2 valid disk nibbles)
wrtab:  dc.b $96,$97,$9A,$9B,$9D,$9E,$9F,$A6
        dc.b $A7,$AB,$AC,$AD,$AE,$AF,$B2,$B3
        dc.b $B4,$B5,$B6,$B7,$B9,$BA,$BB,$BC
        dc.b $BD,$BE,$BF,$CB,$CD,$CE,$CF,$D3
        dc.b $D6,$D7,$D9,$DA,$DB,$DC,$DD,$DE
        dc.b $DF,$E5,$E6,$E7,$E9,$EA,$EB,$EC
        dc.b $ED,$EE,$EF,$F2,$F3,$F4,$F5,$F6
        dc.b $F7,$F9,$FA,$FB,$FC,$FD,$FE,$FF

; pad track 0 to exactly 16 sectors (4096 bytes)
bootend:
        ds 4096 - (bootend - $0800), 0
