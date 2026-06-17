; ============================================================================
; UnoDOS / Nintendo Entertainment System — milestone 1: boot to a launcher.
; ============================================================================
; A bare-metal UnoDOS for the NES (6502/2A03 @ 1.79 MHz, PPU 2C02, 2 KB RAM).
; The NES is the Contract's MINIMAL profile (CONTRACT-ARCH §9): 2 KB RAM, no
; mouse, no window manager — one full-screen launcher with directional nav.
; Built with dasm into an iNES NROM-256 image (32 KB PRG + 8 KB CHR).
;
; Display model: the PPU composes a 32x30 tile nametable from CHR-ROM patterns;
; tiles store palette indices and one background palette ({blue, white, cyan,
; magenta}) colours them (the attribute table is all zeros). The shared 8x8
; font and the x86 icon donors live in CHR-ROM (no runtime tile upload). M1
; draws the launcher: the inverted "UnoDOS 3" title bar + the labelled icon grid.
;
; Contract-owned (CONTRACT-ARCH Phase 4): the tile screen geometry is generated
; by unogen from unodef/unodef.toml ([world.nes]) so it cannot drift.
; ============================================================================

        processor 6502

        include "../unodef/gen/6502/unodef.inc"   ; the Contract surface (SYS_*, enums)
        include "../unodef/gen/nes/sys_gen.inc"    ; SCRCOLS / SCRROWS

PPUCTRL   = $2000
PPUMASK   = $2001
PPUSTATUS = $2002
PPUADDR   = $2006
PPUDATA   = $2007

; zero page
ptr       = $00         ; word: string pointer
col       = $04
row       = $05
tmp       = $06         ; tile base for puts
addrlo    = $08
addrhi    = $09
tile      = $0A         ; draw_icon: TL tile
idx       = $0B         ; icon index
grp       = $0C         ; icon row group

        org $8000
reset:
        sei
        cld
        ldx #$FF
        txs
        inx                     ; x = 0
        stx PPUCTRL             ; NMI off
        stx PPUMASK             ; rendering off
        bit PPUSTATUS
.w1:    bit PPUSTATUS           ; wait for the first vblank
        bpl .w1
        ; clear 2 KB work RAM
        lda #0
        tax
.clr:   sta $0000,x
        sta $0100,x
        sta $0200,x
        sta $0300,x
        sta $0400,x
        sta $0500,x
        sta $0600,x
        sta $0700,x
        inx
        bne .clr
.w2:    bit PPUSTATUS           ; wait for the second vblank (PPU warm)
        bpl .w2

        jsr load_palette
        jsr clear_nametable
        jsr draw_desktop

        ; turn the picture on
        lda #$00
        sta PPUCTRL             ; nametable 0, bg pattern table 0, NMI off
        lda #$0A
        sta PPUMASK             ; show background (+ leftmost 8px)
        lda #$00
        sta $2005
        sta $2005               ; scroll 0,0
loop:   jmp loop

; ---- load_palette: write the 4-colour background palette to $3F00 ----------
load_palette:
        lda PPUSTATUS           ; reset the address latch
        lda #$3F
        sta PPUADDR
        lda #$00
        sta PPUADDR
        ldx #0
.lp:    lda palette,x
        sta PPUDATA
        inx
        cpx #4
        bne .lp
        rts

; ---- clear_nametable: $2000-$23FF (tiles + attributes) to 0 ----------------
clear_nametable:
        lda PPUSTATUS
        lda #$20
        sta PPUADDR
        lda #$00
        sta PPUADDR
        lda #0
        ldy #4                  ; 4 x 256 = 1024 bytes
        ldx #0
.cn:    sta PPUDATA
        inx
        bne .cn
        dey
        bne .cn
        rts

; ---- set_nt_addr: PPUADDR = $2000 + row*32 + col (uses col,row) ------------
set_nt_addr:
        lda row
        sta addrlo
        lda #0
        sta addrhi
        ldx #5                  ; row << 5  (16-bit)
