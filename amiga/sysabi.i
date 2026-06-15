; ============================================================================
; UnoDOS/68K disk-loaded-app ABI - shared by kernel.asm AND every separately
; assembled app (theme_app.asm, dostris_app.asm, ...).
;
; WHY a runtime vector table (not absolute linking like the C64):
;   The kernel is an AmigaDOS hunk executable; the bootblock LoadSeg()s it to
;   an address we do not control, so the kernel's symbols FLOAT at runtime.
;   An app assembled at a fixed org therefore cannot `jsr draw_string` by a
;   compile-time address (unlike the 6502 ports, whose kernel runs at a fixed
;   $0801). Instead the kernel publishes, at a FIXED chip-RAM address, a table
;   of LONG pointers - the runtime (relocated) addresses of each exported
;   routine and each exported data block. Apps call/read through that table.
;   This is a classic syscall-vector design and is relocation-proof.
;
; App load model (windowed multitasking preserved):
;   Each app is a raw binary (-Fbin) assembled at its own fixed slot address
;   APPSLOT(n). The first 5 longs of every app image are a JMP table:
;       +0  jmp app_open    (called once when the window opens; a2 = window)
;       +4  jmp app_draw     (a2 = window)
;       +8  jmp app_key      (d1 = ascii, d2 = raw; a2 = window)
;       +12 jmp app_tick     (a2 = window)
;       +16 jmp app_click    (d0/d1 = click x/y; a2 = window) - mouse-drawing
;                            apps (Paint); others just rts here.
;   The window manager keeps one slot per concurrently-open app (load-on-open),
;   so several app windows can be live at once - the scheduler dispatches each
;   window's draw/key/tick through ITS slot's vectors.
; ============================================================================

; ---- window-entry layout + shared UI metrics (must match kernel.asm).
; Apps receive a2 = window entry in their draw/key/tick vectors.
WSTATE      equ 0
WPROC       equ 1
WX          equ 2
WY          equ 4
WW          equ 6
WH          equ 8
WTITLE      equ 10
WENT_SIZE   equ 16
MAXWIN      equ 6
TBAR_H      equ 10
SCRW        equ 320
SCRH        equ 200

; ---- fixed chip-RAM address of the API vector table (kernel fills at boot) -
; Sits in the free gap between the tracker instrument waves ($76B80) and the
; supervisor stack region; 512 bytes is ample for the exported surface.
APIVEC      equ $77000

; ---- API ordinals (LONG index into APIVEC). KEEP IN SYNC BOTH SIDES.
; Routines (called via KCALL):
API_draw_string      equ 0
API_draw_string_bg   equ 1
API_draw_window      equ 2
API_fill_rect        equ 3
API_clear_screen     equ 4
API_redraw_topmost   equ 5
API_zwin_ptr         equ 6
API_fmt_dec          equ 7
API_str_append       equ 8
API_rect_outline_white equ 9
API_apply_theme      equ 10
API_gm_start         equ 11
API_gm_stop          equ 12
API_rd_entry         equ 13
API_fat_mount        equ 14
API_fat_list_root    equ 15
API_fat_read_file    equ 16
API_fat_find_file    equ 17
API_fat_save_file    equ 18
API_return_desktop   equ 19          ; close the topmost (app exits)
API_dt_rand7         equ 20          ; shared 0..6 RNG (Pac-Man ghost AI)
API_notepad_open_file equ 21         ; Files -> open a ROM-disk file in Notepad
API_notepad_open_fat equ 22          ; Files -> open the selected FAT12 file
API_launch_app       equ 23          ; open/raise a window by proc index
API_tk_stop          equ 24          ; Tracker: stop playback (kernel also calls)
API_NROUTINES        equ 25

