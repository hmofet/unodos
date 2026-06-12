; ============================================================================
; UnoDOS/Genesis PS/2 drivers (docs/GENESIS-PORT.md wiring)
;
;   port 2 = PS/2 KEYBOARD: CLK -> pin 7 (TH, EXT level-2 interrupt),
;            DATA -> pin 1 (D0). Interrupt-driven: every falling clock
;            edge lands in ps2k_edge, which feeds the 11-bit frame
;            assembler; complete bytes go through the scancode-set-2
;            decoder into the shared event queue.
;   port 1 = PS/2 MOUSE: CLK -> TH (polled, host-inhibit), DATA -> D0.
;            The driver holds CLK low (inhibit) and opens a receive
;            window each vblank; stream-mode packets assemble across
;            windows and decimate naturally to the frame rate.
;
; NOTE: emulators do not model PS/2 devices on the control ports, so
; this wiring is REAL-HARDWARE-ONLY. The protocol engines themselves
; (ps2k_feedbit / ps2m_feedbit / the decoders) are pure injectable
; routines, verified in BlastEm by the AUTOTEST_PS2 build.
;
; I/O control registers: bit n = 1 -> pin n is an output ($A10009/0B);
; bit 7 = TH-interrupt enable (port 2 only reaches the 68K as EXT).
; Pin bits in the data registers: 0..3 = D0..D3 (Up/Down/Left/Right),
; 4 = TL, 5 = TR, 6 = TH.
; ============================================================================

PS2_RAW_F1  equ $50                 ; Amiga-port raw codes (app contract)
PS2_RAW_UP  equ $4C
PS2_RAW_DN  equ $4D
PS2_RAW_RT  equ $4E
PS2_RAW_LT  equ $4F

; ----------------------------------------------------------------------------
; ps2_init - port 2 keyboard wiring + port 1 mouse probe.
; Called before interrupts are enabled (the probe is polled).
; ----------------------------------------------------------------------------
ps2_init:
        movem.l d0-d7/a0/a4,-(sp)
        lea     VARS,a4
        ; port 2: all pins inputs, TH transition -> EXT interrupt
        move.b  #$80,IO_CTRL2
        ; port 1: probe for a PS/2 mouse: "enable data reporting" ($F4)
        ; must come back with an ACK ($FA). No device / a gamepad ->
        ; timeouts -> pad mode.
        move.w  #$F4,d0
        bsr     ps2_send
        tst.w   d0
        bmi     .padmode
        bsr     ps2_recv
        tst.w   d0
        bmi     .padmode
        cmp.w   #$FA,d0
        bne     .padmode
        ; mouse answered: stream mode is on; inhibit until the vblank
        ; windows start
        st      v_port1_mode(a4)
        bsr     ps2_inhibit
        bra     .out
.padmode:
        sf      v_port1_mode(a4)
        move.b  #$40,IO_CTRL1       ; TH output (pad select line)
        move.b  #$40,IO_DATA1       ; TH high (idle)
.out:   movem.l (sp)+,d0-d7/a0/a4
        rts

; ps2_inhibit - hold the mouse CLK (TH) low: the device buffers/waits
ps2_inhibit:
        move.b  #$40,IO_CTRL1       ; TH output
        move.b  #$00,IO_DATA1       ; TH low
        rts

; ----------------------------------------------------------------------------
; ps2_send - d0.b = byte -> PS/2 device on port 1. -> d0 = 0 ok / -1 fail.
; Host-to-device: inhibit >=100us, start bit (DATA low), release CLK,
; the device clocks the rest in; we change DATA on each falling edge.
; ----------------------------------------------------------------------------
ps2_send:
        movem.l d1-d5/a0,-(sp)
        lea     IO_DATA1,a0
        ; build the 10 bits to clock out: 8 data (LSB first), odd
        ; parity, stop(1)
        moveq   #0,d1
        move.b  d0,d1
        ; odd parity of d1 -> d3
        moveq   #0,d3
        moveq   #7,d2
        move.b  d1,d4
.par:   lsr.b   #1,d4
        bcc     .nopar
        addq.b  #1,d3
.nopar: dbra    d2,.par
        and.b   #1,d3
        eor.b   #1,d3               ; odd parity bit
        lsl.w   #8,d3
        or.w    d3,d1               ; bit 8 = parity
        bset    #9,d1               ; bit 9 = stop
        ; inhibit: CLK low >= 100us (7.67MHz: ~150us with 120 loops)
        move.b  #$40,IO_CTRL1
        move.b  #$00,(a0)
        move.w  #120,d2
.inh:   dbra    d2,.inh
        ; start bit: DATA low too, then release CLK (device takes over)
        move.b  #$41,IO_CTRL1       ; TH + D0 outputs
        move.b  #$00,(a0)           ; both low
        moveq   #10,d2
