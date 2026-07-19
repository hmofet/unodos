; ============================================================================
; UnoDOS/Apple IIGS kernel: Super Hi-Res desktop, window manager, ADB mouse
; + keyboard, software cursor, SysInfo + Clock, FAT12 storage over SmartPort
; (Files + Notepad), 4096-colour Theme presets, and Ensoniq DOC audio (Music).
; Storage in fs.i (blk_io + FAT12); Files/Notepad in apps.i; palette presets
; in theme.i; the DOC sound engine + Music in snd.i.
;
; The proven SNES M1 (snes/kernel.asm) WM / event / app logic re-expressed
; on the IIGS linear SHR framebuffer: a cell grid of 8x8 px = 40x25 cells on
; 320x200, windows in cell coordinates, a REAL pointer from a polled ADB
; mouse (no joypad-as-pointer), and a save-under software cursor (no hardware
; sprite on SHR).  Rendering writes directly to bank $E1 (no shadow/flush).
;
; Frame model: the main loop free-runs; a `wdm #$02` at the top of each
; iteration is the harness frame marker (a 2-byte NOP on real silicon).  The
; clock ticks off v_frame (60 frames = 1 s; a true $C019 vblank gate is the
; hardware-pass refinement).
;
; Register convention ("kernel-normal"): native, D=0, DBR=$00, 16-bit A AND
; X/Y (rep #$30) in the WM/app/main-loop code; the low-level SHR byte writers
; flip to 8-bit A internally and return 16-bit.  Kernel state and all tables
; live in fast bank-0 RAM (DBR=0); the SHR framebuffer in bank $E1 is reached
; via the 24-bit pointers GP/mtmp and long-indexed (f:SHR*) stores, so DBR
; never moves.  Soft switches (bank 0) are read/written with long (f:) too.
; ============================================================================

.p816
.smart +

.include "sys.inc"
; Greenfield window model (WMODEL.md): window index->byte-offset stride derived
; from the logical [wmodel] by wmgen for [wmodel.platform.iigs] — win_index_to_x
; (A=slot -> X=index*16). Byte-identical to the asl x4 + tax at ent_x / zent_x.
.include "../unodef/gen/wm/iigs/window.inc"

.segment "RODATA"
.include "gen_data.inc"
.segment "CODE"

; ---- exported kernel API (addresses harvested by mkapi.py for disk apps) ----
.export set_attr, calc_gp_px, render_glyph
.export draw_str, draw_char, fill_band, fill_cells, fillcell
.export fmt_dec, div16, put2dig, putchar
.export redraw_topmost, repaint_all, ent_x, zent_x, launch_app
.export return_to_desktop
.export snd_note, snd_off
.export fat_read_file, fat_save_file, fat_list_root

.if 0
; ----------------------------------------------------------- soft switches / regs
; The soft-switch page lives in bank $00; accessed with explicit long (f:)
; addressing so it is correct regardless of DBR.
NEWVIDEO = $00C029
KBD      = $00C000        ; keyboard data (bit7 = key ready)
KBDSTRB  = $00C010        ; clear keyboard strobe
VBL      = $00C019        ; vertical blank status (bit7)
MOUSEDLT = $00C024        ; harness ADB delta FIFO (signed dx, then dy)
MOUSESTA = $00C027        ; bit7 = movement pending, bit0 = button state

; ----------------------------------------------------------- SHR layout (bank E1)
; Kernel-normal DBR=$00 (state/tables in fast bank-0 RAM); the SHR framebuffer
; in bank $E1 is reached via 24-bit pointers ([GP]/[mtmp], bank byte=$E1) and
; long-indexed (f:SHR*) stores, so DBR never has to move.
SHR_PIX  = $2000          ; 16-bit offset within bank $E1
SHR_SCB  = $9D00
SHR_PAL  = $9E00
SHR_BANK = $E1
SHRPIXL  = $E12000        ; full long addresses for f: indexed stores
SHRSCBL  = $E19D00
SHRPALL  = $E19E00
ROWBYTES = 160
NPIX     = 32000

; ----------------------------------------------------------- pseudo-registers (zp)
; Layout mirrors the SNES port so the ported WM/app code reads identically.
P0  = $00
A0  = $02
A1  = $04
A2  = $06
A3  = $08
A4  = $0A
A5  = $0C
A6  = $0E
P1  = $10
S0  = $12
S1  = $14
S2  = $16
S3  = $18
S4  = $1A
S5  = $1C
S6  = $1E
S7  = $20
DVQ = $22
LC0 = $24
LC1 = $26
; ---- SHR primitive scratch ($30+, clear of the pseudo-regs) ----
GP      = $30            ; 24-bit SHR pointer ($30 lo,$31 hi,$32 bank=$E1)
GIDX    = $34            ; 16-bit font index
fbits   = $36
pxtmp    = $37
rowc16  = $38           ; 16-bit row counter
cur_fg  = $3A
cur_bg  = $3B
cur_fgh = $3C
cur_bgh = $3D
kbtmp   = $3E
STRP    = $40            ; 24-bit string ptr ($40 lo,$41 hi,$42 bank=0)
mtmp    = $44            ; 24-bit SHR pointer ($44 lo,$45 hi,$46 bank=$E1)
mtmp2   = $48           ; 16-bit math temp
PX      = $4A            ; fill_band params (pixel byte coords, words)
PY      = $4C
PW      = $4E
PH      = $50
PB      = $52
.endif  ; end of equates block now provided by sys.inc

; PIXBYTE: build a 4bpp pixel byte (two pixels) from fbits mask bits. (a8)
.macro PIXBYTE mh, ml
.local hi0, hidone, lo0, lodone
        lda fbits
        and #mh
        beq hi0
        lda cur_fgh
        bra hidone
hi0:    lda cur_bgh
hidone: sta pxtmp
        lda fbits
        and #ml
        beq lo0
        lda cur_fg
        bra lodone
lo0:    lda cur_bg
lodone: ora pxtmp
.endmacro

.if 0
; ----------------------------------------------------------- kernel state (abs)
VARS = $1000
v_magic     = VARS+$00
v_frame     = VARS+$04
v_secs      = VARS+$06
v_last_secs = VARS+$08
v_mouse_x   = VARS+$0A
v_mouse_y   = VARS+$0C
v_mouse_btn = VARS+$0E
v_last_btn  = VARS+$10
v_click_x   = VARS+$12
v_click_y   = VARS+$14
v_click_seq = VARS+$16
v_last_seq  = VARS+$18
v_drag_active = VARS+$1A
v_drag_win  = VARS+$1C
v_drag_offx = VARS+$1E
v_drag_offy = VARS+$20
v_zcount    = VARS+$22
v_sel_icon  = VARS+$24
v_dbl_icon  = VARS+$26
v_dbl_frame = VARS+$28
v_ev_head   = VARS+$2C
v_ev_tail   = VARS+$2E
v_cur_saved = VARS+$30
v_cur_gp    = VARS+$32
v_frac      = VARS+$34            ; sub-second frame accumulator
v_numbuf    = VARS+$40            ; 16 bytes
v_clkbuf    = VARS+$50            ; 16 bytes
v_zlist     = VARS+$60            ; MAXWIN bytes
v_wintab    = VARS+$70            ; MAXWIN * 16 bytes
v_evq       = VARS+$100           ; EVQ_SIZE * 4 bytes
v_curbuf    = VARS+$200           ; cursor save-under (14 rows * 4 bytes)

; ----------------------------------------------------------- constants
SCRW_C  = 40
SCRH_C  = 25
MAXWIN  = 6
WENT_SIZE = 16
WSTATE  = 0
WPROC   = 1
WX      = 2
WY      = 4
WW      = 6
WH      = 8
WTITLE  = 10
ATTR_NORM = 0
ATTR_INV  = 1
ATTR_ACC  = 2
ATTR_KEY  = 3
MENUBAR_C = 1
TICKS_SEC = 60
DBLCLICK  = 30
NICONS    = 11
EVQ_SIZE  = 32
EV_KEY    = 1
EV_MOUSE  = 4
.endif  ; end of state/constants block now provided by sys.inc

; ============================================================================
.segment "CODE"
start:
        clc
        xce
        rep #$30
        ldx #$01FF
        txs
        lda #0
        tcd

        sep #$20
        lda #$C1
        sta f:NEWVIDEO            ; enable SHR
        phk
        plb                    ; DBR = $00 (kernel-normal: bank-0 state/tables)
        ; preset the bank byte of the two SHR pointers to $E1 (constant)
        lda #SHR_BANK
        sta GP+2
        sta mtmp+2
        rep #$30

        jsr clear_state
        ; UDM1 magic
        sep #$20
        lda #'U'
        sta v_magic+0
        lda #'D'
        sta v_magic+1
        lda #'M'
        sta v_magic+2
        lda #'1'
        sta v_magic+3
        rep #$30

        jsr init_scb_pal
        ; initial cursor centre, no selection
        lda #160
        sta v_mouse_x
        lda #100
        sta v_mouse_y
        lda #$FFFF
        sta v_sel_icon
        sta v_dbl_icon

        ; mount the FAT12 volume and prime the directory + Notepad defaults
        jsr fat_mount
        jsr fat_list_root
        jsr kern_notepad_defaults
        jsr doc_init           ; bring up the Ensoniq DOC

        jsr repaint_all
        jsr draw_cursor

; ----------------------------------------------------------------- main loop
MainLoop:
        rep #$30
        .byte $42, $02         ; WDM #$02 - frame marker for the harness (NOP on hardware)
        jsr erase_cursor
        jsr tick
        jsr poll_keyboard
        jsr poll_mouse
        jsr handle_clicks
        jsr handle_drag
        jsr handle_events
        jsr app_ticks
        jsr dispatch_app_ticks   ; per-frame tick of every resident disk app
        jsr draw_cursor
        bra MainLoop

; ============================================================================
; clear_state: zero VARS and clear the SCB/palette setup later. (enter a16)
; ============================================================================
.a16
.i16
clear_state:
        lda #$0000
        ldx #$0000
@v:     sta VARS,x
        inx
        inx
        cpx #$0500
        bne @v
        rts

; init_scb_pal: 200 SCBs -> palette 0/320 mode, copy palette line 0.
.a16
.i16
init_scb_pal:
        sep #$20
        ldx #0
        lda #$00
@scb:   sta f:SHRSCBL,x
        inx
        cpx #200
        bcc @scb
        ldx #0
@pal:   lda f:pal_main,x
        sta f:SHRPALL,x
        inx
        cpx #32
        bcc @pal
        rep #$30
        rts

; ============================================================================
; tick: advance frame + second counters (60 frames per second)
; ============================================================================
.a16
.i16
tick:
        inc v_frame
        lda v_frac
        inc a
        cmp #TICKS_SEC
        bcc @nos
        lda #0
        inc v_secs
@nos:   sta v_frac
        rts

; ============================================================================
; SHR primitives
; ============================================================================

; calc_gp_px: GP = SHR_PIX + PY*160 + PX  (PX byte, PY row; 16-bit)
.a16
.i16
calc_gp_px:
        lda PY
        asl a
        asl a
        asl a
        asl a
        asl a
        sta mtmp2              ; PY*32
        asl a
        asl a
        clc
        adc mtmp2             ; *160
        clc
        adc #SHR_PIX
        clc
        adc PX
        sta GP
        rts

; fill_band: fill PW(bytes) x PH(rows) at (PX,PY) with PB. (enter/exit a16)
.a16
.i16
fill_band:
        jsr calc_gp_px
        lda PW
        beq @ret              ; 0-width fill: nothing (matches old no-op loop)
        ; P3: fill by 16-bit word stores - two pixel-pair bytes per store instead
        ; of the old sep-#$20 byte-at-a-time loop. Most widths are cx*4 (even), but
        ; window borders are 1 byte wide, so store full words then a single tail
        ; byte for an odd width -> byte-identical for any PW. Replicate PB into
        ; both bytes of a word (as fill_screen does).
        lda PB
        and #$00FF
        sta mtmp2
        asl a
        asl a
        asl a
        asl a
        asl a
        asl a
        asl a
        asl a
        ora mtmp2
        sta mtmp2             ; mtmp2 = PB in both bytes (survives the row loop)
        lda PW
        dec a
        sta mtmp              ; mtmp = PW-1: word-store while y < PW-1 (>=2 left)
        lda PH
        sta rowc16
@row:   ldy #0
        lda mtmp2
@col:   cpy mtmp
        bcs @tail             ; fewer than 2 bytes remain in this row
        sta [GP],y
        iny
        iny
        bra @col
@tail:  cpy PW
        bcs @next             ; even width: no trailing byte
        sep #$20
        sta [GP],y            ; odd width: final single byte (A low = PB)
        rep #$20
@next:  lda GP
        clc
        adc #ROWBYTES
        sta GP
        dec rowc16
        bne @row
@ret:   rts

; fill_screen: fill all NPIX bytes with PB (replicated word). (a16)
.a16
.i16
fill_screen:
        lda PB
        and #$00FF
        sta mtmp2
        asl a
        asl a
        asl a
        asl a
        asl a
        asl a
        asl a
        asl a
        ora mtmp2
        ldx #0
@l:     sta f:SHRPIXL,x
        inx
        inx
        cpx #NPIX
        bcc @l
        rts

; set_attr: A = attr index (0..3) -> cur_fg/bg (+ <<4 forms). (a16)
.a16
.i16
set_attr:
        and #$00FF
        tax
        sep #$20
        lda f:attr_fg,x
        sta cur_fg
        asl a
        asl a
        asl a
        asl a
        sta cur_fgh
        lda f:attr_bg,x
        sta cur_bg
        asl a
        asl a
        asl a
        asl a
        sta cur_bgh
        rep #$20
        rts

; render_glyph: A = char (low byte), GP set, cur_* set. Draws 8x8 4bpp,
; advances GP by 4 (one glyph). (enter/exit a16, i16)
.a16
.i16
render_glyph:
        and #$00FF
        sec
        sbc #FONT_FIRST
        bcc @blank
        cmp #FONT_GLYPHS
        bcc @ok
@blank: lda #0
@ok:    asl a
        asl a
        asl a                  ; *8
        sta GIDX
        sep #$20
        lda #8
        sta rowc16
rg_row:
        ldx GIDX
        lda f:font_data,x
        sta fbits
        rep #$20
        inc GIDX
        sep #$20
        ldy #0
        PIXBYTE $80, $40
        sta [GP],y
        iny
        PIXBYTE $20, $10
        sta [GP],y
        iny
        PIXBYTE $08, $04
        sta [GP],y
        iny
        PIXBYTE $02, $01
        sta [GP],y
        rep #$20
        lda GP
        clc
        adc #ROWBYTES
        sta GP
        sep #$20
        dec rowc16
        beq rg_done
        jmp rg_row
rg_done:
        rep #$20
        lda GP
        sec
        sbc #ROWBYTES*8
        clc
        adc #4
        sta GP
        rts

; ============================================================================
; Cell-level primitives (cell = 8x8 px). Signatures mirror the SNES port.
; ============================================================================

; fill_cells: A0=cx A1=cy A2=ncols A3=nrows A4=attr. (a16)
.a16
.i16
fill_cells:
        lda A0
        asl a
        asl a
        sta PX                 ; cx*4 bytes
        lda A1
        asl a
        asl a
        asl a
        sta PY                 ; cy*8 rows
        lda A2
        asl a
        asl a
        sta PW                 ; ncols*4 bytes
        lda A3
        asl a
        asl a
        asl a
        sta PH                 ; nrows*8 rows
        lda A4
        and #$00FF
        tax
        sep #$20
        lda f:attr_bg,x
        sta pxtmp
        asl a
        asl a
        asl a
        asl a
        ora pxtmp              ; bg*$11
        sta PB
        rep #$20
        jsr fill_band
        rts

; draw_str: P0=ptr (bank0) A0=cx A1=cy A4=attr. (a16)
.a16
.i16
draw_str:
        lda A4
        jsr set_attr
        lda A0
        asl a
        asl a
        sta PX
        lda A1
        asl a
        asl a
        asl a
        sta PY
        jsr calc_gp_px
        lda P0
        sta STRP
        sep #$20
        stz STRP+2
        rep #$20
        stz S7                 ; string index (preserved across render_glyph)
@ch:    ldy S7
        sep #$20
        lda [STRP],y
        rep #$20
        and #$00FF
        beq @done
        jsr render_glyph
        inc S7
        bra @ch
@done:  rts

; draw_char: A0=cx A1=cy A2=char A4=attr. (a16)
.a16
.i16
draw_char:
        lda A4
        jsr set_attr
        lda A0
        asl a
        asl a
        sta PX
        lda A1
        asl a
        asl a
        asl a
        sta PY
        jsr calc_gp_px
        lda A2
        jsr render_glyph
        rts

; fillcell: A0=cx A1=cy A2=colour index -> fill one 8x8 cell with a solid 4bpp
; colour byte (both nibbles = the index).  A kernel primitive (the colour apps
; - Dostris/Paint/Pac-Man/OutLast - all paint through it). Exported for apps.
.a16
.i16
fillcell:
        lda A0
        asl a
        asl a
        sta PX
        lda A1
        asl a
        asl a
        asl a
        sta PY
        lda #4
        sta PW
        lda #8
        sta PH
        lda A2
        and #$000F
        sta pxtmp
        asl a
        asl a
        asl a
        asl a
        ora pxtmp
        sta PB
        jsr fill_band
        rts

; ============================================================================
; Ensoniq 5503 DOC sound engine (kernel-resident infra, like the SHR renderer).
; The Music/Tracker disk apps call the exported snd_note/snd_off; doc_init runs
; at boot.  Sound GLU: $C03E/$C03F=DOC address, $C03C=control (bit5 reg/RAM,
; bit6 autoinc), $C03D=data.  Per-osc register bases below.
; ============================================================================
DOC_FREQL = $00
DOC_FREQH = $20
DOC_VOL   = $40
DOC_WTPTR = $80
DOC_CTRL  = $A0
DOC_WTSZ  = $C0
DOC_OSCEN = $E1

; doc_write: A2 low = DOC register, A3 low = value.
.a16
.i16
doc_write:
        sep #$20
        lda A2
        sta f:SNDADRL
        lda #0
        sta f:SNDADRH
        lda #$00
        sta f:SNDCTL
        lda A3
        sta f:SNDDATA
        rep #$20
        rts

; doc_load_wave: write a 256-byte sawtooth into DOC RAM at $0000.
.a16
.i16
doc_load_wave:
        sep #$20
        rep #$10
        lda #0
        sta f:SNDADRL
        lda #0
        sta f:SNDADRH
        lda #$60
        sta f:SNDCTL
        ldx #0
@w:     txa
        ora #$01
        sta f:SNDDATA
        inx
        cpx #256
        bcc @w
        rep #$30
        rts

; doc_init: halt all 32 oscillators, set the osc-enable scan, load wave.
.a16
.i16
doc_init:
        ldx #0
@halt:  txa
        clc
        adc #DOC_CTRL
        sta A2
        lda #$01
        sta A3
        phx
        jsr doc_write
        plx
        inx
        cpx #32
        bcc @halt
        lda #DOC_OSCEN
        sta A2
        lda #(8*2)
        sta A3
        jsr doc_write
        jsr doc_load_wave
        rts

; snd_note: A0 = oscillator, A1 = DOC frequency word. Programs + starts it.
.a16
.i16
snd_note:
        sep #$20
        lda A0
        sta A2
        lda A1
        sta A3
        rep #$20
        jsr doc_write
        sep #$20
        lda A0
        clc
        adc #DOC_FREQH
        sta A2
        lda A1+1
        sta A3
        rep #$20
        jsr doc_write
        sep #$20
        lda A0
        clc
        adc #DOC_VOL
        sta A2
        lda #$C0
        sta A3
        rep #$20
        jsr doc_write
        sep #$20
        lda A0
        clc
        adc #DOC_WTPTR
        sta A2
        stz A3
        rep #$20
        jsr doc_write
        sep #$20
        lda A0
        clc
        adc #DOC_WTSZ
        sta A2
        stz A3
        rep #$20
        jsr doc_write
        sep #$20
        lda A0
        clc
        adc #DOC_CTRL
        sta A2
        stz A3
        rep #$20
        jsr doc_write
        rts

; snd_off: A0 = oscillator -> halt it.
.a16
.i16
snd_off:
        sep #$20
        lda A0
        clc
        adc #DOC_CTRL
        sta A2
        lda #$01
        sta A3
        rep #$20
        jsr doc_write
        rts

; clear_screen: desktop blue (index 0). (a16)
.a16
.i16
clear_screen:
        stz PB
        jsr fill_screen
        rts

; ============================================================================
; Desktop
; ============================================================================
.a16
.i16
draw_desktop:
        ; menu bar row 0: white (INV), title in blue
        stz A0
        stz A1
        lda #SCRW_C
        sta A2
        lda #1
        sta A3
        lda #ATTR_INV
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
        ; version / build line at the bottom-left
        lda #.loword(str_version)
        sta P0
        stz A0
        lda #(SCRH_C-1)
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        ; icons
        stz LC0
@icon:  lda LC0
        jsr draw_icon
        inc LC0
        lda LC0
        cmp #NICONS
        bne @icon
        rts

; draw_icon: A = icon index (label, inverted when selected). (a16)
.a16
.i16
draw_icon:
        and #$00FF
        sta S4
        asl a
        asl a
        tax
        lda f:icon_tab,x
        sta A0
        lda f:icon_tab+2,x
        sta A1
        lda S4
        asl a
        tax
        lda f:icon_names,x
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

; icon_at: A0=cx A1=cy -> A = icon index or $FFFF. (a16)
.a16
.i16
icon_at:
        stz S2
@scan:  lda S2
        asl a
        asl a
        tax
        lda f:icon_tab,x
        sta S0
        lda f:icon_tab+2,x
        cmp A1
        bne @next
        lda A0
        sec
        sbc S0
        bcc @next
        cmp #9
        bcs @next
        lda S2
        rts
@next:  inc S2
        lda S2
        cmp #NICONS
        bne @scan
        lda #$FFFF
        rts

; select_icon: A = icon index. Redraw old (unselected) + new (selected). (a16)
.a16
.i16
select_icon:
        sta S7
        lda v_sel_icon
        sta S6
        lda S7
        sta v_sel_icon
        lda S6
        cmp #$FFFF
        beq @new
        jsr draw_icon
@new:   lda S7
        jsr draw_icon
        rts

; ============================================================================
; Window manager (cell coordinates) - ported from snes/kernel.asm
; ============================================================================
.a16
.i16
ent_x:
        and #$00FF
        win_index_to_x          ; generated: A=slot -> X=index*16 (stride from Contract)
        rts

.a16
.i16
zent_x:
        and #$00FF
        tax
        sep #$20
        lda v_zlist,x
        rep #$20
        and #$00FF
        win_index_to_x          ; generated: A=slot -> X=index*16 (stride from Contract)
        rts

; ============================================================================
; Disk-app loader (the deferred "diskapp" ABI).  Each app proc maps to a FAT12
; file (<NAME>.APP) and a fixed bank-0 load region (one region per resident
; app, so several stay resident at once - see sys.inc).  ensure_loaded reads
; the binary in the first time the proc is launched; the WM then dispatches
; draw/key/tick/start through the app's loaded JMP vector table.  SysInfo
; (proc 0) + Clock (proc 1) have no disk binary (app_vec_tab entry 0) and stay
; kernel-resident as static desktop info windows.
; ============================================================================

; v_loadproc: stash so ensure_loaded never relies on S2/S3 (launch_app's). (zp-free VAR)
v_loadproc = VARS+$2B0
v_loadbin  = VARS+$2B2

; ensure_loaded: A = proc.  If the proc has a disk binary that isn't resident,
; locate <NAME>.APP in the (already-mounted) root dir and read it into the
; proc's load region, then mark its binary resident.  Clobbers A0-A4/F*/DPTR/P0.
.a16
.i16
ensure_loaded:
        and #$00FF
        sta v_loadproc
        asl a
        tax
        lda f:app_vec_tab,x
        beq @done                ; built-in proc (SysInfo/Clock): nothing to load
        ldx v_loadproc
        sep #$20
        lda f:app_resid_tab,x    ; binary id
        rep #$20
        and #$00FF
        sta v_loadbin
        tax
        sep #$20
        lda v_app_res,x
        rep #$20
        and #$00FF
        bne @done                ; already resident
        ; name pointer + load address for this proc
        lda v_loadproc
        asl a
        tax
        lda f:app_name_tab,x
        sta P1                   ; name ptr (bank 0)
        lda f:app_load_tab,x
        sta DPTR                 ; load dest
        jsr fat_load_named
        ldx v_loadbin
        sep #$20
        lda #1
        sta v_app_res,x          ; mark this binary resident
        rep #$20
@done:  rts

; fat_load_named: P1 = 11-byte FAT name ptr, DPTR = dest.  Scans the on-disk
; root directory (NOT the user-facing v_dir_list, which hides .APP system
; files) for the name, then fat_read_file (first cluster + size) into DPTR.
; No-op if not found (the slot keeps whatever it had).  Uses DIRSEC; the
; cluster/size of the match go to A0/F2 for fat_read_file.
.a16
.i16
fat_load_named:
        lda #ROOT_START
        sta F3                   ; volume LBA
        stz F4                   ; sectors scanned
@sec:   lda F3
        sta A2
        lda #.loword(DIRSEC)
        sta P0
        lda #1
        sta A3
        jsr blk_io
        stz F5                   ; entry 0..15 within sector
@ent:   lda F5
        asl a
        asl a
        asl a
        asl a
        asl a                    ; *32
        sta F6                   ; entry offset in DIRSEC
        tax
        lda DIRSEC,x
        and #$00FF
        beq @nf                  ; $00 = end of directory
        cmp #$00E5
        beq @nextent             ; deleted
        ; compare the 11 name bytes
        ldx F6
        ldy #0
        sep #$20
@cmp:   lda DIRSEC,x
        cmp (P1),y
        bne @cmpno
        inx
        iny
        cpy #11
        bcc @cmp
        rep #$20
        ; match: first cluster (entoff+26), size low (entoff+28)
        ldx F6
        lda DIRSEC+26,x
        sta A0
        lda DIRSEC+28,x
        sta F2
        lda DPTR
        sta P0
        jsr fat_read_file
        rts
@cmpno: rep #$20
@nextent:
        inc F5
        lda F5
        cmp #16
        bcc @ent
        inc F3
        inc F4
        lda F4
        cmp #ROOT_SECS
        bcc @sec
@nf:    rts

; return_to_desktop: an app calls this to close its own (topmost) window and
; return to the desktop.  The app code stays resident.
.a16
.i16
return_to_desktop:
        jsr close_topmost
        rts

; dispatch_app_ticks: per-frame, call the TICK vector of every resident disk
; app (each app's tick scans the window table for its own proc - this IS the
; cooperative scheduler; multiple resident apps all advance each frame).  Loop
; state is in abs VARS, not zp, because a resident app's tick redraws (which
; clobbers the zp pseudo-regs).
.a16
.i16
dispatch_app_ticks:
        stz v_tickidx            ; binary id 0..NAPPS-1
@loop:  lda v_tickidx
        cmp #NAPPS
        bcs @done
        tax
        sep #$20
        lda v_app_res,x
        rep #$20
        and #$00FF
        beq @next
        ; tick vector = app_bin_base[binid] + VEC_TICK
        lda v_tickidx
        asl a
        tax
        lda f:app_bin_base,x
        clc
        adc #VEC_TICK
        sta v_vecptr
        jsr call_vec
@next:  inc v_tickidx
        bra @loop
@done:  rts

; call_vec: indirect-call trampoline.  v_vecptr = target address; the tail-jump
; runs the app vector, which RTSes to call_vec's caller.  Used to dispatch into
; a loaded app's JMP vector table without self-modifying code.
.a16
.i16
call_vec:
        jmp (v_vecptr)

; app_vec_base: A = proc -> A = the proc's loaded 4-vector table base (0 if a
; built-in proc with no disk binary).
.a16
.i16
app_vec_base:
        and #$00FF
        asl a
        tax
        lda f:app_vec_tab,x
        rts

