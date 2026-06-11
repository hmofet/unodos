; ============================================================================
; UnoDOS/68K - Amiga port, milestone 2
; ============================================================================
; Bare-metal UnoDOS desktop for Amiga OCS/ECS (68000, 512KB chip RAM).
; Built as an AmigaDOS hunk executable (vasm -Fhunkexe), packed onto a
; bootable ADF with exe2adf. The OS bootstrap LoadSegs us, then we enter
; supervisor mode, take over the machine (interrupts, DMA, display, input)
; and never return.
;
; Implements the milestone-1 slice of docs/PORT-SPEC.md:
;   - 320x200x4 display, UnoDOS palette (blue/cyan/magenta/white)
;   - desktop: menu bar, icon grid with labels, version footer
;   - hardware-sprite mouse cursor, vblank-polled quadrature mouse
;   - CIA-A keyboard ISR -> event queue (32 entries)
;   - press-time click latch + sequence counter (PORT-SPEC SS3)
;   - window manager: frames, title bars, close button, drag with XOR
;     outline + clamping, z-order with click-to-raise (title or body)
;   - in-kernel apps: SysInfo and Clock (uptime), launched from desktop
;     icons by double-click or arrow keys + Enter; ESC closes
;   - serial debug markers at 9600-8N1
;
; Audit-derived rules carried over (PORT-SPEC SS6): ISRs only update state
; (the hardware sprite makes cursor drawing free), edge-only mouse events,
; press-time click latching, topmost-only periodic content refresh.
; ============================================================================

        mc68000

; ---------------------------------------------------------------- constants
CUSTOM      equ $DFF000
VPOSR       equ $004
JOY0DAT     equ $00A
SERDATR     equ $018
INTENAR     equ $01C
INTREQR     equ $01E
SERDAT      equ $030
SERPER      equ $032
COP1LCH     equ $080
COPJMP1     equ $088
DIWSTRT     equ $08E
DIWSTOP     equ $090
DDFSTRT     equ $092
DDFSTOP     equ $094
DMACON      equ $096
INTENA      equ $09A
INTREQ      equ $09C
BPL1PTH     equ $0E0
BPL2PTH     equ $0E4
BPLCON0     equ $100
BPLCON1     equ $102
BPLCON2     equ $104
BPL1MOD     equ $108
BPL2MOD     equ $10A
SPR0PTH     equ $120
COLOR00     equ $180
COLOR17     equ $1A2

CIAA_PRA    equ $BFE001
CIAA_CRA    equ $BFEE01
CIAA_ICR    equ $BFED01
CIAA_SDR    equ $BFEC01
CIAB_ICR    equ $BFDD00

; fixed chip-RAM layout (all below 512KB)
PLANE0      equ $70000              ; 8000 bytes
PLANE1      equ $72000              ; 8000 bytes
COPLIST     equ $76000
SPRDAT      equ $76800              ; cursor sprite (must be chip)
NULLSPR     equ $76900
STACKTOP    equ $7C000

SCRW        equ 320
SCRH        equ 200
ROWB        equ 40                  ; bytes per row per plane

; UnoDOS palette (PORT-SPEC SS1)
COL_BLUE    equ $000A
COL_CYAN    equ $00AA
COL_MAG     equ $0A0A
COL_WHITE   equ $0FFF

; events (PORT-SPEC SS3)
EV_KEY      equ 1
EV_MOUSE    equ 4
EVQ_SIZE    equ 32                  ; entries of 4 bytes (type.b,pad.b,data.w)

; window manager
MAXWIN      equ 6
WENT_SIZE   equ 16
WSTATE      equ 0                   ; 0=free 1=visible
WPROC       equ 1                   ; app proc index
WX          equ 2
WY          equ 4
WW          equ 6
WH          equ 8
WTITLE      equ 10                  ; title pointer (long)

TBAR_H      equ 10
MENUBAR_H   equ 12                  ; protected desktop rows (drag clamp)

TICKS_SEC   equ 50                  ; PAL vblank (forced in unodos.uae)
DBLCLICK    equ 25                  ; double-click window (0.5s)

ICON0_X     equ 32                  ; byte-aligned by construction
ICON0_Y     equ 30
ICON_PITCH  equ 64                  ; 5 icons across 320px
NICONS      equ 9                   ; 5 per row, wraps to a second row

; Paula audio channel 0
AUD0LCH     equ $0A0
AUD0LEN     equ $0A4
AUD0PER     equ $0A6
AUD0VOL     equ $0A8
SQRWAVE     equ $76A00              ; 8-byte square sample (chip RAM)

NBUF        equ 2048                ; notepad buffer
RD_ENT      equ 20                  ; romdisk entry: 12B name + ptr.l + size.w + cap.w

CURSOR_H    equ 14

; ============================================================================
        section code,code

start:
        move.l  4.w,a6              ; SysBase
        lea     super(pc),a5
        jsr     -30(a6)             ; Supervisor() -> (a5) in supervisor mode

super:
        or.w    #$0700,sr           ; mask all interrupts at the CPU
        lea     STACKTOP,sp

        lea     CUSTOM,a6
        move.w  #$7FFF,INTENA(a6)
        move.w  #$7FFF,INTREQ(a6)
        move.w  #$7FFF,INTREQ(a6)
        move.w  #$7FFF,DMACON(a6)
        move.b  #$7F,CIAA_ICR
        move.b  #$7F,CIAB_ICR
        move.b  CIAA_ICR,d0         ; reading clears pending CIA ints
        move.b  CIAB_ICR,d0

        lea     isr_lvl2(pc),a0     ; 68000: vectors live at address 0
        move.l  a0,$68.w
        lea     isr_lvl3(pc),a0
        move.l  a0,$6C.w

        move.w  #368,SERPER(a6)     ; 9600 baud (PAL clock)
        lea     str_boot(pc),a0
        bsr     ser_puts

        and.b   #$BF,CIAA_CRA       ; keyboard: SP input mode
        move.b  #$88,CIAA_ICR       ; enable CIA-A SP interrupt

        bsr     build_copper

        bsr     clear_screen        ; both planes -> color 0 (desktop blue)

        lea     cursor_spr(pc),a0   ; sprite image -> chip RAM
        lea     SPRDAT,a1
        moveq   #(CURSOR_H*2)+2-1,d0
.spcpy: move.w  (a0)+,(a1)+
        dbra    d0,.spcpy
        clr.l   (a1)                ; sprite terminator
        lea     NULLSPR,a1
        clr.l   (a1)+
        clr.l   (a1)

        ; Paula square-wave sample (4 high, 4 low) into chip RAM
        lea     SQRWAVE,a0
        move.l  #$7F7F7F7F,(a0)+
        move.l  #$81818181,(a0)
        move.l  #SQRWAVE,AUD0LCH(a6)
        move.w  #4,AUD0LEN(a6)      ; 4 words = 8 samples

        move.l  #COPLIST,COP1LCH(a6)
        move.w  COPJMP1(a6),d0
        move.w  #$83A0,DMACON(a6)   ; DMAEN|BPLEN|COPEN|SPREN

        bsr     splash_show         ; "UnoDOS 3" splash (~2s), clears after
        bsr     draw_desktop
        move.w  #$C028,INTENA(a6)   ; INTEN|VERTB|PORTS
        and.w   #$F8FF,sr           ; allow interrupts (stay supervisor)

        lea     str_desktop(pc),a0
        bsr     ser_puts

        ifd     AUTOTEST
        ; Auto-launch the app stack for screenshot verification without
        ; host input injection. Build: -DAUTOTEST=1
        ifd     AUTOTEST_THEME
        ; Theme-app variant: open the picker, select preset 4 (Sunset) and
        ; apply it through the real key handler. Build: -DAUTOTEST_THEME=1
        moveq   #5,d0
        bsr     launch_app
        moveq   #2,d3
.atth:  move.w  d3,-(sp)            ; draw_window clobbers d3
        moveq   #0,d1
        moveq   #$4D,d2             ; down
        bsr     theme_key
        move.w  (sp)+,d3
        dbra    d3,.atth
        moveq   #13,d1
        moveq   #0,d2
        bsr     theme_key           ; Enter = apply
        endc
        ifd     AUTOTEST_PACMAN
        ; Pac-Man variant: new game, force play, run 150 real steps.
        moveq   #8,d0
        bsr     launch_app
        moveq   #0,d1
        move.b  #'n',d1
        moveq   #0,d2
        bsr     pacman_key
        lea     vars(pc),a4
        move.w  #PMS_PLAY,pm_state-vars(a4)
        move.w  #149,d3
