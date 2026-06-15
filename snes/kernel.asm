; ============================================================================
; UnoDOS/SNES - milestone 1: tile desktop, window manager, hardware-sprite
; cursor, pad-as-pointer + soft keyboard, SysInfo + Clock.  The Genesis M1
; surface (genesis/kernel.asm + softkbd.i) re-expressed in 65816 on the SNES
; shadow+DMA architecture (snes/HANDOFF.md SS2).
;
; Rendering: the main loop draws into a WRAM tilemap shadow ($7E:1000) and
; sets a dirty flag; the vblank NMI DMAs the shadow to VRAM, samples the
; joypad, drives the cursor sprite (OAM), and ticks the clock.  No app/WM
; logic runs in the NMI (PORT-SPEC SS6 rule 2) - it only transfers bytes and
; latches input state.  The NMI runs on its OWN direct page ($0100) so its
; scratch never collides with the main loop's ($0000).
;
; Display: Mode 1, BG1 = the desktop plane. 256x224 => 32x28 cells (Genesis
; is 40x28; windows are narrower by 8 columns - documented deviation).
; ============================================================================

.p816
.smart +

.include "gen_data.inc"

; ----------------------------------------------------------------- PPU/CPU regs
INIDISP = $2100
OBJSEL  = $2101
OAMADDL = $2102
OAMDATA = $2104
BGMODE  = $2105
BG1SC   = $2107
BG12NBA = $210B
BG1HOFS = $210D
BG1VOFS = $210E
VMAIN   = $2115
VMADDL  = $2116
VMDATAL = $2118
CGADD   = $2121
CGDATA  = $2122
TM      = $212C
RDNMI   = $4210
HVBJOY  = $4212
NMITIMEN = $4200
JOY1L   = $4218
MDMAEN  = $420B
DMAP0   = $4300
BBAD0   = $4301
A1T0L   = $4302
A1B0    = $4304
DAS0L   = $4305

; ----------------------------------------------------------------- memory map
; Low 8 KB ($0000-$1FFF) mirrors WRAM $7E:0000-$1FFF - kernel state lives here.
;   $0000-$00FF  main-loop direct page (pseudo-registers + scratch)
;   $0100-$01FF  NMI direct page (isolated scratch)
;   $0200-$03FF  VARS (state + window table + buffers)
;   $1000-$17FF  tilemap shadow (32x32 words)
;   $1A00-$1C1F  OAM shadow (512 low + 32 high)
;   $1Fxx        stack
WRAM_BANK = $7E
TMAP    = $1000
OAMSH   = $1A00
NMI_DP  = $0100

; ---- main-loop pseudo-registers (direct page, D=0) ----
P0  = $00          ; 16-bit pointer
P1  = $10          ; 16-bit pointer
A0  = $02          ; general 16-bit args/scratch (mirror of d0..)
A1  = $04
A2  = $06
A3  = $08
A4  = $0A
A5  = $0C
A6  = $0E
S0  = $12          ; loop scratch
S1  = $14
S2  = $16
S3  = $18
S4  = $1A
S5  = $1C
S6  = $1E
S7  = $20
DVQ = $22          ; div16 private quotient (so callers keep S4/S5)
LC0 = $24          ; outer-loop counters - NO draw routine touches these
LC1 = $26

; ---- VARS ----
VARS    = $0200
v_magic     = VARS+$00
v_frame     = VARS+$04
v_frac      = VARS+$06
v_secs      = VARS+$08
v_last_secs = VARS+$0A
v_dbl_frame = VARS+$0C
v_mouse_x   = VARS+$0E
v_mouse_y   = VARS+$10
v_click_x   = VARS+$12
v_click_y   = VARS+$14
v_pad       = VARS+$16
v_padprev   = VARS+$18
v_pad_edge  = VARS+$1A
v_pad_heldn = VARS+$1C
v_mouse_btn = VARS+$1E
v_last_btn  = VARS+$20
v_click_seq = VARS+$22
v_last_seq  = VARS+$24
v_drag_active = VARS+$26
v_drag_win  = VARS+$28
v_drag_offx = VARS+$2A
v_drag_offy = VARS+$2C
v_ev_head   = VARS+$2E
v_ev_tail   = VARS+$30
v_zcount    = VARS+$32
v_sel_icon  = VARS+$34
v_dbl_icon  = VARS+$36
v_kb_vis    = VARS+$38
v_kb_shift  = VARS+$3A
v_kb_toggle = VARS+$3C
v_kb_hover  = VARS+$3E
v_dirty     = VARS+$40
v_auto      = VARS+$42
v_mouse_present = VARS+$44
v_numbuf    = VARS+$60
v_clkbuf    = VARS+$70
v_zlist     = VARS+$80
v_wintab    = VARS+$90
v_evq       = VARS+$100
; ---- M2 storage state ----
v_np_len    = VARS+$180
v_np_dirty  = VARS+$182
v_files_sel = VARS+$184
v_np_name   = VARS+$186     ; 13 bytes (12 name + NUL)
v_npbuf     = $0400         ; 2 KB Notepad buffer ($0400-$0BFF)
NBUF        = 2048
; ---- game state (Dostris/Pac-Man/OutLast) ----
v_dt_state  = VARS+$1A0     ; 0 menu 1 play 2 pause 3 over
v_dt_piece  = VARS+$1A2
v_dt_rot    = VARS+$1A4
v_dt_col    = VARS+$1A6
v_dt_row    = VARS+$1A8
v_dt_next   = VARS+$1AA
v_dt_score  = VARS+$1AC
v_dt_lines  = VARS+$1AE
v_dt_level  = VARS+$1B0
v_dt_seedl  = VARS+$1B2
v_dt_seedh  = VARS+$1B4
v_dt_last   = VARS+$1B6
v_pad_rptn  = VARS+$1B8
v_dt_board  = $0C00         ; 10x20 = 200 bytes

; ---- constants ----
SCRW_C  = 32
SCRH_C  = 28
SCRW    = 256
SCRH    = 224
MAXWIN  = 6
WENT_SIZE = 16
WSTATE  = 0
WPROC   = 1
WX      = 2
WY      = 4
WW      = 6
WH      = 8
WTITLE  = 10
ATTR_NORM = $0000
ATTR_INV  = $0400
ATTR_ACC  = $0800
ATTR_KEY  = $0C00
MENUBAR_C = 1
TICKS_SEC = 60
DBLCLICK  = 30
NICONS   = 4
EVQ_SIZE = 32
EV_KEY   = 1
EV_MOUSE = 4
KBD_TOP  = 22
VRAM_MAP = $0000
VRAM_CHR = $1000
VRAM_OBJ = $4000

