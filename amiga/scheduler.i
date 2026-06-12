; ============================================================================
; UnoDOS/68K milestone 3: cooperative scheduler over the window/app tables
; (PORT-SPEC SS4). Task 0 is the kernel task (input pump, drag, audio
; services, desktop); every open window runs its app proc in its own
; task with a private 2 KB stack. Context switches happen only at
; task_yield / task_wait - cooperative, no preemption.
;
; Keys for the focused window and per-frame ticks are posted into the
; task's one-slot mailbox by the kernel task; the generic task body
; dispatches them to the existing per-proc handlers.
; ============================================================================

NTASKS      equ MAXWIN+1            ; task 0 = kernel
TASKSTK     equ $70000               ; 7 x 2 KB private stacks (chip, free)
TSTK_SZ     equ 2048

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
        lea     task_tab(pc),a1
        lea     (a1,d0.w),a1
        move.w  (sp)+,d0
        rts

; sched_init - mark the kernel task ready, everything else free
sched_init:
        movem.l d0-d1/a1/a4,-(sp)
        lea     vars(pc),a4
        clr.w   cur_task-vars(a4)
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
        move.w  cur_task(pc),d0
        bsr     task_ptr
        move.l  sp,TSK_SP(a1)
        ; round-robin to the next ready task (task 0 is always ready,
        ; so this loop terminates)
        move.w  cur_task(pc),d1
.next:  addq.w  #1,d1
        cmp.w   #NTASKS,d1
        blt     .ck
        moveq   #0,d1
.ck:    move.w  d1,d0
        bsr     task_ptr
        tst.b   TSK_STATE(a1)
        beq     .next
        lea     vars(pc),a0
        move.w  d1,cur_task-vars(a0)
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

; task_body - generic app task: wait for events, dispatch to the proc
task_body:
        ; identify my proc from my window slot
        move.w  cur_task(pc),d2
        subq.w  #1,d2               ; window slot
        bsr     win_ptr_raw         ; a2 = window entry
        moveq   #0,d7
        move.b  WPROC(a2),d7
.loop:  bsr     task_wait           ; -> d0 = type, d1 = ascii, d2 = raw
        cmp.w   #1,d0
        bne     .tick
        move.w  d7,d0
        bsr     app_key_dispatch
        bra     .loop
.tick:  move.w  d7,d0
        bsr     app_tick_dispatch
        bra     .loop

; task_wait - block (yielding) until this task's mailbox has an event
; -> d0 = type (1 key / 2 tick), d1 = ascii, d2 = raw
task_wait:
        movem.l a1,-(sp)
.poll:  move.w  cur_task(pc),d0
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
; (drops the event if the mailbox is full - cooperative backpressure)
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

; app_key_dispatch - d0 = proc, d1 = ascii, d2 = raw
app_key_dispatch:
        cmp.w   #2,d0
        beq     .files
        cmp.w   #3,d0
        beq     .notepad
        cmp.w   #4,d0
        beq     .music
        cmp.w   #5,d0
        beq     .theme
        cmp.w   #6,d0
        beq     .dostris
        cmp.w   #7,d0
        beq     .outlast
        cmp.w   #8,d0
        beq     .pacman
        cmp.w   #9,d0
        beq     .tracker
        cmp.w   #10,d0
        beq     .paint
        rts
.files:   bra     files_key
.notepad: bra     notepad_key
.music:   bra     music_key
.theme:   bra     theme_key
.dostris: bra     dostris_key
.outlast: bra     outlast_key
.pacman:  bra     pacman_key
.tracker: bra     tracker_key
.paint:   bra     paint_key

; app_tick_dispatch - d0 = proc: per-frame work in task context
app_tick_dispatch:
        cmp.w   #6,d0
        beq     .dostris
        cmp.w   #7,d0
        beq     .outlast
        cmp.w   #8,d0
        beq     .pacman
        rts
.dostris: bra     dostris_tick
.outlast: bra     outlast_tick
.pacman:  bra     pacman_tick
