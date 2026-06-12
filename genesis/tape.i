; ============================================================================
; UnoDOS/Genesis tape storage (milestone 4.5) - AFSK over audio, the
; classic 1-bit tape interface (docs/GENESIS-STORAGE.md).
;
; The Genesis has no ADC; like the ZX Spectrum / C64 it stores to tape
; through a 1-bit interface:
;   WRITE: the PSG generates the FSK tones -> Model 1 headphone jack ->
;          tape deck or a PC recording a WAV. Zero extra hardware.
;   READ:  tape/WAV playback -> comparator or Schmitt trigger (LM393 /
;          one transistor; the port wants 5V TTL) -> control port 2
;          pin 1 (D0). Real-hardware-only, like the PS/2 wiring; the
;          decoder below is an injectable pure routine, verified in
;          the emulator by AUTOTEST_TAPE.
;
; Format (Kansas City Standard at 1200 baud, NTSC timings):
;   '0' bit = one cycle of 1200 Hz   (2 half-periods of ~417us)
;   '1' bit = two cycles of 2400 Hz  (4 half-periods of ~208us)
;   byte    = start(0) + 8 data bits LSB-first + 2 stop(1)
;   block   = leader (2400 Hz, ~1.5s) + "UT01" + name[12] + len.w(BE)
;             + data[len] + sum.w(BE, 16-bit additive over data)
;
; Timebase: the read loop counts its own poll iterations between edges
; (~5.7us each at 7.67 MHz NTSC) - no free-running timer needed, no HV
; counter quirks. SHORT (2400 Hz half) ~ 36 counts, LONG (1200 Hz
; half) ~ 73. PAL consoles scale by 7.60/7.67 (within tolerance).
; ============================================================================

TAPE_PSG0   equ 93                  ; PSG tone value for 1200 Hz
TAPE_PSG1   equ 47                  ; PSG tone value for 2400 Hz
TAPE_BITDLY equ 580                 ; dbra count for one 833us bit cell
TAPE_THRESH equ 55                  ; SHORT/LONG poll-count threshold
TAPE_BREAK  equ 220                 ; > this = silence/leader gap
TAPE_SHORT  equ 36                  ; synthetic injection deltas
TAPE_LONG   equ 73

; ----------------------------------------------------------------------------
; tape_save_buf - write the Notepad buffer (v_np_name, v_npbuf, v_np_len)
; to the PSG. tape_save_blk is the parameterized engine (the Tracker
; saves its pattern through it). Interrupts are masked for the duration
; (~20s for 2KB); the screen freezes - by design.
; ----------------------------------------------------------------------------
tape_save_buf:
        movem.l d0/a0-a1/a4,-(sp)
        lea     VARS,a4
        lea     v_np_name(a4),a0
        lea     v_npbuf(a4),a1
        move.w  v_np_len(a4),d0
        bsr     tape_save_blk
        movem.l (sp)+,d0/a0-a1/a4
        rts

; tape_save_blk - a0 = 12-byte name, a1 = data, d0.w = length
tape_save_blk:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     VARS,a4
        move.w  d0,v_tp_len(a4)
        move.l  a0,v_tp_nmd(a4)
        move.l  a1,v_tp_dst(a4)
        move.w  sr,-(sp)
        or.w    #$0700,sr
        ; leader: 2400 Hz for ~1.5s
        moveq   #1,d0
        bsr     tape_tone
        move.w  #1800,d7
.lead:  bsr     tape_bitdelay
        dbra    d7,.lead
        ; header
        lea     str_ut01(pc),a0
        moveq   #3,d7
.mg:    moveq   #0,d0
        move.b  (a0)+,d0
        bsr     tape_tx_byte
        dbra    d7,.mg
        move.l  v_tp_nmd(a4),a0
        moveq   #11,d7
.nm:    moveq   #0,d0
        move.b  (a0)+,d0
        bsr     tape_tx_byte
        dbra    d7,.nm
        move.w  v_tp_len(a4),d0
        lsr.w   #8,d0
        bsr     tape_tx_byte
        move.w  v_tp_len(a4),d0
        bsr     tape_tx_byte
        ; data + checksum
        move.l  v_tp_dst(a4),a0
        moveq   #0,d6               ; sum
        move.w  v_tp_len(a4),d7
        bra     .dchk