.atpm:  move.w  d3,-(sp)
        bsr     pm_step
        move.w  (sp)+,d3
        dbra    d3,.atpm
        endc
        ifd     AUTOTEST_DOSTRIS
        ; Dostris variant: start a game and hard-drop six pieces through
        ; the real key handler. Build: -DAUTOTEST=1 -DAUTOTEST_DOSTRIS=1
        moveq   #6,d0
        bsr     launch_app
        moveq   #0,d1
        move.b  #'n',d1
        moveq   #0,d2
        bsr     dostris_key
        moveq   #5,d3
.atdt:  move.w  d3,-(sp)
        moveq   #0,d1
        moveq   #$4F,d2             ; nudge left
        bsr     dostris_key
        moveq   #32,d1              ; hard drop
        moveq   #0,d2
        bsr     dostris_key
        move.w  (sp)+,d3
        dbra    d3,.atdt
        endc
        ifd     AUTOTEST_OUTLAST
        ; OutLast variant: start driving and run 60 forced physics steps.
        moveq   #7,d0
        bsr     launch_app
        moveq   #0,d1
        move.b  #'n',d1
        moveq   #0,d2
        bsr     outlast_key
        move.w  #59,d3
.atol:  move.w  d3,-(sp)
        lea     vars(pc),a4
        move.l  ticks(pc),d0
        sub.l   #100,d0
        move.l  d0,ol_last-vars(a4) ; force the step gate open
        bsr     outlast_tick
        move.w  (sp)+,d3
        dbra    d3,.atol
        endc
        ifd     AUTOTEST_NOTEPAD
        ; Notepad-focused variant: demo text (caret at end) exercises the
        ; vertical-scroll clamp. Build: -DAUTOTEST=1 -DAUTOTEST_NOTEPAD=1
        bsr     notepad_set_demo
        moveq   #3,d0               ; Notepad (topmost)
        bsr     launch_app
        ; drive six up-arrows through the real key handler: caret should
        ; land on Ln 12 with the goal column held (status bar proves it)
        moveq   #5,d3
.atnp:  move.w  d3,-(sp)            ; draw_window clobbers d3
        moveq   #0,d1
        moveq   #$4C,d2
        bsr     notepad_key
        move.w  (sp)+,d3
        dbra    d3,.atnp
        else
        ifnd    AUTOTEST_THEME
        ifnd    AUTOTEST_DOSTRIS
        ifnd    AUTOTEST_OUTLAST
        ifnd    AUTOTEST_PACMAN
        moveq   #2,d0               ; README.TXT (romdisk sorts: CANON,HELLO,README)
        bsr     notepad_open_file
        moveq   #3,d0               ; Notepad (bottom)
        bsr     launch_app
        moveq   #2,d0               ; Files (middle)
        bsr     launch_app
        moveq   #4,d0               ; Music (topmost, playing)
        bsr     launch_app
        bsr     music_start
        endc
        endc
        endc
        endc
        endc
        endc

; ============================================================================
; Main loop - single cooperative context (milestone-1 scaffolding for the
; portable-core scheduler). All input decisions live here, never in ISRs.
; ============================================================================
main_loop:
        bsr     handle_clicks
        bsr     handle_drag
        bsr     handle_events
        bsr     music_tick
        bsr     gm_tick
        bsr     dostris_tick
        bsr     outlast_tick
        bsr     pacman_tick
        bsr     app_ticks
        bra     main_loop

; ----------------------------------------------------------------------------
; handle_events - drain the queue (keyboard navigation)
; ----------------------------------------------------------------------------
handle_events:
.next:
        bsr     ev_get              ; d0.b = type (0 none), d1.w = data
        tst.b   d0
        beq     .done
        cmp.b   #EV_KEY,d0
        bne     .next               ; mouse events: the click latch rules
        move.w  d1,d2
        lsr.w   #8,d2               ; d2 = raw scancode
        and.w   #$FF,d1             ; d1 = ascii (0 if none)
        ; focused (topmost) window gets first refusal (PORT-SPEC SS3)
        move.w  zcount(pc),d3
        beq     .desktop
        movem.w d1-d2,-(sp)
        move.w  zcount(pc),d2
        subq.w  #1,d2
        bsr     zwin_ptr            ; a2 = topmost (preserves regs)
        moveq   #0,d3
        move.b  WPROC(a2),d3
        movem.w (sp)+,d1-d2
        cmp.w   #2,d3
        beq     .k_files
        cmp.w   #3,d3
        beq     .k_notepad
        cmp.w   #4,d3
        beq     .k_music
        cmp.w   #5,d3
        beq     .k_theme
        cmp.w   #6,d3
        beq     .k_dostris
        cmp.w   #7,d3
        beq     .k_outlast
        cmp.w   #8,d3
        beq     .k_pacman
.k_global:
        cmp.b   #27,d1              ; ESC closes topmost
        bne     .next
        bsr     close_topmost
        bra     .next
.k_files:
        bsr     files_key           ; in: d1=ascii d2=raw; out: d0=0 consumed
        tst.w   d0
        beq     .next
        bra     .k_global
.k_notepad:
        bsr     notepad_key
        tst.w   d0
        beq     .next
        bra     .k_global
.k_music:
        bsr     music_key
        tst.w   d0
        beq     .next
        bra     .k_global
.k_theme:
        bsr     theme_key
        tst.w   d0
        beq     .next
        bra     .k_global
.k_dostris:
        bsr     dostris_key
        tst.w   d0
        beq     .next
        bra     .k_global
.k_outlast:
        bsr     outlast_key
        tst.w   d0
        beq     .next
        bra     .k_global
.k_pacman:
        bsr     pacman_key
        tst.w   d0
        beq     .next
        bra     .k_global
.desktop:
        cmp.b   #$4E,d2
        beq     .selright
        cmp.b   #$4F,d2
        beq     .selleft
        cmp.b   #27,d1
        beq     .esc
        cmp.b   #13,d1
        beq     .launch
        bra     .next
.selright:
        move.w  sel_icon(pc),d0
        addq.w  #1,d0
        cmp.w   #NICONS,d0
        blt     .setsel
        moveq   #0,d0
        bra     .setsel
.selleft:
        move.w  sel_icon(pc),d0
        subq.w  #1,d0
        bge     .setsel
        move.w  #NICONS-1,d0
.setsel:
        bsr     select_icon
        bra     .next
.esc:
        bsr     close_topmost
        bra     .next
.launch:
        move.w  sel_icon(pc),d0
        bsr     launch_app
        bra     .next
.done:
        rts

; ----------------------------------------------------------------------------
; handle_clicks - consume the ISR press latch (PORT-SPEC SS3 / SS6 rule 4)
; ----------------------------------------------------------------------------
handle_clicks:
        move.b  click_seq(pc),d0
        cmp.b   last_seq(pc),d0
        bne     .work
        rts
.work:
        lea     vars(pc),a4
        move.b  d0,last_seq-vars(a4)
        move.w  click_x(pc),d0
        move.w  click_y(pc),d1
        bsr     find_window_at      ; d2 = z index of topmost hit, or -1
        bmi     .desktop
        move.w  d2,d3
        bsr     zwin_ptr            ; a2 = window (preserves d0-d3)
        move.w  WY(a2),d4
        add.w   #TBAR_H,d4
        cmp.w   d4,d1
        blt     .title
        ; body click: raise if not already topmost
        move.w  zcount(pc),d4
        subq.w  #1,d4
        cmp.w   d4,d3
        beq     .out                ; topmost body: app's business
        move.w  d3,d0
        bsr     raise_window
        rts
.title:
        ; close box = rightmost 12px of the bar
        move.w  WX(a2),d4
        add.w   WW(a2),d4
        sub.w   #12,d4
        cmp.w   d4,d0
        blt     .dragstart
        move.w  d3,d0
        bsr     close_window
        rts
.dragstart:
        move.w  d3,d0
        bsr     raise_window        ; (repaints if it actually raised)
        lea     vars(pc),a4
        move.w  zcount(pc),d2
        subq.w  #1,d2
        bsr     zwin_ptr            ; a2 = now-topmost window
        move.w  click_x(pc),d0
        sub.w   WX(a2),d0
        move.w  d0,drag_offx-vars(a4)
        move.w  click_y(pc),d0
        sub.w   WY(a2),d0
        move.w  d0,drag_offy-vars(a4)
        move.l  a2,drag_win-vars(a4)
        move.w  #-1,drag_outx-vars(a4)
        st      drag_active-vars(a4)
        rts
.desktop:
        move.w  click_x(pc),d0
        move.w  click_y(pc),d1
        bsr     icon_at             ; d0 = icon index or -1
        bmi     .deselect
        move.w  d0,d3
        move.l  ticks(pc),d4
        cmp.w   dbl_icon(pc),d0
        bne     .single
        move.l  d4,d5
        sub.l   dbl_tick(pc),d5
        cmp.l   #DBLCLICK,d5
        bgt     .single
        lea     vars(pc),a4
        move.w  #-1,dbl_icon-vars(a4)
        move.w  d3,d0
        bsr     select_icon
        move.w  d3,d0
        bsr     launch_app
        rts
