; ============================================================================
; UnoDOS / Nintendo Entertainment System — milestones 1-3.
; ============================================================================
; A bare-metal UnoDOS for the NES (6502/2A03 @ 1.79 MHz, PPU 2C02, 2 KB RAM).
; The NES is the Contract's MINIMAL profile (CONTRACT-ARCH §9): 2 KB RAM, no
; mouse, NO window manager — ONE full-screen app at a time with directional
; navigation (§8). The SECOND port built fresh on the contract-driven +
; greenfield architecture (the SMS port was the first); it adapts the SMS event
; loop / Dostris / audio patterns to the pointer-less minimal model.
;
; M1: boot to a rendered full-screen launcher (title bar + labelled icon grid).
; M2: read the standard pad ($4016), a vblank-NMI per-frame loop, a directional
;     SELECTION highlight (d-pad moves it, the selected label inverts), A launches
;     the selected app full-screen, B returns to the launcher.
; M3: full-screen apps — SysInfo, live Clock, Notepad, Files, Theme (palette
;     cycling), Music (2A03 APU pulse), the Dostris falling-blocks game, and
;     generic placeholders. One app is resident at a time, dispatched by proc.
;
; Rendering model (the §9 minimal floor): the NMI is minimal (a 60 Hz tick + a
; vblank flag, NO PPU access). The main loop syncs to vblank, then does SMALL
; partial nametable writes (highlight move, clock tick, falling piece, palette)
; that fit inside vblank — flicker-free. Big screen changes (launcher<->app,
; Dostris board on lock) redraw the whole nametable with rendering OFF. Because
; the NMI never touches the PPU there is no ISR/main-loop reentrancy on VRAM.
;
; Contract-owned (CONTRACT-ARCH Phase 4): the tile screen geometry comes from
; unogen ([world.nes] -> gen/nes/sys_gen.inc) so it cannot drift.
; ============================================================================

        processor 6502

        include "build/cfg.inc"                   ; AUTOTEST / AT_* build switches
        include "../unodef/gen/6502/unodef.inc"   ; the Contract surface (SYS_*, enums)
        include "../unodef/gen/nes/sys_gen.inc"    ; SCRCOLS / SCRROWS

; ---- PPU / APU / pad registers ---------------------------------------------
PPUCTRL   = $2000
PPUMASK   = $2001
PPUSTATUS = $2002
PPUSCROLL = $2005
PPUADDR   = $2006
PPUDATA   = $2007
APUSTATUS = $4015
JOYPAD1   = $4016

; standard controller bits after the 8-bit shift (A ends in bit7)
PAD_A     = $80
PAD_B     = $40
PAD_SEL   = $20
PAD_ST    = $10
PAD_U     = $08
PAD_D     = $04
PAD_L     = $02
PAD_R     = $01

; ---- Dostris geometry ------------------------------------------------------
BW        = 10          ; board width (cells)
BH        = 12          ; board height (cells)
BORG_COL  = 2           ; board origin: top-left so it sits in the visible grab
BORG_ROW  = 3
FALLRATE  = 30          ; frames per gravity step (~0.5 s)

; ---- zero page -------------------------------------------------------------
ptr       = $00         ; word: string pointer (puts)
ptr2      = $02         ; word: content-table pointer (draw_content)
col       = $04
row       = $05
tmp       = $06         ; tile base for puts
addrlo    = $08
addrhi    = $09
tile      = $0A         ; draw_icon: TL tile
idx       = $0B         ; icon/label index
grp       = $0C         ; icon row group

v_pad     = $10         ; current pad
v_padp    = $11         ; previous pad
v_pade    = $12         ; pressed edges (new & ~prev)
v_inapp   = $13         ; 0 = launcher, 1 = an app is resident
v_sel     = $14         ; selected icon 0..NICONS-1
v_selp    = $15         ; previous selection (for highlight erase)
v_vbl     = $16         ; NMI sets to 1 each frame
v_tick    = $17         ; word: 60 Hz frame counter
v_dirty   = $19         ; full (rendering-off) redraw requested
v_app     = $1A         ; current app proc when in-app

v_frac    = $1B         ; clock: frames within the current second
v_ss      = $1C
v_mm      = $1D
v_hh      = $1E
v_theme   = $1F         ; theme preset index

m_idx     = $20         ; music: current note
m_timer   = $21         ; music: frames left on the current note
m_play    = $22         ; music: non-zero while playing

a_idx     = $23         ; AUTOTEST: script step index
a_tmr     = $24         ; AUTOTEST: frames left on the current step
a_pad     = $25         ; AUTOTEST: pad value for the current step
a_gpause  = $26         ; AUTOTEST: freeze game gravity once the script ends