; kern_notepad_defaults: seed Notepad's shared state at boot (the buffer is
; empty; the default 8.3 name).  The Notepad disk app owns the per-open logic,
; but the buffer state lives in shared VARS so it survives before first load.
.a16
.i16
kern_notepad_defaults:
        stz v_np_len
        stz v_np_dirty
        stz v_np_loaded
        ldx #0
        sep #$20
@cn:    lda f:kern_note_defname,x
        sta v_np_name,x
        inx
        cpx #11
        bcc @cn
        rep #$20
        rts
kern_note_defname: .byte "NOTE    TXT"

.a16
.i16
launch_app:
        sta S3
        jsr ensure_loaded        ; A = proc; read its disk binary if not resident
        lda S3
        stz S2
@scan:  lda S2
        jsr ent_x
        sep #$20
        lda v_wintab+WSTATE,x
        beq @next
        lda v_wintab+WPROC,x
        cmp S3
        bne @next
        rep #$20
        lda S2
        jsr z_index_of
        cmp #$FFFF
        beq @next16
        jsr raise_window
        rts
@next:  rep #$20
@next16:
        inc S2
        lda S2
        cmp #MAXWIN
        bne @scan
        lda S3
        jsr win_create
        rts

.a16
.i16
win_create:
        sta S3
        stz S2