.single:
        lea     vars(pc),a4
        move.w  d3,dbl_icon-vars(a4)
        move.l  d4,dbl_tick-vars(a4)
        move.w  d3,d0
        bsr     select_icon
        rts
.deselect:
        lea     vars(pc),a4
        move.w  #-1,dbl_icon-vars(a4)
.out:
        rts

; ----------------------------------------------------------------------------
; handle_drag - track/finish a title-bar drag (XOR outline, main loop only)
; ----------------------------------------------------------------------------
handle_drag:
        move.b  drag_active(pc),d0  ; (tst.b (pc) is 68020+)
        bne     .active
        rts
.active:
        lea     vars(pc),a4
        move.l  drag_win(pc),a2
        ; target = mouse - grab offset, clamped (PORT-SPEC SS2)
        move.w  mouse_x(pc),d0
        sub.w   drag_offx(pc),d0
        move.w  mouse_y(pc),d1
        sub.w   drag_offy(pc),d1
        tst.w   d0
        bge     .xmin
        moveq   #0,d0
.xmin:  move.w  #SCRW,d2
        sub.w   WW(a2),d2
        cmp.w   d2,d0
        ble     .xok
        move.w  d2,d0
.xok:   cmp.w   #MENUBAR_H,d1
        bge     .ymin
        move.w  #MENUBAR_H,d1
.ymin:  move.w  #SCRH-TBAR_H,d2
        cmp.w   d2,d1
        ble     .yok
        move.w  d2,d1
.yok:
        move.b  mouse_btn(pc),d2    ; (tst.b (pc) is 68020+)
        beq     .finish
        cmp.w   drag_outx(pc),d0
        bne     .redraw
        cmp.w   drag_outy(pc),d1
        bne     .redraw
        rts
.redraw:
        move.w  d0,-(sp)
        move.w  d1,-(sp)
        bsr     erase_outline
        move.w  (sp)+,d1
        move.w  (sp)+,d0
        lea     vars(pc),a4
        move.w  d0,drag_outx-vars(a4)
        move.w  d1,drag_outy-vars(a4)
        bsr     draw_outline
        rts
.finish:
        move.w  d0,-(sp)
        move.w  d1,-(sp)
        bsr     erase_outline
        move.w  (sp)+,d1
        move.w  (sp)+,d0
        move.l  drag_win(pc),a2
        move.w  d0,WX(a2)
        move.w  d1,WY(a2)
        lea     vars(pc),a4
        sf      drag_active-vars(a4)
        bsr     repaint_all
        rts

; erase_outline: XOR away the previous outline (skips if none drawn yet)
erase_outline:
        move.w  drag_outx(pc),d0
        bmi     eo_skip
        move.w  drag_outy(pc),d1
        bra     do_outline
eo_skip:
        rts
; draw_outline: XOR an outline at d0/d1 with the dragged window's size
draw_outline:
do_outline:
        move.l  drag_win(pc),a2
        move.w  WW(a2),d2
        move.w  WH(a2),d3
        bsr     xor_rect
        rts

; ----------------------------------------------------------------------------
; app_ticks - once a second, refresh the TOPMOST window's content
; (single-topmost content rule, PORT-SPEC SS2)
; ----------------------------------------------------------------------------
app_ticks:
        move.l  ticks(pc),d0
        move.l  d0,d1
        sub.l   last_secs(pc),d1
        cmp.l   #TICKS_SEC,d1
        bge     .go
        rts
.go:
        lea     vars(pc),a4
        move.l  d0,last_secs-vars(a4)
        move.w  zcount(pc),d2
        bne     .have
        rts
.have:  subq.w  #1,d2
        bsr     zwin_ptr
        moveq   #0,d0
        move.b  WPROC(a2),d0
        cmp.w   #6,d0               ; games (6-8) drive their own drawing
        bge     .skip
        bsr     app_draw_content
.skip:  rts

; ============================================================================
; Desktop
; ============================================================================
draw_desktop:
        lea     str_menutitle(pc),a0
        moveq   #4,d0
        moveq   #2,d1
        moveq   #3,d2
        bsr     draw_string
        lea     str_version(pc),a0
        moveq   #4,d0
        move.w  #SCRH-10,d1
        moveq   #3,d2
        bsr     draw_string
        lea     str_build(pc),a0
        move.w  #SCRW-96,d0
        move.w  #SCRH-10,d1
        moveq   #3,d2
        bsr     draw_string
        moveq   #0,d7
.icons: move.w  d7,d0
        bsr     draw_icon_cell
        addq.w  #1,d7
        cmp.w   #NICONS,d7
        blt     .icons
        rts

; icon_pos - d0 = icon index -> d0 = x, d1 = y (5 per row, rows 44px apart)
icon_pos:
        move.l  d2,-(sp)
        moveq   #0,d2
        move.w  d0,d2
        divu    #5,d2               ; low = row, high = col
        move.w  d2,d1
        mulu    #44,d1
        add.w   #ICON0_Y,d1
        swap    d2
        move.w  d2,d0
        mulu    #ICON_PITCH,d0
        add.w   #ICON0_X,d0
        move.l  (sp)+,d2
        rts

; draw_icon_cell - d0 = icon index (draws icon, label, selection box)
draw_icon_cell:
        move.w  d0,d7
        bsr     icon_pos
        ; clear the cell (selection box area)
        move.w  d0,-(sp)
        move.w  d1,-(sp)
        subq.w  #4,d0
        subq.w  #4,d1
        moveq   #24,d2
        moveq   #24,d3
        moveq   #0,d4
        bsr     fill_rect
        move.w  (sp)+,d1
        move.w  (sp)+,d0
        ; icon bitmap
        move.w  d0,-(sp)
        move.w  d1,-(sp)
        move.w  d7,d2
        lsl.w   #2,d2
        lea     icon_tab(pc),a0
        move.l  (a0,d2.w),a0
        bsr     draw_icon16
        move.w  (sp)+,d1
        move.w  (sp)+,d0
        ; label (PORT-SPEC metric: label x = icon_x - 8)
        move.w  d0,-(sp)
        move.w  d1,-(sp)
        move.w  d7,d2
        lsl.w   #2,d2
        lea     name_tab(pc),a0
        move.l  (a0,d2.w),a0
        subq.w  #8,d0
        add.w   #20,d1
        moveq   #3,d2
        bsr     draw_string
        move.w  (sp)+,d1
        move.w  (sp)+,d0
        ; selection box
        cmp.w   sel_icon(pc),d7
        beq     .sel
        rts
.sel:   subq.w  #4,d0
        subq.w  #4,d1
        moveq   #24,d2
        moveq   #24,d3
        bsr     rect_outline_white
        rts

; select_icon - d0 = new selection
select_icon:
        move.w  d0,d6
        move.w  sel_icon(pc),d5
        lea     vars(pc),a4
        move.w  d6,sel_icon-vars(a4)
        cmp.w   d5,d6
        beq     .done
        move.w  d5,d0
        bmi     .new
        bsr     draw_icon_cell      ; deselect previous
.new:   move.w  d6,d0
        bsr     draw_icon_cell
.done:  rts

; icon_at - d0/d1 = point -> d0 = icon index or -1
icon_at:
        movem.l d3-d5,-(sp)
        move.w  d0,d3               ; px
        move.w  d1,d4               ; py
        moveq   #0,d7
.try:   move.w  d7,d0
        bsr     icon_pos            ; d0 = x, d1 = y
        move.w  d0,d5
        subq.w  #4,d5
        cmp.w   d5,d3
        blt     .no
        add.w   #24,d5
        cmp.w   d5,d3
        bge     .no
        move.w  d1,d5
        subq.w  #4,d5
        cmp.w   d5,d4
        blt     .no
        add.w   #32,d5
        cmp.w   d5,d4
        bge     .no
        move.w  d7,d0
        movem.l (sp)+,d3-d5
        rts
.no:    addq.w  #1,d7
        cmp.w   #NICONS,d7
        blt     .try
        moveq   #-1,d0
        movem.l (sp)+,d3-d5
        rts

; launch_app - d0 = icon/proc index (re-raises if already open)
launch_app:
        move.w  d0,d7
        moveq   #0,d6
.scan:  move.w  d6,d2
        bsr     win_ptr_raw         ; a2 = slot d2
        tst.b   WSTATE(a2)
        beq     .next
        moveq   #0,d0
        move.b  WPROC(a2),d0
        cmp.w   d7,d0
        bne     .next
        bsr     z_index_of          ; a2 -> d0 = z index
        bmi     .next
        bsr     raise_window
        rts
