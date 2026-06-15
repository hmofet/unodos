; ============================================================================
; UnoDOS/C64 kernel - milestone 1
;
; A standalone operating system for the bare Commodore 64 (6510). Loaded as a
; PRG at $0801 with a one-line BASIC stub (SYS 2061); from `start` on, UnoDOS
; owns the machine - it banks the VIC-II into a hi-res bitmap, runs its own
; renderer / window manager / desktop, scans the keyboard matrix itself, and
; never returns to BASIC. There is no KERNAL call anywhere (we SEI and poll).
;
; Unlike the 1-bit Apple II port, the C64 has COLOUR: standard hi-res bitmap
; gives two colours per 8x8 cell from screen RAM (upper nibble = "1"/fg pixel,
; lower nibble = "0"/bg), so the desktop, windows, title bars and icons are
; genuinely colourful. The Clock reads a REAL hardware clock (the CIA #1
; Time-of-Day registers, BCD), not a soft tick. SysInfo detects PAL vs NTSC
; from the raster counter. Sound is the SID.
;
; Memory map (VIC bank 1 = $4000-$7FFF):
;   $0801-$3FFF  BASIC stub + kernel code/data (~14KB)
;   $4000-$43E7  screen RAM (per-cell fg/bg colour nibbles)
;   $6000-$7F3F  hi-res bitmap (8000 bytes, 320x200)
;   $C000-$CFFF  free RAM (M2/M3 buffers)
;
; Build: dasm kernel.s -f3 [-DAUTOTEST=1] -obuild/kernel.bin   (see build.sh)
; ============================================================================

        processor 6502

; ---- zero page (we SEI and never call KERNAL, so $02-$8F is ours; $00/$01
;      are the 6510 banking port - never touch as scratch) ----
zpPtr       equ $02   ; (2) string/data pointer
zpFontPtr   equ $04   ; (2) glyph pointer
zpDst       equ $06   ; (2) bitmap dest pointer
zpScrPtr    equ $08   ; (2) screen-RAM (colour) pointer
zpCol       equ $0A   ; draw cell column 0-39
zpRow       equ $0B   ; draw cell row 0-24
zpInv       equ $0C   ; draw_char EOR mask ($00 normal, $FF inverted)
zpFCol      equ $0D   ; current cell colour (fg<<4 | bg)
zpTmp       equ $0E
zpTmp2      equ $0F
zpI         equ $10
zpJ         equ $11
zpFX        equ $12   ; fill_rows: cell column
zpFY        equ $13   ; fill_rows: pixel row 0-199
zpFW        equ $14   ; fill_rows: width in cells
zpFH        equ $15   ; fill_rows: height in pixel rows
zpFPat      equ $16   ; fill_rows: bitmap byte
zpCX        equ $17   ; color_fill: cell column
zpCY        equ $18   ; color_fill: cell row
zpCW        equ $19   ; color_fill: width in cells
zpCH        equ $1A   ; color_fill: height in cell rows
zpWX        equ $1B   ; draw_win: window cell rect
zpWY        equ $1C
zpWW        equ $1D
zpWH        equ $1E
zpWF        equ $1F   ; draw_win: focused flag 0/1
zpSIdx      equ $20   ; draw_string index
clkbuf      equ $21   ; (9) "HH:MM:SS",0
zpHH        equ $2A
zpMM        equ $2B
zpSS        equ $2C
zpKey       equ $2D   ; decoded key this scan (0 = none)
zpShift     equ $2E   ; nonzero if shift held
zpRasLo     equ $30   ; detect_video: max raster low byte seen
zpRasHi     equ $31   ; detect_video: max raster bit8 seen
zpCnt       equ $32   ; (2) detect_video loop counter
; slots the fill_rows/color_fill primitives never touch, for values that must
; survive a call to them (they clobber zpTmp/zpTmp2/zpI/zpJ/zpDst/zpScrPtr):
zpSlot      equ $35   ; draw_icons: icon loop index
zpWtop      equ $36   ; win_outline: window top pixel row
zpSel       equ $37   ; draw_icon: selected flag
zpIconX     equ $38   ; draw_icon: icon's base column (win_outline mutates zpFX)
zpCharY     equ $39   ; draw_char: saved caller Y (draw_name12 uses Y as index)
zpIconY     equ $3A   ; draw_icon: icon box top cell row

; ---- logical key codes (scan_keyboard -> zpKey, handle_key dispatch) ----
K_RET    equ $0D
K_ESC    equ $1B      ; RUN/STOP
K_LEFT   equ $11      ; CRSR<> + shift
K_RIGHT  equ $12      ; CRSR<>
K_UP     equ $13      ; CRSR^v + shift
K_DOWN   equ $14      ; CRSR^v
K_SPACE  equ $20
K_BS     equ $08      ; DEL/INST (backspace)
K_SAVE   equ $06      ; F1 (Notepad save)

; ---- colour bytes (fg<<4 | bg); see palette in harness.py ----
COL_DESK     equ $E6   ; light-blue dither on blue
COL_MENU     equ $0F   ; black on light-grey
COL_WIN      equ $01   ; black on white (window content)
COL_TITLE_F  equ $16   ; white on blue (focused title)
COL_TITLE_U  equ $0F   ; black on light-grey (unfocused title)
COL_ICON     equ $01   ; black on white (icon box)
COL_ICON_SEL equ $16   ; white on blue (selected icon label)
BORDERCOL    equ $06   ; VIC border = blue

; ---- screen layout (40 cell cols x 25 cell rows) ----
SCRCOLS  equ 40
SCRROWS  equ 25

SI_X     equ 2        ; SysInfo window (cells)
SI_Y     equ 2
SI_W     equ 26
SI_H     equ 8
CK_X     equ 2        ; Clock window (cells)
CK_Y     equ 11
CK_W     equ 14
CK_H     equ 5

; icon grid: up to 10 icons in 4 columns x 3 rows. Each box is ICONW cells
; wide x ICONH cells tall, label on the box's bottom row. Per-icon position
; comes from icon_x_tab / icon_y_tab so the grid can grow app by app.
ICONW    equ 9        ; icon box width in cells (inner label width = ICONW-2 = 7)
ICONH    equ 2
NICONS   equ 6        ; populated icon slots (grows as M3 apps are added)

; full-screen app layout (app_mode != 0): row0 = title + separator, rows1-22
; content, row23 = status/help line.
APP_CONTENT_Y  equ 8     ; pixel row of content row 1
APP_CONTENT_H  equ 176   ; 22 rows * 8px
APP_VIEW_ROWS  equ 22

; ---- C64 I/O ----
VIC_D011 equ $D011
VIC_D012 equ $D012
VIC_D016 equ $D016
VIC_D018 equ $D018
VIC_D020 equ $D020
VIC_D021 equ $D021
SID_BASE equ $D400
CIA1_PRA equ $DC00    ; keyboard column select (output)
CIA1_PRB equ $DC01    ; keyboard row read (input)
CIA1_DDRA equ $DC02
CIA1_DDRB equ $DC03
CIA1_TOD10 equ $DC08  ; TOD tenths (reading releases the latch)
CIA1_TODSEC equ $DC09
CIA1_TODMIN equ $DC0A
CIA1_TODHR  equ $DC0B  ; reading latches the time
CIA2_PRA  equ $DD00   ; VIC bank select (bits 0-1)
CIA2_DDRA equ $DD02

BITMAP   equ $6000
SCREEN   equ $4000

; ============================================================================
        org $0801

; ---- BASIC stub: 10 SYS 2061 ----
basic:
        dc.w bend               ; link to next line
        dc.w 10                 ; line number
        dc.b $9E                ; SYS token
        dc.b "2061"
        dc.b 0                  ; end of line
bend:   dc.w 0                  ; end of program

; ---- kernel entry ($080D = 2061) ----
start:
        jmp init
        dc.b "UDC1"             ; harness var-block discovery magic
        dc.w vars

init:
        sei
        cld
        ldx #$FF
        txs

        ; --- VIC bank 1 ($4000-$7FFF): CIA2 port A bits 1:0 = %10 ---
        lda CIA2_DDRA
        ora #$03
        sta CIA2_DDRA
        lda CIA2_PRA
        and #$FC
        ora #$02
        sta CIA2_PRA
        ; screen RAM at bank+$0000 = $4000, bitmap at bank+$2000 = $6000
        lda #$08
        sta VIC_D018
        ; standard hi-res bitmap mode, display on, 25 rows, 40 cols
        lda #$3B
        sta VIC_D011
        lda #$C8
        sta VIC_D016
        lda #BORDERCOL
        sta VIC_D020
        ; keyboard CIA1 directions: port A out (cols), port B in (rows)
        lda #$FF
        sta CIA1_DDRA
        lda #$00
        sta CIA1_DDRB

        jsr build_rom_tod       ; seed the TOD clock if it isn't running
        jsr detect_video        ; PAL/NTSC -> is_pal
        jsr fs_init             ; load/format the USV1 mini-FS
        jsr clear_bitmap

        lda #$FF
        sta zlist
        sta zlist+1
        sta focus               ; $FF = desktop focus

        jsr draw_desktop

        ifconst AUTOTEST
        lda #0
        jsr open_or_raise       ; open SysInfo
        lda #1
        jsr open_or_raise       ; open Clock (topmost / focused)
        lda #1
        sta sel_icon
        endif

        jsr draw_icons
        jmp mainloop

; ============================================================================
; main loop - poll the keyboard matrix (edge-detected) and refresh the clock.
; No interrupts: a single cooperative context, like the other UnoDOS ports.
; ============================================================================
mainloop:
        jsr scan_keyboard
        lda zpKey
        cmp last_key
        beq ml_clk              ; unchanged (held or still idle)
        sta last_key
        beq ml_clk              ; transitioned to 0 (release) - no event
        jsr handle_key
ml_clk:
        jsr update_clock
        jsr game_tick
        jmp mainloop

; game_tick - advance the active animating game (each manages its own rate via
; a counter). No-op for static apps. Extended as games are added.
game_tick:
        lda app_mode
        cmp #4
        bne gt_5
        jmp dostris_tick
gt_5:
        cmp #5
        bne gt_done
        jmp music_tick
        ;== M3-GAMES-TICK ==
gt_done:
        rts

; update_clock - if a second has ticked, the Clock window is open and no
; full-screen app covers the desktop, redraw HH:MM:SS.
update_clock:
        jsr read_tod
        lda zpSS
        cmp last_sec
        beq uc_done
        sta last_sec
        lda app_mode
        bne uc_done
        lda win_state+1
        beq uc_done
        jsr draw_clock_content
uc_done:
        rts

; ============================================================================
; keyboard - scan the CIA #1 matrix into a single decoded key event (zpKey).
; ============================================================================

; read_col - X = column 0..7; returns A = pressed-row mask (1 bit = pressed).
read_col:
        lda colmask,x
        sta CIA1_PRA
        lda CIA1_PRB
        eor #$FF
        rts
colmask: dc.b $FE,$FD,$FB,$F7,$EF,$DF,$BF,$7F

; scan_keyboard - scan all 8 columns into key_matrix, derive shift, and map the
; first pressed key (column- then row-order) through `keymap` into zpKey (0 if
; none). Cursor keys produce their unshifted code; with SHIFT held, CRSR<> ->
; left, CRSR^v -> up. keymap codes of 0 (shift/ctrl/unused keys) are skipped.
scan_keyboard:
        ldx #0
sk_scan:
        lda colmask,x
        sta CIA1_PRA
        lda CIA1_PRB
        eor #$FF
        sta key_matrix,x
        inx
        cpx #8
        bne sk_scan
        ; shift = LSHIFT (col1,row7) OR RSHIFT (col6,row4)
        lda key_matrix+1
        and #$80
        sta zpShift
        lda key_matrix+6
        and #$10
        ora zpShift
        sta zpShift
        ; find first pressed key with a nonzero keymap code
        lda #0
        sta zpKey
        ldx #0                  ; column
sk_col:
        lda key_matrix,x
        beq sk_colnext
        ldy #0                  ; row