@find:  lda S2
        jsr ent_x
        sep #$20
        lda v_wintab+WSTATE,x
        rep #$20
        beq @got
        inc S2
        lda S2
        cmp #MAXWIN
        bne @find
        rts
@got:   ; def offset = proc*10 -> X (long index; 65816 long-indexed is X-only)
        lda S3
        asl a                  ; *2
        sta S0
        asl a
        asl a                  ; *8
        clc
        adc S0                 ; *10
        tax
        ; read the 5 geometry/title words into temps via long,x
        lda f:app_def_tab+0,x
        sta S4
        lda f:app_def_tab+2,x
        sta S5
        lda f:app_def_tab+4,x
        sta S6
        lda f:app_def_tab+6,x
        sta S7
        lda f:app_def_tab+8,x
        sta A6
        ; now X = entry offset, store the new window
        lda S2
        jsr ent_x
        sep #$20
        lda #$01
        sta v_wintab+WSTATE,x
        lda S3
        sta v_wintab+WPROC,x
        rep #$20
        lda S4
        sta v_wintab+WX,x
        lda S5
        sta v_wintab+WY,x
        lda S6
        sta v_wintab+WW,x
        lda S7
        sta v_wintab+WH,x
        lda A6
        sta v_wintab+WTITLE,x
        lda v_zcount
        phx
        tax
        lda S2
        sep #$20
        sta v_zlist,x
        rep #$20
        plx
        inc v_zcount
        ; per-app open hook: call the loaded app's START vector (the app inits
        ; its own state - new game / fresh pattern / Music begins playing / a
        ; fresh-or-preloaded Notepad buffer).  Built-ins (SysInfo/Clock) skip.
        lda S3
        jsr app_vec_base
        beq @nohook
        clc
        adc #VEC_START
        sta v_vecptr
        jsr call_vec           ; app START RTSes back here
