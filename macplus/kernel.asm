; ============================================================================
; UnoDOS/MacPlus - standalone OS for compact 68000 Macs, milestone 1
; ============================================================================
; Bare-metal UnoDOS desktop for the Mac Plus/SE/Classic class (68000,
; 512x342x1 framebuffer, >=1MB RAM). Loaded at KERNBASE by our own boot
; blocks (boot.asm) after the ROM Start Manager bootstraps them -- the ROM
; plays the same role the BIOS does for the x86 reference port. From here
; on UnoDOS owns the machine: its own stack, vectors, interrupt handlers,
; input drivers and renderer. No Toolbox, no Mac OS.
;
; Hardware layer (everything else is the portable UnoDOS core, ported
; from amiga/kernel.asm):
;   - video: 1-bit linear framebuffer via low-mem ScrnBase ($824),
;     512x342, 64 bytes/row, set bit = black. UnoDOS logical colors
;     0-3 map to dither patterns: 0=white 1=25% 2=50% 3=black.
;   - mouse: quadrature. X1/Y1 arrive as SCC DCD-A/DCD-B ext/status
;     interrupts (level 2), X2/Y2 are VIA1 PB4/PB5, button is PB3.
;   - keyboard: M0110/M0110A protocol over the VIA shift register
;     (Instant $14 poll each tick; response = (scan<<1)|1, bit7 = up,
;     $79 = keypad prefix, $7B = null).
;   - tick: VIA CA1 vblank interrupt (level 1), 60.15 Hz.
;   - cursor: software arrow, save-under, erased around every drawing
;     pass in the main loop (ISRs never draw - PORT-SPEC SS6).
;
; Audit-derived rules carried over (PORT-SPEC SS6): ISRs only update
; state, edge-only mouse events, press-time click latching, topmost-only
; periodic content refresh, and find_window_at's explicit tst before rts
; (the 2026-06-12 click-through fix).
; ============================================================================

        mc68000
        org     $20000

; ---------------------------------------------------------------- constants
KERNBASE    equ $20000
STACKTOP    equ $78000              ; our stack, below any screen base

; VIA1 (registers at base + $200*r)
VIA_ORB     equ $EFE1FE             ; r0:  port B (PB3 btn, PB4 X2, PB5 Y2)
VIA_DDRB    equ $EFE5FE             ; r2
VIA_SR      equ $EFF5FE             ; r10: keyboard shift register
VIA_ACR     equ $EFF7FE             ; r11: SR mode in bits 4-2
VIA_PCR     equ $EFF9FE             ; r12
VIA_IFR     equ $EFFBFE             ; r13: bit1 CA1(vblank) bit2 SR
VIA_IER     equ $EFFDFE             ; r14

; SCC (Zilog 8530): reads at even addrs off $9FFFF8, writes off $BFFFF9
SCCR_B      equ $9FFFF8             ; RR0 ch B (mouse Y1 on DCD)
SCCR_A      equ $9FFFFA             ; RR0 ch A (mouse X1 on DCD)
SCCW_B      equ $BFFFF9
SCCW_A      equ $BFFFFB

; low-memory globals the ROM filled in before our boot blocks ran
LM_SCRNBASE equ $824                ; -> top-left of the 1-bit framebuffer
LM_MEMTOP   equ $108                ; physical RAM top

SCRW        equ 512
SCRH        equ 342
ROWB        equ 64                  ; bytes per row

; keyboard protocol
KB_INQUIRY  equ $10                 ; poll for key events (waits ~0.25s)
KB_INSTANT  equ $14                 ; fetch the stashed $79-prefix byte
KB_NULL     equ $7B
KB_PREFIX   equ $79

; events (PORT-SPEC SS3)
EV_KEY      equ 1
EV_MOUSE    equ 4
EVQ_SIZE    equ 32

; window manager
MAXWIN      equ 6
WENT_SIZE   equ 16
WSTATE      equ 0
WPROC       equ 1
WX          equ 2
WY          equ 4
WW          equ 6
WH          equ 8
WTITLE      equ 10

TBAR_H      equ 10
MENUBAR_H   equ 12

TICKS_SEC   equ 60                  ; CA1 vblank rate
DBLCLICK    equ 30                  ; double-click window (0.5s)

ICON0_X     equ 48
ICON0_Y     equ 40
ICON_PITCH  equ 80
NICONS      equ 2                   ; milestone 1: SysInfo, Clock

CURSOR_H    equ 14

; ============================================================================
start:
        bra.w   start2
        dc.b    "UDM1"              ; harness discovery header: magic +
        dc.l    vars                ; the kernel variable block address
start2:
        or.w    #$0700,sr           ; boot blocks ran us in supervisor mode
        lea     STACKTOP,sp

        ; quiet + take over the VIA: SR/CA1 ints only, PB3-5 inputs
        move.b  #$7F,VIA_IER        ; disable all VIA interrupts
        move.b  #$7F,VIA_IFR        ; clear pending flags
        move.b  VIA_DDRB,d0
        and.b   #%11000111,d0       ; PB3 btn, PB4 X2, PB5 Y2 = inputs
        move.b  d0,VIA_DDRB
        move.b  #0,VIA_PCR          ; CA1 = input, negative edge
        move.b  VIA_ACR,d0
        and.b   #%11100011,d0       ; SR mode off until the first poll
        move.b  d0,VIA_ACR

        ; vectors live at address 0 (ROM overlay is long gone by boot time)
        lea     isr_lvl1(pc),a0     ; VIA: vblank tick + keyboard SR
        move.l  a0,$64.w
        lea     isr_lvl2(pc),a0     ; SCC: mouse quadrature
        move.l  a0,$68.w
        lea     berr_h(pc),a0
        move.l  a0,$8.w             ; bus error
        move.l  a0,$C.w             ; address error
        lea     ill_h(pc),a0
        move.l  a0,$10.w            ; illegal instruction

        ; screen base from the ROM's low-mem global (varies with RAM size)
        move.l  LM_SCRNBASE,d0
        lea     vars(pc),a4
        move.l  d0,scrn-vars(a4)
        move.l  $82C,d0             ; baseline RawMouse (absolute fallback)
        move.l  d0,rawm_last-vars(a4)

        ; SCC: enable DCD ext/status interrupts on both channels
        move.b  SCCR_A,d0           ; sync the pointer state
        bsr     scc_init_ch_a
        bsr     scc_init_ch_b
        move.b  #$08,SCCW_A         ; WR9 via either channel: MIE
        nop
        move.b  #$08,SCCW_A
        ; prime the previous-DCD snapshot
        move.b  SCCR_A,d0
        lsr.b   #3,d0
        and.b   #1,d0
        move.b  d0,scc_xprev-vars(a4)
        move.b  SCCR_B,d0
        lsr.b   #3,d0
        and.b   #1,d0
        move.b  d0,scc_yprev-vars(a4)

        bsr     clear_screen        ; desktop gray
        bsr     draw_desktop

        move.b  #$86,VIA_IER        ; enable CA1 (bit1) + SR (bit2)
        and.w   #$F8FF,sr           ; allow interrupts (stay supervisor)

        ifd     AUTOTEST
        moveq   #0,d0               ; SysInfo (bottom)
        bsr     launch_app
        moveq   #1,d0               ; Clock (topmost)
        bsr     launch_app
        endc