.data:  moveq   #0,d0
        move.b  (a0)+,d0
        add.w   d0,d6
        bsr     tape_tx_byte
.dchk:  dbra    d7,.data
        move.w  d6,d0
        lsr.w   #8,d0
        bsr     tape_tx_byte
        move.w  d6,d0
        bsr     tape_tx_byte
        ; tail leader + silence
        moveq   #1,d0
        bsr     tape_tone
        move.w  #600,d7
.tail:  bsr     tape_bitdelay
        dbra    d7,.tail
        move.b  #$9F,PSG            ; ch0 off
        move.w  (sp)+,sr
        move.b  #1,v_tp_msg(a4)
        movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; tape_tx_byte - d0.b: start + 8 LSB-first + 2 stop
tape_tx_byte:
        movem.l d0-d2,-(sp)
        move.b  d0,d2
        moveq   #0,d0               ; start bit
        bsr     tape_tx_bit
        moveq   #7,d1
.bit:   moveq   #0,d0
        lsr.b   #1,d2
        bcc     .z
        moveq   #1,d0
.z:     bsr     tape_tx_bit
        dbra    d1,.bit
        moveq   #1,d0               ; stop bits
        bsr     tape_tx_bit
        moveq   #1,d0
        bsr     tape_tx_bit
        movem.l (sp)+,d0-d2
        rts

; tape_tx_bit - d0 = 0/1: hold the bit's tone for one cell
tape_tx_bit:
        bsr     tape_tone
        bsr     tape_bitdelay
        rts

; tape_tone - d0 = 0 (1200 Hz) / 1 (2400 Hz) on PSG ch0
tape_tone:
        movem.l d0-d1,-(sp)
        move.w  #TAPE_PSG0,d1
        tst.w   d0
        beq     .set
        move.w  #TAPE_PSG1,d1
.set:   move.w  d1,d0
        and.b   #$0F,d0
        or.b    #$80,d0
        move.b  d0,PSG
        lsr.w   #4,d1
        and.b   #$3F,d1
        move.b  d1,PSG
        move.b  #$90,PSG            ; full volume
        movem.l (sp)+,d0-d1
        rts

; tape_bitdelay - one 833us bit cell
tape_bitdelay:
        move.w  d7,-(sp)
        move.w  #TAPE_BITDLY,d7
.dly:   dbra    d7,.dly
        move.w  (sp)+,d7
        rts

; ----------------------------------------------------------------------------
; tape_load_buf - read one block from port 2 D0 into the Notepad buffer.
; tape_load_core is the parameterized engine (v_tp_dst/v_tp_nmd/v_tp_max
; tell the block parser where the data and name land; the Tracker points
; them at its pattern). Interrupts masked; bounded (~12s) so a missing
; signal can't hang the machine. Sets v_tp_msg: 1 = ok, 2 = error.
; ----------------------------------------------------------------------------
tape_load_buf:
        movem.l d0/a0/a4,-(sp)
        lea     VARS,a4
        lea     v_npbuf(a4),a0
        move.l  a0,v_tp_dst(a4)
        lea     v_np_name(a4),a0
        move.l  a0,v_tp_nmd(a4)
        move.w  #NBUF-1,v_tp_max(a4)
        bsr     tape_load_core
        move.b  v_tp_done(a4),d0
        cmp.b   #1,d0
        bne     .out
        ; success: finalize the Notepad buffer
        move.w  v_tp_got(a4),d0
        move.w  d0,v_np_len(a4)
        clr.w   v_np_caret(a4)
        clr.w   v_np_top(a4)
        move.w  #-1,v_np_goal(a4)
        sf      v_np_dirty(a4)
.out:   movem.l (sp)+,d0/a0/a4
        rts

