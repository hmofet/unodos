; ============================================================================
; UnoDOS/Genesis milestone 6: cooperative scheduler over the window/app
; tables - the port of amiga/scheduler.i (PORT-SPEC SS4). Task 0 is the
; kernel task (input pump, drag, audio services, desktop) on the boot
; stack; every open window runs its app proc in its own task with a
; private 2 KB stack in work RAM. Context switches happen only at
; task_yield / task_wait - cooperative, no preemption, so VDP access
; never interleaves (tasks yield between events, never mid-draw).
;
; Keys for the focused window and per-frame ticks are posted into the
; task's one-slot mailbox by the kernel task; the generic task body
; dispatches them to the existing per-proc handlers. Key posts use a
; bounded yield-retry (task_post_key) so a burst of queued keys - soft
; keyboard taps, fast PS/2 typing, AUTOTEST scripts - reaches the app
; instead of being dropped on the full mailbox.
; ============================================================================

NTASKS      equ MAXWIN+1            ; task 0 = kernel
TASKSTK     equ $FF4000             ; stacks: task n tops out at
TSTK_SZ     equ 2048                ;   TASKSTK + (n+1)*2KB (max $FF7800)

TSK_SP      equ 0                   ; saved stack pointer (long)
TSK_STATE   equ 4                   ; 0 = free, 1 = ready
TSK_EVT     equ 5                   ; mailbox: 0 none, 1 key, 2 tick
TSK_D1      equ 6                   ; key ascii
TSK_D2      equ 8                   ; key rawcode
TSK_SIZE    equ 10

; task_ptr - d0 = task index -> a1 = task entry. Preserves d0.
task_ptr:
        move.w  d0,-(sp)
        mulu    #TSK_SIZE,d0
        lea     VARS+v_tasktab,a1
        lea     (a1,d0.w),a1
        move.w  (sp)+,d0
        rts

; sched_init - mark the kernel task ready, everything else free
sched_init:
        movem.l d0-d1/a1/a4,-(sp)
        lea     VARS,a4
        clr.w   v_cur_task(a4)
        moveq   #0,d0
.t:     bsr     task_ptr
        clr.b   TSK_STATE(a1)
        clr.b   TSK_EVT(a1)
        addq.w  #1,d0
        cmp.w   #NTASKS,d0
        blt     .t
        moveq   #0,d0
        bsr     task_ptr
        move.b  #1,TSK_STATE(a1)    ; kernel task always ready
        movem.l (sp)+,d0-d1/a1/a4
        rts

; task_yield - save this task's context, run the next ready task
task_yield:
        movem.l d0-d7/a0-a6,-(sp)
        move.w  VARS+v_cur_task,d0
        bsr     task_ptr
        move.l  sp,TSK_SP(a1)
        ; round-robin to the next ready task (task 0 is always ready,
        ; so this loop terminates)
        move.w  VARS+v_cur_task,d1
.next:  addq.w  #1,d1
        cmp.w   #NTASKS,d1
        blt     .ck
        moveq   #0,d1
.ck:    move.w  d1,d0
        bsr     task_ptr
        tst.b   TSK_STATE(a1)
        beq     .next
        move.w  d1,VARS+v_cur_task
        move.l  TSK_SP(a1),sp
        movem.l (sp)+,d0-d7/a0-a6
        rts

; task_spawn - d0 = window slot (0-based): create the app task
task_spawn:
        movem.l d0-d2/a0-a2,-(sp)
        addq.w  #1,d0               ; task index = slot + 1
        bsr     task_ptr
        ; build the initial frame on the task's private stack so the
        ; first task_yield into it "returns" into task_body
        move.w  d0,d1
        mulu    #TSTK_SZ,d1
        lea     TASKSTK,a0
        add.l   d1,a0
        lea     TSTK_SZ(a0),a0      ; stack top (exclusive)
        lea     task_body(pc),a2
        move.l  a2,-(a0)            ; rts target after the register pop
        moveq   #15-1,d2            ; d0-d7/a0-a6 = 15 saved registers
.z:     clr.l   -(a0)
        dbra    d2,.z
        move.l  a0,TSK_SP(a1)
        move.b  #1,TSK_STATE(a1)
        clr.b   TSK_EVT(a1)
        movem.l (sp)+,d0-d2/a0-a2
        rts