.st:    dbra    d2,.st
        move.b  #$01,IO_CTRL1       ; release CLK, keep DATA (low)
        ; 10 bits: wait CLK low -> present bit -> wait CLK high
        moveq   #9,d4
.bit:   bsr     ps2_wait_clk_lo
        bmi     .fail
        move.b  d1,d5
        and.b   #1,d5
        move.b  d5,(a0)             ; D0 = current bit (TH is input)
        lsr.w   #1,d1
        bsr     ps2_wait_clk_hi
        bmi     .fail
        dbra    d4,.bit
        ; release DATA; device ACK-pulses (CLK+DATA low once)
        move.b  #$00,IO_CTRL1
        bsr     ps2_wait_clk_lo
        bmi     .fail
        bsr     ps2_wait_clk_hi
        bmi     .fail
        moveq   #0,d0
        movem.l (sp)+,d1-d5/a0
        rts
.fail:  move.b  #$00,IO_CTRL1       ; leave the port floating
        moveq   #-1,d0
        movem.l (sp)+,d1-d5/a0
        rts

; ps2_recv - polled receive of one device byte on port 1 (bounded).
; -> d0 = byte or -1. Frame check: start=0, stop=1.
ps2_recv:
        movem.l d1-d4/a0,-(sp)
        lea     IO_DATA1,a0
        move.b  #$00,IO_CTRL1       ; all inputs (CLK released)
        moveq   #0,d1               ; shifter
        moveq   #10,d4              ; 11 bits
        moveq   #0,d3               ; bit position
.bit:   bsr     ps2_wait_clk_lo
        bmi     .fail
        move.b  (a0),d2
        and.w   #1,d2               ; DATA
        lsl.w   d3,d2
        or.w    d2,d1
        bsr     ps2_wait_clk_hi
        bmi     .fail
        addq.w  #1,d3
        dbra    d4,.bit
        ; validate start(bit0)=0, stop(bit10)=1
        btst    #0,d1
        bne     .fail
        btst    #10,d1
        beq     .fail
        lsr.w   #1,d1
        and.w   #$FF,d1
        move.w  d1,d0
        movem.l (sp)+,d1-d4/a0
        rts
.fail:  moveq   #-1,d0
        movem.l (sp)+,d1-d4/a0
        rts

; ps2_wait_clk_lo / _hi - bounded spins on port 1 TH. -> d0 minus on timeout.
ps2_wait_clk_lo:
        move.w  #8000,d0            ; ~20ms+
.spin:  btst    #6,(a0)
        beq     .ok
        dbra    d0,.spin
        moveq   #-1,d0
        rts
.ok:    moveq   #0,d0
        rts
ps2_wait_clk_hi:
        move.w  #8000,d0
.spin:  btst    #6,(a0)
        bne     .ok
        dbra    d0,.spin
        moveq   #-1,d0
        rts
.ok:    moveq   #0,d0
        rts

; ----------------------------------------------------------------------------
; ps2m_window - vblank receive window for the port-1 mouse (ISR context,
; a4 = VARS). Release CLK, collect edges for ~one byte time, re-inhibit.
; Frames straddle windows fine: a byte aborted by the inhibit is resent
; by the device, and the assembler resyncs on the start-bit check.
; ----------------------------------------------------------------------------
ps2m_window:
        movem.l d0-d3/a0,-(sp)
        lea     IO_DATA1,a0
        move.b  #$00,IO_CTRL1       ; release CLK
        move.w  #900,d3             ; poll budget (~1.2ms)
        moveq   #1,d2               ; previous CLK level (released = high)
.poll:  move.b  (a0),d0
        btst    #6,d0
        sne     d1                  ; d1 = $FF if CLK high
        and.b   #1,d1
        cmp.b   d2,d1
        beq     .same
        move.b  d1,d2
        tst.b   d1
        bne     .same               ; rising edge: ignore
        and.w   #1,d0               ; falling edge: sample DATA
        move.w  d0,d1
        bsr     ps2m_feedbit
.same:  dbra    d3,.poll
        bsr     ps2_inhibit
        movem.l (sp)+,d0-d3/a0
        rts

; ps2m_feedbit - d1.w = bit. 11-bit frame assembler -> ps2m_byte.
ps2m_feedbit:
        movem.l d0-d2,-(sp)
        moveq   #0,d0
        move.b  v_ps2m_bits(a4),d0
        move.w  v_ps2m_shift(a4),d2
        lsl.w   d0,d1
        or.w    d1,d2
        move.w  d2,v_ps2m_shift(a4)
        addq.b  #1,d0
        move.b  d0,v_ps2m_bits(a4)
        cmp.b   #11,d0
        blt     .out
        sf      v_ps2m_bits(a4)
        clr.w   v_ps2m_shift(a4)
        ; frame check: start=0, stop=1 (resync: drop bad frames)
        btst    #0,d2
        bne     .out
        btst    #10,d2
        beq     .out
        lsr.w   #1,d2
        and.w   #$FF,d2
        move.w  d2,d0
        bsr     ps2m_byte
