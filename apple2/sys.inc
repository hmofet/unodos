; ============================================================================
; UnoDOS/AppleII shared system equates - included by kernel.s AND by every
; disk-loaded app (theme.s, dostris.s, pacman.s, ...). Defines the zero-page
; layout, key codes, screen layout, KBSS buffers and the disk-app loader ABI
; so an app assembled separately agrees with the kernel. Routine entry points
; come from the generated build/kernel_api.inc (addresses mkapi.py extracts
; from the kernel listing) - the app is linked to the kernel BY ADDRESS.
; ============================================================================

; ---- zero page: kernel scratch + draw/fs API params ($00-$4F). Apps set the
;      param slots (zpCol/zpRow/zpInv/zpFX.. /zpPtr/zpFS*) before calling the
;      draw_*/fill_*/fs_* API and read them back; the rest is kernel-internal.
zpPtr       equ $00   ; (2) generic string/data pointer (draw_string input)
zpCol       equ $02   ; draw column, byte 0-39
zpRow       equ $03   ; draw char-row, 0-23
zpInv       equ $04   ; draw_char EOR mask ($00 normal, $7F inverted)
zpTmp       equ $05   ; handle_key passes the key code here to an app
zpFontPtr   equ $06   ; (2)
zpRowLoPtr  equ $08   ; (2)
zpRowHiPtr  equ $0A   ; (2)
zpDst       equ $0C   ; (2) bitmap dest pointer (apps that poke the framebuffer)
zpSIdx      equ $0E
zpFX        equ $0F   ; fill_rows/frame_rect/dither_rect params
zpFY        equ $10
zpFW        equ $11
zpFH        equ $12
zpFPat      equ $13
zpI         equ $14
zpJ         equ $15
zpFX0       equ $16   ; frame_rect: saved original params
zpFY0       equ $17
zpFW0       equ $18
zpFH0       equ $19
zpWX        equ $1A   ; draw_win params
zpWY        equ $1B
zpWW        equ $1C
zpWH        equ $1D
zpWF        equ $1E   ; focused flag (0/1)
zpSecs      equ $1F   ; (2) clock_format scratch
zpHH        equ $21
zpMM        equ $22
zpSS        equ $23
clkbuf      equ $24   ; (9) "HH:MM:SS",0
zpTens      equ $2D

; ---- fs.i zero-page ($2E-$3A) - owned by the kernel FS; apps set the
;      published param slots below and never touch the rest. ----
zpFSPtr  equ $2E   ; (2) pointer to a CATBUF directory entry (or its fields)
zpFSName equ $30   ; (2) pointer to a 12-byte name (find/save input)
zpFSRel  equ $32   ; relative sector index, 0..FS_SECTORS-1
zpFSCnt  equ $33   ; sector-count loop counter (read/write-sectors)
zpFSSize equ $34   ; (2) byte size (LE) - fs_size output / fs_save input / draw_dec16 value
zpFSIdx  equ $36   ; directory index scratch (fs_delete)
zpFSDat  equ $37   ; (2) caller data pointer for read/write-sectors loops
zpFSTmp  equ $39   ; scratch (fs_save's N sectors-needed / fs_delete's victim start)
zpFSTmp2 equ $3A   ; scratch (logical sector addr for fs_read/write_sectors loops)
zpFSEnt  equ zpFSRel ; (2) alias of zpFSRel/zpFSCnt, reused in fs_delete

; ---- draw_dec16 / Files / Notepad working zero page ($3B-$4D) ----
zpDVlo   equ $3B   ; (2) draw_dec16 working value
zpDVhi   equ $3C
zpDDig   equ $3D   ; draw_dec16 current digit
zpDLead  equ $3E   ; draw_dec16 leading-zero-suppress flag
zpNPtr   equ $3F   ; (2) notepad_draw: pointer into NOTEBUF
zpNRem   equ $41   ; (2) notepad_draw: bytes remaining in scan
zpNTot   equ $43   ; (2) notepad_draw pass1: total line count (1-based)
zpNFCol  equ $45   ; (2) notepad_draw pass1: final (cursor) column, 0-based
zpNSkip  equ $47   ; (2) notepad_draw: lines to skip (tail scroll)
zpNVis   equ $49   ; notepad_draw pass2: current line visible flag
zpNLine  equ $4A   ; (2) notepad_draw pass2: current line number (0-based)
zpNCol   equ $4C   ; notepad_draw pass2: current screen column (capped 40)
zpNRow   equ $4D   ; notepad_draw pass2: current screen row

