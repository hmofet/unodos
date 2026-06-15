; ============================================================================
; UnoDOS/SNES cooperative scheduler (milestone 3).
;
; VERDICT (engineering decision, documented in HANDOFF SS6): the Genesis
; scheduler (genesis/scheduler.i) gives each window a TASK with its own
; private ~1 KB stack and context-switches between them. That model is
; INFEASIBLE on this port: the 65816 hardware stack must live in bank 0
; ($00:0000-$1FFF = the low-8 KB WRAM mirror), and that 8 KB is essentially
; full here -
;   $0000-00FF main DP   $0100-01FF NMI DP   $0200-03FF VARS
;   $0400-0BFF Notepad buffer (2 KB)         $0C00-0FFF Dostris + Pac-Man
;   $1000-17FF tilemap shadow                $1800-19FF palette/Tracker/Paint
;   $1A00-1C1F OAM shadow                    $1Fxx hardware stack
; - leaving ~736 bytes free, far short of even ONE 1 KB task stack, let alone
; the Genesis's six.
;
; So the SNES port schedules COOPERATIVELY BY TICKS instead: every app exposes
; a *_tick entry that the main loop runs once per frame. This delivers the
; same observable behaviour the per-task scheduler exists for - Music and the
; Tracker keep playing under any window, the Clock advances, the games step,
; Paint's canvas flushes - without per-task stacks or a context switch. The
; apps are already authored as tick functions (not coroutines), so this is
; their natural form, not a downgrade. sched_run is that dispatcher; the main
; loop calls it each iteration after input/window handling.
; ============================================================================

; sched_run - run one cooperative round: every app's per-frame tick.
.proc sched_run
.a16
.i16
        jsr game_tick           ; Dostris / OutLast / Pac-Man (topmost game)
        jsr music_tick          ; Music sequencer (plays under any window)
        jsr tracker_tick        ; Tracker sequencer (plays under any window)
        jsr paint_tick          ; Paint: stroke under the pointer when held
        jsr app_ticks           ; once-a-second topmost redraw (Clock)
        rts
.endproc