; ============================================================================
; Main loop - single cooperative context. All input decisions live here,
; never in ISRs. The cursor is software: erase before any pass that can
; draw, redraw after (the saved under-image stays valid by construction).
; ============================================================================
main_loop:
        ifd     AUTOTEST
        ; debug: show the last raw keyboard byte top-right in the menu bar
        move.b  kb_dbg(pc),d0
        cmp.b   kb_dbgs(pc),d0
        beq     .nodbg
        lea     vars(pc),a4
        move.b  d0,kb_dbgs-vars(a4)
        lea     numbuf(pc),a0
        move.b  d0,d1
        lsr.b   #4,d1
        bsr     hexdig
        move.b  d1,(a0)
        move.b  d0,d1
        and.b   #$F,d1
        bsr     hexdig
        move.b  d1,1(a0)
        clr.b   2(a0)
        move.w  #SCRW-20,d0
        moveq   #2,d1
        moveq   #3,d2
        moveq   #0,d3
        bsr     draw_string_bg
.nodbg:
        endc
        move.b  click_seq(pc),d0
        cmp.b   last_seq(pc),d0
        bne     .work
        move.b  drag_active(pc),d0
        bne     .work
        move.w  ev_head(pc),d0
        cmp.w   ev_tail(pc),d0
        bne     .work
        move.l  ticks(pc),d0
        sub.l   last_secs(pc),d0
        cmp.l   #TICKS_SEC,d0
        bge     .work
        move.w  mouse_x(pc),d0
        cmp.w   cur_x(pc),d0
        bne     .move
        move.w  mouse_y(pc),d0
        cmp.w   cur_y(pc),d0
        bne     .move
        bra     main_loop
.move:  bsr     cur_erase
        bsr     cur_draw
        bra     main_loop
.work:  bsr     cur_erase
        bsr     handle_clicks
        bsr     handle_drag
        bsr     handle_events
        bsr     app_ticks
        bsr     cur_draw
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
        lsr.w   #8,d2               ; d2 = raw (canonical) code
        and.w   #$FF,d1             ; d1 = ascii (0 if none)
        move.w  zcount(pc),d3
        beq     .desktop
        cmp.b   #27,d1              ; ESC ('`' on the M0110) closes topmost
        bne     .focus
        bsr     close_topmost
        bra     .next
.focus: ; milestone 1: no app key handlers yet (SysInfo/Clock are static)
        bra     .next
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
        beq     .out                ; topmost body: the app's business (M2+)
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
        bsr     raise_window
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

erase_outline:
        move.w  drag_outx(pc),d0
        bmi     eo_skip
        move.w  drag_outy(pc),d1
        bra     do_outline
eo_skip:
        rts
draw_outline:
do_outline:
        move.l  drag_win(pc),a2
        move.w  WW(a2),d2
        move.w  WH(a2),d3
        bsr     xor_rect
        rts

; ----------------------------------------------------------------------------
; app_ticks - once a second, refresh the TOPMOST window's content
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
        bsr     app_draw_content
        rts

; ============================================================================
; Desktop
; ============================================================================
draw_desktop:
        ; white menu bar with a black underline (classic compact-Mac look)
        moveq   #0,d0
        moveq   #0,d1
        move.w  #SCRW,d2
        moveq   #MENUBAR_H-1,d3
        moveq   #0,d4
        bsr     fill_rect
        moveq   #0,d0
        moveq   #MENUBAR_H-1,d1
        move.w  #SCRW,d2
        moveq   #1,d3
        moveq   #3,d4
        bsr     fill_rect
        lea     str_menutitle(pc),a0
        moveq   #4,d0
        moveq   #2,d1
        moveq   #3,d2
        bsr     draw_string
        ; footer: version (left), build (right) on the gray desktop
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

; icon_pos - d0 = icon index -> d0 = x, d1 = y (5 per row)
icon_pos:
        move.l  d2,-(sp)
        moveq   #0,d2
        move.w  d0,d2
        divu    #5,d2
        move.w  d2,d1
        mulu    #48,d1
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
        ; clear the cell (white box on the gray desktop)
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
        ; label
        move.w  d0,-(sp)
        move.w  d1,-(sp)
        move.w  d7,d2
        lsl.w   #2,d2
        lea     name_tab(pc),a0
        move.l  (a0,d2.w),a0
        subq.w  #8,d0
        add.w   #24,d1
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
        bsr     rect_outline_fg
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
        move.w  d0,d3
        move.w  d1,d4
        moveq   #0,d7
.try:   move.w  d7,d0
        bsr     icon_pos
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
        add.w   #36,d5
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
        bsr     win_ptr_raw
        tst.b   WSTATE(a2)
        beq     .next
        moveq   #0,d0
        move.b  WPROC(a2),d0
        cmp.w   d7,d0
        bne     .next
        bsr     z_index_of
        bmi     .next
        bsr     raise_window
        rts
.next:  addq.w  #1,d6
        cmp.w   #MAXWIN,d6
        blt     .scan
        move.w  d7,d0
        mulu    #10,d0
        lea     app_def_tab(pc),a0
        lea     (a0,d0.w),a0
        move.w  (a0)+,d1            ; x
        move.w  (a0)+,d2            ; y
        move.w  (a0)+,d3            ; w
        move.w  (a0)+,d4            ; h
        move.w  (a0),d5             ; title offset from 'start'
        lea     start(pc),a1
        add.w   d5,a1
        move.w  d7,d0
        bsr     win_create
        rts

