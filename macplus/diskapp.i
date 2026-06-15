; ============================================================================
; Disk-loaded app framework (generalized from the single-app demo loader).
;
; Every windowed app except SysInfo (proc 0) and Clock (proc 1) now lives on
; the boot floppy's FAT12 volume as a separate .APP binary, NOT in the kernel.
; The window manager dispatches each window's draw / key / tick / click through
; the loaded app's entry vectors, so the kernel holds NO app UI code.
;
; MULTI-APP RESIDENCY: each app has its own fixed load region (a "slot") in the
; free RAM above the kernel/buffers and below the stack ($40000-$78000). An app
; stays resident once loaded, so several windows of different apps can be open
; at once - the windowed multitasking UX is preserved. Files and Notepad are
; coupled (Files opens a file *into* Notepad), so they ship as ONE binary that
; serves both proc 2 and proc 3 from a single slot.
;
; ABI (mirrors the C64 port's fixed-org + JMP-table discipline): an app is a
; raw 68K binary assembled at its slot address whose first four longs are JMPs:
;     slot+0   draw   (d3 = proc, a2 = window)
;     slot+4   key    (d1 = ascii, d2 = raw, d3 = proc, a2 = window) -> d0
;     slot+8   tick   (d3 = proc, a2 = window)
;     slot+12  click  (d0 = x, d1 = y, d3 = proc, a2 = window)
; The app references kernel routines and variables by ABSOLUTE address via the
; generated build/kernel_api.inc (mkapi.py), so it is linked against the kernel
; by address - no a5 service table, no PIC gymnastics. The app reads window
; geometry from a2 (WX/WY/WW/WH) and is free to clobber any register.
; ============================================================================

APP_LOAD    equ $40000              ; legacy single-slot base (kept; = slot 0)
APP_SLOT_SZ equ $4000               ; 16 KB per resident slot
NAPPSLOT    equ 9                   ; Files/Notepad, Dostris, PacMan, OutLast,
                                    ; Paint, Music, Tracker, Theme (+1 spare)
; slot region: $40000 + slot*16KB. Slot 8 tops out at $64000, well under the
; task stacks ($3C000-$3F800 are BELOW $40000) and the kernel stack ($78000).

APP_DRAW    equ 0
APP_KEY     equ 4
APP_TICK    equ 8
APP_CLICK   equ 12

; ---- proc index -> (slot, FAT name). proc 0/1 (SysInfo/Clock) are in-kernel
; and have slot $FF. Files (2) and Notepad (3) share slot 0 / FILES.APP.
proc_slot:
        ;       0    1    2  3   4  5  6  7  8  9 10 11
        ;      SysI Clk  Fi No Dem Do Pa Ou Pn Mu Tk Th
        dc.b    $FF, $FF, 0, 0, 8, 1, 2, 3, 4, 5, 6, 7
        even
app_names:                          ; one 11-char FAT name per SLOT (0..8)
        dc.b    "FILES   APP"       ; slot 0: Files + Notepad
        dc.b    "DOSTRIS APP"       ; slot 1
        dc.b    "PACMAN  APP"       ; slot 2
        dc.b    "OUTLAST APP"       ; slot 3
        dc.b    "PAINT   APP"       ; slot 4
        dc.b    "MUSIC   APP"       ; slot 5
        dc.b    "TRACKER APP"       ; slot 6
        dc.b    "THEME   APP"       ; slot 7
        dc.b    "DEMO    APP"       ; slot 8 (legacy demo, optional)
        even

; slot_addr - d0 = slot -> a0 = slot load address. Preserves d0.
slot_addr:
        move.l  d1,-(sp)
        move.l  d0,d1
        mulu    #APP_SLOT_SZ,d1
        lea     APP_LOAD,a0
        add.l   d1,a0
        move.l  (sp)+,d1
        rts

; app_slot_for - d3 = proc -> d0 = slot, or -1 if in-kernel/invalid.
app_slot_for:
        cmp.w   #12,d3
        bcc     .no
        lea     proc_slot(pc),a0
        moveq   #0,d0
        move.b  (a0,d3.w),d0
        cmp.b   #$FF,d0
        beq     .no
        rts
.no:    moveq   #-1,d0
        rts

; load_app_slot - d0 = slot. Reads SLOT.APP off the FAT12 volume into the
; slot's load region. Sets the matching app_resident bit. -> d0 = 0 ok / -1.
load_app_slot:
        movem.l d1-d7/a0-a4,-(sp)
        move.w  d0,d7               ; slot
        ; already resident?
        bsr     app_is_resident     ; d0 (slot) -> d1 = 0/1
        tst.b   d1
        bne     .ok
        ; ensure the volume is mounted
        move.b  fat_mounted(pc),d0
        bne     .mounted
        bsr     files_mount
.mounted:
        move.b  fat_mounted(pc),d0
        beq     .fail
        ; name = app_names[slot]
        move.w  d7,d0
        mulu    #11,d0
        lea     app_names(pc),a0
        lea     (a0,d0.w),a0
        bsr     fat_find_file       ; d0 = cluster/-1, d1 = size
        tst.w   d0
        bmi     .fail
        cmp.l   #APP_SLOT_SZ,d1
        ble     .szok
        move.l  #APP_SLOT_SZ,d1     ; clamp to the slot (apps are well under)
.szok:  move.w  d0,d6               ; keep the cluster (slot_addr clobbers d0)
        move.w  d7,d0
        bsr     slot_addr           ; a0 = load address
        move.l  a0,a1
        move.w  d6,d0               ; d0 = cluster again
        bsr     fat_read_file       ; d0 = cluster d1 = budget a1 = dest -> bytes
        tst.l   d0
        bmi     .fail
        move.w  d7,d0
        bsr     app_set_resident
.ok:    movem.l (sp)+,d1-d7/a0-a4
        moveq   #0,d0
        rts
.fail:  movem.l (sp)+,d1-d7/a0-a4
        moveq   #-1,d0
        rts

; app_is_resident - d0 = slot -> d1 = 0/1 (bit in app_resident_bits).
app_is_resident:
        move.l  d0,-(sp)
        lea     app_resident_bits(pc),a0
        move.w  (a0),d1
        btst    d0,d1
        sne     d1
        and.w   #1,d1
        move.l  (sp)+,d0
        rts

; app_set_resident - d0 = slot: mark resident.
app_set_resident:
        movem.l d0-d1/a0/a4,-(sp)
        lea     vars(pc),a4
        lea     app_resident_bits(pc),a0
        move.w  (a0),d1
        bset    d0,d1
        move.w  d1,app_resident_bits-vars(a4)
        movem.l (sp)+,d0-d1/a0/a4
        rts

; app_ensure - d3 = proc. Ensure that proc's app is loaded into its slot.
; -> a1 = slot base address, or a1 = 0 if in-kernel / not loadable / missing.
; Preserves d0/d1/d2/d3 and a2 (the message args the dispatchers pass through).
app_ensure:
        movem.l d0-d7/a0/a3-a6,-(sp)    ; save everything except a1/a2
        move.l  a2,-(sp)
        bsr     app_slot_for            ; d3 = proc -> d0 = slot / -1
        tst.l   d0
        bmi     .no
        move.l  d0,d7                    ; slot
        bsr     load_app_slot           ; d0 = slot -> d0 = 0 ok / -1
        tst.l   d0
        bmi     .no
        move.l  d7,d0
        bsr     slot_addr               ; a0 = slot base
        move.l  a0,a1
        move.l  (sp)+,a2
        movem.l (sp)+,d0-d7/a0/a3-a6
        rts
.no:    move.l  (sp)+,a2
        movem.l (sp)+,d0-d7/a0/a3-a6
        moveq   #0,d0
        move.l  d0,a1
        rts

; ---------------------------------------------------------------- dispatch
; The four window-event dispatchers. Each is reached from the kernel's WM with
; the proc index in d0 (draw/key/tick) and the usual message args. They load
; the app on demand and call through its fixed-org vector.

; app_disp_draw - d0 = proc, a2 = window.
app_disp_draw:
        move.w  d0,d3
        bsr     app_ensure          ; -> a1 = slot base / 0
        move.l  a1,d0
        beq     .err
        move.l  APP_DRAW(a1),a0     ; vector address from the header table
        jsr     (a0)                ; d3 = proc, a2 = window
        rts
.err:                               ; image missing: message instead of garbage
        move.w  WX(a2),d0
        addq.w  #6,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+6,d1
        lea     str_app_missing(pc),a0
        moveq   #3,d2
        bsr     draw_string
        rts

; app_disp_key - d0 = proc, d1 = ascii, d2 = raw, a2 = window -> d0 = 0/1.
; Redraws the topmost window when the app consumes the key.
app_disp_key:
        move.w  d0,d3
        bsr     app_ensure          ; -> a1 = slot base / 0
        move.l  a1,d0
        beq     .no
        move.l  APP_KEY(a1),a0
        jsr     (a0)                ; -> d0 = 0 consumed / 1 not
        tst.w   d0
        bne     .notc
        bsr     redraw_topmost
        moveq   #0,d0
        rts
.notc:  moveq   #1,d0
        rts
.no:    moveq   #1,d0
        rts

; app_disp_tick - d0 = proc, a2 = window (task context).
app_disp_tick:
        move.w  d0,d3
        bsr     app_ensure
        move.l  a1,d0
        beq     .out
        move.l  APP_TICK(a1),a0
        jsr     (a0)
.out:   rts

; app_disp_click - d0 = x, d1 = y, d3 = proc, a2 = window (Paint uses it).
; d0/d1 must reach the app vector, so test a1 without clobbering them.
app_disp_click:
        bsr     app_ensure          ; d3 = proc -> a1 = slot base (preserves d0/d1)
        cmpa.l  #0,a1
        beq     .out
        move.l  APP_CLICK(a1),a0
        jsr     (a0)
.out:   rts
