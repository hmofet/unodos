; ============================================================================
; UnoDOS/C64 shared system equates - included by kernel.s AND by every
; disk-loaded app (pacman.s, tracker.s, ...). Defines the zero-page layout,
; key codes, colours, screen layout and I/O addresses so an app assembled
; separately agrees with the kernel. Routine entry points come from the
; generated build/kernel_api.inc (addresses extracted from the kernel listing).
; ============================================================================

; ---- zero page (we SEI and never call KERNAL, so $02-$8F is ours; $00/$01
;      are the 6510 banking port - never touch as scratch) ----
zpPtr       equ $02   ; (2) string/data pointer
zpFontPtr   equ $04   ; (2) glyph pointer
zpDst       equ $06   ; (2) bitmap dest pointer
zpScrPtr    equ $08   ; (2) screen-RAM (colour) pointer
zpCol       equ $0A   ; draw cell column 0-39
zpRow       equ $0B   ; draw cell row 0-24
zpInv       equ $0C   ; draw_char EOR mask ($00 normal, $FF inverted)
zpFCol      equ $0D   ; current cell colour (fg<<4 | bg)
zpTmp       equ $0E
zpTmp2      equ $0F
zpI         equ $10
zpJ         equ $11
zpFX        equ $12   ; fill_rows: cell column
zpFY        equ $13   ; fill_rows: pixel row 0-199
zpFW        equ $14   ; fill_rows: width in cells
zpFH        equ $15   ; fill_rows: height in pixel rows
zpFPat      equ $16   ; fill_rows: bitmap byte
zpCX        equ $17   ; color_fill: cell column
zpCY        equ $18   ; color_fill: cell row
zpCW        equ $19   ; color_fill: width in cells
zpCH        equ $1A   ; color_fill: height in cell rows
zpWX        equ $1B   ; draw_win / win_outline: cell rect
zpWY        equ $1C
zpWW        equ $1D
zpWH        equ $1E
zpWF        equ $1F   ; draw_win: focused flag 0/1
zpSIdx      equ $20   ; draw_string index
clkbuf      equ $21   ; (9) "HH:MM:SS",0
zpHH        equ $2A
zpMM        equ $2B
zpSS        equ $2C
zpKey       equ $2D   ; decoded key this scan (0 = none)
zpShift     equ $2E   ; nonzero if shift held
zpRasLo     equ $30
zpRasHi     equ $31
zpCnt       equ $32   ; (2)
; slots the fill_rows/color_fill primitives never touch (for values that must
; survive a call to them - they clobber zpTmp/zpTmp2/zpI/zpJ/zpDst/zpScrPtr):
zpSlot      equ $35
zpWtop      equ $36
zpSel       equ $37
zpIconX     equ $38
zpCharY     equ $39   ; draw_char saves caller Y here
zpIconY     equ $3A

; ---- FS / draw_dec16 input-output slots (apps set these before calling the
;      fs_*/draw_name12/draw_dec16 API; fs.i owns the rest of $40-$57) ----
zpFSPtr  equ $44   ; (2) directory entry / 12-byte name pointer
zpFSName equ $46   ; (2) name to find/save
zpFSSize equ $48   ; (2) byte size / draw_dec16 value
zpFSDat  equ $4A   ; (2) caller data pointer (read dest / save source)

; ---- zero page free for disk-loaded apps ($67-$8F; the kernel API routines
;      never touch these, only the param/scratch slots above) ----
zpApp0   equ $67
zpApp1   equ $68
zpApp2   equ $69
zpApp3   equ $6A
zpApp4   equ $6B
zpApp5   equ $6C
zpApp6   equ $6D
zpApp7   equ $6E
zpApp8   equ $6F
zpApp9   equ $70
zpAppA   equ $71
zpAppB   equ $72
zpAppC   equ $73
zpAppD   equ $74
zpAppE   equ $75
zpAppF   equ $76

; ---- logical key codes (zpKey / zpTmp) ----
K_RET    equ $0D
K_ESC    equ $1B
K_LEFT   equ $11
K_RIGHT  equ $12
K_UP     equ $13
K_DOWN   equ $14
K_SPACE  equ $20
K_BS     equ $08
K_SAVE   equ $06

; ---- colour bytes (fg<<4 | bg) ----
COL_DESK     equ $E6
COL_MENU     equ $0F
COL_WIN      equ $01
COL_TITLE_F  equ $16
COL_TITLE_U  equ $0F
COL_ICON     equ $01
COL_ICON_SEL equ $16
BORDERCOL    equ $06

; ---- screen layout ----
SCRCOLS  equ 40
SCRROWS  equ 25
SI_X     equ 2
SI_Y     equ 2
SI_W     equ 26
SI_H     equ 8
CK_X     equ 2
CK_Y     equ 11
CK_W     equ 14
CK_H     equ 5
ICONW    equ 9
ICONH    equ 2
NICONS   equ 10       ; built icon slots (bumped as loadable apps are added)
APP_CONTENT_Y  equ 8
APP_CONTENT_H  equ 176
APP_VIEW_ROWS  equ 22

; ---- C64 I/O ----
VIC_D011 equ $D011
VIC_D012 equ $D012
VIC_D016 equ $D016
VIC_D018 equ $D018
VIC_D020 equ $D020
VIC_D021 equ $D021
SID_BASE equ $D400
CIA1_PRA equ $DC00
CIA1_PRB equ $DC01
CIA1_DDRA equ $DC02
CIA1_DDRB equ $DC03
CIA1_TOD10 equ $DC08
CIA1_TODSEC equ $DC09
CIA1_TODMIN equ $DC0A
CIA1_TODHR  equ $DC0B
CIA2_PRA  equ $DD00
CIA2_DDRA equ $DD02

BITMAP   equ $6000
SCREEN   equ $4000

; ---- disk-app loader ABI ----
; A disk-loaded app is a raw binary assembled at APP_BASE whose first three
; words are JMPs: app_init / app_key / app_tick. The kernel reads the app's
; bytes into APP_BASE (via the harness/IEC-backed loader at LOAD_PORT), sets
; app_mode = APP_MODE, and jsr APP_BASE (init). While active, handle_key does
; jmp APP_KEY (the key is in zpTmp) and game_tick does jmp APP_TICK. The app
; calls return_to_desktop (see kernel_api.inc) to exit.
APP_BASE  equ $5000
APP_KEY   equ APP_BASE+3
APP_TICK  equ APP_BASE+6
APP_MODE  equ 10
LOAD_PORT equ $DE00   ; write app-id here -> loader copies the app to APP_BASE
                      ; ($DE00 = the C64 I/O-1 expansion slot; unmapped on a
                      ; stock machine, harness-backed here, an IEC/cartridge
                      ; loader on real hardware - the M3 real-hw follow-up)

; ---- app shared work RAM (in the VIC-bank-1 hole, not read by the VIC) ----
APP_BSS   equ $4C00   ; $4C00-$4FFF: app boards/buffers (maze, pattern, canvas)