; Data-block pointers (read via KDATA -> the relocated kernel address).
; Ordinals continue immediately after the routine slots so they never collide.
KD_vars              equ API_NROUTINES+0   ; base of the shared 'vars' block
KD_font8x8           equ API_NROUTINES+1
KD_npstat            equ API_NROUTINES+2   ; shared 64-byte scratch line
KD_theme_pal         equ API_NROUTINES+3
KD_cop_colptr        equ API_NROUTINES+4   ; &(copperlist COLOR00 value word)
KD_romdisk_tab       equ API_NROUTINES+5
KD_fat_tab           equ API_NROUTINES+6
KD_dt_board          equ API_NROUTINES+7   ; Dostris 10x20 board (kernel bss)
KD_pm_maze           equ API_NROUTINES+8   ; Pac-Man live maze (kernel bss)
KD_pm_maze_tpl       equ API_NROUTINES+9   ; Pac-Man maze template
KD_koro_notes        equ API_NROUTINES+10  ; Dostris music note pairs
KD_ext_palette       equ API_NROUTINES+11
KD_koro_count        equ API_NROUTINES+12  ; Dostris music note count (word)
KD_romdisk_count     equ API_NROUTINES+13  ; Files ROM-disk entry count (word)
KD_mus_notes         equ API_NROUTINES+14  ; Music: (period,dur,yoff) triplets
KD_mus_count         equ API_NROUTINES+15  ; Music: note count (word)
KD_drive_notes       equ API_NROUTINES+16  ; OutLast: (period,dur) note pairs
KD_drive_count       equ API_NROUTINES+17  ; OutLast: note count (word)
KD_pt_canvas         equ API_NROUTINES+18  ; Paint: byte-per-pixel backing store
KD_pt_stack          equ API_NROUTINES+19  ; Paint: flood-fill scanline stack
KD_NENTRIES          equ API_NROUTINES+20

; ---- vars-block field offsets (VO_<field>) come from the generated
; build/kernel_api.inc - mkapi.py reads the kernel listing so the offsets
; can never drift out of sync with kernel.asm's 'vars:' layout. Apps do:
;     KDATA a4,vars        ; a4 = runtime &vars
;     move.w VO_zcount(a4),d0

; ---- app slots: one fixed load region per concurrently-open window.
; MAXWIN windows -> MAXWIN slots. The free contiguous chip-RAM window below
; the bitplanes ($60000) and above the relocated kernel (<$14000 on a 512KB
; machine) is $50000..$60000 = 64 KB. Each slot is 8 KB (assembled apps are a
; few KB of code each; the largest, Paint, is ~5.5 KB).
APPSLOT0    equ $50000
APPSLOT_SZ  equ $2000              ; 8 KB per slot
; APPSLOT(n) = APPSLOT0 + n*APPSLOT_SZ

; app JMP-table entry offsets (from the slot base). A fifth vector, APP_CLICK,
; lets the kernel mouse handler hand a body-click to a disk app that draws with
; the mouse (Paint runs its synchronous drag loop there). Apps that do not need
; it still emit the slot (an `rts`), keeping every image's header uniform.
APP_OPEN    equ 0
APP_DRAW    equ 4
APP_KEY     equ 8
APP_TICK    equ 12
APP_CLICK   equ 16

; ---- app-id -> disk filename is fixed by the loader's app_file_tab.
; proc indices (match app_def_tab / dispatch order in the kernel):
PROC_SYSINFO equ 0
PROC_CLOCK   equ 1
PROC_FILES   equ 2
PROC_NOTEPAD equ 3
PROC_MUSIC   equ 4
PROC_THEME   equ 5
PROC_DOSTRIS equ 6
PROC_OUTLAST equ 7
PROC_PACMAN  equ 8
PROC_TRACKER equ 9
PROC_PAINT   equ 10

; ---- KCALL macro: call exported kernel routine \1.
; The trampoline uses a6 (never an INPUT argument to any exported routine -
; a0/a1/d0-d4 are the argument registers, so KCALL must NOT use a0). a6 is
; caller-clobbered, which is fine: the apps never hold state in a6.
; Usage:  KCALL draw_string   (args already in registers per the routine)
KCALL       macro
            move.l  APIVEC+(API_\1*4),a6
            jsr     (a6)
            endm

; ---- KCALLT: tail-call (jmp) an exported routine.
KCALLT      macro
            move.l  APIVEC+(API_\1*4),a6
            jmp     (a6)
            endm

; ---- KDATA macro: load exported kernel data-block address \2 into An \1.
; Usage:  KDATA a4,vars      ; a4 = runtime &vars
KDATA       macro
            move.l  APIVEC+(KD_\2*4),\1
            endm