pf_hl     = $28         ; partial: highlight moved
pf_clk    = $29         ; partial: clock string changed
pf_pal    = $2A         ; partial: theme palette changed
pf_pc     = $2B         ; partial: Dostris piece moved
pf_score  = $2C         ; partial: Dostris/Music status changed

; ---- Dostris game state ----
g_type    = $30
g_rot     = $31
g_px      = $32
g_py      = $33
g_state   = $34         ; 0 = playing, 1 = game over
g_fall    = $35
g_lines   = $36
g_seed    = $37
g_tx      = $39         ; piece_collide / lock trial origin
g_ty      = $3A
g_tmp     = $3B
g_srot    = $3C         ; rotate: saved rotation
g_row     = $3D
g_lt      = $3E         ; lock tile
g_pt      = $3F         ; piece tile
mlo       = $40         ; current 16-bit mask, working copy
mhi       = $41
g_oldpx   = $42         ; previous piece pos (for the partial erase)
g_oldpy   = $43
g_oldrot  = $44
g_bx      = $45
g_by      = $46
g_n       = $47         ; scratch

numstr    = $50         ; 4 bytes: decimal scratch
clk_str   = $54         ; "HH:MM:SS",0 (9 bytes -> $5C)

g_board   = $0400       ; BW*BH = 120 bytes (0 = empty, else a block tile)

        org $8000
; ============================================================================
; reset / boot
; ============================================================================
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

        ; initial OS state
        lda #0
        sta v_inapp
        sta v_sel
        sta v_selp
        sta v_theme
        sta v_dirty

        jsr clear_nametable
        jsr draw_launcher       ; rendering still off
        jsr scroll_reset

        lda #$0A
        sta PPUMASK             ; show background (+ leftmost 8px)
        lda #$80
        sta PPUCTRL             ; nametable 0, bg pattern 0, NMI on

; ============================================================================
; main loop — sync to vblank, flush small partials, then run a frame of logic
; ============================================================================
main:
        jsr wait_vbl            ; resume at vblank start (NMI set the flag)
        jsr render_partials     ; SMALL nametable writes that fit in vblank
        jsr scroll_reset
        jsr read_pad
        jsr clock_advance       ; the clock runs whether or not its app is open
        jsr update              ; mode-specific logic -> sets v_dirty / pf_*
        lda v_dirty
        beq main
        jsr full_redraw         ; rendering-off whole-screen redraw
        jmp main

; ---- wait_vbl: spin until the NMI signals the next frame --------------------
wait_vbl:
        lda #0
        sta v_vbl
.w:     lda v_vbl
        beq .w
        rts

; ---- scroll_reset: restore the top-left view after any PPUADDR write --------
scroll_reset:
        bit PPUSTATUS           ; reset the address latch
        lda #$80
        sta PPUCTRL             ; NMI on, nametable 0, bg pattern 0
        lda #0
        sta PPUSCROLL
        sta PPUSCROLL           ; scroll X=0, Y=0
        rts

; ---- read_pad: standard controller -> v_pad + v_pade (pressed edges) --------
read_pad:
  IF AUTOTEST
        jmp auto_input
  ENDIF
        lda v_pad
        sta v_padp
        lda #1
        sta JOYPAD1             ; strobe high
        lda #0
        sta JOYPAD1             ; strobe low -> latch
        ldx #8
.rp:    lda JOYPAD1
        lsr                     ; bit0 -> carry
        rol v_pad               ; carry -> v_pad, shifting A..Right into place
        dex
        bne .rp
        ; v_pad: bit7=A 6=B 5=Sel 4=Start 3=Up 2=Down 1=Left 0=Right
        lda v_padp
        eor #$FF
        and v_pad
        sta v_pade
        rts

; ============================================================================
; update: per-frame logic (no PPU writes here)
; ============================================================================
update:
        lda v_inapp
        bne up_app
        jmp nav_input           ; launcher: directional select + A launches
up_app:
        lda v_pade
        and #PAD_B
        beq up_disp
        jmp enter_launcher      ; B returns to the launcher
up_disp:
        lda v_app
        cmp #3
        bne ud1
        jmp music_tick
ud1:    cmp #5
        bne ud2
        jmp theme_input
ud2:    cmp #7
        bne ud3
        jmp dostris_update
ud3:    rts                     ; clock (proc 1): clock_advance already ran

; ============================================================================
; launcher navigation (M2)
; ============================================================================
nav_input:
        lda v_pade
        and #PAD_A
        beq nav_dir
        ; launch the selected app
        lda v_sel
        sta v_app
        lda #1
        sta v_inapp
        jmp enter_app