; SNES joypad bit masks (16-bit word from $4218)
PAD_B    = $8000
PAD_Y    = $4000
PAD_SEL  = $2000
PAD_STA  = $1000
PAD_UP   = $0800
PAD_DN   = $0400
PAD_LT   = $0200
PAD_RT   = $0100
PAD_A    = $0080
PAD_X    = $0040
PAD_L    = $0020
PAD_R    = $0010
PAD_DPAD = $0F00

.segment "CODE"

; ============================================================================
; Reset / boot
; ============================================================================
.proc Reset
        sei
        clc
        xce
        rep #$38
.a16
.i16
        ldx #$1FFF
        txs
        phk
        plb                     ; DB = 0
        lda #$0000
        tcd                     ; main-loop DP = 0

        sep #$20
.a8
        lda #$8F
        sta INIDISP             ; forced blank
        stz $420B               ; MDMAEN off
        stz $420C               ; HDMAEN off
        ; zero the whole DMA/HDMA channel register block ($4300-$437F) so no
        ; stale channel config can drive HDMA and corrupt the PPU per-scanline
        ldx #$00
@dmaz:  stz $4300,x
        inx
        cpx #$80
        bne @dmaz
        jsr InitPPURegs
        jsr ClearState

.ifdef AUTOTEST
        lda #$01
.else
        lda #$00
.endif
        sta v_auto

        ; "UDM1" magic
        lda #'U'
        sta v_magic+0
        lda #'D'
        sta v_magic+1
        lda #'M'
        sta v_magic+2
        lda #'1'
        sta v_magic+3

        jsr LoadBGTiles
        jsr LoadSprTiles
        jsr LoadPalette
        jsr InitOAM

        ; initial cursor at screen centre
        rep #$20
.a16
        lda #128
        sta v_mouse_x
        lda #112
        sta v_mouse_y
        lda #$FFFF
        sta v_dbl_icon
        sta v_kb_hover
        ; SNES mouse not detected at boot (probe is M1 backlog -> pad)
        stz v_mouse_present
        ; default Notepad name; format SRAM if uninitialised
        lda #.loword(str_demo_name)
        sta P0
        jsr np_setname
        jsr sram_init

        ; build the desktop into the shadow (16-bit), flush once (8-bit)
        rep #$30
        jsr repaint_all
        sep #$20
.a8
        jsr FlushTilemap

        ; Mode 1, BG1
        lda #$01
        sta BGMODE
        lda #((VRAM_MAP >> 8) & $FC)
        sta BG1SC
        lda #(VRAM_CHR >> 12)
        sta BG12NBA
        lda #(VRAM_OBJ >> 13)   ; OBJSEL name base = $4000 words, 8x8/16x16
        sta OBJSEL
        lda #$11
        sta TM                  ; enable BG1 + OBJ on the main screen

        lda #$0F
        sta INIDISP             ; screen on
        lda #$81
        sta NMITIMEN            ; NMI + auto-joypad
        cli

        rep #$30
.a16
.i16
.ifdef AUTOTEST
        jsr AutotestSetup
.endif

; ----------------------------------------------------------------- main loop
MainLoop:
        rep #$30
        wai
        jsr pad_events
        jsr kbd_toggle_chk
        jsr handle_clicks
        jsr handle_drag
        jsr handle_events
        jsr softkbd_hover
        jsr app_ticks
        bra MainLoop
.endproc

; ============================================================================
; PPU register init (A 8-bit)
; ============================================================================
.proc InitPPURegs
.a8
        ldx #$00
@l:     stz $2105,x
        inx
        cpx #($2134 - $2105)
        bne @l
        lda #$80
        sta VMAIN
        stz BG1HOFS
        stz BG1HOFS
        stz BG1VOFS
        stz BG1VOFS
        rts
.endproc

; ============================================================================
; Clear kernel state + tilemap shadow (A 8-bit)
; ============================================================================
.proc ClearState
.a8
        rep #$20
.a16
        lda #$0000
        ldx #$0000
@v:     sta VARS,x              ; clear $0200..$03FF
        inx
        inx
        cpx #$0200
        bne @v
        ldx #$0000
@t:     sta TMAP,x
        inx
        inx
        cpx #(SCRW_C*32*2)
        bne @t
        sep #$20
.a8
        rts
.endproc

; ============================================================================
; DMA helpers (forced blank or vblank)
; ============================================================================

; LoadBGTiles - tile blob -> BG1 char base
.proc LoadBGTiles
.a8
        rep #$20
.a16
        ldx #VRAM_CHR
        stx VMADDL
        sep #$20
.a8
        lda #$01
        sta DMAP0
        lda #<VMDATAL
        sta BBAD0
        rep #$20
.a16
        ldx #.loword(tiles_bg)
        stx A1T0L
        ldx #BGTILE_BYTES
        stx DAS0L
        sep #$20
.a8
        lda #^tiles_bg
        sta A1B0
        lda #$01
        sta MDMAEN
        rts
.endproc

; LoadSprTiles - cursor sprite tiles -> OBJ char base
.proc LoadSprTiles
.a8
        rep #$20
.a16
        ldx #VRAM_OBJ
        stx VMADDL
        sep #$20
.a8
        lda #$01
        sta DMAP0
        lda #<VMDATAL
        sta BBAD0
        rep #$20
.a16
        ldx #.loword(tiles_spr)
        stx A1T0L
        ldx #SPRTILE_BYTES
        stx DAS0L
        sep #$20
.a8
        lda #^tiles_spr
        sta A1B0
        lda #$01
        sta MDMAEN
        rts
.endproc

; LoadPalette - 64 BG colours to CGRAM 0, 16 sprite colours to CGRAM 128
.proc LoadPalette
.a8
        stz CGADD
        lda #$00
        sta DMAP0
        lda #<CGDATA
        sta BBAD0
        rep #$20
.a16
        ldx #.loword(pal_bg)
        stx A1T0L
        ldx #128                ; 64 colours * 2
        stx DAS0L
        sep #$20
.a8
        lda #^pal_bg
        sta A1B0
        lda #$01
        sta MDMAEN
        ; sprite palette 0 at CGRAM 128
        lda #128
        sta CGADD
        lda #$00
        sta DMAP0
        lda #<CGDATA
        sta BBAD0
        rep #$20
