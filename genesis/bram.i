; ============================================================================
; UnoDOS/Genesis Sega CD backup RAM (milestone 5) - Mode-1 Sub-CPU stub +
; BIOS BURAM, per docs/GENESIS-STORAGE.md tier 3.
;
; The cartridge stays the booted program; a Sega/Mega CD attachment is a
; peripheral. Only the Sub-CPU (the CD's own 68000) can touch the backup
; RAM, so the kernel boots it with a ~300 byte stub that services a tiny
; RPC (LIST / READ / WRITE / DELETE) over the gate-array mailbox, with
; file data staged through Word RAM. The stub calls the BIOS _BURAM traps
; so the on-media format is the standard Sega directory - files saved
; here are visible to the console's own Backup RAM manager.
;
; Mode-1 bring-up (the documented-but-fiddly part; sequence per the
; community-standard Mode 1 init, viciious/SegaCDMode1PCM):
;   1. find the Kosinski-compressed Sub-CPU BIOS inside the main BIOS
;      ROM at $400000 ("SEGA"/"WONDER" at candidate+$6D: $415800 most
;      models, $416000 some, $41AD00 LaserActive/WonderMega)
;   2. gate-array reset dance, assert sub reset + bus request
;   3. clear PRG-RAM bank 0, Kosinski-decompress the sub BIOS into it,
;      copy our stub to $6000 (the standard SP module slot)
;   4. write-protect the BIOS region, release the Sub-CPU, pulse INT2
;      every vblank (the sub BIOS needs the level-2 ticks)
;
; Transport is injectable (PORT-SPEC standing rule): AUTOTEST_BRAM swaps
; the mailbox for a RAM-backed fake implementing the same four ops over
; a 64-byte-block store, so the protocol + Files/Notepad integration is
; emulator-verified; the BIOS-trap path needs a CD-capable emulator
; (Genesis Plus GX / Ares) or real hardware.
;
; File mapping: BRAM names are 11 chars, space-padded; UnoDOS 8.3 names
; map with the dot replaced by '_' ("DEMO.TXT" -> "DEMO_TXT   "). The
; payload's first 14 bytes are a header (original 12-char name + len.w)
; so round trips restore the exact name and byte length; foreign files
; (other software's saves) fail the header sanity check and open raw.
; ============================================================================

; ---------------------------------------------------------------- hardware
GA_RESET    equ $A12000             ; b8 IFL2 (sub INT2); b1 SBRQ, b0 SRES
GA_RESETB   equ $A12001             ; byte access to SBRQ/SRES
GA_MEM      equ $A12002             ; hi: write-protect; lo: bank/MODE/DMNA/RET
GA_MEMB     equ $A12003
GA_CFLAG_M  equ $A1200E             ; main->sub comm flag (RW here)
GA_CFLAG_S  equ $A1200F             ; sub->main comm flag (RO here)
GA_CMD0     equ $A12010             ; main->sub command words
GA_STAT0    equ $A12020             ; sub->main status words
CD_BIOS     equ $400000
CD_PRGRAM   equ $420000             ; PRG-RAM window (128KB, banked)
CD_WORDRAM  equ $600000             ; Word RAM, 2M mode (256KB)

; RPC ops (GA_CMD0); results land at GA_STAT0 (0 ok / $FFFF error)
BROP_LIST   equ 1
BROP_READ   equ 2
BROP_WRITE  equ 3
BROP_DEL    equ 4

BR_MAXDIR   equ 8                   ; listing cache entries (UI rows)
BRFK_BLKS   equ 48                  ; fake store heap: 48 x 64 bytes

; ============================================================================
; Boot init - detect, boot the Sub-CPU, first LIST. Interrupts are off
; (called from start before the ISRs are enabled), so every wait below
; is a bounded cycle loop, and we pulse INT2 by hand for the sub BIOS.
; ============================================================================
bram_init:
        movem.l d0-d7/a0-a3/a4,-(sp)
        lea     VARS,a4
        sf      v_bram_pres(a4)
        sf      v_bram_rt(a4)
        clr.w   v_brcount(a4)
        clr.w   v_files_vol(a4)
        move.b  #1,v_brseq(a4)

        ifd     AUTOTEST_BRAM
        ; injectable transport: a formatted, empty RAM-backed store
        st      v_bram_pres(a4)
        clr.w   v_brfk_cnt(a4)
        bra     brin_out
        endc

        ; ---- 0. anything on the expansion port at all? $A10001 bit 5
        ; (DISK) reads 0 only with an expansion device attached. Not
        ; every emulator models the bit, so the probe ALSO arms a bus-
        ; error recovery (kernel.asm berr): with nothing attached,
        ; $400000+ may be unmapped and reading it raises a bus error
        ; (BlastEm) instead of returning open bus - the handler unwinds
        ; to brin_out via v_brsp and boot continues with no CD.
        btst    #5,HW_VERSION
        bne     brin_out            ; no expansion device
        move.l  sp,v_brsp(a4)
        st      v_brprobe(a4)

        ; ---- 1. find the compressed Sub-CPU BIOS in the main BIOS ROM
        lea     CD_BIOS+$15800,a3
        bsr     brin_sig
        beq     brin_have
        lea     CD_BIOS+$16000,a3
        bsr     brin_sig
        beq     brin_have
        lea     CD_BIOS+$1AD00,a3
        bsr     brin_sig
        beq     brin_have
        lea     CD_BIOS+$D500,a3
        bsr     brin_sig
        beq     brin_have
        bra     brin_out            ; no CD attached
brin_have:
        ; ---- 2. gate-array reset (LaserActive needs the full dance),
        ;         then hold the Sub-CPU in reset and take its bus
        move.w  #$FF00,GA_MEM
        move.b  #$03,GA_RESETB
        move.b  #$02,GA_RESETB
        move.b  #$00,GA_RESETB
        move.l  #100000,d7
.bus:   move.b  #$02,GA_RESETB      ; SBRQ=1, SRES=0
        move.b  GA_RESETB,d0
        and.b   #$02,d0
        bne     .busok
        subq.l  #1,d7
        bne     .bus
        bra     brin_out            ; bus never granted: treat as no CD
.busok:
        ; ---- 3. PRG-RAM: no write-protect, bank 0, 2M mode
        move.w  #$0002,GA_MEM
        ; clear bank 0 (the LaserActive sub BIOS hangs on dirty RAM)
        lea     CD_PRGRAM,a0
        move.w  #$8000-1,d0
        moveq   #0,d1
.clr:   move.l  d1,(a0)+
        dbra    d0,.clr
        ; decompress the sub BIOS to PRG-RAM 0, stub to $6000
        move.l  a3,a0
        lea     CD_PRGRAM,a1
        bsr     kos_decomp
        lea     bram_sub_start(pc),a0
        lea     CD_PRGRAM+$6000,a1
        move.w  #(bram_sub_end-bram_sub_start)-1,d0
.cps:   move.b  (a0)+,(a1)+
        dbra    d0,.cps
        ; ---- 4. release the Sub-CPU and wait for the stub's READY flag
        move.b  #$00,GA_CFLAG_M
        move.b  #$2A,GA_MEM         ; write-protect the sub BIOS (< $5400)
                                    ; (byte write: keep bank/DMNA bits)
        move.b  #$01,GA_RESETB      ; SRES=1, SBRQ=0: run
        move.l  #60000,d7           ; ~3s outer bound
.run:   move.b  GA_RESETB,d0
        and.b   #$01,d0
        bne     .ready
        move.b  #$01,GA_RESETB
        subq.l  #1,d7
        bne     .run
        bra     brin_out
.ready:
        ; the sub BIOS boots on INT2 ticks; pulse one every ~poll while
        ; waiting for the stub to write 'R' (ready) / 'U' (no/unformatted
        ; backup RAM) into its comm flag
        move.l  #40000,d7
.alive: move.w  GA_RESET,d0
        ori.w   #$0100,d0
        move.w  d0,GA_RESET         ; IFL2: sub INT2
        move.w  #200,d0
.idl:   dbra    d0,.idl
        move.b  GA_CFLAG_S,d0
        cmp.b   #'R',d0
        beq     .up
        cmp.b   #'U',d0
        beq     brin_out            ; CD there, but no usable backup RAM
        subq.l  #1,d7
        bne     .alive
        bra     brin_out            ; stub never came up: treat as no CD
.up:    st      v_bram_pres(a4)
        st      v_bram_rt(a4)       ; vblank ISR now feeds INT2
brin_out:
        sf      VARS+v_brprobe      ; disarm the bus-error recovery
        movem.l (sp)+,d0-d7/a0-a3/a4
        rts

; brin_sig - a3 = candidate: Z set when (a3+$6D) is "SEGA" or "WOND"
brin_sig:
        cmp.l   #'SEGA',$6D(a3)
        beq     .yes
        cmp.l   #'WOND',$6D(a3)
.yes:   rts

; ============================================================================
; Kosinski decompression - a0 = src, a1 = dst (the sub BIOS images in
; every CD BIOS ROM are Kosinski-compressed). Standard reference
; decompressor, registers d0-d6 trashed.
; ============================================================================
kos_decomp:
        movem.l d0-d6/a0-a1,-(sp)
        subq.l  #2,sp
        move.b  (a0)+,1(sp)
        move.b  (a0)+,(sp)
        move.w  (sp),d5             ; first descriptor field
        moveq   #15,d4
.loop:  lsr.w   #1,d5
        move    sr,d6
        dbra    d4,.chk
        move.b  (a0)+,1(sp)
        move.b  (a0)+,(sp)
        move.w  (sp),d5
        moveq   #15,d4
.chk:   move    d6,ccr
        bcc     .rle
        move.b  (a0)+,(a1)+         ; literal
        bra     .loop
.rle:   moveq   #0,d3
        lsr.w   #1,d5
        move    sr,d6
        dbra    d4,.chk2
        move.b  (a0)+,1(sp)
        move.b  (a0)+,(sp)
        move.w  (sp),d5
        moveq   #15,d4
.chk2:  move    d6,ccr
        bcs     .sep
        lsr.w   #1,d5               ; inline match: 2-bit count
        dbra    d4,.b1
        move.b  (a0)+,1(sp)
        move.b  (a0)+,(sp)
        move.w  (sp),d5
        moveq   #15,d4
.b1:    roxl.w  #1,d3
        lsr.w   #1,d5
        dbra    d4,.b2
        move.b  (a0)+,1(sp)
        move.b  (a0)+,(sp)
        move.w  (sp),d5
        moveq   #15,d4
.b2:    roxl.w  #1,d3
        addq.w  #1,d3
        moveq   #-1,d2
        move.b  (a0)+,d2            ; 8-bit offset
        bra     .copy
.sep:   move.b  (a0)+,d0            ; full match: 13-bit offset
        move.b  (a0)+,d1
        moveq   #-1,d2
        move.b  d1,d2
        lsl.w   #5,d2
        move.b  d0,d2
        andi.w  #7,d1
        beq     .sep2
        move.b  d1,d3
        addq.w  #1,d3
.copy:  move.b  (a1,d2.w),d0
        move.b  d0,(a1)+
        dbra    d3,.copy
        bra     .loop
.sep2:  move.b  (a0)+,d1
        beq     .done               ; 0 = end of stream
        cmpi.b  #1,d1
        beq     .loop               ; 1 = fetch a new descriptor
        move.b  d1,d3
        bra     .copy
.done:  addq.l  #2,sp
        movem.l (sp)+,d0-d6/a0-a1
        rts

; ============================================================================
; RPC transport (main side). bram_wr -> a3 = the staging area: real Word
; RAM or the fake's RAM buffer - the ONLY place the two transports
; diverge besides the submit call itself.
; ============================================================================

; bram_wr - a3 = staging base. Preserves everything else.
bram_wr:
        move.b  VARS+v_bram_rt,d0
        beq     .fake
        lea     CD_WORDRAM,a3
        rts
.fake:  lea     VARS+v_brwr,a3
        rts

; bram_acquire - wait (tick-bounded) until main owns Word RAM (RET=1).
; -> d0 = 0 ok / -1 timeout. No-op for the fake transport.
bram_acquire:
        move.b  VARS+v_bram_rt,d0
        beq     .ok
        movem.l d1-d2/a4,-(sp)
        lea     VARS,a4
        move.l  v_ticks(a4),d2
        add.l   #180,d2             ; 3s
.wait:  move.b  GA_MEMB,d0
        and.b   #$01,d0             ; RET
        bne     .got
        move.l  v_ticks(a4),d1
        cmp.l   d2,d1
        blt     .wait
        movem.l (sp)+,d1-d2/a4
        moveq   #-1,d0
        rts
.got:   movem.l (sp)+,d1-d2/a4
.ok:    moveq   #0,d0
        rts

; bram_rpc - d0.w = op (staging area already filled by the caller)
; -> d0 = 0 ok / -1 error or timeout
bram_rpc:
        move.b  VARS+v_bram_rt,d1
        beq     bram_fake_rpc
        movem.l d2-d3/a4,-(sp)
        lea     VARS,a4
        move.w  d0,GA_CMD0
        move.b  v_brseq(a4),d3
.sq:    addq.b  #1,d3
        beq     .sq                 ; sequence skips 0 (0 = idle)
        cmp.b   GA_CFLAG_S,d3       ; never reuse the live ack value
        beq     .sq                 ; (the sub would skip the command)
        move.b  d3,v_brseq(a4)
        move.b  d3,GA_CFLAG_M
        bset    #1,GA_MEMB          ; DMNA: hand Word RAM to the sub
        ; wait for the ack (sub copies the sequence into its flag)
        move.l  v_ticks(a4),d2
        add.l   #180,d2
.wait:  move.b  GA_CFLAG_S,d0
        cmp.b   d3,d0
        beq     .acked
        move.l  v_ticks(a4),d1
        cmp.l   d2,d1
        blt     .wait
        movem.l (sp)+,d2-d3/a4
        moveq   #-1,d0
        rts
.acked: bsr     bram_acquire        ; results come back with Word RAM
        tst.w   d0
        bmi     .fail
        move.w  GA_STAT0,d0
        beq     .done
.fail:  movem.l (sp)+,d2-d3/a4
        moveq   #-1,d0
        rts
.done:  movem.l (sp)+,d2-d3/a4
        moveq   #0,d0
        rts

; ============================================================================
; Main-side API over the RPC. The directory cache (v_brdir) holds up to
; BR_MAXDIR entries of 16 bytes in the BRMDIR shape: name[11]+NUL,
; start.w, blocks.w.
; ============================================================================

; bram_refresh - LIST into the cache. -> d0 = 0 ok / -1
bram_refresh:
        movem.l d1-d2/a0-a1/a3/a4,-(sp)
        lea     VARS,a4
        clr.w   v_brcount(a4)
        bsr     bram_acquire
        tst.w   d0
        bmi     .out
        moveq   #BROP_LIST,d0
        bsr     bram_rpc
        tst.w   d0
        bmi     .out
        bsr     bram_wr
        move.w  (a3)+,d1            ; count
        cmp.w   #BR_MAXDIR,d1
        bls     .cnt
        moveq   #BR_MAXDIR,d1
.cnt:   move.w  d1,v_brcount(a4)
        beq     .ok
        lea     v_brdir(a4),a1
        lsl.w   #4,d1               ; *16 bytes
        subq.w  #1,d1
.cp:    move.b  (a3)+,(a1)+
        dbra    d1,.cp
.ok:    moveq   #0,d0
.out:   movem.l (sp)+,d1-d2/a0-a1/a3/a4
        rts

; bram_count -> d0.w
bram_count:
        move.w  VARS+v_brcount,d0
        rts

; bram_entry - d0 = index -> a1 = cache entry. Preserves d0.
bram_entry:
        move.w  d0,-(sp)
        lsl.w   #4,d0
        lea     VARS+v_brdir,a1
        lea     (a1,d0.w),a1
        move.w  (sp)+,d0
        rts

; bram_name - d0 = index, a0 = 13-byte dest (11 chars + NUL, trailing
; spaces trimmed)
bram_name:
        movem.l d0-d2/a0-a1,-(sp)
        bsr     bram_entry
        moveq   #10,d2
.cp:    move.b  (a1)+,(a0)+
        dbra    d2,.cp
        clr.b   (a0)
        moveq   #10,d2              ; trim trailing spaces
.trim:  move.b  -(a0),d1
        cmp.b   #' ',d1
        bne     .done
        clr.b   (a0)
        dbra    d2,.trim
.done:  movem.l (sp)+,d0-d2/a0-a1
        rts

; bram_size - d0 = index -> d0.w = approx bytes (blocks * 64)
bram_size:
        move.l  a1,-(sp)
        bsr     bram_entry
        move.w  14(a1),d0           ; blocks
        lsl.w   #6,d0
        move.l  (sp)+,a1
        rts

; bram_norm - a0 = 12-byte UnoDOS name -> 11 normalized chars at a1:
; '.' -> '_', NULs -> spaces, truncated/padded to exactly 11.
bram_norm:
        movem.l d0-d2/a0-a1,-(sp)
        moveq   #0,d2               ; source exhausted flag
        moveq   #10,d1
.ch:    moveq   #' ',d0
        tst.b   d2
        bne     .put
        move.b  (a0)+,d0
        bne     .map
        st      d2                  ; hit the NUL: pad from here on
        moveq   #' ',d0
        bra     .put
.map:   cmp.b   #'.',d0
        bne     .put
        moveq   #'_',d0
.put:   move.b  d0,(a1)+
        dbra    d1,.ch
        movem.l (sp)+,d0-d2/a0-a1
        rts

; bram_read - d0 = index, a0 = 12-byte name dest, a1 = data dest,
; d2 = max bytes -> d0.w = bytes read (0 on failure)
bram_read:
        movem.l d1-d5/a0-a3,-(sp)
        move.w  d2,d5               ; max
        move.l  a1,a2               ; data dest
        bsr     bram_acquire
        tst.w   d0
        bmi     .fail
        bsr     bram_entry          ; a1 = cache entry
        move.l  a1,d4
        bsr     bram_wr
        move.l  d4,a1
        moveq   #10,d1              ; stage the 11-char BRAM name
        move.l  a3,d4
.nm:    move.b  (a1)+,(a3)+
        dbra    d1,.nm
        clr.b   (a3)
        move.l  d4,a3
        moveq   #BROP_READ,d0
        bsr     bram_rpc
        tst.w   d0
        bmi     .fail
        bsr     bram_wr
        move.w  (a3),d3             ; blocks read
        lsl.w   #6,d3               ; payload cap in bytes
        beq     .fail
        ; UnoDOS header? len.w at +16 must fit inside blocks*64-14
        move.w  16(a3),d1           ; wr+4+12: stored byte length
        move.w  d3,d2
        sub.w   #14,d2
        cmp.w   d2,d1
        bhi     .raw
        ; restore the original 12-char name + exact length
        lea     4(a3),a1
        moveq   #11,d2
.onm:   move.b  (a1)+,(a0)+
        dbra    d2,.onm
        lea     18(a3),a1           ; data
        bra     .clamp
.raw:   ; foreign file: keep the BRAM name (caller already has it via
        ; bram_name), deliver the whole payload
        move.w  d3,d1
        lea     4(a3),a1
.clamp: cmp.w   d5,d1
        bls     .len
        move.w  d5,d1
.len:   move.w  d1,d2
        beq     .zero
        subq.w  #1,d2
.cp:    move.b  (a1)+,(a2)+
        dbra    d2,.cp
.zero:  move.w  d1,d0
        movem.l (sp)+,d1-d5/a0-a3
        rts
.fail:  moveq   #0,d0
        movem.l (sp)+,d1-d5/a0-a3
        rts

; bram_save - a0 = 12-byte UnoDOS name, a1 = data, d1.w = len
; -> d0 = 0 ok / -1 (refreshes the listing on success)
bram_save:
        movem.l d1-d4/a0-a3,-(sp)
        move.w  d1,d4               ; len
        move.l  a1,a2               ; data
        bsr     bram_acquire
        tst.w   d0
        bmi     .fail
        move.l  a0,d3
        bsr     bram_wr
        move.l  d3,a0
        move.l  a3,a1               ; wr+0: normalized BRAM name (11)
        bsr     bram_norm
        move.b  #0,11(a3)
        move.w  d4,d0               ; wr+12: blocks = ceil((len+14)/64)
        add.w   #14+63,d0
        lsr.w   #6,d0
        move.w  d0,12(a3)
        ; wr+16: payload = original name[12] + len.w + data
        lea     16(a3),a1
        moveq   #11,d2
.nm:    move.b  (a0)+,(a1)+
        dbra    d2,.nm
        move.w  d4,(a1)+
        move.w  d4,d2
        beq     .sent
        subq.w  #1,d2
.cp:    move.b  (a2)+,(a1)+
        dbra    d2,.cp
.sent:  moveq   #BROP_WRITE,d0
        bsr     bram_rpc
        tst.w   d0
        bmi     .fail
        bsr     bram_refresh
        moveq   #0,d0
        movem.l (sp)+,d1-d4/a0-a3
        rts
.fail:  moveq   #-1,d0
        movem.l (sp)+,d1-d4/a0-a3
        rts

; bram_delete - d0 = index (refreshes the listing)
bram_delete:
        movem.l d0-d2/a0-a1/a3,-(sp)
        bsr     bram_acquire
        tst.w   d0
        bmi     .out
        bsr     bram_entry          ; a1 = entry
        bsr     bram_wr
        moveq   #10,d1
        move.l  a3,d2
.nm:    move.b  (a1)+,(a3)+
        dbra    d1,.nm
        clr.b   (a3)
        move.l  d2,a3
        moveq   #BROP_DEL,d0
        bsr     bram_rpc
        bsr     bram_refresh
.out:   movem.l (sp)+,d0-d2/a0-a1/a3
        rts

; ============================================================================
; Volume dispatch - the Files app and Notepad F1 talk to the active
; volume (v_files_vol: 0 = cartridge SRAM, 1 = Sega CD BRAM) through
; these. Same register contracts as the sram_* originals.
; ============================================================================
store_count:
        tst.w   VARS+v_files_vol
        bne     bram_count
        bra     sram_count
store_name:                         ; d0 = index, a0 = 13-byte dest
        tst.w   VARS+v_files_vol
        bne     bram_name
        bra     sram_name
store_size:                         ; d0 = index -> d0.w
        tst.w   VARS+v_files_vol
        bne     bram_size
        bra     sram_size
store_delete:                       ; d0 = index
        tst.w   VARS+v_files_vol
        bne     bram_delete
        bra     sram_delete
; store_open_read - d0 = index, a0 = 12-byte name dest (pre-filled by
; the caller from the listing; the BRAM path overwrites it from the
; payload header on UnoDOS files), a1 = dest, d2 = max -> d0 = bytes
store_open_read:
        tst.w   VARS+v_files_vol
        bne     bram_read
        bra     sram_read
; store_save - a0 = 12-byte name, a1 = src, d1.w = len -> d0 = 0/-1
store_save:
        tst.w   VARS+v_files_vol
        bne     bram_save
        bra     sram_save

; ============================================================================
; Fake transport (AUTOTEST_BRAM + any build with no CD): a RAM-backed
; block store with the same op semantics the sub stub implements over
; _BURAM - 64-byte blocks, 11-char names, BRMDIR-shaped listings.
; Store layout: v_brfk_cnt.w, v_brfk_dir = 8 x 16 (name[11]+NUL,
; start.w block index, blocks.w), v_brfk_heap = BRFK_BLKS x 64.
; ============================================================================
bram_fake_rpc:
        movem.l d1-d7/a0-a3/a4,-(sp)
        lea     VARS,a4
        move.w  d0,d7               ; op (bram_wr clobbers d0)
        bsr     bram_wr             ; a3 = staging
        cmp.w   #BROP_LIST,d7
        beq     .list
        cmp.w   #BROP_READ,d7
        beq     .read
        cmp.w   #BROP_WRITE,d7
        beq     .write
        cmp.w   #BROP_DEL,d7
        beq     .del
        bra     .err
        ; ---- LIST: count.w + 16-byte entries
.list:  move.w  v_brfk_cnt(a4),d1
        move.w  d1,(a3)+
        beq     .ok
        lea     v_brfk_dir(a4),a0
        lsl.w   #4,d1
        subq.w  #1,d1
.lcp:   move.b  (a0)+,(a3)+
        dbra    d1,.lcp
        bra     .ok
        ; ---- READ: name at wr+0 -> blocks.w at wr+0, payload at wr+4
.read:  bsr     brfk_find           ; a3+0 name -> d1 = entry index or -1
        bmi     .err
        move.w  d1,d0
        bsr     brfk_entry          ; a1 = dir entry
        move.w  14(a1),d2           ; blocks
        move.w  12(a1),d3           ; start block
        move.w  d2,(a3)
        lea     4(a3),a0            ; dest
        bsr     brfk_blkptr         ; d3 -> a2 = heap ptr
        move.w  d2,d1
        lsl.w   #6,d1
        beq     .ok
        subq.w  #1,d1
.rcp:   move.b  (a2)+,(a0)+
        dbra    d1,.rcp
        bra     .ok
        ; ---- WRITE: name wr+0, blocks.w wr+12, data wr+16
.write: bsr     brfk_find
        bmi     .wnew
        move.w  d1,d0
        bsr     brfk_delete         ; overwrite = delete + append
.wnew:  move.w  v_brfk_cnt(a4),d4
        cmp.w   #BR_MAXDIR,d4
        bhs     .err
        move.w  12(a3),d2           ; blocks wanted
        bsr     brfk_used           ; -> d6 = blocks in use
        move.w  d6,d0
        add.w   d2,d0
        cmp.w   #BRFK_BLKS,d0
        bhi     .err
        ; append: entry d4 = name, start=d6, blocks=d2
        move.w  d4,d0
        bsr     brfk_entry
        moveq   #10,d1
        move.l  a3,a0
.wnm:   move.b  (a0)+,(a1)+
        dbra    d1,.wnm
        clr.b   (a1)+
        move.w  d6,(a1)+            ; start
        move.w  d2,(a1)             ; blocks
        move.w  d6,d3
        bsr     brfk_blkptr         ; a2 = heap dest
        lea     16(a3),a0
        move.w  d2,d1
        lsl.w   #6,d1
        beq     .wcnt
        subq.w  #1,d1
.wcp:   move.b  (a0)+,(a2)+
        dbra    d1,.wcp
.wcnt:  addq.w  #1,v_brfk_cnt(a4)
        bra     .ok
        ; ---- DELETE: name at wr+0
.del:   bsr     brfk_find
        bmi     .err
        move.w  d1,d0
        bsr     brfk_delete
        bra     .ok
.ok:    moveq   #0,d0
        movem.l (sp)+,d1-d7/a0-a3/a4
        rts
.err:   moveq   #-1,d0
        movem.l (sp)+,d1-d7/a0-a3/a4
        rts

; brfk_entry - d0 = index -> a1 (preserves d0)
brfk_entry:
        move.w  d0,-(sp)
        lsl.w   #4,d0
        lea     VARS+v_brfk_dir,a1
        lea     (a1,d0.w),a1
        move.w  (sp)+,d0
        rts

; brfk_blkptr - d3 = block index -> a2
brfk_blkptr:
        move.w  d3,-(sp)
        lsl.w   #6,d3
        lea     VARS+v_brfk_heap,a2
        lea     (a2,d3.w),a2
        move.w  (sp)+,d3
        rts

; brfk_find - 11-char name at (a3) -> d1 = index or -1 (N flag mirrors)
brfk_find:
        movem.l d0/d2/a0-a1,-(sp)
        moveq   #0,d1
.ent:   cmp.w   VARS+v_brfk_cnt,d1
        bge     .miss
        move.w  d1,d0
        bsr     brfk_entry
        move.l  a3,a0
        moveq   #0,d2
.ch:    move.b  (a0,d2.w),d0
        cmp.b   (a1,d2.w),d0
        bne     .next
        addq.w  #1,d2
        cmp.w   #11,d2
        blt     .ch
        bra     .hit
.next:  addq.w  #1,d1
        bra     .ent
.miss:  moveq   #-1,d1
.hit:   movem.l (sp)+,d0/d2/a0-a1
        tst.w   d1
        rts

; brfk_used -> d6 = total blocks allocated
brfk_used:
        movem.l d0-d1/a1,-(sp)
        moveq   #0,d6
        moveq   #0,d1
.e:     cmp.w   VARS+v_brfk_cnt,d1
        bge     .done
        move.w  d1,d0
        bsr     brfk_entry
        add.w   14(a1),d6
        addq.w  #1,d1
        bra     .e
.done:  movem.l (sp)+,d0-d1/a1
        rts

; brfk_delete - d0 = entry index: compact the heap + directory
brfk_delete:
        movem.l d0-d7/a0-a2,-(sp)
        move.w  d0,d7               ; victim index
        bsr     brfk_entry
        move.w  12(a1),d5           ; victim start block
        move.w  14(a1),d4           ; victim block count
        bsr     brfk_used           ; -> d6 = total in use
        ; heap compaction: copy blocks [start+count .. used) down to start
        move.w  d5,d0               ; dst block
        move.w  d5,d1
        add.w   d4,d1               ; src block
.cw:    cmp.w   d6,d1
        bhs     .fixup
        move.w  d1,d3
        bsr     brfk_blkptr
        move.l  a2,a0               ; src
        move.w  d0,d3
        bsr     brfk_blkptr         ; a2 = dst
        moveq   #64-1,d2
.bcp:   move.b  (a0)+,(a2)+
        dbra    d2,.bcp
        addq.w  #1,d0
        addq.w  #1,d1
        bra     .cw
.fixup: ; entries whose start sits above the victim move down d4 blocks
        moveq   #0,d1
.fx:    cmp.w   VARS+v_brfk_cnt,d1
        bge     .shift
        move.w  d1,d0
        bsr     brfk_entry
        move.w  12(a1),d0
        cmp.w   d5,d0
        bls     .fnx
        sub.w   d4,d0
        move.w  d0,12(a1)
.fnx:   addq.w  #1,d1
        bra     .fx
.shift: ; directory entries [victim+1 .. count) move down one slot
        move.w  d7,d1
.sh:    addq.w  #1,d1
        cmp.w   VARS+v_brfk_cnt,d1
        bge     .cnt
        move.w  d1,d0
        bsr     brfk_entry
        move.l  a1,a0
        lea     -16(a1),a2
        moveq   #16-1,d2
.scp:   move.b  (a0)+,(a2)+
        dbra    d2,.scp
        bra     .sh
.cnt:   subq.w  #1,VARS+v_brfk_cnt
        movem.l (sp)+,d0-d7/a0-a2
        rts

; ============================================================================
; Sub-CPU stub - copied to PRG-RAM $6000, entered by the sub BIOS via
; the standard SP module header. Position-independent (pc-relative);
; the BIOS jump table and gate-array registers are fixed sub-side
; addresses ($5F16.w trap, $8000.w block). Scratch lives at PRG-RAM
; $7800 (above the stub, below nothing).
;
; Sub-side staging: Word RAM 2M at $080000. Comm: cmd words $8010+,
; status $8020+, flags $800E (main) / $800F (sub).
; ============================================================================
SUB_WORK    equ $7800               ; BRMINIT scratch ($640)
SUB_STR     equ $7E40               ; BRMINIT string buffer (12)
SUB_NAME    equ $7E50               ; staged filename (12)
SUB_FINFO   equ $7E60               ; BRMWRITE file info (14)
SUB_WRAM    equ $080000             ; Word RAM, sub side (2M)
; sub-side gate array registers (abs.w sign-extended addressing)
SGA_MEM     equ $FFFF8003           ; memory mode (MODE/DMNA/RET)
SGA_FLG_M   equ $FFFF800E           ; main comm flag (RO here)
SGA_FLG_S   equ $FFFF800F           ; sub comm flag (RW here)
SGA_CMD0    equ $FFFF8010           ; command words (RO here)
SGA_STAT0   equ $FFFF8020           ; status words (RW here)

bram_sub_start:
        dc.b    "MAIN-SUBCPU",0     ; module name + version/type
        dc.w    $0001,$0000
        dc.l    0                   ; next module
        dc.l    0                   ; size
        dc.l    $20                 ; offset to the jump table
        dc.l    0                   ; work RAM
        ; jump table (offsets from the table itself)
        dc.w    bsp_init-bram_sub_start-$20
        dc.w    bsp_main-bram_sub_start-$20
        dc.w    bsp_null-bram_sub_start-$20
        dc.w    bsp_null-bram_sub_start-$20
        dc.w    0

bsp_init:
        andi.b  #$E2,(SGA_MEM).w      ; priority off, 2M mode
        ori.b   #$01,(SGA_MEM).w      ; RET: Word RAM to the main CPU
        rts
bsp_null:
        rts

bsp_main:
        ; backup RAM up? BRMINIT: cc = Sega-formatted RAM present
        moveq   #0,d0
        lea     SUB_WORK,a0
        lea     SUB_STR,a1
        jsr     ($5F16).w
        bcs     .noram
        move.b  #'R',(SGA_FLG_S).w      ; READY
        bra     .loop
.noram: move.b  #'U',(SGA_FLG_S).w      ; unformatted / no RAM: report + park
.park:  bra     .park
.loop:  ; wait for a new command sequence from the main CPU
        move.b  (SGA_FLG_M).w,d7
        beq     .loop
        cmp.b   (SGA_FLG_S).w,d7
        beq     .loop
        move.w  (SGA_CMD0).w,d0        ; op
        cmp.w   #BROP_LIST,d0
        beq     .list
        cmp.w   #BROP_READ,d0
        beq     .read
        cmp.w   #BROP_WRITE,d0
        beq     .write
        cmp.w   #BROP_DEL,d0
        beq     .del
        bra     .err
        ; ---- LIST: BRMSTAT for the count, one BRMDIR for the entries
.list:  moveq   #1,d0               ; BRMSTAT
        lea     SUB_STR,a1
        jsr     ($5F16).w
        cmp.w   #8,d1
        bls     .lcnt
        moveq   #8,d1
.lcnt:  lea     SUB_WRAM,a2
        move.w  d1,(a2)+
        moveq   #7,d0               ; BRMDIR: skip 0, 8-entry buffer
        moveq   #0,d1               ; (carry = more files than fit; the
        move.w  #8*16,d1            ;  first 8 entries still land)
        lea     bsp_star(pc),a0
        move.l  a2,a1
        jsr     ($5F16).w
        bra     .okst
        ; ---- READ: name at WR+0 -> blocks at WR+0, data at WR+4
.read:  bsr     bsp_getname
        moveq   #2,d0               ; BRMSERCH (size for the reply)
        lea     SUB_NAME,a0
        jsr     ($5F16).w
        bcs     .err
        move.w  d0,SUB_WRAM
        moveq   #3,d0               ; BRMREAD
        lea     SUB_NAME,a0
        lea     SUB_WRAM+4,a1
        jsr     ($5F16).w
        bcs     .err
        bra     .okst
        ; ---- WRITE: name WR+0, blocks.w WR+12, data WR+16
.write: bsr     bsp_getname
        moveq   #2,d0               ; overwrite: BRMSERCH + BRMDEL
        lea     SUB_NAME,a0
        jsr     ($5F16).w
        bcs     .wnew
        moveq   #5,d0               ; BRMDEL
        lea     SUB_NAME,a0
        jsr     ($5F16).w
.wnew:  lea     SUB_FINFO,a1
        lea     SUB_NAME,a0
        moveq   #10,d1
.fi:    move.b  (a0)+,(a1)+
        dbra    d1,.fi
        move.b  #0,(a1)+            ; mode: normal (64-byte blocks)
        move.w  SUB_WRAM+12,(a1)    ; blocks
        moveq   #4,d0               ; BRMWRITE (d1 must be 0, TB#1)
        moveq   #0,d1
        lea     SUB_FINFO,a0
        lea     SUB_WRAM+16,a1
        jsr     ($5F16).w
        bcs     .err
        bra     .okst
        ; ---- DELETE: name at WR+0
.del:   bsr     bsp_getname
        moveq   #5,d0               ; BRMDEL
        lea     SUB_NAME,a0
        jsr     ($5F16).w
        bcs     .err
        bra     .okst
.okst:  move.w  #0,(SGA_STAT0).w
        bra     .ack
.err:   move.w  #$FFFF,(SGA_STAT0).w
.ack:   ori.b   #$01,(SGA_MEM).w      ; RET: Word RAM back to main
        move.b  d7,(SGA_FLG_S).w        ; ack the sequence
        bra     .loop

; bsp_getname - copy the staged name (Word RAM +0) to SUB_NAME + NUL
bsp_getname:
        lea     SUB_WRAM,a0
        lea     SUB_NAME,a1
        moveq   #10,d1
.cp:    move.b  (a0)+,(a1)+
        dbra    d1,.cp
        clr.b   (a1)
        rts

bsp_star:
        dc.b    "*",0
        even
bram_sub_end:
