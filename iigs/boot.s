; ============================================================================
; UnoDOS/Apple IIGS - block-0 boot stage (loaded by ProDOS block firmware).
;
; Contract (see iigs/HANDOFF.md SS2b): firmware reads block 0 of the boot
; device to $0800, verifies byte $0800 = $01, and JMPs $0801 in 6502
; EMULATION mode with X = slot<<4.  We:
;   1. derive the slot firmware's ProDOS block-driver entry = $Cn00+[$CnFF],
;   2. read kernel blocks 1..KBLOCKS to $00:2000 via that driver
;      (ProDOS device-driver call: $42=cmd, $43=unit, $44/45=buf, $46/47=blk),
;   3. switch to native mode (clc/xce, rep #$30) and JMP the kernel.
;
; The kernel itself enables Super Hi-Res; the boot stage only loads + hands
; off, so this code works against the REAL firmware and the harness's
; WDM-trap driver stub identically.  mkdsk.py patches KBLOCKS into the
; unique "CMP #$4B" below.
; ============================================================================

.p816
.a8
.i8

KERNEL  = $2000        ; kernel load address (bank 0)

; ProDOS device-driver zero page (firmware ABI)
cmd      = $42
unit     = $43
bufptr   = $44         ; $44/$45
blocknum = $46         ; $46/$47
; our scratch
drvptr   = $06         ; $06/$07  -> $Cn00
entry    = $08         ; $08/$09  -> driver entry

.segment "CODE"
        .byte $01                  ; $0800: ProDOS block-boot signature

boot:                              ; $0801: firmware entry, E-mode, X=slot<<4
        sei
        cld
        ; --- form pointer to slot firmware page $Cn00 ---
        txa
        lsr a
        lsr a
        lsr a
        lsr a                      ; A = slot (0..7)
        ora #$C0                   ; A = $Cn (firmware page high byte)
        stz drvptr                 ; drvptr lo = 0
        sta drvptr+1               ; drvptr = $Cn00
        ; --- driver entry = $Cn00 + [$CnFF] ---
        ldy #$FF
        lda (drvptr),y             ; A = [$CnFF] = entry low offset
        sta entry
        lda drvptr+1
        sta entry+1                ; entry = $Cn00 + offset
        ; --- stash the driver entry + unit for the kernel's runtime blk_io ---
        ; (bank-0 $0300-$0302 survives the kernel's VARS clear at $1000+)
        lda entry
        sta $0300
        lda entry+1
        sta $0301
        stx $0302                  ; unit = slot<<4
        ; --- set up the ProDOS READ for block 1 -> $2000 ---
        stx unit                   ; unit = slot<<4 (drive 1, bit7=0)
        lda #1
        sta cmd                    ; 1 = READ
        stz bufptr                 ; buffer lo = $00
        lda #$20
        sta bufptr+1               ; buffer = $2000
        lda #1
        sta blocknum               ; block = 1
        stz blocknum+1

read_loop:
        jsr call_driver
        bcs boot_err
        ; advance destination buffer by 512 bytes ($200)
        lda bufptr+1
        clc
        adc #2
        sta bufptr+1
        ; next block
        inc blocknum
        bne :+
        inc blocknum+1
:       lda blocknum
        cmp #$4B                   ; <-- mkdsk patches $4B = KBLOCKS+1
        bne read_loop

        ; --- all kernel blocks loaded: go native and launch ---
        clc
        xce                        ; native mode
        rep #$30                   ; 16-bit A/X/Y
        jmp KERNEL                 ; PBR still 0 -> $00:2000

; ProDOS driver trampoline: driver RTSes back to after the JSR that called us.
call_driver:
        jmp (entry)

boot_err:
        ; load failure - hang with a visible value on the screen border-ish.
        ; (real firmware would beep; harness flags carry-set return.)
        jmp boot_err
