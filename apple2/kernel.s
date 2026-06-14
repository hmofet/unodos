; ============================================================================
; UnoDOS/AppleII kernel - milestone 1
;
; Loaded by boot.s's RWTS at $4000. UDM1 header at the top lets the harness
; discover the vars block (magic at $4003, vars pointer word at $4007).
; Hi-res page 1 ($2000-$3FFF) is the framebuffer; row base addresses come
; from the rowlo/rowhi tables (addr = $2000 + (y&7)*$400 + ((y>>3)&7)*$80 +
; (y>>6)*$28 + c, precomputed for c=0, y=0..191 - see HANDOFF.md SS4).
;
; Screen layout (40 byte-cols x 24 char-rows of 8px):
;   row0            menu bar ("UnoDOS" + separator line)
;   rows1-8         SysInfo window (cols1-30)
;   rows9-13        Clock window (cols1-20)
;   rows14-18       empty dithered desktop
;   rows19-23       icon grid: SysInfo icon (col2), Clock icon (col14)
;
; Build: dasm kernel.s -f3 [-DAUTOTEST=1] -obuild/kernel.bin
; ============================================================================

        processor 6502

; ---- zero page (post-boot, all of $00-$EF is free) ----
zpPtr       equ $00   ; (2) generic string/data pointer
zpCol       equ $02   ; draw column, byte 0-39
zpRow       equ $03   ; draw char-row, 0-23
zpInv       equ $04   ; draw_char EOR mask ($00 normal, $7F inverted)
zpTmp       equ $05
zpFontPtr   equ $06   ; (2)
zpRowLoPtr  equ $08   ; (2)
zpRowHiPtr  equ $0A   ; (2)
zpDst       equ $0C   ; (2)
zpSIdx      equ $0E
zpFX        equ $0F   ; fill_rows/frame_rect/dither_rect params
zpFY        equ $10
zpFW        equ $11
zpFH        equ $12
zpFPat      equ $13
zpI         equ $14
zpJ         equ $15
zpFX0       equ $16   ; frame_rect: saved original params
zpFY0       equ $17
zpFW0       equ $18
zpFH0       equ $19
zpWX        equ $1A   ; draw_win params
zpWY        equ $1B
zpWW        equ $1C
zpWH        equ $1D
zpWF        equ $1E   ; focused flag (0/1)
zpSecs      equ $1F   ; (2) clock_format scratch
zpHH        equ $21
zpMM        equ $22
zpSS        equ $23
clkbuf      equ $24   ; (9) "HH:MM:SS",0
zpTens      equ $2D

; ---- zero page ($3B-$4D: Files/Notepad apps; fs.i owns $2E-$3A) ----
zpDVlo   equ $3B   ; (2) draw_dec16 working value
zpDVhi   equ $3C
zpDDig   equ $3D   ; draw_dec16 current digit
zpDLead  equ $3E   ; draw_dec16 leading-zero-suppress flag
zpNPtr   equ $3F   ; (2) notepad_draw: pointer into NOTEBUF
zpNRem   equ $41   ; (2) notepad_draw: bytes remaining in scan
zpNTot   equ $43   ; (2) notepad_draw pass1: total line count (1-based)
zpNFCol  equ $45   ; (2) notepad_draw pass1: final (cursor) column, 0-based
zpNSkip  equ $47   ; (2) notepad_draw: lines to skip (tail scroll)
zpNVis   equ $49   ; notepad_draw pass2: current line visible flag
zpNLine  equ $4A   ; (2) notepad_draw pass2: current line number (0-based)
zpNCol   equ $4C   ; notepad_draw pass2: current screen column (capped 40)
zpNRow   equ $4D   ; notepad_draw pass2: current screen row

zpBeepHalf equ $4E ; beep: half-period delay constant
zpBeepN    equ $4F ; beep: remaining $C030 toggles

; ---- zero page ($76-$78: Music/Tracker note_play, see music.i) ----
zpNoteHalf equ $76 ; note_play: half-period inner-delay constant
zpNoteDur  equ $77 ; (2) note_play: remaining $C030 toggles (16-bit)

; ---- zero page ($79-$7D: Paint, see paint.i) ----
zpPB     equ $79   ; (2) PAINTBUF cell pointer
zpPBT    equ $7B   ; (2) pt_render_cell/pt_ptr col/row scratch
zpPBInk  equ $7D   ; pt_apply: resolved ink pattern

; ---- zero page ($7E-$80: OutLast band-centre math, see outlast.i) ----
zpTmp2   equ $7E
zpTmp3   equ $7F
zpTmp4   equ $80

; ---- zero page ($50-$60: Dostris, see dostris.i) ----
zpDosMask  equ $50 ; (2) current/test piece+rotation 4x4 bitmask, lo/hi
zpDosBit   equ $52 ; mask bit index 0-15 during cell iteration
zpDosBX    equ $53 ; board column for the cell at zpDosBit
zpDosBY    equ $54 ; board row for the cell at zpDosBit
zpDosTX    equ $55 ; piece X under test (move/rotate/spawn checks)
zpDosTY    equ $56 ; piece Y under test
zpDosIdx   equ $58 ; board byte index (zpDosBY*10+zpDosBX)
zpDosT1    equ $59 ; mul10/shift scratch
zpDosT2    equ $5A ; mul10/shift scratch
zpDosFull  equ $5B ; clear_lines: rows cleared this lock (0-4)
zpDosRow   equ $5C ; clear_lines: row loop index
zpDosSrc   equ $5D ; (2) shift_down: source row pointer
zpDosDst   equ $5F ; (2) shift_down/newgame: dest row / clear pointer

; ---- zero page ($61-$75: Pac-Man, see pacman.i) ----
zpPMCol    equ $61 ; working maze column (0-12)
zpPMRow    equ $62 ; working maze row (0-12)
zpPMIdx    equ $63 ; maze index = row*13+col
zpPMNCol   equ $64 ; pm_step_walkable: candidate column
zpPMNRow   equ $65 ; pm_step_walkable: candidate row
zpPMT1     equ $66 ; mul13 scratch (pm_calc_idx/pm_step_walkable)
zpPMT2     equ $67 ; abs-value scratch (pm_try_dir/pm_manhattan_to_pac)
zpPMBestD  equ $68 ; pm_steer: best distance found so far
zpPMBestDir equ $69 ; pm_steer: best direction found so far ($FF=none)
zpPMTCol   equ $6A ; pm_steer: target column
zpPMTRow   equ $6B ; pm_steer: target row
zpPMDir    equ $6C ; pm_steer: direction loop counter 0-3
zpPMGI     equ $6D ; pm_steer: ghost index (0/1), saved across X-clobbering calls
zpPMDir2   equ $6E ; pm_try_dir: direction under test, saved across pm_step_walkable
zpPMRevDir equ $6F ; pm_steer: reverse of the ghost's current direction
zpPMOldPC  equ $70 ; pacman_tick: pac column before this tick's move
zpPMOldPR  equ $71 ; pacman_tick: pac row before this tick's move
zpPMOldGC  equ $72 ; (2) pacman_tick: ghost columns before this tick's move
zpPMOldGR  equ $74 ; (2) pacman_tick: ghost rows before this tick's move

