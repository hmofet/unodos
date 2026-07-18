; ============================================================================
; UnoDOS/SNES SRAM storage (milestone 2) - the USV1 mini-filesystem.
;
; 8 KB of battery-backed cartridge SRAM. LoROM maps it at $70:0000-$70:1FFF
; (byte-addressable - none of the Genesis odd-lane *2 dance); emulators and
; flashcarts persist it as a .srm. The header declares it (cart type $02,
; SRAM size byte $FFD8 = 3 = 8 KB).
;
; Mini-filesystem ("USV1"), little-endian words (the SNES native order - SRAM
; is not interchange media, so we keep it simple):
;   0..3    magic "USV1"
;   4..5    file count (word)
;   6..7    heap top = offset of the next free heap byte
;   16..143 8 directory entries x 16: name[12] (NUL-padded), size.w, off.w
;   144..   heap (8048 bytes); files are contiguous, delete compacts.
; ============================================================================

SRAM_FAR    = $700000              ; long base of the SRAM window
SRAM_SIZE   = 8192
SRD_OFF     = 16                   ; directory offset
SRD_MAX     = 8                    ; directory entries
SRD_ENT     = 16                   ; entry size
SRH_OFF     = 144                  ; heap offset

; ---- byte / word SRAM accessors (X = byte offset, 16-bit) ----

; sr_b: X = offset -> A = byte (A 8-bit on return)
.proc sr_b
.a8
.i16
        lda f:SRAM_FAR,x
        rts
.endproc

; sw_b: X = offset, A (8-bit) = byte
.proc sw_b
.a8
.i16
        sta f:SRAM_FAR,x
        rts
.endproc

; sr_w: X = offset -> A = word (A 16-bit on return)
.proc sr_w
.a16
.i16
        lda f:SRAM_FAR,x
        rts
.endproc

; sw_w: X = offset, A (16-bit) = word
.proc sw_w
.a16
.i16
        sta f:SRAM_FAR,x
        rts
.endproc

; ============================================================================
; FS primitives. Conventions: pass offsets/values in A0..A6 / S* DP slots.
; ============================================================================

; sram_init - validate the magic, format an uninitialised store
.proc sram_init
.a16
.i16
        ; check magic "USV1" (4 bytes)
        ldx #$0000
        sep #$20
.a8
        lda f:SRAM_FAR+0,x
        cmp #'U'
        bne @format
        lda f:SRAM_FAR+1,x
        cmp #'S'
        bne @format
        lda f:SRAM_FAR+2,x
        cmp #'V'
        bne @format
        lda f:SRAM_FAR+3,x
        cmp #'1'
        bne @format
        rep #$20
.a16
        rts
@format:
.a8                             ; reached in 8-bit mode from the magic check
        lda #'U'
        sta f:SRAM_FAR+0
        lda #'S'
        sta f:SRAM_FAR+1
        lda #'V'
        sta f:SRAM_FAR+2
        lda #'1'
        sta f:SRAM_FAR+3
        rep #$20
.a16
        lda #0
        ldx #4
        jsr sw_w                ; count = 0
        lda #SRH_OFF
        ldx #6
        jsr sw_w                ; heap top
        rts
.endproc

; sram_count -> A = file count
.proc sram_count
.a16
.i16
        ldx #4
        jsr sr_w
        cmp #SRD_MAX+1          ; S1: clamp a crafted count to the 8-entry
        bcc :+                  ; directory so files_draw/find/delete loops
        lda #SRD_MAX            ; can't walk past the tilemap into bank 0
:
        rts
.endproc

; sram_entry: A = index -> A = directory byte offset (index*16 + SRD_OFF)
.proc sram_entry
.a16
.i16
        asl a
        asl a
        asl a
        asl a
        clc
        adc #SRD_OFF
        rts
.endproc

; sram_name: A = index, P1 = 13-byte dest -> name copied, NUL-terminated
.proc sram_name
.a16
.i16
        jsr sram_entry
        tax                     ; X = entry offset
        ldy #$0000
@cp:    sep #$20
.a8
        lda f:SRAM_FAR,x
        sta (P1),y
        rep #$20
.a16
        inx
        iny
        cpy #12
        bne @cp
        sep #$20
.a8
        lda #0
        sta (P1),y
        rep #$20
.a16
        rts
.endproc

; sram_size: A = index -> A = size word
.proc sram_size
.a16
.i16
        jsr sram_entry
        clc
        adc #12
        tax
        jsr sr_w
        rts
.endproc

; sram_find: P1 = 12-byte name -> A = index or $FFFF
.proc sram_find
.a16
.i16
        jsr sram_count
        sta S4                  ; count
        stz S5                  ; index
@ent:   lda S5
        cmp S4
        bcs @miss
        lda S5
        jsr sram_entry
        tax                     ; entry offset
        ldy #$0000
@ch:    sep #$20
.a8
        lda f:SRAM_FAR,x
        cmp (P1),y
        bne @next
        rep #$20
.a16
        inx
        iny
        cpy #12
        bne @ch
        lda S5                  ; match
        rts
