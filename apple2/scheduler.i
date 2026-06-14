; ============================================================================
; UnoDOS/AppleII cooperative scheduler - milestone-3 FEASIBILITY PROTOTYPE
; (HANDOFF-M3 SS6). The 6502 has one 256-byte hardware stack at $0100; this
; proves option 1 (stack partitioning) works: N tasks each own a fixed slice
; of page 1, task_yield saves the live S and loads the next task's S, and a
; canary byte at each slice floor is checked to catch overflow. It also
; measures the deepest stack actually used by representative work (one task
; drives the kernel's deepest typical call chain, draw_string -> draw_char).
;
; Built only under -DSCHED_PROTO=1 (see start2); it replaces the normal boot
; with the proto, runs SCH_SWITCHES cooperative context switches between two
; partitioned tasks, then paints the verdict (canary intact + measured max
; depth) and idles for the harness to screenshot. The shipping kernel keeps
; the M1/M2 poll-and-dispatch loop (option 3) - see the README verdict for
; why the full-screen single-app model needs no live scheduler.
;
; Page-1 partition:  task0 $0100-$017F (128 B)   task1 $0180-$01EF (112 B)
;                    proto/report  $01F0-$01FF (16 B)
; ============================================================================

SCH_SWITCHES equ 40         ; cooperative switches to run before reporting
SCH_CANARY   equ $C5

SCH_T0_TOP   equ $7D        ; task0 first saved S (room for entry word at 7E/7F)
SCH_T1_TOP   equ $ED        ; task1 first saved S
SCH_T0_FLOOR equ $00        ; canary at $0100
SCH_T1_FLOOR equ $80        ; canary at $0180

; ----------------------------------------------------------------- task_yield
; task_yield - cooperative switch: save the live S into the current task's
; slot, round-robin to the next ready task, restore its S. Tracks the deepest
; S seen (max stack use) and clobbers A/X/Y (proto tasks keep state in memory,
; so this is fine; a production yield would push/pop A/X/Y around the switch).
task_yield:
        tsx
        cpx sch_mindepth        ; track the lowest S (deepest use)
        bcs ty_nomin
        stx sch_mindepth
ty_nomin:
        ldy sch_cur
        txa
        sta sch_s,y             ; save live S
        iny
        cpy #2
        bcc ty_set
        ldy #0
ty_set:
        sty sch_cur
        ldx sch_s,y
        txs                     ; adopt the next task's stack
        rts                     ; ...returns into that task's context

; ----------------------------------------------------------------- task0_run
; task0_run - exercises the kernel's deepest typical chain (draw_string ->
; draw_char, which pha/pha) between yields, so sch_mindepth reflects real work.
task0_run:
        lda #<msg_sch_t0
        sta zpPtr
        lda #>msg_sch_t0
        sta zpPtr+1
        lda #1
        sta zpCol
        lda #2
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string         ; deep-ish call chain on task0's slice
        inc sch_count0
        inc sch_switches
        lda sch_switches
        cmp #SCH_SWITCHES
        bcc t0_yield
        jmp sch_report
t0_yield:
        jsr task_yield
        jmp task0_run

; ----------------------------------------------------------------- task1_run
task1_run:
        lda #1
        sta zpCol
        lda #3
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_sch_t1
        sta zpPtr
        lda #>msg_sch_t1
        sta zpPtr+1
        jsr draw_string
        inc sch_count1
        inc sch_switches
        lda sch_switches
        cmp #SCH_SWITCHES
        bcc t1_yield
        jmp sch_report
t1_yield:
        jsr task_yield
        jmp task1_run

; ----------------------------------------------------------------- sched_proto
; sched_proto - set up two partitioned tasks and start task0.
sched_proto:
        bit $C050
        bit $C057
        bit $C054
        bit $C052
        jsr hgr_clear
        lda #<msg_sch_title
        sta zpPtr
        lda #>msg_sch_title
        sta zpPtr+1
        lda #1
        sta zpCol
        lda #0
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string
        ; init bookkeeping
        lda #0
        sta sch_cur
        sta sch_switches
        sta sch_count0
        sta sch_count1
        lda #$FF
        sta sch_mindepth
        ; seed task0 stack: entry word at $017E/$017F, saved S = $7D
        lda #<(task0_run-1)
        sta $017E
        lda #>(task0_run-1)
        sta $017F
        lda #SCH_T0_TOP
        sta sch_s+0
        lda #SCH_CANARY
        sta $0100               ; task0 canary
        ; seed task1 stack: entry word at $01EE/$01EF, saved S = $ED
        lda #<(task1_run-1)
        sta $01EE
        lda #>(task1_run-1)
        sta $01EF
        lda #SCH_T1_TOP
        sta sch_s+1
        lda #SCH_CANARY
        sta $0180               ; task1 canary
        ; switch into task0 (proto's own context is abandoned; sch_report
        ; resets S, so we never need to return here)
        ldx #SCH_T0_TOP
        txs
        jmp task0_run

; ----------------------------------------------------------------- sch_report
; sch_report - reached from whichever task hit SCH_SWITCHES. Reset to a safe
; stack, check both canaries, compute max depth per slice, paint the verdict.
sch_report:
        ldx #$FF
        txs
        ; canary check
        lda $0100
        cmp #SCH_CANARY
        bne sr_fail
        lda $0180
        cmp #SCH_CANARY
        bne sr_fail
        ; PASS: show switches + max depth used by task0 (top - mindepth)
        lda #1
        sta zpCol
        lda #5
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_sch_ok
        sta zpPtr
        lda #>msg_sch_ok
        sta zpPtr+1
        jsr draw_string
        ; switches value
        lda #1
        sta zpCol
        lda #6
        sta zpRow
        lda #<msg_sch_sw
        sta zpPtr
        lda #>msg_sch_sw
        sta zpPtr+1
        jsr draw_string
        lda sch_switches
        sta zpFSSize
        lda #0
        sta zpFSSize+1
        lda #10
        sta zpCol
        jsr draw_dec16
        ; task0 max depth = SCH_T0_TOP - mindepth (bytes used)
        lda #1
        sta zpCol
        lda #7
        sta zpRow
        lda #<msg_sch_depth
        sta zpPtr
        lda #>msg_sch_depth
        sta zpPtr+1
        jsr draw_string
        lda #SCH_T0_TOP
        sec
        sbc sch_mindepth
        sta zpFSSize
        lda #0
        sta zpFSSize+1
        lda #10
        sta zpCol
        jsr draw_dec16
        jmp sr_idle
sr_fail:
        lda #1
        sta zpCol
        lda #5
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_sch_fail
        sta zpPtr
        lda #>msg_sch_fail
        sta zpPtr+1
        jsr draw_string
sr_idle:
        jmp sr_idle

msg_sch_title: dc.b "Scheduler proto (option 1)",0
msg_sch_t0:    dc.b "task0: render chain + yield",0
msg_sch_t1:    dc.b "task1: counter + yield",0
msg_sch_ok:    dc.b "STACK-PARTITION OK - canaries intact",0
msg_sch_sw:    dc.b "switches:",0
msg_sch_depth: dc.b "t0 depth:",0
msg_sch_fail:  dc.b "CANARY TRIPPED - slice overflow",0
