; ============================================================================
; UnoDOS/MacPlus boot blocks (sectors 0-1, 1024 bytes)
; ============================================================================
; The Mac ROM Start Manager reads these two sectors, validates the $4C4B
; signature + version $4418 (high byte bit 6 = "execute boot code") and
; jumps to the bra below. At that point the ROM has initialized the trap
; dispatcher and opened the .Sony driver (refNum -5) -- the same services
; the x86 port gets from its BIOS. We use exactly one of them (_Read) to
; pull the kernel image off the raw sectors after the boot blocks, then
; jump to it and never come back. Layout per Inside Macintosh: Files
; p. 2-57 (old-format header), bootstrap technique per EMILE first.S.
;
; All code is position-independent: the ROM loads the boot blocks at an
; address it does not document.
; ============================================================================

KERNBASE    equ $20000              ; kernel load address (fixed org)

        mc68000
        org     0

begin:
        dc.w    $4C4B               ; bbID 'LK'
        bra.w   bootcode            ; bbEntry (longword field = bra.w)
        dc.w    $4418               ; bbVersion: execute boot code
        dc.w    0                   ; bbPageFlags

        ; seven Str15 filename fields (16 bytes each) -- unused by us,
        ; they only feed the standard System boot path
        dc.b    14,"UnoDOS MacPlus",0
        dc.b    14,"standalone OS ",0
        dc.b    14,"ROM bootstraps",0
        dc.b    14,"then UnoDOS   ",0
        dc.b    14,"owns the      ",0
        dc.b    14,"machine.      ",0
        dc.b    14,"(c) 2026      ",0

        dc.w    10                  ; bbCntFCBs
        dc.w    20                  ; bbCntEvts
        dc.l    $00004300           ; bb128KSHeap
        dc.l    $00008000           ; bb256KSHeap (reserved)
        dc.l    $00020000           ; bbSysHeapSize

; ---------------------------------------------------------------- boot code
bootcode:
        lea     param_block(pc),a0
        ; the boot disk is NOT always drive 1: a FloppyEmu on the SE's
        ; external port (or the lower bay of a dual-drive SE) enumerates
        ; as drive 2/3. The Start Manager publishes the real boot drive
        ; in low-mem BootDrive ($210) before running the boot blocks.
        move.w  $210,d0
        bne     .drvok
        moveq   #1,d0               ; unset: fall back to internal drive 1
.drvok: move.w  d0,22(a0)           ; ioVRefNum
        dc.w    $A002               ; _Read (PBReadSync): .Sony raw sectors
        tst.w   d0
        bne     fail
        ; paranoia: the kernel really arrived? ('UDM1' magic at entry+4)
        cmp.l   #$55444D31,KERNBASE+4
        bne     badimg
        jmp     KERNBASE            ; hand the machine to UnoDOS

; Distinctive Sad Mac minor codes (shown as 0000000F XXXXXXXX):
;   $42 = the kernel _Read failed (wrong drive/disk or I/O error)
;   $43 = read "succeeded" but the kernel magic is missing (short/garbage)
fail:
        moveq   #$42,d0
        dc.w    $A9C9               ; _SysError
badimg:
        moveq   #$43,d0
        dc.w    $A9C9               ; _SysError

; ---------------------------------------------------------------- param block
; IM:Files ParamBlockRec, I/O variant. ioReqCount is patched by mkdisk.py
; with the real (sector-rounded) kernel size -- the 'KSIZ' placeholder
; keeps the patch site findable.
        even
param_block:
        dc.l    0                   ; qLink
        dc.w    0                   ; qType
        dc.w    0                   ; ioTrap
        dc.l    0                   ; ioCmdAddr
        dc.l    0                   ; ioCompletion
        dc.w    0                   ; ioResult
        dc.l    0                   ; ioNamePtr
        dc.w    1                   ; ioVRefNum = drive 1 (internal floppy)
        dc.w    -5                  ; ioRefNum  = .Sony driver
        dc.b    0                   ; ioVersNum
        dc.b    0                   ; ioPermssn
        dc.l    0                   ; ioMisc
        dc.l    KERNBASE            ; ioBuffer
        dc.l    $4B53495A           ; ioReqCount ('KSIZ', patched by mkdisk)
        dc.l    0                   ; ioActCount
        dc.w    1                   ; ioPosMode = fsFromStart
        dc.l    1024                ; ioPosOffset = right after these blocks

; ---------------------------------------------------------------- filler
        dcb.b   1024-(*-begin),$DA
        end