; ---- screen layout constants ----
SCRCOLS         equ 40
SCRROWS         equ 24

SI_X            equ 1     ; SysInfo window
SI_Y            equ 8
SI_W            equ 30
SI_H            equ 64
SI_CONTENT_X    equ (SI_X+1)
SI_CONTENT_Y    equ (SI_Y+8)
SI_CONTENT_W    equ (SI_W-2)
SI_CONTENT_H    equ (SI_H-9)

CK_X            equ 1     ; Clock window
CK_Y            equ 72
CK_W            equ 20
CK_H            equ 40
CK_CONTENT_X    equ (CK_X+1)
CK_CONTENT_Y    equ (CK_Y+8)
CK_CONTENT_W    equ (CK_W-2)
CK_CONTENT_H    equ (CK_H-9)

; icon grid - 2 rows x 4 cols (8 slots; M3 adds Theme/Dostris/Pac-Man/
; Music/Paint to M1/M2's SysInfo/Clock/Files). Each slot is ICONW
; byte-cols x ICONH px; row1 frame top = ICONROW1_Y, row2 = ICONROW2_Y;
; the label sits on the middle char-row of each slot.
ICONW           equ 9
ICONH           equ 24
ICONROW0_Y      equ 120
ICONROW1_Y      equ 144
ICONROW2_Y      equ 168
LABELROW0       equ 16
LABELROW1       equ 19
LABELROW2       equ 22
ICONCOL0_X      equ 1
ICONCOL1_X      equ 11
ICONCOL2_X      equ 21
ICONCOL3_X      equ 31
NICONS          equ 10    ; populated icon slots (M3: 3 rows x 4 cols)

; Files/Notepad - full-screen apps (app_mode != 0): row0 = title + separator
; (as draw_desktop), rows1-22 = content, row23 = status/help line.
APP_CONTENT_Y   equ 8     ; pixel row of content row1
APP_CONTENT_H   equ 176   ; 22 rows * 8px
APP_VIEW_ROWS   equ 22
APP_STATUS_Y    equ 184   ; pixel row of row23
NOTE_MAXLEN     equ 2048  ; notepad text buffer cap

; TICKS_PER_SEC: main-loop passes per soft-clock second. Calibrated with
; harness.py's TICK_INSTRS so `wait N` advances clock_secs predictably
; (HANDOFF SS6b) - see harness.py for the paired constant and the math.
TICKS_PER_SEC   equ 1000

; KBSS: kernel buffers (RWTS decode tables, FS/file buffers) live above the
; assembled image - mkdsk.py asserts the image fits below this. Raised from
; $6000 to $9000 for M3 (5 new apps): gives code $4000-$8FFF (20KB) and
; leaves $9D00-$BFFF free (after NOTEBUF ends at KBSS+$CFF) for new M3 BSS
; tables (Dostris board, Pac-Man maze, Music/Tracker data, Paint buffer,
; pat_tab).
KBSS            equ $9000

        org $4000

; ---------------------------------------------------------------- header
start:
        jmp start2
        dc.b "UDM1"
        dc.w vars

; ---------------------------------------------------------------- entry
start2:
        bit $C050               ; graphics mode (TXTCLR)
        bit $C057               ; hi-res (HIRES)
        bit $C054               ; page 1 (LOWSCR)
        bit $C052               ; full screen (MIXCLR)

        lda #$FF
        sta zlist
        sta zlist+1
        sta focus

        jsr rwts_init
        jsr fs_init

        jsr draw_desktop

        ifconst AUTOTEST
        jsr rwts_selftest
        jsr fs_selftest
        lda #0
        jsr open_or_raise       ; open SysInfo
        lda #1
        jsr open_or_raise       ; open Clock (becomes topmost/focused)
        lda #1
        sta sel_icon            ; Clock icon initially selected
        endif

        jsr draw_icons
        jmp mainloop

; ============================================================================
; main loop - poll keyboard, else advance the soft clock (no ISRs, single
; cooperative context per HANDOFF SS8)
; ============================================================================
mainloop:
        lda $C000
        bpl ml_tick
        bit $C010               ; clear keyboard strobe
        jsr handle_key
ml_tick:
        inc frame_ctr
        bne ml_check
        inc frame_ctr+1
ml_check:
        lda frame_ctr
        cmp #<TICKS_PER_SEC
        bne mainloop
        lda frame_ctr+1
        cmp #>TICKS_PER_SEC
        bne mainloop
        lda #0
        sta frame_ctr
        sta frame_ctr+1
        inc clock_secs
        bne ml_dos
        inc clock_secs+1
ml_dos:
        lda app_mode            ; Dostris gravity: one soft-drop step per
        cmp #4                  ; soft-clock second (dostris_tick handles
        bne ml_pac              ; pause/game-over and the level-based rate)
        jsr dostris_tick
        jmp mainloop
ml_pac:
        lda app_mode            ; Pac-Man: one actor step per soft-clock
        cmp #5                  ; second (pacman_tick handles game-over)
        bne ml_ol
        jsr pacman_tick
        jmp mainloop
ml_ol:
        lda app_mode            ; OutLast: scroll the road one step per
        cmp #9                  ; soft-clock second
        bne ml_cdraw
        jsr ol_tick
        jmp mainloop
ml_cdraw:
        lda app_mode            ; Files/Notepad/Theme cover the desktop -
        bne mainloop            ; don't let the clock redraw clobber them
        lda win_state+1
        beq mainloop
        jsr draw_clock_content
        jmp mainloop

; handle_key - A = key code from $C000 (bit7 set). When a full-screen app is
; active (app_mode != 0: 1=Files, 2=Notepad, 3=Theme, 4=Dostris), keys are
; routed there entirely. Otherwise: ESC closes the topmost window; if the
; desktop has focus, left/right move sel_icon (NICONS icons, wrapping) and
; Return launches/raises the selected app (Files, Theme and Dostris are
; full-screen apps opened directly; SysInfo/Clock open as windows); if a
; window has focus, other keys are routed to it (no-op for M1 apps).
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
        cmp #4
        beq hk_dostris
        cmp #5
        beq hk_pacman
        cmp #6
        beq hk_music
        cmp #7
        beq hk_tracker
        cmp #8
        beq hk_paint
        jmp outlast_key
hk_files:
        jmp files_key
hk_notepad:
        jmp notepad_key
hk_theme:
        jmp theme_key
hk_dostris:
        jmp dostris_key
hk_pacman:
        jmp pacman_key
hk_music:
        jmp music_key
hk_tracker:
        jmp tracker_key
hk_paint:
        jmp paint_key
hk_desktop:
        lda zpTmp
        cmp #$9B                ; ESC
        bne hk_notesc
        jsr close_topmost
        rts
hk_notesc:
        lda focus
        cmp #$FF
        beq hk_deskfocus        ; desktop focus -> handle icon nav
        rts                     ; window focused: no app key handling at M1
hk_deskfocus:
        lda zpTmp
        cmp #$8D                ; Return
        beq hk_return
        cmp #$95                ; right
        beq hk_right
        cmp #$88                ; left (== backspace on a II+)
        beq hk_left
        jmp hk_done
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
        rts
