; ============================================================================
; Disk-loaded app support (proc index 4). The launcher reads an app image off
; the floppy's FAT12 volume into APP_LOAD and runs it as a windowed app - the
; same idea as the x86 launcher loading .BIN apps. The app is position-
; independent 68K code; the kernel calls its entry (image offset 0) with:
;   d0 = 0  -> draw   (a2 = window, a5 = ksys_table)
;   d0 = 1  -> key    (d1 = ascii, d2 = raw, a2 = window, a5 = ksys_table)
; and the app returns d0 = 0 consumed / 1 not (key only). All kernel services
; are reached through the a5 = ksys_table jump table, so the app needs no
; absolute kernel addresses and could load anywhere.
; ============================================================================

APP_LOAD    equ $40000              ; load address: free RAM above kernel+buffers,
                                    ; below the stack ($78000) on every target
APP_MAX     equ $00020000           ; 128 KB image cap

; ksys_table - the service pointers the disk app calls through a5
        even
ksys_table:
        dc.l    draw_string         ; 0:  a0=str d0=x d1=y d2=color
        dc.l    draw_string_bg      ; 4:  + d3=bg color
        dc.l    fill_rect           ; 8:  d0=x d1=y d2=w d3=h d4=color
        dc.l    rect_outline_fg     ; 12: d0=x d1=y d2=w d3=h
        dc.l    fat_find_file       ; 16: a0=name -> d0=cluster d1=size
        dc.l    fat_read_file       ; 20: d0=cl d1=budget a1=dest -> d0=bytes
        dc.l    ksys_get_ticks      ; 24: -> d0 = ticks since boot

ksys_get_ticks:
        move.l  ticks(pc),d0
        rts

app_name:       dc.b    "DEMO    APP"     ; 11-char 8.3 name of the disk app
                dc.b    0
        even

; load_disk_app -> d0 = 0 ok / -1. Mounts the volume if needed, then finds and
; reads the app image into APP_LOAD. Sets diskapp_loaded.
load_disk_app:
        movem.l d1-d7/a0-a4,-(sp)
        move.b  fat_mounted(pc),d0
        bne     .mounted
        bsr     files_mount
.mounted:
        move.b  fat_mounted(pc),d0
        beq     .fail
        lea     app_name(pc),a0
        bsr     fat_find_file       ; d0 = cluster / -1, d1 = size
        tst.w   d0
        bmi     .fail
        cmp.l   #APP_MAX,d1
        ble     .szok
        move.l  #APP_MAX,d1
.szok:  lea     APP_LOAD,a1
        bsr     fat_read_file       ; d0 = bytes / -1
        tst.l   d0
        bmi     .fail
        lea     vars(pc),a4
        st      diskapp_loaded-vars(a4)
        moveq   #0,d0
        bra     .out
.fail:  lea     vars(pc),a4
        sf      diskapp_loaded-vars(a4)
        moveq   #-1,d0
.out:   movem.l (sp)+,d1-d7/a0-a4
        rts

; diskapp_draw - a2 = window. Lazy-loads the image, then calls its draw entry.
diskapp_draw:
        move.b  diskapp_loaded(pc),d0
        bne     .run
        bsr     load_disk_app
        move.b  diskapp_loaded(pc),d0
        beq     .err
.run:   movem.l d0-d7/a0-a6,-(sp)
        lea     ksys_table(pc),a5
        moveq   #0,d0               ; message: draw
        jsr     APP_LOAD
        movem.l (sp)+,d0-d7/a0-a6
        rts
.err:   ; image missing: show a message instead of jumping into garbage
        move.w  WX(a2),d0
        addq.w  #6,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+6,d1
        lea     str_app_missing(pc),a0
        moveq   #3,d2
        bsr     draw_string
        rts

; diskapp_key - d1=ascii d2=raw, a2 = window -> d0 = 0 consumed / 1 not.
; Redraws the topmost window when the app consumes the key.
diskapp_key:
        move.b  diskapp_loaded(pc),d0
        beq     .no
        movem.l d1-d7/a0-a6,-(sp)
        lea     ksys_table(pc),a5
        moveq   #1,d0               ; message: key (d1/d2/a2 still set)
        jsr     APP_LOAD            ; -> d0 = 0 consumed / 1 not
        tst.w   d0
        bne     .notc
        bsr     redraw_topmost
        movem.l (sp)+,d1-d7/a0-a6
        moveq   #0,d0
        rts
.notc:  movem.l (sp)+,d1-d7/a0-a6
.no:    moveq   #1,d0
        rts