.out:   movem.l (sp)+,d0-d2
        rts

; ps2m_byte - d0.b = mouse byte: 3-byte stream packets -> cursor state
ps2m_byte:
        movem.l d0-d3/a0,-(sp)
        lea     v_ps2m_pkt(a4),a0
        moveq   #0,d1
        move.b  v_ps2m_pktn(a4),d1
        bne     .store
        ; byte 0 must have the sync bit (bit 3): drop until aligned
        btst    #3,d0
        beq     .out
.store: move.b  d0,(a0,d1.w)
        addq.b  #1,d1
        move.b  d1,v_ps2m_pktn(a4)
        cmp.b   #3,d1
        blt     .out
        sf      v_ps2m_pktn(a4)
        ; decode: b0 = YS XS YV XV 1 M R L, b1 = dx, b2 = dy (up = +)
        moveq   #0,d0
        move.b  (a0),d0
        moveq   #0,d1
        move.b  1(a0),d1            ; dx
        btst    #4,d0
        beq     .xpos
        sub.w   #256,d1
.xpos:  moveq   #0,d2
        move.b  2(a0),d2            ; dy
        btst    #5,d0
        beq     .ypos
        sub.w   #256,d2
.ypos:
        move.w  v_mouse_x(a4),d3
        add.w   d1,d3
        bge     .x0
        moveq   #0,d3
.x0:    cmp.w   #SCRW-1,d3
        ble     .x1
        move.w  #SCRW-1,d3
.x1:    move.w  d3,v_mouse_x(a4)
        move.w  v_mouse_y(a4),d3
        sub.w   d2,d3               ; PS/2 +y is up
        bge     .y0
        moveq   #0,d3
.y0:    cmp.w   #SCRH-1,d3
        ble     .y1
        move.w  #SCRH-1,d3
.y1:    move.w  d3,v_mouse_y(a4)
        ; left button -> the shared edge/latch path (mouse_buttons)
        moveq   #0,d1
        btst    #0,d0
        beq     .btn
        moveq   #1,d1
.btn:   move.b  d1,v_mouse_btn(a4)
.out:   movem.l (sp)+,d0-d3/a0
        rts

; ----------------------------------------------------------------------------
; ps2k_edge - EXT level-2 interrupt: a port-2 TH (= keyboard CLK)
; transition. Sample on falling edges; resync on inter-bit gaps.
; (ISR context, a4 = VARS)
; ----------------------------------------------------------------------------
ps2k_edge:
        move.b  IO_DATA2,d0
        btst    #6,d0               ; CLK level
        bne     .out                ; rising edge: ignore
        ; resync: > 3 ticks since the last edge = a new frame must start
        move.b  v_ticks+3(a4),d1
        move.b  d1,d2
        sub.b   v_ps2k_lastt(a4),d2
        move.b  d1,v_ps2k_lastt(a4)
        cmp.b   #3,d2
        bls     .feed
        sf      v_ps2k_bits(a4)
        clr.w   v_ps2k_shift(a4)
.feed:  and.w   #1,d0               ; DATA
        move.w  d0,d1
        bsr     ps2k_feedbit
.out:   rts

; ps2k_feedbit - d1.w = bit. 11-bit frame assembler -> ps2k_byte.
ps2k_feedbit:
        movem.l d0-d2,-(sp)
        moveq   #0,d0
        move.b  v_ps2k_bits(a4),d0
        move.w  v_ps2k_shift(a4),d2
        lsl.w   d0,d1
        or.w    d1,d2
        move.w  d2,v_ps2k_shift(a4)
        addq.b  #1,d0
        move.b  d0,v_ps2k_bits(a4)
        cmp.b   #11,d0
        blt     .out
        sf      v_ps2k_bits(a4)
        clr.w   v_ps2k_shift(a4)
        btst    #0,d2               ; start must be 0
        bne     .out
        btst    #10,d2              ; stop must be 1
        beq     .out
        lsr.w   #1,d2
        and.w   #$FF,d2
        move.w  d2,d0
        bsr     ps2k_byte
.out:   movem.l (sp)+,d0-d2
        rts

; ps2k_byte - d0.b = scancode byte (set 2) -> decoded key events
ps2k_byte:
        movem.l d0-d3/a0,-(sp)
        cmp.b   #$F0,d0             ; break prefix
        bne     .nf0
        st      v_ps2k_break(a4)
        bra     .out
.nf0:   cmp.b   #$E0,d0             ; extended prefix
        bne     .ne0
        st      v_ps2k_ext(a4)
        bra     .out
