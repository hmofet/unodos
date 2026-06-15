; ============================================================================
; Kernel-resident app services. These stay in the kernel (they are NOT app UI):
;   - shared format/string helpers the apps call by address (fmt_dec,
;     str_append);
;   - redraw_topmost / files_mount (window + volume helpers used by the loader,
;     the kernel, and the apps);
;   - the AUDIO sequencers (music_tick, tracker_tick) and the game tick gate
;     (tick_wanted, games_tick): the task explicitly keeps audio in the kernel.
;     A music/tracker song keeps playing while its window is not topmost, so
;     its sequencer must run from the kernel's main-loop audio service, not
;     from a per-window task tick. The visual refresh is delegated to
;     redraw_topmost (repaints only if that app is topmost), so this file holds
;     no app DRAW code - the draw lives in the disk-loaded .APP.
; ============================================================================

; ---- fmt_dec - d0.w unsigned -> decimal digits + NUL at a0 (no suffix)
fmt_dec:
        movem.l d0-d1/a0-a1,-(sp)
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
        movem.l (sp)+,d0-d1/a0-a1
        rts

; ---- str_append - append NUL string a1 to cursor a0 (a0 -> new NUL)
str_append:
.cp:    move.b  (a1)+,(a0)+
        bne     .cp
        subq.l  #1,a0
        rts

; ---- redraw_topmost - repaint just the topmost window (frame + content)
redraw_topmost:
        movem.l d2/a2,-(sp)
        move.w  zcount(pc),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        bsr     draw_window
.out:   movem.l (sp)+,d2/a2
        rts

; ---- files_mount - mount the volume and cache the root listing (idempotent)
files_mount:
        movem.l d0-d7/a0-a3,-(sp)
        lea     vars(pc),a4
        bsr     fat_mount
        tst.w   d0
        bmi     .fail
        bsr     fat_list_root
        st      fat_mounted-vars(a4)
        bra     .out
.fail:  sf      fat_mounted-vars(a4)
        clr.w   fat_count-vars(a4)
.out:   movem.l (sp)+,d0-d7/a0-a3
        rts

; ============================================================================
; Game-tick gate + per-frame audio services (kernel main loop)
; ============================================================================

DOSTRIS_PROC equ 5
PACMAN_PROC  equ 6
OUTLAST_PROC equ 7
MUSIC_PROC   equ 9
TRACKER_PROC equ 10

; tick_wanted -> d0 != 0 when the topmost window is a game that needs the main
; loop to keep running (so gravity/AI advance without input), or when audio is
; playing. Keeps the idle optimisation intact for everything else.
tick_wanted:
        move.b  mus_playing(pc),d0  ; Music plays even when not topmost
        bne     .yes0
        move.b  tk_playing(pc),d0   ; so does the Tracker
        bne     .yes0
        move.w  zcount(pc),d0
        beq     .no
        movem.l d2/a2,-(sp)
        subq.w  #1,d0
        move.w  d0,d2
        bsr     zwin_ptr
        cmp.b   #DOSTRIS_PROC,WPROC(a2)
        bne     .pm
        move.w  dt_state(pc),d0
        cmp.w   #1,d0               ; 1 = playing
        bne     .no2
        bra     .yes
.pm:    cmp.b   #PACMAN_PROC,WPROC(a2)
        bne     .ol
        move.w  pm_state(pc),d0
        cmp.w   #1,d0               ; PMS_READY (auto-starts)
        beq     .yes
        cmp.w   #2,d0               ; PMS_PLAY
        bne     .no2
        bra     .yes
.ol:    cmp.b   #OUTLAST_PROC,WPROC(a2)
        bne     .no2
        move.w  ol_state(pc),d0
        cmp.w   #1,d0               ; driving (incl. crash recovery)
        bne     .no2
.yes:   moveq   #1,d0
        movem.l (sp)+,d2/a2
        rts
.no2:   movem.l (sp)+,d2/a2
.no:    moveq   #0,d0
        rts
.yes0:  moveq   #1,d0
        rts

; games_tick - kernel-task audio services. The games' own per-frame ticks
; (dostris/pacman/outlast) run in TASK context via post_ticks; the audio
; sequencers (game music, Music app, Tracker) run here every frame.
games_tick:
        bsr     gm_tick
        bsr     music_tick
        bsr     tracker_tick
        rts

; ---------------------------------------------------------------------------
; Music app sequencer (audio). Advances the Canon-in-D player and, if the
; Music window is topmost, refreshes it via redraw_topmost. UI (music_draw /
; music_key) lives in MUSIC.APP.
; ---------------------------------------------------------------------------
music_tick:
        move.b  mus_playing(pc),d0
        bne     .on
        rts
.on:
        movem.l d0-d3/a0/a2/a4,-(sp)
        move.l  ticks(pc),d0
        cmp.l   mus_end(pc),d0
        blt     .out
        lea     vars(pc),a4
        move.w  mus_ix(pc),d1
        addq.w  #1,d1
        cmp.w   mus_count(pc),d1
        blt     .ixok
        moveq   #0,d1               ; loop