; ============================================================================
; Window manager (portable core, verbatim from the Amiga port)
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
        ; a second-or-later window deactivates the previous topmost
        ; (its title stripes must go), so repaint everything then
        cmp.w   #1,d1
        beq     .first
        bsr     repaint_all
        rts
.first: bsr     draw_window
        rts

; win_ptr_raw - d2 = table index -> a2. Preserves d0-d7/a0-a1.
win_ptr_raw:
        move.w  d2,-(sp)
        lsl.w   #4,d2
        lea     wintab(pc),a2
        lea     (a2,d2.w),a2
        move.w  (sp)+,d2
        rts

; zwin_ptr - d2 = z index -> a2. Preserves ALL data registers.
zwin_ptr:
        movem.l d2/a0,-(sp)
        lea     zlist(pc),a0
        and.w   #$FF,d2
        move.b  (a0,d2.w),d2
        and.w   #$FF,d2
        lsl.w   #4,d2
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
        lsr.w   #4,d0
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
        bsr     zwin_ptr
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
.out:   tst.w   d2                  ; N must mirror the result: the hit
        rts                         ; path falls in with stale cmp flags
.next:  subq.w  #1,d2
        bra     .scan

; raise_window - d0 = z index: move to top of zlist, repaint if changed
raise_window:
        move.w  zcount(pc),d1
        subq.w  #1,d1
        cmp.w   d1,d0
        bne     .doit
        rts
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

; draw_window - a2 = window entry. Classic Mac chrome: 1px-offset drop
; shadow, white title bar with pinstripes when active (topmost), centered
; title on a white patch, square close box (hit region: rightmost 12px).
draw_window:
        move.l  a2,-(sp)
        ; drop shadow: right edge (clipped to the screen)
        move.w  WX(a2),d0
        add.w   WW(a2),d0
        cmp.w   #SCRW,d0
        bge     .nrsh
        move.w  WY(a2),d1
        addq.w  #1,d1
        move.w  WH(a2),d3
        move.w  d1,d4
        add.w   d3,d4
        cmp.w   #SCRH,d4
        ble     .rshok
        move.w  #SCRH,d3
        sub.w   d1,d3
        ble     .nrsh
.rshok: moveq   #1,d2
        moveq   #3,d4
        bsr     fill_rect
        move.l  (sp),a2
.nrsh:  ; drop shadow: bottom edge
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        cmp.w   #SCRH,d1
        bge     .nbsh
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WW(a2),d2
        move.w  d0,d4
        add.w   d2,d4
        cmp.w   #SCRW,d4
        ble     .bshok
        move.w  #SCRW,d2
        sub.w   d0,d2
        ble     .nbsh
.bshok: moveq   #1,d3
        moveq   #3,d4
        bsr     fill_rect
        move.l  (sp),a2
.nbsh:  ; content background (white)
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
        ; title bar: white, with a black separator under it
        move.w  WX(a2),d0
        move.w  WY(a2),d1
        move.w  WW(a2),d2
        moveq   #TBAR_H,d3
        moveq   #0,d4
        bsr     fill_rect
        move.l  (sp),a2
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H-1,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        moveq   #1,d3
        moveq   #3,d4
        bsr     fill_rect
        move.l  (sp),a2
        ; border
        move.w  WX(a2),d0
        move.w  WY(a2),d1
        move.w  WW(a2),d2
        move.w  WH(a2),d3
        bsr     rect_outline_fg
        move.l  (sp),a2
        ; pinstripes, only when active (= topmost in the z list)
        move.w  zcount(pc),d2
        subq.w  #1,d2
        bmi     .inactive
        move.l  a2,d5
        bsr     zwin_ptr            ; a2 = topmost (preserves data regs)
        move.l  a2,d6
        move.l  d5,a2
        cmp.l   d5,d6
        bne     .inactive
        moveq   #2,d6               ; rows y+2, y+4, y+6, y+8
.stripe:
        move.w  WX(a2),d0
        addq.w  #3,d0
        move.w  WY(a2),d1
        add.w   d6,d1
        move.w  WW(a2),d2
        subq.w  #6,d2
        moveq   #1,d3
        moveq   #3,d4
        bsr     fill_rect
        addq.w  #2,d6
        cmp.w   #TBAR_H-1,d6
        blt     .stripe
.inactive:
        ; close box: white patch + empty square (classic Mac)
        move.w  WX(a2),d0
        add.w   WW(a2),d0
        sub.w   #13,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        moveq   #11,d2
        moveq   #TBAR_H-2,d3
        moveq   #0,d4
        bsr     fill_rect
        move.l  (sp),a2
        move.w  WX(a2),d0
        add.w   WW(a2),d0
        sub.w   #11,d0
        move.w  WY(a2),d1
        addq.w  #2,d1
        moveq   #7,d2
        moveq   #7,d3
        bsr     rect_outline_fg
        move.l  (sp),a2
        ; centered title on a white patch (kills stripes behind it)
        move.l  WTITLE(a2),a0
        bsr     str_len             ; d0 = chars (preserves a0)
        lsl.w   #3,d0               ; *8 px
        move.w  WW(a2),d1
        sub.w   d0,d1
        lsr.w   #1,d1               ; (w - len*8)/2
        move.w  d0,d2               ; text width
        move.w  WX(a2),d0
        add.w   d1,d0
        move.l  a0,-(sp)
        subq.w  #4,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        addq.w  #8,d2               ; patch = text + 4px each side
        moveq   #TBAR_H-2,d3
        moveq   #0,d4
        bsr     fill_rect
        move.l  (sp)+,a0
        addq.w  #4,d0
        moveq   #3,d2
        bsr     draw_string
        move.l  (sp),a2
        ; content
        moveq   #0,d0
        move.b  WPROC(a2),d0
        bsr     app_draw_content
        move.l  (sp)+,a2
        rts

; str_len - a0 = NUL string -> d0 = length. Preserves a0.
str_len:
        move.l  a0,-(sp)
        moveq   #0,d0
.scan:  tst.b   (a0)+
        beq     .done
        addq.w  #1,d0
        bra     .scan
.done:  move.l  (sp)+,a0
        rts

; ============================================================================
; Apps (in-kernel window procs, milestone 1)
; ============================================================================

; app_draw_content - d0 = proc index, a2 = window
app_draw_content:
        move.l  a2,-(sp)
        tst.w   d0
        beq     .sysinfo
        bsr     clock_draw
        bra     .done