sk_row:
        lsr                     ; bit0 (row Y) -> carry
        bcc sk_rownext
        sta zpTmp               ; remaining row bits
        sty zpTmp2              ; row index
        txa
        asl
        asl
        asl
        clc
        adc zpTmp2              ; idx = col*8 + row
        tay
        lda keymap,y
        beq sk_restore          ; code 0 -> ignore this key, keep scanning
        sta zpKey
        jmp sk_found
sk_restore:
        ldy zpTmp2
        lda zpTmp
sk_rownext:
        iny
        cpy #8
        bne sk_row
sk_colnext:
        inx
        cpx #8
        bne sk_col
        rts                     ; nothing pressed (zpKey = 0)
sk_found:
        lda zpKey
        cmp #K_RIGHT
        bne sk_chkdn
        lda zpShift
        beq sk_kdone
        lda #K_LEFT
        sta zpKey
        rts
sk_chkdn:
        cmp #K_DOWN
        bne sk_kdone
        lda zpShift
        beq sk_kdone
        lda #K_UP
        sta zpKey
sk_kdone:
        rts

; keymap - matrix index (col*8 + row) -> key code; 0 = ignore (shift/ctrl/
; function/graphic keys). Letters map to uppercase ASCII; F1 = save ($06).
keymap:
        dc.b $08,$0D,$12,$00,$06,$00,$00,$14   ; col0: DEL RET CRSR<> F7 F1 F3 F5 CRSR^v
        dc.b $33,$57,$41,$34,$5A,$53,$45,$00   ; col1: 3 W A 4 Z S E LSHIFT
        dc.b $35,$52,$44,$36,$43,$46,$54,$58   ; col2: 5 R D 6 C F T X
        dc.b $37,$59,$47,$38,$42,$48,$55,$56   ; col3: 7 Y G 8 B H U V
        dc.b $39,$49,$4A,$30,$4D,$4B,$4F,$4E   ; col4: 9 I J 0 M K O N
        dc.b $2B,$50,$4C,$2D,$2E,$3A,$40,$2C   ; col5: + P L - . : @ ,
        dc.b $00,$2A,$3B,$00,$00,$3D,$00,$2F   ; col6: PND * ; HOME RSHIFT = UP /
        dc.b $31,$00,$00,$32,$20,$00,$51,$1B   ; col7: 1 <- CTRL 2 SPACE C= Q STOP

; handle_key - A = decoded key (nonzero). When a full-screen app is active
; (app_mode != 0: 1=Files, 2=Notepad) keys route there; otherwise ESC closes the
; topmost window, and with desktop focus left/right move the icon selection and
; Return launches the selected app (Files is full-screen; SysInfo/Clock windows).
handle_key:
        sta zpTmp
        lda app_mode
        beq hk_desktop
        cmp #1
        beq hk_files
        cmp #2
        beq hk_notepad
        cmp #3
        beq hk_theme
        jmp app_key             ; M3 games / apps (app_mode >= 4)
hk_files:
        jmp files_key
hk_notepad:
        jmp notepad_key
hk_theme:
        jmp theme_key
hk_desktop:
        lda zpTmp
        cmp #K_ESC
        bne hk_notesc
        jsr close_topmost
        rts
hk_notesc:
        lda focus
        cmp #$FF
        bne hk_done             ; a window is focused: no app keys at M1
        lda zpTmp
        cmp #K_RIGHT
        beq hk_right
        cmp #K_LEFT
        beq hk_left
        cmp #K_RET
        beq hk_return
        rts
hk_left:
        lda sel_icon
        bne hkl_dec
        lda #(NICONS-1)
        sta sel_icon
        jmp hk_redraw
hkl_dec:
        dec sel_icon
        jmp hk_redraw
hk_right:
        lda sel_icon
        cmp #(NICONS-1)
        bne hkr_inc
        lda #0
        sta sel_icon
        jmp hk_redraw
hkr_inc:
        inc sel_icon
hk_redraw:
        jsr draw_icons
hk_done:
        rts
hk_return:
        lda sel_icon
        cmp #2
        bne hk_ret3
        jmp files_open          ; Files icon -> full-screen app
hk_ret3:
        cmp #3
        bne hk_ret4
        jmp theme_open
hk_ret4:
        cmp #4
        bne hk_ret5
        jmp dostris_open
hk_ret5:
        cmp #5
        bne hk_ret_win
        jmp music_open
        ;== M3-GAMES-RETURN ==   (extended as games are added)
hk_ret_win:
        jsr sid_click
        lda sel_icon
        jsr open_or_raise
        lda sel_icon
        sta focus
        rts

; app_key - route a key into the active M3 game/app (app_mode >= 4).
app_key:
        lda app_mode
        cmp #4
        bne ak_5
        jmp dostris_key
ak_5:
        cmp #5
        bne ak_done
        jmp music_key
        ;== M3-GAMES-KEY ==      (extended as games are added)
ak_done:
        rts

; ============================================================================
; window manager - 2 fixed windows (SysInfo=0, Clock=1), z-order in zlist
; ([0]=topmost/focused, $FF=empty). Mirrors the Apple II port.
; ============================================================================

; open_or_raise - A = window id; open at front, or raise if not topmost.
open_or_raise:
        sta zpTmp
        tay
        lda win_state,y
        bne oor_raise
        lda #1
        sta win_state,y
        lda zlist
        sta zlist+1
        lda zpTmp
        sta zlist
        jmp oor_redraw
oor_raise:
        lda zlist
        cmp zpTmp
        beq oor_done
        sta zlist+1
        lda zpTmp
        sta zlist
oor_redraw:
        jsr draw_sysinfo_win
        jsr draw_clock_win
oor_done:
        rts

; close_topmost - close zlist[0]; new top becomes focused, else desktop focus.
close_topmost:
        lda zlist
        cmp #$FF
        beq ct_done
        tay
        lda #0
        sta win_state,y
        lda zlist+1
        sta zlist
        lda #$FF
        sta zlist+1
        jsr draw_sysinfo_win
        jsr draw_clock_win
        lda zlist
        sta focus               ; $FF if none left -> desktop focus
ct_done:
        rts

; draw_sysinfo_win - dither behind it if closed, else frame+title+content.
draw_sysinfo_win:
        lda win_state
        bne dsiw_open
        lda #SI_X
        sta zpCX
        lda #SI_Y
        sta zpCY
        lda #SI_W
        sta zpCW
        lda #SI_H
        sta zpCH
        jsr restore_desktop
        rts
dsiw_open:
        lda zlist
        cmp #0
        beq dsiw_f
        lda #0
        jmp dsiw_go
dsiw_f:
        lda #1
dsiw_go:
        sta zpWF
        lda #SI_X
        sta zpWX
        lda #SI_Y
        sta zpWY
        lda #SI_W
        sta zpWW
        lda #SI_H
        sta zpWH
        lda #<msg_sysinfo
        sta zpPtr
        lda #>msg_sysinfo
        sta zpPtr+1
        jsr draw_win
        jsr draw_sysinfo_content
        rts

; draw_clock_win - same for window 1.
draw_clock_win:
        lda win_state+1
        bne dckw_open
        lda #CK_X
        sta zpCX
        lda #CK_Y
        sta zpCY
        lda #CK_W
        sta zpCW
        lda #CK_H
        sta zpCH
        jsr restore_desktop
        rts
dckw_open:
        lda zlist
        cmp #1
        beq dckw_f
        lda #0
        jmp dckw_go
dckw_f:
        lda #1
dckw_go:
        sta zpWF
        lda #CK_X
        sta zpWX
        lda #CK_Y
        sta zpWY
        lda #CK_W
        sta zpWW
        lda #CK_H
        sta zpWH
        lda #<msg_clock
        sta zpPtr
        lda #>msg_clock
        sta zpPtr+1
        jsr draw_win
        jsr draw_clock_content
        rts

; draw_win - colour the window cells, clear its bitmap, draw a 1px outline,
; then the title bar (focused = white-on-blue, unfocused = black-on-grey).
; Inputs: zpWX/zpWY/zpWW/zpWH (cells), zpWF (focus), zpPtr (title).
draw_win:
        ; content colour over the whole window
        lda zpWX
        sta zpCX
        lda zpWY
        sta zpCY
        lda zpWW
        sta zpCW
        lda zpWH
        sta zpCH
        lda #COL_WIN
        sta zpFCol
        jsr color_fill
        ; clear bitmap area (background pixels)
        lda zpWX
        sta zpFX
        lda zpWY
        jsr cy_to_py            ; A = zpWY*8
        sta zpFY
        lda zpWW
        sta zpFW
        lda zpWH
        jsr cy_to_py            ; A = zpWH*8
        sta zpFH
        lda #$00
        sta zpFPat
        jsr fill_rows
        jsr win_outline
        ; title bar colour
        lda zpWX
        sta zpCX
        lda zpWY
        sta zpCY
        lda zpWW
        sta zpCW
        lda #1
        sta zpCH
        lda zpWF
        beq dw_unf
        lda #COL_TITLE_F
        jmp dw_tcol
dw_unf:
        lda #COL_TITLE_U
dw_tcol:
        sta zpFCol
        jsr color_fill
        ; title text
        lda zpWX
        clc
        adc #1
        sta zpCol
        lda zpWY
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string
        rts

; win_outline - 1px border around the window bitmap rect: top + bottom full
; rows, left + right single-pixel columns ($80 = leftmost, $01 = rightmost).
win_outline:
        ; top
        lda zpWX
        sta zpFX
        lda zpWY
        jsr cy_to_py
        sta zpFY
        sta zpWtop              ; remember window top pixel (fill_rows clobbers zpTmp)
        lda zpWW
        sta zpFW
        lda #1
        sta zpFH
        lda #$FF
        sta zpFPat
        jsr fill_rows
        ; bottom (pixel row = top + WH*8 - 1)
        lda zpWH
        jsr cy_to_py
        clc
        adc zpWtop
        sec
        sbc #1
        sta zpFY
        lda #$FF
        sta zpFPat
        jsr fill_rows
        ; left edge (single column, $80)
        lda zpWX
        sta zpFX
        lda zpWtop
        sta zpFY
        lda #1
        sta zpFW
        lda zpWH
        jsr cy_to_py
        sta zpFH
        lda #$80
        sta zpFPat
        jsr fill_rows
        ; right edge (last column, $01)
        lda zpWX
        clc
        adc zpWW
        sec
        sbc #1
        sta zpFX
        lda #$01
        sta zpFPat
        jsr fill_rows
        rts

; restore_desktop - re-dither + recolour a cell rect after a window closes.
; Inputs: zpCX/zpCY/zpCW/zpCH (cells).
restore_desktop:
        lda theme_desk
        sta zpFCol
        jsr color_fill
        lda zpCX
        sta zpFX
        lda zpCY
        jsr cy_to_py
        sta zpFY
        lda zpCW
        sta zpFW
        lda zpCH
        jsr cy_to_py
        sta zpFH
        jsr dither_rect
        rts

; cy_to_py - A = cell count -> A = pixel count (A*8). Caller keeps A<32.
cy_to_py:
        asl
        asl
        asl
        rts

; ============================================================================
; SysInfo app
; ============================================================================
draw_sysinfo_content:
        ; line 0: machine name
        lda #(SI_X+1)
        sta zpCol
        lda #(SI_Y+2)
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda #<msg_mach
        sta zpPtr
        lda #>msg_mach
        sta zpPtr+1
        jsr draw_string
        ; line 1: CPU + clock (PAL/NTSC)
        lda #(SI_X+1)
        sta zpCol
        lda #(SI_Y+3)
        sta zpRow
        lda is_pal
        beq sic_cpu_ntsc
        lda #<msg_cpu_pal
        sta zpPtr
        lda #>msg_cpu_pal
        sta zpPtr+1
        jmp sic_cpu_go
sic_cpu_ntsc:
        lda #<msg_cpu_ntsc
        sta zpPtr
        lda #>msg_cpu_ntsc
        sta zpPtr+1
sic_cpu_go:
        jsr draw_string
        ; line 2: RAM
        lda #(SI_X+1)
        sta zpCol
        lda #(SI_Y+4)
        sta zpRow
        lda #<msg_ram
        sta zpPtr
        lda #>msg_ram
        sta zpPtr+1
        jsr draw_string
        ; line 3: video chip
        lda #(SI_X+1)
        sta zpCol
        lda #(SI_Y+5)
        sta zpRow
        lda is_pal
        beq sic_vid_ntsc
        lda #<msg_vid_pal
        sta zpPtr
        lda #>msg_vid_pal
        sta zpPtr+1
        jmp sic_vid_go