@nohook:
        jsr redraw_topmost     ; new window is topmost; iigs has no active-title
        rts                    ; restyle + static desktop -> only it changed (P1)

.a16
.i16
z_index_of:
        and #$00FF
        sta S0
        stz S1
@scan:  lda S1
        cmp v_zcount
        bcs @no
        lda S1
        tax
        sep #$20
        lda v_zlist,x
        rep #$20
        and #$00FF
        cmp S0
        beq @yes
        inc S1
        bra @scan
@yes:   lda S1
        rts
@no:    lda #$FFFF
        rts

.a16
.i16
find_window_at:
        lda v_zcount
        beq @no
        dec a
        sta S2
@scan:  lda S2
        bmi @no
        jsr zent_x
        lda A0
        cmp v_wintab+WX,x
        bcc @next
        lda v_wintab+WX,x
        clc
        adc v_wintab+WW,x
        dec a
        cmp A0
        bcc @next
        lda A1
        cmp v_wintab+WY,x
        bcc @next
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        dec a
        cmp A1
        bcc @next
        lda S2
        rts
@next:  dec S2
        bra @scan
@no:    lda #$FFFF
        rts

.a16
.i16
raise_window:
        sta S0
        lda v_zcount
        dec a
        cmp S0
        beq @done
        lda S0
        tax
        sep #$20
        lda v_zlist,x
        sta S1
        rep #$20
