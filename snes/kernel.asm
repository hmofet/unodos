; ============================================================================
; UnoDOS/SNES - milestone 0: LoROM skeleton that boots to a tile splash and
; reacts to the joypad. The Genesis port's twin re-expressed in 65816
; (snes/HANDOFF.md). Built with cc65 (ca65 --cpu 65816 + ld65) into a plain
; LoROM .sfc cartridge.
;
; Rendering rule (snes/HANDOFF.md SS2): unlike Genesis (which pokes the VDP
; from the main loop), the SNES PPU is only writable during vblank / forced
; blank. So the main loop renders into a WRAM *tilemap shadow* and the NMI
; (vblank) handler DMAs the dirty shadow to VRAM and samples input. Static
; tile/palette data is uploaded once at boot under forced blank.
;
; Display: Mode 1, BG1 = the desktop plane. 256x224 visible => 32x28 cells of
; 8px (Genesis is 40x28 - documented as a deviation in snes/README.md).
; ============================================================================

.p816
.smart +

; ----------------------------------------------------------------- generated
; tiles_all / pal0 / NTILES / TILES_BYTES / T_* from snes/mkdata.py
.include "gen_data.inc"

; ----------------------------------------------------------------- PPU / CPU regs
INIDISP   = $2100        ; forced blank + brightness
BGMODE    = $2105
BG1SC     = $2107        ; BG1 tilemap base / size
BG12NBA   = $210B        ; BG1/BG2 char base
BG1HOFS   = $210D        ; BG1 horizontal scroll (write twice)
BG1VOFS   = $210E        ; BG1 vertical scroll   (write twice)
VMAIN     = $2115        ; VRAM address increment mode
VMADDL    = $2116        ; VRAM word address (16-bit)
VMDATAL   = $2118
CGADD     = $2121        ; CGRAM word address
CGDATA    = $2122
TM        = $212C        ; main-screen layer enable
RDNMI     = $4210        ; NMI flag (read to ack)
HVBJOY    = $4212        ; bit0 = auto-joypad busy
NMITIMEN  = $4200        ; NMI + auto-joypad enable
JOY1L     = $4218        ; auto-read joypad 1 (16-bit)
MDMAEN    = $420B        ; DMA channel enable
; DMA channel 0 registers
DMAP0     = $4300
BBAD0     = $4301
A1T0L     = $4302
A1B0      = $4304
DAS0L     = $4305

; ----------------------------------------------------------------- memory map
; Low 8 KB of every bank mirrors WRAM $7E:0000-$1FFF. We keep the kernel state
; and the tilemap shadow there so the CPU reaches it with DB=0 / 16-bit abs,
; while DMA reads the same bytes via the real WRAM bank $7E.
VARS      = $0100
v_magic   = VARS + $00   ; "UDM1" (4 bytes) - the rig's discovery handle
v_ticks   = VARS + $04   ; word: vblank counter
v_frame   = VARS + $06   ; word: free-running frame counter
v_pad     = VARS + $08   ; word: current joypad
v_padprev = VARS + $0A   ; word: previous joypad (edge detect)
v_dirty   = VARS + $0C   ; byte: tilemap shadow needs flushing
v_auto    = VARS + $0D   ; byte: AUTOTEST mode active

TMAP      = $1000        ; tilemap shadow, 32x32 words = $800 bytes ($1000-$17FF)
TMAP_BANK = $7E          ; DMA reads the shadow from real WRAM
HEXBUF    = $0040        ; 4-byte scratch in ZP for hex conversion

; cell geometry
COLS      = 32
ROWS      = 28           ; visible rows (tilemap is 32 tall; rows 28-31 off-screen)

; tilemap byte offset for (col,row) = (row*COLS + col)*2 = row*64 + col*2.
; NOTE: do NOT express this as a parameterised `.define` - ca65 drops the
; outer grouping there and evaluates row*COLS + col*2 (half row stride).
; Use the CELLOFF macro / inline arithmetic in real .macro bodies instead.

; VRAM layout (word addresses)
VRAM_MAP  = $0000        ; BG1 tilemap (32x32)
VRAM_CHR  = $1000        ; BG1 character base

.segment "CODE"

; ============================================================================
; Reset / boot
; ============================================================================
.proc Reset
        sei
        clc
        xce                     ; -> native mode
        rep #$38                ; A/X/Y 16-bit, decimal off