; tape_load_core - receive one block into (v_tp_dst, max v_tp_max),
; name into (v_tp_nmd). Callers set the pointers first.
tape_load_core:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     VARS,a4
        move.w  sr,-(sp)
        or.w    #$0700,sr
        bsr     tape_rx_init
        move.b  #$00,IO_CTRL2       ; all inputs, TH interrupt off
        lea     IO_DATA2,a0
        move.b  (a0),d3
        and.b   #1,d3               ; current level
        moveq   #0,d5               ; iteration counter since last edge
        move.l  #2200000,d6         ; total budget (~12s)
.poll:  addq.l  #1,d5
        subq.l  #1,d6
        beq     .timeout
        move.b  (a0),d2
        and.b   #1,d2
        cmp.b   d3,d2
        beq     .poll
        move.b  d2,d3               ; edge
        move.l  d5,d0
        cmp.l   #1000,d0            ; clamp wild gaps
        ble     .feed
        move.w  #1000,d0
.feed:  moveq   #0,d5
        bsr     tape_feed_half
        move.b  v_tp_done(a4),d0
        beq     .poll
        cmp.b   #1,d0
        bne     .timeout
        move.b  #1,v_tp_msg(a4)
        bra     .out
.timeout:
        move.b  #2,v_tp_msg(a4)
.out:   move.b  #$80,IO_CTRL2       ; restore the PS/2 EXT wiring
        move.w  (sp)+,sr
        movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; tape_rx_init - reset the decoder + block parser
tape_rx_init:
        movem.l d0,-(sp)
        sf      v_tp_state(a4)      ; 0 = hunting leader
        sf      v_tp_done(a4)
        sf      v_tp_bits(a4)
        sf      v_tp_need(a4)
        clr.w   v_tp_cnt(a4)
        clr.w   v_tp_byte(a4)
        clr.w   v_tp_bstate(a4)
        clr.w   v_tp_len(a4)
        clr.w   v_tp_sum(a4)
        clr.w   v_tp_got(a4)
        movem.l (sp)+,d0
        rts

; ----------------------------------------------------------------------------
; tape_feed_half - d0.w = poll counts between two edges (one half-period).
; The injectable decoder core: classify SHORT/LONG/BREAK, walk the KCS
; bit state machine, deliver bytes to tape_rx_byte. (a4 = VARS)
;
; States: 0 hunt leader (64 consecutive SHORTs arm the decoder)
;         1 idle - shorts (stop bits / leader) ignored, LONG = start
;         2 second half of the start cell (must be LONG)
;         3 first half of a data bit (LONG = 0, SHORT = 1)
;         4 finishing a 0-bit (1 more LONG)
;         5 finishing a 1-bit (3 more SHORTs)
; ----------------------------------------------------------------------------
tape_feed_half:
        movem.l d0-d2,-(sp)
        ; classify -> d1: 0 = SHORT, 1 = LONG, 2 = BREAK
        moveq   #0,d1
        cmp.w   #TAPE_THRESH,d0
        blt     .cls
        moveq   #1,d1
        cmp.w   #TAPE_BREAK,d0
        blt     .cls
        moveq   #2,d1
.cls:   moveq   #0,d2
        move.b  v_tp_state(a4),d2
        cmp.b   #2,d1
        bne     .nobrk
        sf      v_tp_state(a4)      ; silence: re-hunt
        clr.w   v_tp_cnt(a4)
        bra     .out
.nobrk: tst.b   d2
        beq     .hunt
        cmp.b   #1,d2
        beq     .idle
        cmp.b   #2,d2
        beq     .start2
        cmp.b   #3,d2
        beq     .bit1
        cmp.b   #4,d2
        beq     .bit0f
        bra     .bit1f
.hunt:  tst.w   d1
        bne     .hz
        addq.w  #1,v_tp_cnt(a4)
        cmp.w   #64,v_tp_cnt(a4)
        blt     .out
        move.b  #1,v_tp_state(a4)   ; leader heard: armed
        bra     .out
.hz:    clr.w   v_tp_cnt(a4)
        bra     .out
.idle:  tst.w   d1
        beq     .out                ; shorts between bytes: ignore
        move.b  #2,v_tp_state(a4)   ; first half of a start bit
        bra     .out