zpBeepHalf equ $4E ; beep: half-period delay constant (kernel beep)
zpBeepN    equ $4F ; beep: remaining $C030 toggles

; ============================================================================
; zero page free for disk-loaded apps ($50-$80). Only ONE app is resident at a
; time, so apps share this whole block; the kernel API routines never touch it
; (only the $00-$4F params above). The named slots below preserve each app's
; historical layout; an app references whichever names it needs.
; ============================================================================
; -- Dostris --
zpDosMask  equ $50 ; (2) current/test piece+rotation 4x4 bitmask
zpDosBit   equ $52
zpDosBX    equ $53
zpDosBY    equ $54
zpDosTX    equ $55
zpDosTY    equ $56
zpDosIdx   equ $58
zpDosT1    equ $59
zpDosT2    equ $5A
zpDosFull  equ $5B
zpDosRow   equ $5C
zpDosSrc   equ $5D ; (2)
zpDosDst   equ $5F ; (2)
; -- Pac-Man --
zpPMCol    equ $61
zpPMRow    equ $62
zpPMIdx    equ $63
zpPMNCol   equ $64
zpPMNRow   equ $65
zpPMT1     equ $66
zpPMT2     equ $67
zpPMBestD  equ $68
zpPMBestDir equ $69
zpPMTCol   equ $6A
zpPMTRow   equ $6B
zpPMDir    equ $6C
zpPMGI     equ $6D
zpPMDir2   equ $6E
zpPMRevDir equ $6F
zpPMOldPC  equ $70
zpPMOldPR  equ $71
zpPMOldGC  equ $72 ; (2)
zpPMOldGR  equ $74 ; (2)
; -- Music/Tracker note_play (shared blocking square-wave) --
zpNoteHalf equ $76 ; note_play: half-period inner-delay constant
zpNoteDur  equ $77 ; (2) note_play: remaining $C030 toggles (16-bit)
; -- Paint --
zpPB     equ $79   ; (2) PAINTBUF cell pointer
zpPBT    equ $7B   ; (2) pt_render_cell/pt_ptr col/row scratch
zpPBInk  equ $7D   ; pt_apply: resolved ink pattern
; -- OutLast band-centre math --
zpTmp2   equ $7E
zpTmp3   equ $7F
zpTmp4   equ $80

; ---- screen layout constants ----
; Cell screen geometry is Contract-owned (CONTRACT-ARCH Phase 4): generated by
; unogen from unodef/unodef.toml ([world.apple2]) so it can't drift. Byte-identical
; to the SCRCOLS/SCRROWS equates this replaced. (This single_app port has no
; window-entry struct / event queue to source — see [port.apple2].)
                include "../unodef/gen/apple2/sys_gen.inc"

SI_X            equ 1     ; SysInfo window
SI_Y            equ 8
SI_W            equ 30
SI_H            equ 64
SI_CONTENT_X    equ (SI_X+1)
SI_CONTENT_Y    equ (SI_Y+8)
SI_CONTENT_W    equ (SI_W-2)
SI_CONTENT_H    equ (SI_H-9)

CK_X            equ 1     ; Clock window
CK_Y            equ 72
CK_W            equ 20
CK_H            equ 40
CK_CONTENT_X    equ (CK_X+1)
CK_CONTENT_Y    equ (CK_Y+8)
CK_CONTENT_W    equ (CK_W-2)
CK_CONTENT_H    equ (CK_H-9)

; icon grid - 3 rows x 4 cols
ICONW           equ 9
ICONH           equ 24
ICONROW0_Y      equ 120
ICONROW1_Y      equ 144
ICONROW2_Y      equ 168
LABELROW0       equ 16
LABELROW1       equ 19
LABELROW2       equ 22
ICONCOL0_X      equ 1
ICONCOL1_X      equ 11
ICONCOL2_X      equ 21
ICONCOL3_X      equ 31
NICONS          equ 10    ; populated icon slots (M3: 3 rows x 4 cols)

