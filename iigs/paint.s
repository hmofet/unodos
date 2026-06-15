; ============================================================================
; UnoDOS/Apple IIGS - Paint disk app (proc 6).  Loaded into SLOT_PAINT.
; ============================================================================
.p816
.smart +
.include "sys.inc"
.include "kernel_api.inc"

.segment "CODE"
.org SLOT_PAINT
        jmp paint_draw           ; VEC_DRAW
        jmp paint_key            ; VEC_KEY
        jmp paint_tick           ; VEC_TICK
        jmp paint_start          ; VEC_START

.include "paint.i"