.ne0:
        move.b  v_ps2k_break(a4),d1
        beq     .make
        ; break: track shift release, swallow everything else
        sf      v_ps2k_break(a4)
        sf      v_ps2k_ext(a4)
        cmp.b   #$12,d0
        beq     .shup
        cmp.b   #$59,d0
        bne     .out
.shup:  sf      v_ps2k_shdown(a4)
        bra     .out
.make:
        cmp.b   #$12,d0             ; shift make
        beq     .shdn
        cmp.b   #$59,d0
        bne     .nsh
.shdn:  st      v_ps2k_shdown(a4)
        sf      v_ps2k_ext(a4)
        bra     .out
.nsh:
        move.b  v_ps2k_ext(a4),d1
        beq     .base
        ; extended: arrows (the only E0 codes we map)
        sf      v_ps2k_ext(a4)
        moveq   #0,d2
        cmp.b   #$75,d0
        bne     .ne75
        moveq   #PS2_RAW_UP,d2
.ne75:  cmp.b   #$72,d0
        bne     .ne72
        moveq   #PS2_RAW_DN,d2
.ne72:  cmp.b   #$74,d0
        bne     .ne74
        moveq   #PS2_RAW_RT,d2
.ne74:  cmp.b   #$6B,d0
        bne     .ne6b
        moveq   #PS2_RAW_LT,d2
.ne6b:  tst.w   d2
        beq     .out
        lsl.w   #8,d2
        move.w  d2,d1
        moveq   #EV_KEY,d0
        bsr     ev_post
        bra     .out
.base:
        cmp.b   #$05,d0             ; F1
        bne     .nf1
        move.w  #PS2_RAW_F1<<8,d1
        moveq   #EV_KEY,d0
        bsr     ev_post
        bra     .out
.nf1:
        cmp.b   #132,d0             ; beyond the map (unsigned!)
        bhs     .out
        moveq   #0,d2
        move.b  d0,d2
        lea     ps2map(pc),a0
        move.b  v_ps2k_shdown(a4),d1
        beq     .map
        lea     ps2map_sh(pc),a0
.map:   moveq   #0,d1
        move.b  (a0,d2.w),d1
        beq     .out
        moveq   #EV_KEY,d0
        bsr     ev_post
.out:   movem.l (sp)+,d0-d3/a0
        rts

        ifd     AUTOTEST_PS2
; ----------------------------------------------------------------------------
; ps2_selftest - clock synthetic frames through the REAL bit engines:
; keyboard: "ps2 ok" (with break codes), mouse: one stream packet
; (dx +100, dy +40 up, no buttons) -> cursor jumps to (260, 72).
; ----------------------------------------------------------------------------
ps2_selftest:
        movem.l d0-d3/a0/a4,-(sp)
        lea     VARS,a4
        lea     .codes(pc),a0
.key:   moveq   #0,d0
        move.b  (a0)+,d0
        beq     .mouse
        move.l  a0,-(sp)
        bsr     ps2_inject_kbyte    ; make
        move.w  #$F0,d0
        bsr     ps2_inject_kbyte    ; break prefix
        move.l  (sp),a0
        moveq   #0,d0
        move.b  -1(a0),d0
        bsr     ps2_inject_kbyte    ; break code
        move.l  (sp)+,a0
        bra     .key
.mouse: move.w  #$08,d0             ; b0: sync, no buttons, signs +
        bsr     ps2_inject_mbyte
        move.w  #100,d0             ; dx
        bsr     ps2_inject_mbyte
        move.w  #40,d0              ; dy (up)
        bsr     ps2_inject_mbyte
        movem.l (sp)+,d0-d3/a0/a4
        rts
.codes: dc.b    $4D,$1B,$1E,$29,$44,$42,0   ; p s 2 spc o k
        even

; ps2_inject_kbyte / ps2_inject_mbyte - d0.b -> 11 ps2*_feedbit calls
ps2_inject_kbyte:
        lea     ps2k_feedbit(pc),a1
        bra     ps2_inject
ps2_inject_mbyte:
        lea     ps2m_feedbit(pc),a1
ps2_inject:
        movem.l d0-d4,-(sp)
        ; frame = start(0) + 8 data LSB + odd parity + stop(1)
        moveq   #0,d3               ; parity accumulator
        move.b  d0,d4
        moveq   #0,d1               ; start bit
        jsr     (a1)
        moveq   #7,d2
.bits:  moveq   #0,d1
        lsr.b   #1,d4
        bcc     .z
        moveq   #1,d1
        addq.b  #1,d3
.z:     jsr     (a1)
        dbra    d2,.bits
        and.b   #1,d3
        eor.b   #1,d3               ; odd parity
        moveq   #0,d1
        move.b  d3,d1
        jsr     (a1)
        moveq   #1,d1               ; stop bit
        jsr     (a1)
        movem.l (sp)+,d0-d4
        rts
        endc