.a16
.i16
        ldx #$1FFF
        txs                     ; stack in bank 0 low mirror
        phk
        plb                     ; DB = program bank = $00
        lda #$0000
        tcd                     ; DP = 0

        ; --- PPU into forced blank, registers to a known state ---
        sep #$20
.a8
        lda #$8F
        sta INIDISP             ; forced blank, brightness 0
        jsr InitPPURegs

        ; --- clear kernel vars + tilemap shadow ---
        jsr ClearState

        ; --- record AUTOTEST flag (assembled-in) ---
.ifdef AUTOTEST
        lda #$01
.else
        lda #$00
.endif
        sta v_auto

        ; --- "UDM1" magic so the rig can locate the vars block ---
        lda #'U'
        sta v_magic+0
        lda #'D'
        sta v_magic+1
        lda #'M'
        sta v_magic+2
        lda #'1'
        sta v_magic+3

        ; --- upload static graphics under forced blank ---
        jsr LoadTiles
        jsr LoadPalette

        ; --- compose the splash into the shadow, flush once ---
        jsr DrawSplash
        jsr FlushTilemap

        ; --- BG1 = Mode 1 desktop plane ---
        lda #$01
        sta BGMODE              ; Mode 1, 8x8 tiles
        lda #(VRAM_MAP >> 8) & $FC    ; BG1SC: tilemap base, 32x32
        sta BG1SC
        lda #(VRAM_CHR >> 12)         ; BG12NBA: BG1 char base in 4K-word units
        sta BG12NBA
        lda #$01
        sta TM                  ; enable BG1 on the main screen

        ; --- screen on, NMI + auto-joypad on ---
        lda #$0F
        sta INIDISP             ; forced blank off, full brightness
        lda #$81
        sta NMITIMEN            ; NMI enable + auto-joypad
        cli

; ----------------------------------------------------------------- main loop
MainLoop:
        wai                     ; sleep until the NMI returns
        jsr UpdatePadLine       ; render the live joypad word into the shadow
        bra MainLoop
.endproc

; ============================================================================
; PPU register init (canonical clear), A 8-bit on entry
; ============================================================================
.proc InitPPURegs
.a8
        ldx #$00
@loop:  stz $2105,x             ; zero $2105-$2133
        inx
        cpx #($2134 - $2105)
        bne @loop
        ; sane VRAM increment: +1 word after writing the high byte
        lda #$80
        sta VMAIN
        ; explicitly zero the BG scroll registers - they are write-twice, and
        ; the single-byte clear above leaves their shared latch nonzero.
        stz BG1HOFS
        stz BG1HOFS
        stz BG1VOFS
        stz BG1VOFS
        rts
.endproc

; ============================================================================
; Clear kernel state + tilemap shadow (A 8-bit on entry)
; ============================================================================
.proc ClearState
.a8
        rep #$20
.a16
        lda #$0000
        ; vars block ($0100..$011F)
        ldx #$0000
@vloop: sta VARS,x
        inx
        inx
        cpx #$20
        bne @vloop
        ; tilemap shadow
        ldx #$0000
@tloop: sta TMAP,x
        inx
        inx
        cpx #(COLS * 32 * 2)
        bne @tloop
        sep #$20
.a8
        rts
.endproc

; ============================================================================
; DMA the tile blob to BG1 char base (A 8-bit on entry, forced blank)
; ============================================================================
.proc LoadTiles
.a8
        rep #$20
.a16
        ldx #VRAM_CHR
        stx VMADDL
        sep #$20
.a8
        lda #$01
        sta DMAP0               ; mode 1: write pairs to $2118/$2119
        lda #<VMDATAL
        sta BBAD0               ; B-bus dest = $2118
        rep #$20
.a16
        ldx #.loword(tiles_all)
        stx A1T0L
        ldx #TILES_BYTES
        stx DAS0L
        sep #$20
.a8
        lda #^tiles_all
        sta A1B0                ; source bank
        lda #$01
        sta MDMAEN              ; fire channel 0
        rts
.endproc