.a16
        ldx #.loword(pal_spr)
        stx A1T0L
        ldx #32
        stx DAS0L
        sep #$20
.a8
        lda #^pal_spr
        sta A1B0
        lda #$01
        sta MDMAEN
        rts
.endproc

; InitOAM - park all sprites off-screen, set up the two cursor sprites
.proc InitOAM
.a8
        ; clear low table; Y byte = $F0 (off-screen) for every sprite
        ldx #$0000
@l:     stz OAMSH,x             ; X low
        lda #$F0
        sta OAMSH+1,x           ; Y
        stz OAMSH+2,x           ; tile
        stz OAMSH+3,x           ; attr
        inx
        inx
        inx
        inx
        cpx #512
        bne @l
        ; high table = 0 (small size, X bit8 = 0)
        ldx #$0000
@h:     stz OAMSH+512,x
        inx
        cpx #32
        bne @h
        ; sprite 0 = cursor top, sprite 1 = cursor bottom
        lda #0
        sta OAMSH+2             ; tile 0
        lda #$30
        sta OAMSH+3             ; palette 0, priority 3
        lda #1
        sta OAMSH+6             ; tile 1
        lda #$30
        sta OAMSH+7
        rts
.endproc

; FlushTilemap - shadow -> VRAM tilemap
.proc FlushTilemap
        rep #$10                ; force 16-bit index (size reg needs full word)
.i16
        sep #$20
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
        ldx #(SCRW_C*32*2)
        stx DAS0L
        sep #$20
.a8
        lda #WRAM_BANK
        sta A1B0
        lda #$01
        sta MDMAEN
        rts
.endproc

; FlushOAM - OAM shadow -> OAM (vblank)
.proc FlushOAM
.a8
        stz OAMADDL
        stz OAMADDL+1
        lda #$00
        sta DMAP0
        lda #<OAMDATA
        sta BBAD0
        rep #$20
.a16
        ldx #.loword(OAMSH)
        stx A1T0L
        ldx #544
        stx DAS0L
        sep #$20
.a8
        lda #WRAM_BANK
        sta A1B0
        lda #$01
        sta MDMAEN
        rts
.endproc

; ============================================================================
; Cell drawing primitives - write the WRAM tilemap shadow (main-loop context)
; All take 16-bit A/X/Y. Cell offset = (cy*32 + cx)*2.
; ============================================================================

; fill_cells: A0=cx A1=cy A2=w A3=h A4=cell-word. Clobbers A/X, A0..A4 scratch.
.proc fill_cells
.a16
.i16
        lda A3
        beq @done
        sta S0                  ; rows remaining
@row:   lda A1
        asl a
        asl a
        asl a
        asl a
        asl a                   ; cy*32
        clc
        adc A0                  ; +cx
        asl a                   ; *2
        tax
        lda A2
        beq @next
        sta S1                  ; cols remaining
        lda A4
@col:   sta TMAP,x
        inx
        inx
        dec S1
        bne @col
@next:  inc A1                  ; cy++
        dec S0
        bne @row
@done:  rts
.endproc

; draw_str: P0=ptr A0=cx A1=cy A4=attr. Clips at the right edge.
.proc draw_str
.a16
.i16
        lda A1
        asl a
        asl a
        asl a
        asl a
        asl a
        clc
        adc A0
        asl a
        tax
        lda #SCRW_C
        sec
        sbc A0
        sta S0                  ; cells available
        ldy #$0000
@ch:    sep #$20
.a8
        lda (P0),y
        sta S1
        rep #$20
.a16
        lda S1
        and #$00FF
        beq @done
        lda S0
        beq @done
        lda S1
        and #$00FF
        sec
        sbc #32
        bcc @sp
        cmp #95
        bcc @ok
@sp:    lda #0
@ok:    clc
        adc #T_FONT
        ora A4
        sta TMAP,x
        inx
        inx
        dec S0
        iny
        bra @ch
@done:  rts
.endproc

; draw_char: A0=cx A1=cy A2=char A4=attr.
.proc draw_char
.a16
.i16
        lda A1
        asl a
        asl a
        asl a
        asl a
        asl a
        clc
        adc A0
        asl a
        tax
        lda A2
        and #$00FF
        sec
        sbc #32
        bcc @sp
        cmp #95
        bcc @ok
@sp:    lda #0
@ok:    clc
        adc #T_FONT
        ora A4
        sta TMAP,x
        rts
.endproc

; clear_screen - desktop blue
.proc clear_screen
.a16
.i16
        stz A0
        stz A1
        lda #SCRW_C
        sta A2
        lda #SCRH_C
        sta A3
        lda #(ATTR_NORM+T_SOLBG)
        sta A4
        jsr fill_cells
        rts
.endproc

; ============================================================================
; Desktop
; ============================================================================
.proc draw_desktop
.a16
.i16
        ; menu bar: row 0 white, title in blue
        stz A0
        stz A1
        lda #SCRW_C
        sta A2
        lda #1
        sta A3
        lda #(ATTR_INV+T_SOLBG)
        sta A4
        jsr fill_cells
        lda #.loword(str_menutitle)
        sta P0
        lda #1
        sta A0
        stz A1
        lda #ATTR_INV
        sta A4
        jsr draw_str
        ; icons
        stz LC0                 ; icon index
@icon:  lda LC0
        jsr draw_icon
        inc LC0
        lda LC0
        cmp #NICONS
        bne @icon
        rts
.endproc

