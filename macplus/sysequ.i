; ============================================================================
; Shared STATIC system equates - included by kernel.asm AND by every disk app.
; These are compile-time constants (memory map, window-record layout, screen
; geometry, key raw codes), so they live in one file both sides agree on.
; Dynamic addresses (kernel routine entry points, variable-block offsets) are
; NOT here - those come from the generated build/kernel_api.inc (mkapi.py).
; ============================================================================

; An app's entry-vector header is FOUR dc.l ADDRESSES at the slot base:
;   slot+0 draw  slot+4 key  slot+8 tick  slot+12 click
; The loader reads the address and jsr's it (move.l (off,a1),aX; jsr (aX)),
; so the offsets are exact 4-byte longs - no jmp/bra size ambiguity.

; ---- memory map ----
KERNBASE    equ $20000
KBSS        equ $30000
STACKTOP    equ $78000

; ---- disk-app resident slots (sync: diskapp.i) ----
APP_LOAD    equ $40000              ; slot 0 base
APP_SLOT_SZ equ $4000               ; 16 KB per resident slot
; an app sets `org APP_LOAD+SLOT*APP_SLOT_SZ` with its slot number, so the
; load address the kernel computes always matches the assembled org.
APPSLOT_FILES   equ 0
APPSLOT_DOSTRIS equ 1
APPSLOT_PACMAN  equ 2
APPSLOT_OUTLAST equ 3
APPSLOT_PAINT   equ 4
APPSLOT_MUSIC   equ 5
APPSLOT_TRACKER equ 6
APPSLOT_THEME   equ 7
APPSLOT_DEMO    equ 8

; KBSS buffers (out-of-image fixed RAM; the kernel memsets [KBSS,KBSS_END)).
; Apps read/write these directly (Notepad's buffer, the game boards, the Paint
; canvas), so the addresses must match kernel.asm exactly.
npbuf       equ KBSS                ; $800  Notepad edit buffer (NBUF)
fat_tab     equ KBSS+$800           ; $120  root-dir cache (16*18)
fat_buf     equ KBSS+$A00           ; $600  whole FAT (3 sectors)
FATSECBUF   equ KBSS+$1000          ; $200  FAT sector scratch
sony_pb     equ KBSS+$1200          ; $40   .Sony ParamBlockRec
dt_board    equ KBSS+$1300          ; $C8   Dostris board (10*20)
pm_maze     equ KBSS+$1400          ; $2BC  Pac-Man maze (28*25)
pm_gh       equ KBSS+$1700          ; $1E   ghost records
pm_old      equ KBSS+$1720          ; $10   pre-step actor positions
pt_canvas   equ KBSS+$1800          ; $8C00 Paint ink store (256*140)
pt_stack    equ KBSS+$A400          ; $7F8  flood-fill stack (510*4)
KBSS_END    equ KBSS+$AC00
NBUF        equ 2048                ; Notepad edit buffer size

; ---- window-manager record (WENT) layout (a2 = window in app entries) ----
WENT_SIZE   equ 16
WSTATE      equ 0
WPROC       equ 1
WX          equ 2
WY          equ 4
WW          equ 6
WH          equ 8
WTITLE      equ 10
TBAR_H      equ 10
MENUBAR_H   equ 12

TICKS_SEC   equ 60

; ---- canonical UnoDOS raw key codes (the d2 the apps dispatch on) ----
K_UP        equ $4C
K_DOWN      equ $4D
K_LEFT      equ $4F
K_RIGHT     equ $4E
K_SAVE      equ $50                 ; Clr key (F1) = save