sic_vid_ntsc:
        lda #<msg_vid_ntsc
        sta zpPtr
        lda #>msg_vid_ntsc
        sta zpPtr+1
sic_vid_go:
        jsr draw_string
        ; line 4: SID
        lda #(SI_X+1)
        sta zpCol
        lda #(SI_Y+6)
        sta zpRow
        lda #<msg_sid
        sta zpPtr
        lda #>msg_sid
        sta zpPtr+1
        jsr draw_string
        rts

; ============================================================================
; Clock app - reads the CIA #1 Time-of-Day registers (BCD, a real hardware
; clock) and shows HH:MM:SS.
; ============================================================================

; read_tod - latch + read TOD into zpHH/zpMM/zpSS (BCD). Reading the hours
; register latches the whole time; reading tenths releases it.
read_tod:
        lda CIA1_TODHR
        and #$1F                ; strip the PM bit, keep BCD hour
        sta zpHH
        lda CIA1_TODMIN
        sta zpMM
        lda CIA1_TODSEC
        sta zpSS
        lda CIA1_TOD10          ; release the latch
        rts

draw_clock_content:
        jsr read_tod
        ; format BCD -> "HH:MM:SS",0
        lda zpHH
        ldy #0
        jsr fmt_bcd
        lda #$3A                ; ':'
        sta clkbuf+2
        lda zpMM
        ldy #3
        jsr fmt_bcd
        lda #$3A
        sta clkbuf+5
        lda zpSS
        ldy #6
        jsr fmt_bcd
        lda #0
        sta clkbuf+8
        ; draw it
        lda #(CK_X+3)
        sta zpCol
        lda #(CK_Y+2)
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda #<clkbuf
        sta zpPtr
        lda #>clkbuf
        sta zpPtr+1
        jsr draw_string
        rts

; fmt_bcd - A = BCD byte, Y = offset; write 2 ASCII digits to clkbuf+Y.
fmt_bcd:
        pha
        lsr
        lsr
        lsr
        lsr
        clc
        adc #$30
        sta clkbuf,y
        pla
        and #$0F
        clc
        adc #$30
        iny
        sta clkbuf,y
        dey
        rts

; build_rom_tod - if the TOD seconds read back as 0 (clock never started),
; seed it to 12:00:00 so the Clock shows a sensible time. Writing the hours
; register starts the clock ticking from the AC line frequency. (On the
; harness the TOD is driven from CPU steps; on real hardware from the mains.)
build_rom_tod:
        lda CIA1_TODHR
        and #$1F
        ora CIA1_TODMIN
        ora CIA1_TODSEC
        bne brt_done            ; clock already running
        ; set 50/60Hz TOD source per video standard via CIA1 CRA bit7 left as
        ; KERNAL default; just program the start time (sec/min first, hr last)
        lda #$00
        sta CIA1_TOD10
        lda #$00
        sta CIA1_TODSEC
        lda #$00
        sta CIA1_TODMIN
        lda #$12                ; 12 (BCD), AM
        sta CIA1_TODHR
brt_done:
        rts

; ============================================================================
; PAL/NTSC detection - sample the raster counter ($D012 + $D011 bit7) many
; times and keep the maximum 9-bit value. PAL tops out at line 311 ($137),
; NTSC at 262 ($106): both set bit 8, but the low byte ($37 vs $06) tells
; them apart. is_pal = 1 if PAL.
; ============================================================================
detect_video:
        lda #0
        sta zpRasLo
        sta zpRasHi
        sta zpCnt
        lda #$F0                ; 4096 samples ($10000-$F000)
        sta zpCnt+1
dv_loop:
        lda VIC_D011
        and #$80                ; raster bit 8 (0 or $80)
        beq dv_lo               ; current high bit clear
        ; current bit8 set: it's >= any sample whose bit8 is clear
        lda zpRasHi
        bne dv_cmplo            ; both have bit8 -> compare low bytes
        ; new max has bit8, old didn't -> take it
        lda #$80
        sta zpRasHi
        lda VIC_D012
        sta zpRasLo
        jmp dv_next
dv_cmplo:
        lda VIC_D012
        cmp zpRasLo
        bcc dv_next
        sta zpRasLo
        jmp dv_next
dv_lo:
        ; current bit8 clear: only update if old max also bit8-clear & bigger
        lda zpRasHi
        bne dv_next
        lda VIC_D012
        cmp zpRasLo
        bcc dv_next
        sta zpRasLo
dv_next:
        inc zpCnt
        bne dv_loop
        inc zpCnt+1
        lda zpCnt+1
        bne dv_loop
        ; decide: bit8 set AND low byte >= $20 -> PAL (311), else NTSC (262)
        lda #0
        sta is_pal
        lda zpRasHi
        beq dv_done
        lda zpRasLo
        cmp #$20
        bcc dv_done
        lda #1
        sta is_pal
dv_done:
        rts

; ============================================================================
; SID - short blip on app launch. The harness counts SID register writes so a
; test can assert a beep happened.
; ============================================================================
sid_click:
        lda #$00
        sta SID_BASE+4          ; voice 1 control: gate off
        lda #$30
        sta SID_BASE+1          ; freq hi
        lda #$00
        sta SID_BASE+0          ; freq lo
        lda #$0A
        sta SID_BASE+5          ; attack/decay
        lda #$00
        sta SID_BASE+6          ; sustain/release
        lda #$0F
        sta SID_BASE+24         ; master volume
        lda #$11
        sta SID_BASE+4          ; triangle + gate on
        ldx #$40
sc_d1:  ldy #$00
sc_d2:  dey
        bne sc_d2
        dex
        bne sc_d1
        lda #$10
        sta SID_BASE+4          ; gate off (release)
        rts