hk_return:
        lda sel_icon
        cmp #2
        bne hk_ret_3
        jmp files_open
hk_ret_3:
        cmp #3
        bne hk_ret_4
        jmp theme_open
hk_ret_4:
        cmp #4
        bne hk_ret_5
        jmp dostris_open
hk_ret_5:
        cmp #5
        bne hk_ret_6
        jmp pacman_open
hk_ret_6:
        cmp #6
        bne hk_ret_7
        jmp music_open
hk_ret_7:
        cmp #7
        bne hk_ret_8
        jmp tracker_open
hk_ret_8:
        cmp #8
        bne hk_ret_9
        jmp paint_open
hk_ret_9:
        cmp #9
        bne hk_ret_win
        jmp outlast_open
hk_ret_win:
        lda sel_icon
        jsr open_or_raise
        lda sel_icon
        sta focus
hk_done:
        rts

; ============================================================================
; window manager - 2 fixed-position windows (SysInfo=0, Clock=1), z-order
; in zlist (zlist[0] = topmost/focused, $FF = empty slot)
; ============================================================================

; open_or_raise - A = window id. Opens it (front of zlist) if closed, else
; raises it to the front if not already topmost.
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
        sta top_win
        inc zcount
        jmp oor_redraw
oor_raise:
        lda zlist
        cmp zpTmp
        beq oor_done
        sta zpJ
        lda zpTmp
        sta zlist
        sta top_win
        lda zpJ
        sta zlist+1
oor_redraw:
        jsr draw_sysinfo_win
        jsr draw_clock_win
oor_done:
        rts

; close_topmost - close zlist[0] (ESC handler). New top becomes focused;
; if no window remains open, focus returns to the desktop.
close_topmost:
        lda zlist
        cmp #$FF
        beq ct_done
        sta zpTmp
        tay
        lda #0
        sta win_state,y
        lda zlist+1
        sta zlist
        sta top_win
        lda #$FF
        sta zlist+1
        dec zcount
        jsr draw_sysinfo_win
        jsr draw_clock_win
        lda zlist
        cmp #$FF
        bne ct_focus
        sta focus               ; A == $FF here -> desktop focus
        rts
ct_focus:
        sta focus
ct_done:
        rts

; ============================================================================
; window drawing
; ============================================================================

; draw_sysinfo_win - redraw window 0: dithered desktop if closed, else
; frame+title (focused iff zlist[0]==0) + content.
draw_sysinfo_win:
        lda win_state
        bne dsiw_open
        lda #SI_X
        sta zpFX
        lda #SI_Y
        sta zpFY
        lda #SI_W
        sta zpFW
        lda #SI_H
        sta zpFH
        jsr dither_rect
        rts
dsiw_open:
        lda zlist
        cmp #0
        beq dsiw_f1
        lda #0
        jmp dsiw_go
dsiw_f1:
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

; draw_clock_win - same as above for window 1.
draw_clock_win:
        lda win_state+1
        bne dckw_open
        lda #CK_X
        sta zpFX
        lda #CK_Y
        sta zpFY
        lda #CK_W
        sta zpFW
        lda #CK_H
        sta zpFH
        jsr dither_rect
        rts
dckw_open:
        lda zlist
        cmp #1
        beq dckw_f1
        lda #0
        jmp dckw_go
dckw_f1:
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

; draw_win - frame + title bar for zpWX/zpWY/zpWW/zpWH, title zpPtr,
; focused zpWF (0/1). Focused = inverted (bright) title bar with dark text;
; unfocused = normal title text plus a separator line under it.
draw_win:
        lda zpWX
        sta zpFX
        lda zpWY
        sta zpFY
        lda zpWW
        sta zpFW
        lda zpWH
        sta zpFH
        jsr frame_rect

        lda zpWX
        sta zpFX
        lda zpWY
        sta zpFY
        lda zpWW
        sta zpFW
        lda #8
        sta zpFH
        lda zpWF
        beq dw_unfoc
        lda pat_tab+2
        jmp dw_pat
dw_unfoc:
        lda pat_tab+3
dw_pat:
        sta zpFPat
        jsr fill_rows

        lda zpWX
        clc
        adc #1
        sta zpCol
        lda zpWY
        lsr
        lsr
        lsr
        sta zpRow
        lda zpWF
        beq dw_inv0
        lda #$7F
        jmp dw_inv
dw_inv0:
        lda #$00
dw_inv:
        sta zpInv
        jsr draw_string

        lda zpWF
        bne dw_done
        lda zpWX
        sta zpFX
        lda zpWY
        clc
        adc #7
        sta zpFY
        lda zpWW
        sta zpFW
        lda #1
        sta zpFH
        lda pat_tab+2
        sta zpFPat
        jsr fill_rows
dw_done:
        rts

; ============================================================================
; SysInfo app
; ============================================================================

; draw_sysinfo_content - clear the content area and draw machine/CPU/RAM
; lines. Machine name comes from sysinfo_detect ($FBB3/$FBC0, SS6c).
draw_sysinfo_content:
        lda #SI_CONTENT_X
        sta zpFX
        lda #SI_CONTENT_Y
        sta zpFY
        lda #SI_CONTENT_W
        sta zpFW
        lda #SI_CONTENT_H
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows

        jsr sysinfo_detect
        lda #SI_CONTENT_X
        sta zpCol
        lda #2
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string

        lda #<msg_cpu
        sta zpPtr
        lda #>msg_cpu
        sta zpPtr+1
        lda #SI_CONTENT_X
        sta zpCol
        lda #3
        sta zpRow
        jsr draw_string

        lda #<msg_ram
        sta zpPtr
        lda #>msg_ram
        sta zpPtr+1
        lda #SI_CONTENT_X
        sta zpCol
        lda #4
        sta zpRow
        jsr draw_string
        rts

; sysinfo_detect - set zpPtr to a machine-name string per $FBB3/$FBC0.
; Real values (HANDOFF SS6c): $FBB3 = $38 (II), $EA (II+), $06 (IIe
; family, then $FBC0 = $EA IIe / $E0 enhanced IIe / $00 IIc). The harness
; seeds $FBB3=$FBC0=$EA -> "Apple II+" (the M1 target floor).
sysinfo_detect:
        lda $FBB3
        cmp #$38
        beq sd_ii
        cmp #$EA
        beq sd_iiplus
        cmp #$06
        beq sd_iiefam
        lda #<msg_mach_unk
        sta zpPtr
        lda #>msg_mach_unk
        sta zpPtr+1
        rts
sd_ii:
        lda #<msg_mach_ii
        sta zpPtr
        lda #>msg_mach_ii
        sta zpPtr+1
        rts
sd_iiplus:
        lda #<msg_mach_iiplus
        sta zpPtr
        lda #>msg_mach_iiplus
        sta zpPtr+1
        rts
sd_iiefam:
        lda $FBC0
        cmp #$EA
        beq sd_iie
        cmp #$E0
        beq sd_iie_enh
        cmp #$00
        beq sd_iic
        lda #<msg_mach_iiefam
        sta zpPtr
        lda #>msg_mach_iiefam
        sta zpPtr+1
        rts
sd_iie:
        lda #<msg_mach_iie
        sta zpPtr
        lda #>msg_mach_iie
        sta zpPtr+1
        rts
