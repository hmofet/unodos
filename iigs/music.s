; ============================================================================
; UnoDOS/Apple IIGS - Music disk app (proc 4).  Loaded into SLOT_MUSIC.
; Calls the kernel-resident DOC engine (snd_note/snd_off) via kernel_api.inc.
; ============================================================================
.p816
.smart +
.include "sys.inc"
.include "kernel_api.inc"

.segment "CODE"
.org SLOT_MUSIC
        jmp music_draw           ; VEC_DRAW
        jmp app_noop             ; VEC_KEY   (no key handler)
        jmp music_tick           ; VEC_TICK
        jmp music_start          ; VEC_START

app_noop:
        rts

.include "snd.i"
