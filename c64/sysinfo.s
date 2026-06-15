; ============================================================================
; UnoDOS/C64 SysInfo - a DISK-LOADED app (app0.bin, icon 0). Was a desktop
; window in the M1/M2 kernel; in the full-screen one-app-at-a-time model it is
; a full-screen disk-loaded app like every other. Shows the machine identity,
; CPU/video standard (read from the kernel's is_pal flag, exported via the API)
; and SID. Static: no per-frame work (tick = rts). RUN/STOP returns.
;
; App ABI: first three words are JMPs (init / key / tick). See sys.inc.
; ============================================================================

        processor 6502
        include "sys.inc"
        include "build/kernel_api.inc"

        org APP_BASE
        jmp si_init
        jmp si_key
        jmp si_tick

si_init:
        jmp si_draw

si_key:
        lda zpTmp
        cmp #K_ESC
        bne sk_done
        jmp return_to_desktop
sk_done:
        rts

si_tick:
        rts

; si_line - draw the NUL-terminated string at zpPtr on row A, col 1.
si_line:
        sta zpRow
        lda #1
        sta zpCol
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        jmp draw_string

si_draw:
        jsr app_clear
        ; title row
        lda #<msg_si_title
        sta zpPtr
        lda #>msg_si_title
        sta zpPtr+1
        lda #1
        sta zpCol
        lda #0
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string
        ; separator
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
        ; line: machine
        lda #<msg_mach
        sta zpPtr
        lda #>msg_mach
        sta zpPtr+1
        lda #2
        jsr si_line
        ; line: CPU (PAL/NTSC)
        lda is_pal
        beq sid_cpu_ntsc
        lda #<msg_cpu_pal
        sta zpPtr
        lda #>msg_cpu_pal
        sta zpPtr+1
        jmp sid_cpu_go
sid_cpu_ntsc:
        lda #<msg_cpu_ntsc
        sta zpPtr
        lda #>msg_cpu_ntsc
        sta zpPtr+1
sid_cpu_go:
        lda #3
        jsr si_line
        ; line: RAM
        lda #<msg_ram
        sta zpPtr
        lda #>msg_ram
        sta zpPtr+1
        lda #4
        jsr si_line
        ; line: video
        lda is_pal
        beq sid_vid_ntsc
        lda #<msg_vid_pal
        sta zpPtr
        lda #>msg_vid_pal
        sta zpPtr+1
        jmp sid_vid_go
sid_vid_ntsc:
        lda #<msg_vid_ntsc
        sta zpPtr
        lda #>msg_vid_ntsc
        sta zpPtr+1
sid_vid_go:
        lda #5
        jsr si_line
        ; line: SID
        lda #<msg_sid
        sta zpPtr
        lda #>msg_sid
        sta zpPtr+1
        lda #6
        jsr si_line
        ; help line
        lda #<msg_si_help
        sta zpPtr
        lda #>msg_si_help
        sta zpPtr+1
        lda #23
        jmp si_line

msg_si_title:  dc.b "SysInfo",0
msg_mach:      dc.b "Commodore 64",0
msg_cpu_pal:   dc.b "CPU: 6510 @ 0.985MHz",0
msg_cpu_ntsc:  dc.b "CPU: 6510 @ 1.023MHz",0
msg_ram:       dc.b "RAM: 64K",0
msg_vid_pal:   dc.b "Video: PAL VIC-II 6569",0
msg_vid_ntsc:  dc.b "Video: NTSC VIC-II 6567",0
msg_sid:       dc.b "Sound: SID 6581",0
msg_si_help:   dc.b "STOP=back",0
