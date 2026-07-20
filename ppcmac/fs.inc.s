# ============================================================================
# UnoDOS / PowerPC Mac — USV1 mini-FS (byte-heap catalog), ported from c64/fs.i.
# Storage is a fixed 4 KB RAM region (FSBASE) the harness persists across runs
# via --storage=<sidecar>. On real hardware this region would be backed by an
# Open Firmware disk read/write; the FS logic, Files and Notepad are exercised
# and persisted exactly as on the other ports.
#
# Layout (big-endian, this port's native order):
#   FSBASE+0..3    magic "USV1" (word 0x55535631)
#   FSBASE+4..7    file count (word, 0..15)
#   FSBASE+8..11   heap_used  (word)
#   FSBASE+16..255 15 dir entries x 16 bytes:
#       0..11  name (NUL-padded, 12 bytes)
#       12..13 size (halfword)
#       14..15 start = byte offset into the heap (halfword)
#   FSHEAP = FSBASE+256: contiguous file data, heap grows upward (3840 bytes).
#
# fs_save overwrites by delete-then-append; fs_delete compacts the heap and
# fixes every later entry's start. fs_init formats + seeds README.TXT/HELLO.TXT
# only when the magic is absent; a persisted store is loaded as-is.
# ============================================================================

# fs_init: mount (magic present) or format + seed. Non-leaf.
fs_init:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LA    r3, FSBASE
    lwz   r4, 0(r3)
    lis   r5, 0x5553
    ori   r5, r5, 0x5631                  # "USV1"
    cmpw  r4, r5
    beq   fsi_ret
    li    r6, 64                          # zero catalog (256 bytes)
    mtctr r6
    mr    r6, r3
fsi_z:
    li    r0, 0
    stw   r0, 0(r6)
    addi  r6, r6, 4
    bdnz  fsi_z
    stw   r5, 0(r3)                       # magic
    LA    r3, fn_readme
    LA    r4, seed_readme
    li    r5, seed_readme_len
    bl    fs_save
    LA    r3, fn_hello
    LA    r4, seed_hello
    li    r5, seed_hello_len
    bl    fs_save
fsi_ret:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# fs_find: r3 = 12-byte name ptr -> r3 = index or -1. Leaf.
fs_find:
    mr    r5, r3
    LA    r6, FSBASE
    lwz   r7, 4(r6)                       # count
    li    r8, 0
ffd_e:
    cmpw  r8, r7
    bge   ffd_no
    slwi  r9, r8, 4
    addi  r9, r9, 16
    add   r9, r9, r6                      # entry
    li    r10, 0
ffd_c:
    lbzx  r11, r9, r10
    lbzx  r12, r5, r10
    cmpw  r11, r12
    bne   ffd_n
    addi  r10, r10, 1
    cmpwi r10, 12
    bne   ffd_c
    mr    r3, r8                          # all 12 matched
    blr
ffd_n:
    addi  r8, r8, 1
    b     ffd_e
ffd_no:
    li    r3, -1
    blr

# fs_read: r3 = index, r4 = dest, r5 = max -> r3 = bytes copied (clamped). Leaf.
fs_read:
    slwi  r3, r3, 4
    addi  r3, r3, 16
    LA    r6, FSBASE
    add   r3, r3, r6                      # entry
    lhz   r7, 12(r3)                      # size
    lhz   r8, 14(r3)                      # start
    cmpw  r7, r5
    ble   fsr_ok
    mr    r7, r5                          # clamp to the caller's buffer
fsr_ok:
    addi  r6, r6, 256                     # heap base
    add   r8, r6, r8                      # src
    mr    r9, r4
    mr    r3, r7
    cmpwi r7, 0
    beq   fsr_d
    mtctr r7
fsr_l:
    lbz   r10, 0(r8)
    stb   r10, 0(r9)
    addi  r8, r8, 1
    addi  r9, r9, 1
    bdnz  fsr_l
fsr_d:
    blr

# fs_delete: r3 = index. Compact heap + directory. Leaf (loops only).
fs_delete:
    LA    r4, FSBASE
    slwi  r5, r3, 4
    addi  r5, r5, 16
    add   r5, r5, r4                      # victim entry
    lhz   r6, 12(r5)                      # N = size
    lhz   r7, 14(r5)                      # S = start
    lwz   r8, 8(r4)                       # heap_used
    subf  r9, r7, r8
    subf  r9, r6, r9                      # move len = used - S - N
    addi  r10, r4, 256
    add   r11, r10, r7                    # dst = heap+S
    add   r12, r11, r6                    # src = heap+S+N
    cmpwi r9, 0
    beq   fsd_m
    mtctr r9
fsd_c:
    lbz   r0, 0(r12)
    stb   r0, 0(r11)
    addi  r11, r11, 1
    addi  r12, r12, 1
    bdnz  fsd_c