; task_body - generic app task: wait for events, dispatch to the proc.
; The window entry and proc are re-derived per event so handlers are
; free to clobber any register (no hidden d7/a2 preservation contract).
task_body:
        bsr     task_wait           ; -> d0 = type, d1 = ascii, d2 = raw
        move.w  d0,d3
        move.w  VARS+v_cur_task,d0
        subq.w  #1,d0               ; window slot
        bsr     win_ptr_raw_d0      ; a2 = window entry
        moveq   #0,d0
        move.b  WPROC(a2),d0
        cmp.w   #1,d3
        bne     .tick
        bsr     app_key             ; d1 = ascii, d2 = raw, a2 = window
        bra     task_body
.tick:  bsr     app_tick_dispatch
        bra     task_body

; task_wait - block (yielding) until this task's mailbox has an event
; -> d0 = type (1 key / 2 tick), d1 = ascii, d2 = raw
task_wait:
        movem.l a1,-(sp)
.poll:  move.w  VARS+v_cur_task,d0
        bsr     task_ptr
        moveq   #0,d0
        move.b  TSK_EVT(a1),d0
        bne     .got
        bsr     task_yield
        bra     .poll
.got:   clr.b   TSK_EVT(a1)
        moveq   #0,d1
        move.w  TSK_D1(a1),d1
        moveq   #0,d2
        move.w  TSK_D2(a1),d2
        movem.l (sp)+,a1
        rts

; task_post - d0 = window slot, d1 = type, d2 = ascii, d3 = raw
; (drops the event if the mailbox is full - used for frame ticks,
; where the next frame brings another)
task_post:
        movem.l d0/a1,-(sp)
        addq.w  #1,d0
        bsr     task_ptr
        tst.b   TSK_STATE(a1)
        beq     .out                ; no task
        tst.b   TSK_EVT(a1)
        bne     .out                ; mailbox full
        move.w  d2,TSK_D1(a1)
        move.w  d3,TSK_D2(a1)
        move.b  d1,TSK_EVT(a1)
.out:   movem.l (sp)+,d0/a1
        rts

; task_post_key - like task_post, but when the mailbox is full it
; yields (bounded) so the app drains it - key bursts survive.
; Kernel-task context only.
task_post_key:
        movem.l d0/d4/a1,-(sp)
        addq.w  #1,d0
        move.w  #100,d4             ; bounded: a wedged task drops keys
.try:   bsr     task_ptr
        tst.b   TSK_STATE(a1)
        beq     .out                ; no task
        tst.b   TSK_EVT(a1)
        beq     .post
        bsr     task_yield          ; let the app consume its mailbox
        dbra    d4,.try
        bra     .out
.post:  move.w  d2,TSK_D1(a1)
        move.w  d3,TSK_D2(a1)
        move.b  d1,TSK_EVT(a1)
.out:   movem.l (sp)+,d0/d4/a1
        rts

; task_kill - d0 = window slot: free the task
task_kill:
        movem.l d0/a1,-(sp)
        addq.w  #1,d0
        bsr     task_ptr
        clr.b   TSK_STATE(a1)
        clr.b   TSK_EVT(a1)
        movem.l (sp)+,d0/a1
        rts

; sched_ntasks -> d0 = number of live tasks (incl. the kernel task)
sched_ntasks:
        movem.l d1/a1,-(sp)
        moveq   #0,d0
        moveq   #0,d1
.t:     exg     d0,d1
        bsr     task_ptr
        exg     d0,d1
        tst.b   TSK_STATE(a1)
        beq     .n
        addq.w  #1,d0
.n:     addq.w  #1,d1
        cmp.w   #NTASKS,d1
        blt     .t
        movem.l (sp)+,d1/a1
        rts

; post_ticks - put a frame tick in the topmost window's task mailbox
post_ticks:
        movem.l d0-d3/a0/a4,-(sp)
        lea     VARS,a4
        move.w  v_zcount(a4),d0
        beq     .out
        lea     v_zlist(a4),a0
        move.w  v_zcount(a4),d1
        subq.w  #1,d1
        moveq   #0,d0
        move.b  (a0,d1.w),d0        ; topmost window slot
        moveq   #2,d1               ; tick
        moveq   #0,d2
        moveq   #0,d3
        bsr     task_post
.out:   movem.l (sp)+,d0-d3/a0/a4
        rts

; app_tick_dispatch - d0 = proc: per-frame work in task context
app_tick_dispatch:
        cmp.w   #4,d0
        beq     dostris_tick
        cmp.w   #5,d0
        beq     outlast_tick
        cmp.w   #6,d0
        beq     pacman_tick
        rts
