; ============================================================================
; UnoDOS/MacPlus .Sony block device - the fdd_* interface the FAT12 core
; (fat12.i) expects, implemented over the ROM's .Sony disk driver via the
; same _Read/_Write A-traps the boot blocks use. The driver presents a flat
; 512-byte logical sector space (ioPosOffset is a raw byte offset, regardless
; of the on-disk GCR zoning), so a volume sector N just maps to disk byte
; (FS_START_SECTOR + N) * 512.
;
; This is the only ROM facility UnoDOS uses past boot, and only for disk I/O
; (the analog of the x86 port's BIOS INT 13h). It works in both Plus and
; ROM-assisted (SE/II) modes: classic .Sony I/O is polled with interrupts
; masked, so it is unaffected by our owning the VIA/SCC vectors.
;
;   fdd_read_sector   d0 = volume LBA, a1 = dest   -> d0 = 0 ok / -1
;   fdd_write_sector  d0 = volume LBA, a1 = source -> d0 = 0 ok / -1
;   fdd_invalidate    no-op (no track cache; the .Sony driver caches)
; ============================================================================

; The UnoDOS FAT12 filesystem volume begins here on the 800K boot disk,
; well past the boot blocks (sectors 0-1) and the kernel image. Keep this in
; sync with mkfs.py's FS_START_SECTOR.
FS_START_SECTOR equ 256             ; 256 * 512 = 131072 (128 KB) into the disk
SONY_REFNUM     equ -5              ; .Sony driver refNum
SONY_DRIVE      equ 1               ; internal floppy ioVRefNum

; sony_io - shared _Read/_Write plumbing.
;   d0 = volume LBA, a1 = buffer, d2 = trap word ($A002 read / $A003 write)
; Builds a ParamBlockRec in sony_pb and issues the trap. -> d0 = 0 / -1.
sony_io:
        movem.l d1-d3/a0-a1,-(sp)
        lea     sony_pb,a0
        ; zero the param block (50 bytes -> 13 longs, last partial)
        moveq   #12,d1
.zero:  clr.l   (a0)
        addq.l  #4,a0
        dbra    d1,.zero
        lea     sony_pb,a0
        move.w  #SONY_DRIVE,22(a0)          ; ioVRefNum = drive 1
        move.w  #SONY_REFNUM,24(a0)         ; ioRefNum  = .Sony
        move.l  a1,32(a0)                   ; ioBuffer
        move.l  #512,36(a0)                 ; ioReqCount = one sector
        move.w  #1,44(a0)                   ; ioPosMode = fsFromStart
        ; ioPosOffset = (FS_START_SECTOR + LBA) * 512
        and.l   #$FFFF,d0
        add.l   #FS_START_SECTOR,d0
        lsl.l   #8,d0
        lsl.l   #1,d0                        ; * 512
        move.l  d0,46(a0)                   ; ioPosOffset
        ; issue the trap (a0 = param block, per the .Sony calling convention)
        move.w  d2,d3
        cmp.w   #$A003,d3
        beq     .write
        dc.w    $A002                       ; _Read  (PBReadSync)
        bra     .chk
.write: dc.w    $A003                       ; _Write (PBWriteSync)
.chk:   tst.w   d0                          ; trap returns ioResult in d0
        bne     .err
        moveq   #0,d0
        bra     .out
.err:   moveq   #-1,d0
.out:   movem.l (sp)+,d1-d3/a0-a1
        rts

fdd_read_sector:
        move.w  #$A002,d2
        bra     sony_io

fdd_write_sector:
        move.w  #$A003,d2
        bra     sony_io

fdd_invalidate:
        rts