; ============================================================================
; DMA the palette to CGRAM (A 8-bit on entry, forced blank)
; ============================================================================
.proc LoadPalette
.a8
        stz CGADD               ; CGRAM word address 0
        lda #$00
        sta DMAP0               ; mode 0: single register, write once
        lda #<CGDATA
        sta BBAD0               ; B-bus dest = $2122
        rep #$20
.a16
        ldx #.loword(pal0)
        stx A1T0L
        ldx #32                 ; 16 colours * 2 bytes
        stx DAS0L
        sep #$20
.a8
        lda #^pal0
        sta A1B0
        lda #$01
        sta MDMAEN
        rts
.endproc

; ============================================================================
; Flush the whole tilemap shadow to VRAM (called in forced blank or vblank)
; ============================================================================
.proc FlushTilemap
.a8
        rep #$20
.a16
        ldx #VRAM_MAP
        stx VMADDL
        sep #$20
.a8
        lda #$01
        sta DMAP0
        lda #<VMDATAL
        sta BBAD0
        rep #$20
.a16
        ldx #.loword(TMAP)
        stx A1T0L
        ldx #(COLS * 32 * 2)
        stx DAS0L
        sep #$20
.a8
        lda #TMAP_BANK
        sta A1B0
        lda #$01
        sta MDMAEN
        rts
.endproc

; ============================================================================
; draw_str: ZP $00 = string ptr (bank 0 / ROM low), X = shadow byte offset.
; Writes palette-0 tile words until a 0 terminator. A/X/Y 16-bit on entry.
; ============================================================================
.proc draw_str
.a16
.i16
        ldy #$0000
@loop:  sep #$20
.a8
        lda ($00),y             ; next char
        beq @done
        sec
        sbc #32                 ; glyph index
        rep #$20
.a16
        and #$00FF
        clc
        adc #T_FONT             ; tile = T_FONT + (char-32)
        sta TMAP,x
        inx
        inx
        iny
        bra @loop
@done:  rep #$20
.a16
        rts
.endproc

.macro DRAWSTR strlbl, col, row
        rep #$20
        ldx #.loword(strlbl)
        stx $00
        ldx #(row * 64 + col * 2)
        jsr draw_str
.endmacro

; ============================================================================
; DrawSplash: fill the desktop, draw the static splash text (A 8-bit on entry)
; ============================================================================
.proc DrawSplash
.a8
        rep #$30
.a16
.i16
        ; fill the visible field with the solid desktop cell
        lda #T_SOLBG
        ldx #$0000
@fill:  sta TMAP,x
        inx
        inx
        cpx #(COLS * 32 * 2)
        bne @fill

        DRAWSTR s_title,  12,  9
        DRAWSTR s_sub,     8, 11
        DRAWSTR s_port,   10, 13
        DRAWSTR s_pad,     9, 16
        DRAWSTR s_hint,    4, 22

.ifdef AUTOTEST
        DRAWSTR s_auto,   11, 19
.endif

        sep #$20
.a8
        lda #$01
        sta v_dirty
        rts
.endproc

; ============================================================================
; UpdatePadLine: render the current joypad word as hex into the PAD: line.
; (main-loop context; only touches the shadow + sets the dirty flag)
; ============================================================================
.proc UpdatePadLine
.a8
        rep #$20
.a16
.i16
        lda v_pad
        jsr Word2Hex            ; HEXBUF[0..3] = ascii hex digits
        ; draw the 4 digits at column 14 of the PAD row (row 16)
        ldx #(16 * 64 + 14 * 2)
        ldy #$0000
@d:     sep #$20
.a8
        lda HEXBUF,y
        sec
        sbc #32
        rep #$20
.a16
        and #$00FF
        clc
        adc #T_FONT
        sta TMAP,x
        inx
        inx
        iny
        cpy #$0004
        bne @d
        sep #$20
.a8
        lda #$01
        sta v_dirty
        rts
.endproc

; ----------------------------------------------------------------------------
; Word2Hex: A (16-bit) -> 4 ascii hex chars in HEXBUF (MSdigit first).
; A/X/Y 16-bit on entry; preserves nothing of note.
; ----------------------------------------------------------------------------
.proc Word2Hex
.a16
.i16
        sta $02                 ; stash the value
        ldy #$0000              ; digit index 0..3
