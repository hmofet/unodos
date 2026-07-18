; ============================================================================
; UnoDOS/68K disk-loaded-app loader + APIVEC publisher (kernel side).
; See sysabi.i for the ABI rationale. Included by kernel.asm.
;
;   apivec_init   - fill the fixed APIVEC table with runtime addresses (boot)
;   load_app      - d0 = proc index, d1 = window slot: read APPS\<id>.APP off
;                   DF1 (FAT12) into the slot region and call its app_open.
;                   -> d0 = 0 loaded, -1 not a disk app / load failed
;   is_disk_app   - d0 = proc -> Z set (eq) if this proc is a disk-loaded app
;   app_slot_base - d1 = window slot -> a1 = slot load address
;   the three dispatch shims app_draw/app_key/app_tick route a disk app's
;   window through its slot's JMP-table vectors.
; ============================================================================

; ---- which procs are disk-loaded (the rest stay in-kernel for now). A 1 byte
; per proc, indexed by proc id; 1 = disk app.
disk_app_tab:
        dc.b    0,0             ; 0 sysinfo 1 clock (in-kernel: static)
        dc.b    1               ; 2 files   -> disk
        dc.b    1               ; 3 notepad -> disk
        dc.b    1               ; 4 music   -> disk
        dc.b    1               ; 5 theme   -> disk
        dc.b    1               ; 6 dostris -> disk
        dc.b    1               ; 7 outlast -> disk
        dc.b    1               ; 8 pacman  -> disk
        dc.b    1               ; 9 tracker -> disk
        dc.b    1               ; 10 paint  -> disk
        even

; ---- proc id -> 11-char FAT name of its .APP file (padded "NAME    APP")
app_file_tab:
        dc.b    "FILES   APP"   ; idx 0 (proc 2)
        dc.b    "THEME   APP"   ; idx 1 (proc 5)
        dc.b    "DOSTRIS APP"   ; idx 2 (proc 6)
        dc.b    "PACMAN  APP"   ; idx 3 (proc 8)
        dc.b    "NOTEPAD APP"   ; idx 4 (proc 3)
        dc.b    "MUSIC   APP"   ; idx 5 (proc 4)
        dc.b    "OUTLAST APP"   ; idx 6 (proc 7)
        dc.b    "TRACKER APP"   ; idx 7 (proc 9)
        dc.b    "PAINT   APP"   ; idx 8 (proc 10)
app_file_idx:                   ; proc -> entry index into app_file_tab (-1)
        ;       0  1  2  3  4  5  6  7  8  9 10
        dc.b    -1,-1, 0, 4, 5, 1, 2, 6, 3, 7, 8
        even

; is_disk_app - d0 = proc -> d1 = 1 if disk app else 0 (Z reflects d1).
; Preserves d0.
is_disk_app:
        move.l  a0,-(sp)
        lea     disk_app_tab(pc),a0
        moveq   #0,d1
        move.b  (a0,d0.w),d1
        move.l  (sp)+,a0
        tst.b   d1
        rts

; app_slot_base - d1 = window slot (0..MAXWIN-1) -> a1 = slot load address.
; Preserves all data registers.
app_slot_base:
        movem.l d0/d1,-(sp)
        move.w  d1,d0
        mulu    #APPSLOT_SZ,d0
        move.l  #APPSLOT0,a1
        add.l   d0,a1
        movem.l (sp)+,d0/d1
        rts