; ============================================================================
; desktop chrome - menu bar, dithered background, icons
; ============================================================================
draw_desktop:
        ; menu bar (row 0): colour + clear + title
        lda #0
        sta zpCX
        lda #0
        sta zpCY
        lda #SCRCOLS
        sta zpCW
        lda #1
        sta zpCH
        lda #COL_MENU
        sta zpFCol
        jsr color_fill
        lda #0
        sta zpFX
        lda #0
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #8
        sta zpFH
        lda #$00
        sta zpFPat
        jsr fill_rows
        lda #1
        sta zpCol
        lda #0
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_MENU
        sta zpFCol
        lda #<msg_title
        sta zpPtr
        lda #>msg_title
        sta zpPtr+1
        jsr draw_string
        ; desktop background (rows 1..24)
        lda #0
        sta zpCX
        lda #1
        sta zpCY
        lda #SCRCOLS
        sta zpCW
        lda #(SCRROWS-1)
        sta zpCH
        lda theme_desk          ; themeable desktop colour
        sta zpFCol
        jsr color_fill
        lda #0
        sta zpFX
        lda #8
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #192
        sta zpFH
        jsr dither_rect
        lda theme_border        ; themeable VIC border
        sta VIC_D020
        rts

; draw_icons - redraw both desktop icons, highlighting sel_icon.
draw_icons:
        ldx #0
di_loop:
        stx zpSlot              ; preserve slot index (draw_icon clobbers zpTmp*)
        lda icon_x_tab,x
        sta zpFX
        lda icon_y_tab,x
        sta zpIconY
        lda icon_lbl_lo,x
        sta zpPtr
        lda icon_lbl_hi,x
        sta zpPtr+1
        cpx sel_icon
        beq di_sel
        lda #0
        jmp di_go
di_sel:
        lda #1
di_go:
        jsr draw_icon
        ldx zpSlot
        inx
        cpx #NICONS
        bne di_loop
        rts

; draw_icon - zpFX = icon cell column, zpIconY = box top cell row, zpPtr =
; label, A = 1 if selected. Label sits on the box's bottom row (zpIconY+ICONH-1).
draw_icon:
        sta zpSel               ; selected flag (survives fill primitives)
        lda zpFX
        sta zpIconX             ; stable icon column (win_outline clobbers zpFX)
        ; box colour (whole icon) + clear bitmap + outline
        lda zpFX
        sta zpCX
        lda zpIconY
        sta zpCY
        lda #ICONW
        sta zpCW
        lda #ICONH
        sta zpCH
        lda #COL_ICON
        sta zpFCol
        jsr color_fill
        lda zpFX
        sta zpWX                ; reuse win_outline via zpW*
        lda zpIconY
        sta zpWY
        lda #ICONW
        sta zpWW
        lda #ICONH
        sta zpWH
        lda zpFX
        sta zpFX
        lda zpIconY
        jsr cy_to_py
        sta zpFY
        lda #ICONW
        sta zpFW
        lda #(ICONH*8)
        sta zpFH
        lda #$00
        sta zpFPat
        jsr fill_rows
        jsr win_outline
        ; label row = box bottom
        lda zpIconY
        clc
        adc #(ICONH-1)
        sta zpTmp2              ; label cell row (survives below; primitives use zpTmp not zpTmp2? color_fill uses X only)
        ; label band colour (selected = white-on-blue)
        lda zpIconX
        clc
        adc #1
        sta zpCX
        lda zpTmp2
        sta zpCY
        lda #(ICONW-2)
        sta zpCW
        lda #1
        sta zpCH
        lda zpSel
        beq di_lbl_n
        lda #COL_ICON_SEL
        jmp di_lbl_go
di_lbl_n:
        lda #COL_ICON
di_lbl_go:
        sta zpFCol
        jsr color_fill
        ; label text
        lda zpIconX
        clc
        adc #1
        sta zpCol
        lda zpIconY
        clc
        adc #(ICONH-1)
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string
        rts

; icon grid tables (10 slots: 4 cols x 3 rows). Only NICONS are drawn.
icon_x_tab:  dc.b 1,11,21,31, 1,11,21,31, 1,11
icon_y_tab:  dc.b 16,16,16,16, 19,19,19,19, 22,22
; icon index == app_mode for the M3 apps: 4=Dostris 5=Music 6=Pac-Man
; 7=Tracker 8=Paint 9=OutLast
icon_lbl_lo: dc.b <msg_sysinfo,<msg_clock,<msg_files,<msg_theme,<msg_dostris,<msg_music,<msg_pacman,<msg_tracker,<msg_paint,<msg_outlast
icon_lbl_hi: dc.b >msg_sysinfo,>msg_clock,>msg_files,>msg_theme,>msg_dostris,>msg_music,>msg_pacman,>msg_tracker,>msg_paint,>msg_outlast

; ============================================================================
; renderer primitives
; ============================================================================

; clear_bitmap - zero the 8000-byte bitmap ($6000-$7F3F, rounded to $2000).
clear_bitmap:
        lda #<BITMAP
        sta zpDst
        lda #>BITMAP
        sta zpDst+1
        ldx #$20                ; $2000 bytes (8192, covers the 8000)
cb_page:
        ldy #0
        lda #0
cb_byte:
        sta (zpDst),y
        iny
        bne cb_byte
        inc zpDst+1
        dex
        bne cb_page
        rts

; color_fill - set screen-RAM colour zpFCol over the cell rect
; zpCX,zpCY,zpCW,zpCH (cols/rows in cells).
color_fill:
        lda zpCY
        sta zpI
        clc
        adc zpCH
        sta zpJ
cf_row:
        lda zpI
        cmp zpJ
        beq cf_done
        ldy zpI
        lda scr_lo,y
        sta zpScrPtr
        lda scr_hi,y
        sta zpScrPtr+1
        ldx zpCW
        ldy zpCX
        lda zpFCol
cf_col:
        sta (zpScrPtr),y
        iny
        dex
        bne cf_col
        inc zpI
        jmp cf_row
cf_done:
        rts

; fill_rows - write bitmap byte zpFPat to cells zpFX..zpFX+zpFW-1 across pixel
; rows zpFY..zpFY+zpFH-1. Cells are 8 bytes apart; the byte at (col,row) is
; rowbase[row] + col*8.
fill_rows:
        lda zpFY
        sta zpI
        clc
        adc zpFH
        sta zpJ                 ; end pixel row (exclusive)