@next:  rep #$20
.a16
        inc S5
        bra @ent
@miss:  lda #$FFFF
        rts
.endproc

; sram_read: A = index, P0 = dest, A2 = max -> A = bytes read
.proc sram_read
.a16
.i16
        jsr sram_entry
        sta S4                  ; entry offset
        clc
        adc #12
        tax
        jsr sr_w
        sta S5                  ; size
        cmp A2
        bcc @szok
        lda A2
        sta S5                  ; clamp to max
@szok:  lda S4
        clc
        adc #14
        tax
        jsr sr_w                ; A = data offset
        tax                     ; X = data offset
        ldy #$0000
@cp:    cpy S5
        bcs @done
        sep #$20
.a8
        lda f:SRAM_FAR,x
        sta (P0),y
        rep #$20
.a16
        inx
        iny
        bra @cp
@done:  lda S5
        rts
.endproc

; sram_delete: A = index - compact heap, fix offsets, shift directory
.proc sram_delete
.a16
.i16
        sta S0                  ; victim index
        jsr sram_entry
        sta S1                  ; victim entry offset
        clc
        adc #12
        tax
        jsr sr_w
        sta S2                  ; victim size
        lda S1
        clc
        adc #14
        tax
        jsr sr_w
        sta S3                  ; victim data offset
        ; heap top
        ldx #6
        jsr sr_w
        sta S4                  ; heap top
        ; move [off+size, heaptop) down by size
        lda S3
        clc
        adc S2
        sta S5                  ; src
        lda S3
        sta S6                  ; dst
@mv:    lda S5
        cmp S4
        bcs @mvdone
        ldx S5
        sep #$20
.a8
        lda f:SRAM_FAR,x
        ldx S6
        sta f:SRAM_FAR,x
        rep #$20
.a16
        inc S5
        inc S6
        bra @mv
@mvdone:
        ; heap top -= size
        lda S4
        sec
        sbc S2
        ldx #6
        jsr sw_w
        ; fix data offsets above the victim's
        jsr sram_count
        sta S4                  ; count
        stz S7                  ; i
@fix:   lda S7
        cmp S4
        bcs @fixdone
        lda S7
        jsr sram_entry
        clc
        adc #14
        tax
        stx S6                  ; off-field address
        jsr sr_w
        cmp S3
        bcc @fnext
        beq @fnext
        sec
        sbc S2
        ldx S6
        jsr sw_w
@fnext: inc S7
        bra @fix
@fixdone:
        ; shift directory entries [victim+1 .. count) down one slot (16 bytes)
        lda S0
        sta S7                  ; i = victim
@shift: inc S7
        lda S7
        cmp S4
        bcs @shdone
        lda S7
        jsr sram_entry
        sta S6                  ; src entry offset
        ldy #$0000
@shb:   tya
        clc
        adc S6                  ; src = entry + y
        tax
        sep #$20
.a8
        lda f:SRAM_FAR,x        ; read src byte
        pha
        rep #$20
.a16
        txa
        sec
        sbc #SRD_ENT            ; dst = src - 16
        tax
        sep #$20
.a8
        pla
        sta f:SRAM_FAR,x        ; write dst byte
        rep #$20
.a16
        iny
        cpy #SRD_ENT
        bne @shb
        bra @shift
@shdone:
        ; count--
        jsr sram_count
        dec a
        ldx #4
        jsr sw_w
        rts
.endproc

; sram_save: P1 = 12-byte name, P0 = src, A1 = len -> A = 0 ok / $FFFF full
.proc sram_save
.a16
.i16
        lda A1
        sta S7                  ; len
        ; overwrite: delete an existing file of this name
        jsr sram_find
        cmp #$FFFF
        beq @fresh
        jsr sram_delete
@fresh: jsr sram_count
        sta S4                  ; count
        cmp #SRD_MAX
        bcs @full
        ldx #6
        jsr sr_w
        sta S5                  ; heap top
        clc
        adc S7
        cmp #(SRAM_SIZE+1)
        bcs @full
        ; directory entry
        lda S4
        jsr sram_entry
        sta S6                  ; entry offset
        tax
        ldy #$0000
@nm:    sep #$20
.a8
        lda (P1),y
        sta f:SRAM_FAR,x
        rep #$20
.a16
        inx
        iny
        cpy #12
        bne @nm
        lda S6
        clc
        adc #12
        tax
        lda S7
        jsr sw_w                ; size
        lda S6
        clc
        adc #14
        tax
        lda S5
        jsr sw_w                ; offset
        ; data
        lda S5
        tax                     ; X = heap write offset
        ldy #$0000
@cp:    cpy S7
        bcs @cpdone
        sep #$20
.a8
        lda (P0),y
        sta f:SRAM_FAR,x
        rep #$20
.a16
        inx
        iny
        bra @cp
@cpdone:
        ; heap top += len; count++
        lda S5
        clc
        adc S7
        ldx #6
        jsr sw_w
        lda S4
        inc a
        ldx #4
        jsr sw_w
        lda #0
        rts
@full:  lda #$FFFF
        rts
.endproc