@shift: lda S0
        cmp v_zcount
        bcs @place
        inc a
        cmp v_zcount
        bcs @place
        ldx S0
        sep #$20
        lda v_zlist+1,x
        sta v_zlist,x
        rep #$20
        inc S0
        bra @shift
@place: ldx S0
        sep #$20
        lda S1
        sta v_zlist,x
        rep #$20
        jsr redraw_topmost     ; raised window goes topmost; nothing else changes
@done:  rts

.a16
.i16
close_window:
        sta S0
        jsr zent_x
        sep #$20
        stz v_wintab+WSTATE,x
        rep #$20
        lda v_zcount
        dec a
        sta v_zcount
@shift: lda S0
        cmp v_zcount
        bcs @done
        ldx S0
        sep #$20
        lda v_zlist+1,x
        sta v_zlist,x
        rep #$20
        inc S0
        bra @shift
@done:  jsr repaint_all
        rts

.a16
.i16
close_topmost:
        lda v_zcount
        beq @out
        dec a
        jsr close_window
@out:   rts

.a16
.i16
repaint_all:
        jsr clear_screen
        jsr draw_desktop
        stz LC1
@wins:  lda LC1
        cmp v_zcount
        bcs @done
        jsr zent_x
        jsr draw_window
        inc LC1
        bra @wins
@done:  rts

.a16
.i16
redraw_topmost:
        lda v_zcount
        beq @out
        dec a
        jsr zent_x
        jsr draw_window
@out:   rts