fsd_m:
    subf  r8, r6, r8
    stw   r8, 8(r4)                       # used -= N
    # fix start of every entry whose start > S
    lwz   r8, 4(r4)                       # count
    li    r9, 0
fsd_f:
    cmpw  r9, r8
    bge   fsd_s
    slwi  r10, r9, 4
    addi  r10, r10, 16
    add   r10, r10, r4
    lhz   r11, 14(r10)
    cmpw  r11, r7
    ble   fsd_fn
    subf  r11, r6, r11
    sth   r11, 14(r10)
fsd_fn:
    addi  r9, r9, 1
    b     fsd_f
fsd_s:
    # drop the dir slot: shift entries idx+1.. down one slot
    mr    r9, r3
fsd_sh:
    addi  r10, r9, 1
    cmpw  r10, r8
    bge   fsd_cnt
    slwi  r11, r9, 4
    addi  r11, r11, 16
    add   r11, r11, r4                    # dst entry
    addi  r12, r11, 16                    # src entry
    li    r5, 16
    mtctr r5
fsd_cp:
    lbz   r0, 0(r12)
    stb   r0, 0(r11)
    addi  r11, r11, 1
    addi  r12, r12, 1
    bdnz  fsd_cp
    addi  r9, r9, 1
    b     fsd_sh
fsd_cnt:
    subi  r8, r8, 1
    stw   r8, 4(r4)
    blr

# fs_save: r3 = name, r4 = data, r5 = size -> r3 = 0 ok / -1 full. Non-leaf.
fs_save:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    mr    r14, r3                         # name
    mr    r15, r4                         # data
    mr    r16, r5                         # size
    bl    fs_find                         # (r3 = name still)
    cmpwi r3, -1
    beq   fsv_f
    bl    fs_delete                       # overwrite = delete-then-append
fsv_f:
    LA    r4, FSBASE
    lwz   r5, 4(r4)                       # count
    cmpwi r5, FS_MAXFILES
    bge   fsv_full
    lwz   r6, 8(r4)                       # used
    add   r7, r6, r16
    cmpwi r7, FS_HEAPSZ
    bgt   fsv_full
    slwi  r8, r5, 4
    addi  r8, r8, 16
    add   r8, r8, r4                      # new entry
    li    r9, 12
    mtctr r9
    mr    r9, r8
    mr    r10, r14
fsv_n:
    lbz   r0, 0(r10)
    stb   r0, 0(r9)
    addi  r9, r9, 1
    addi  r10, r10, 1
    bdnz  fsv_n
    sth   r16, 12(r8)                     # size
    sth   r6, 14(r8)                      # start = used
    addi  r9, r4, 256
    add   r9, r9, r6                      # heap dst
    mr    r10, r15
    cmpwi r16, 0
    beq   fsv_u
    mtctr r16
fsv_d:
    lbz   r0, 0(r10)
    stb   r0, 0(r9)
    addi  r9, r9, 1
    addi  r10, r10, 1
    bdnz  fsv_d
fsv_u:
    add   r6, r6, r16
    stw   r6, 8(r4)                       # used += size
    addi  r5, r5, 1
    stw   r5, 4(r4)                       # count++
    li    r3, 0
    b     fsv_r
fsv_full:
    li    r3, -1
fsv_r:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

# fmt_dec4: r3 = value (0..9999), r4 = dest (5 bytes: 4 digits + NUL). Leaf.
fmt_dec4:
    cmpwi r3, 9999
    ble   fd4_ok
    li    r3, 9999
fd4_ok:
    LA    r5, dec_pows
    li    r6, 4
fd4_d:
    lwz   r7, 0(r5)
    li    r8, '0'
fd4_s:
    cmpw  r3, r7
    blt   fd4_w
    subf  r3, r7, r3
    addi  r8, r8, 1
    b     fd4_s
fd4_w:
    stb   r8, 0(r4)
    addi  r4, r4, 1
    addi  r5, r5, 4
    subic. r6, r6, 1
    bne   fd4_d
    li    r8, 0
    stb   r8, 0(r4)
    blr

.section .rodata
.align 2
dec_pows:  .long 1000, 100, 10, 1
fn_readme: .ascii "README.TXT"
           .byte 0,0
fn_hello:  .ascii "HELLO.TXT"
           .byte 0,0,0
fn_note:   .ascii "NOTE.TXT"
           .byte 0,0,0,0
seed_readme:
    .ascii "UnoDOS/ppcmac - PowerPC Mac.\r"
    .ascii "This file lives in the USV1\r"
    .ascii "mini-FS (RAM, persisted by\r"
    .ascii "the harness sidecar).\r"
.equ seed_readme_len, . - seed_readme
seed_hello:
    .ascii "Hello from the PowerPC Mac!\r"
.equ seed_hello_len, . - seed_hello
np_seed:
    .ascii "Notepad over Open Firmware.\r"
.equ np_seed_len, . - np_seed
.align 2
.section .text