.sysinfo:
        bsr     sysinfo_draw
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
        ; "RAM     xxxx KB" from the ROM's MemTop global
        lea     str_si3(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   #20,d1
        moveq   #3,d2
        bsr     draw_string
        move.l  LM_MEMTOP,d0
        moveq   #10,d1
        lsr.l   d1,d0               ; KB
        lea     numbuf(pc),a0
        bsr     fmt_u16
        lea     numbuf(pc),a0
.kfix:  cmp.b   #'s',(a0)+          ; reuse the buffer: "1024s" -> "1024K"
        bne     .kfix
        move.b  #'K',-1(a0)
        lea     numbuf(pc),a0
        move.w  d6,d0
        add.w   #64,d0
        move.w  d5,d1
        add.w   #20,d1
        moveq   #3,d2
        moveq   #0,d3
        bsr     draw_string_bg
        lea     str_si4(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   #30,d1
        moveq   #3,d2
        bsr     draw_string
        ; uptime seconds (fixed-width overwrite)
        move.l  ticks(pc),d0
        divu    #TICKS_SEC,d0
        lea     numbuf(pc),a0
        bsr     fmt_u16
        lea     numbuf(pc),a0
        move.w  d6,d0
        add.w   #64,d0
        move.w  d5,d1
        add.w   #30,d1
        moveq   #3,d2
        moveq   #0,d3
        bsr     draw_string_bg
        lea     str_si5(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   #44,d1
        moveq   #3,d2
        bsr     draw_string
        rts

; ---- Clock (uptime as HH:MM:SS) ----
clock_draw:
        move.l  ticks(pc),d0
        divu    #TICKS_SEC,d0
        and.l   #$FFFF,d0
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
        lea     str_uptime(pc),a0
        move.w  WX(a2),d0
        addq.w  #6,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+4,d1
        moveq   #3,d2
        bsr     draw_string
        move.w  WX(a2),d0
        move.w  WW(a2),d1
        lsr.w   #1,d1
        add.w   d1,d0
        sub.w   #32,d0
        move.w  WY(a2),d1
        move.w  WH(a2),d3
        lsr.w   #1,d3
        add.w   d3,d1
        subq.w  #2,d1
        lea     clkbuf(pc),a0
        moveq   #3,d2
        moveq   #0,d3
        bsr     draw_string_bg
        rts

; hexdig - d1.b (0-15) -> ASCII hex digit in d1
hexdig:
        and.b   #$F,d1
        cmp.b   #10,d1
        blt     .num
        add.b   #'A'-10-'0',d1
.num:   add.b   #'0',d1
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
        lea     8(a0),a1
        clr.b   -(a1)
.dig:   divu    #10,d0
        swap    d0
        add.b   #'0',d0
        move.b  d0,-(a1)
        clr.w   d0
        swap    d0
        tst.w   d0
        bne     .dig
.copy:  move.b  (a1)+,(a0)+
        bne     .copy
        subq.l  #1,a0
        move.b  #'s',(a0)+
        move.b  #' ',(a0)+
        clr.b   (a0)
        rts

; ============================================================================
; Graphics primitives (1 bitplane, 512x342, set bit = black)
; ============================================================================
; UnoDOS logical colors -> row-dither patterns (2 bytes, even/odd rows):
;   0 = white, 1 = 25% gray, 2 = 50% gray (the desktop), 3 = black
pat_tab:
        dc.b    $00,$00             ; 0: white
        dc.b    $88,$22             ; 1: light gray
        dc.b    $AA,$55             ; 2: medium gray
        dc.b    $FF,$FF             ; 3: black

; clear_screen - desktop gray (50% dither, phase-locked to absolute y)
clear_screen:
        movem.l d0-d3/a0,-(sp)
        move.l  scrn(pc),a0
        move.w  #SCRH/2-1,d0
        move.l  #$AAAAAAAA,d1
        move.l  #$55555555,d2
.rows:  moveq   #ROWB/4-1,d3
.r0:    move.l  d1,(a0)+
        dbra    d3,.r0
        moveq   #ROWB/4-1,d3
.r1:    move.l  d2,(a0)+
        dbra    d3,.r1
        dbra    d0,.rows
        movem.l (sp)+,d0-d3/a0
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
        ; a3 = byte address of (x,y); a1 = pattern pair for this color
        and.w   #3,d4
        add.w   d4,d4
        lea     pat_tab(pc),a1
        lea     (a1,d4.w),a1
        move.w  d1,d4               ; d4 = y (row parity tracker)
        move.w  d1,d6
        mulu    #ROWB,d6
        move.w  d0,d1
        lsr.w   #3,d1
        and.l   #$FFFF,d1
        add.l   d1,d6
        add.l   scrn(pc),d6
        move.l  d6,a3
        move.w  d0,d6
        add.w   d2,d6
        subq.w  #1,d6               ; d6 = xend
        move.w  d6,d2
        lsr.w   #3,d2
        sub.w   d1,d2               ; d2 = span-1 in bytes
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
.multi: subq.w  #1,d3               ; dbra row count
.row:   ; d6 = pattern byte for this row
        move.w  d4,d0
        and.w   #1,d0
        move.b  (a1,d0.w),d6
        move.l  a3,a2
        move.b  (a2),d0
        eor.b   d6,d0
        and.b   d5,d0
        eor.b   d0,(a2)             ; left partial: b ^= ((b^fill)&mask)
        tst.w   d2
        beq     .next
        move.b  (a2,d2.w),d0
        eor.b   d6,d0
        and.b   d7,d0
        eor.b   d0,(a2,d2.w)        ; right partial
        move.w  d2,d0
        subq.w  #1,d0
        beq     .next
        subq.w  #1,d0
        lea     1(a2),a0
.mid:   move.b  d6,(a0)+
        dbra    d0,.mid
.next:  lea     ROWB(a3),a3
        addq.w  #1,d4
        dbra    d3,.row
fr_out: rts

; rect_outline_fg - d0=x d1=y d2=w d3=h (1px black frame)
rect_outline_fg:
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

; xor_rect - d0=x d1=y d2=w d3=h: invert a 1px outline (drag rubber band;
; sides inset so corners XOR exactly once)
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
        movem.l d0-d7/a0-a2,-(sp)
        bsr     xor_span_raw
        movem.l (sp)+,d0-d7/a0-a2
        rts
xor_span_raw:
        tst.w   d2
        ble     xs_out
        tst.w   d3
        ble     xs_out
        move.w  d0,d6
        add.w   d2,d6
        subq.w  #1,d6               ; d6 = xend
        move.w  d1,d5
        mulu    #ROWB,d5
        add.l   scrn(pc),d5
        move.l  d5,a0
        move.w  d0,d5
        lsr.w   #3,d5
        add.w   d5,a0
        move.w  d6,d2
        lsr.w   #3,d2
        sub.w   d5,d2               ; d2 = span-1 in bytes
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
        eor.b   d5,(a2)
        tst.w   d2
        beq     .next
        move.w  d2,d1
        subq.w  #1,d1
        ble     .rightset
        subq.w  #1,d1
        lea     1(a2),a2
.mid:   not.b   (a2)+
        dbra    d1,.mid
        bra     .right
.rightset:
        lea     1(a2),a2
.right: eor.b   d7,(a2)
.next:  lea     ROWB(a0),a0
        dbra    d3,.row
xs_out: rts

; draw_icon16 - a0 = planar icon (16 words plane0, then 16 words plane1),
;               d0 = x (multiple of 8), d1 = y
; Mono mapping: ink = plane0 XOR plane1 (UnoDOS pixel 1/2 = midtones ->
; black; pixel 3 = white highlight stays paper; pixel 0 = background).
draw_icon16:
        movem.l d0-d5/a0-a4,-(sp)
        move.l  a0,a4
        move.w  d1,d5
        mulu    #ROWB,d5
        add.l   scrn(pc),d5
        move.l  d5,a3
        move.w  d0,d5
        lsr.w   #3,d5
        add.w   d5,a3
        lea     32(a4),a1           ; plane1 words
        moveq   #15,d2
.row:   move.w  (a4)+,d3
        move.w  (a1)+,d4
        eor.w   d4,d3
        move.b  d3,1(a3)
        lsr.w   #8,d3
        move.b  d3,(a3)
        lea     ROWB(a3),a3
        dbra    d2,.row
        movem.l (sp)+,d0-d5/a0-a4
        rts

; ----------------------------------------------------------------------------
; draw_char - d0=x d1=y d2=char d3=fg(0-3) d4=bg(0-3, or -1 = transparent)
; For legibility at 8px, nonzero fg always renders as ink (black); bg
; renders as its honest dither. Unaligned: 2-byte RMW per row.
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
        ; a3 = screen address of (x,y)
        move.w  d1,d2               ; d2 = y (row parity)
        mulu    #ROWB,d1
        move.w  d0,d5
        lsr.w   #3,d5
        and.l   #$FFFF,d5
        add.l   d5,d1
        add.l   scrn(pc),d1
        move.l  d1,a3
        move.w  d0,d6
        and.w   #7,d6               ; d6 = shift
        ; fg ink word repeated: d3 = 0 -> no ink, else ink
        tst.w   d3
        beq     .fgw
        moveq   #-1,d3              ; ink
.fgw:   ; a0 = bg pattern pair (or bg < 0 = transparent)
        tst.w   d4
        bmi     .rows
        and.w   #3,d4
        add.w   d4,d4
        lea     pat_tab(pc),a0
        lea     (a0,d4.w),a0
.rows:  moveq   #7,d7               ; 8 rows
.row:
        moveq   #0,d1
        move.b  (a4)+,d1
        lsl.w   #8,d1
        lsr.w   d6,d1               ; d1 = glyph bits in 16px window
        move.w  #$FF00,d5
        lsr.w   d6,d5               ; d5 = coverage mask
        ; value = (fg ? glyph : 0) | (bgpat & cover & ~glyph)
        move.w  d1,d0
        and.w   d3,d0               ; fg contribution
        tst.w   d4
        bmi     .put                ; transparent: OR ink only
        ; bg pattern byte for this row, spread to a word
        movem.l d2-d3,-(sp)
        move.w  d2,d3
        and.w   #1,d3
        moveq   #0,d2
        move.b  (a0,d3.w),d2
        move.b  d2,d3
        lsl.w   #8,d2
        move.b  d3,d2               ; d2 = pattern word
        and.w   d5,d2
        move.w  d1,d3
        not.w   d3
        and.w   d3,d2               ; bg & cover & ~glyph
        or.w    d2,d0
        movem.l (sp)+,d2-d3
        ; word = (word & ~coverage) | value
        move.w  d5,d1
        not.w   d1
        and.b   d1,1(a3)
        ror.w   #8,d1
        and.b   d1,(a3)
        move.w  d0,d1
        or.b    d1,1(a3)
        ror.w   #8,d1
        or.b    d1,(a3)
        bra     .nrow
.put:   move.w  d0,d1               ; transparent: OR ink bits only
        or.b    d1,1(a3)
        ror.w   #8,d1
        or.b    d1,(a3)
.nrow:  lea     ROWB(a3),a3
        addq.w  #1,d2
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
        addq.w  #8,d0
        cmp.w   #SCRW-8,d0
        ble     .ch
.done:  movem.l (sp)+,d0-d6/a0
        rts

; ============================================================================
; Software cursor - save-under arrow, drawn ONLY from the main loop.
; cur_erase before any pass that may draw keeps the under-image valid.
; ============================================================================

; cur_erase - restore the saved under-image (no-op when not visible)
cur_erase:
        movem.l d0-d2/a0-a1,-(sp)
        move.b  cur_vis(pc),d0
        beq     .out
        lea     vars(pc),a4
        sf      cur_vis-vars(a4)
        move.w  cur_py(pc),d0
        mulu    #ROWB,d0
        add.l   scrn(pc),d0
        move.l  d0,a0
        move.w  cur_px(pc),d0
        lsr.w   #3,d0
        add.w   d0,a0
        lea     cur_save(pc),a1
        moveq   #CURSOR_H-1,d1
.row:   move.b  (a1)+,(a0)
        move.b  (a1)+,1(a0)
        move.b  (a1)+,2(a0)
        lea     ROWB(a0),a0
        dbra    d1,.row
.out:   movem.l (sp)+,d0-d2/a0-a1
        rts

; cur_draw - save under-image at the (clamped) mouse position, then
; paint: black = data rows, white halo = mask & ~data.
cur_draw:
        movem.l d0-d7/a0-a3,-(sp)
        lea     vars(pc),a4
        move.w  mouse_x(pc),d0
        move.w  d0,cur_x-vars(a4)
        move.w  mouse_y(pc),d1
        move.w  d1,cur_y-vars(a4)
        cmp.w   #SCRW-17,d0         ; clamp the drawn box on-screen
        ble     .xok
        move.w  #SCRW-17,d0
.xok:   cmp.w   #SCRH-CURSOR_H-1,d1
        ble     .yok
        move.w  #SCRH-CURSOR_H-1,d1
.yok:   move.w  d0,cur_px-vars(a4)
        move.w  d1,cur_py-vars(a4)
        ; a0 = screen byte, d6 = bit shift
        mulu    #ROWB,d1
        add.l   scrn(pc),d1
        move.l  d1,a0
        move.w  d0,d5
        lsr.w   #3,d5
        add.w   d5,a0
        and.w   #7,d0
        moveq   #8,d6
        sub.w   d0,d6               ; d6 = 8 - (x&7): aligns a 16-bit row
                                    ; into bits 23..8 of a 24-bit window
        lea     cur_save(pc),a1
        lea     cursor_mask(pc),a2
        lea     cursor_data(pc),a3
        st      cur_vis-vars(a4)
        moveq   #CURSOR_H-1,d7
.row:   ; save under
        move.b  (a0),(a1)+
        move.b  1(a0),(a1)+
        move.b  2(a0),(a1)+
        ; build 24-bit window in d0
        moveq   #0,d0
        move.b  (a0),d0
        lsl.l   #8,d0
        move.b  1(a0),d0
        lsl.l   #8,d0
        move.b  2(a0),d0
        ; mask/data aligned
        moveq   #0,d1
        move.w  (a2)+,d1
        lsl.l   d6,d1               ; mask
        moveq   #0,d2
        move.w  (a3)+,d2
        lsl.l   d6,d2               ; data (black bits)
        not.l   d1
        and.l   d1,d0               ; clear masked area to white
        or.l    d2,d0               ; ink
        ; store 3 bytes back
        move.b  d0,2(a0)
        lsr.l   #8,d0
        move.b  d0,1(a0)
        lsr.l   #8,d0
        move.b  d0,(a0)
        lea     ROWB(a0),a0
        dbra    d7,.row
        movem.l (sp)+,d0-d7/a0-a3
        rts

; classic arrow: mask = full shape incl. 1px white halo, data = black body
cursor_mask:
        dc.w    %1000000000000000
        dc.w    %1100000000000000
        dc.w    %1110000000000000
        dc.w    %1111000000000000
        dc.w    %1111100000000000
        dc.w    %1111110000000000
        dc.w    %1111111000000000
        dc.w    %1111111100000000
        dc.w    %1111110000000000
        dc.w    %1101100000000000
        dc.w    %1000110000000000
        dc.w    %0000110000000000
        dc.w    %0000011000000000
        dc.w    %0000011000000000
cursor_data:
        dc.w    %0000000000000000
        dc.w    %0100000000000000
        dc.w    %0110000000000000
        dc.w    %0111000000000000
        dc.w    %0111100000000000
        dc.w    %0111110000000000
        dc.w    %0111111000000000
        dc.w    %0111111100000000
        dc.w    %0111100000000000
        dc.w    %0100100000000000
        dc.w    %0000010000000000
        dc.w    %0000010000000000
        dc.w    %0000001000000000
        dc.w    %0000001000000000

; ============================================================================
; Interrupt handlers - state only, no drawing (PORT-SPEC SS6 rule 2)
; ============================================================================

; level 1: VIA - CA1 = vblank tick, SR = keyboard shift register
isr_lvl1:
        movem.l d0-d3/a0,-(sp)
        move.b  VIA_IFR,d0
        btst    #1,d0               ; CA1: vblank
        beq     .trysr
        move.b  #$02,VIA_IFR        ; ack CA1
        lea     vars(pc),a0
        addq.l  #1,ticks-vars(a0)
        ; left button: PB3, active low. Edge-only events + press latch.
        moveq   #0,d1
        btst    #3,VIA_ORB
        bne     .btn
        moveq   #1,d1
.btn:   move.b  mouse_btn(pc),d2
        move.b  d1,mouse_btn-vars(a0)
        ; absolute-mouse fallback: emulators (Mini vMac) inject host mouse
        ; motion by writing low-mem RawMouse ($82C, Point = v.w,h.w)
        ; instead of synthesizing quadrature. On real hardware we own the
        ; SCC vectors so RawMouse goes stale -> this path is inert there.
        move.l  $82C,d3
        cmp.l   rawm_last(pc),d3
        beq     .nabs
        move.l  d3,rawm_last-vars(a0)
        move.w  d3,d0               ; h (x)
        bge     .axp
        moveq   #0,d0
.axp:   cmp.w   #SCRW-1,d0
        ble     .axok
        move.w  #SCRW-1,d0
.axok:  move.w  d0,mouse_x-vars(a0)
        swap    d3
        move.w  d3,d0               ; v (y)
        bge     .ayp
        moveq   #0,d0
.ayp:   cmp.w   #SCRH-1,d0
        ble     .ayok
        move.w  #SCRH-1,d0
.ayok:  move.w  d0,mouse_y-vars(a0)
.nabs:
        cmp.b   d2,d1
        beq     .kbd
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
.kbd:   ; keyboard poll state machine kick + watchdog
        move.b  kb_state(pc),d0
        beq     .poll
        move.l  ticks(pc),d1
        sub.l   kb_t0(pc),d1
        cmp.l   #TICKS_SEC,d1       ; stuck for a second: restart
        blt     .trysr
        lea     vars(pc),a0
        clr.b   kb_state-vars(a0)
        bra     .trysr
.poll:  lea     vars(pc),a0
        move.l  ticks(pc),d1
        move.l  d1,kb_t0-vars(a0)
        move.b  #1,kb_state-vars(a0)
        ; M0110 send: attention first - SR mode 110 (shift out under the
        ; system clock) with 0 holds the data line low, telling the
        ; keyboard a command is coming; then load the real command in
        ; mode 111 (shift out under the keyboard's clock).
        move.b  VIA_ACR,d0
        and.b   #%11100011,d0
        or.b    #%00011000,d0
        move.b  d0,VIA_ACR
        move.b  #0,VIA_SR           ; data line low (attention)
        or.b    #%00011100,d0
        move.b  d0,VIA_ACR
        moveq   #KB_INQUIRY,d1      ; normal poll: Inquiry
        move.b  kb_prefix(pc),d0
        beq     .pcmd
        moveq   #KB_INSTANT,d1      ; $79 seen: Instant fetches the stash
.pcmd:  move.b  d1,VIA_SR
.trysr:
        move.b  VIA_IFR,d0
        btst    #2,d0               ; SR: shift complete
        beq     .done
        move.b  kb_state(pc),d0
        cmp.b   #1,d0
        beq     .sent
        cmp.b   #2,d0
        bne     .flush
        ; response byte ready
        move.b  VIA_SR,d1           ; read clears the SR flag
        lea     vars(pc),a0
        clr.b   kb_state-vars(a0)
        bsr     kb_byte
        bra     .done
.sent:  ; command went out: switch SR to input for the response
        move.b  VIA_SR,d1           ; clear flag (discard)
        move.b  VIA_ACR,d0          ; SR mode 011: shift in, ext clock
        and.b   #%11100011,d0
        or.b    #%00001100,d0
        move.b  d0,VIA_ACR
        lea     vars(pc),a0
        move.b  #2,kb_state-vars(a0)
        bra     .done
.flush: move.b  VIA_SR,d1           ; unexpected: clear and drop
.done:  movem.l (sp)+,d0-d3/a0
        rte

; kb_byte - d1.b = keyboard response (ISR context)
; (scan<<1)|1, bit7 = up. $7B null, $79 keypad/arrow prefix.
kb_byte:
        movem.l d0-d3/a0-a1,-(sp)
        lea     vars(pc),a1
        ifd     AUTOTEST
        cmp.b   #KB_NULL,d1
        beq     .ndbg
        move.b  d1,kb_dbg-vars(a1)  ; surface non-null wire bytes
.ndbg:
        endc
        cmp.b   #KB_NULL,d1
        bne     .nn
        sf      kb_prefix-vars(a1)  ; a null Instant reply = stash lost
        bra     .out
.nn:
        cmp.b   #KB_PREFIX,d1
        bne     .key
        st      kb_prefix-vars(a1)
        bra     .out
.key:   move.b  d1,d2
        btst    #7,d2               ; key-up: consume the prefix, ignore
        beq     .down
        sf      kb_prefix-vars(a1)
        bra     .out
.down:  lsr.b   #1,d2
        and.w   #$3F,d2
        move.b  kb_prefix(pc),d0
        beq     .map
        or.w    #$40,d2             ; keypad/arrow page
        sf      kb_prefix-vars(a1)
.map:   lea     kb_ascii(pc),a0
        move.b  (a0,d2.w),d3        ; ascii (0 if none)
        lea     kb_raw(pc),a0
        moveq   #0,d0
        move.b  (a0,d2.w),d0        ; canonical UnoDOS raw code
        lsl.w   #8,d0
        or.b    d3,d0
        tst.w   d0                  ; or.b only set flags for the low byte
        beq     .out                ; (the raw code lives in the high byte)
        move.w  d0,d1
        moveq   #EV_KEY,d0
        bsr     ev_post
.out:   movem.l (sp)+,d0-d3/a0-a1
        rts

; level 2: SCC - mouse quadrature (X1 = DCD-A, Y1 = DCD-B)
isr_lvl2:
        movem.l d0-d3/a0,-(sp)
        lea     vars(pc),a0
        ; ---- X axis: RR0 channel A bit 3
        move.b  SCCR_A,d0
        lsr.b   #3,d0
        and.w   #1,d0
        move.b  scc_xprev(pc),d1
        cmp.b   d0,d1
        beq     .ydcd
        move.b  d0,scc_xprev-vars(a0)
        move.b  #$10,SCCW_A         ; WR0: reset ext/status latches
        ; direction: X1 vs X2 (VIA PB4)
        moveq   #0,d1
        btst    #4,VIA_ORB
        beq     .x2c
        moveq   #1,d1
.x2c:   move.w  mouse_x(pc),d2
        eor.b   d1,d0
        bne     .xneg
        addq.w  #1,d2
        bra     .xclamp
.xneg:  subq.w  #1,d2
.xclamp:
        tst.w   d2
        bge     .xlo
        moveq   #0,d2
.xlo:   cmp.w   #SCRW-1,d2
        ble     .xok
        move.w  #SCRW-1,d2
.xok:   move.w  d2,mouse_x-vars(a0)
.ydcd:  ; ---- Y axis: RR0 channel B bit 3
        move.b  SCCR_B,d0
        lsr.b   #3,d0
        and.w   #1,d0
        move.b  scc_yprev(pc),d1
        cmp.b   d0,d1
        beq     .ack
        move.b  d0,scc_yprev-vars(a0)
        move.b  #$10,SCCW_B
        moveq   #0,d1
        btst    #5,VIA_ORB          ; Y2 = PB5
        beq     .y2c
        moveq   #1,d1
.y2c:   move.w  mouse_y(pc),d2
        eor.b   d1,d0
        bne     .yneg
        addq.w  #1,d2
        bra     .yclamp
.yneg:  subq.w  #1,d2
.yclamp:
        tst.w   d2
        bge     .ylo
        moveq   #0,d2
.ylo:   cmp.w   #SCRH-1,d2
        ble     .yok
        move.w  #SCRH-1,d2
.yok:   move.w  d2,mouse_y-vars(a0)
.ack:   move.b  #$38,SCCW_A         ; WR0: reset highest IUS
        movem.l (sp)+,d0-d3/a0
        rte

; scc_init_ch_x - WR15 = $08 (DCD IE), WR1 = $01 (ext int enable)
scc_init_ch_a:
        move.b  #15,SCCW_A
        nop
        move.b  #$08,SCCW_A
        nop
        move.b  #1,SCCW_A
        nop
        move.b  #$01,SCCW_A
        nop
        move.b  #$10,SCCW_A         ; reset ext/status (twice: it latches)
        nop
        move.b  #$10,SCCW_A
        rts
scc_init_ch_b:
        move.b  #15,SCCW_B
        nop
        move.b  #$08,SCCW_B
        nop
        move.b  #1,SCCW_B
        nop
        move.b  #$01,SCCW_B
        nop
        move.b  #$10,SCCW_B
        nop
        move.b  #$10,SCCW_B
        rts

; ============================================================================
; Fault handlers - black screen, white PC dump, halt
; ============================================================================
berr_h:
        move.l  10(sp),d6           ; bus/address error frame: PC at +10
        bra     fault_show
ill_h:
        move.l  2(sp),d6            ; group-2 frame: PC at +2
fault_show:
        or.w    #$0700,sr
        moveq   #0,d0
        moveq   #0,d1
        move.w  #SCRW,d2
        move.w  #SCRH,d3
        moveq   #3,d4
        bsr     fill_rect
        lea     str_fault(pc),a0
        moveq   #16,d0
        moveq   #16,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        ; hex PC
        lea     numbuf(pc),a0
        moveq   #7,d2
.dig:   rol.l   #4,d6
        move.b  d6,d0
        and.b   #$F,d0
        cmp.b   #10,d0
        blt     .num
        add.b   #'A'-10-'0',d0
.num:   add.b   #'0',d0
        move.b  d0,(a0)+
        dbra    d2,.dig
        clr.b   (a0)
        lea     numbuf(pc),a0
        move.w  #16+8*8,d0
        moveq   #16,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
.halt:  bra     .halt

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

        include "../amiga/gen_data.i"

; ============================================================================
; Data
; ============================================================================
        even
str_menutitle:  dc.b    "UnoDOS MacPlus",0
str_version:    dc.b    "UnoDOS/MacPlus v0.1.0",0
str_build:      dc.b    "Milestone 1",0
str_x:          dc.b    "X",0
str_fault:      dc.b    "FAULT @ ",0
str_si1:        dc.b    "Video   512x342x1",0
str_si2:        dc.b    "Machine Mac Plus/SE",0
str_si3:        dc.b    "RAM",0
str_si4:        dc.b    "Uptime",0
str_si5:        dc.b    "UnoDOS/MacPlus  Milestone 1",0
str_uptime:     dc.b    "Uptime",0
str_t_sysinfo:  dc.b    "System Info",0
str_t_clock:    dc.b    "Clock",0
name_sysinfo:   dc.b    "Sys Info",0
name_clock:     dc.b    "Clock",0

        even
; app definitions: x, y, w, h, title offset from 'start'
app_def_tab:
        dc.w    80,80,240,90,   str_t_sysinfo-start
        dc.w    260,160,150,70, str_t_clock-start

icon_tab:
        dc.l    icon_sysinfo
        dc.l    icon_clock
name_tab:
        dc.l    name_sysinfo
        dc.l    name_clock

; ---------------------------------------------------------------- keymaps
; M0110/M0110A scan code (post-prefix page at $40) -> ASCII, unshifted US.
; '`' doubles as ESC (the M0110 has no Escape key).
        even
kb_ascii:
        dc.b    'a','s','d','f','h','g','z','x'    ; 00-07
        dc.b    'c','v',0,'b','q','w','e','r'      ; 08-0F
        dc.b    't','y','1','2','3','4','6','5'    ; 10-17
        dc.b    '=','9','7','-','8','0',']','o'    ; 18-1F
        dc.b    'u','[','i','p',13,'l','j',39      ; 20-27
        dc.b    'k',';',92,',','/','n','m','.'     ; 28-2F
        dc.b    9,' ',27,8,13,0,0,0                ; 30-37 (32='`'->ESC)
        dc.b    0,0,0,0,0,0,0,0                    ; 38-3F (modifiers)
        dc.b    0,'.',0,0,0,0,0,0                  ; 40-47 (42=right 46=left)
        dc.b    0,0,0,0,13,0,'-',0                 ; 48-4F (48=down 4D=up)
        dc.b    0,0,'0','1','2','3','4','5'        ; 50-57
        dc.b    '6','7',0,'8','9',0,0,0            ; 58-5F
        dc.b    0,0,'*',0,0,0,'+',0                ; 60-67
        dc.b    '=',0,0,0,0,'/',0,0                ; 68-6F
        dc.b    0,0,0,0,0,0,0,0                    ; 70-77
        dc.b    0,0,0,0,0,0,0,0                    ; 78-7F (arrows: raw only)

; scan -> canonical UnoDOS raw code (what the portable core dispatches on)
kb_raw:
        dc.b    0,0,0,0,0,0,0,0                    ; 00-07
        dc.b    0,0,0,0,0,0,0,0                    ; 08-0F
        dc.b    0,0,0,0,0,0,0,0                    ; 10-17
        dc.b    0,0,0,0,0,0,0,0                    ; 18-1F
        dc.b    0,0,0,0,0,0,0,0                    ; 20-27
        dc.b    0,0,0,0,0,0,0,0                    ; 28-2F
        dc.b    0,0,0,0,0,0,0,0                    ; 30-37
        dc.b    0,0,0,0,0,0,0,0                    ; 38-3F
        dc.b    0,0,$4E,0,0,0,$4F,$50              ; 40-47 (M0110A arrows;
        dc.b    $4D,0,0,0,0,$4C,0,0                ; 48-4F  Clr = F1/save)
        dc.b    0,0,0,0,0,0,0,0                    ; 50-57
        dc.b    0,0,0,0,0,0,0,0                    ; 58-5F
        dc.b    0,0,0,0,0,0,0,0                    ; 60-67
        dc.b    0,0,0,0,0,0,0,0                    ; 68-6F
        dc.b    0,0,0,0,0,0,0,0                    ; 70-77
        dc.b    0,0,0,$4F,$4E,$4D,$4C,0            ; 78-7F (ADB-style arrows
                                                   ; as sent by Mini vMac)

; ---------------------------------------------------------------- variables
        even
vars:
ticks:          dc.l    0
last_secs:      dc.l    0
dbl_tick:       dc.l    0
drag_win:       dc.l    0
scrn:           dc.l    0           ; framebuffer base (low-mem ScrnBase)
kb_t0:          dc.l    0
rawm_last:      dc.l    0           ; last seen low-mem RawMouse value
mouse_x:        dc.w    256
mouse_y:        dc.w    171
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
cur_x:          dc.w    -1          ; last mouse pos the cursor was drawn at
cur_y:          dc.w    -1
cur_px:         dc.w    0           ; clamped draw position
cur_py:         dc.w    0
mouse_btn:      dc.b    0
click_seq:      dc.b    0
last_seq:       dc.b    0
drag_active:    dc.b    0
cur_vis:        dc.b    0
kb_state:       dc.b    0           ; 0 idle, 1 cmd out, 2 awaiting reply
kb_prefix:      dc.b    0
scc_xprev:      dc.b    0
scc_yprev:      dc.b    0
kb_dbg:         dc.b    0           ; AUTOTEST: last non-null wire byte
kb_dbgs:        dc.b    0           ; ...and the last one shown
                dc.b    0           ; pad: keep word vars even
        even
evq:            ds.b    EVQ_SIZE*4
zlist:          ds.b    MAXWIN
        even
wintab:         ds.b    MAXWIN*WENT_SIZE
numbuf:         ds.b    16
clkbuf:         ds.b    12
cur_save:       ds.b    3*CURSOR_H
        even

        end