; draw_window: X = entry byte offset
.a16
.i16
draw_window:
        stx S2
        ; body NORM (blue) whole window
        ldx S2
        lda v_wintab+WX,x
        sta A0
        lda v_wintab+WY,x
        sta A1
        lda v_wintab+WW,x
        sta A2
        lda v_wintab+WH,x
        sta A3
        lda #ATTR_NORM
        sta A4
        jsr fill_cells
        ; title bar INV (white) 1 cell
        ldx S2
        lda v_wintab+WX,x
        sta A0
        lda v_wintab+WY,x
        sta A1
        lda v_wintab+WW,x
        sta A2
        lda #1
        sta A3
        lda #ATTR_INV
        sta A4
        jsr fill_cells
        jsr draw_frame_px
        ; title text INV
        ldx S2
        lda v_wintab+WTITLE,x
        sta P0
        lda v_wintab+WX,x
        sta A0
        lda v_wintab+WY,x
        sta A1
        lda #ATTR_INV
        sta A4
        jsr draw_str
        ; close 'X' INV at top-right
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
        ; content
        ldx S2
        sep #$20
        lda v_wintab+WPROC,x
        rep #$20
        and #$00FF
        jsr app_draw_content
        rts

; draw_frame_px: thin white outline of the window (S2 = entry offset).
.a16
.i16
draw_frame_px:
        ; top
        ldx S2
        lda v_wintab+WX,x
        asl a
        asl a
        sta PX
        lda v_wintab+WY,x
        asl a
        asl a
        asl a
        sta PY
        lda v_wintab+WW,x
        asl a
        asl a
        sta PW
        lda #1
        sta PH
        lda #$11
        sta PB
        jsr fill_band
        ; bottom
        ldx S2
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        asl a
        asl a
        asl a
        dec a
        sta PY
        lda #1
        sta PH
        jsr fill_band
        ; left
        ldx S2
        lda v_wintab+WX,x
        asl a
        asl a
        sta PX
        lda v_wintab+WY,x
        asl a
        asl a
        asl a
        sta PY
        lda #1
        sta PW
        lda v_wintab+WH,x
        asl a
        asl a
        asl a
        sta PH
        jsr fill_band
        ; right
        ldx S2
        lda v_wintab+WX,x
        clc
        adc v_wintab+WW,x
        asl a
        asl a
        dec a
        sta PX
        lda v_wintab+WY,x
        asl a
        asl a
        asl a
        sta PY
        lda #1
        sta PW
        lda v_wintab+WH,x
        asl a
        asl a
        asl a
        sta PH
        jsr fill_band
        rts

; app_draw_content: A = proc index, S2 = entry offset.  Built-ins (SysInfo/
; Clock) draw in-kernel; disk apps dispatch through their loaded DRAW vector.
.a16
.i16
app_draw_content:
        cmp #0
        beq @sysinfo
        cmp #1
        beq @clock
        ; disk app: vector base + VEC_DRAW
        jsr app_vec_base
        beq @none
        clc
        adc #VEC_DRAW
        sta v_vecptr
        jmp (v_vecptr)           ; app DRAW RTSes to draw_window's caller
@none:  rts
@sysinfo:
        jmp sysinfo_draw
@clock:
        jmp clock_draw

; ============================================================================
; SysInfo (proc 0) + Clock (proc 1)
; ============================================================================
.a16
.i16
sysinfo_draw:
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta S3                 ; left col
        lda v_wintab+WY,x
        clc
        adc #2
        sta A6                 ; top row
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
        lda #.loword(str_si5)
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
        ; uptime (accent)
        lda v_secs
        sta S0
        jsr fmt_dec
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

.a16
.i16
clock_draw:
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
        ; HH:MM:SS into v_clkbuf
        lda #.loword(v_clkbuf)
        sta P1
        lda v_secs
        sta S0
        lda #60
        sta S1
        jsr div16              ; A = total minutes, S0 = seconds
        sta S7
        lda S0
        sta A5                 ; seconds
        lda S7
        sta S0
        lda #60
        sta S1
        jsr div16              ; A = hours, S0 = minutes
        sta A6
        lda S0
        sta A3
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
        lda #0
        sta (P1)
        rep #$20
        ; centred accent time
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

; ============================================================================
; Number formatting
; ============================================================================
.a16
.i16
div16:
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

.a16
.i16
put2dig:
        sta S0
        lda #10
        sta S1
        jsr div16
        clc
        adc #'0'
        pha
        lda S0
        clc
        adc #'0'
        sta S4
        pla
        jsr putchar
        lda S4
        jsr putchar
        rts

.a16
.i16
putchar:
        sep #$20
        sta (P1)
        rep #$20
        inc P1
        rts

.a16
.i16
fmt_dec:
        stz S5
@gen:   lda #10
        sta S1
        jsr div16
        sta S6
        lda S0
        clc
        adc #'0'
        pha
        inc S5
        lda S6
        sta S0
        bne @gen
        lda #.loword(v_numbuf)
        sta P1
@pop:   pla
        jsr putchar
        dec S5
        bne @pop
        sep #$20
        lda #0
        sta (P1)
        rep #$20
        rts

; ============================================================================
; Event queue
; ============================================================================
.a16
.i16
ev_post:
        lda v_ev_tail
        sta S6
        inc a
        and #(EVQ_SIZE-1)
        sta S7
        cmp v_ev_head
        beq @full
        lda S6
        asl a
        asl a
        tax
        sep #$20
        lda A0
        sta v_evq,x
        rep #$20
        lda A1
        sta v_evq+2,x
        lda S7
        sta v_ev_tail
@full:  rts

.a16
.i16
ev_get:
        lda v_ev_head
        cmp v_ev_tail
        beq @empty
        sta S6
        asl a
        asl a
        tax
        sep #$20
        lda v_evq,x
        sta S7
        rep #$20
        lda v_evq+2,x
        sta A1
        lda S6
        inc a
        and #(EVQ_SIZE-1)
        sta v_ev_head
        lda S7
        and #$00FF
        rts
@empty: lda #0
        rts

; ============================================================================
; Input
; ============================================================================

; poll_keyboard: $C000 latch -> EV_KEY (ascii in A1 low). (a16)
.a16
.i16
poll_keyboard:
        sep #$20
        lda f:KBD
        bpl @none
        and #$7F
        sta kbtmp
        lda f:KBDSTRB            ; clear strobe
        rep #$20
        lda kbtmp
        and #$00FF
        sta A1
        lda #EV_KEY
        sta A0
        jsr ev_post
        rts
@none:  rep #$20
        rts

; poll_mouse: drain the ADB delta FIFO into v_mouse_x/y; latch press. (a16)
.a16
.i16
poll_mouse:
@loop:  sep #$20
        lda f:MOUSESTA
        bpl @btn               ; bit7 clear: no movement pending
        rep #$20
        ; dx
        sep #$20
        lda f:MOUSEDLT
        rep #$20
        and #$00FF
        cmp #$0080
        bcc @dxp
        ora #$FF00
@dxp:   clc
        adc v_mouse_x
        bpl @dxnn
        lda #0
@dxnn:  cmp #320
        bcc @dxok
        lda #319