.start2:
        tst.w   d1
        beq     .resync             ; broken start cell
        move.b  #3,v_tp_state(a4)
        sf      v_tp_bits(a4)
        clr.w   v_tp_byte(a4)
        bra     .out
.bit1:  tst.w   d1
        beq     .one
        move.b  #4,v_tp_state(a4)   ; 0-bit: one more LONG
        bra     .out
.one:   move.b  #5,v_tp_state(a4)   ; 1-bit: three more SHORTs
        move.b  #3,v_tp_need(a4)
        bra     .out
.bit0f: tst.w   d1
        beq     .resync
        moveq   #0,d0
        bra     .record
.bit1f: tst.w   d1
        bne     .resync
        subq.b  #1,v_tp_need(a4)
        bne     .out
        moveq   #1,d0
.record:
        ; shift the bit in (LSB first)
        move.w  v_tp_byte(a4),d2
        lsr.w   #1,d2
        tst.w   d0
        beq     .rz
        or.w    #$80,d2
.rz:    move.w  d2,v_tp_byte(a4)
        addq.b  #1,v_tp_bits(a4)
        cmp.b   #8,v_tp_bits(a4)
        blt     .more
        move.w  d2,d0
        bsr     tape_rx_byte
        move.b  #1,v_tp_state(a4)   ; stops absorb as idle shorts
        bra     .out
.more:  move.b  #3,v_tp_state(a4)
        bra     .out
.resync:
        move.b  #1,v_tp_state(a4)
.out:   movem.l (sp)+,d0-d2
        rts

; tape_rx_byte - d0.b: block parser ("UT01" + name + len + data + sum)
tape_rx_byte:
        movem.l d0-d3/a0-a1,-(sp)
        move.w  v_tp_bstate(a4),d2
        cmp.w   #4,d2
        blt     .magic
        cmp.w   #16,d2
        blt     .name
        cmp.w   #18,d2
        blt     .len
        ; data or checksum?
        move.w  d2,d3
        sub.w   #18,d3              ; bytes so far past the header
        cmp.w   v_tp_len(a4),d3
        blt     .data
        ; checksum bytes
        move.w  d2,d1
        sub.w   #18,d1
        sub.w   v_tp_len(a4),d1     ; 0 = hi, 1 = lo
        bne     .sumlo
        lsl.w   #8,d0
        move.w  d0,v_tp_cnt(a4)     ; stash the hi byte
        bra     .adv
.sumlo: or.w    v_tp_cnt(a4),d0
        cmp.w   v_tp_sum(a4),d0
        beq     .good
        move.b  #2,v_tp_done(a4)    ; checksum mismatch
        bra     .out
.good:  move.w  v_tp_len(a4),d0
        move.w  d0,v_tp_got(a4)
        move.b  #1,v_tp_done(a4)
        bra     .out
.magic: lea     str_ut01(pc),a0
        cmp.b   (a0,d2.w),d0
        bne     .reset
        bra     .adv
.name:  move.l  v_tp_nmd(a4),a0
        move.b  d0,-4(a0,d2.w)      ; bstate 4..15 -> name 0..11
        bra     .adv
.len:   cmp.w   #16,d2
        bne     .lenlo
        lsl.w   #8,d0
        move.w  d0,v_tp_len(a4)
        bra     .adv
.lenlo: or.w    d0,v_tp_len(a4)
        move.w  v_tp_len(a4),d0
        cmp.w   v_tp_max(a4),d0     ; oversized block: refuse
        ble     .adv
        bra     .reset
.data:  move.l  v_tp_dst(a4),a0
        move.b  d0,(a0,d3.w)
        and.w   #$FF,d0
        add.w   d0,v_tp_sum(a4)
.adv:   addq.w  #1,v_tp_bstate(a4)
        bra     .out
.reset: clr.w   v_tp_bstate(a4)
        clr.w   v_tp_sum(a4)
.out:   movem.l (sp)+,d0-d3/a0-a1
        rts

str_ut01:       dc.b    "UT01"
        even

        ifd     AUTOTEST_TAPE