.next:  addq.w  #1,d6
        cmp.w   #MAXWIN,d6
        blt     .scan
        ; create a new window from the app definition table
        move.w  d7,d0
        mulu    #10,d0
        lea     app_def_tab(pc),a0
        lea     (a0,d0.w),a0
        move.w  (a0)+,d1            ; x
        move.w  (a0)+,d2            ; y
        move.w  (a0)+,d3            ; w
        move.w  (a0)+,d4            ; h
        move.w  (a0),d5             ; title offset from start
        lea     start(pc),a1
        add.w   d5,a1
        move.w  d7,d0
        bsr     win_create
        lea     str_launch(pc),a0
        bsr     ser_puts
        rts

; ============================================================================
; Window manager
; ============================================================================

; win_create - d0=proc, d1=x d2=y d3=w d4=h, a1=title
win_create:
        movem.l d0-d4/a1,-(sp)
        moveq   #0,d6
.find:  move.w  d6,d2
        bsr     win_ptr_raw
        tst.b   WSTATE(a2)
        beq     .got
        addq.w  #1,d6
        cmp.w   #MAXWIN,d6
        blt     .find
        movem.l (sp)+,d0-d4/a1      ; table full: ignore (defined behavior)
        rts
.got:
        movem.l (sp)+,d0-d4/a1
        st      WSTATE(a2)
        move.b  d0,WPROC(a2)
        move.w  d1,WX(a2)
        move.w  d2,WY(a2)
        move.w  d3,WW(a2)
        move.w  d4,WH(a2)
        move.l  a1,WTITLE(a2)
        lea     vars(pc),a4
        lea     zlist-vars(a4),a0
        move.w  zcount(pc),d1
        move.b  d6,(a0,d1.w)
        addq.w  #1,d1
        move.w  d1,zcount-vars(a4)
        bsr     draw_window         ; topmost: nothing else needs repainting
        rts

; win_ptr_raw - d2 = table index -> a2. Preserves d0-d7/a0-a1.
win_ptr_raw:
        move.w  d2,-(sp)
        lsl.w   #4,d2               ; * WENT_SIZE
        lea     wintab(pc),a2
        lea     (a2,d2.w),a2
        move.w  (sp)+,d2
        rts

; zwin_ptr - d2 = z index -> a2. Preserves ALL data registers (d2 is the
; loop counter in find_window_at, so it must survive).
zwin_ptr:
        movem.l d2/a0,-(sp)
        lea     zlist(pc),a0
        and.w   #$FF,d2
        move.b  (a0,d2.w),d2       ; d2 = table slot for this z index
        and.w   #$FF,d2
        lsl.w   #4,d2              ; * WENT_SIZE
        lea     wintab(pc),a2
        lea     (a2,d2.w),a2
        movem.l (sp)+,d2/a0
        rts

; z_index_of - a2 = window entry -> d0 = z index or -1
z_index_of:
        move.l  a0,-(sp)
        lea     wintab(pc),a0
        move.l  a2,d0
        sub.l   a0,d0
        lsr.w   #4,d0               ; table index
        lea     zlist(pc),a0
        moveq   #0,d1
.scan:  cmp.w   zcount(pc),d1
        bge     .no
        moveq   #0,d2
        move.b  (a0,d1.w),d2
        cmp.w   d0,d2
        beq     .yes
        addq.w  #1,d1
        bra     .scan
.yes:   move.w  d1,d0
        move.l  (sp)+,a0
        rts
.no:    moveq   #-1,d0
        move.l  (sp)+,a0
        rts

; find_window_at - d0/d1 = point -> d2 = z index (topmost hit) or -1.
; Preserves d0/d1.
find_window_at:
        move.w  zcount(pc),d2
        subq.w  #1,d2
.scan:  tst.w   d2
        bmi     .out
        bsr     zwin_ptr            ; a2 (preserves d0-d3)
        cmp.w   WX(a2),d0
        blt     .next
        move.w  WX(a2),d3
        add.w   WW(a2),d3
        cmp.w   d3,d0
        bge     .next
        cmp.w   WY(a2),d1
        blt     .next
        move.w  WY(a2),d3
        add.w   WH(a2),d3
        cmp.w   d3,d1
        bge     .next
.out:   rts
.next:  subq.w  #1,d2
        bra     .scan

; raise_window - d0 = z index: move to top of zlist, repaint if changed
raise_window:
        move.w  zcount(pc),d1
        subq.w  #1,d1
        cmp.w   d1,d0
        bne     .doit
        rts                         ; already topmost
.doit:
        lea     zlist(pc),a0
        moveq   #0,d2
        move.b  (a0,d0.w),d2
.shift: cmp.w   d1,d0
        bge     .place
        move.b  1(a0,d0.w),(a0,d0.w)
        addq.w  #1,d0
        bra     .shift
.place: move.b  d2,(a0,d0.w)
        bsr     repaint_all
        rts

; close_window - d0 = z index
close_window:
        bsr     gm_stop             ; close silences audio (PORT-SPEC SS2)
        move.w  d0,-(sp)
        move.w  d0,d2
        bsr     zwin_ptr
        sf      WSTATE(a2)
        move.w  (sp)+,d0
        lea     vars(pc),a4
        lea     zlist-vars(a4),a0
        move.w  zcount(pc),d1
        subq.w  #1,d1
        move.w  d1,zcount-vars(a4)
.shift: cmp.w   d1,d0
        bge     .done
        move.b  1(a0,d0.w),(a0,d0.w)
        addq.w  #1,d0
        bra     .shift
.done:  bsr     repaint_all
        rts

close_topmost:
        move.w  zcount(pc),d0
        bne     .have
        rts
.have:  subq.w  #1,d0
        bsr     close_window
        rts

; repaint_all - desktop, then windows bottom-up (paint order = z-clip)
repaint_all:
        bsr     clear_screen
        bsr     draw_desktop
        moveq   #0,d7
.wins:  cmp.w   zcount(pc),d7
        bge     .done
        move.w  d7,d2
        bsr     zwin_ptr
        bsr     draw_window
        addq.w  #1,d7
        bra     .wins
.done:  rts

; draw_window - a2 = window entry (frame + title + close + content)
draw_window:
        move.l  a2,-(sp)
        ; content background (blue)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        sub.w   #TBAR_H+1,d3
        moveq   #0,d4
        bsr     fill_rect
        move.l  (sp),a2
        ; title bar (white)
        move.w  WX(a2),d0
        move.w  WY(a2),d1
        move.w  WW(a2),d2
        moveq   #TBAR_H,d3
        moveq   #3,d4
        bsr     fill_rect
        move.l  (sp),a2
        ; border
        move.w  WX(a2),d0
        move.w  WY(a2),d1
        move.w  WW(a2),d2
        move.w  WH(a2),d3
        bsr     rect_outline_white
        move.l  (sp),a2
        ; title text (blue on white)
        move.l  WTITLE(a2),a0
        move.w  WX(a2),d0
        addq.w  #4,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        move.l  (sp),a2
        ; close glyph
        lea     str_x(pc),a0
        move.w  WX(a2),d0
        add.w   WW(a2),d0
        sub.w   #10,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        move.l  (sp),a2
        ; content
        moveq   #0,d0
        move.b  WPROC(a2),d0
        bsr     app_draw_content
        move.l  (sp)+,a2
        rts

; ============================================================================
; Apps (in-kernel window procs, milestone 1)
; ============================================================================

; app_draw_content - d0 = proc index, a2 = window
app_draw_content:
        move.l  a2,-(sp)
        cmp.w   #1,d0
        blt     .sysinfo
        beq     .clock
        cmp.w   #3,d0
        blt     .files
        beq     .notepad
        cmp.w   #5,d0
        beq     .theme
        cmp.w   #6,d0
        beq     .dostris
        cmp.w   #7,d0
        beq     .outlast
        cmp.w   #8,d0
        beq     .pacman
        bsr     music_draw
        bra     .done
.theme: bsr     theme_draw
        bra     .done
.dostris:
        bsr     dostris_draw
        bra     .done
.outlast:
        bsr     outlast_draw
        bra     .done
.pacman:
        bsr     pacman_draw
        bra     .done
.sysinfo:
        bsr     sysinfo_draw
        bra     .done
.clock: bsr     clock_draw
        bra     .done
.files: bsr     files_draw
        bra     .done
.notepad:
        bsr     notepad_draw
.done:  move.l  (sp)+,a2
        rts