; full-screen app content area
APP_CONTENT_Y   equ 8     ; pixel row of content row1
APP_CONTENT_H   equ 176   ; 22 rows * 8px
APP_VIEW_ROWS   equ 22
APP_STATUS_Y    equ 184   ; pixel row of row23
NOTE_MAXLEN     equ 2048  ; notepad text buffer cap

; TICKS_PER_SEC: main-loop passes per soft-clock second (harness-calibrated
; with harness.py's TICK_INSTRS).
TICKS_PER_SEC   equ 1000

; mini-FS catalog / directory-entry layout (apps that index CATBUF directly)
FSC_COUNT       equ 4
FSC_NEXTFREE    equ 5
FSC_DIR         equ 16
FSE_SIZE        equ 12

; ---- KBSS: kernel buffers + shared app work RAM. Above the kernel image and
;      the app load region; only one app runs at a time so the app BSS tables
;      do not conflict. (rwts.i/fs.i define DECTAB..NOTEBUF inside this range.)
KBSS            equ $9000
CATBUF   equ KBSS+$400   ; $9400-$94FF: cached catalog sector (rel sector 0)
NOTEBUF  equ KBSS+$500   ; $9500-$9CFF: Notepad text buffer (2048 bytes)
DOSBOARD equ KBSS+$D00   ; $9D00-$9DC7: Dostris 10x20 board
PMMAZE   equ KBSS+$E00   ; $9E00-$9EA8: Pac-Man 13x13 maze
PAINTBUF equ KBSS+$1000  ; $A000-$A43F: Paint 32x34 cell patterns (page-aligned)
PT_STK   equ KBSS+$1500  ; $A500.. Paint flood-fill stack

; ---- key codes (Apple II $C000 high-bit-set codes) ----
K_RET    equ $8D
K_ESC    equ $9B
K_LEFT   equ $88
K_RIGHT  equ $95
K_UP     equ $8B
K_DOWN   equ $8A
K_SPACE  equ $A0
K_BS     equ $88
K_SAVE   equ $93     ; Ctrl-S

; ============================================================================
; disk-app loader ABI
; ----------------------------------------------------------------------------
; A disk-loaded app is a raw binary assembled at APP_BASE whose first three
; bytes are a JMP to its init entry, followed at +3 by a JMP to key and at +6
; by a JMP to tick. The kernel's load_app reads the app's sectors off the disk
; (real RWTS) into APP_BASE, sets app_mode = APP_MODE, and JSRs APP_BASE (init).
; While the app is active, handle_key does jmp APP_KEY (key code in zpTmp) and
; the mainloop does jsr APP_TICK each pass (a static app's tick is just rts).
; The app calls return_to_desktop (see kernel_api.inc) to exit.
;
; The app id is the icon index 0..NICONS-1. Ids 0 (SysInfo) and 1 (Clock) have
; NO disk binary - they are kernel-drawn launcher windows. Ids 2..9 are disk
; apps. Files+Notepad ship as ONE binary (id 2). id_app_track / id_app_secs
; tables in the kernel map id -> (start track, sector count).
; ============================================================================
APP_BASE   equ $6000
APP_INIT   equ APP_BASE         ; jmp <app>_init
APP_KEY    equ APP_BASE+3       ; jmp <app>_key   (key in zpTmp)
APP_TICK   equ APP_BASE+6       ; jmp <app>_tick
APP_MODE   equ 10               ; app_mode value while a disk app is resident

; app ids (icon index). 0/1 are kernel windows; 2-9 are disk binaries.
APPID_FILES   equ 2     ; Files + Notepad (one binary)
APPID_THEME   equ 3
APPID_DOSTRIS equ 4
APPID_PACMAN  equ 5
APPID_MUSIC   equ 6
APPID_TRACKER equ 7
APPID_PAINT   equ 8
APPID_OUTLAST equ 9
