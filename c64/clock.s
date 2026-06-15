; ============================================================================
; UnoDOS/C64 Clock - a DISK-LOADED app (app1.bin, icon 1). Was a desktop window
; in the M1/M2 kernel; now a full-screen disk-loaded app. Reads the CIA #1
; Time-of-Day registers (BCD, a real hardware clock) via the kernel's read_tod
; API and shows HH:MM:SS, refreshed once per second from its tick. RUN/STOP
; returns to the launcher.
;
; App ABI: first three words are JMPs (init / key / tick). See sys.inc.
; ============================================================================

        processor 6502
        include "sys.inc"
        include "build/kernel_api.inc"

CK_TX    equ 16          ; time cell column
CK_TY    equ 11          ; time cell row

        org APP_BASE
        jmp ck_init_r
        jmp ck_key
        jmp ck_tick

ck_init_r:
        lda #$FF
        sta ck_last_sec         ; force a redraw on the first tick
        jsr ck_draw_static
        jmp ck_draw_time

ck_key:
        lda zpTmp
        cmp #K_ESC
        bne ckk_done
        jmp return_to_desktop
ckk_done:
        rts

; ck_tick - re-read the TOD; redraw the time only if the second changed.
ck_tick:
        jsr read_tod
        lda zpSS
        cmp ck_last_sec
        beq ckt_done
        jmp ck_draw_time
ckt_done:
        rts

; ck_draw_static - clear + title + separator + help (drawn once at init).
ck_draw_static:
        jsr app_clear
        lda #<msg_ck_title
        sta zpPtr
        lda #>msg_ck_title
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
        lda #$FF
        sta zpFPat
        jsr fill_rows
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda #<msg_ck_help
        sta zpPtr
        lda #>msg_ck_help
        sta zpPtr+1
        jmp draw_string

; ck_draw_time - read TOD, format HH:MM:SS, draw it; remember the second.
ck_draw_time:
        jsr read_tod
        lda zpSS
        sta ck_last_sec
        lda zpHH
        ldy #0
        jsr ck_fmt_bcd
        lda #$3A                ; ':'
        sta clkbuf+2
        lda zpMM
        ldy #3
        jsr ck_fmt_bcd
        lda #$3A
        sta clkbuf+5
        lda zpSS
        ldy #6
        jsr ck_fmt_bcd
        lda #0
        sta clkbuf+8
        lda #CK_TX
        sta zpCol
        lda #CK_TY
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda #<clkbuf
        sta zpPtr
        lda #>clkbuf
        sta zpPtr+1
        jmp draw_string

; ck_fmt_bcd - A = BCD byte, Y = offset; write 2 ASCII digits to clkbuf+Y.
ck_fmt_bcd:
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

msg_ck_title: dc.b "Clock",0
msg_ck_help:  dc.b "STOP=back",0

; mutable state (reset on each launch)
ck_last_sec:  dc.b $FF
