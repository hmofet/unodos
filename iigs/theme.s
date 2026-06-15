; ============================================================================
; UnoDOS/Apple IIGS - Theme disk app (proc 3).  Loaded into SLOT_THEME.
; ============================================================================
.p816
.smart +
.include "sys.inc"
.include "kernel_api.inc"

.segment "CODE"
.org SLOT_THEME
        jmp theme_draw           ; VEC_DRAW
        jmp theme_key            ; VEC_KEY
        jmp app_noop             ; VEC_TICK  (no per-frame work)
        jmp app_noop             ; VEC_START (palette applies on key)

app_noop:
        rts

.include "theme.i"
