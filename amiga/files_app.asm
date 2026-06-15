; ============================================================================
; UnoDOS/68K Files - DISK-LOADED app (FILES.APP, proc 2). Was apps_m2.i's
; files_draw/files_key in the kernel; now a separate -Fbin binary loaded off
; DF1. Browses the boot ROM-disk and (after 'm') the DF1 FAT12 data disk;
; Enter opens the selected file in Notepad (still kernel-resident).
;
; Kernel links via APIVEC: draw_string/fill_rect/fmt_dec/redraw_topmost,
; rd_entry, fat_mount/fat_list_root, notepad_open_file/notepad_open_fat,
; launch_app... launch_app is reached through the normal proc dispatch, so the
; app just opens Notepad by asking the kernel via API. State (files_src/
; files_sel) + scratch (numbuf) live in the kernel vars; fat_tab/romdisk_count
; are kernel data.
; ============================================================================

        mc68000
        include "sysabi.i"
        include "build/kernel_api.inc"

        org     APPSLOT0
; ---- JMP table ----
        jmp     files_open(pc)
        jmp     files_draw(pc)
        jmp     files_key(pc)
        jmp     files_tick(pc)

files_open:
        rts
files_tick:
        rts

; files_draw - a2 = window
files_draw:
        movem.l d0-d7/a0-a4,-(sp)
        KDATA   a4,vars
        move.w  WX(a2),d6
        addq.w  #6,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+4,d5
        ; header
        lea     str_f_hdr(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #1,d2
        KCALL   draw_string
        moveq   #0,d7
.row:   move.b  VO_files_src(a4),d0
        beq     .romcnt
        cmp.w   VO_fat_count(a4),d7
        bge     .foot
        bra     .haveidx
.romcnt:
        KDATA   a0,romdisk_count
        cmp.w   (a0),d7
        bge     .foot
.haveidx:
        move.b  VO_files_src(a4),d0
        beq     .romsrc
        move.w  d7,d0
        mulu    #18,d0
        KDATA   a3,fat_tab
        lea     (a3,d0.w),a3
        bra     .gotentry
.romsrc:
        move.w  d7,d0
        KCALL   rd_entry            ; -> a3 = romdisk entry
.gotentry:
        move.w  d5,d1
        add.w   #12,d1
        move.w  d7,d0
        mulu    #11,d0
        add.w   d0,d1
        ; selection bar
        cmp.w   VO_files_sel(a4),d7
        bne     .name
        movem.l d1/a3,-(sp)
        move.w  WX(a2),d0
        addq.w  #2,d0
        subq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #4,d2
        moveq   #10,d3
        moveq   #3,d4
        KCALL   fill_rect
        movem.l (sp)+,d1/a3
.name:
        move.b  VO_files_src(a4),d0
        beq     .nmrom
        movem.l d1-d2/a1,-(sp)
        lea     VO_numbuf(a4),a0
        moveq   #10,d2
        move.l  a3,a1
.nmcp:  move.b  (a1)+,(a0)+
        dbra    d2,.nmcp
        clr.b   (a0)
        movem.l (sp)+,d1-d2/a1
        lea     VO_numbuf(a4),a0
        bra     .nmgo
.nmrom: move.l  a3,a0
.nmgo:  move.w  d6,d0
        movem.l d1/a3,-(sp)
        cmp.w   VO_files_sel(a4),d7
        bne     .fgw
        moveq   #0,d2
        bra     .dN
.fgw:   moveq   #3,d2
.dN:    KCALL   draw_string
        movem.l (sp)+,d1/a3
        ; size
        move.b  VO_files_src(a4),d0
        beq     .szrom
        move.l  12(a3),d0
        cmp.l   #65535,d0
        ble     .szok
        move.l  #65535,d0
.szok:  bra     .szgo
.szrom: move.w  12+4(a3),d0
.szgo:
        lea     VO_numbuf(a4),a0
        KCALL   fmt_dec
        lea     VO_numbuf(a4),a0
        move.w  d6,d0
        add.w   #112,d0
        movem.l d1/a3,-(sp)
        cmp.w   VO_files_sel(a4),d7
        bne     .fgw2
        moveq   #0,d2
        bra     .dS
.fgw2:  moveq   #1,d2
.dS:    KCALL   draw_string
        movem.l (sp)+,d1/a3
        addq.w  #1,d7
        bra     .row
.foot:
        lea     str_f_foot(pc),a0
        move.b  VO_files_src(a4),d0
        beq     .footgo
        lea     str_f_footf(pc),a0
.footgo:
        move.w  d6,d0
        move.w  WY(a2),d1
        add.w   WH(a2),d1
        sub.w   #12,d1
        moveq   #1,d2
        KCALL   draw_string
        movem.l (sp)+,d0-d7/a0-a4
        rts

; files_key - d1=ascii d2=raw -> d0=0 consumed
files_key:
        KDATA   a4,vars
        cmp.b   #$4D,d2
        beq     .down
        cmp.b   #$4C,d2
        beq     .up
        cmp.b   #13,d1
        beq     .open
        cmp.b   #'r',d1
        beq     .remount
        cmp.b   #'m',d1
        beq     .mount
        moveq   #1,d0
        rts
.mount:
.remount:
        move.b  VO_files_src(a4),d0
        bne     .domount
        cmp.b   #'m',d1
        bne     .redraw
.domount:
        KCALL   fat_mount
        tst.w   d0
        bmi     .mfail
        KCALL   fat_list_root
        KDATA   a4,vars
        st      VO_files_src(a4)
        clr.w   VO_files_sel(a4)
        bra     .redraw
.mfail: KDATA   a4,vars
        sf      VO_files_src(a4)
        clr.w   VO_files_sel(a4)
        bra     .redraw
.down:  move.w  VO_files_sel(a4),d0
        addq.w  #1,d0
        move.b  VO_files_src(a4),d1
        beq     .cntrom
        cmp.w   VO_fat_count(a4),d0
        bge     .redraw
        bra     .selok
.cntrom:
        movem.l d0,-(sp)
        KDATA   a0,romdisk_count
        move.w  (a0),d1
        movem.l (sp)+,d0
        cmp.w   d1,d0
        bge     .redraw
.selok:
        move.w  d0,VO_files_sel(a4)
        bra     .redraw
.up:    move.w  VO_files_sel(a4),d0
        subq.w  #1,d0
        blt     .redraw
        move.w  d0,VO_files_sel(a4)
        bra     .redraw
.open:
        move.b  VO_files_src(a4),d0
        bne     .openfat
        move.w  VO_files_sel(a4),d0
        KCALL   notepad_open_file
        bra     .launch
.openfat:
        KCALL   notepad_open_fat
.launch:
        moveq   #3,d0
        KCALL   launch_app          ; open/raise Notepad (proc 3)
        KCALL   redraw_topmost
        moveq   #0,d0
        rts
.redraw:
        KCALL   redraw_topmost
        moveq   #0,d0
        rts

; ---- app-private strings ----
str_f_hdr:      dc.b    "Name          Size",0
str_f_foot:     dc.b    "Enter:open  m:mount DF1 disk",0
str_f_footf:    dc.b    "FAT12 DF1   Enter:open r:refresh",0
        even
        end
