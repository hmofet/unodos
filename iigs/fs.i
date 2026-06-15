; ============================================================================
; UnoDOS/Apple IIGS - FAT12 storage over the SmartPort/ProDOS block driver.
;
; blk_io calls the slot firmware's ProDOS block driver (entry + unit stashed
; by boot.s at $0300-$0302) in 6502 EMULATION mode - the driver is 6502 ROM
; code, so we sec/xce around the call and clc/xce back to native, capturing
; the result carry before the mode flip clobbers it.  Works identically
; against real firmware and the harness WDM-trap stub.
;
; FAT12 geometry is fixed and MUST stay in sync with mkfs.py (PORT-SPEC SS6
; rule 8 - define it once): 512-byte sectors, 1 KB clusters (SPC=2), 1
; reserved sector, 2 FATs x 3 sectors, 112 root entries (7 sectors).  The
; whole 3-sector FAT is cached in FATBUF at mount, so 12-bit entries never
; straddle a sector.  Little-endian on-disk fields read natively (no swap).
; ============================================================================

; ---- volume geometry (sync with mkfs.py) ----
FS_START_BLOCK = 256          ; disk block of FAT12 volume sector 0
BPS        = 512
SPC        = 2                ; sectors/cluster
NFATS      = 2
SPF        = 3                ; sectors/FAT
FAT_START  = 1                ; volume sector of FAT #1
ROOT_START = 7                ; volume sector of root directory
ROOT_SECS  = 7
DATA_START = 14               ; volume sector of cluster 2
FAT_BYTES  = SPF * BPS        ; 1536

; ---- bank-0 buffers (above kernel code, below $C000 I/O) ----
DIRSEC = $9000                ; 512B directory/sector scratch
SECBUF = $9800                ; 512B general sector buffer (save path)
FATBUF = $9200                ; 1536B cached FAT
NBUF   = $A000                ; 4 KB Notepad text buffer
NBUFSZ = 4096

; ---- FS state (VARS region; clear_state clears through $0500) ----
v_fs_dircount = VARS+$300
v_fs_sel      = VARS+$302
v_np_len      = VARS+$306
v_np_dirty    = VARS+$308
v_np_caret    = VARS+$30A
v_np_clus     = VARS+$30C     ; first cluster of the open file (0 = new)
v_np_name     = VARS+$310     ; 11-byte FAT name (+ pad)
v_dir_list    = VARS+$340     ; 16 entries x 16 bytes: name(11) pad cluster(2) size(2)
DIRENT_SZ = 16
; ---- save-path scratch (VARS) ----
v_sv_data     = VARS+$320
v_sv_len      = VARS+$322
v_sv_lba      = VARS+$324
v_sv_off      = VARS+$326
v_sv_freelba  = VARS+$328
v_sv_freeoff  = VARS+$32A
v_sv_first    = VARS+$32C

; ---- FS scratch (zp, clear of pseudo-regs and SHR scratch) ----
F0 = $54
F1 = $56
F2 = $58
F3 = $5A
F4 = $5C
F5 = $5E
F6 = $60
DPTR = $62                    ; 16-bit dest pointer (bank 0)
BLK0 = $64                    ; blk_io private temps (must NOT alias F0/F1,
BLK1 = $66                    ;   which callers keep live across blk_io)

