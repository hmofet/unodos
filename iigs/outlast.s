; ============================================================================
; UnoDOS/Apple IIGS - OutLast disk app (proc 10).  Loaded into SLOT_OUTLAST.
; ============================================================================
.p816
.smart +
.include "sys.inc"
.include "kernel_api.inc"

.segment "CODE"
.org SLOT_OUTLAST
        jmp outlast_draw         ; VEC_DRAW
        jmp outlast_key          ; VEC_KEY   (steer)
        jmp outlast_tick         ; VEC_TICK
        jmp outlast_start        ; VEC_START

.include "outlast.i"
