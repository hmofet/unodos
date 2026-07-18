; ============================================================================
; UnoDOS/AppleII mini-FS (milestone 2) - "USV1" sector-heap catalog, the
; track/sector analogue of genesis/sram.i's byte-heap SRAM filesystem
; (HANDOFF-M2 SS2). Built on rwts.i's rwts_read/rwts_write.
;
; Layout: FS region = tracks FS_TRACK..34 (FS_TRACKS=15 tracks, 240 x
; 256-byte sectors). Sectors are numbered 0..239 "relative" to the region;
; rel -> (track,logical) is FS_TRACK+(rel>>4), rel&$0F (FS_SECTORS=240 is a
; multiple of 16, so this is exact - no remainder sector).
;
; Catalog = rel sector 0 (cached in RAM at CATBUF, flushed on every mutation):
;   0..3    magic "USV1" (written by mkfs.py; not re-validated at boot)
;   4       file count (0..FS_MAXFILES=15)
;   5       next-free-sector (rel index, heap grows upward from 1)
;   6..15   reserved (0)
;   16..255 15 directory entries x 16 bytes:
;     0..11   name (NUL-padded, 12 bytes)
;     12..13  size in bytes, word, LITTLE-endian (interchange media -
;             FloppyEmu/AppleWin .dsk, unlike Genesis's BE SRAM)
;     14..15  start = first rel sector of the file's data, word LE
;             (high byte always 0: FS_SECTORS=240 < 256)
;
; Files are contiguous runs of rel sectors in the heap (rel 1..239); the
; last sector of a file may have trailing garbage past its byte size.
; fs_delete compacts the heap (shift later files down by the freed sector
; count, fix every entry's start, drop the directory slot) and fs_save
; overwrites by delete-then-append - same semantics as genesis/sram.i, so
; Files/Notepad port with the same call shape: fs_find/fs_read/fs_save/
; fs_delete + fs_entry_ptr/fs_size/fs_start for listing.
;
; mkfs.py formats the catalog and seeds disk/*.TXT at image-build time;
; the kernel only ever reads/writes the catalog it finds (fs_init loads it,
; never reformats).
; ============================================================================

; ---- zero page: the zpFS* slots + CATBUF/NOTEBUF + FSE_SIZE now live in
;      sys.inc (shared with disk-loaded apps); kernel.s includes sys.inc
;      before this file. The FS-internal layout constants stay here. ----
zpFSEnt  equ zpFSRel ; (2) alias of zpFSRel/zpFSCnt, reused as a source-entry
                   ; pointer by fs_delete's fsdd_loop (directory-shift copy) -
                   ; safe because fsd_fix is zpFSCnt's last reader before this,
                   ; and fsv_fresh resets both zpFSRel/zpFSCnt right after
                   ; fs_delete returns (must NOT alias zpFSDat, which fs_save
                   ; needs to survive fs_delete for its fs_write_sectors call)

; ---- layout constants ----
FS_TRACK    equ 20
FS_TRACKS   equ 35-FS_TRACK    ; 15
FS_SECTORS  equ FS_TRACKS*16   ; 240
FS_MAXFILES equ 15

; FSC_COUNT/FSC_NEXTFREE/FSC_DIR/FSE_SIZE now live in sys.inc (shared with the
; disk apps). FS-internal entry offsets:
FSE_START    equ 14
FSE_LEN      equ 16

; ---------------------------------------------------------------- fs_init
; fs_init - call once at boot (after rwts_init): load the catalog into CATBUF.
fs_init:
        lda #FS_TRACK
        sta zpTrkWant
        lda #<CATBUF
        sta zpBuf
        lda #>CATBUF
        sta zpBuf+1
        lda #0
        jsr rwts_read
        ; S2: a crafted catalog file-count would drive fs_find / files_draw /
        ; fs_delete loops past the 15-entry directory -> clamp it at load.
        lda CATBUF+FSC_COUNT
        cmp #FS_MAXFILES+1
        bcc fsi_ok
        lda #FS_MAXFILES
        sta CATBUF+FSC_COUNT
fsi_ok:
        rts

; ---------------------------------------------------------------- fs_flush
; fs_flush - write CATBUF back to rel sector 0 (after any mutation).
fs_flush:
        lda #FS_TRACK
        sta zpTrkWant
        lda #<CATBUF
        sta zpBuf
        lda #>CATBUF
        sta zpBuf+1
        lda #0
        jmp rwts_write

; ------------------------------------------------------------- fs_entry_ptr
; fs_entry_ptr - A = directory index (0..14) -> zpFSPtr = &CATBUF entry.
; CATBUF is page-aligned and FSC_DIR+14*16+16=256, so the low byte never
; carries.
fs_entry_ptr:
        asl
        asl
        asl
        asl
        clc
        adc #FSC_DIR
        sta zpFSPtr
        lda #>CATBUF
        sta zpFSPtr+1
        rts

; ------------------------------------------------------------------ fs_size
; fs_size - A = directory index -> zpFSSize = entry's size (LE, 2 bytes).
fs_size:
        jsr fs_entry_ptr
        ldy #FSE_SIZE
        lda (zpFSPtr),y
        sta zpFSSize
        iny
        lda (zpFSPtr),y
        sta zpFSSize+1
        rts

; ----------------------------------------------------------------- fs_start
; fs_start - A = directory index -> A = entry's start rel sector (0..239;
; the stored high byte is always 0 and is not returned).
fs_start:
        jsr fs_entry_ptr
        ldy #FSE_START
        lda (zpFSPtr),y
        rts

; ------------------------------------------------------------- fs_size_to_sectors
; fs_size_to_sectors - zpFSSize (LE) -> A = ceil(size/256). 0 bytes -> 0.
fs_size_to_sectors:
        lda zpFSSize+1
        ldx zpFSSize
        beq fsts_done
        clc
        adc #1
fsts_done:
        rts

; ------------------------------------------------------------- fs_sector_addr
; fs_sector_addr - A = rel sector (0..239) -> sets zpTrkWant, returns the
; logical sector (0..15) in A, ready for rwts_read/rwts_write.
fs_sector_addr:
        pha
        lsr
        lsr
        lsr
        lsr
        clc
        adc #FS_TRACK
        sta zpTrkWant
        pla
        and #$0F
        rts

; ------------------------------------------------------------- fs_read_sectors
; fs_read_sectors - in: zpFSRel = first rel sector, zpFSCnt = sector count,
; zpFSDat = dest pointer (advanced by 256 per sector - caller's buffer
; should be page-aligned). Reads zpFSCnt sectors into *zpFSDat.
fs_read_sectors:
frs_loop:
        lda zpFSCnt
        beq frs_done
        lda zpFSRel
        jsr fs_sector_addr
        sta zpFSTmp2
        lda zpFSDat
        sta zpBuf
        lda zpFSDat+1
        sta zpBuf+1
        lda zpFSTmp2
        jsr rwts_read
        inc zpFSDat+1
        inc zpFSRel
        dec zpFSCnt
        jmp frs_loop
frs_done:
        rts

; ------------------------------------------------------------ fs_write_sectors
; fs_write_sectors - same in/out convention as fs_read_sectors, but writes.
fs_write_sectors:
fws_loop:
        lda zpFSCnt
        beq fws_done
        lda zpFSRel
        jsr fs_sector_addr
        sta zpFSTmp2
        lda zpFSDat
        sta zpBuf
        lda zpFSDat+1
        sta zpBuf+1
        lda zpFSTmp2
        jsr rwts_write
        inc zpFSDat+1
        inc zpFSRel
        dec zpFSCnt
        jmp fws_loop
fws_done:
        rts

; ------------------------------------------------------------------ fs_find
; fs_find - zpFSName = pointer to a 12-byte NUL-padded name -> A = directory
; index (0..14) or $FF if not found.
fs_find:
        ldx #0
ff_loop:
        cpx CATBUF+FSC_COUNT
        bcs ff_miss
        stx zpFSIdx
        txa
        jsr fs_entry_ptr
        ldy #0
ff_cmp:
        lda (zpFSPtr),y
        cmp (zpFSName),y
        bne ff_next
        iny
        cpy #12
        bne ff_cmp
        lda zpFSIdx
        rts
ff_next:
        inx
        jmp ff_loop
ff_miss:
        lda #$FF
        rts

; ------------------------------------------------------------------ fs_read
; fs_read - A = directory index, zpFSDat = dest pointer (page-aligned) ->
; reads the file's sectors into *zpFSDat; zpFSSize = byte size (LE) on exit.
fs_read:
        pha                     ; fs_size returns A=size hi byte, not index -
        jsr fs_size             ; save the index across the call so fs_start
        pla                     ; (A = directory index) gets the right one
        jsr fs_start
        sta zpFSRel
        jsr fs_size_to_sectors
        sta zpFSCnt
        ; S1: clamp the sector count to the caller's buffer capacity (zpFSTmp,
        ; set by every fs_read caller) so a crafted catalog size field can't
        ; drive the read past the destination buffer into adjacent RAM.
        cmp zpFSTmp
        bcc frd_go              ; count < cap
        beq frd_go              ; count == cap
        lda zpFSTmp             ; count > cap -> clamp to cap
        sta zpFSCnt
frd_go:
        jmp fs_read_sectors

; ----------------------------------------------------------------- fs_delete
; fs_delete - A = directory index: free its sectors, compact the heap (shift
; later files' sectors down and fix their start fields), drop the
; directory slot, flush the catalog.
fs_delete:
        sta zpFSIdx
        jsr fs_entry_ptr
        ldy #FSE_START
        lda (zpFSPtr),y
        sta zpFSTmp             ; v = victim's start
        ldy #FSE_SIZE
        lda (zpFSPtr),y
        sta zpFSSize
        iny
        lda (zpFSPtr),y
        sta zpFSSize+1
        jsr fs_size_to_sectors
        sta zpFSCnt             ; n = victim's sector count

; ---- shift heap sectors [v+n .. nextfree-1] down by n ----
        lda zpFSTmp
        clc
        adc zpFSCnt
        sta zpFSRel             ; src rel sector
fsd_shift:
        lda zpFSRel
        cmp CATBUF+FSC_NEXTFREE
        bcs fsd_shiftdone
        jsr fs_sector_addr
        sta zpFSTmp2
        lda #<SECBUF
        sta zpBuf
        lda #>SECBUF
        sta zpBuf+1
        lda zpFSTmp2
        jsr rwts_read
        lda zpFSRel
        sec
        sbc zpFSCnt
        jsr fs_sector_addr
        sta zpFSTmp2
        lda #<SECBUF
        sta zpBuf
        lda #>SECBUF
        sta zpBuf+1
        lda zpFSTmp2
        jsr rwts_write
        inc zpFSRel
        jmp fsd_shift
fsd_shiftdone:

; ---- nextfree -= n ----
        lda CATBUF+FSC_NEXTFREE
        sec
        sbc zpFSCnt
        sta CATBUF+FSC_NEXTFREE

; ---- fix start fields: any entry with start > v gets start -= n ----
        ldx #0
fsd_fix:
        cpx CATBUF+FSC_COUNT
        bcs fsd_fixdone
        txa
        jsr fs_entry_ptr
        ldy #FSE_START
        lda (zpFSPtr),y
        cmp zpFSTmp
        bcc fsd_fixnext
        beq fsd_fixnext
        sec
        sbc zpFSCnt
        sta (zpFSPtr),y
fsd_fixnext:
        inx
        jmp fsd_fix
fsd_fixdone:

; ---- shift directory entries (victim+1..count-1) down one slot ----
        ldx zpFSIdx
fsdd_loop:
        inx
        cpx CATBUF+FSC_COUNT
        bcs fsdd_done
        txa
        jsr fs_entry_ptr
        lda zpFSPtr
        sta zpFSEnt
        lda zpFSPtr+1
        sta zpFSEnt+1
        txa
        sec
        sbc #1
        jsr fs_entry_ptr
        ldy #0
fsdd_cp:
        lda (zpFSEnt),y
        sta (zpFSPtr),y
        iny
        cpy #FSE_LEN
        bne fsdd_cp
        jmp fsdd_loop
fsdd_done:
        dec CATBUF+FSC_COUNT
        jmp fs_flush

; ------------------------------------------------------------------ fs_save
; fs_save - zpFSName = pointer to a 12-byte NUL-padded name, zpFSDat =
; source pointer (page-aligned), zpFSSize = byte size (LE). Overwrites an
; existing same-named file (delete + append). Returns A = 0 ok, $FF if the
; directory or heap is full (FS_MAXFILES entries / FS_SECTORS sectors).
fs_save:
        jsr fs_find
        cmp #$FF
        beq fsv_fresh

; ---- overwrite: fs_delete clobbers zpFSSize with the VICTIM's old size (its
; own scratch for size_to_sectors) - stash the caller's new size on the
; stack and restore it once fs_delete returns ----
        lda zpFSSize
        pha
        lda zpFSSize+1
        pha
        jsr fs_delete
        pla
        sta zpFSSize+1
        pla
        sta zpFSSize
fsv_fresh:
        lda CATBUF+FSC_COUNT
        cmp #FS_MAXFILES
        bcs fsv_full
        jsr fs_size_to_sectors
        sta zpFSTmp             ; N = sectors needed (persists past fs_write_sectors)
        clc
        adc CATBUF+FSC_NEXTFREE
        cmp #FS_SECTORS+1
        bcs fsv_full

; ---- new directory entry at index = count ----
        lda CATBUF+FSC_COUNT
        jsr fs_entry_ptr
        ldy #0
fsv_nm:
        lda (zpFSName),y
        sta (zpFSPtr),y
        iny
        cpy #12
        bne fsv_nm
        ldy #FSE_SIZE
        lda zpFSSize
        sta (zpFSPtr),y
        iny
        lda zpFSSize+1
        sta (zpFSPtr),y
        ldy #FSE_START
        lda CATBUF+FSC_NEXTFREE
        sta (zpFSPtr),y
        iny
        lda #0
        sta (zpFSPtr),y

; ---- write the data sectors ----
        lda CATBUF+FSC_NEXTFREE
        sta zpFSRel
        lda zpFSTmp
        sta zpFSCnt
        jsr fs_write_sectors

; ---- nextfree += N, count += 1 ----
        lda CATBUF+FSC_NEXTFREE
        clc
        adc zpFSTmp
        sta CATBUF+FSC_NEXTFREE
        inc CATBUF+FSC_COUNT

        jsr fs_flush
        lda #0
        rts
fsv_full:
        lda #$FF
        rts