@next:  lda $02
        ; rotate so the top nibble is in the low 4 bits
        ; shift left a nibble count based on (3 - y)*4 ... simpler: peel MSN each pass
        ldx #$0000              ; (unused) keep i16 happy
        ; isolate high nibble
        and #$F000
        ; move it down to bits 0..3
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
        lsr a
        lsr a
        ; A = high nibble (0..15)
        sep #$20
.a8
        cmp #10
        bcc @dig
        adc #('A' - 10 - 1)     ; +carry set -> 'A'..'F'
        bra @put
@dig:   adc #'0'
@put:   sta HEXBUF,y
        rep #$20
.a16
        ; shift the value left one nibble for the next pass
        lda $02
        asl a
        asl a
        asl a
        asl a
        sta $02
        iny
        cpy #$0004
        bne @next
        rts
.endproc

; ============================================================================
; NMI (vblank): ack, count, sample input, flush the shadow if dirty.
; No app/WM logic here (PORT-SPEC SS6 rule 2) - transfers + input only.
; ============================================================================
.proc NMI
        rep #$30
.a16
.i16
        pha
        phx
        phy
        phb
        phd
        lda #$0000
        tcd                     ; DP = 0
        sep #$20
.a8
        lda #$00
        pha
        plb                     ; DB = 0
        lda RDNMI               ; ack NMI

        rep #$20
.a16
        inc v_ticks
        inc v_frame

        ; wait out the auto-joypad read, then latch JOY1
        sep #$20
.a8
@wait:  lda HVBJOY
        and #$01
        bne @wait
        rep #$20
.a16
        lda v_pad
        sta v_padprev
        lda JOY1L
        sta v_pad

.ifdef AUTOTEST
        ; deterministic synthetic input: prove the read->shadow->DMA->display
        ; pipeline without depending on emulator input injection. After ~1.5s
        ; "press" a recognisable pattern so the PAD: line changes on screen.
        lda v_frame
        cmp #90
        bcc @noauto
        lda #$C0A0              ; B + Start + A + L  -> "PAD:C0A0"
        sta v_pad
@noauto:
.endif

        ; flush the tilemap shadow if the main loop dirtied it
        sep #$20
.a8
        lda v_dirty
        beq @nodma
        stz v_dirty
        jsr FlushTilemap
@nodma:
        rep #$30
.a16
.i16
        pld
        plb
        ply
        plx
        pla
        rti
.endproc

; ============================================================================
; Strings (bank 0 ROM, ascii, 0-terminated)
; ============================================================================
.segment "RODATA"
s_title: .byte "UnoDOS 3", 0
s_sub:   .byte "Bare-metal desktop", 0
s_port:  .byte "SNES port - M0", 0
s_pad:   .byte "PAD: 0000", 0
s_hint:  .byte "D-pad moves - any button changes PAD", 0
s_auto:  .byte "* AUTOTEST *", 0

; ============================================================================
; Cartridge header ($00:FFC0) + interrupt vectors ($00:FFE0)
; ============================================================================
.segment "SNESHEADER"
        ;       123456789012345678901   (21 bytes, $FFC0-$FFD4)
        .byte "UNODOS 3 - SNES PORT "
        .byte $20               ; $FFD5 map mode: LoROM, slow ROM
        .byte $00               ; $FFD6 cartridge type: ROM only (SRAM at M2)
        .byte $05               ; $FFD7 ROM size: 32 KB (2^5 KB)
        .byte $00               ; $FFD8 SRAM size: none
        .byte $01               ; $FFD9 country: NTSC (US)
        .byte $00               ; $FFDA developer id
        .byte $00               ; $FFDB version
        .byte $00,$00           ; $FFDC checksum complement (patched by build.sh)
        .byte $00,$00           ; $FFDE checksum            (patched by build.sh)

.segment "SNESVECTORS"
        ; native mode ($FFE0)
        .word $0000, $0000      ; reserved
        .word $0000             ; COP
        .word $0000             ; BRK
        .word $0000             ; ABORT
        .word NMI               ; NMI
        .word $0000             ; reserved
        .word $0000             ; IRQ
        ; emulation mode ($FFF0)
        .word $0000, $0000      ; reserved
        .word $0000             ; COP
        .word $0000             ; reserved
        .word $0000             ; ABORT
        .word $0000             ; NMI
        .word Reset             ; RESET
        .word $0000             ; IRQ/BRK