@dxok:  sta v_mouse_x
        ; dy
        sep #$20
        lda f:MOUSEDLT
        rep #$20
        and #$00FF
        cmp #$0080
        bcc @dyp
        ora #$FF00
@dyp:   clc
        adc v_mouse_y
        bpl @dynn
        lda #0
@dynn:  cmp #200
        bcc @dyok
        lda #199
@dyok:  sta v_mouse_y
        bra @loop
@btn:   rep #$20
        sep #$20
        lda f:MOUSESTA
        and #$01
        rep #$20
        and #$00FF
        sta v_mouse_btn
        cmp v_last_btn
        beq @done
        sta v_last_btn
        cmp #1
        bne @done
        ; press edge: latch position + sequence
        lda v_mouse_x
        sta v_click_x
        lda v_mouse_y
        sta v_click_y
        inc v_click_seq
@done:  rts

; handle_clicks: consume the press latch (PORT-SPEC rule 4). (a16)
.a16
.i16
handle_clicks:
        lda v_click_seq
        cmp v_last_seq
        bne @work
        rts
@work:  sta v_last_seq
        lda v_click_x
        lsr a
        lsr a
        lsr a
        sta A0                 ; cell x
        lda v_click_y
        lsr a
        lsr a
        lsr a
        sta A1                 ; cell y
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
        rts
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
        beq @desel
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
        lda f:icon_procs,x
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
@desel: lda #$FFFF
        sta v_dbl_icon
        rts

; handle_drag: live cell-snapped drag while button held. (a16)
.a16
.i16
handle_drag:
        lda v_drag_active
        bne @active
        rts
@active:
        lda v_mouse_btn
        bne @held
        stz v_drag_active
        rts
@held:  ldx v_drag_win
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
@ymin:  lda #SCRH_C             ; B1: max WY = SCRH_C - WH (keep the window's
        sec                     ; bottom edge on screen), mirroring the X clamp.
        sbc v_wintab+WH,x       ; was #(SCRH_C-1): ignored height -> drag past the
        cmp A1                  ; SHR framebuffer into the SCB/palette bytes.
        bcs @yok
        sta A1
@yok:   ldx v_drag_win
        lda A0
        cmp v_wintab+WX,x
        bne @move
        lda A1
        cmp v_wintab+WY,x
        beq @done
@move:  ldx v_drag_win
        lda A0
        sta v_wintab+WX,x
        lda A1
        sta v_wintab+WY,x
        jsr repaint_all
@done:  rts

; handle_events: drain the queue. ESC closes topmost; else route by focus. (a16)
.a16
.i16
handle_events:
@next:  jsr ev_get
        cmp #0
        bne @hk
        rts
@hk:    cmp #EV_KEY
        bne @next
        lda A1
        and #$00FF
        sta S0                 ; ascii
        lda v_zcount
        beq @desktop
        lda S0
        cmp #$1B
        bne @app
        jsr close_topmost
        bra @next
@app:   ; route the key to the topmost window's app handler
        lda v_zcount
        dec a
        jsr zent_x
        sep #$20
        lda v_wintab+WPROC,x
        rep #$20
        and #$00FF
        jsr app_key            ; A=proc, S0=ascii
        bra @next
@desktop:
        lda S0
        cmp #$15               ; right arrow
        beq @right
        cmp #$08               ; left arrow
        beq @left
        cmp #$0D               ; return
        beq @launch
        bra @next
@right: lda v_sel_icon
        cmp #$FFFF
        beq @selz
        inc a
        cmp #NICONS
        bcc @setsel
@selz:  lda #0
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
        beq @ldone
        asl a
        tax
        lda f:icon_procs,x
        jsr launch_app
@ldone: jmp @next

; app_ticks: refresh topmost once a second (clock/uptime). (a16)
.a16
.i16
app_ticks:
        lda v_secs
        cmp v_last_secs
        beq @out
        sta v_last_secs
        lda v_zcount
        beq @out
        jsr redraw_topmost
@out:   rts

; ============================================================================
; Software cursor (save-under). 8x14 arrow, white over the desktop.
; ============================================================================
.a16
.i16
draw_cursor:
        lda v_mouse_x
        lsr a
        cmp #157
        bcc @xc
        lda #156
@xc:    sta S0                 ; xbyte
        lda v_mouse_y
        cmp #186
        bcc @yc
        lda #185
@yc:    sta S1                 ; ytop
        lda S0
        sta PX
        lda S1
        sta PY
        jsr calc_gp_px
        lda GP
        sta v_cur_gp
        sta mtmp               ; running addr
        ; save 14 rows x 4 bytes
        ldx #0
        lda #14
        sta rowc16
@srow:  ldy #0
        sep #$20
@scol:  lda [mtmp],y
        sta v_curbuf,x
        inx
        iny
        cpy #4
        bcc @scol
        rep #$20
        lda mtmp
        clc
        adc #ROWBYTES
        sta mtmp
        dec rowc16
        bne @srow
        ; draw arrow mask white
        lda v_cur_gp
        sta mtmp
        stz S2                 ; row r
@drow:  ldx S2
        sep #$20
        lda f:cursor_mask,x
        sta fbits
        rep #$20
        stz S3                 ; col c
@dcol:  ldx S3
        sep #$20
        lda f:bitmask_tab,x
        and fbits
        beq @dnext
        ; set pixel white at (mtmp + c>>1), nibble per parity
        rep #$20
        lda S3
        lsr a
        tay                    ; byte offset within row
        sep #$20
        lda [mtmp],y
        pha
        lda S3
        and #1
        bne @lo
        pla
        and #$0F
        ora #$10
        bra @wr
@lo:    pla
        and #$F0
        ora #$01
@wr:    sta [mtmp],y
        rep #$20
@dnext: rep #$20
        inc S3
        lda S3
        cmp #8
        bcc @dcol
        lda mtmp
        clc
        adc #ROWBYTES
        sta mtmp
        inc S2
        lda S2
        cmp #14
        bcc @drow
        lda #1
        sta v_cur_saved
        rts

.a16
.i16
erase_cursor:
        lda v_cur_saved
        bne @go
        rts
@go:    stz v_cur_saved
        lda v_cur_gp
        sta mtmp
        ldx #0
        lda #14
        sta rowc16
@row:   ldy #0
        sep #$20
@col:   lda v_curbuf,x
        sta [mtmp],y
        inx
        iny
        cpy #4
        bcc @col
        rep #$20
        lda mtmp
        clc
        adc #ROWBYTES
        sta mtmp
        dec rowc16
        bne @row
        rts

; app_key: A = proc index, S0 = ascii.  Dispatch to the focused app's loaded
; KEY vector (SysInfo/Clock have no key handler).
.a16
.i16
app_key:
        jsr app_vec_base
        beq @none
        clc
        adc #VEC_KEY
        sta v_vecptr
        jmp (v_vecptr)
@none:  rts

; ---- M2 storage (FAT12 over SmartPort) stays in the kernel.  Files/Notepad
;      and all M3 apps are now disk-loaded binaries (see *.s + build.sh). ----
.include "fs.i"