sd_iie_enh:
        lda #<msg_mach_iie_enh
        sta zpPtr
        lda #>msg_mach_iie_enh
        sta zpPtr+1
        rts
sd_iic:
        lda #<msg_mach_iic
        sta zpPtr
        lda #>msg_mach_iic
        sta zpPtr+1
        rts

; ============================================================================
; Clock app - calibrated soft tick (no RTC, no timer IRQ; mainloop bumps
; clock_secs every TICKS_PER_SEC passes)
; ============================================================================

; draw_clock_content - clear the content area and draw HH:MM:SS.
draw_clock_content:
        lda #CK_CONTENT_X
        sta zpFX
        lda #CK_CONTENT_Y
        sta zpFY
        lda #CK_CONTENT_W
        sta zpFW
        lda #CK_CONTENT_H
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows

        jsr clock_format
        lda #<clkbuf
        sta zpPtr
        lda #>clkbuf
        sta zpPtr+1
        lda #CK_CONTENT_X
        sta zpCol
        lda #10
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string
        rts

; clock_format - convert the 16-bit clock_secs into "HH:MM:SS",0 in clkbuf
; via repeated subtraction (no hardware divide on 6502).
clock_format:
        lda clock_secs
        sta zpSecs
        lda clock_secs+1
        sta zpSecs+1
        lda #0
        sta zpHH
cf_h:
        lda zpSecs+1
        cmp #>3600
        bcc cf_hdone
        bne cf_hsub
        lda zpSecs
        cmp #<3600
        bcc cf_hdone
cf_hsub:
        lda zpSecs
        sec
        sbc #<3600
        sta zpSecs
        lda zpSecs+1
        sbc #>3600
        sta zpSecs+1
        inc zpHH
        jmp cf_h
cf_hdone:
        lda #0
        sta zpMM
cf_m:
        lda zpSecs+1
        bne cf_msub
        lda zpSecs
        cmp #60
        bcc cf_mdone
cf_msub:
        lda zpSecs
        sec
        sbc #60
        sta zpSecs
        lda zpSecs+1
        sbc #0
        sta zpSecs+1
        inc zpMM
        jmp cf_m
cf_mdone:
        lda zpSecs
        sta zpSS

        lda zpHH
        ldy #0
        jsr fmt2
        lda #$3A                    ; ':'
        sta clkbuf+2
        lda zpMM
        ldy #3
        jsr fmt2
        lda #$3A                    ; ':'
        sta clkbuf+5
        lda zpSS
        ldy #6
        jsr fmt2
        lda #0
        sta clkbuf+8
        rts

; fmt2 - A = value 0..99, Y = offset; writes 2 ASCII digits to
; clkbuf+Y, clkbuf+Y+1 via repeated subtraction of 10.
fmt2:
        sta zpTmp
        sty zpJ
        lda #0
        sta zpTens
fmt2_t:
        lda zpTmp
        cmp #10
        bcc fmt2_done
        sec
        sbc #10
        sta zpTmp
        inc zpTens
        jmp fmt2_t
fmt2_done:
        ldy zpJ
        lda zpTens
        clc
        adc #$30
        sta clkbuf,y
        lda zpTmp
        clc
        adc #$30
        sta clkbuf+1,y
        rts

; ============================================================================
; speaker - blocking tone generator (HANDOFF-M2 SS5)
; ============================================================================

; beep - A = half-period delay constant, X = number of $C030 toggles (2 per
; full cycle). Blocking: each toggle is followed by a busy-wait of ~(5*A+1)
; cycles, so frequency ~= 1MHz / (2*(5*A+~10)) and duration ~= toggles *
; that half-period. Clobbers A/X/Y/zpBeepHalf/zpBeepN.
beep:
        sta zpBeepHalf
        stx zpBeepN
bp_toggle:
        lda $C030               ; toggle the speaker
        ldx zpBeepHalf
bp_delay:
        dex
        bne bp_delay
        dec zpBeepN
        bne bp_toggle
        rts

; beep_click - ~1kHz, ~100ms: app launch, save complete, FULL/error.
BEEP_CLICK_HALF  equ 99
BEEP_CLICK_COUNT equ 200

beep_click:
        lda #BEEP_CLICK_HALF
        ldx #BEEP_CLICK_COUNT
        jmp beep

; ============================================================================
; desktop chrome - menu bar, dithered background, icon grid
; ============================================================================

; draw_desktop - menu bar title + separator line, then dither the whole
; desktop area (rows1-23). Windows/icons are drawn on top afterward.
draw_desktop:
        lda #<msg_title
        sta zpPtr
        lda #>msg_title
        sta zpPtr+1
        lda #1
        sta zpCol
        lda #0
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string

        lda #0
        sta zpFX
        lda #7
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #1
        sta zpFH
        lda #$7F
        sta zpFPat
        jsr fill_rows

        lda #0
        sta zpFX
        lda #8
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #184
        sta zpFH
        jsr dither_rect
        rts