; ----------------------------------------------------------------------------
; tape_selftest - clock a complete synthetic block through the REAL
; decoder (tape_feed_half) with idealized half-period counts: leader,
; "UT01", name, len, payload, checksum. Lands in the Notepad buffer
; exactly as a real tape load would.
; ----------------------------------------------------------------------------
tape_selftest:
        movem.l d0-d7/a0/a4,-(sp)
        lea     VARS,a4
        bsr     tape_rx_init
        lea     v_npbuf(a4),a0      ; receive into the Notepad buffer
        move.l  a0,v_tp_dst(a4)
        lea     v_np_name(a4),a0
        move.l  a0,v_tp_nmd(a4)
        move.w  #NBUF-1,v_tp_max(a4)
        ; leader: 100 shorts
        move.w  #99,d7
.lead:  move.w  #TAPE_SHORT,d0
        bsr     tape_feed_half
        dbra    d7,.lead
        ; magic + name + len
        lea     str_ut01(pc),a0
        moveq   #3,d7
.mg:    moveq   #0,d0
        move.b  (a0)+,d0
        bsr     tape_inject_byte
        dbra    d7,.mg
        lea     .tname(pc),a0
        moveq   #11,d7
.nm:    moveq   #0,d0
        move.b  (a0)+,d0
        bsr     tape_inject_byte
        dbra    d7,.nm
        lea     .tdata(pc),a0
        moveq   #0,d6               ; length
.cnt:   tst.b   (a0)+
        beq     .gotlen
        addq.w  #1,d6
        bra     .cnt
.gotlen:
        move.w  d6,d0
        lsr.w   #8,d0
        bsr     tape_inject_byte
        move.w  d6,d0
        bsr     tape_inject_byte
        ; payload + running sum
        lea     .tdata(pc),a0
        moveq   #0,d5
        move.w  d6,d7
        subq.w  #1,d7
.dat:   moveq   #0,d0
        move.b  (a0)+,d0
        add.w   d0,d5
        bsr     tape_inject_byte
        dbra    d7,.dat
        move.w  d5,d0
        lsr.w   #8,d0
        bsr     tape_inject_byte
        move.w  d5,d0
        bsr     tape_inject_byte
        ; on success the buffer is finalized like a real load
        move.b  v_tp_done(a4),d0
        cmp.b   #1,d0
        bne     .fail
        move.w  v_tp_got(a4),d0
        move.w  d0,v_np_len(a4)
        clr.w   v_np_caret(a4)
        clr.w   v_np_top(a4)
        move.w  #-1,v_np_goal(a4)
        sf      v_np_dirty(a4)
        move.b  #1,v_tp_msg(a4)
        bra     .stout
.fail:  move.b  #2,v_tp_msg(a4)
.stout: movem.l (sp)+,d0-d7/a0/a4
        rts
.tname: dc.b    "TAPE.TXT",0,0,0,0
.tdata: dc.b    "HELLO FROM THE TAPE DECK",13
        dc.b    "decoded by tape_feed_half",0
        even

; tape_inject_byte - d0.b through the real bit engine: start cell (2
; longs), data cells (2 longs / 4 shorts), stop cells (8 shorts)
tape_inject_byte:
        movem.l d0-d3,-(sp)
        move.b  d0,d3
        bsr     .cell0              ; start
        moveq   #7,d2
.bit:   lsr.b   #1,d3
        bcs     .one
        bsr     .cell0
        bra     .nx
.one:   bsr     .cell1
.nx:    dbra    d2,.bit
        bsr     .cell1              ; stop bits = shorts, absorbed as idle
        bsr     .cell1
        movem.l (sp)+,d0-d3
        rts
.cell0: move.w  #TAPE_LONG,d0
        bsr     tape_feed_half
        move.w  #TAPE_LONG,d0
        bsr     tape_feed_half
        rts
.cell1: move.w  #TAPE_SHORT,d0
        bsr     tape_feed_half
        move.w  #TAPE_SHORT,d0
        bsr     tape_feed_half
        move.w  #TAPE_SHORT,d0
        bsr     tape_feed_half
        move.w  #TAPE_SHORT,d0
        bsr     tape_feed_half
        rts
        endc