; ---- SysInfo ----
sysinfo_draw:
        move.w  WX(a2),d6
        addq.w  #6,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+4,d5
        lea     str_si1(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        lea     str_si2(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   #10,d1
        moveq   #3,d2
        bsr     draw_string
        lea     str_si3(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   #20,d1
        moveq   #3,d2
        bsr     draw_string
        lea     str_si4(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   #30,d1
        moveq   #3,d2
        bsr     draw_string
        ; uptime seconds (cyan on blue, fixed-width overwrite)
        move.l  ticks(pc),d0
        divu    #TICKS_SEC,d0
        lea     numbuf(pc),a0
        bsr     fmt_u16
        lea     numbuf(pc),a0
        move.w  d6,d0
        add.w   #72,d0
        move.w  d5,d1
        add.w   #30,d1
        moveq   #1,d2
        moveq   #0,d3
        bsr     draw_string_bg
        lea     str_si5(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   #44,d1
        moveq   #2,d2
        bsr     draw_string
        rts

; ---- Clock (uptime as HH:MM:SS) ----
clock_draw:
        move.l  ticks(pc),d0
        divu    #TICKS_SEC,d0
        and.l   #$FFFF,d0           ; seconds
        divu    #60,d0
        move.l  d0,d3
        swap    d3                  ; d3.w = seconds
        and.l   #$FFFF,d0
        divu    #60,d0
        move.l  d0,d2
        swap    d2                  ; d2.w = minutes
        and.w   #$FFFF,d0           ; d0.w = hours
        lea     clkbuf(pc),a0
        bsr     put2dig
        move.b  #':',(a0)+
        move.w  d2,d0
        bsr     put2dig
        move.b  #':',(a0)+
        move.w  d3,d0
        bsr     put2dig
        clr.b   (a0)
        ; "Uptime" caption
        lea     str_uptime(pc),a0
        move.w  WX(a2),d0
        addq.w  #6,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+4,d1
        moveq   #3,d2
        bsr     draw_string
        ; centered time, cyan on blue
        move.w  WX(a2),d0
        move.w  WW(a2),d1
        lsr.w   #1,d1
        add.w   d1,d0
        sub.w   #32,d0              ; 8 chars * 8px / 2
        move.w  WY(a2),d1
        move.w  WH(a2),d3
        lsr.w   #1,d3
        add.w   d3,d1
        subq.w  #2,d1
        lea     clkbuf(pc),a0
        moveq   #1,d2
        moveq   #0,d3
        bsr     draw_string_bg
        rts

; put2dig - d0.w (0..99) -> two ASCII digits at (a0)+. Trashes d0/d1.
put2dig:
        and.l   #$FFFF,d0
        divu    #10,d0
        move.l  d0,d1
        add.b   #'0',d0
        move.b  d0,(a0)+
        swap    d1
        add.b   #'0',d1
        move.b  d1,(a0)+
        rts

; fmt_u16 - d0.w -> decimal + 's' + NUL at a0 (max "65535s")
fmt_u16:
        and.l   #$FFFF,d0
        lea     8(a0),a1            ; build digits backwards in the tail
        clr.b   -(a1)
.dig:   divu    #10,d0
        swap    d0
        add.b   #'0',d0
        move.b  d0,-(a1)
        clr.w   d0
        swap    d0
        tst.w   d0
        bne     .dig
.copy:  move.b  (a1)+,(a0)+         ; forward copy to buffer start (safe)
        bne     .copy
        subq.l  #1,a0
        move.b  #'s',(a0)+
        move.b  #' ',(a0)+          ; pad: erases a shrinking digit
        clr.b   (a0)
        rts

; ============================================================================
; Graphics primitives (2 bitplanes, 320x200, byte-wise RMW)
; ============================================================================

; clear_screen - both planes to 0. NOTE: the planes are NOT contiguous
; (PLANE0+8000 = $71F40, PLANE1 = $72000) - clear each separately or
; stale memory shows up as a garbage band in the last rows of plane 1.
clear_screen:
        movem.l d0-d1/a0,-(sp)
        lea     PLANE0,a0
        move.w  #(8000/4)-1,d0
        moveq   #0,d1
.c0:    move.l  d1,(a0)+
        dbra    d0,.c0
        lea     PLANE1,a0
        move.w  #(8000/4)-1,d0
.c1:    move.l  d1,(a0)+
        dbra    d0,.c1
        movem.l (sp)+,d0-d1/a0
        rts

; plane_row - d1 = y -> a0/a1 = row bases. Trashes d5.
plane_row:
        move.w  d1,d5
        mulu    #ROWB,d5
        lea     PLANE0,a0
        add.l   d5,a0
        lea     PLANE1,a1
        add.l   d5,a1
        rts

; fill_rect - d0=x d1=y d2=w d3=h d4=color(0-3). Preserves all registers.
fill_rect:
        movem.l d0-d7/a0-a3,-(sp)
        bsr     fill_rect_raw
        movem.l (sp)+,d0-d7/a0-a3
        rts
fill_rect_raw:
        tst.w   d2
        ble     fr_out
        tst.w   d3
        ble     fr_out
        move.w  d0,d6
        add.w   d2,d6
        subq.w  #1,d6               ; d6 = xend
        bsr     plane_row
        move.w  d0,d5
        lsr.w   #3,d5
        add.w   d5,a0
        add.w   d5,a1
        move.w  d6,d2
        lsr.w   #3,d2
        sub.w   d5,d2               ; d2 = span-1 in bytes
        ; left mask = $FF >> (x&7)
        moveq   #0,d5
        move.b  #$FF,d5
        and.w   #7,d0
        lsr.b   d0,d5
        ; right mask = $FF << (7 - (xend&7))
        moveq   #0,d7
        move.b  #$FF,d7
        and.w   #7,d6
        moveq   #7,d1
        sub.w   d6,d1
        lsl.b   d1,d7
        tst.w   d2
        bne     .multi
        and.b   d7,d5               ; single byte: combined mask
        move.b  d5,d7
.multi:
        subq.w  #1,d3               ; dbra row count
.row:
        move.l  a0,a2
        move.l  a1,a3
        ; left byte
        btst    #0,d4
        beq     .l0c
        or.b    d5,(a2)
        bra     .l0d
.l0c:   move.b  d5,d0
        not.b   d0
        and.b   d0,(a2)
.l0d:   btst    #1,d4
        beq     .l1c
        or.b    d5,(a3)
        bra     .l1d
.l1c:   move.b  d5,d0
        not.b   d0
        and.b   d0,(a3)
.l1d:
        tst.w   d2
        beq     .rownext            ; single-byte row
        move.w  d2,d1
        subq.w  #1,d1               ; middle bytes = span-1 - 1
        ble     .rightset
        subq.w  #1,d1               ; dbra count
        lea     1(a2),a2
        lea     1(a3),a3
.mid:   btst    #0,d4
        beq     .m0c
        move.b  #$FF,(a2)+
        bra     .m0d
.m0c:   clr.b   (a2)+
.m0d:   btst    #1,d4
        beq     .m1c
        move.b  #$FF,(a3)+
        bra     .m1d
.m1c:   clr.b   (a3)+
.m1d:   dbra    d1,.mid
        bra     .right
.rightset:
        lea     1(a2),a2
        lea     1(a3),a3
.right:
        btst    #0,d4
        beq     .r0c
        or.b    d7,(a2)
        bra     .r0d
.r0c:   move.b  d7,d0
        not.b   d0
        and.b   d0,(a2)
.r0d:   btst    #1,d4
        beq     .r1c
        or.b    d7,(a3)
        bra     .r1d
.r1c:   move.b  d7,d0
        not.b   d0
        and.b   d0,(a3)
.r1d:
.rownext:
        lea     ROWB(a0),a0
        lea     ROWB(a1),a1
        dbra    d3,.row
fr_out: rts

; rect_outline_white - d0=x d1=y d2=w d3=h
rect_outline_white:
        movem.w d0-d3,-(sp)
        moveq   #1,d3
        moveq   #3,d4
        bsr     fill_rect
        movem.w (sp),d0-d3
        add.w   d3,d1
        subq.w  #1,d1
        moveq   #1,d3
        moveq   #3,d4
        bsr     fill_rect
        movem.w (sp),d0-d3
        moveq   #1,d2
        moveq   #3,d4
        bsr     fill_rect
        movem.w (sp),d0-d3
        add.w   d2,d0
        subq.w  #1,d0
        moveq   #1,d2
        moveq   #3,d4
        bsr     fill_rect
        movem.w (sp)+,d0-d3
        rts

; xor_rect - d0=x d1=y d2=w d3=h: invert a 1px outline on BOTH planes
; (self-erasing drag outline; sides inset so corners XOR exactly once)
xor_rect:
        movem.w d0-d3,-(sp)
        moveq   #1,d3
        bsr     xor_span
        movem.w (sp),d0-d3
        add.w   d3,d1
        subq.w  #1,d1
        moveq   #1,d3
        bsr     xor_span
        movem.w (sp),d0-d3
        addq.w  #1,d1
        subq.w  #2,d3
        moveq   #1,d2
        bsr     xor_span
        movem.w (sp),d0-d3
        add.w   d2,d0
        subq.w  #1,d0
        addq.w  #1,d1
        subq.w  #2,d3
        moveq   #1,d2
        bsr     xor_span
        movem.w (sp)+,d0-d3
        rts

; xor_span - d0=x d1=y d2=w d3=h: EOR the rect with 1s. Preserves all regs.
xor_span:
        movem.l d0-d7/a0-a3,-(sp)
        bsr     xor_span_raw
        movem.l (sp)+,d0-d7/a0-a3
        rts
xor_span_raw:
        tst.w   d2
        ble     xs_out
        tst.w   d3
        ble     xs_out
        move.w  d0,d6
        add.w   d2,d6
        subq.w  #1,d6
        bsr     plane_row
        move.w  d0,d5
        lsr.w   #3,d5
        add.w   d5,a0
        add.w   d5,a1
        move.w  d6,d2
        lsr.w   #3,d2
        sub.w   d5,d2
        moveq   #0,d5
        move.b  #$FF,d5
        and.w   #7,d0
        lsr.b   d0,d5
        moveq   #0,d7
        move.b  #$FF,d7
        and.w   #7,d6
        moveq   #7,d1
        sub.w   d6,d1
        lsl.b   d1,d7
        tst.w   d2
        bne     .multi
        and.b   d7,d5
        move.b  d5,d7
.multi: subq.w  #1,d3
.row:   move.l  a0,a2
        move.l  a1,a3
        eor.b   d5,(a2)
        eor.b   d5,(a3)
        tst.w   d2
        beq     .next
        move.w  d2,d1
        subq.w  #1,d1
        ble     .rightset
        subq.w  #1,d1
        lea     1(a2),a2
        lea     1(a3),a3
.mid:   not.b   (a2)+
        not.b   (a3)+
        dbra    d1,.mid
        bra     .right
.rightset:
        lea     1(a2),a2
        lea     1(a3),a3
.right: eor.b   d7,(a2)
        eor.b   d7,(a3)
.next:  lea     ROWB(a0),a0
        lea     ROWB(a1),a1
        dbra    d3,.row
xs_out: rts

; draw_icon16 - a0 = planar icon (16 words plane0, then 16 words plane1),
;               d0 = x (multiple of 8), d1 = y
draw_icon16:
        movem.l d0-d5/a0-a4,-(sp)
        bsr     di16_raw
        movem.l (sp)+,d0-d5/a0-a4
        rts
di16_raw:
        move.l  a0,a4               ; a4 = source data
        bsr     plane_row           ; a0/a1 = row bases (trashes d5)
        move.w  d0,d5
        lsr.w   #3,d5
        add.w   d5,a0
        add.w   d5,a1
        moveq   #15,d2
.r0:    move.w  (a4)+,d3
        move.b  d3,1(a0)
        lsr.w   #8,d3
        move.b  d3,(a0)
        lea     ROWB(a0),a0
        dbra    d2,.r0
        moveq   #15,d2
.r1:    move.w  (a4)+,d3
        move.b  d3,1(a1)
        lsr.w   #8,d3
        move.b  d3,(a1)
        lea     ROWB(a1),a1
        dbra    d2,.r1
        rts

; ----------------------------------------------------------------------------
; draw_char - d0=x d1=y d2=char d3=fg(0-3) d4=bg(0-3, or -1 = transparent)
; Unaligned: 2-byte RMW per row per plane. Preserves all registers.
; ----------------------------------------------------------------------------
draw_char:
        movem.l d0-d7/a0-a4,-(sp)
        sub.w   #32,d2
        bge     .lo
        moveq   #0,d2
.lo:    cmp.w   #95,d2
        blt     .hi
        moveq   #0,d2
.hi:    lsl.w   #3,d2
        lea     font8x8(pc),a4
        lea     (a4,d2.w),a4        ; a4 = glyph rows
        bsr     plane_row           ; a0/a1 (trashes d5)
        move.w  d0,d5
        lsr.w   #3,d5
        add.w   d5,a0
        add.w   d5,a1
        move.w  d0,d6
        and.w   #7,d6               ; d6 = shift
        moveq   #7,d7               ; 8 rows
.row:
        moveq   #0,d2
        move.b  (a4)+,d2
        lsl.w   #8,d2
        lsr.w   d6,d2               ; d2 = glyph bits in 16px window
        move.w  #$FF00,d5
        lsr.w   d6,d5               ; d5 = coverage mask
        ; ---- plane 0 ----
        moveq   #0,d0
        btst    #0,d3
        beq     .g0
        move.w  d2,d0               ; fg contributes glyph bits
.g0:    tst.w   d4
        bmi     .t0                 ; transparent background
        btst    #0,d4
        beq     .w0
        move.w  d5,d1
        eor.w   d2,d1               ; bg contributes mask&~glyph
        or.w    d1,d0
.w0:    move.w  d5,d1               ; clear masked bits, then set value
        not.w   d1
        and.b   d1,1(a0)
        ror.w   #8,d1
        and.b   d1,(a0)
        move.w  d0,d1
        or.b    d1,1(a0)
        ror.w   #8,d1
        or.b    d1,(a0)
        bra     .pl1
.t0:    move.w  d0,d1
        or.b    d1,1(a0)
        ror.w   #8,d1
        or.b    d1,(a0)
.pl1:
        ; ---- plane 1 ----
        moveq   #0,d2
        move.b  -1(a4),d2
        lsl.w   #8,d2
        lsr.w   d6,d2
        moveq   #0,d0
        btst    #1,d3
        beq     .g1
        move.w  d2,d0
.g1:    tst.w   d4
        bmi     .t1
        btst    #1,d4
        beq     .w1
        move.w  d5,d1
        eor.w   d2,d1
        or.w    d1,d0
.w1:    move.w  d5,d1
        not.w   d1
        and.b   d1,1(a1)
        ror.w   #8,d1
        and.b   d1,(a1)
        move.w  d0,d1
        or.b    d1,1(a1)
        ror.w   #8,d1
        or.b    d1,(a1)
        bra     .nx
.t1:    move.w  d0,d1
        or.b    d1,1(a1)
        ror.w   #8,d1
        or.b    d1,(a1)
.nx:
        lea     ROWB(a0),a0
        lea     ROWB(a1),a1
        dbra    d7,.row
        movem.l (sp)+,d0-d7/a0-a4
        rts

; draw_string    - a0 = NUL string, d0=x d1=y d2=fg (transparent bg)
; draw_string_bg - a0 = NUL string, d0=x d1=y d2=fg d3=bg
draw_string:
        moveq   #-1,d3
draw_string_bg:
        movem.l d0-d6/a0,-(sp)
        move.w  d2,d6               ; fg
        move.w  d3,d5               ; bg
.ch:    moveq   #0,d2
        move.b  (a0)+,d2
        beq     .done
        movem.l d0-d1/a0,-(sp)
        move.w  d6,d3
        move.w  d5,d4
        bsr     draw_char
        movem.l (sp)+,d0-d1/a0
        addq.w  #8,d0               ; advance = 8 (PORT-SPEC SS1)
        cmp.w   #SCRW-8,d0
        ble     .ch
.done:  movem.l (sp)+,d0-d6/a0
        rts

; ============================================================================
; Interrupt handlers - state only, no drawing (PORT-SPEC SS6 rule 2)
; ============================================================================

; level 3: vertical blank
isr_lvl3:
        movem.l d0-d3/a0-a1/a6,-(sp)
        lea     CUSTOM,a6
        move.w  INTREQR(a6),d0
        btst    #5,d0
        beq     .ack
        lea     vars(pc),a0
        addq.l  #1,ticks-vars(a0)
        ; mouse deltas (quadrature counters)
        move.w  JOY0DAT(a6),d0
        move.b  d0,d1
        sub.b   mouse_cntx(pc),d1
        ext.w   d1
        move.b  d0,mouse_cntx-vars(a0)
        add.w   mouse_x(pc),d1
        bge     .xlo
        moveq   #0,d1
.xlo:   cmp.w   #SCRW-1,d1
        ble     .xok
        move.w  #SCRW-1,d1
.xok:   move.w  d1,mouse_x-vars(a0)
        move.w  d0,d1
        lsr.w   #8,d1
        move.b  d1,d2
        sub.b   mouse_cnty(pc),d2
        ext.w   d2
        move.b  d1,mouse_cnty-vars(a0)
        add.w   mouse_y(pc),d2
        bge     .ylo
        moveq   #0,d2
.ylo:   cmp.w   #SCRH-1,d2
        ble     .yok
        move.w  #SCRH-1,d2
.yok:   move.w  d2,mouse_y-vars(a0)
        ; left button: edge-only events + press latch (PORT-SPEC SS3)
        moveq   #0,d1
        btst    #6,CIAA_PRA
        bne     .btn
        moveq   #1,d1
.btn:   move.b  mouse_btn(pc),d2
        move.b  d1,mouse_btn-vars(a0)
        cmp.b   d2,d1
        beq     .sprite
        tst.b   d1
        beq     .post
        move.w  mouse_x(pc),d3
        move.w  d3,click_x-vars(a0)
        move.w  mouse_y(pc),d3
        move.w  d3,click_y-vars(a0)
        addq.b  #1,click_seq-vars(a0)
.post:  moveq   #0,d2
        move.b  d1,d2
        move.w  d2,d1
        moveq   #EV_MOUSE,d0
        bsr     ev_post
.sprite:
        ; update hardware sprite position words
        move.w  mouse_y(pc),d0
        add.w   #$2C,d0             ; vstart
        move.w  d0,d1
        add.w   #CURSOR_H,d1        ; vstop
        move.w  mouse_x(pc),d2
        add.w   #$80,d2             ; hstart
        lea     SPRDAT,a1
        move.b  d0,(a1)             ; POS hi = vstart 7..0
        move.w  d2,d3
        lsr.w   #1,d3
        move.b  d3,1(a1)            ; POS lo = hstart 8..1
        move.b  d1,2(a1)            ; CTL hi = vstop 7..0
        moveq   #0,d3
        btst    #8,d0
        beq     .v8
        addq.b  #4,d3               ; SV8
.v8:    btst    #8,d1
        beq     .v9
        addq.b  #2,d3               ; EV8
.v9:    btst    #0,d2
        beq     .h0
        addq.b  #1,d3               ; H0
.h0:    move.b  d3,3(a1)
.ack:   move.w  #$0020,INTREQ(a6)
        movem.l (sp)+,d0-d3/a0-a1/a6
        rte

; level 2: CIA-A (keyboard)
isr_lvl2:
        movem.l d0-d3/a0/a6,-(sp)
        lea     CUSTOM,a6
        move.b  CIAA_ICR,d0         ; read clears CIA flags
        btst    #3,d0               ; SP?
        beq     .ack
        move.b  CIAA_SDR,d1
        or.b    #$40,CIAA_CRA       ; handshake: KDAT low >= 85us
        moveq   #127,d2
.dly:   tst.b   CIAA_PRA
        dbra    d2,.dly
        and.b   #$BF,CIAA_CRA
        not.b   d1
        ror.b   #1,d1               ; d1 = scancode | up-bit
        btst    #7,d1
        bne     .ack                ; ignore key-up
        lea     keymap_ascii(pc),a0
        moveq   #0,d2
        move.b  d1,d2
        move.b  (a0,d2.w),d3
        moveq   #0,d0
        move.b  d1,d0
        lsl.w   #8,d0
        or.b    d3,d0               ; data = (raw<<8) | ascii
        move.w  d0,d1
        moveq   #EV_KEY,d0
        bsr     ev_post
.ack:   move.w  #$0008,INTREQ(a6)
        movem.l (sp)+,d0-d3/a0/a6
        rte

; ============================================================================
; Event queue - 32 x 4 bytes; ISR producer, main-loop consumer
; ============================================================================

; ev_post - d0.b = type, d1.w = data (ISR context: interrupts already off)
ev_post:
        movem.l d2-d3/a0-a1,-(sp)
        lea     vars(pc),a0
        move.w  ev_tail(pc),d2
        move.w  d2,d3
        addq.w  #1,d3
        and.w   #EVQ_SIZE-1,d3
        cmp.w   ev_head(pc),d3
        beq     .full               ; drop-when-full (PORT-SPEC SS6 rule 10)
        lea     evq-vars(a0),a1
        lsl.w   #2,d2
        move.b  d0,(a1,d2.w)
        move.w  d1,2(a1,d2.w)
        move.w  d3,ev_tail-vars(a0)
.full:  movem.l (sp)+,d2-d3/a0-a1
        rts

; ev_get - d0.b = type (0 if empty), d1.w = data. Trashes d2-d3/a0-a1.
ev_get:
        lea     vars(pc),a1
        move.w  sr,d2
        or.w    #$0700,sr
        move.w  ev_head(pc),d0
        cmp.w   ev_tail(pc),d0
        beq     .empty
        lea     evq-vars(a1),a0
        move.w  d0,d1
        lsl.w   #2,d1
        move.w  d0,d3
        addq.w  #1,d3
        and.w   #EVQ_SIZE-1,d3
        move.w  d3,ev_head-vars(a1)
        moveq   #0,d0
        move.b  (a0,d1.w),d0
        move.w  2(a0,d1.w),d1
        move.w  d2,sr
        rts
.empty: move.w  d2,sr
        moveq   #0,d0
        rts

; ============================================================================
; Copper list + serial
; ============================================================================
build_copper:
        lea     COPLIST,a0
        move.w  #DIWSTRT,(a0)+
        move.w  #$2C81,(a0)+
        move.w  #DIWSTOP,(a0)+
        move.w  #$F4C1,(a0)+
        move.w  #DDFSTRT,(a0)+
        move.w  #$0038,(a0)+
        move.w  #DDFSTOP,(a0)+
        move.w  #$00D0,(a0)+
        move.w  #BPLCON0,(a0)+
        move.w  #$2200,(a0)+        ; 2 bitplanes, color
        move.w  #BPLCON1,(a0)+
        move.w  #0,(a0)+
        move.w  #BPLCON2,(a0)+
        move.w  #$0024,(a0)+        ; sprites in front of playfield
        move.w  #BPL1MOD,(a0)+
        move.w  #0,(a0)+
        move.w  #BPL2MOD,(a0)+
        move.w  #0,(a0)+
        move.w  #BPL1PTH,(a0)+
        move.w  #(PLANE0>>16)&$FFFF,(a0)+
        move.w  #BPL1PTH+2,(a0)+
        move.w  #PLANE0&$FFFF,(a0)+
        move.w  #BPL2PTH,(a0)+
        move.w  #(PLANE1>>16)&$FFFF,(a0)+
        move.w  #BPL2PTH+2,(a0)+
        move.w  #PLANE1&$FFFF,(a0)+
        movem.l a1/a4,-(sp)
        lea     vars(pc),a4
        move.l  a0,cop_colptr-vars(a4)  ; theme engine patches these words
        lea     theme_pal(pc),a1
        move.w  #COLOR00,(a0)+
        move.w  (a1)+,(a0)+
        move.w  #COLOR00+2,(a0)+
        move.w  (a1)+,(a0)+
        move.w  #COLOR00+4,(a0)+
        move.w  (a1)+,(a0)+
        move.w  #COLOR00+6,(a0)+
        move.w  (a1)+,(a0)+
        movem.l (sp)+,a1/a4
        move.w  #COLOR17,(a0)+      ; sprite 0/1 colors: cursor
        move.w  #COL_WHITE,(a0)+
        move.w  #COLOR17+2,(a0)+
        move.w  #COL_BLUE,(a0)+
        move.w  #COLOR17+4,(a0)+
        move.w  #COL_CYAN,(a0)+
        move.w  #SPR0PTH,(a0)+
        move.w  #(SPRDAT>>16)&$FFFF,(a0)+
        move.w  #SPR0PTH+2,(a0)+
        move.w  #SPRDAT&$FFFF,(a0)+
        moveq   #6,d0               ; sprites 1-7 -> null sprite
        move.w  #SPR0PTH+4,d1
.spr:   move.w  d1,(a0)+
        move.w  #(NULLSPR>>16)&$FFFF,(a0)+
        addq.w  #2,d1
        move.w  d1,(a0)+
        move.w  #NULLSPR&$FFFF,(a0)+
        addq.w  #2,d1
        dbra    d0,.spr
        move.w  #$FFFF,(a0)+
        move.w  #$FFFE,(a0)+
        rts

; ser_puts - a0 = NUL string -> built-in serial, polled (9600-8N1)
ser_puts:
        movem.l d0/a0/a6,-(sp)
        lea     CUSTOM,a6
.ch:    moveq   #0,d0
        move.b  (a0)+,d0
        beq     .done
.wait:  btst    #5,SERDATR(a6)      ; TBE (bit 13 -> bit 5 of high byte)
        beq     .wait
        or.w    #$0100,d0           ; stop bit
        move.w  d0,SERDAT(a6)
        bra     .ch
.done:  movem.l (sp)+,d0/a0/a6
        rts

        include "apps_m2.i"
        include "theme.i"
        include "games.i"
        include "pacman.i"

; ============================================================================
; Data
; ============================================================================
        even
str_boot:       dc.b    "UNODOS68K: boot",13,10,0
str_desktop:    dc.b    "UNODOS68K: desktop up",13,10,0
str_launch:     dc.b    "UNODOS68K: app launched",13,10,0
str_menutitle:  dc.b    "UnoDOS 68K",0
str_version:    dc.b    "UnoDOS/68K v0.2.0",0
str_build:      dc.b    "Milestone 2",0
str_x:          dc.b    "X",0
str_si1:        dc.b    "Video   320x200x4",0
str_si2:        dc.b    "Machine Amiga OCS",0
str_si3:        dc.b    "Chip    512 KB",0
str_si4:        dc.b    "Uptime",0
str_si5:        dc.b    "UnoDOS/68K  Milestone 2",0
str_uptime:     dc.b    "Uptime",0
str_t_sysinfo:  dc.b    "System Info",0
str_t_clock:    dc.b    "Clock",0
str_t_files:    dc.b    "Files",0
str_t_notepad:  dc.b    "Notepad",0
str_t_music:    dc.b    "Music",0
name_sysinfo:   dc.b    "Sys Info",0
name_clock:     dc.b    "Clock",0
name_files:     dc.b    "Files",0
name_notepad:   dc.b    "Notepad",0
name_music:     dc.b    "Music",0
str_f_hdr:      dc.b    "Name          Size",0
str_f_foot:     dc.b    "Enter: open in Notepad",0
str_n_save:     dc.b    " F1 save",0
str_n_ln:       dc.b    "Ln ",0
str_n_co:       dc.b    " Co ",0
str_n_b:        dc.b    " B",0
str_n_dirty:    dc.b    " *",0
str_m_title:    dc.b    "Canon in D  (Pachelbel)",0
str_m_play:     dc.b    "Space: play/stop",0
demo_text:      dc.b    "UnoDOS/68K milestone 2",13
                dc.b    "The quick brown fox",13
                dc.b    "jumps over the lazy dog.",13
                dc.b    "L04 scroll test",13
                dc.b    "L05 scroll test",13
                dc.b    "L06 scroll test",13
                dc.b    "L07 scroll test",13
                dc.b    "L08 scroll test",13
                dc.b    "L09 scroll test",13
                dc.b    "L10 scroll test",13
                dc.b    "L11 scroll test",13
                dc.b    "L12 scroll test",13
                dc.b    "L13 scroll test",13
                dc.b    "L14 scroll test",13
                dc.b    "L15 scroll test",13
                dc.b    "L16 scroll test",13
                dc.b    "L17 scroll test",13
                dc.b    "L18 last line",0

        even
; app definitions: x, y, w, h, title offset from 'start'
app_def_tab:
        dc.w    24,40,220,90,  str_t_sysinfo-start
        dc.w    90,60,150,70,  str_t_clock-start
        dc.w    16,24,200,150, str_t_files-start
        dc.w    12,14,296,176, str_t_notepad-start
        dc.w    40,42,240,120, str_t_music-start
        dc.w    56,30,270,142, str_t_theme-start
        dc.w    8,12,300,182,  str_t_dostris-start
        dc.w    4,12,310,182,  str_t_outlast-start
        dc.w    4,8,312,190,   str_t_pacman-start

icon_tab:
        dc.l    icon_sysinfo
        dc.l    icon_clock
        dc.l    icon_files
        dc.l    icon_notepad
        dc.l    icon_music
        dc.l    icon_theme
        dc.l    icon_dostris
        dc.l    icon_outlast
        dc.l    icon_pacman
name_tab:
        dc.l    name_sysinfo
        dc.l    name_clock
        dc.l    name_files
        dc.l    name_notepad
        dc.l    name_music
        dc.l    name_theme
        dc.l    name_dostris
        dc.l    name_outlast
        dc.l    name_pacman

; mouse cursor sprite (UnoDOS-style arrow, 14 rows; POS/CTL rewritten live)
        even
cursor_spr:
        dc.w    $2C80,$3A00
        dc.w    %1000000000000000,%0000000000000000
        dc.w    %1100000000000000,%0100000000000000
        dc.w    %1110000000000000,%0110000000000000
        dc.w    %1111000000000000,%0111000000000000
        dc.w    %1111100000000000,%0111100000000000
        dc.w    %1111110000000000,%0111110000000000
        dc.w    %1111111000000000,%0111111000000000
        dc.w    %1111111100000000,%0111111100000000
        dc.w    %1111110000000000,%0111100000000000
        dc.w    %1101100000000000,%0100100000000000
        dc.w    %1000110000000000,%0000010000000000
        dc.w    %0000110000000000,%0000010000000000
        dc.w    %0000011000000000,%0000001000000000
        dc.w    %0000011000000000,%0000001000000000

        even
        include "gen_data.i"

; ---------------------------------------------------------------- variables
        even
vars:
ticks:          dc.l    0
last_secs:      dc.l    0
dbl_tick:       dc.l    0
drag_win:       dc.l    0
mouse_x:        dc.w    160
mouse_y:        dc.w    100
click_x:        dc.w    0
click_y:        dc.w    0
drag_offx:      dc.w    0
drag_offy:      dc.w    0
drag_outx:      dc.w    -1
drag_outy:      dc.w    0
sel_icon:       dc.w    0
dbl_icon:       dc.w    -1
ev_head:        dc.w    0
ev_tail:        dc.w    0
zcount:         dc.w    0
mouse_cntx:     dc.b    0
mouse_cnty:     dc.b    0
mouse_btn:      dc.b    0
click_seq:      dc.b    0
last_seq:       dc.b    0
drag_active:    dc.b    0
        even
files_sel:      dc.w    0
np_len:         dc.w    0
np_caret:       dc.w    0
np_file:        dc.w    -1          ; romdisk index, -1 = untitled
np_top:         dc.w    0           ; first visible line (vertical scroll)
thm_sel:        dc.w    0           ; theme app: selected preset row
thm_slot:       dc.w    0           ; theme app: custom-edit palette slot
        even
theme_pal:      dc.w    $000A,$00AA,$0A0A,$0FFF  ; live palette (Classic VGA)
cop_colptr:     dc.l    0           ; -> COLOR00 reg word in COPLIST
dt_state:       dc.w    0           ; Dostris: 0 menu 1 play 2 pause 3 over
dt_piece:       dc.w    0
dt_rot:         dc.w    0
dt_col:         dc.w    0
dt_row:         dc.w    0
dt_next:        dc.w    0
dt_score:       dc.w    0
dt_lines:       dc.w    0
dt_level:       dc.w    1
        even
dt_last:        dc.l    0
dt_seed:        dc.l    1
ol_state:       dc.w    0           ; OutLast: 0 title 1 play 2 over
ol_x:           dc.w    148
ol_speed:       dc.w    0
ol_score:       dc.w    0
ol_time:        dc.w    60
ol_crash:       dc.w    0
ol_roadl:       dc.w    100
ol_roadr:       dc.w    200
        even
ol_z:           dc.l    0
ol_last:        dc.l    0
ol_lastsec:     dc.l    0
ol_traf0:       dc.l    400
ol_traf1:       dc.l    1600
ol_traf2:       dc.l    800
ol_traf3:       dc.l    2000
gm_notes:       dc.l    0           ; game music: note table ptr
gm_end:         dc.l    0
gm_count:       dc.w    0
gm_ix:          dc.w    0
gm_owner:       dc.w    0
gm_on:          dc.b    0
        even
pm_state:       dc.w    0
pm_x:           dc.w    0
pm_y:           dc.w    0
pm_dir:         dc.w    1
pm_nextdir:     dc.w    1
pm_score:       dc.w    0
pm_hi:          dc.w    0
pm_lives:       dc.w    3
pm_level:       dc.w    1
pm_dots:        dc.w    0
pm_mode:        dc.w    0
pm_modet:       dc.w    0
pm_fright:      dc.w    0
pm_kills:       dc.w    0
pm_tmp:         dc.w    0
        even
pm_last:        dc.l    0
pm_statet:      dc.l    0
np_goal:        dc.w    -1          ; up/down goal column, -1 = none
mus_ix:         dc.w    0
mus_end:        dc.l    0
np_dirty:       dc.b    0
mus_playing:    dc.b    0
        even
evq:            ds.b    EVQ_SIZE*4
zlist:          ds.b    MAXWIN
        even
wintab:         ds.b    MAXWIN*WENT_SIZE
numbuf:         ds.b    16
clkbuf:         ds.b    12
npbuf:          ds.b    NBUF
npline:         ds.b    40
npstat:         ds.b    48
dt_board:       ds.b    200
pm_gh:          ds.b    30          ; 3 ghosts x 5 words
pm_maze:        ds.b    700
        even
pm_old:         ds.w    8           ; pac + 3 ghosts old x,y
        even

        end