; ---- apivec_init: publish the runtime API table. Called once at boot, after
; the kernel's own code/data are in place (their relocated addresses are the
; values of the PC-relative leas below).
apivec_init:
        movem.l d0/a0-a1,-(sp)
        lea     APIVEC,a1
        ; --- routines (ordinals 0..API_NROUTINES-1) ---
        lea     draw_string(pc),a0
        move.l  a0,(API_draw_string*4)(a1)
        lea     draw_string_bg(pc),a0
        move.l  a0,(API_draw_string_bg*4)(a1)
        lea     draw_window(pc),a0
        move.l  a0,(API_draw_window*4)(a1)
        lea     fill_rect(pc),a0
        move.l  a0,(API_fill_rect*4)(a1)
        lea     clear_screen(pc),a0
        move.l  a0,(API_clear_screen*4)(a1)
        lea     redraw_topmost(pc),a0
        move.l  a0,(API_redraw_topmost*4)(a1)
        lea     zwin_ptr(pc),a0
        move.l  a0,(API_zwin_ptr*4)(a1)
        lea     fmt_dec(pc),a0
        move.l  a0,(API_fmt_dec*4)(a1)
        lea     str_append(pc),a0
        move.l  a0,(API_str_append*4)(a1)
        lea     rect_outline_white(pc),a0
        move.l  a0,(API_rect_outline_white*4)(a1)
        lea     apply_theme(pc),a0
        move.l  a0,(API_apply_theme*4)(a1)
        lea     gm_start(pc),a0
        move.l  a0,(API_gm_start*4)(a1)
        lea     gm_stop(pc),a0
        move.l  a0,(API_gm_stop*4)(a1)
        lea     rd_entry(pc),a0
        move.l  a0,(API_rd_entry*4)(a1)
        lea     fat_mount(pc),a0
        move.l  a0,(API_fat_mount*4)(a1)
        lea     fat_list_root(pc),a0
        move.l  a0,(API_fat_list_root*4)(a1)
        lea     fat_read_file(pc),a0
        move.l  a0,(API_fat_read_file*4)(a1)
        lea     fat_find_file(pc),a0
        move.l  a0,(API_fat_find_file*4)(a1)
        lea     fat_save_file(pc),a0
        move.l  a0,(API_fat_save_file*4)(a1)
        lea     return_to_desktop_app(pc),a0
        move.l  a0,(API_return_desktop*4)(a1)
        lea     dt_rand7(pc),a0
        move.l  a0,(API_dt_rand7*4)(a1)
        lea     notepad_open_file(pc),a0
        move.l  a0,(API_notepad_open_file*4)(a1)
        lea     notepad_open_fat(pc),a0
        move.l  a0,(API_notepad_open_fat*4)(a1)
        lea     launch_app(pc),a0
        move.l  a0,(API_launch_app*4)(a1)
        lea     tk_stop(pc),a0
        move.l  a0,(API_tk_stop*4)(a1)
        ; --- data blocks (ordinals KD_*) ---
        lea     vars(pc),a0
        move.l  a0,(KD_vars*4)(a1)
        lea     font8x8(pc),a0
        move.l  a0,(KD_font8x8*4)(a1)
        lea     npstat(pc),a0
        move.l  a0,(KD_npstat*4)(a1)
        lea     theme_pal(pc),a0
        move.l  a0,(KD_theme_pal*4)(a1)
        lea     cop_colptr(pc),a0
        move.l  a0,(KD_cop_colptr*4)(a1)
        lea     romdisk_tab(pc),a0
        move.l  a0,(KD_romdisk_tab*4)(a1)
        lea     fat_tab(pc),a0
        move.l  a0,(KD_fat_tab*4)(a1)
        lea     dt_board(pc),a0
        move.l  a0,(KD_dt_board*4)(a1)
        lea     pm_maze(pc),a0
        move.l  a0,(KD_pm_maze*4)(a1)
        lea     pm_maze_tpl(pc),a0
        move.l  a0,(KD_pm_maze_tpl*4)(a1)
        lea     koro_notes(pc),a0
        move.l  a0,(KD_koro_notes*4)(a1)
        lea     ext_palette(pc),a0
        move.l  a0,(KD_ext_palette*4)(a1)
        lea     koro_count(pc),a0
        move.l  a0,(KD_koro_count*4)(a1)
        lea     romdisk_count(pc),a0
        move.l  a0,(KD_romdisk_count*4)(a1)
        lea     mus_notes(pc),a0
        move.l  a0,(KD_mus_notes*4)(a1)
        lea     mus_count(pc),a0
        move.l  a0,(KD_mus_count*4)(a1)
        lea     drive_notes(pc),a0
        move.l  a0,(KD_drive_notes*4)(a1)
        lea     drive_count(pc),a0
        move.l  a0,(KD_drive_count*4)(a1)
        lea     pt_canvas,a0        ; bss hunk: absolute (relocated) reference
        move.l  a0,(KD_pt_canvas*4)(a1)
        lea     pt_stack,a0
        move.l  a0,(KD_pt_stack*4)(a1)
        movem.l (sp)+,d0/a0-a1
        rts

