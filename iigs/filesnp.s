; ============================================================================
; UnoDOS/Apple IIGS - Files + Notepad disk app (procs 7 and 2).  One binary,
; one region (SLOT_FILESNP): Files and Notepad are coupled (Files opens the
; selected file into Notepad's shared NBUF and launches proc 2), so they ship
; together.  TWO vector tables in the region: Notepad at +0, Files at FILES_VEC
; (+VEC_SIZE).  The kernel loader maps proc 2 -> +0 and proc 7 -> FILES_VEC.
; ============================================================================
.p816
.smart +
.include "sys.inc"
.include "kernel_api.inc"

.segment "CODE"
.org SLOT_FILESNP
; ---- Notepad vector table (proc 2) ----
        jmp notepad_draw         ; VEC_DRAW
        jmp notepad_key          ; VEC_KEY
        jmp app_noop             ; VEC_TICK
        jmp notepad_start        ; VEC_START
; ---- Files vector table (proc 7) at FILES_VEC = SLOT_FILESNP + VEC_SIZE ----
        jmp files_draw           ; VEC_DRAW
        jmp files_key            ; VEC_KEY
        jmp app_noop             ; VEC_TICK
        jmp app_noop             ; VEC_START (Files needs no per-open init)

app_noop:
        rts

; notepad_start: per-open hook.  A fresh Notepad window starts empty unless
; Files preloaded NBUF (v_np_loaded) just before launching us; then clear the
; flag.  (Mirrors the old win_create proc-2 hook, now app-owned.)
.a16
.i16
notepad_start:
        lda v_np_loaded
        bne @keep
        jsr notepad_new
@keep:  stz v_np_loaded
        rts

.include "apps.i"
