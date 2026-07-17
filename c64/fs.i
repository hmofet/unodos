; ============================================================================
; UnoDOS/C64 mini-FS (milestone 2) - "USV1" byte-heap catalog, the flat-RAM
; analogue of the Apple II port's sector-heap (apple2/fs.i) and a direct cousin
; of genesis/sram.i's byte heap.
;
; The C64 has no sectors to address here - storage is a 4 KB RAM region the
; harness persists across reboots (FSBASE..FSBASE+$FFF). On real hardware this
; region would be backed by a 1541 disk file via an IEC driver (that driver is
; M2's remaining real-hardware work); the FS logic, Files and Notepad are all
; exercised and persisted in the harness exactly as on the other ports.
;
; Layout (all little-endian):
;   FSBASE+0..3    magic "USV1"
;   FSBASE+4       file count (0..FS_MAXFILES=15)
;   FSBASE+5..6    heap_used (bytes consumed in the data heap)
;   FSBASE+7..15   reserved
;   FSBASE+16..255 15 directory entries x 16 bytes:
;       0..11   name (NUL-padded, 12 bytes)
;       12..13  size in bytes (word)
;       14..15  start = byte offset into the heap (word)
;   FSHEAP = FSBASE+256 ($C100): contiguous file data, heap grows upward.
;
; fs_save overwrites by delete-then-append (so an edited file moves to the end
; of the listing); fs_delete compacts the heap (memmove later files down, fix
; every entry's start) and drops the directory slot. API matches the Apple II /
; Genesis ports so Files/Notepad port with the same call shape.
;
; fs_init formats + seeds README.TXT/HELLO.TXT only when the magic is absent
; (first boot / unformatted store); a persisted store is loaded as-is.
; ============================================================================

; ---- zero page ($40-$55) ----
zpDVlo   equ $40   ; draw_dec16 working value lo
zpDVhi   equ $41
zpDDig   equ $42   ; draw_dec16 current digit
zpDLead  equ $43   ; draw_dec16 leading-zero-suppress flag
; zpFSPtr/Name/Size/Dat ($44-$4B) are in sys.inc (shared with disk-loaded apps)
zpFSSrc  equ $4C   ; (2) fs_memcpy source
zpFSDst  equ $4E   ; (2) fs_memcpy dest
zpFSLen  equ $50   ; (2) fs_memcpy length
zpFSTmp  equ $52   ; (2) scratch (victim start / shift bookkeeping)
zpFSIdx  equ $54   ; directory index scratch
zpFSCnt  equ $55   ; loop counter scratch
zpFSNewSz equ $56  ; (2) fs_save: new file size, kept across fs_delete
                   ; (aliased as zpFSMax in sys.inc: fs_read's caller-set dest
                   ; cap - safe, fs_save and fs_read never overlap)

; ---- layout constants ----
FSBASE      equ $C000
FSHEAP      equ FSBASE+256
HEAPSIZE    equ $D000-FSHEAP   ; $F00 = 3840 bytes
FS_MAXFILES equ 15
FSC_COUNT    equ 4
FSC_USED     equ 5             ; heap_used (word)
FSC_DIR      equ 16
FSE_SIZE     equ 12
FSE_START    equ 14
FSE_LEN      equ 16

; ---- Notepad text buffer (volatile working RAM, not in the VIC bank) ----
NOTEBUF     equ $4400          ; $4400-$4BFF (2 KB)
NOTE_MAXLEN equ 2048

; ---------------------------------------------------------------- fs_init
; fs_init - load/validate the catalog. Format + seed only if "USV1" is absent.
fs_init:
        lda FSBASE+0
        cmp #$55                ; 'U'
        bne fsi_format
        lda FSBASE+1
        cmp #$53                ; 'S'
        bne fsi_format
        lda FSBASE+2
        cmp #$56                ; 'V'
        bne fsi_format
        lda FSBASE+3
        cmp #$31                ; '1'
        beq fsi_done            ; valid store - keep it
fsi_format:
        lda #$55                ; 'U'
        sta FSBASE+0
        lda #$53                ; 'S'
        sta FSBASE+1
        lda #$56                ; 'V'
        sta FSBASE+2
        lda #$31                ; '1'
        sta FSBASE+3
        lda #0
        sta FSBASE+FSC_COUNT
        sta FSBASE+FSC_USED
        sta FSBASE+FSC_USED+1
        ; zero the directory region (16..255)
        ldx #FSC_DIR
fsi_zero:
        lda #0
        sta FSBASE,x
        inx
        bne fsi_zero
        ; seed README.TXT + HELLO.TXT
        lda #<seed_readme_name
        sta zpFSName
        lda #>seed_readme_name
        sta zpFSName+1
        lda #<seed_readme
        sta zpFSDat
        lda #>seed_readme
        sta zpFSDat+1
        lda #seed_readme_len
        sta zpFSSize
        lda #0
        sta zpFSSize+1
        jsr fs_save
        lda #<seed_hello_name
        sta zpFSName
        lda #>seed_hello_name
        sta zpFSName+1
        lda #<seed_hello
        sta zpFSDat
        lda #>seed_hello
        sta zpFSDat+1
        lda #seed_hello_len
        sta zpFSSize
        lda #0
        sta zpFSSize+1
        jsr fs_save
fsi_done:
        rts

; ---------------------------------------------------------------- fs_entry_ptr
; fs_entry_ptr - A = directory index -> zpFSPtr = &FSBASE[FSC_DIR + idx*16].
; Clobbers A.
fs_entry_ptr:
        asl
        asl
        asl
        asl                     ; idx*16
        clc
        adc #<(FSBASE+FSC_DIR)
        sta zpFSPtr
        lda #0
        adc #>(FSBASE+FSC_DIR)
        sta zpFSPtr+1
        rts

; ---------------------------------------------------------------- fs_size/start
; fs_size - zpFSPtr -> zpFSSize (word). fs_start -> zpFSTmp (word).
fs_size:
        ldy #FSE_SIZE
        lda (zpFSPtr),y
        sta zpFSSize
        iny
        lda (zpFSPtr),y
        sta zpFSSize+1
        rts
fs_start:
        ldy #FSE_START
        lda (zpFSPtr),y
        sta zpFSTmp
        iny
        lda (zpFSPtr),y
        sta zpFSTmp+1
        rts

; ---------------------------------------------------------------- fs_find
; fs_find - zpFSName -> 12-byte name; returns A = index or $FF if not found.
fs_find:
        lda #0
        sta zpFSIdx
ff_loop:
        lda zpFSIdx
        cmp FSBASE+FSC_COUNT
        bcc ff_check
        lda #$FF
        rts
ff_check:
        lda zpFSIdx
        jsr fs_entry_ptr
        ldy #0
ff_cmp:
        lda (zpFSPtr),y
        cmp (zpFSName),y
        bne ff_next
        iny
        cpy #12
        bne ff_cmp
        lda zpFSIdx             ; all 12 matched
        rts
ff_next:
        inc zpFSIdx
        jmp ff_loop

; ---------------------------------------------------------------- fs_memcpy
; fs_memcpy - copy zpFSLen bytes zpFSSrc -> zpFSDst (forward; safe when
; dst < src, i.e. a downward move). Clobbers A/Y.
fs_memcpy:
        ldy #0
fmc_lp:
        lda zpFSLen
        ora zpFSLen+1
        beq fmc_done
        lda (zpFSSrc),y
        sta (zpFSDst),y
        inc zpFSSrc
        bne fmc_s
        inc zpFSSrc+1
fmc_s:
        inc zpFSDst
        bne fmc_d
        inc zpFSDst+1
fmc_d:
        lda zpFSLen
        bne fmc_declo
        dec zpFSLen+1
fmc_declo:
        dec zpFSLen
        jmp fmc_lp
fmc_done:
        rts

; ---------------------------------------------------------------- fs_read
; fs_read - A = index, zpFSDat = dest. Copies the file's bytes to dest and
; sets zpFSSize. (heap addr = FSHEAP + start.)
fs_read:
        jsr fs_entry_ptr
        jsr fs_size             ; -> zpFSSize
        jsr fs_start            ; -> zpFSTmp (start offset)
        clc
        lda #<FSHEAP
        adc zpFSTmp
        sta zpFSSrc
        lda #>FSHEAP
        adc zpFSTmp+1
        sta zpFSSrc+1
        lda zpFSDat
        sta zpFSDst
        lda zpFSDat+1
        sta zpFSDst+1
        ; S1: clamp the copy length to the caller's buffer capacity (zpFSMax) so
        ; a crafted catalog size field can't overrun the destination buffer.
        lda zpFSSize+1
        cmp zpFSMax+1
        bcc fsr_len             ; hi(size) < hi(cap)
        bne fsr_clamp           ; hi(size) > hi(cap)
        lda zpFSSize
        cmp zpFSMax
        bcc fsr_len             ; lo(size) < lo(cap)
        beq fsr_len             ; size == cap
fsr_clamp:
        lda zpFSMax
        sta zpFSSize
        lda zpFSMax+1
        sta zpFSSize+1
fsr_len:
        lda zpFSSize
        sta zpFSLen
        lda zpFSSize+1
        sta zpFSLen+1
        jsr fs_memcpy
        rts

; ---------------------------------------------------------------- fs_save
; fs_save - zpFSName, zpFSDat, zpFSSize. Delete-then-append. Returns A = 0 on
; success, $FF if the directory or heap is full. zpFSSize is preserved on
; entry (we copy it before fs_find/fs_delete can touch nothing - they don't).
fs_save:
        ; stash the new size where fs_delete can't reach it (fs_delete uses
        ; zpFSSize AND zpFSTmp for the victim's size/start)
        lda zpFSSize
        sta zpFSNewSz
        lda zpFSSize+1
        sta zpFSNewSz+1
        ; delete existing file with this name, if any
        jsr fs_find
        cmp #$FF
        beq fsv_fresh
        jsr fs_delete
fsv_fresh:
        ; restore size
        lda zpFSNewSz
        sta zpFSSize
        lda zpFSNewSz+1
        sta zpFSSize+1
        ; directory full?
        lda FSBASE+FSC_COUNT
        cmp #FS_MAXFILES
        bcc fsv_room
        lda #$FF
        rts
fsv_room:
        ; heap full?  heap_used + size > HEAPSIZE ?
        clc
        lda FSBASE+FSC_USED
        adc zpFSSize
        tax                     ; low of new used
        lda FSBASE+FSC_USED+1
        adc zpFSSize+1          ; high of new used (A)
        cmp #>HEAPSIZE
        bcc fsv_fits
        bne fsv_full
        cpx #<HEAPSIZE
        bcc fsv_fits
        beq fsv_fits
fsv_full:
        lda #$FF
        rts
fsv_fits:
        ; new entry index = count
        lda FSBASE+FSC_COUNT
        jsr fs_entry_ptr        ; zpFSPtr = new dir slot
        ; copy name (12)
        ldy #0
fsv_name:
        lda (zpFSName),y
        sta (zpFSPtr),y
        iny
        cpy #12
        bne fsv_name
        ; size
        ldy #FSE_SIZE
        lda zpFSSize
        sta (zpFSPtr),y
        iny
        lda zpFSSize+1
        sta (zpFSPtr),y
        ; start = current heap_used
        ldy #FSE_START
        lda FSBASE+FSC_USED
        sta (zpFSPtr),y
        iny
        lda FSBASE+FSC_USED+1
        sta (zpFSPtr),y
        ; copy data into heap at FSHEAP + heap_used
        clc
        lda #<FSHEAP
        adc FSBASE+FSC_USED
        sta zpFSDst
        lda #>FSHEAP
        adc FSBASE+FSC_USED+1
        sta zpFSDst+1
        lda zpFSDat
        sta zpFSSrc
        lda zpFSDat+1
        sta zpFSSrc+1
        lda zpFSSize
        sta zpFSLen
        lda zpFSSize+1
        sta zpFSLen+1
        jsr fs_memcpy
        ; heap_used += size
        clc
        lda FSBASE+FSC_USED
        adc zpFSSize
        sta FSBASE+FSC_USED
        lda FSBASE+FSC_USED+1
        adc zpFSSize+1
        sta FSBASE+FSC_USED+1
        ; count++
        inc FSBASE+FSC_COUNT
        lda #0
        rts

; ---------------------------------------------------------------- fs_delete
; fs_delete - A = index. Compact the heap (move bytes after the victim down by
; its size, fix later entries' start), then drop the directory slot.
fs_delete:
        sta zpFSIdx
        jsr fs_entry_ptr        ; zpFSPtr = victim entry
        jsr fs_size             ; zpFSSize = victim size (N)
        jsr fs_start            ; zpFSTmp = victim start (S)
        ; --- memmove heap[S+N .. heap_used) down to heap[S] ---
        ; dst = FSHEAP + S
        clc
        lda #<FSHEAP
        adc zpFSTmp
        sta zpFSDst
        lda #>FSHEAP
        adc zpFSTmp+1
        sta zpFSDst+1
        ; src = FSHEAP + S + N
        clc
        lda zpFSDst
        adc zpFSSize
        sta zpFSSrc
        lda zpFSDst+1
        adc zpFSSize+1
        sta zpFSSrc+1
        ; len = heap_used - (S + N)
        sec
        lda FSBASE+FSC_USED
        sbc zpFSTmp
        sta zpFSLen
        lda FSBASE+FSC_USED+1
        sbc zpFSTmp+1
        sta zpFSLen+1           ; len = heap_used - S
        sec
        lda zpFSLen
        sbc zpFSSize
        sta zpFSLen
        lda zpFSLen+1
        sbc zpFSSize+1
        sta zpFSLen+1           ; len -= N
        jsr fs_memcpy
        ; heap_used -= N
        sec
        lda FSBASE+FSC_USED
        sbc zpFSSize
        sta FSBASE+FSC_USED
        lda FSBASE+FSC_USED+1
        sbc zpFSSize+1
        sta FSBASE+FSC_USED+1
        ; --- fix start of every entry whose start > S ---
        lda #0
        sta zpFSCnt
fsd_fix:
        lda zpFSCnt
        cmp FSBASE+FSC_COUNT
        bcs fsd_dir
        lda zpFSCnt
        jsr fs_entry_ptr
        ldy #FSE_START
        lda (zpFSPtr),y         ; entry start lo
        sta zpFSDst             ; reuse zpFSDst as scratch (word)
        iny
        lda (zpFSPtr),y
        sta zpFSDst+1
        ; if start > S then start -= N
        lda zpFSDst+1
        cmp zpFSTmp+1
        bcc fsd_nextfix
        bne fsd_dofix
        lda zpFSDst
        cmp zpFSTmp
        bcc fsd_nextfix
        beq fsd_nextfix         ; equal = the victim's own (being removed) - skip
fsd_dofix:
        sec
        lda zpFSDst
        sbc zpFSSize
        sta zpFSDst
        lda zpFSDst+1
        sbc zpFSSize+1
        sta zpFSDst+1
        lda zpFSCnt
        jsr fs_entry_ptr
        ldy #FSE_START
        lda zpFSDst
        sta (zpFSPtr),y
        iny
        lda zpFSDst+1
        sta (zpFSPtr),y
fsd_nextfix:
        inc zpFSCnt
        jmp fsd_fix
        ; --- drop the directory slot: shift entries idx+1.. down by 16 ---
fsd_dir:
        lda zpFSIdx
fsd_shift:
        clc
        adc #1
        cmp FSBASE+FSC_COUNT
        bcs fsd_count           ; no more entries to pull down
        ; copy entry (A) into entry (A-1): 16 bytes
        pha
        jsr fs_entry_ptr        ; zpFSPtr = source (A)
        lda zpFSPtr
        sta zpFSSrc
        lda zpFSPtr+1
        sta zpFSSrc+1
        pla
        pha
        sec
        sbc #1
        jsr fs_entry_ptr        ; zpFSPtr = dest (A-1)
        ldy #0
fsd_copy:
        lda (zpFSSrc),y
        sta (zpFSPtr),y
        iny
        cpy #FSE_LEN
        bne fsd_copy
        pla
        jmp fsd_shift
fsd_count:
        dec FSBASE+FSC_COUNT
        rts

; ---------------------------------------------------------------- seed data
seed_readme_name: dc.b "README.TXT",0,0
seed_hello_name:  dc.b "HELLO.TXT",0,0,0
seed_readme:
        dc.b "UnoDOS/C64 - a real OS for the bare",$0D
        dc.b "Commodore 64. This file lives in the",$0D
        dc.b "USV1 mini-FS. Open HELLO.TXT, edit it",$0D
        dc.b "and press F1 to save.",$0D
seed_readme_len equ . - seed_readme
seed_hello:
        dc.b "Hello from the C64!",$0D
seed_hello_len equ . - seed_hello