; draw_icon: A=icon index. Selected icon -> inverted label.
; Uses S4/S5 internally (+ draw_str's S0/S1); never touches outer counters.
.proc draw_icon
.a16
.i16
        and #$00FF
        sta S4                  ; index
        asl a
        asl a                   ; *4
        tax
        lda icon_tab,x          ; x cell
        sta A0
        lda icon_tab+2,x        ; y cell
        sta A1
        lda S4
        asl a
        tax
        lda icon_names,x
        sta P0
        lda #ATTR_NORM
        sta A4
        ldx v_sel_icon
        cpx S4
        bne @attr
        lda #ATTR_INV
        sta A4
@attr:  jsr draw_str
        rts
.endproc

; icon_at: A0=cx A1=cy -> A = icon index or $FFFF
.proc icon_at
.a16
.i16
        stz S2                  ; index
@scan:  lda S2
        asl a
        asl a
        tax
        lda icon_tab,x          ; ix
        sta S0
        lda icon_tab+2,x        ; iy
        cmp A1
        bne @next
        lda A0
        sec
        sbc S0
        bcc @next               ; cx < ix
        cmp #9                  ; icon hit width
        bcs @next
        lda S2                  ; hit
        rts
@next:  inc S2
        lda S2
        cmp #NICONS
        bne @scan
        lda #$FFFF
        rts
.endproc

; select_icon: A = icon index. Re-render the old (unselected) + new icons.
.proc select_icon
.a16
.i16
        sta S7                  ; new
        lda v_sel_icon
        sta S6                  ; old
        lda S7
        sta v_sel_icon          ; set new first so the old redraws unselected
        lda S6
        cmp #$FFFF
        beq @drawnew
        jsr draw_icon           ; redraw old (now unselected)
@drawnew:
        lda S7
        jsr draw_icon           ; draw new (selected)
        lda #$01
        sta v_dirty
        rts
.endproc

; ============================================================================
; Window manager (cell coordinates)
; ============================================================================

; ent_x: A = table index -> X = index*16 (entry byte offset)
.proc ent_x
.a16
.i16
        and #$00FF
        asl a
        asl a
        asl a
        asl a
        tax
        rts
.endproc

; zent_x: A = z index -> X = slot*16 (entry byte offset)
.proc zent_x
.a16
.i16
        and #$00FF
        tax
        sep #$20
.a8
        lda v_zlist,x
        rep #$20
.a16
        and #$00FF
        asl a
        asl a
        asl a
        asl a
        tax
        rts
.endproc

; launch_app: A = proc index. Raise an existing window or create one.
.proc launch_app
.a16
.i16
        sta S3                  ; proc
        ; scan for an existing window with this proc
        stz S2                  ; table index
@scan:  lda S2
        jsr ent_x
        sep #$20
.a8
        lda v_wintab+WSTATE,x
        beq @next
        lda v_wintab+WPROC,x
        cmp S3
        bne @next
        rep #$20
.a16
        ; found: raise its z index
        lda S2
        jsr z_index_of
        cmp #$FFFF
        beq @next16
        jsr raise_window
        rts
@next:  rep #$20
.a16
@next16:
        inc S2
        lda S2
        cmp #MAXWIN
        bne @scan
        ; create from the app definition table
        lda S3
        jsr win_create
        rts
.endproc

; win_create: A = proc index (looks up app_def_tab for geometry/title)
.proc win_create
.a16
.i16
        sta S3                  ; proc
        ; find a free slot
        stz S2
@find:  lda S2
        jsr ent_x
        sep #$20
.a8
        lda v_wintab+WSTATE,x
        rep #$20
.a16
        beq @got
        inc S2
        lda S2
        cmp #MAXWIN
        bne @find
        rts                     ; table full: no-op
@got:   ; X still = slot*16 (from the last ent_x in the loop). Recompute.
        lda S2
        jsr ent_x
        phx                     ; save entry offset
        ; app_def_tab entry = proc*10
        lda S3
        asl a                   ; *2
        sta S0
        asl a
        asl a                   ; *8
        clc
        adc S0                  ; *10
        tay                     ; Y = def offset
        plx                     ; X = entry offset
        sep #$20
.a8
        lda #$01
        sta v_wintab+WSTATE,x
        lda S3
        sta v_wintab+WPROC,x
        rep #$20
.a16
        lda app_def_tab+0,y
        sta v_wintab+WX,x
        lda app_def_tab+2,y
        sta v_wintab+WY,x
        lda app_def_tab+4,y
        sta v_wintab+WW,x
        lda app_def_tab+6,y
        sta v_wintab+WH,x
        lda app_def_tab+8,y
        sta v_wintab+WTITLE,x
        ; push slot onto the z-list
        lda v_zcount
        phx
        tax
        lda S2
        sep #$20
.a8
        sta v_zlist,x
        rep #$20
.a16
        plx
        inc v_zcount
        jsr repaint_all
        rts
.endproc

; z_index_of: A = table index -> A = z index or $FFFF
.proc z_index_of
.a16
.i16
        and #$00FF
        sta S0                  ; target slot
        stz S1                  ; z
@scan:  lda S1
        cmp v_zcount
        bcs @no
        lda S1
        tax
        sep #$20
.a8
        lda v_zlist,x
        rep #$20
.a16
        and #$00FF
        cmp S0
        beq @yes
        inc S1
        bra @scan
@yes:   lda S1
        rts
@no:    lda #$FFFF
        rts
.endproc

; find_window_at: A0=cx A1=cy -> A = z index (topmost hit) or $FFFF
.proc find_window_at
.a16
.i16
        lda v_zcount
        beq @no
        dec a
        sta S2                  ; z
@scan:  lda S2
        bmi @no
        jsr zent_x              ; X = entry offset
        lda A0
        cmp v_wintab+WX,x
        bcc @next               ; cx < wx
        lda v_wintab+WX,x
        clc
        adc v_wintab+WW,x
        dec a
        cmp A0
        bcc @next               ; cx >= wx+ww
        lda A1
        cmp v_wintab+WY,x
        bcc @next
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        dec a
        cmp A1
        bcc @next
        lda S2                  ; hit
        rts
@next:  dec S2
        bra @scan
@no:    lda #$FFFF
        rts
.endproc

; raise_window: A = z index
.proc raise_window
.a16
.i16
        sta S0                  ; z
        lda v_zcount
        dec a
        cmp S0
        beq @done               ; already topmost
        ; slot = zlist[z]
        lda S0
        tax
        sep #$20
.a8
        lda v_zlist,x
        sta S1                  ; slot
        rep #$20
.a16
@shift: lda S0
        cmp v_zcount
        bcs @place
        inc a
        cmp v_zcount
        bcs @place
        ; zlist[z] = zlist[z+1]
        ldx S0
        sep #$20
.a8
        lda v_zlist+1,x
        sta v_zlist,x
        rep #$20
.a16
        inc S0
        bra @shift
@place: ldx S0
        sep #$20
.a8
        lda S1
        sta v_zlist,x
        rep #$20
.a16
        jsr repaint_all
@done:  rts
.endproc

; close_window: A = z index
.proc close_window
.a16
.i16
        sta S0                  ; z
        jsr zent_x
        sep #$20
.a8
        stz v_wintab+WSTATE,x   ; free the slot
        rep #$20
.a16
        ; remove from z-list, shift down
        lda v_zcount
        dec a
        sta v_zcount
@shift: lda S0
        cmp v_zcount
        bcs @done
        ldx S0
        sep #$20
.a8
        lda v_zlist+1,x
        sta v_zlist,x
        rep #$20
.a16
        inc S0
        bra @shift
@done:  jsr repaint_all
        rts
.endproc

; close_topmost
.proc close_topmost
.a16
.i16
        lda v_zcount
        beq @out
        dec a
        jsr close_window
@out:   rts
.endproc

; repaint_all - desktop, windows bottom-up, soft keyboard last
.proc repaint_all
.a16
.i16
        jsr clear_screen
        jsr draw_desktop
        stz LC1                 ; z = 0
@wins:  lda LC1
        cmp v_zcount
        bcs @kbd
        jsr zent_x
        jsr draw_window
        inc LC1
        bra @wins
@kbd:   lda v_kb_vis
        and #$00FF
        beq @done
        jsr softkbd_draw
@done:  lda #$01
        sta v_dirty
        rts
.endproc

; redraw_topmost - repaint just the topmost window + kbd overlay
.proc redraw_topmost
.a16
.i16
        lda v_zcount
        beq @out
        dec a
        jsr zent_x
        jsr draw_window
        lda v_kb_vis
        and #$00FF
        beq @out
        jsr softkbd_draw
@out:   lda #$01
        sta v_dirty
        rts
.endproc

; draw_window: X = entry byte offset
.proc draw_window
.a16
.i16
        stx S2                  ; entry offset (X gets clobbered by fills)
        ; title bar: white row
        ldx S2
        lda v_wintab+WX,x
        sta A0
        lda v_wintab+WY,x
        sta A1
        lda v_wintab+WW,x
        sta A2
        lda #1
        sta A3
        lda #(ATTR_INV+T_SOLBG)
        sta A4
        jsr fill_cells
        ; title text
        ldx S2
        lda v_wintab+WTITLE,x
        sta P0
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        sta A1
        lda #ATTR_INV
        sta A4
        jsr draw_str
        ; close box 'X' at top-right
        ldx S2
        lda v_wintab+WX,x
        clc
        adc v_wintab+WW,x
        sec
        sbc #2
        sta A0
        lda v_wintab+WY,x
        sta A1
        lda #'X'
        sta A2
        lda #ATTR_INV
        sta A4
        jsr draw_char
        ; body fill (inside borders)
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        inc a
        sta A1
        lda v_wintab+WW,x
        sec
        sbc #2
        sta A2
        lda v_wintab+WH,x
        sec
        sbc #2
        sta A3
        lda #(ATTR_NORM+T_SOLBG)
        sta A4
        jsr fill_cells
        ; left border
        ldx S2
        lda v_wintab+WX,x
        sta A0
        lda v_wintab+WY,x
        inc a
        sta A1
        lda #1
        sta A2
        lda v_wintab+WH,x
        sec
        sbc #2
        sta A3
        lda #(ATTR_NORM+T_EDGEL)
        sta A4
        jsr fill_cells
        ; right border
        ldx S2
        lda v_wintab+WX,x
        clc
        adc v_wintab+WW,x
        dec a
        sta A0
        lda v_wintab+WY,x
        inc a
        sta A1
        lda #1
        sta A2
        lda v_wintab+WH,x
        sec
        sbc #2
        sta A3
        lda #(ATTR_NORM+T_EDGER)
        sta A4
        jsr fill_cells
        ; bottom-left corner
        ldx S2
        lda v_wintab+WX,x
        sta A0
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        dec a
        sta A1
        lda #1
        sta A2
        lda #1
        sta A3
        lda #(ATTR_NORM+T_CORNBL)
        sta A4
        jsr fill_cells
        ; bottom edge
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        dec a
        sta A1
        lda v_wintab+WW,x
        sec
        sbc #2
        sta A2
        lda #1
        sta A3
        lda #(ATTR_NORM+T_EDGEB)
        sta A4
        jsr fill_cells
        ; bottom-right corner
        ldx S2
        lda v_wintab+WX,x
        clc
        adc v_wintab+WW,x
        dec a
        sta A0
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        dec a
        sta A1
        lda #1
        sta A2
        lda #1
        sta A3
        lda #(ATTR_NORM+T_CORNBR)
        sta A4
        jsr fill_cells
        ; content
        ldx S2
        sep #$20
.a8
        lda v_wintab+WPROC,x
        rep #$20
.a16
        and #$00FF
        jsr app_draw_content
        rts
.endproc

; app_draw_content: A = proc index, X = entry offset (S2 holds it)
.proc app_draw_content
.a16
.i16
        cmp #0
        beq @sysinfo
        cmp #1
        beq @clock
        cmp #2
        beq @notepad
        cmp #7
        beq @files
        rts
@sysinfo:
        jsr sysinfo_draw
        rts
@clock:
        jsr clock_draw
        rts
@notepad:
        jsr notepad_draw
        rts
@files:
        jsr files_draw
        rts
.endproc

; ============================================================================
; SysInfo (proc 0) + Clock (proc 1)
; ============================================================================
.proc sysinfo_draw
.a16
.i16
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta S3                  ; left col
        lda v_wintab+WY,x
        clc
        adc #2
        sta A6                  ; top row
        ; lines
        lda #.loword(str_si1)
        sta P0
        lda S3
        sta A0
        lda A6
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        lda #.loword(str_si2)
        sta P0
        lda S3
        sta A0
        lda A6
        inc a
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        lda #.loword(str_si3)
        sta P0
        lda S3
        sta A0
        lda A6
        clc
        adc #2
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        lda #.loword(str_si4)
        sta P0
        lda S3
        sta A0
        lda A6
        clc
        adc #3
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        ; mouse present?
        lda v_mouse_present
        bne @havemouse
        lda #.loword(str_si_nomouse)
        bra @msestr
@havemouse:
        lda #.loword(str_si_mouse)
@msestr:
        sta P0
        lda S3
        sta A0
        lda A6
        clc
        adc #4
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        ; uptime seconds (accent)
        lda v_secs
        sta S0
        lda #.loword(v_numbuf)
        sta P1
        jsr fmt_dec             ; v_numbuf = decimal of S0
        lda #.loword(str_uptime)
        sta P0
        lda S3
        sta A0
        lda A6
        clc
        adc #6
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        lda #.loword(v_numbuf)
        sta P0
        lda S3
        clc
        adc #8
        sta A0
        lda A6
        clc
        adc #6
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        rts
.endproc

.proc clock_draw
.a16
.i16
        ; caption
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta A0
        lda v_wintab+WY,x
        clc
        adc #2
        sta A1
        lda #.loword(str_uptime)
        sta P0
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        ; HH:MM:SS into v_clkbuf (S2 holds the window offset throughout)
        lda #.loword(v_clkbuf)
        sta P1
        lda v_secs
        sta S0                  ; total seconds
        lda #60
        sta S1
        jsr div16               ; A = minutes-total, S0 = seconds
        sta S7                  ; minutes-total
        lda S0
        sta A5                  ; seconds
        lda S7
        sta S0
        lda #60
        sta S1
        jsr div16               ; A = hours, S0 = minutes
        sta A6                  ; hours
        lda S0
        sta A3                  ; minutes
        lda A6
        jsr put2dig
        lda #':'
        jsr putchar
        lda A3
        jsr put2dig
        lda #':'
        jsr putchar
        lda A5
        jsr put2dig
        sep #$20
.a8
        lda #0
        sta (P1)                ; NUL terminate
        rep #$20
.a16
        ; centered time, accent
        ldx S2
        lda v_wintab+WW,x
        lsr a
        clc
        adc v_wintab+WX,x
        sec
        sbc #4
        sta A0
        lda v_wintab+WH,x
        lsr a
        clc
        adc v_wintab+WY,x
        sta A1
        lda #.loword(v_clkbuf)
        sta P0
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        rts
.endproc

; ============================================================================
; Number formatting (all main-loop context)
; ============================================================================

; div16: dividend S0, divisor S1 -> A = quotient, S0 = remainder.
; Uses only DVQ + A internally (callers keep all S* slots).
.proc div16
.a16
.i16
        stz DVQ
@loop:  lda S0
        cmp S1
        bcc @done
        sec
        sbc S1
        sta S0
        inc DVQ
        bra @loop
@done:  lda DVQ
        rts
.endproc

; put2dig: A = value (0..99) -> two ascii digits via (P1)+
.proc put2dig
.a16
.i16
        sta S0
        lda #10
        sta S1
        jsr div16               ; A = tens, S0 = ones
        clc
        adc #'0'
        pha
        lda S0
        clc
        adc #'0'
        sta S4                  ; ones char
        pla                     ; tens char
        jsr putchar
        lda S4
        jsr putchar
        rts
.endproc

; putchar: A low byte -> *(P1)++
.proc putchar
.a16
.i16
        sep #$20
.a8
        sta (P1)
        rep #$20
.a16
        inc P1
        rts
.endproc

; fmt_dec: value S0 -> decimal ascii at v_numbuf, NUL-terminated.
; Generates digits LSB-first on the stack, then pops them MSB-first.
.proc fmt_dec
.a16
.i16
        stz S5                  ; digit count
@gen:   lda #10
        sta S1
        jsr div16               ; A = quotient, S0 = remainder (digit)
        sta S6                  ; quotient
        lda S0
        clc
        adc #'0'
        pha                     ; push digit char
        inc S5
        lda S6
        sta S0
        bne @gen                ; more digits while quotient != 0
        lda #.loword(v_numbuf)
        sta P1
@pop:   pla
        jsr putchar             ; low byte -> *(P1)++
        dec S5
        bne @pop
        sep #$20
.a8
        lda #0
        sta (P1)
        rep #$20
.a16
        rts
.endproc

; ============================================================================
; Event queue (32 x 4 bytes) - main-loop only (NMI latches state, never posts)
; ============================================================================

; ev_post: A0 = type (low byte), A1 = data word
.proc ev_post
.a16
.i16
        lda v_ev_tail
        sta S0
        inc a
        and #(EVQ_SIZE-1)
        sta S1
        cmp v_ev_head
        beq @full               ; drop-when-full
        lda S0
        asl a
        asl a
        tax
        sep #$20
.a8
        lda A0
        sta v_evq,x
        rep #$20
.a16
        lda A1
        sta v_evq+2,x
        lda S1
        sta v_ev_tail
@full:  rts
.endproc

; ev_get: -> A = type (0 if empty), A1 = data
.proc ev_get
.a16
.i16
        lda v_ev_head
        cmp v_ev_tail
        beq @empty
        sta S0
        asl a
        asl a
        tax
        sep #$20
.a8
        lda v_evq,x
        sta S1
        rep #$20
.a16
        lda v_evq+2,x
        sta A1
        lda S0
        inc a
        and #(EVQ_SIZE-1)
        sta v_ev_head
        lda S1
        and #$00FF
        rts
@empty: lda #0
        rts
.endproc

; ============================================================================
; Main-loop input dispatch
; ============================================================================

; pad_events - consume the NMI edge latch, post key events / toggle kbd
.proc pad_events
.a16
.i16
        sep #$20
.a8
        lda #$01
        sta NMITIMEN            ; NMI off (auto-joypad stays on)
        rep #$20
.a16
        lda v_pad_edge
        sta S0
        stz v_pad_edge
        sep #$20
.a8
        lda #$81
        sta NMITIMEN            ; NMI back on
        rep #$20
.a16
        lda S0
        beq @done
        bit #PAD_B
        beq @nb
        lda #1
        sta v_kb_toggle
@nb:    lda S0
        bit #PAD_Y
        beq @ny
        lda #EV_KEY
        sta A0
        lda #13
        sta A1
        jsr ev_post
@ny:    lda S0
        bit #PAD_STA
        beq @nst
        lda #EV_KEY
        sta A0
        lda #27
        sta A1
        jsr ev_post
@nst:   lda S0
        bit #PAD_X
        beq @nx
        lda #EV_KEY
        sta A0
        lda #8
        sta A1
        jsr ev_post
@nx:    lda S0
        bit #PAD_SEL
        beq @done
        lda #EV_KEY
        sta A0
        lda #32
        sta A1
        jsr ev_post
@done:  rts
.endproc

; kbd_toggle_chk - the B-button latch requested a soft-keyboard toggle
.proc kbd_toggle_chk
.a16
.i16
        lda v_kb_toggle
        beq @out
        stz v_kb_toggle
        lda v_kb_vis
        and #$00FF
        beq @show
        jsr softkbd_hide
        rts
@show:  jsr softkbd_show
@out:   rts
.endproc

; handle_events - drain the queue (ESC closes topmost; desktop nav otherwise)
.proc handle_events
.a16
.i16
@next:  jsr ev_get
        cmp #0
        bne @hk
        rts
@hk:    cmp #EV_KEY
        bne @next
        lda A1
        and #$00FF
        sta S0                  ; ascii
        lda v_zcount
        beq @desktop
        lda S0
        cmp #27
        bne @app
        jsr close_topmost
        bra @next
@app:   ; route the key to the topmost window's app handler
        lda v_zcount
        dec a
        jsr zent_x
        sep #$20
.a8
        lda v_wintab+WPROC,x
        rep #$20
.a16
        and #$00FF
        jsr app_key
        bra @next
@desktop:
        lda A1
        xba
        and #$00FF
        cmp #$4E
        beq @right
        cmp #$4F
        beq @left
        lda S0
        cmp #13
        beq @launch
        bra @next
@right: lda v_sel_icon
        inc a
        cmp #NICONS
        bcc @setsel
        lda #0
        bra @setsel
@left:  lda v_sel_icon
        cmp #$FFFF
        beq @lwrap
        dec a
        bpl @setsel
@lwrap: lda #(NICONS-1)
@setsel:
        jsr select_icon
        jmp @next
@launch:
        lda v_sel_icon
        cmp #$FFFF
        beq @done
        asl a
        tax
        lda icon_procs,x
        jsr launch_app
        jmp @next
@done:  rts
.endproc

; handle_clicks - consume the NMI press latch (PORT-SPEC SS6 rule 4)
.proc handle_clicks
.a16
.i16
        sep #$20
.a8
        lda v_click_seq
        cmp v_last_seq
        bne @work
        rep #$20
.a16
        rts
@work:  sta v_last_seq
        rep #$20
.a16
        lda v_click_x
        lsr a
        lsr a
        lsr a
        sta A0
        lda v_click_y
        lsr a
        lsr a
        lsr a
        sta A1
        ; soft keyboard claims its panel rows
        lda v_kb_vis
        and #$00FF
        beq @windows
        lda A1
        cmp #KBD_TOP
        bcc @windows
        jsr softkbd_click
        rts
@windows:
        jsr find_window_at
        cmp #$FFFF
        beq @desktop
        sta S3
        jsr zent_x
        lda A1
        cmp v_wintab+WY,x
        beq @title
        ; body click: raise if not topmost
        lda v_zcount
        dec a
        cmp S3
        bne :+
        jmp @done
:       lda S3
        jsr raise_window
        rts
@title: lda v_wintab+WX,x
        clc
        adc v_wintab+WW,x
        sec
        sbc #2
        cmp A0
        bcc @close
        beq @close
        ; drag start
        lda S3
        jsr raise_window
        lda v_zcount
        dec a
        jsr zent_x
        lda A0
        sec
        sbc v_wintab+WX,x
        sta v_drag_offx
        lda A1
        sec
        sbc v_wintab+WY,x
        sta v_drag_offy
        stx v_drag_win
        lda #1
        sta v_drag_active
        rts
@close: lda S3
        jsr close_window
        rts
@desktop:
        jsr icon_at
        cmp #$FFFF
        beq @deselect
        sta S3
        lda v_dbl_icon
        cmp S3
        bne @single
        lda v_frame
        sec
        sbc v_dbl_frame
        cmp #DBLCLICK
        bcs @single
        lda #$FFFF
        sta v_dbl_icon
        lda S3
        jsr select_icon
        lda S3
        asl a
        tax
        lda icon_procs,x
        jsr launch_app
        rts
@single:
        lda S3
        sta v_dbl_icon
        lda v_frame
        sta v_dbl_frame
        lda S3
        jsr select_icon
        rts
@deselect:
        lda #$FFFF
        sta v_dbl_icon
@done:  rts
.endproc

; handle_drag - cell-snapped live drag
.proc handle_drag
.a16
.i16
        lda v_drag_active
        bne @active
        rts
@active:
        lda v_mouse_btn
        beq @finish
        ldx v_drag_win
        lda v_mouse_x
        lsr a
        lsr a
        lsr a
        sec
        sbc v_drag_offx
        bpl @xpos
        lda #0
@xpos:  sta A0
        lda #SCRW_C
        sec
        sbc v_wintab+WW,x
        cmp A0
        bcs @xok
        sta A0
@xok:   lda v_mouse_y
        lsr a
        lsr a
        lsr a
        sec
        sbc v_drag_offy
        sta A1
        lda A1
        cmp #MENUBAR_C
        bcs @ymin
        lda #MENUBAR_C
        sta A1
@ymin:  lda #(SCRH_C-1)
        cmp A1
        bcs @yok
        sta A1
@yok:   lda A0
        cmp v_wintab+WX,x
        bne @move
        lda A1
        cmp v_wintab+WY,x
        beq @nochange
@move:  lda A0
        sta v_wintab+WX,x
        lda A1
        sta v_wintab+WY,x
        jsr repaint_all
@nochange:
        rts
@finish:
        stz v_drag_active
        rts
.endproc

; app_ticks - once a second, refresh the topmost window (clock/uptime)
.proc app_ticks
.a16
.i16
        lda v_secs
        cmp v_last_secs
        beq @out
        sta v_last_secs
        lda v_zcount
        beq @out
        jsr redraw_topmost
@out:   rts
.endproc

; ============================================================================
; NMI (vblank) - tick, joypad, cursor, flush. Runs on its own direct page so
; its scratch never collides with the main loop's.
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
        lda #NMI_DP
        tcd
        sep #$20
.a8
        lda #$00
        pha
        plb
        lda RDNMI               ; ack
        ; tick
        lda v_frac
        inc a
        cmp #60
        bcc @savefrac
        rep #$20
.a16
        inc v_secs
        sep #$20
.a8
        lda #0
@savefrac:
        sta v_frac
        rep #$20
.a16
        inc v_frame
        ; joypad
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
        jsr AutotestInput
.endif
        jsr pad_to_mouse
        jsr mouse_buttons
        jsr cursor_oam
        sep #$20
.a8
        jsr FlushOAM
        lda v_dirty
        beq @nodma
        stz v_dirty
        jsr FlushTilemap
@nodma: rep #$30
.a16
.i16
        pld
        plb
        ply
        plx
        pla
        rti
.endproc

; pad_to_mouse - d-pad moves the cursor (held-time accel, L/R = turbo),
; A = button (level), B/Y/Start/X/Select edges -> the pad-edge latch.
; (NMI context, NMI direct page)
.proc pad_to_mouse
.a16
.i16
        lda v_pad
        sta S0
        and #PAD_DPAD
        bne @held
        stz v_pad_heldn
        lda #1
        sta S1
        bra @move
@held:  lda v_pad_heldn
        inc a
        sta v_pad_heldn
        lsr a
        lsr a
        lsr a
        inc a
        cmp #5
        bcc @cap
        beq @cap
        lda #5
@cap:   sta S1
        lda S0
        and #(PAD_L|PAD_R)
        beq @move
        lda #8
        sta S1
@move:  lda v_mouse_x
        sta S2
        lda v_mouse_y
        sta S3
        lda S0
        bit #PAD_UP
        beq @nu
        lda S3
        sec
        sbc S1
        sta S3
@nu:    lda S0
        bit #PAD_DN
        beq @nd
        lda S3
        clc
        adc S1
        sta S3
@nd:    lda S0
        bit #PAD_LT
        beq @nl
        lda S2
        sec
        sbc S1
        sta S2
@nl:    lda S0
        bit #PAD_RT
        beq @nr
        lda S2
        clc
        adc S1
        sta S2
@nr:    lda S2
        bpl @x0
        lda #0
        sta S2
@x0:    lda S2
        cmp #SCRW
        bcc @x1
        lda #(SCRW-1)
        sta S2
@x1:    lda S3
        bpl @y0
        lda #0
        sta S3
@y0:    lda S3
        cmp #SCRH
        bcc @y1
        lda #(SCRH-1)
        sta S3
@y1:    lda S2
        sta v_mouse_x
        lda S3
        sta v_mouse_y
        lda S0
        bit #PAD_A
        beq @nob
        lda #1
        sta v_mouse_btn
        bra @edges
@nob:   stz v_mouse_btn
@edges: lda v_padprev
        eor #$FFFF
        and S0
        and #(PAD_B|PAD_Y|PAD_STA|PAD_X|PAD_SEL)
        ora v_pad_edge
        sta v_pad_edge
        rts
.endproc

; mouse_buttons - button press edge -> click latch + sequence counter
.proc mouse_buttons
.a16
.i16
        sep #$20
.a8
        lda v_mouse_btn
        cmp v_last_btn
        beq @out
        sta v_last_btn
        cmp #0
        beq @out
        rep #$20
.a16
        lda v_mouse_x
        sta v_click_x
        lda v_mouse_y
        sta v_click_y
        sep #$20
.a8
        inc v_click_seq
@out:   rep #$20
.a16
        rts
.endproc

; cursor_oam - write the two cursor sprites from the mouse position
.proc cursor_oam
.a16
.i16
        sep #$20
.a8
        lda v_mouse_x
        sta OAMSH+0
        sta OAMSH+4
        lda v_mouse_y
        sta OAMSH+1
        clc
        adc #8
        sta OAMSH+5
        rep #$20
.a16
        rts
.endproc

.ifdef AUTOTEST
; AutotestSetup - M2 scene: seed Notepad, save DEMO.TXT to SRAM, open Files
.proc AutotestSetup
.a16
.i16
        jsr notepad_set_demo
        jsr np_save             ; persist DEMO.TXT to SRAM
        lda #2
        jsr launch_app          ; Notepad
        lda #7
        jsr launch_app          ; Files (lists the saved file)
        lda #0
        jsr select_icon
        rts
.endproc

; AutotestInput - synthetic pad: drive the cursor right then down (NMI ctx),
; proving the joypad -> cursor path without host input injection.
.proc AutotestInput
.a16
.i16
        lda v_frame
        cmp #40
        bcs @phase2
        lda #PAD_RT
        sta v_pad
        rts
@phase2:
        cmp #80
        bcs @done
        lda #PAD_DN
        sta v_pad
@done:  rts
.endproc
.endif

.include "softkbd.inc"
.include "sram.inc"
.include "apps.inc"

; ============================================================================
; Data
; ============================================================================
.segment "RODATA"
str_menutitle: .byte "UnoDOS 3", 0
str_t_sysinfo: .byte "System Info", 0
str_t_clock:   .byte "Clock", 0
name_sysinfo:  .byte "Sys Info", 0
name_clock:    .byte "Clock", 0
str_si1:       .byte "UnoDOS/SNES v0.3", 0
str_si2:       .byte "CPU: 65C816 3.58MHz", 0
str_si3:       .byte "WRAM: 128 KB", 0
str_si4:       .byte "Region: NTSC", 0
str_si_mouse:  .byte "Input: SNES Mouse", 0
str_si_nomouse: .byte "Input: joypad", 0
str_uptime:    .byte "Uptime:", 0

; icon table: x cell, y cell (2 words per icon)
icon_tab:
        .word 2, 24             ; 0 Sys Info
        .word 12, 24            ; 1 Clock
        .word 2, 26             ; 2 Notepad
        .word 12, 26            ; 3 Files
icon_names:
        .word name_sysinfo
        .word name_clock
        .word name_notepad
        .word name_files
; icon index -> app proc number
icon_procs:
        .word 0, 1, 2, 7

; app definitions: x, y, w, h (cells), title pointer (5 words per app), procs 0-7
app_def_tab:
        .word 4, 4, 24, 9,  str_t_sysinfo    ; 0
        .word 10, 9, 14, 8, str_t_clock      ; 1
        .word 1, 1, 30, 22, str_t_notepad    ; 2 Notepad
        .word 0, 0, 0, 0,   str_t_sysinfo    ; 3 (unused)
        .word 0, 0, 0, 0,   str_t_sysinfo    ; 4 (unused)
        .word 0, 0, 0, 0,   str_t_sysinfo    ; 5 (unused)
        .word 0, 0, 0, 0,   str_t_sysinfo    ; 6 (unused)
        .word 3, 3, 26, 18, str_t_files      ; 7 Files

; ============================================================================
; Cartridge header + vectors
; ============================================================================
.segment "SNESHEADER"
        .byte "UNODOS 3 - SNES PORT "
        .byte $20               ; LoROM, slow
        .byte $02               ; ROM + RAM + battery
        .byte $05               ; 32 KB
        .byte $03               ; 8 KB SRAM (2^3 KB)
        .byte $01               ; NTSC
        .byte $00, $00
        .byte $00, $00          ; checksum (patched)
        .byte $00, $00

.segment "SNESVECTORS"
        .word $0000, $0000
        .word $0000             ; COP
        .word $0000             ; BRK
        .word $0000             ; ABORT
        .word NMI               ; NMI
        .word $0000
        .word $0000             ; IRQ
        .word $0000, $0000
        .word $0000             ; COP
        .word $0000
        .word $0000             ; ABORT
        .word $0000             ; NMI
        .word Reset             ; RESET
        .word $0000             ; IRQ/BRK