fr_row:
        lda zpI
        cmp zpJ
        beq fr_done
        ldy zpI
        lda rowbase_lo,y
        sta zpDst
        lda rowbase_hi,y
        sta zpDst+1
        ; + zpFX*8 (16-bit; col up to 39 -> 312)
        lda zpFX
        sta zpTmp
        lda #0
        sta zpTmp2
        asl zpTmp
        rol zpTmp2
        asl zpTmp
        rol zpTmp2
        asl zpTmp
        rol zpTmp2
        clc
        lda zpDst
        adc zpTmp
        sta zpDst
        lda zpDst+1
        adc zpTmp2
        sta zpDst+1
        ldx zpFW
fr_col:
        ldy #0
        lda zpFPat
        sta (zpDst),y
        clc
        lda zpDst
        adc #8
        sta zpDst
        lda zpDst+1
        adc #0
        sta zpDst+1
        dex
        bne fr_col
        inc zpI
        jmp fr_row
fr_done:
        rts

; dither_rect - like fill_rows but checkerboard: $55 on even pixel rows, $AA
; on odd. Inputs zpFX/zpFY/zpFW/zpFH (cols / pixel rows).
dither_rect:
        lda zpFY
        sta zpI
        clc
        adc zpFH
        sta zpJ
dr_row:
        lda zpI
        cmp zpJ
        beq dr_done
        ldy zpI
        lda rowbase_lo,y
        sta zpDst
        lda rowbase_hi,y
        sta zpDst+1
        lda zpFX
        sta zpTmp
        lda #0
        sta zpTmp2
        asl zpTmp
        rol zpTmp2
        asl zpTmp
        rol zpTmp2
        asl zpTmp
        rol zpTmp2
        clc
        lda zpDst
        adc zpTmp
        sta zpDst
        lda zpDst+1
        adc zpTmp2
        sta zpDst+1
        lda zpI
        and #1
        beq dr_even
        lda #$AA
        jmp dr_pat
dr_even:
        lda #$55
dr_pat:
        sta zpFPat
        ldx zpFW
dr_col:
        ldy #0
        lda zpFPat
        sta (zpDst),y
        clc
        lda zpDst
        adc #8
        sta zpDst
        lda zpDst+1
        adc #0
        sta zpDst+1
        dex
        bne dr_col
        inc zpI
        jmp dr_row
dr_done:
        rts

; draw_char - A = char (32-126), cell zpCol/zpRow, EOR zpInv, colour zpFCol.
; Bitmap byte addr = rowbase[zpRow*8] + zpCol*8; the 8 glyph rows are the 8
; consecutive bytes from there. Also stamps the cell colour.
draw_char:
        sty zpCharY             ; preserve caller's Y (draw_name12 uses it as index)
        sec
        sbc #32
        sta zpTmp
        lda #0
        sta zpFontPtr+1
        lda zpTmp
        asl
        rol zpFontPtr+1
        asl
        rol zpFontPtr+1
        asl
        rol zpFontPtr+1         ; A:zpFontPtr+1 = (ch-32)*8
        clc
        adc #<font8
        sta zpFontPtr
        lda zpFontPtr+1
        adc #>font8
        sta zpFontPtr+1
        ; dest = rowbase[zpRow*8] + zpCol*8
        lda zpRow
        asl
        asl
        asl
        tay
        lda rowbase_lo,y
        sta zpDst
        lda rowbase_hi,y
        sta zpDst+1
        lda zpCol
        sta zpTmp
        lda #0
        sta zpTmp2
        asl zpTmp
        rol zpTmp2
        asl zpTmp
        rol zpTmp2
        asl zpTmp
        rol zpTmp2
        clc
        lda zpDst
        adc zpTmp
        sta zpDst
        lda zpDst+1
        adc zpTmp2
        sta zpDst+1
        ; blit 8 glyph rows
        ldy #0
dc_loop:
        lda (zpFontPtr),y
        eor zpInv
        sta (zpDst),y
        iny
        cpy #8
        bne dc_loop
        ; stamp cell colour
        ldy zpRow
        lda scr_lo,y
        sta zpScrPtr
        lda scr_hi,y
        sta zpScrPtr+1
        ldy zpCol
        lda zpFCol
        sta (zpScrPtr),y
        ldy zpCharY             ; restore caller's Y
        rts

; draw_string - zpPtr = NUL-terminated text at cell zpCol/zpRow (zpInv,zpFCol),
; advancing one cell per char.
draw_string:
        lda #0
        sta zpSIdx
ds_loop:
        ldy zpSIdx
        lda (zpPtr),y
        beq ds_done
        jsr draw_char
        inc zpCol
        inc zpSIdx
        jmp ds_loop
ds_done:
        rts

; ============================================================================
; M2 text helpers (used by Files / Notepad) - all stamp cell colour zpFCol.
; ============================================================================

; app_clear - blank the bitmap and paint the whole screen COL_WIN (black-on-
; white), leaving zpFCol = COL_WIN so a full-screen app's draws inherit it.
app_clear:
        jsr clear_bitmap
        lda #0
        sta zpCX
        sta zpCY
        lda #SCRCOLS
        sta zpCW
        lda #SCRROWS
        sta zpCH
        lda #COL_WIN
        sta zpFCol
        jmp color_fill

; draw_name12 - zpFSPtr = 12-byte NUL-padded name; draw all 12 (NUL shown as
; space, columns stay aligned), advancing zpCol by 12.
draw_name12:
        ldy #0
dn12_loop:
        lda (zpFSPtr),y
        bne dn12_ch
        lda #$20
dn12_ch:
        jsr draw_char
        inc zpCol
        iny
        cpy #12
        bne dn12_loop
        rts

; draw_name12_title - like draw_name12 but stops at the first NUL (no padding).
draw_name12_title:
        ldy #0
dnt_loop:
        lda (zpFSPtr),y
        beq dnt_done
        jsr draw_char
        inc zpCol
        iny
        cpy #12
        bne dnt_loop
dnt_done:
        rts