nav_dir:
        lda v_pade
        and #PAD_R
        beq nd_l
        jsr sel_right
        rts                     ; B2: one direction per frame - a diagonal must
nd_l:   lda v_pade              ; not run a second sel_* and overwrite v_selp
        and #PAD_L
        beq nd_d
        jsr sel_left
        rts
nd_d:   lda v_pade
        and #PAD_D
        beq nd_u
        jsr sel_down
        rts
nd_u:   lda v_pade
        and #PAD_U
        beq nd_done
        jsr sel_up
nd_done:
        rts

sel_right:
        lda v_sel
        sta v_selp
        clc
        adc #1
        cmp #NICONS
        bcc sr_s
        lda #0
sr_s:   sta v_sel
        jmp mark_hl
sel_left:
        lda v_sel
        sta v_selp
        bne sl_d
        lda #NICONS
sl_d:   sec
        sbc #1
        sta v_sel
        jmp mark_hl
sel_down:
        lda v_sel
        clc
        adc #4
        cmp #NICONS
        bcs sd_no               ; would leave the grid -> no move
        pha
        lda v_sel
        sta v_selp
        pla
        sta v_sel
        jmp mark_hl
sd_no:  rts
sel_up:
        lda v_sel
        cmp #4
        bcc su_no
        sta v_selp
        sec
        sbc #4
        sta v_sel
        jmp mark_hl
su_no:  rts
mark_hl:
        lda #1
        sta pf_hl
        rts

; ---- enter_app: prep app state, request a full redraw ----------------------
enter_app:
        lda v_app
        cmp #7
        bne ea_music
        jsr dostris_init
ea_music:
        lda v_app
        cmp #3
        bne ea_done
        jsr music_init
ea_done:
        lda #1
        sta v_dirty
        rts

; ---- enter_launcher: leave the app, restore the desktop --------------------
enter_launcher:
        lda #0
        sta v_inapp
        jsr music_silence       ; ensure the APU is quiet on the way out
        lda #1
        sta v_dirty
        rts

; ============================================================================
; render_partials: small vblank-time writes (we are right after wait_vbl)
; ============================================================================
render_partials:
        lda v_inapp
        bne rp_app
        ; launcher: move the selection highlight
        lda pf_hl
        bne rp_hl
        rts
rp_hl:  jsr draw_highlight
        lda #0
        sta pf_hl
        rts
rp_app:
        lda v_app
        cmp #1
        bne rpa1
        jmp rp_clock
rpa1:   cmp #3
        bne rpa2
        jmp rp_music
rpa2:   cmp #5
        bne rpa3
        jmp rp_theme
rpa3:   cmp #7
        bne rpa4
        jmp rp_dostris
rpa4:   rts
rp_clock:
        lda pf_clk
        bne rpc
        rts
rpc:    jsr draw_clock_time
        lda #0
        sta pf_clk
        rts
rp_music:
        lda pf_score
        bne rpm
        rts
rpm:    jsr draw_music_status
        lda #0
        sta pf_score
        rts
rp_theme:
        lda pf_pal
        bne rpt
        rts
rpt:    jsr draw_theme_status
        lda #0
        sta pf_pal
        rts
rp_dostris:
        lda pf_pc
        bne rpd
        rts
rpd:    jsr draw_piece_partial
        lda #0
        sta pf_pc
        rts

; ---- draw_highlight: old label normal, new label inverted ------------------
draw_highlight:
        lda v_selp
        jsr label_pos
        lda #T_FONT
        sta tmp
        jsr puts
        lda v_sel
        jsr label_pos
        lda #T_FONTINV
        sta tmp
        jmp puts

; ============================================================================
; full_redraw: rendering-off whole-screen redraw (launcher or app)
; ============================================================================
full_redraw:
        lda #0
        sta v_dirty
        sta pf_hl
        sta pf_pc
        lda #0
        sta PPUMASK             ; rendering off (NMI keeps ticking, never touches PPU)
        jsr clear_nametable
        lda v_inapp
        bne fr_app
        jsr draw_launcher
        jmp fr_on
fr_app:
        jsr draw_app
fr_on:
        jsr scroll_reset
        lda #$0A
        sta PPUMASK             ; rendering on
        rts