; draw_icons - redraw all desktop icons, highlighting sel_icon. Table-driven
; over NICONS slots (icon_x_tab/icon_y_tab/icon_lr_tab/icon_lbl_lo/hi); X =
; slot index is passed through to draw_icon, which indexes icon_y_tab and
; icon_lr_tab itself (frame_rect/fill_rows/draw_string don't touch X).
draw_icons:
        ldx #0
di_loop:
        lda icon_x_tab,x
        sta zpFX
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
        inx
        cpx #NICONS
        bne di_loop
        rts

; draw_icon - X = icon slot index, zpFX = box x, zpPtr = label, A = 1 if
; selected (inverted label band) else 0. Icon y / label row come from
; icon_y_tab/icon_lr_tab,X. Label fill/invert use the themeable accent
; (pat_tab+2 selected / pat_tab+3 unselected - see theme.i).
draw_icon:
        sta zpTmp
        lda icon_y_tab,x
        sta zpFY
        lda #ICONW
        sta zpFW
        lda #ICONH
        sta zpFH
        jsr frame_rect

        lda zpFX
        clc
        adc #1
        sta zpFX
        lda icon_y_tab,x
        clc
        adc #8
        sta zpFY
        lda #(ICONW-2)
        sta zpFW
        lda #8
        sta zpFH
        lda zpTmp
        beq di_pat0
        lda pat_tab+2
        jmp di_pat
di_pat0:
        lda pat_tab+3
di_pat:
        sta zpFPat
        jsr fill_rows

        lda zpFX
        sta zpCol
        lda icon_lr_tab,x
        sta zpRow
        lda zpTmp
        beq di_inv0
        lda #$7F
        jmp di_inv
di_inv0:
        lda #$00
di_inv:
        sta zpInv
        jsr draw_string
        rts

; icon slot tables (NICONS entries; M3 grows these as Dostris/Pac-Man/
; Music/Paint icons fill row 2's remaining slots).
; 3 rows x 4 cols: SysInfo/Clock/Files/Theme (row0), Dostris/Pac-Man/Music/
; Tracker (row1), Paint (row2.col0).
icon_x_tab:  dc.b ICONCOL0_X,ICONCOL1_X,ICONCOL2_X,ICONCOL3_X,ICONCOL0_X,ICONCOL1_X,ICONCOL2_X,ICONCOL3_X,ICONCOL0_X,ICONCOL1_X
icon_y_tab:  dc.b ICONROW0_Y,ICONROW0_Y,ICONROW0_Y,ICONROW0_Y,ICONROW1_Y,ICONROW1_Y,ICONROW1_Y,ICONROW1_Y,ICONROW2_Y,ICONROW2_Y
icon_lr_tab: dc.b LABELROW0,LABELROW0,LABELROW0,LABELROW0,LABELROW1,LABELROW1,LABELROW1,LABELROW1,LABELROW2,LABELROW2
icon_lbl_lo: dc.b <msg_sysinfo,<msg_clock,<msg_files,<msg_theme,<msg_dostris,<msg_pacman,<msg_music,<msg_tracker,<msg_paint,<msg_outlast
icon_lbl_hi: dc.b >msg_sysinfo,>msg_clock,>msg_files,>msg_theme,>msg_dostris,>msg_pacman,>msg_music,>msg_tracker,>msg_paint,>msg_outlast

; ============================================================================
; renderer primitives (all byte-column / 7-px aligned)
; ============================================================================

; hgr_clear - fill $2000-$3FFF with $00
hgr_clear:
        lda #$00
        sta zpDst
        lda #$20
        sta zpDst+1
hc_page:
        ldy #0
hc_byte:
        lda #$00
        sta (zpDst),y
        iny
        bne hc_byte
        inc zpDst+1
        lda zpDst+1
        cmp #$40
        bne hc_page
        rts

; fill_rows - fill zpFH pixel rows starting at pixel row zpFY, byte columns
; zpFX..zpFX+zpFW-1, with pattern byte zpFPat.
fill_rows:
        lda zpFY
        sta zpI
        clc
        adc zpFH
        sta zpJ                 ; end row, exclusive
fr_row:
        lda zpI
        cmp zpJ
        beq fr_done
        ldy zpI
        lda rowlo,y
        clc
        adc zpFX
        sta zpDst
        lda rowhi,y
        adc #0
        sta zpDst+1
        ldy #0
        lda zpFPat
fr_col:
        sta (zpDst),y
        iny
        cpy zpFW
        bne fr_col
        inc zpI
        jmp fr_row
fr_done:
        rts

; frame_rect - draw a 1px border around zpFX,zpFY,zpFW,zpFH (pixel rows /
; byte columns) using the themeable accent pattern (pat_tab+2).
frame_rect:
        lda zpFX
        sta zpFX0
        lda zpFY
        sta zpFY0
        lda zpFW
        sta zpFW0
        lda zpFH
        sta zpFH0
        lda pat_tab+2
        sta zpFPat

        lda zpFX0
        sta zpFX
        lda zpFY0
        sta zpFY
        lda zpFW0
        sta zpFW
        lda #1
        sta zpFH
        jsr fill_rows

        lda zpFY0
        clc
        adc zpFH0
        sec
        sbc #1
        sta zpFY
        jsr fill_rows

        lda zpFX0
        sta zpFX
        lda zpFY0
        sta zpFY
        lda #1
        sta zpFW
        lda zpFH0
        sta zpFH
        jsr fill_rows

        lda zpFX0
        clc
        adc zpFW0
        sec
        sbc #1
        sta zpFX
        jsr fill_rows

        ; restore params (caller may use zpFX/zpFY/zpFW/zpFH afterward)
        lda zpFX0
        sta zpFX
        lda zpFY0
        sta zpFY
        lda zpFW0
        sta zpFW
        lda zpFH0
        sta zpFH
        rts

; dither_rect - fill zpFX,zpFY,zpFW,zpFH with the themeable desktop dither
; (pat_tab+0 on even pixel rows, pat_tab+1 on odd - see theme.i pat_tab).
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
        lda rowlo,y
        clc
        adc zpFX
        sta zpDst
        lda rowhi,y
        adc #0
        sta zpDst+1
        lda zpI
        and #1
        beq dr_even
        lda pat_tab+1
        jmp dr_go
dr_even:
        lda pat_tab+0
dr_go:
        ldy #0
dr_col:
        sta (zpDst),y
        iny
        cpy zpFW
        bne dr_col
        inc zpI
        jmp dr_row
dr_done:
        rts

; draw_char - A=char code (32-126), position zpCol/zpRow, EOR mask zpInv.
; clobbers zpTmp, zpFontPtr, zpRowLoPtr/Hi, zpDst, A; preserves X/Y (so
; callers may use X/Y as their own per-character loop counters).
draw_char:
        sec
        sbc #32
        sta zpTmp
        lsr
        lsr
        lsr
        lsr
        lsr
        clc
        adc #>font7
        sta zpFontPtr+1
        lda zpTmp
        asl
        asl
        asl
        clc
        adc #<font7
        sta zpFontPtr
        bcc dc_nc
        inc zpFontPtr+1
dc_nc:
        lda zpRow
        asl
        asl
        asl
        sta zpTmp               ; rowbase = zpRow*8
        clc
        lda #<rowlo
        adc zpTmp
        sta zpRowLoPtr
        lda #>rowlo
        adc #0
        sta zpRowLoPtr+1
        clc
        lda #<rowhi
        adc zpTmp
        sta zpRowHiPtr
        lda #>rowhi
        adc #0
        sta zpRowHiPtr+1

        tya
        pha
        txa
        pha
        ldx #0
dc_loop:
        txa
        tay
        lda (zpRowLoPtr),y
        clc
        adc zpCol
        sta zpDst
        lda (zpRowHiPtr),y
        adc #0
        sta zpDst+1
        lda (zpFontPtr),y
        eor zpInv
        ldy #0
        sta (zpDst),y
        inx
        cpx #8
        bne dc_loop
        pla
        tax
        pla
        tay
        rts

; draw_string - zpPtr = null-terminated string, position zpCol/zpRow,
; mode zpInv. Advances zpCol by one byte-column per character.
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

; draw_name12 - zpFSPtr = pointer to a 12-byte NUL-padded name field; draws
; all 12 bytes at zpCol/zpRow (zpInv mode), NUL shown as space (so columns
; stay aligned in a listing). Advances zpCol by 12.
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

; draw_name12_title - zpFSPtr = pointer to a 12-byte NUL-padded name field;
; draws up to 12 chars, stopping at the first NUL (no trailing padding).
; Advances zpCol accordingly.
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

; draw_dec16 - zpFSSize (2-byte LE, 0..65535) -> decimal digits at
; zpCol/zpRow (zpInv mode), no leading zeros ("0" for zero). Advances
; zpCol by the number of digits printed. Clobbers zpFSSize/zpDV*/zpDDig/
; zpDLead/A; preserves X/Y (X is used internally as the digit-place index,
; saved/restored via the stack).
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
dd16_digit:
        lda #0
        sta zpDDig
dd16_sub:
        lda zpDVhi
        cmp dec16_hi,x
        bcc dd16_done
        bne dd16_doit
        lda zpDVlo
        cmp dec16_lo,x
        bcc dd16_done
dd16_doit:
        lda zpDVlo
        sec
        sbc dec16_lo,x
        sta zpDVlo
        lda zpDVhi
        sbc dec16_hi,x
        sta zpDVhi
        inc zpDDig
        jmp dd16_sub
dd16_done:
        lda zpDDig
        bne dd16_print
        lda zpDLead
        bne dd16_print
        cpx #4
        beq dd16_print
        jmp dd16_next
dd16_print:
        lda #1
        sta zpDLead
        lda zpDDig
        clc
        adc #$30
        jsr draw_char
        inc zpCol
dd16_next:
        inx
        cpx #5
        bne dd16_digit
        pla
        tax
        rts

        ifconst AUTOTEST
; ---------------------------------------------------------------- rwts_selftest
; rwts_selftest - M2 SS1 proof gate: read track RWTS_TTRACK logical sector
; RWTS_TSEC into SECBUF, XOR every byte with $FF, write the result back,
; re-read into SECBUF2, and compare. Draws "RWTS PASS"/"RWTS FAIL" on the
; title bar (row 0, right of "UnoDOS"). RWTS_TTRACK/TSEC now sit inside the
; kernel's own image (the kernel has grown past track 1 since M1, when track
; 2 was scratch) - the XORed pattern is written back to SECBUF and restored
; to disk afterward (pass or fail) so a --writeback image isn't corrupted.
RWTS_TTRACK equ 2
RWTS_TSEC   equ 3

rwts_selftest:
        lda #RWTS_TTRACK
        sta zpTrkWant
        lda #<SECBUF
        sta zpBuf
        lda #>SECBUF
        sta zpBuf+1
        lda #RWTS_TSEC
        jsr rwts_read

        ldy #0
rst_xor:
        lda SECBUF,y
        eor #$FF
        sta SECBUF,y
        iny
        bne rst_xor

        lda #RWTS_TTRACK
        sta zpTrkWant
        lda #<SECBUF
        sta zpBuf
        lda #>SECBUF
        sta zpBuf+1
        lda #RWTS_TSEC
        jsr rwts_write

        lda #RWTS_TTRACK
        sta zpTrkWant
        lda #<SECBUF2
        sta zpBuf
        lda #>SECBUF2
        sta zpBuf+1
        lda #RWTS_TSEC
        jsr rwts_read

        ldy #0
rst_cmp:
        lda SECBUF,y
        cmp SECBUF2,y
        bne rst_fail
        iny
        bne rst_cmp

        lda #<msg_rwts_pass
        sta zpPtr
        lda #>msg_rwts_pass
        sta zpPtr+1
        jmp rst_restore
rst_fail:
        lda #<msg_rwts_fail
        sta zpPtr
        lda #>msg_rwts_fail
        sta zpPtr+1

; ---- restore the test sector to its pre-test contents (SECBUF is still the
; XORed pattern; XOR it back to the original and write it out) ----
rst_restore:
        ldy #0
rst_unxor:
        lda SECBUF,y
        eor #$FF
        sta SECBUF,y
        iny
        bne rst_unxor

        lda #RWTS_TTRACK
        sta zpTrkWant
        lda #<SECBUF
        sta zpBuf
        lda #>SECBUF
        sta zpBuf+1
        lda #RWTS_TSEC
        jsr rwts_write

        lda #22
        sta zpCol
        lda #0
        sta zpRow
        lda #0
        sta zpInv
        jmp draw_string

; ---------------------------------------------------------------- fs_selftest
; fs_selftest - M2 SS2 proof gate: exercise fs_find/fs_read/fs_save/
; fs_delete (incl. heap compaction + directory-slot removal) against the
; mkfs-seeded catalog. Draws "FS PASS"/"FS FAIL" on the title bar (row 0,
; col 32). zpTmp ($05, free pre-mainloop) holds the file count across calls.
; fst_fail is far enough from these checks that direct beq/bne fst_fail
; would be out-of-range branches; "bcc/bcs/beq/bne *+5 / jmp fst_fail" is
; the standard dasm short-branch-around-a-jmp idiom (the 2-byte branch
; skips the 3-byte jmp).
fs_selftest:
; ---- 1. find + read the seeded README.TXT (2 sectors) ----
        lda #<fst_name_readme
        sta zpFSName
        lda #>fst_name_readme
        sta zpFSName+1
        jsr fs_find
        cmp #$FF
        bne *+5
        jmp fst_fail
        pha                     ; save dir index - <SECBUF=0 would clobber A
        lda #<SECBUF
        sta zpFSDat
        lda #>SECBUF
        sta zpFSDat+1
        pla
        jsr fs_read
        lda zpFSSize
        cmp #<361
        beq *+5
        jmp fst_fail
        lda zpFSSize+1
        cmp #>361
        beq *+5
        jmp fst_fail
        lda SECBUF
        cmp #$55                ; 'U'
        beq *+5
        jmp fst_fail

; ---- 2. save two small test files ----
        lda CATBUF+FSC_COUNT
        sta zpTmp               ; count before (2: HELLO.TXT, README.TXT)

        lda #<fst_name_t1
        sta zpFSName
        lda #>fst_name_t1
        sta zpFSName+1
        lda #<fst_data1
        sta zpFSDat
        lda #>fst_data1
        sta zpFSDat+1
        lda #16
        sta zpFSSize
        lda #0
        sta zpFSSize+1
        jsr fs_save
        cmp #0
        beq *+5
        jmp fst_fail

        lda #<fst_name_t2
        sta zpFSName
        lda #>fst_name_t2
        sta zpFSName+1
        lda #<fst_data2
        sta zpFSDat
        lda #>fst_data2
        sta zpFSDat+1
        lda #16
        sta zpFSSize
        lda #0
        sta zpFSSize+1
        jsr fs_save
        cmp #0
        beq *+5
        jmp fst_fail

        lda CATBUF+FSC_COUNT
        sec
        sbc zpTmp
        cmp #2
        beq *+5
        jmp fst_fail

; ---- 3. delete the first test file (not last) -> forces heap compaction ----
        lda #<fst_name_t1
        sta zpFSName
        lda #>fst_name_t1
        sta zpFSName+1
        jsr fs_find
        cmp #$FF
        bne *+5
        jmp fst_fail
        jsr fs_delete

        lda #<fst_name_t1
        sta zpFSName
        lda #>fst_name_t1
        sta zpFSName+1
        jsr fs_find
        cmp #$FF
        beq *+5                 ; FST1 must be gone
        jmp fst_fail

; ---- 4. the second test file must have survived with its data intact ----
        lda #<fst_name_t2
        sta zpFSName
        lda #>fst_name_t2
        sta zpFSName+1
        jsr fs_find
        cmp #$FF
        bne *+5
        jmp fst_fail
        pha                     ; save dir index - <SECBUF=0 would clobber A
        lda #<SECBUF
        sta zpFSDat
        lda #>SECBUF
        sta zpFSDat+1
        pla
        jsr fs_read
        ldy #0
fst_cmp2:
        lda SECBUF,y
        cmp fst_data2,y
        beq *+5
        jmp fst_fail
        iny
        cpy #16
        bne fst_cmp2

; ---- 5. clean up FST2 too; the catalog must return to its original state ----
        lda #<fst_name_t2
        sta zpFSName
        lda #>fst_name_t2
        sta zpFSName+1
        jsr fs_find
        cmp #$FF
        bne *+5
        jmp fst_fail
        jsr fs_delete
        lda CATBUF+FSC_COUNT
        cmp zpTmp
        beq *+5
        jmp fst_fail

        lda #<msg_fs_pass
        sta zpPtr
        lda #>msg_fs_pass
        sta zpPtr+1
        jmp fst_draw
fst_fail:
        lda #<msg_fs_fail
        sta zpPtr
        lda #>msg_fs_fail
        sta zpPtr+1
fst_draw:
        lda #32
        sta zpCol
        lda #0
        sta zpRow
        lda #0
        sta zpInv
        jmp draw_string

fst_name_readme: dc.b "README.TXT",0,0
fst_name_t1:     dc.b "FSTEST1.TXT",0
fst_name_t2:     dc.b "FSTEST2.TXT",0
fst_data1:       dc.b "FSTEST1 PAYLOAD!"
fst_data2:       dc.b "FSTEST2 PAYLOAD!"
        endif

; ============================================================================
; pat_tab - mutable renderer fill patterns (M3 Theme, see theme.i). Read by
; dither_rect/frame_rect/draw_win/draw_icon; rewritten live by theme_apply.
;   [0] dither even-row   [1] dither odd-row
;   [2] accent ("on": borders, focused titles, selected icon labels)
;   [3] background ("off": unfocused titles, unselected icon labels)
; Initial values = the Classic preset (th_presets index 0, theme.i).
; ============================================================================
pat_tab:          dc.b $55,$2A,$7F,$00

; ============================================================================
; strings
; ============================================================================
msg_title:        dc.b "UnoDOS",0
msg_sysinfo:      dc.b "SysInfo",0
msg_clock:        dc.b "Clock",0
msg_files:        dc.b "Files",0
msg_theme:        dc.b "Theme",0
msg_dostris:      dc.b "Dostris",0
msg_pacman:       dc.b "Pac-Man",0
msg_music:        dc.b "Music",0
msg_tracker:      dc.b "Tracker",0
msg_paint:        dc.b "Paint",0
msg_outlast:      dc.b "OutLast",0
msg_files_title:  dc.b "Files",0
msg_notepad_title: dc.b "Notepad: ",0
msg_files_help:   dc.b "RET=Open  D=Delete  R=Rescan  ESC=Back",0
msg_files_empty:  dc.b "(no files)",0
msg_confirm1:     dc.b "Delete ",0
msg_confirm2:     dc.b "? (Y/N)",0
msg_note_help:    dc.b "  ^S=Save  ESC=Back",0
msg_ln:           dc.b "Ln:",0
msg_col:          dc.b "  Col:",0
msg_bytes:        dc.b "  Bytes:",0
msg_full:         dc.b "  FULL",0
msg_saved:        dc.b "  SAVED",0
msg_cpu:          dc.b "CPU: 6502 @ 1MHz",0
msg_ram:          dc.b "RAM: 64K",0
msg_mach_ii:      dc.b "Apple II",0
msg_mach_iiplus:  dc.b "Apple II+",0
msg_mach_iie:     dc.b "Apple IIe",0
msg_mach_iie_enh: dc.b "Apple IIe Enh.",0
msg_mach_iic:     dc.b "Apple IIc",0
msg_mach_iiefam:  dc.b "Apple IIe family",0
msg_mach_unk:     dc.b "Apple ?",0

        ifconst AUTOTEST
msg_rwts_pass:    dc.b "RWTS PASS",0
msg_rwts_fail:    dc.b "RWTS FAIL",0
msg_fs_pass:      dc.b "FS PASS",0
msg_fs_fail:      dc.b "FS FAIL",0
        endif

; ============================================================================
; hi-res row address tables (lo/hi bytes of the column-0 address for
; pixel rows y=0..191; see header comment for the formula)
; ============================================================================
rowlo:
	dc.b $00,$00,$00,$00,$00,$00,$00,$00,$80,$80,$80,$80,$80,$80,$80,$80
	dc.b $00,$00,$00,$00,$00,$00,$00,$00,$80,$80,$80,$80,$80,$80,$80,$80
	dc.b $00,$00,$00,$00,$00,$00,$00,$00,$80,$80,$80,$80,$80,$80,$80,$80
	dc.b $00,$00,$00,$00,$00,$00,$00,$00,$80,$80,$80,$80,$80,$80,$80,$80
	dc.b $28,$28,$28,$28,$28,$28,$28,$28,$A8,$A8,$A8,$A8,$A8,$A8,$A8,$A8
	dc.b $28,$28,$28,$28,$28,$28,$28,$28,$A8,$A8,$A8,$A8,$A8,$A8,$A8,$A8
	dc.b $28,$28,$28,$28,$28,$28,$28,$28,$A8,$A8,$A8,$A8,$A8,$A8,$A8,$A8
	dc.b $28,$28,$28,$28,$28,$28,$28,$28,$A8,$A8,$A8,$A8,$A8,$A8,$A8,$A8
	dc.b $50,$50,$50,$50,$50,$50,$50,$50,$D0,$D0,$D0,$D0,$D0,$D0,$D0,$D0
	dc.b $50,$50,$50,$50,$50,$50,$50,$50,$D0,$D0,$D0,$D0,$D0,$D0,$D0,$D0
	dc.b $50,$50,$50,$50,$50,$50,$50,$50,$D0,$D0,$D0,$D0,$D0,$D0,$D0,$D0
	dc.b $50,$50,$50,$50,$50,$50,$50,$50,$D0,$D0,$D0,$D0,$D0,$D0,$D0,$D0
rowhi:
	dc.b $20,$24,$28,$2C,$30,$34,$38,$3C,$20,$24,$28,$2C,$30,$34,$38,$3C
	dc.b $21,$25,$29,$2D,$31,$35,$39,$3D,$21,$25,$29,$2D,$31,$35,$39,$3D
	dc.b $22,$26,$2A,$2E,$32,$36,$3A,$3E,$22,$26,$2A,$2E,$32,$36,$3A,$3E
	dc.b $23,$27,$2B,$2F,$33,$37,$3B,$3F,$23,$27,$2B,$2F,$33,$37,$3B,$3F
	dc.b $20,$24,$28,$2C,$30,$34,$38,$3C,$20,$24,$28,$2C,$30,$34,$38,$3C
	dc.b $21,$25,$29,$2D,$31,$35,$39,$3D,$21,$25,$29,$2D,$31,$35,$39,$3D
	dc.b $22,$26,$2A,$2E,$32,$36,$3A,$3E,$22,$26,$2A,$2E,$32,$36,$3A,$3E
	dc.b $23,$27,$2B,$2F,$33,$37,$3B,$3F,$23,$27,$2B,$2F,$33,$37,$3B,$3F
	dc.b $20,$24,$28,$2C,$30,$34,$38,$3C,$20,$24,$28,$2C,$30,$34,$38,$3C
	dc.b $21,$25,$29,$2D,$31,$35,$39,$3D,$21,$25,$29,$2D,$31,$35,$39,$3D
	dc.b $22,$26,$2A,$2E,$32,$36,$3A,$3E,$22,$26,$2A,$2E,$32,$36,$3A,$3E
	dc.b $23,$27,$2B,$2F,$33,$37,$3B,$3F,$23,$27,$2B,$2F,$33,$37,$3B,$3F

        include "build/font7.s"

        include "rwts.i"
        include "fs.i"
        include "files.i"
        include "notepad.i"
        include "theme.i"
        include "dostris.i"
        include "pacman.i"
        include "music.i"
        include "tracker.i"
        include "paint.i"
        include "outlast.i"
        include "build/notes7.s"

; ============================================================================
; vars - kernel variable block (UDM1 discovery header points here)
; ============================================================================
vars:
frame_ctr:   dc.w 0       ; vars+0  main-loop pass counter (mod TICKS_PER_SEC)
sel_icon:    dc.b 0       ; vars+2  desktop icon selection (0=SysInfo,1=Clock,2=Files)
zcount:      dc.b 0       ; vars+3  number of open windows
clock_secs:  dc.w 0       ; vars+4  soft-clock seconds since boot
top_win:     dc.b 0       ; vars+6  zlist[0] mirror ($FF = none)
focus:       dc.b 0       ; vars+7  $FF = desktop, else focused window id
win_state:   dc.b 0,0     ; vars+8  per-window open(1)/closed(0)
zlist:       dc.b 0,0     ; vars+10 z-order, [0]=topmost, $FF=empty

; ---- Files/Notepad/Theme app state ----
app_mode:    dc.b 0       ; 0=desktop, 1=Files, 2=Notepad, 3=Theme
files_sel:   dc.b 0       ; selected directory index in Files
files_confirm: dc.b 0     ; 0=normal, 1=awaiting delete y/n
note_idx:    dc.b 0       ; directory index Notepad was opened from
note_name:   dc.b 0,0,0,0,0,0,0,0,0,0,0,0   ; (12) file being edited
note_len:    dc.w 0       ; current buffer length (bytes)
note_dirty:  dc.b 0       ; 1 if unsaved changes
note_flash:  dc.b 0       ; status line: 0=help, 1=SAVED, 2=FULL
th_sel:      dc.b 0       ; Theme: highlighted preset (cursor)
th_cur:      dc.b 0       ; Theme: currently-applied preset (matches pat_tab)

; ---- Dostris state (board itself is DOSBOARD in BSS, see dostris.i) ----
dos_piece:   dc.b 0       ; current piece type 0-6 (I,O,T,S,Z,J,L)
dos_rot:     dc.b 0       ; current rotation 0-3
dos_px:      dc.b 0       ; current piece origin column (4x4 box, 0-9)
dos_py:      dc.b 0       ; current piece origin row (4x4 box, 0-19)
dos_next:    dc.b 0       ; next piece type 0-6
dos_score:   dc.w 0       ; score
dos_lines:   dc.b 0       ; total lines cleared
dos_level:   dc.b 0       ; level = dos_lines/4
dos_dctr:    dc.b 0       ; gravity tick counter (soft-clock seconds)
dos_paused:  dc.b 0       ; 1 = paused
dos_over:    dc.b 0       ; 1 = game over
dos_rng:     dc.b 1       ; LCG seed (must stay nonzero)
dos_justlock: dc.b 0      ; softdrop set: 1 if this call locked+respawned

; ---- Pac-Man state (maze itself is PMMAZE in BSS, see pacman.i) ----
pac_col:     dc.b 0       ; pac tile column 0-12
pac_row:     dc.b 0       ; pac tile row 0-12
pac_dir:     dc.b 1       ; current direction 0=up,1=left,2=down,3=right
pac_nextdir: dc.b 1       ; queued turn (applied at the next open tile)
gh_col:      dc.b 0,0     ; ghost tile columns
gh_row:      dc.b 0,0     ; ghost tile rows
gh_dir:      dc.b 0,0     ; ghost directions
gh_mode:     dc.b 0,0     ; 0=normal,1=frightened,2=eaten
pm_score:    dc.w 0
pm_lives:    dc.b 3
pm_level:    dc.b 1
pm_dots:     dc.w 0       ; remaining dots+power pellets
pm_state:    dc.b 0       ; 0=playing, 1=game over
pm_fright:   dc.b 0       ; frightened seconds remaining
pm_mode:     dc.b 0       ; 0=scatter, 1=chase
pm_modet:    dc.b 0       ; seconds in the current scatter/chase phase
pm_subtick:  dc.b 0       ; half-speed toggle (frightened ghosts)
pm_stepctr:  dc.b 0       ; soft-seconds since the last game step
pm_rng:      dc.b 1       ; LCG seed (must stay nonzero)

; ---- Music/Tracker state (note table is mus_notes in build/notes7.s) ----
mus_idx:     dc.b 0       ; music: current note index during draw/playback
tk_row:      dc.b 0       ; tracker: cursor row 0-31
tk_chan:     dc.b 0       ; tracker: cursor channel 0-3
tk_top:      dc.b 0       ; tracker: first visible row (scroll)
tk_vis:      dc.b 0       ; tracker draw: visible-row index 0-11
tk_cur:      dc.b 0       ; tracker draw/play: absolute row being processed
tk_ch:       dc.b 0       ; tracker draw/play: channel loop index
tk_cellbuf:  dc.b 0,0,0,0,0,0   ; tracker: formatted "C-2:0" cell text

; ---- Paint state (canvas itself is PAINTBUF in BSS, see paint.i) ----
pt_cx:       dc.b 0       ; cursor cell column 0-31
pt_cy:       dc.b 0       ; cursor cell row 0-33
pt_tool:     dc.b 0       ; 0=pencil 1=rect 2=frect 3=fill
pt_ink:      dc.b 3       ; current ink index 0-3
pt_anchor:   dc.b 0       ; rect/frect: 1 if an anchor is set
pt_ax:       dc.b 0       ; anchor cell column
pt_ay:       dc.b 0       ; anchor cell row
pt_outline:  dc.b 0       ; fillrect: 1=border only
pt_target:   dc.b 0       ; flood fill: pattern being replaced
pt_sp:       dc.b 0       ; flood fill: stack pointer
pt_px:       dc.b 0       ; paint draw/flood: working column
pt_py:       dc.b 0       ; paint draw/flood: working row
pt_rx0:      dc.b 0       ; fillrect: normalised bounds
pt_rx1:      dc.b 0
pt_ry0:      dc.b 0
pt_ry1:      dc.b 0
pt_rx:       dc.b 0       ; fillrect: col iterator
pt_ry:       dc.b 0       ; fillrect: row iterator

; ---- OutLast state (feasibility prototype, see outlast.i) ----
ol_carx:     dc.b 0       ; car byte-column
ol_scroll:   dc.b 0       ; road scroll phase
ol_dist:     dc.w 0       ; distance travelled
ol_crash:    dc.b 0       ; 1 = off road
ol_b:        dc.b 0       ; ol_draw: band loop index
ol_band:     dc.b 0       ; ol_draw_band: current band
ol_bandy:    dc.b 0       ; ol_draw_band: band pixel y
ol_bc:       dc.b 0       ; band road centre
ol_bhw:      dc.b 0       ; band road half-width
ol_bl:       dc.b 0       ; band road left col
ol_br:       dc.b 0       ; band road right col
