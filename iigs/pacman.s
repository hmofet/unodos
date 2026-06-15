; ============================================================================
; UnoDOS/Apple IIGS - Pac-Man disk app (proc 9).  Loaded into SLOT_PACMAN.
; ============================================================================
.p816
.smart +
.include "sys.inc"
.include "kernel_api.inc"

.segment "CODE"
.org SLOT_PACMAN
        jmp pacman_draw          ; VEC_DRAW
        jmp pacman_key           ; VEC_KEY
        jmp pacman_tick          ; VEC_TICK
        jmp pacman_start         ; VEC_START

.include "pacman.i"