; ============================================================================
; Disk-app loader data tables.  Indexed by proc (0..10) except app_bin_base
; which is indexed by binary id (0..NAPPS-1).  SysInfo(0)/Clock(1) are built
; in (vector base 0).  Files(7) shares the Notepad(2) binary + region; its
; vector table sits VEC_SIZE into that region (FILES_VEC).
; ============================================================================
NAPPS = 8                        ; distinct disk binaries

.segment "RODATA"
; proc -> loaded 4-vector table base (0 = built-in)
app_vec_tab:
        .word 0, 0
        .word SLOT_FILESNP       ; 2 Notepad
        .word SLOT_THEME         ; 3
        .word SLOT_MUSIC         ; 4
        .word SLOT_DOSTRIS       ; 5
        .word SLOT_PAINT         ; 6
        .word FILES_VEC          ; 7 Files (2nd table in the filesnp region)
        .word SLOT_TRACKER       ; 8
        .word SLOT_PACMAN        ; 9
        .word SLOT_OUTLAST       ; 10
; proc -> load region base (where the binary is read)
app_load_tab:
        .word 0, 0
        .word SLOT_FILESNP, SLOT_THEME, SLOT_MUSIC, SLOT_DOSTRIS, SLOT_PAINT
        .word SLOT_FILESNP       ; 7 Files: same binary/region as Notepad
        .word SLOT_TRACKER, SLOT_PACMAN, SLOT_OUTLAST
; proc -> FAT name pointer (the .APP file to read)
app_name_tab:
        .word 0, 0
        .word fn_filesnp, fn_theme, fn_music, fn_dostris, fn_paint
        .word fn_filesnp         ; 7 Files: shares the filesnp binary
        .word fn_tracker, fn_pacman, fn_outlast
; proc -> binary id (procs sharing a binary share the residency flag)
app_resid_tab:
        .byte 0, 0
        .byte 0                  ; 2 Notepad -> binary 0 (filesnp)
        .byte 1                  ; 3 Theme
        .byte 2                  ; 4 Music
        .byte 3                  ; 5 Dostris
        .byte 4                  ; 6 Paint
        .byte 0                  ; 7 Files  -> binary 0 (filesnp)
        .byte 5                  ; 8 Tracker
        .byte 6                  ; 9 Pac-Man
        .byte 7                  ; 10 OutLast
; binary id -> load region base (for the per-frame tick dispatch)
app_bin_base:
        .word SLOT_FILESNP       ; 0 filesnp
        .word SLOT_THEME         ; 1
        .word SLOT_MUSIC         ; 2
        .word SLOT_DOSTRIS       ; 3
        .word SLOT_PAINT         ; 4
        .word SLOT_TRACKER       ; 5
        .word SLOT_PACMAN        ; 6
        .word SLOT_OUTLAST       ; 7

; 11-byte FAT names (8.3, space-padded) of the app binaries on the FAT12 volume
fn_filesnp: .byte "FILESNP APP"
fn_theme:   .byte "THEME   APP"
fn_music:   .byte "MUSIC   APP"
fn_dostris: .byte "DOSTRIS APP"
fn_paint:   .byte "PAINT   APP"
fn_tracker: .byte "TRACKER APP"
fn_pacman:  .byte "PACMAN  APP"
fn_outlast: .byte "OUTLAST APP"

; ============================================================================
.segment "RODATA"
str_menutitle: .byte "UnoDOS 3", 0
str_version:   .byte "UnoDOS 3.29  IIGS  Build 420", 0
str_t_sysinfo: .byte "System Info", 0
str_t_clock:   .byte "Clock", 0
str_t_notepad: .byte "Notepad", 0
str_t_files:   .byte "Files", 0
str_t_theme:   .byte "Theme", 0
str_t_music:   .byte "Music", 0
str_t_dostris: .byte "Dostris", 0
str_t_paint:   .byte "Paint", 0
str_t_tracker: .byte "Tracker", 0
str_t_pacman:  .byte "Pac-Man", 0
str_t_outlast: .byte "OutLast", 0
name_sysinfo:  .byte "Sys Info", 0
name_clock:    .byte "Clock", 0
name_notepad:  .byte "Notepad", 0
name_files:    .byte "Files", 0
name_theme:    .byte "Theme", 0
name_music:    .byte "Music", 0
name_dostris:  .byte "Dostris", 0
name_paint:    .byte "Paint", 0
name_tracker:  .byte "Tracker", 0
name_pacman:   .byte "Pac-Man", 0
name_outlast:  .byte "OutLast", 0
str_si1:       .byte "UnoDOS 3 / Apple IIGS", 0
str_si2:       .byte "CPU: 65C816 2.8 MHz", 0
str_si3:       .byte "Video: Super Hi-Res", 0
str_si4:       .byte "RAM: 1 MB+", 0
str_si5:       .byte "Input: ADB mouse+kbd", 0
str_uptime:    .byte "Uptime:", 0

attr_fg: .byte 1, 0, 2, 1
attr_bg: .byte 0, 1, 0, 6
bitmask_tab: .byte $80,$40,$20,$10,$08,$04,$02,$01
cursor_mask: .byte $80,$C0,$E0,$F0,$F8,$FC,$FE,$FF,$FC,$D8,$8C,$0C,$06,$06

; icon table: x cell, y cell (2 words per icon)
icon_tab:
        .word 4, 4
        .word 16, 4
        .word 26, 4
        .word 4, 8
        .word 16, 8
        .word 26, 8
        .word 4, 12
        .word 16, 12
        .word 26, 12
        .word 4, 16
        .word 16, 16
icon_names:
        .word name_sysinfo
        .word name_clock
        .word name_notepad
        .word name_files
        .word name_theme
        .word name_music
        .word name_dostris
        .word name_paint
        .word name_tracker
        .word name_pacman
        .word name_outlast
icon_procs:
        .word 0, 1, 2, 7, 3, 4, 5, 6, 8, 9, 10

; app definitions: x, y, w, h (cells), title pointer (5 words per app)
app_def_tab:
        .word 4, 4, 30, 12, str_t_sysinfo     ; 0 SysInfo
        .word 12, 8, 16, 9, str_t_clock       ; 1 Clock
        .word 2, 2, 36, 21, str_t_notepad     ; 2 Notepad
        .word 6, 5, 28, 12, str_t_theme       ; 3 Theme
        .word 10, 7, 22, 10, str_t_music      ; 4 Music
        .word 7, 2, 26, 21, str_t_dostris     ; 5 Dostris
        .word 1, 2, 38, 22, str_t_paint       ; 6 Paint
        .word 4, 3, 22, 18, str_t_files       ; 7 Files
        .word 8, 2, 24, 21, str_t_tracker     ; 8 Tracker
        .word 6, 3, 26, 17, str_t_pacman      ; 9 Pac-Man
        .word 2, 2, 38, 22, str_t_outlast     ; 10 OutLast
