; ============================================================================
; UnoDOS/Apple IIGS - Dostris disk app (proc 5).  A standalone binary loaded
; from the FAT12 volume into its fixed bank-0 region (SLOT_DOSTRIS) by the
; kernel loader; the WM dispatches draw/key/tick/start through the JMP vector
; table below.  Links to the kernel purely by address (kernel_api.inc).
; ============================================================================
.p816
.smart +
.include "sys.inc"
.include "kernel_api.inc"

.segment "CODE"
.org SLOT_DOSTRIS
; ---- app ABI vector table (draw / key / tick / start) ----
        jmp dostris_draw         ; VEC_DRAW  (S2 = window entry offset)
        jmp dostris_key          ; VEC_KEY   (S0 = ascii)
        jmp game_tick            ; VEC_TICK  (per frame)
        jmp dostris_start        ; VEC_START (window open)

.include "dostris.i"