; ---- load_app: d0 = proc index, d1 = window slot.
; Reads the app's .APP file from the DF1 FAT12 disk into the slot region and
; calls the app's app_open (a2 = window entry). Returns d0=0 on success, -1 if
; the proc is not a disk app or the file could not be loaded.
;   d5 = proc (held), d6 = window slot (held), a3 = slot base (held)
load_app:
        movem.l d1-d7/a0-a4,-(sp)
        move.w  d0,d5               ; d5 = proc
        move.w  d1,d6               ; d6 = window slot
        ; disk app?
        lea     disk_app_tab(pc),a0
        moveq   #0,d0
        move.w  d5,d0
        tst.b   (a0,d0.w)
        beq     .fail
        ; ensure FAT12 mounted (the app disk is on DF1)
        move.b  fat_mounted(pc),d0
        tst.b   d0
        bne     .mounted
        bsr     fat_mount
        tst.w   d0
        bmi     .fail               ; no DF1 / mount failed
        lea     vars(pc),a4
        st      fat_mounted-vars(a4)
        bsr     fat_list_root
.mounted:
        ; proc -> file-name index
        lea     app_file_idx(pc),a0
        moveq   #0,d0
        move.w  d5,d0
        move.b  (a0,d0.w),d3
        bmi     .fail
        ext.w   d3
        mulu    #11,d3
        lea     app_file_tab(pc),a0
        lea     (a0,d3.w),a0        ; a0 = 11-char FAT name
        bsr     fat_find_file       ; -> d0 = start cluster, d1 = size
        cmp.w   #-1,d0
        beq     .fail
        move.w  d0,d4               ; d4 = start cluster (held)
        move.l  d1,d7               ; d7 = size / read budget (held)
        cmp.l   #APPSLOT_SZ,d7      ; S1: refuse an app image larger than its 8 KB
        bhi     .fail               ; slot - else fat_read_file overruns the slot
                                    ; into the neighbour + bitplanes at $60000
        ; dest = slot base
        move.w  d6,d1
        bsr     app_slot_base       ; a1 = dest (preserves d-regs)
        move.l  a1,a3               ; a3 = slot base
        ; read the chain: d0 = cluster, d1 = byte budget, a1 = dest
        move.w  d4,d0
        and.l   #$FFFF,d0
        move.l  d7,d1
        bsr     fat_read_file       ; -> d0 = bytes read
        tst.l   d0                  ; bytes read
        ble     .fail
        lea     str_load_ok(pc),a0
        bsr     ser_puts
        ; call the app's app_open vector (a2 = window entry for this slot)
        move.w  d6,d2
        bsr     win_ptr_raw         ; a2 = window entry
        move.l  a3,a0
        jsr     APP_OPEN(a0)
        moveq   #0,d0
        movem.l (sp)+,d1-d7/a0-a4
        rts
.fail:
        lea     str_load_fail(pc),a0
        bsr     ser_puts
        moveq   #-1,d0
        movem.l (sp)+,d1-d7/a0-a4
        rts
str_load_ok:    dc.b    "LOADER: app loaded from DF1",13,10,0
str_load_fail:  dc.b    "LOADER: app load FAILED",13,10,0
        even

; ============================================================================
; return_to_desktop_app - an app calls this to close its own (topmost) window.
; Mirrors the kernel's ESC/close path: close the topmost z-window.
; ============================================================================
return_to_desktop_app:
        move.w  zcount(pc),d0
        beq     .out
        subq.w  #1,d0               ; topmost z index
        bsr     close_window
.out:   rts

; ============================================================================
; Disk-app dispatch shims. A window whose proc is a disk app routes its
; draw/key/tick through the JMP table at its slot. d6/d1 carry the window slot
; depending on the caller; each shim recomputes the slot from a2 when needed.
; ============================================================================

; slot_of_window - a2 = window entry -> d1 = window slot (table index).
; Preserves d0/a0-a2.
slot_of_window:
        move.l  d0,-(sp)
        move.l  a0,-(sp)
        lea     wintab(pc),a0
        move.l  a2,d0
        sub.l   a0,d0
        lsr.w   #4,d0               ; / WENT_SIZE
        move.w  d0,d1
        move.l  (sp)+,a0
        move.l  (sp)+,d0
        rts

