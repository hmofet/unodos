; ============================================================================
; cpu8086.inc - 8086/8088 compatibility macros for UnoDOS
; ============================================================================
; UnoDOS targets the original IBM PC/XT (Intel 8088). PUSHA/POPA, shifts by
; an immediate count > 1, MOVZX, PUSH imm, etc. are 186+/386+ instructions:
; on an 8088, opcodes 0x60/0x61 silently execute as JO/JNO and corrupt
; control flow. These macros replace them with 8086-legal sequences.
;
; Include from the repo root build (cwd-relative): %include "kernel/cpu8086.inc"
; ============================================================================

%ifndef CPU8086_INC
%define CPU8086_INC

; PUSHA86/POPA86 preserve the exact 8-word PUSHA frame layout
; (AX,CX,DX,BX,SP,BP,SI,DI from high to low address). The SP slot holds a
; junk placeholder (BP) and is never read back - matching real POPA, which
; discards the saved SP. POPA86 is flag-preserving (POP does not alter
; flags), so it is safe in CF-returning paths just like real POPA.

%macro PUSHA86 0
    push ax
    push cx
    push dx
    push bx
    push bp             ; SP slot placeholder (avoids 8086 'push sp' quirk)
    push bp
    push si
    push di
%endmacro

%macro POPA86 0
    pop di
    pop si
    pop bp
    pop bx              ; discard SP slot
    pop bx
    pop dx
    pop cx
    pop ax
%endmacro

; Shift/rotate by immediate count. 8086 only encodes count=1 or count=CL.
; Repeated 1-shifts keep CF correct for the final shift and leave ZF/SF/PF
; set from the final value, matching the multi-bit forms for all flags
; that real code tests (OF after multi-bit shifts is undefined anyway).

%macro SHL_N 2
  %rep %2
    shl %1, 1
  %endrep
%endmacro

%macro SHR_N 2
  %rep %2
    shr %1, 1
  %endrep
%endmacro

%macro SAR_N 2
  %rep %2
    sar %1, 1
  %endrep
%endmacro

%macro ROL_N 2
  %rep %2
    rol %1, 1
  %endrep
%endmacro

%macro ROR_N 2
  %rep %2
    ror %1, 1
  %endrep
%endmacro

%macro RCL_N 2
  %rep %2
    rcl %1, 1
  %endrep
%endmacro

%macro RCR_N 2
  %rep %2
    rcr %1, 1
  %endrep
%endmacro

%endif ; CPU8086_INC
