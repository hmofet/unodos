; ============================================================================
; UnoDOS/Apple IIGS - Tracker disk app (proc 8).  Loaded into SLOT_TRACKER.
; 4-voice DOC playback via the kernel snd_note/snd_off (kernel_api.inc).
; ============================================================================
.p816
.smart +
.include "sys.inc"
.include "kernel_api.inc"

.segment "CODE"
.org SLOT_TRACKER
        jmp tracker_draw         ; VEC_DRAW
        jmp tracker_key          ; VEC_KEY
        jmp tracker_tick         ; VEC_TICK
        jmp tracker_start        ; VEC_START

.include "tracker.i"