.s:     asl addrlo
        rol addrhi
        dex
        bne .s
        lda addrlo
        clc
        adc col
        sta addrlo
        lda addrhi
        adc #0
        sta addrhi
        clc
        adc #$20                ; + $2000
        sta addrhi
        lda PPUSTATUS
        lda addrhi
        sta PPUADDR
        lda addrlo
        sta PPUADDR
        rts

; ---- puts: ptr=string (NUL-term), col/row, tmp=tile base -------------------
puts:
        jsr set_nt_addr
        ldy #0
.pl:    lda (ptr),y
        beq .done
        sec
        sbc #32
        clc
        adc tmp
        sta PPUDATA
        iny
        bne .pl
.done:  rts

; ---- draw_icon: A=icon index, col/row -> 2x2 tile block --------------------
draw_icon:
        asl
        asl                     ; icon*4
        clc
        adc #T_ICONS
        sta tile                ; TL
        jsr set_nt_addr
        lda tile
        sta PPUDATA             ; TL
        clc
        adc #1
        sta PPUDATA             ; TR (PPUDATA auto-increments)
        inc row
        jsr set_nt_addr
        lda tile
        clc
        adc #2
        sta PPUDATA             ; BL
        lda tile
        clc
        adc #3
        sta PPUDATA             ; BR
        dec row
        rts

; ---- draw_desktop: title bar + the labelled icon grid ----------------------
draw_desktop:
        ; title bar (row 0): 32 white blocks
        lda #0
        sta col
        sta row
        jsr set_nt_addr
        ldx #SCRCOLS
        lda #T_WHITE
.tb:    sta PPUDATA
        dex
        bne .tb
        ; title text "UnoDOS 3" (inverted) at (1,0)
        lda #<s_title
        sta ptr
        lda #>s_title
        sta ptr+1
        lda #1
        sta col
        lda #0
        sta row
        lda #T_FONTINV
        sta tmp
        jsr puts
        ; icon grid
        ldx #0
.ig:    stx idx
        ; col = (idx & 3)*8 + 1
        txa
        and #3
        asl
        asl
        asl
        clc
        adc #1
        sta col
        ; row = 3 + 7*(idx/4)
        txa
        lsr
        lsr
        sta grp
        asl
        asl                     ; 4*grp
        clc
        adc grp                 ; 5
        adc grp                 ; 6
        adc grp                 ; 7
        clc
        adc #3
        sta row
        lda idx
        jsr draw_icon
        ; label at (col, row+2)
        lda row
        clc
        adc #2
        sta row
        lda idx
        asl
        tay
        lda icon_lbl,y
        sta ptr
        lda icon_lbl+1,y
        sta ptr+1
        lda #T_FONT
        sta tmp
        jsr puts
        ldx idx
        inx
        cpx #NICONS
        bne .ig
        rts

; ---- strings ---------------------------------------------------------------
s_title:   dc.b "UnoDOS 3",0
icon_lbl:
        dc.w l_sysinfo, l_clock, l_notepad, l_music, l_files, l_theme
        dc.w l_tracker, l_dostris, l_outlast, l_pacman, l_paint
l_sysinfo: dc.b "SysInfo",0
l_clock:   dc.b "Clock",0
l_notepad: dc.b "Notepad",0
l_music:   dc.b "Music",0
l_files:   dc.b "Files",0
l_theme:   dc.b "Theme",0
l_tracker: dc.b "Tracker",0
l_dostris: dc.b "Dostris",0
l_outlast: dc.b "OutLast",0
l_pacman:  dc.b "Pac-Man",0
l_paint:   dc.b "Paint",0

        include "nes_data.inc"     ; T_FONT/T_FONTINV/T_WHITE/T_ICONS/NICONS + palette

; ---- interrupt vectors -----------------------------------------------------
nmi:
irq:    rti

        ds $FFFA-*, $FF
        dc.w nmi                ; $FFFA  NMI
        dc.w reset              ; $FFFC  RESET
        dc.w irq                ; $FFFE  IRQ