; slot_loaded - d1 = slot -> Z clear (ne) if a disk app is loaded there.
; Preserves d0/d1/a0-a2.
slot_loaded:
        move.l  a0,-(sp)
        lea     app_loaded(pc),a0
        tst.b   (a0,d1.w)
        move.l  (sp)+,a0
        rts

; dispatch_app_draw - a2 = window (disk app): call its draw vector
dispatch_app_draw:
        movem.l d1/a0-a1,-(sp)
        bsr     slot_of_window      ; d1 = slot
        bsr     slot_loaded
        beq     .skip               ; not loaded -> leave the frame as-is
        bsr     app_slot_base       ; a1 = slot base
        move.l  a1,a0
        jsr     APP_DRAW(a0)
.skip:  movem.l (sp)+,d1/a0-a1
        rts

; dispatch_app_key - a2 = window (disk app), d1 = ascii, d2 = raw
dispatch_app_key:
        movem.l d1/a0-a1,-(sp)
        move.w  d1,-(sp)            ; ascii needs to survive slot calc
        bsr     slot_of_window      ; d1 = slot (clobbers d1)
        bsr     slot_loaded
        beq     .skip
        bsr     app_slot_base       ; a1 = slot base
        move.w  (sp)+,d1            ; restore ascii
        move.l  a1,a0
        jsr     APP_KEY(a0)
        movem.l (sp)+,d1/a0-a1
        rts
.skip:  addq.l  #2,sp               ; drop the saved ascii word
        movem.l (sp)+,d1/a0-a1
        rts

; dispatch_app_tick - a2 = window (disk app)
dispatch_app_tick:
        movem.l d1/a0-a1,-(sp)
        bsr     slot_of_window
        bsr     slot_loaded
        beq     .skip
        bsr     app_slot_base
        move.l  a1,a0
        jsr     APP_TICK(a0)
.skip:  movem.l (sp)+,d1/a0-a1
        rts

; dispatch_app_click - a2 = window (disk app), d0/d1 = click x/y. Routes a
; body click to the slot's APP_CLICK vector (Paint's mouse-drawing entry).
; Non-drawing apps emit an `rts` there, so this is always safe to call.
dispatch_app_click:
        movem.l d0-d1/a0-a1,-(sp)
        movem.l d0-d1,-(sp)         ; click x/y must survive the slot calc
        bsr     slot_of_window      ; d1 = slot (clobbers d1)
        bsr     slot_loaded
        beq     .skip
        bsr     app_slot_base       ; a1 = slot base
        movem.l (sp)+,d0-d1         ; restore click x/y
        move.l  a1,a0
        jsr     APP_CLICK(a0)
        movem.l (sp)+,d0-d1/a0-a1
        rts
.skip:  addq.l  #8,sp               ; drop the saved click x/y long pair
        movem.l (sp)+,d0-d1/a0-a1
        rts

; at_app_key - AUTOTEST helper: deliver a key (d1=ascii, d2=raw) to the
; TOPMOST window's loaded disk app, then refresh it. Mirrors the runtime
; focused-key path without the scheduler/mailbox.
at_app_key:
        movem.l d0-d3/a2,-(sp)
        move.w  zcount(pc),d3
        beq     .done
        subq.w  #1,d3
        move.w  d1,-(sp)            ; ascii
        move.w  d2,-(sp)            ; raw
        move.w  d3,d2
        bsr     zwin_ptr            ; a2 = topmost window
        move.w  (sp)+,d2            ; raw
        move.w  (sp)+,d1            ; ascii
        bsr     dispatch_app_key
        bsr     redraw_topmost
.done:  movem.l (sp)+,d0-d3/a2
        rts

; at_app_tick - AUTOTEST helper: drive one tick on the topmost disk app.
at_app_tick:
        movem.l d0-d3/a2,-(sp)
        move.w  zcount(pc),d2
        beq     .done
        subq.w  #1,d2
        bsr     zwin_ptr            ; a2 = topmost window
        bsr     dispatch_app_tick
.done:  movem.l (sp)+,d0-d3/a2
        rts