; ============================================================================
; low-level PPU helpers
; ============================================================================
; ---- load_palette: the 4-colour background palette to $3F00 ----------------
load_palette:
        bit PPUSTATUS
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
        bit PPUSTATUS
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
; Unrolled (no X) so callers can keep a loop counter in X across this call.
set_nt_addr:
        lda row
        sta addrlo
        lda #0
        sta addrhi
        asl addrlo
        rol addrhi
        asl addrlo
        rol addrhi
        asl addrlo
        rol addrhi
        asl addrlo
        rol addrhi
        asl addrlo
        rol addrhi              ; row << 5
        lda addrlo
        clc
        adc col
        sta addrlo
        lda addrhi
        adc #0
        clc
        adc #$20                ; + $2000
        sta addrhi
        bit PPUSTATUS
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

; ---- cputs: ptr/col/row set by caller; draw in the normal font -------------
cputs:
        lda #T_FONT
        sta tmp
        jmp puts

; ---- draw_content: ptr2 -> table of {db col,row ; dw str}, end col=$FF ------
draw_content:
        ldy #0
.dcl:   lda (ptr2),y
        cmp #$FF
        beq .dcd
        sta col
        iny
        lda (ptr2),y
        sta row
        iny
        lda (ptr2),y
        sta ptr
        iny
        lda (ptr2),y
        sta ptr+1
        iny
        tya
        pha                     ; puts clobbers Y
        jsr cputs
        pla
        tay
        jmp .dcl
.dcd:   rts

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

; ---- label_pos: A=icon index -> ptr=label, col, row(label row) -------------
label_pos:
        sta idx
        and #3
        asl
        asl
        asl                     ; (idx&3)*8
        clc
        adc #1
        sta col
        lda idx
        lsr
        lsr                     ; group = idx>>2
        sta grp
        asl
        asl                     ; 4*grp
        clc
        adc grp                 ; 5
        adc grp                 ; 6
        adc grp                 ; 7*grp
        clc
        adc #3                  ; icon row
        clc
        adc #2                  ; label row = iconrow + 2
        sta row
        lda idx
        asl
        tay
        lda icon_lbl,y
        sta ptr
        lda icon_lbl+1,y
        sta ptr+1
        rts

; ============================================================================
; draw_launcher: title bar + the labelled icon grid (selected label inverted)
; ============================================================================
draw_launcher:
        jsr load_palette        ; restore the default palette (theme may have changed it)
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
        asl
        clc
        adc grp
        adc grp
        adc grp
        clc
        adc #3
        sta row
        lda idx
        jsr draw_icon
        ; label at (col, row+2): inverted if selected, else normal
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
        ldy idx
        cpy v_sel
        bne .nf
        lda #T_FONTINV
.nf:    sta tmp
        jsr puts
        ldx idx
        inx
        cpx #NICONS
        bne .ig
        rts

; ============================================================================
; app frame chrome: title bar (ptr=title) + "B = Back" footer
; ============================================================================
draw_chrome:
        ; title bar (row 0) white fill
        lda #0
        sta col
        sta row
        jsr set_nt_addr
        ldx #SCRCOLS
        lda #T_WHITE
.cdtb:  sta PPUDATA
        dex
        bne .cdtb
        ; title (ptr) inverted at (1,0)
        lda #1
        sta col
        lda #0
        sta row
        lda #T_FONTINV
        sta tmp
        jsr puts
        ; footer hint at the bottom row
        lda #<s_back
        sta ptr
        lda #>s_back
        sta ptr+1
        lda #1
        sta col
        lda #SCRROWS-1
        sta row
        lda #T_FONT
        sta tmp
        jmp puts

; ---- two_digits: A=0..99 -> X=tens char, A=units char ----------------------
two_digits:
        ldx #$30                ; '0'
.tdt:   cmp #10
        bcc .tdd
        sbc #10
        inx
        jmp .tdt
.tdd:   clc
        adc #$30                ; '0'
        rts

; ---- includes --------------------------------------------------------------
        include "apps.inc"         ; app draws, clock/theme/music, AUTOTEST script + data
        include "dostris.inc"      ; the Dostris (falling-blocks) game
        include "nes_data.inc"     ; T_*/NICONS/palette + piece + music tables

; ---- interrupt vectors -----------------------------------------------------
; The NMI is minimal: a 60 Hz tick + a vblank flag. It NEVER touches the PPU,
; so it can never race the main loop's VRAM writes (PORT-SPEC ISR rule).
nmi:    pha
        inc v_tick
        bne .nz
        inc v_tick+1
.nz:    lda #1
        sta v_vbl
        pla
        rti
irq:    rti

        ds $FFFA-*, $FF
        dc.w nmi                ; $FFFA  NMI
        dc.w reset              ; $FFFC  RESET
        dc.w irq                ; $FFFE  IRQ