; draw_dec16 - zpFSSize (word) -> decimal at zpCol/zpRow, no leading zeros
; ("0" for zero). Advances zpCol. Preserves X (saved on the stack).
dec16_lo: dc.b <10000,<1000,<100,<10,<1
dec16_hi: dc.b >10000,>1000,>100,>10,>1
draw_dec16:
        txa
        pha
        lda zpFSSize
        sta zpDVlo
        lda zpFSSize+1
        sta zpDVhi
        lda #0
        sta zpDLead
        ldx #0
dd_digit:
        lda #0
        sta zpDDig
dd_sub:
        lda zpDVhi
        cmp dec16_hi,x
        bcc dd_done
        bne dd_doit
        lda zpDVlo
        cmp dec16_lo,x
        bcc dd_done
dd_doit:
        lda zpDVlo
        sec
        sbc dec16_lo,x
        sta zpDVlo
        lda zpDVhi
        sbc dec16_hi,x
        sta zpDVhi
        inc zpDDig
        jmp dd_sub
dd_done:
        lda zpDDig
        bne dd_print
        lda zpDLead
        bne dd_print
        cpx #4
        beq dd_print
        jmp dd_next
dd_print:
        lda #1
        sta zpDLead
        lda zpDDig
        clc
        adc #$30
        jsr draw_char
        inc zpCol
dd_next:
        inx
        cpx #5
        bne dd_digit
        pla
        tax
        rts

; ============================================================================
; strings
; ============================================================================
msg_title:     dc.b "UnoDOS/C64",0
msg_sysinfo:   dc.b "SysInfo",0
msg_clock:     dc.b "Clock",0
msg_mach:      dc.b "Commodore 64",0
msg_cpu_pal:   dc.b "CPU: 6510 @ 0.985MHz",0
msg_cpu_ntsc:  dc.b "CPU: 6510 @ 1.023MHz",0
msg_ram:       dc.b "RAM: 64K",0
msg_vid_pal:   dc.b "Video: PAL VIC-II 6569",0
msg_vid_ntsc:  dc.b "Video: NTSC VIC-II 6567",0
msg_sid:       dc.b "Sound: SID 6581",0
msg_files:     dc.b "Files",0
msg_theme:     dc.b "Theme",0
msg_dostris:   dc.b "Dostris",0
msg_pacman:    dc.b "Pac-Man",0
msg_music:     dc.b "Music",0
msg_tracker:   dc.b "Tracker",0
msg_paint:     dc.b "Paint",0
msg_outlast:   dc.b "OutLast",0
msg_files_title: dc.b "Files",0
msg_notepad_title: dc.b "Notepad: ",0
msg_files_help:  dc.b "RET=Open  D=Del  R=Rescan  STOP=Back",0
msg_files_empty: dc.b "(no files)",0
msg_confirm1:    dc.b "Delete ",0
msg_confirm2:    dc.b "? (Y/N)",0
msg_note_help:   dc.b "  F1=Save  STOP=Back",0
msg_ln:          dc.b "Ln:",0
msg_col:         dc.b "  Col:",0
msg_bytes:       dc.b "  Bytes:",0
msg_full:        dc.b "  FULL",0
msg_saved:       dc.b "  SAVED",0

; ============================================================================
; apps + filesystem (milestone 2)
; ============================================================================
        include "fs.i"
        include "files.i"
        include "notepad.i"
        include "theme.i"
        include "dostris.i"
        include "music.i"

; ============================================================================
; generated tables (VIC bitmap address tables + the shared 8x8 font)
; ============================================================================
        include "build/tables.s"
        include "build/font8.s"

; ============================================================================
; vars - kernel variable block (UDC1 discovery header points here)
; ============================================================================
vars:
sel_icon:   dc.b 0       ; vars+0  desktop icon selection (0=SysInfo,1=Clock)
focus:      dc.b 0       ; vars+1  $FF = desktop, else focused window id
last_key:   dc.b 0       ; vars+2  edge-detection: previous scan's key
last_sec:   dc.b $FF     ; vars+3  last displayed clock second (BCD)
is_pal:     dc.b 0       ; vars+4  1 = PAL detected, 0 = NTSC
app_mode:   dc.b 0       ; vars+5  0=desktop, 1=Files, 2=Notepad
win_state:  dc.b 0,0     ; vars+6  per-window open(1)/closed(0)
zlist:      dc.b 0,0     ; vars+8  z-order, [0]=topmost, $FF=empty
key_matrix: dc.b 0,0,0,0,0,0,0,0   ; vars+10  scan_keyboard: 8-column matrix snapshot

; ---- Files / Notepad app state ----
files_sel:   dc.b 0      ; selected directory index in Files
files_confirm: dc.b 0    ; 0=normal, 1=awaiting delete y/n
note_idx:    dc.b 0      ; directory index Notepad was opened from
note_name:   dc.b 0,0,0,0,0,0,0,0,0,0,0,0   ; (12) file being edited
note_len:    dc.w 0      ; current buffer length (bytes)
note_dirty:  dc.b 0      ; 1 if unsaved changes
note_flash:  dc.b 0      ; status line: 0=help, 1=SAVED, 2=FULL

; ---- Theme app state + live desktop colours (read by draw_desktop) ----
theme_desk:   dc.b COL_DESK    ; desktop dither colour (fg<<4|bg)
theme_border: dc.b BORDERCOL   ; VIC border colour
th_sel:       dc.b 0           ; Theme: cursor
th_cur:       dc.b 0           ; Theme: applied preset

; ---- Dostris state (board itself is DOSBOARD in BSS) ----
dos_piece:   dc.b 0
dos_rot:     dc.b 0
dos_px:      dc.b 0
dos_py:      dc.b 0
dos_next:    dc.b 0
dos_score:   dc.w 0
dos_lines:   dc.b 0
dos_level:   dc.b 0
dos_dctr:    dc.b 0
dos_paused:  dc.b 0
dos_over:    dc.b 0
dos_rng:     dc.b 1           ; LCG seed (must stay nonzero)
dos_justlock: dc.b 0

; ---- Music state ----
mus_idx:     dc.b 0           ; current note index in the tune
mus_ctr:     dc.b 0           ; pass counter for note duration
mus_playing: dc.b 0           ; 1 while the tune is sounding