.ixok:  move.w  d1,mus_ix-vars(a4)
        move.w  d1,d2
        mulu    #6,d2
        lea     mus_notes,a0        ; gen_data table (absolute; far from here)
        add.w   d2,a0
        moveq   #0,d2
        move.w  2(a0),d2            ; duration (PAL ticks)
        mulu    #6,d2
        divu    #5,d2
        and.l   #$FFFF,d2
        add.l   d2,d0
        move.l  d0,mus_end-vars(a4)
        move.w  (a0),d0
        bsr     snd_tone
        ; topmost-only visual refresh (PORT-SPEC SS2)
        move.w  zcount(pc),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        moveq   #0,d3
        move.b  WPROC(a2),d3
        cmp.w   #MUSIC_PROC,d3
        bne     .out
        bsr     redraw_topmost
.out:   movem.l (sp)+,d0-d3/a0/a2/a4
        rts

; music_start / music_stop - called by MUSIC.APP's key handler (exported).
music_start:
        movem.l d0-d1/a0/a4,-(sp)
        lea     vars(pc),a4
        clr.w   mus_ix-vars(a4)
        st      mus_playing-vars(a4)
        lea     mus_notes,a0
        move.w  (a0),d0             ; first period
        moveq   #0,d1
        move.w  2(a0),d1            ; first duration (PAL ticks)
        mulu    #6,d1
        divu    #5,d1
        and.l   #$FFFF,d1
        add.l   ticks(pc),d1
        move.l  d1,mus_end-vars(a4)
        bsr     snd_tone
        movem.l (sp)+,d0-d1/a0/a4
        rts
music_stop:
        movem.l d0/a4,-(sp)
        lea     vars(pc),a4
        sf      mus_playing-vars(a4)
        bsr     snd_off
        movem.l (sp)+,d0/a4
        rts

; ---------------------------------------------------------------------------
; Tracker sequencer (audio). Advances the pattern playback and triggers the
; leftmost-voice note; refresh via redraw_topmost. UI lives in TRACKER.APP.
; tk_cell / tk_periods are exported so TRACKER.APP shares this note geometry.
; ---------------------------------------------------------------------------
TK_ROWS     equ 32
TK_CHANS    equ 4

; ProTracker periods, C-2..B-3 (notes 1..24)
tk_periods:
        dc.w    428,404,381,360,339,320,302,285,269,254,240,226
        dc.w    214,202,190,180,170,160,151,143,135,127,120,113
        even

; tk_cell - d0=row d1=chan -> a0 = cell ptr (2 bytes: note, instr)
tk_cell:
        movem.l d0-d1,-(sp)
        lsl.w   #2,d0               ; row * 4 chans
        add.w   d1,d0
        add.w   d0,d0               ; * 2 bytes
        lea     tk_pat,a0           ; KBSS/vars (absolute)
        lea     (a0,d0.w),a0
        movem.l (sp)+,d0-d1
        rts

; tk_trigger_row - d0 = row: play the leftmost channel holding a note
tk_trigger_row:
        movem.l d0-d2/a0,-(sp)
        moveq   #0,d1               ; channel
.ch:    bsr     tk_cell             ; a0 = cell
        moveq   #0,d2
        move.b  (a0),d2             ; note (0 = empty)
        bne     .play
        addq.w  #1,d1
        cmp.w   #TK_CHANS,d1
        blt     .ch
        bra     .out                ; no note this row: sustain
.play:  subq.w  #1,d2
        add.w   d2,d2
        lea     tk_periods(pc),a0
        move.w  (a0,d2.w),d0
        lsl.w   #2,d0               ; 32-byte-wave period -> 8-sample x4
        bsr     snd_tone
.out:   movem.l (sp)+,d0-d2/a0
        rts

tracker_tick:
        movem.l d0-d2/a2/a4,-(sp)
        move.b  tk_playing(pc),d0
        beq     .out
        move.l  ticks(pc),d0
        sub.l   tk_last(pc),d0
        cmp.l   #7,d0
        blt     .out
        lea     vars(pc),a4
        move.l  ticks(pc),d0
        move.l  d0,tk_last-vars(a4)
        move.w  tk_prow(pc),d0
        addq.w  #1,d0
        cmp.w   #TK_ROWS,d0
        blt     .rok
        moveq   #0,d0               ; loop the pattern
.rok:   move.w  d0,tk_prow-vars(a4)
        bsr     tk_trigger_row
        ; redraw if we are the topmost window
        move.w  zcount(pc),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        cmp.b   #TRACKER_PROC,WPROC(a2)
        bne     .out
        bsr     redraw_topmost
.out:   movem.l (sp)+,d0-d2/a2/a4
        rts

; tk_stop - called by TRACKER.APP (exported): stop playback + silence
tk_stop:
        movem.l d0/a4,-(sp)
        lea     vars(pc),a4
        sf      tk_playing-vars(a4)
        bsr     snd_off
        movem.l (sp)+,d0/a4
        rts
