; ============================================================================
; UnoDOS/MacPlus sound foundation - the classic Mac pulse-width buffer.
;
; The sound circuit scans 370 words at MemTop-$300 once per frame (one word
; per scanline, 22.257 kHz); the HIGH byte of each word is the 8-bit PWM
; sample. The LOW bytes are the .Sony variable-speed disk PWM - NEVER
; touched here (clobbering them corrupts GCR reads on real drives), so all
; buffer writes are byte writes with stride 2.
;
; Machine gates (mach_ver, captured at boot from ROMBase+8):
;   $75 Plus  - full control: VIA PB7 sound enable (0=on) + PB0-2 volume
;   $76 SE    - same buffer hardware, but its VIA belongs to the ROM's ADB
;               stack (ROM-assisted input mode): buffer writes only, no VIA
;               pokes. Audibility depends on the ROM's boot state - real-hw
;               checklist item.
;   $78+ II   - ASC chip, no PWM buffer: sound disabled entirely.
;
; Square synth: Paula period (the shared cross-port note tables) -> half
; period in samples = period/40 (3546895/(p*8) Hz at 22257 Hz sample rate).
; The buffer is refilled on note change; the 370-word wrap seam is the
; authentic compact-Mac square-wave buzz.
;
;   snd_init             boot setup (after mach_ver is captured)
;   snd_tone   d0 = Paula period (0 = rest): fill buffer + enable
;   snd_off              silence (fill $00 + disable on the Plus)
;
; Game-music sequencer (real gm_* - games.i note tables, PAL durations
; rescaled 50->60 Hz):
;   gm_start   a0 = (period.w,dur.w) pairs, d0 = count, d1 = owner proc
;   gm_stop / gm_tick    (gm_tick mutes when the owner is not topmost)
; ============================================================================

SNDBUF_OFF  equ $300                ; buffer = MemTop - $300
SND_WORDS   equ 370
SND_AMP     equ $C0                 ; square high level ($00 low)

; snd_init - locate the buffer, set Plus VIA volume, start silent
snd_init:
        movem.l d0-d1/a0-a1/a4,-(sp)
        lea     vars(pc),a4
        move.l  LM_MEMTOP,d0
        sub.l   #SNDBUF_OFF,d0
        move.l  d0,snd_buf-vars(a4)
        move.b  mach_ver(pc),d0     ; (cmpi on (pc) is 68020+: load first)
        cmp.b   #$76,d0             ; Plus/SE have the PWM buffer
        bls     .has
        sf      snd_ok-vars(a4)
        bra     .out
.has:   st      snd_ok-vars(a4)
        move.b  mach_ver(pc),d0
        cmp.b   #$75,d0             ; VIA control on the Plus only
        bne     .novia
        move.b  VIA_DDRB,d0
        or.b    #%10000111,d0       ; PB7 + PB0-2 as outputs
        move.b  d0,VIA_DDRB
        move.b  VIA_ORB,d0
        or.b    #%10000111,d0       ; volume 7, PB7=1 (disabled)
        move.b  d0,VIA_ORB
.novia: bsr     snd_off
.out:   movem.l (sp)+,d0-d1/a0-a1/a4
        rts

; snd_fill - d1.b = sample for ALL slots (high bytes only, stride 2)
snd_fill:
        movem.l d0/a0,-(sp)
        move.l  snd_buf(pc),a0
        move.w  #SND_WORDS-1,d0
.f:     move.b  d1,(a0)
        addq.l  #2,a0
        dbra    d0,.f
        movem.l (sp)+,d0/a0
        rts

; snd_enable - d0 = 0 enable / 1 disable (Plus PB7; no-op elsewhere)
snd_enable:
        movem.l d0-d1,-(sp)
        move.b  mach_ver(pc),d1
        cmp.b   #$75,d1
        bne     .out
        move.b  VIA_ORB,d1
        and.b   #%01111111,d1
        tst.w   d0
        beq     .set
        or.b    #%10000000,d1
.set:   move.b  d1,VIA_ORB
.out:   movem.l (sp)+,d0-d1
        rts

; snd_tone - d0.w = Paula period (0 = rest): square into the buffer
snd_tone:
        movem.l d0-d3/a0/a4,-(sp)
        lea     vars(pc),a4
        move.b  snd_ok(pc),d1
        beq     .out
        tst.w   d0
        beq     .rest
        ; half period in samples = period/40, clamped >= 2
        and.l   #$FFFF,d0
        divu    #40,d0
        cmp.w   #2,d0
        bge     .hok
        moveq   #2,d0
.hok:   move.w  d0,d2               ; d2 = half period
        move.w  d0,d3               ; d3 = run countdown
        moveq   #0,d1               ; current level: low
        move.l  snd_buf(pc),a0
        move.w  #SND_WORDS-1,d0
.f:     move.b  d1,(a0)
        addq.l  #2,a0
        subq.w  #1,d3
        bne     .nf
        move.w  d2,d3               ; run done: toggle level
        eor.b   #SND_AMP,d1
.nf:    dbra    d0,.f
        moveq   #0,d0
        bsr     snd_enable
        bra     .out
.rest:  moveq   #0,d1
        bsr     snd_fill            ; flat buffer = silence, stay enabled
.out:   movem.l (sp)+,d0-d3/a0/a4
        rts

; snd_off - silence and (Plus) disable
snd_off:
        movem.l d0-d1,-(sp)
        move.b  snd_ok(pc),d0
        beq     .out
        moveq   #0,d1
        bsr     snd_fill
        moveq   #1,d0
        bsr     snd_enable
.out:   movem.l (sp)+,d0-d1
        rts

; ---------------------------------------------------------------------------
; Game-music sequencer (the Amiga gm_* engine over snd_tone). Note tables
; are (Paula period.w, PAL-tick duration.w) pairs shared across ports;
; durations are rescaled 50 Hz -> TICKS_SEC here (x6/5).
; ---------------------------------------------------------------------------

; gm_start - a0 = note pairs, d0 = count, d1 = owner proc
gm_start:
        movem.l d0-d1/a4,-(sp)
        lea     vars(pc),a4
        move.l  a0,gm_notes-vars(a4)
        move.w  d0,gm_count-vars(a4)
        move.w  d1,gm_owner-vars(a4)
        subq.w  #1,d0
        move.w  d0,gm_ix-vars(a4)   ; first tick wraps to note 0
        st      gm_on-vars(a4)
        move.l  ticks(pc),d0
        move.l  d0,gm_end-vars(a4)
        movem.l (sp)+,d0-d1/a4
        rts

; gm_stop - silence and disable the sequencer
gm_stop:
        movem.l d0/a4,-(sp)
        lea     vars(pc),a4
        sf      gm_on-vars(a4)
        bsr     snd_off
        movem.l (sp)+,d0/a4
        rts

; gm_tick - advance the sequencer (called from the main loop).
; Mutes (but keeps position) while the owning game is not topmost.
gm_tick:
        movem.l d0-d3/a0/a2/a4,-(sp)
        move.b  gm_on(pc),d0
        beq     .out
        move.w  zcount(pc),d2
        beq     .mute
        subq.w  #1,d2
        bsr     zwin_ptr
        moveq   #0,d0
        move.b  WPROC(a2),d0
        cmp.w   gm_owner(pc),d0
        bne     .mute
        move.l  ticks(pc),d0
        cmp.l   gm_end(pc),d0
        blt     .out
        lea     vars(pc),a4
        move.w  gm_ix(pc),d1
        addq.w  #1,d1
        cmp.w   gm_count(pc),d1
        blt     .ixok
        moveq   #0,d1
.ixok:  move.w  d1,gm_ix-vars(a4)
        move.l  gm_notes(pc),a0
        add.w   d1,d1
        add.w   d1,d1               ; index * 4 bytes
        lea     (a0,d1.w),a0
        move.w  (a0)+,d2            ; Paula period (0 = rest)
        moveq   #0,d3
        move.w  (a0),d3             ; duration in PAL (50 Hz) ticks
        mulu    #6,d3
        divu    #5,d3               ; -> 60 Hz ticks, same tempo
        and.l   #$FFFF,d3
        move.l  ticks(pc),d0
        add.l   d3,d0
        move.l  d0,gm_end-vars(a4)
        move.w  d2,d0
        bsr     snd_tone            ; period or rest
        bra     .out
.mute:  moveq   #0,d0
        bsr     snd_tone            ; flat buffer, position kept
.out:   movem.l (sp)+,d0-d3/a0/a2/a4
        rts