; ============================================================================
; blk_io: A2 = volume LBA, P0 = bank-0 buffer ptr, A3 = cmd (1 read / 2 write)
;         -> A = 0 ok, nonzero on error.  Native in, native out.
; ============================================================================
.a16
.i16
blk_io:
        lda A2
        clc
        adc #FS_START_BLOCK
        sta BLK0               ; absolute disk block (private - F0 stays caller's)
        sep #$20
        lda A3
        sta $42                ; command
        lda $0302
        sta $43                ; unit
        lda P0
        sta $44                ; buffer lo
        lda P0+1
        sta $45                ; buffer hi
        lda BLK0
        sta $46                ; block lo
        lda BLK0+1
        sta $47                ; block hi
        sec
        xce                    ; -> 6502 emulation mode
        jsr prodos_call
        lda #0
        rol a                  ; A bit0 = result carry (1 = error)
        sta BLK1
        clc
        xce                    ; -> native
        rep #$30
        lda BLK1
        and #$00FF
        rts

prodos_call:
        jmp ($0300)            ; stashed ProDOS driver entry; driver RTSes back

; ============================================================================
; fat_mount: cache the whole FAT (3 sectors) into FATBUF.
; ============================================================================
.a16
.i16
fat_mount:
        lda #FAT_START
        sta F3                 ; volume LBA
        lda #.loword(FATBUF)
        sta F4                 ; running dest
        ldx #0
@s:     phx
        lda F3
        sta A2
        lda F4
        sta P0
        lda #1
        sta A3
        jsr blk_io
        lda F4
        clc
        adc #BPS
        sta F4
        inc F3
        plx
        inx
        cpx #SPF
        bcc @s
        rts

; ============================================================================
; fat_next_cluster: F0 = cluster -> A0 = next cluster (>= $0FF8 means EOF).
; ============================================================================
.a16
.i16
fat_next_cluster:
        lda F0
        lsr a
        clc
        adc F0                 ; byte offset = cluster*3/2
        tax
        lda FATBUF,x           ; 16-bit (little-endian) FAT word
        pha                    ; save raw word (the test below clobbers A)
        lda F0
        and #1
        beq @even
        pla
        lsr a
        lsr a
        lsr a
        lsr a                  ; odd cluster: the high 12 bits
        and #$0FFF
        sta A0
        rts
@even:  pla
        and #$0FFF             ; even cluster: the low 12 bits
        sta A0
        rts

; ============================================================================
; fat_read_file: A0 = first cluster, F2 = max bytes, P0 = dest (bank 0).
;                Reads whole clusters until F2 is satisfied or EOF.
; ============================================================================
.a16
.i16
fat_read_file:
        lda A0
        sta F0                 ; current cluster
@loop:  lda F0
        cmp #$0FF8
        bcs @done
        lda F2                 ; bytes remaining?
        beq @done
        ; first data sector of the cluster = DATA_START + (cluster-2)*SPC
        lda F0
        sec
        sbc #2
        asl a                  ; *SPC (2)
        clc
        adc #DATA_START
        sta F3                 ; volume LBA
        ldx #0
@sec:   phx
        lda F3
        sta A2
        ; P0 already the running dest
        lda #1
        sta A3
        jsr blk_io
        lda P0
        clc
        adc #BPS
        sta P0
        inc F3
        ; decrement remaining (saturating)
        lda F2
        sec
        sbc #BPS
        bcs @nz
        lda #0
@nz:    sta F2
        plx
        inx
        cpx #SPC
        bcc @sec
        jsr fat_next_cluster
        lda A0
        sta F0
        bra @loop
@done:  rts

; ============================================================================
; fat_list_root: parse the root directory into v_dir_list (max 16 entries).
; ============================================================================
.a16
.i16
fat_list_root:
        stz v_fs_dircount
        lda #ROOT_START
        sta F3                 ; volume LBA
        stz F4                 ; sectors done
@secloop:
        lda F3
        sta A2
        lda #.loword(DIRSEC)
        sta P0
        lda #1
        sta A3
        jsr blk_io
        stz F5                 ; entry within sector (0..15)
@ent:   lda F5
        asl a
        asl a
        asl a
        asl a
        asl a                  ; *32
        sta F6                 ; entry offset in DIRSEC
        tax
        lda DIRSEC,x
        and #$00FF
        beq @done              ; $00 first byte = end of directory
        cmp #$00E5
        beq @next              ; deleted
        ; attribute byte (offset 11)
        lda DIRSEC+11,x
        and #$00FF
        sta F0                 ; attr
        and #$000F
        cmp #$000F
        beq @next              ; LFN
        lda F0
        and #$0008
        bne @next              ; volume label
        lda v_fs_dircount
        cmp #16
        bcs @done
        ; dest pointer DPTR = v_dir_list + dircount*16
        asl a
        asl a
        asl a
        asl a                  ; *16
        clc
        adc #.loword(v_dir_list)
        sta DPTR
        ; copy 11 name bytes (8-bit)
        ldx F6
        ldy #0
        sep #$20
@cn:    lda DIRSEC,x
        sta (DPTR),y
        inx
        iny
        cpy #11
        bcc @cn
        rep #$20
        ; cluster (entoff+26) -> dest+12  ; size low (entoff+28) -> dest+14
        ldx F6
        lda DIRSEC+26,x
        ldy #12
        sta (DPTR),y
        ldx F6
        lda DIRSEC+28,x
        ldy #14
        sta (DPTR),y
        inc v_fs_dircount
@next:  inc F5
        lda F5
        cmp #16
        bcc @ent
        inc F3
        inc F4
        lda F4
        cmp #ROOT_SECS
        bcs @done
        jmp @secloop
@done:  rts

; ============================================================================
; FAT12 write path
; ============================================================================

; fat_set_entry: F0 = cluster, F1 = 12-bit value -> updates FATBUF only.
.a16
.i16
fat_set_entry:
        lda F0
        lsr a
        clc
        adc F0
        tax
        lda F0
        and #1
        bne @odd
        lda FATBUF,x
        and #$F000
        ora F1
        sta FATBUF,x
        rts
@odd:   lda FATBUF,x
        and #$000F
        sta F3
        lda F1
        asl a
        asl a
        asl a
        asl a
        ora F3
        sta FATBUF,x
        rts

; fat_free_chain: F0 = first cluster -> sets each entry in the chain to 0.
.a16
.i16
fat_free_chain:
        lda F0
@loop:  cmp #2
        bcc @done
        cmp #$0FF8
        bcs @done
        sta F0
        jsr fat_next_cluster   ; A0 = next (reads F0)
        lda #0
        sta F1
        jsr fat_set_entry      ; free F0
        lda A0
        bra @loop
@done:  rts

; fat_alloc: -> A0 = a free cluster (FATBUF entry == 0), or 0 if none.
.a16
.i16
fat_alloc:
        lda #2
        sta F0
@scan:  lda F0
        cmp #667               ; clusters 2..666 (665 data clusters)
        bcs @full
        jsr fat_next_cluster   ; A0 = entry value at F0
        lda A0
        beq @found
        inc F0
        bra @scan
@found: lda F0
        sta A0
        rts
@full:  stz A0
        rts

; fat_alloc_chain: F4 = cluster count -> A0 = first cluster (0 = disk full).
.a16
.i16
fat_alloc_chain:
        stz F5                 ; prev cluster (0 = none yet)
        stz F6                 ; first cluster
@loop:  lda F4
        beq @done
        jsr fat_alloc
        lda A0
        bne @ok
        stz A0
        rts                    ; disk full
@ok:    sta F0
        lda #$0FFF
        sta F1
        jsr fat_set_entry      ; mark new cluster EOF
        lda F5
        beq @first
        ; link prev -> this
        lda F0
        pha
        lda F5
        sta F0
        pla
        sta F1
        jsr fat_set_entry
        lda F1
        sta F0                 ; restore F0 = this cluster
        bra @setprev
@first: lda F0
        sta F6                 ; chain head
@setprev:
        lda F0
        sta F5
        dec F4
        bra @loop
@done:  lda F6
        sta A0
        rts

; fat_flush: write FATBUF back to both on-disk FATs.
.a16
.i16
fat_flush:
        stz F4                 ; FAT copy index
@fat:   lda F4
        sta F0
        asl a
        clc
        adc F0                 ; *SPF (3)
        clc
        adc #FAT_START
        sta F3                 ; running vol LBA
        lda #.loword(FATBUF)
        sta F5                 ; running src
        ldx #0
@s:     phx
        lda F3
        sta A2
        lda F5
        sta P0
        lda #2
        sta A3
        jsr blk_io
        lda F5
        clc
        adc #BPS
        sta F5
        inc F3
        plx
        inx
        cpx #SPF
        bcc @s
        inc F4
        lda F4
        cmp #NFATS
        bcc @fat
        rts

; sv_fill_secbuf: copy up to 512 data bytes from (DPTR) into SECBUF, zero-pad
; the tail, advance DPTR by 512, and decrement F2 by the bytes copied.
.a16
.i16
sv_fill_secbuf:
        ldy #0
@c:     cpy #512
        bcs @adv
        lda F2
        beq @pad
        sep #$20
        lda (DPTR),y
        sta SECBUF,y
        rep #$20
        dec F2
        iny
        bra @c
@pad:   sep #$20
        lda #0
        sta SECBUF,y
        rep #$20
        iny
        bra @c
@adv:   lda DPTR
        clc
        adc #512
        sta DPTR
        rts

; fat_save_file: v_np_name = 11-byte name, P0 = data ptr, F2 = byte length.
;                -> A = 0 ok, nonzero on error.
.a16
.i16
fat_save_file:
        lda P0
        sta v_sv_data
        lda F2
        sta v_sv_len
        lda #$FFFF
        sta v_sv_freeoff
        lda #ROOT_START
        sta F3
        stz F4
@sec:   lda F3
        sta A2
        lda #.loword(DIRSEC)
        sta P0
        lda #1
        sta A3
        jsr blk_io
        stz F5
@ent:   lda F5
        asl a
        asl a
        asl a
        asl a
        asl a
        sta F6
        tax
        lda DIRSEC,x
        and #$00FF
        beq @free
        cmp #$00E5
        beq @free
        ldx F6
        ldy #0
        sep #$20
@cmp:   lda DIRSEC,x
        cmp v_np_name,y
        bne @cmpno
        inx
        iny
        cpy #11
        bcc @cmp
        rep #$20
        lda F3
        sta v_sv_lba
        lda F6
        sta v_sv_off
        ldx F6
        lda DIRSEC+26,x
        sta F0
        jsr fat_free_chain
        jmp @gotslot
@cmpno: rep #$20
        bra @nextent
@free:  lda v_sv_freeoff
        cmp #$FFFF
        bne @nextent
        lda F3
        sta v_sv_freelba
        lda F6
        sta v_sv_freeoff
@nextent:
        inc F5
        lda F5
        cmp #16
        bcc @ent
        inc F3
        inc F4
        lda F4
        cmp #ROOT_SECS
        bcs :+
        jmp @sec
:       lda v_sv_freeoff
        cmp #$FFFF
        bne :+
        jmp @dirfull
:       sta v_sv_off
        lda v_sv_freelba
        sta v_sv_lba
@gotslot:
        ; nclust = ceil(len/1024), min 1
        lda v_sv_len
        clc
        adc #(SPC*BPS-1)
        lsr a
        lsr a
        lsr a
        lsr a
        lsr a
        lsr a
        lsr a
        lsr a
        lsr a
        lsr a
        bne @nz
        lda #1
@nz:    sta F4
        jsr fat_alloc_chain
        lda A0
        bne @haveclust
        lda #1
        rts
@haveclust:
        sta v_sv_first
        sta F0                 ; current cluster
        lda v_sv_data
        sta DPTR               ; running data ptr (zp, for sv_fill_secbuf)
        lda v_sv_len
        sta F2
@wloop: lda F0
        cmp #$0FF8
        bcs @wdone
        lda F0
        sec
        sbc #2
        asl a
        clc
        adc #DATA_START
        sta F3
        ldx #0
@ws:    phx
        jsr sv_fill_secbuf
        lda F3
        sta A2
        lda #.loword(SECBUF)
        sta P0
        lda #2
        sta A3
        jsr blk_io
        inc F3
        plx
        inx
        cpx #SPC
        bcc @ws
        jsr fat_next_cluster
        lda A0
        sta F0
        bra @wloop
@wdone:
        ; update the directory entry
        lda v_sv_lba
        sta A2
        lda #.loword(DIRSEC)
        sta P0
        lda #1
        sta A3
        jsr blk_io
        ldx v_sv_off
        ldy #0
        sep #$20
@sn:    lda v_np_name,y
        sta DIRSEC,x
        inx
        iny
        cpy #11
        bcc @sn
        lda #$20
        sta DIRSEC,x           ; attr = archive
        rep #$20
        ldx v_sv_off
        lda v_sv_first
        sta DIRSEC+26,x
        lda v_sv_len
        sta DIRSEC+28,x
        lda #0
        sta DIRSEC+30,x
        lda v_sv_lba
        sta A2
        lda #.loword(DIRSEC)
        sta P0
        lda #2
        sta A3
        jsr blk_io
        jsr fat_flush
        lda #0
        rts
@dirfull:
        lda #2
        rts
