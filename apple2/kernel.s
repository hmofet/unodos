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

ICONW           equ 10    ; icon grid
ICONH           equ 40
ICONY           equ 152
LABELY          equ 168
LABELROW        equ 21
ICON0_X         equ 2     ; SysInfo icon
ICON1_X         equ 14    ; Clock icon

; TICKS_PER_SEC: main-loop passes per soft-clock second. Calibrated with
; harness.py's TICK_INSTRS so `wait N` advances clock_secs predictably
; (HANDOFF SS6b) - see harness.py for the paired constant and the math.
TICKS_PER_SEC   equ 1000

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

        jsr draw_desktop

        ifconst AUTOTEST
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
        bne ml_cdraw
        inc clock_secs+1
ml_cdraw:
        lda win_state+1
        beq mainloop
        jsr draw_clock_content
        jmp mainloop

; handle_key - A = key code from $C000 (bit7 set). ESC always closes the
; topmost window (kernel-side). Otherwise: if the desktop has focus,
; left/right move sel_icon and Return launches/raises the selected app;
; if a window has focus, other keys are routed to it (no-op for M1 apps).
handle_key:
        sta zpTmp
        cmp #$9B                ; ESC
        bne hk_notesc
        jsr close_topmost
        rts
hk_notesc:
        lda focus
        cmp #$FF
        bne hk_done             ; window focused: no app key handling at M1
        lda zpTmp
        cmp #$8D                ; Return
        beq hk_return
        cmp #$95                ; right
        beq hk_move
        cmp #$88                ; left (== backspace on a II+)
        beq hk_move
        jmp hk_done
hk_move:
        lda sel_icon            ; only 2 icons: left/right both toggle
        eor #1
        sta sel_icon
        jsr draw_icons
        rts
hk_return:
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
        lda #$7F
        jmp dw_pat
dw_unfoc:
        lda #$00
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
        lda #$7F
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

; draw_icons - redraw both desktop icons, highlighting sel_icon.
draw_icons:
        lda #ICON0_X
        sta zpFX
        lda #<msg_sysinfo
        sta zpPtr
        lda #>msg_sysinfo
        sta zpPtr+1
        lda sel_icon
        cmp #0
        beq di0_sel
        lda #0
        jmp di0_go
di0_sel:
        lda #1
di0_go:
        jsr draw_icon

        lda #ICON1_X
        sta zpFX
        lda #<msg_clock
        sta zpPtr
        lda #>msg_clock
        sta zpPtr+1
        lda sel_icon
        cmp #1
        beq di1_sel
        lda #0
        jmp di1_go
di1_sel:
        lda #1
di1_go:
        jsr draw_icon
        rts

; draw_icon - zpFX = box x, zpPtr = label, A = 1 if selected (inverted
; label band) else 0.
draw_icon:
        sta zpTmp
        lda #ICONY
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
        lda #LABELY
        sta zpFY
        lda #(ICONW-2)
        sta zpFW
        lda #8
        sta zpFH
        lda zpTmp
        beq di_pat0
        lda #$7F
        jmp di_pat
di_pat0:
        lda #$00
di_pat:
        sta zpFPat
        jsr fill_rows

        lda zpFX
        sta zpCol
        lda #LABELROW
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
; byte columns) using pattern $7F (all 7 pixels set).
frame_rect:
        lda zpFX
        sta zpFX0
        lda zpFY
        sta zpFY0
        lda zpFW
        sta zpFW0
        lda zpFH
        sta zpFH0
        lda #$7F
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

; dither_rect - fill zpFX,zpFY,zpFW,zpFH with a checkerboard ($55/$2A
; alternating by pixel row - the macplus pat_tab 50% dither, byte form).
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
        lda #$2A
        jmp dr_go
dr_even:
        lda #$55
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
; clobbers zpTmp, zpFontPtr, zpRowLoPtr/Hi, zpDst, A/X/Y.
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

; ============================================================================
; strings
; ============================================================================
msg_title:        dc.b "UnoDOS",0
msg_sysinfo:      dc.b "SysInfo",0
msg_clock:        dc.b "Clock",0
msg_cpu:          dc.b "CPU: 6502 @ 1MHz",0
msg_ram:          dc.b "RAM: 64K",0
msg_mach_ii:      dc.b "Apple II",0
msg_mach_iiplus:  dc.b "Apple II+",0
msg_mach_iie:     dc.b "Apple IIe",0
msg_mach_iie_enh: dc.b "Apple IIe Enh.",0
msg_mach_iic:     dc.b "Apple IIc",0
msg_mach_iiefam:  dc.b "Apple IIe family",0
msg_mach_unk:     dc.b "Apple ?",0

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

; ============================================================================
; vars - kernel variable block (UDM1 discovery header points here)
; ============================================================================
vars:
frame_ctr:   dc.w 0       ; vars+0  main-loop pass counter (mod TICKS_PER_SEC)
sel_icon:    dc.b 0       ; vars+2  desktop icon selection (0=SysInfo,1=Clock)
zcount:      dc.b 0       ; vars+3  number of open windows
clock_secs:  dc.w 0       ; vars+4  soft-clock seconds since boot
top_win:     dc.b 0       ; vars+6  zlist[0] mirror ($FF = none)
focus:       dc.b 0       ; vars+7  $FF = desktop, else focused window id
win_state:   dc.b 0,0     ; vars+8  per-window open(1)/closed(0)
zlist:       dc.b 0,0     ; vars+10 z-order, [0]=topmost, $FF=empty
