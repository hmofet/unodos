

# CONFIRMED (97)

## [critical/high] apps/launcher.asm (shell, segment 0x2000) uses pusha/popa and shl-imm in 61 places
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\launcher.asm:apps/launcher.asm: 25 pusha / 24 popa / 12 shl-imm at the cited lines (verified exact); but the binding 8088 blockers are kernel/kernel.asm:3026 (pusha in draw_char, executed at boot splash before the launcher loads) and kernel/kernel.asm:172-173, 203-204 (movzx+bt in the INT 0x80 dispatcher — 386+ only) | area:8088 compatibility
DESC: 61 CPU-8086 assembly errors: pusha x25 / popa x24 (line 94 'pusha', 822 'popa', etc.) and shl reg,imm x12 (line 522 'shl ax, 6', line 570 'shl ax, 4 ; * 16', line 1530 'shl ax, 2'). On a real 8088 the first pusha at the launcher's event-loop entry executes as JO (undocumented opcode alias), corrupting control flow before the desktop draws. The launcher is the mandatory shell at 0x2000, so the desktop never comes up. No 32-bit registers — fully mechanical fix.
VERIFIER: The instruction-level facts are CONFIRMED, but the claimed failure path and severity are WRONG and must be corrected.

CONFIRMED (verified by reading C:\Users\arin\Documents\Github\unodos\apps\launcher.asm and by assembling test encodings with NASM):
1. Exact count holds: 25 pusha (lines 94, 381, 510, 806, 829, 889, 910, 922, 961, 997, 1032, 1164, 1293, 1386, 1444, 1457, 1471, 1483, 1577, 1592, 1701, 1781, 1915, 2018, 2038) + 24 popa (502, 554, 822, 881, 903, 915, 954, 990, 1022, 1078, 1279, 1328, 1437, 1450, 1464, 1476, 1493, 1584, 1689, 1748, 1828, 2000, 2029, 2063) + 12 shl reg,imm>1 (522, 570, 630, 656, 689, 754, 1047, 1530, 1707, 1718, 1795, 1933) = 61. (Line 1100 'shl ax,1' is 8086-legal and correctly excluded.)
2. No 'CPU' directive exists anywhere in the repo, so NASM emits 186+ encodings: pusha=60h, popa=61h, 'shl ax,6'=C1 E0 06 (verified by assembling). On real 8088/8086 silicon 60h/61h decode as JO/JNO rel8 aliases and C0h/C1h alias RETN imm16/RETN — so these do corrupt control flow on a real 8088.
3. The project does document 8088 as minimum CPU (README.md line 201 'Intel 8088 @ 4.77 MHz' minimum; CHANGELOG.md line 1202 'Target hardware: Intel 8088'), so this is a genuine violation of the stated hardware contract. TODO.md line 31 ('8086-compatible boot for HP 200LX, Sharp PC-3100') shows 8086 compat is a known open item.

WRONG in the finding:
1. The failure narrative ('the first pusha at the launcher's event-loop entry executes as JO ... so the desktop never comes up') misidentifies the first failure point. On a real 8088 the KERNEL crashes during its boot splash, before LAUNCHER.BIN is even loaded: kernel entry (kernel/kernel.asm:15) draws the version/build strings via gfx_draw_string_stub (calls at ~lines 77-87; routine at 7523) which calls draw_char (call at 7570), whose first instruction is pusha (kernel.asm:3026) — all before auto_load_launcher (kernel.asm:94). kernel.asm contains 43 pusha/popa of its own.
2. Worse: the kernel's INT 0x80 syscall dispatcher uses movzx (kernel.asm:172, 203) and bt (173, 204). bt is a 386+ instruction — the kernel today requires a 386; even a 286 (where pusha/popa/shl-imm are fine) cannot run it. So the launcher's 186-isms are never the binding constraint on any CPU.
3. 'Critical' severity is therefore wrong as scoped: fixing launcher.asm alone changes nothing observable on any CPU that can currently boot this OS. The real issue is project-wide (kernel 43 pusha/popa + bt/movzx; nearly all 14 apps: notepad.asm 65, pacman.asm 73, tetrisv.asm 40 occurrences, etc.). Correct classification: latent portability gap vs. documented hardware claims — either fix the entire codebase for 8086 or amend README to state 386+ minimum (boot/mbr.asm line 9 already openly comments 'requires 386+ CPU').
4. 'Fully mechanical fix' is overstated for pusha/popa: the cooperative scheduler bakes the exact 8-word PUSHA frame into each task's initial stack (kernel.asm:14899 'FFEE-FFE0: pusha frame (8 words) ← for yield's popa') and yield_stub pops it with popa (see also CHANGELOG Build 198 regression). Any PUSHA86/POPA86 macro must preserve the 8-word frame layout (placeholder SP slot), and the kernel yield path must be converted in lockstep, or app launch breaks.
FIX: Only worth doing as part of a whole-tree 8086 pass (kernel first); launcher-only changes have no observable effect. Mechanical recipe for launcher.asm:

1. Add at top of apps/launcher.asm (and every .asm): `cpu 8086` — makes NASM reject all 186+/386+ encodings at build time.

2. Shared macro file (must keep the 8-word frame byte-identical to real PUSHA, because kernel task startup at kernel.asm:14899 and yield's popa depend on it):
%macro PUSHA86 0
    push ax
    push cx
    push dx
    push bx
    push sp          ; placeholder slot; value unused (8086 pushes post-decrement SP, POPA86 discards it)
    push bp
    push si
    push di
%endmacro
%macro POPA86 0
    pop di
    pop si
    pop bp
    add sp, 2        ; skip SP slot
    pop bx
    pop dx
    pop cx
    pop ax
%endmacro
Replace the 25 pusha / 24 popa sites 1:1. Convert kernel.asm's yield/popa path and dummy-frame builder in the same commit.

3. shl-imm sites:
- shl ax,2 (1530, 1795): `shl ax,1` x2
- shl ax,4 (570, 630, 1707, 1933): `shl ax,1` x4
- shl ax,6 (522, 656, 689, 754, 1047, 1718): `mov cl,6` / `shl ax,cl` — CL is dead at all six sites (each is an index*64 computation where CX is reloaded afterward, e.g. line 525 `mov cx,64`), so the clobber is safe; verify per-site when applying.

Alternative minimal fix if 8086 support is not actually wanted: change README.md line 201 / FEATURES.md minimum CPU to 80386, matching boot/mbr.asm's stated requirement and the kernel's bt/movzx usage.

## [critical/high] boot/mbr.asm uses 386 32-bit LBA math (self-documented '386+ required')
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\boot\mbr.asm:boot/mbr.asm lines 67, 68, 112, 113, 114, 115, 119, 120, 121, 124 (exactly 10 non-8086 instructions; note the first instruction after .found_partition, line 64 'mov [partition_entry], si', is 8086-clean — garbage decoding starts at line 67, the third instruction of the block) | area:8088 compatibility
DESC: 10 CPU-8086 errors, all in the partition-LBA handling: line 67 'mov eax, [si + 8]', 68 'mov [dap_lba], eax', 112-121 'mov eax,[dap_lba] / xor edx,edx / movzx ebx, byte [bios_spt] / div ebx' (twice), and line 124 'shl ah, 6'. Line 9 even says 'NOTE: This version requires 386+ CPU'. On an 8088 these opcodes decode as garbage and the machine crashes/hangs at the first instruction after .found_partition — hard-disk boot is impossible. The INT 13h extension gating itself is correct (AH=41h check at lines 70-77 with CHS fallback via AH=08h).
VERIFIER: CONFIRMED with severity caveats. Verified by assembling C:\Users\arin\Documents\Github\unodos\boot\mbr.asm with a 'cpu 8086' directive prepended (NASM 2.16.01 in WSL): exactly 10 'no instruction for this cpu level' errors at lines 67, 68, 112, 113, 114, 115, 119, 120, 121, 124 — matching the finding's count and locations. Line 9 comment confirmed verbatim. The INT 13h AH=41h gating (lines 70-77) and AH=08h CHS fallback (88-108) are correct as claimed, but irrelevant to 8088 survival since the 386 opcodes at 67-68 execute BEFORE the gating. On a real 8086/8088 the bytes decode as aliased opcodes (0x66 operand prefix aliases JBE, 0x0F in movzx executes as POP CS, the 0xC0 shift-immediate group aliases RET imm16), so execution goes off the rails unpredictably — hard-disk boot on 8086-class CPUs is genuinely impossible. HOWEVER, three caveats downgrade 'critical': (1) This is a known, documented limitation, not a latent bug — besides mbr.asm:9, vbr.asm:6 and stage2_hd.asm:5 carry the same '386+ required' note, docs/bootloader-architecture.md:221 explicitly tables the CPU requirement as 'floppy=8086, HDD/USB=386+ (for now)', and TODO.md:31 lists '8086-compatible boot for HP 200LX, Sharp PC-3100' as a roadmap item. README's Target Hardware table puts hard drive/CF under 'Recommended', floppy under 'Minimum', so the 8088-minimum config boots from floppy. (2) Fixing mbr.asm alone does NOT enable 8088 HD boot: the same chain's stage2_hd.asm has 56 cpu-8086 violations (pervasive EAX/movzx/imul) and vbr.asm has 1 (shr al,4 at line 230) — the MBR fix is necessary but far from sufficient. (3) Side finding: even the documented-8086 FLOPPY path is not 8086-clean — boot/stage2.asm has 3 violations (pusha line 64, popa line 138, shr al,4 line 217; all 186+ only), while boot/boot.asm is clean — so the README line 9 'Intel 8088 or later' claim is currently violated by every stage-2, and the floppy-stage2 gap arguably matters more for XT-class hardware than the MBR. Also, the finding's suggested 'mov cl,6 / shl ah,cl' replacement is WRONG in this context: CL already holds the sector bits (line 117 'mov cl, dl') at that point, so loading the shift count into CL would destroy the sector number; use and ah,3 + 2x ror ah,1 instead (bit-identical to shl ah,6 for valid cylinders). My corrected fix (below) was verified: assembles cleanly under 'cpu 8086', fits the 446-byte MBR code budget (343 bytes used vs 328 original), and a Python simulation over 200,000 random (LBA, SPT, heads) tuples across the FULL 32-bit LBA range produced CHS output bit-identical to the original 386 code — it is exact for all 32-bit LBAs (the finding's proposed fix was only exact below 2^24), and neither 16-bit DIV can fault since each high-word remainder is strictly less than the divisor.
FIX: In boot/mbr.asm, replace lines 66-68 with:

    ; Get partition start LBA (offset 8 in partition entry) - 8086-safe
    mov ax, [si + 8]
    mov [dap_lba], ax
    mov ax, [si + 10]
    mov [dap_lba + 2], ax

and replace lines 110-125 with:

    ; Convert partition LBA to CHS using BIOS-reported geometry
    ; (partition table CHS may not match BIOS translation for CF cards)
    ; 8086-safe 32/16 long division: divide the high word first; its
    ; remainder feeds the low-word division. Exact for any 32-bit LBA,
    ; and no divide fault is possible (remainder < divisor at each step).
    mov al, [bios_spt]
    xor ah, ah
    mov bx, ax                      ; BX = sectors per track
    mov ax, [dap_lba + 2]           ; LBA high word
    xor dx, dx
    div bx                          ; AX = track_hi, DX = remainder
    mov si, ax                      ; SI = track_hi
    mov ax, [dap_lba]               ; DX:AX = remainder:LBA_low
    div bx                          ; AX = track_lo, DX = LBA mod SPT
    inc dl                          ; Sector (1-based)
    mov cl, dl                      ; CL[5:0] = sector

    mov bl, [bios_heads]
    xor bh, bh                      ; BX = head count
    xchg ax, si                     ; AX = track_hi, SI = track_lo
    xor dx, dx
    div bx                          ; AX = cyl_hi (discarded, as original), DX = rem
    xchg ax, si                     ; AX = track_lo, SI = cyl_hi
    div bx                          ; DX:AX = rem:track_lo -> AX = cylinder, DX = head
    mov dh, dl                      ; DH = head
    mov ch, al                      ; CH = cylinder low
    and ah, 3                       ; Cylinder bits 8-9
    ror ah, 1
    ror ah, 1                       ; -> bits 7:6 (8086-safe shl ah,6)
    or cl, ah                       ; CL[7:6] = cylinder high bits

(SI is free to clobber here: the partition-entry pointer was saved to [partition_entry] at line 64 and is reloaded at line 143 before jumping to the VBR.) Also update the line 9 comment. Optionally add 'cpu 8086' after [BITS 16] to make NASM enforce it. Verified: assembles under cpu 8086, output fits the 0x1BE budget (343 bytes), CHS results bit-identical to the 386 version over 200k randomized full-range cases. NOTE: this alone does not deliver 8088 hard-disk boot — boot/stage2_hd.asm (56 violations) and boot/vbr.asm line 230 (shr al,4) need the same treatment, and the floppy path's boot/stage2.asm has 3 violations of its own (pusha/popa at lines 64/138, shr al,4 at line 217) despite docs claiming the floppy path is 8086-compatible.

## [critical/high] boot/stage2.asm (floppy stage 2) has 3 non-8086 instructions
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\boot\stage2.asm:boot/stage2.asm lines 64 (pusha), 138 (popa), 217 (shr al,4) — line numbers verified exact via NASM listing | area:8088 compatibility
DESC: 3 CPU-8086 errors: line 64 'pusha', line 138 'popa', line 217 'shr al, 4'. The pusha at line 64 is on the main load path, so floppy boot of the kernel dies on a real 8088 (pusha executes as JO). This is the primary boot path for the 1.44MB floppy image.
VERIFIER: CONFIRMED by direct read and by NASM. Inserting 'cpu 8086' into C:\Users\arin\Documents\Github\unodos\boot\stage2.asm and assembling (NASM 2.16.01 in WSL) produces "no instruction for this cpu level" at exactly the three cited source lines and nowhere else: line 64 'pusha' (opcode 60h at file offset 0x33), line 138 'popa' (61h at 0xBF), line 217 'shr al, 4' (C0 E8 04 at 0x12A). All three are 80186+ instructions; the file has no cpu directive and the Makefile (line 92-93) passes no CPU restriction.

Concrete failure trace on a real 8088/8086 (where 60h-6Fh alias to the conditional jumps 70h-7Fh and C0h aliases to C2h RET imm16):
- Line 64 'pusha' (60h) decodes as JO rel8, consuming the next byte B8h (first byte of 'mov ax, KERNEL_SEGMENT') as displacement -72. If OF=1: wild jump. If OF=0: falls through one byte INTO the mov, decoding 00 10 as 'add [bx+si], dl' (corrupts a byte in stage2's own segment) then '8E C0' as 'mov es, ax' with AX still 0x0800 from entry — so the kernel is read to 0x0800:0000, overwriting the running stage2 loader itself. Boot dies either way. This is the main load path: load_kernel is called unconditionally at line 38.
- Line 138 'popa' (61h) decodes as JNO rel8 with displacement C3h (-61); the preceding 'dec byte [sectors_left]' (1->0) leaves OF=0, so the jump is always taken, landing 61 bytes back inside the load loop.
- Line 217 'shr al, 4' (C0 E8 04) decodes as RET 0x04E8 — a wild return popping the saved CX as the return IP. Lower impact: print_hex_byte is only reached from the .disk_error diagnostic path (lines 141-164).

No mitigating mechanism exists: stage 1 (boot/boot.asm) is fully 8086-clean and jumps straight to stage2, so stage2 is the first failure point; there is no CPU detection anywhere in the boot chain. stage2.bin is embedded in both the 360KB and 1.44MB floppy images (Makefile lines 155-169), and 8088 is the project's stated minimum (README.md lines 9/201, docs/FEATURES.md line 7); docs/bootloader-architecture.md line 221 explicitly states the floppy path's CPU requirement is 8086.

Two refinements to the original finding: (1) Severity caveat — kernel/kernel.asm itself contains ~230 occurrences of movzx/pusha/popa/32-bit registers, so fixing stage2 alone moves the failure into the kernel rather than making 8088 boot work; the stage2 fix is necessary but not sufficient for the project's 8088 claim. (2) The auditor's suggested fix 'mov cl,4 / shr al,cl' is itself buggy: line 215 ('mov cl, al') uses CL to hold the saved nibble value, so loading the shift count into CL would clobber it. Use four 'shr al, 1' instructions (8086-legal D0 /5 form) or re-home the saved value to BL.
FIX: In C:\Users\arin\Documents\Github\unodos\boot\stage2.asm:

1. Line 6-7, add enforcement directive after [BITS 16]:
   cpu 8086

2. Line 64, replace 'pusha' with (load_kernel clobbers only AX/BX/CX/DX plus ES, and the caller at lines 41-42 reloads ES itself; SI/DI/BP are untouched on the success path; .disk_error never returns):
   push ax
   push bx
   push cx
   push dx

3. Line 138, replace 'popa' with:
   pop dx
   pop cx
   pop bx
   pop ax

4. Line 217, replace 'shr al, 4' with (do NOT use 'mov cl,4 / shr al,cl' — CL holds the saved value from line 215):
   shr al, 1
   shr al, 1
   shr al, 1
   shr al, 1

After the change, 'nasm -f bin' assembles cleanly with cpu 8086 (verified: the modified file with cpu 8086 errors only on these three instructions, so no other violations exist in stage2.asm). Note: stage2_hd.asm and kernel.asm were not in scope of this finding; kernel.asm is known NOT 8086-clean (~230 hits), so 8088 hardware support requires further work beyond this fix.

## [critical/high] boot/vbr.asm has one 186+ instruction (shr al, 4)
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\boot\vbr.asm:    shr al, 1                       ; High nibble (8086-safe)
    shr al, 1
    shr al, 1
    shr al, 1 | area:8088 compatibility
DESC: Single CPU-8086 error: line 230 'shr al, 4 ; High nibble' (shift-by-immediate>1 is 186+). On 8088 this decodes with the immediate byte treated as the start of the next instruction, corrupting the hex-print path. Otherwise the VBR is 8086-clean.
VERIFIER: CONFIRMED as a fact, but the claimed severity (critical) is wrong - real severity is LOW/latent because the instruction is unreachable dead code. Verified by assembly: boot/vbr.asm:230 'shr al, 4' emits C0 E8 04 (186+ encoding) at binary offset 0x145; reassembling with 'cpu 8086' injected yields exactly ONE error at that line, so the 'otherwise 8086-clean' sub-claim is also correct (the file's header comment claiming EAX/push dword usage is stale - none remains). However: (1) print_hex_byte (lines 226-249) has ZERO callers in vbr.asm; the VBR is a standalone 512-byte binary (Makefile:311, never %included), the 'call print_hex_byte' sites in stage2.asm:150-162 and stage2_hd.asm:554-557 invoke their own local copies in separately assembled binaries, and control leaves the VBR permanently at line 186 (jmp 0x0800:0x0002). The instruction can never execute on any CPU, so 'corrupting the hex-print path' cannot occur. (2) The auditor's decode mechanism is wrong: on 8086/8088 opcode C0h is an undocumented alias of C2h (RET imm16), so C0 E8 04 would execute as RET 0x04E8 (popping the caller-saved BX as a return IP and adding 0x04E8 to SP) - not 'immediate byte treated as the next instruction'. Catastrophic if executed, but it never is. (3) The VBR is explicitly documented as 386+ for now (vbr.asm:6-7 TODO, docs/bootloader-architecture.md:221, TODO.md:31); 8086 boot is a stated future goal, not a current contract - the 8088-targeted path is the floppy path. Fixing it is cheap, future-proofing hygiene: the listing shows 80 bytes of padding headroom. Note in passing (out of scope): stage2_hd.asm:528 has the same 'shr al, 4', consistent with that file's documented 386+ requirement.
FIX: Minimal in-place fix at boot/vbr.asm:230 (touches only that line, preserves the routine's register contract, +5 bytes with 80 bytes of padding available):

    shr al, 1                       ; High nibble (8086-safe)
    shr al, 1
    shr al, 1
    shr al, 1

Optionally add 'cpu 8086' on a new line after '[BITS 16]' (line 17) to enforce cleanliness going forward (verified: the rest of the file assembles error-free under cpu 8086), and update the stale 386+ header comment at lines 6-7. Alternatively, since print_hex_byte is dead code in this binary, deleting lines 225-249 entirely is equally valid and frees ~30 bytes. Do NOT use the suggested 'mov cl, 4 / shr al, cl' without also push/pop cx - it would silently clobber the caller's CL, diverging from the CX-preserving convention used by the live copy in stage2_hd.asm:523-548.

## [critical/high] Heap base 0x1400:0000 lies inside the 44KB kernel image; first mem_alloc overwrites kernel code, heap range also spans the launcher segment
- src:boot-memory-map | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:9978 (heap segment 0x1400), 9983+9989 (heap_initialized accessed via heap DS - additional defect), 9987 (initial block size 0xF000), 10010 (limit 0xF000), 10047 (mem_free segment 0x1400); boot/stage2.asm:14 (KERNEL_SECTORS=88 proves overlap) | area:memory map / heap init
DESC: The allocator still assumes the original 16KB kernel. Line 9954 comment: 'Heap starts at 0x1400:0x0000'. mem_alloc_stub does 'mov ax, 0x1400 / mov ds, ax / mov es, ax' (9978-9980) and on first use writes the free-block header at offset 0: 'mov word [0], 0xF000 / mov word [2], 0' (9987-9988). Linear 0x14000 is kernel file offset 0x4000 of the kernel loaded at 0x1000:0000 with size 45056 bytes (0x10000-0x1AFFF). I verified build/kernel.bin offset 0x4000 contains live instruction bytes (10 83 f8 04 7c 52 ...), so the header write corrupts kernel code. The heap's allocatable range runs to 'cmp si, 0xF000' (10010), i.e. linear 0x14000-0x22FFF, which covers the last 28KB of the kernel AND the first 12KB of the launcher fixed at 0x2000:0000. mem_alloc/mem_free are exported as syscalls 7/8 in the API table (lines 7312-7313), so any app calling malloc corrupts the kernel and the shell. README.md line 220-221 even lists 'Kernel 45 KB at 0x1000:0000' immediately followed by 'Heap at 0x1400:0000' - internally contradictory. This is a direct candidate for 'apps crash when launching apps' and random visual/input corruption for any app that uses the malloc API (none of the in-tree apps under apps/ call AH=7, so on the stock image this is a loaded landmine rather than the active fault).
VERIFIER: Every factual claim checks out against the code and the built binary. (1) Kernel footprint: boot/stage2.asm:13-14 loads 88 sectors (45056 = 0xB000 bytes) at 0x1000:0000, and build/kernel.bin is exactly 45056 bytes, so the kernel occupies linear 0x10000-0x1AFFF. (2) Heap base inside kernel: kernel/kernel.asm:9978-9980 sets DS=ES=0x1400 (linear 0x14000 = kernel file offset 0x4000). I dumped kernel.bin at 0x4000 and found the exact bytes the auditor cited (10 83 f8 04 7c 52 ...), a coherent instruction stream sitting between the API table (padded to offset 0x3320, line 7292) and kernel data (~0x8B00) - live code, not padding. (3) Corruption path: on first call, lines 9987-9988 write the free-block header at 0x1400:0000-0003 (4 bytes of kernel code), line 10021 writes 0xFFFF at 0x1400:0002, and the returned pointer (offset 4) points the app's buffer at kernel code from linear 0x14004; since the allocator never splits blocks, the first allocation grants the whole 0xF000-byte block. (4) Range: the limit check 'cmp si, 0xF000' (line 10010) makes the allocatable range linear 0x14000-0x22FFF, covering the last 28KB of the kernel plus the first 12KB of the shell segment (APP_SEGMENT_SHELL equ 0x2000, line 20327; launcher.bin is 7986 bytes, entirely inside the overlap). (5) Exposure: API slots 7/8 (lines 7312-7313) are reachable from any app via int 0x80 AH=7/8; the dispatcher (lines 121-160) runs with CS=DS=0x1000 and no other mechanism guards the range. Grep confirms no in-tree app or kernel code calls mem_alloc, so on the stock image it is a latent landmine, exactly as the auditor characterized. (6) README.md:220-221 is internally contradictory as claimed. One refinement the auditor missed, which partially invalidates their suggested fix: heap_initialized (kernel offset 0x8BB9, verified by locating the init byte sequence 83 3e b9 8b 00 75 12 c7 06 00 00 00 f0 at file offset 0x466B) is accessed at lines 9983/9989 AFTER DS is switched to the heap segment, so the flag actually lives at linear heap_seg*16+0x8BB9 (currently 0x1CBB9, stray RAM), never at the real kernel variable. Their proposed 'HEAP_SEG equ 0x1C00' would silently move this stray flag access to linear 0x24BB9, inside the launcher's 64KB slot. A correct fix must also make the flag access segment-correct (cs: override works: CS=0x1000 in the INT 0x80 path). The gap 0x1B000-0x1FFFF is verified free: no kernel references to segments 0x1B00-0x1F00, kernel stack is at SS=0:SP=0x7C00 (boot.asm:46-47), and app/launcher stacks sit at the top of their own segments (kernel.asm:14915-14916), so HEAP_SEG=0x1B00 with HEAP_SIZE=0x5000 (20KB) is safe. mem_free_stub (line 10047) hardcodes 0x1400 too and must be updated, though it only flips a flag word and cannot allocate. Severity: critical-but-latent (no in-tree caller); any third-party app using the documented malloc API corrupts kernel code on the first call.
FIX: Add constants near APP_SEGMENT_SHELL (~kernel.asm:20327):

HEAP_SEG    equ 0x1B00      ; linear 0x1B000: free gap between kernel end (0x1AFFF = 88 sectors) and shell (0x20000)
HEAP_SIZE   equ 0x5000      ; 20KB: linear 0x1B000-0x1FFFF

In mem_alloc_stub, replace lines 9978-9989 with (note cs: overrides - CS=0x1000 in the INT 0x80 dispatch path, so this reaches the real kernel variable; plain [heap_initialized] with DS=heap seg hits stray RAM):

    mov ax, HEAP_SEG
    mov ds, ax
    mov es, ax

    cmp word [cs:heap_initialized], 0
    jne .heap_ready

    mov word [0], HEAP_SIZE         ; one free block spanning the whole heap
    mov word [2], 0                 ; flags: free
    mov word [cs:heap_initialized], 1

Replace line 10010:
    cmp si, HEAP_SIZE               ; heap limit (was 0xF000)

In mem_free_stub, replace line 10047:
    mov bx, HEAP_SEG

Also update the stale comments at lines 9954, 9961, 10036, README.md:221 ('Heap at 0x1B00:0000, 20 KB'), and docs/API_REFERENCE.md (mem_alloc pointer is an offset from HEAP_SEG). All instructions are 8086-safe (cs: is just the 0x2E prefix). Longer term: emit HEAP_SEG from the build (kernel size) so kernel growth past 0x1B000 fails the build instead of regressing; if 20KB is too small, use 0x9800 (28KB, below EBDA) after auditing clipboard usage of SCRATCH_SEGMENT 0x9000.

## [critical/high] post_event has no interrupt masking - task-context posts race IRQ posts, losing/corrupting events
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm:10092-10106 (unprotected tail RMW inside post_event 10083-10112); IRQ-context callers 624, 1142, 1274; IF=1 task-context callers 4375, 4680, 5043, 16258, 16354, 16477, 19324 (IF=1 because int_80_handler executes STI at line 151) | area:event queue
DESC: post_event does an unprotected read-modify-write of event_queue_tail: 'mov bx, [event_queue_tail] ... mov [event_queue + si], al ... inc bx / and bx, 0x1F / cmp bx, [event_queue_head] / je .done / mov [event_queue_tail], bx' with no CLI. It is called BOTH from hardware-IRQ context (INT 09h line 624, IRQ12 line 1142, BIOS INT15 mouse callback line 1274) and from task context with IF=1 (mouse_process_drag line 4372-4375, win_create/destroy/focus paths lines 16255-16258, 16351-16354, 16473-16477, 19323). If an IRQ fires between the task's read of tail and its write-back, the IRQ's event is written to the same slot and tail advances; the task then overwrites that slot and can write tail BACKWARD (its stale tail+1), losing one or more keyboard/mouse events and orphaning queued ones. Directly explains intermittent 'keyboard and mouse input issues': keystrokes and clicks vanish exactly when window-manager activity (focus change, redraw posts) coincides with typing/mouse movement.
VERIFIER: Verified against C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. post_event (lines 10083-10112) performs an unprotected read-modify-write of event_queue_tail: read tail (10092), write 3-byte event at slot tail*3 (10098-10099), then inc/wrap/full-check and write tail back (10102-10106). There is no CLI in the function and none of the cli sites in the file (47, 869, 1808, 3737, 3778, 4452, 4517, 14842, 15035) cover any post_event call site (the cli/sti pairs at 4452-4457 and 4517-4520 protect only drag-state reads and end before the post_event at 4680).

Both contexts confirmed: (a) IRQ context with IF=0 - INT09h keyboard handler posts EVENT_KEY_PRESS at line 624, native IRQ12 handler int_74_handler posts EVENT_MOUSE at line 1142 (no sti inside), BIOS INT15 mouse callback posts at line 1274. (b) Task context with IF=1 - the INT 0x80 dispatcher executes STI at line 151 (deliberate, commented: floppy DMA needs IRQs), so every API runs with interrupts enabled. Task-side callers: 4375 and 4680 in mouse_process_drag (invoked from event_get_stub line 10126, i.e. on every event poll), 5043 (menu_close), 16258 and 16354 (redraw_affected_windows), 16477 (win_destroy focus promotion), 19324 (win_redraw_stub).

Concrete failure: task reads tail=T at 10092; keyboard/mouse IRQ fires in the ~7-instruction window; the IRQ's post_event writes its event at slot T and advances tail to T+1; on resume the task overwrites slot T with its event and stores tail=T+1 - the IRQ's event (keystroke/click) is silently lost. If two IRQ events land in the window (e.g. kbd + mouse both pending; after the first IRET the second fires before the task's next instruction), tail reaches T+2 and the task's stale store of T+1 moves tail backward, orphaning a queued event that the next post overwrites. Because mouse_process_drag posts WIN_REDRAW during drag/focus activity on every event_get call, the race fires exactly when window-manager activity coincides with typing/mouse input, matching the intermittent input-loss symptom. Severity 'critical' is fair for an interactive OS (silent input loss), though it is probabilistic, not a crash.

The auditor's fix shape is also correct: post_event is entered with IF=0 from IRQ handlers and IF=1 from API context, so PUSHF/CLI...POPF is required (blind STI would re-enable interrupts inside IRQ handlers). This mirrors the existing pattern in mouse_cursor_hide (3736-3737). All instructions are 8086-safe, and no caller consumes flags from post_event (documented 'Preserves: All registers'), so POPF is safe. The consumer (event_get_stub) is the sole writer of event_queue_head, so protecting post_event's tail RMW fully closes this hazard.

One refinement to the original finding: with a single interleaved IRQ event the tail write-back is same-value (not backward) and exactly one event is lost; the backward-tail/orphaning case requires two or more IRQ posts within the window - rarer but possible. The dominant symptom is single lost events.
FIX: In post_event (kernel\kernel.asm line 10083), bracket the body with pushf/cli ... popf:

post_event:
    push bx
    push si
    push ds
    pushf                           ; save caller IF (IRQ ctx = 0, API ctx = 1)
    cli                             ; atomic tail RMW vs IRQ posts

    mov bx, 0x1000
    mov ds, bx

    ; ... existing body unchanged (lines 10088-10106) ...

.done:
    popf                            ; restore caller IF
    pop ds
    pop si
    pop bx
    ret

i.e. insert 'pushf' + 'cli' immediately after 'push ds' (line 10086) and 'popf' immediately after '.done:' before 'pop ds' (line 10109). PUSHF/CLI/POPF are 8086-safe; do NOT use a plain STI since post_event is also called with IF=0 from the INT09h/IRQ12 handlers.

## [critical/high] CGA scroll clear-all path falls through into VESA bank-switching code
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:19655 (jnz .sa_clear_all_loop) — insert 'jmp .sa_done' between it and '.sa_vesa:' at 19657; the auditor's cited lines 19645/19647/19610 correspond to actual 19655/19657/19620 | area:gfx_scroll_area (API 80)
DESC: In the CGA path of gfx_scroll_area, the '.sa_clear_all' loop (scroll >= height: clear whole region) ends with 'jnz .sa_clear_all_loop' at line 19645 and then falls straight through into the '.sa_vesa:' label at 19647 — there is no 'jmp .sa_done'. The normal CGA copy path has the jump (line 19610) but the clear-all path does not. In CGA mode (the default) any scroll call where scroll_pixels >= region height then re-executes the VESA path: it computes Y*640-pitch offsets against ES=0xB800 CGA memory and calls vesa_fill_rect, which both writes a large garbage fill at wrong addresses in the CGA framebuffer and issues INT 0x10 AX=4F05 (VESA set window) while in BIOS mode 4. This is screen corruption plus an undefined BIOS call, directly matching the 'visual anomalies' symptom for any app that scrolls/clears via API 80 (e.g., clearing a text area by scrolling its full height).
VERIFIER: The finding is real; only its line numbers were off by +10. In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, the CGA clear-all loop of gfx_scroll_area (.sa_clear_all, lines 19622-19655) ends with 'jnz .sa_clear_all_loop' at line 19655 and falls straight through into '.sa_vesa:' at line 19657 — there is no 'jmp .sa_done', unlike the normal CGA copy path which has 'jmp .sa_done' at line 19620 (and the mode-12h and VGA clear-all paths, which end with 'jmp .sa_done' at 19862 and via 19923 respectively).

Concrete failure trace, verified in code:
1. API 80 dispatches to gfx_scroll_area (dispatch table line 7429). At entry ES is loaded from [video_segment] (line 19487), which is 0xB800 in CGA mode 0x04 — the boot default (lines 50-51).
2. Mode dispatch (19497-19502) jumps away only for video_mode 0x01/0x12/0x13; CGA falls into the planar-CGA path.
3. Lines 19517-19520: AX = H - scroll; 'jle .sa_clear_all' is taken whenever scroll >= H — the designed "clear whole region" case, trivially reachable (e.g., an app clearing a text area by scrolling its full height, scroll == H).
4. After the clear-all loop finishes (correctly clearing the region in CGA interleaved layout), execution falls into .sa_vesa (19657). There, AX = [.sa_h] - [.sa_scroll] is recomputed (19661-19663); since those saved params are unchanged and were <= 0, 'jle .sa_vesa_clear_all' (19664) is taken.
5. .sa_vesa_clear_all (19748-19755) calls vesa_fill_rect (line 2866) with BX=X, CX=Y, DX=W, SI=H, AL=0 — but ES is still 0xB800, while vesa_fill_rect's contract requires ES=0xA000.
6. vesa_fill_rect computes per-row linear offsets Y*640+X and calls vesa_set_bank (line 2789) with the high word. vesa_cur_bank is 0xFFFF at boot (line 20005, only reset when entering VESA mode at 18659), so the cache check at 2790 misses and INT 0x10 AX=4F05 (VESA set window) is issued while the BIOS is in mode 4 — undefined behavior (typically a harmless "unsupported" return, but some VESA BIOSes will program window registers). It also pollutes the vesa_cur_bank cache (harmless only because mode switch re-invalidates it).
7. The 'rep stosb' then writes W zero-bytes per row for H rows at 640-byte-pitch offsets into segment 0xB800 — wrong layout for CGA (80-byte pitch, 0x2000 field interleave) — blacking out arbitrary scanline spans far outside the requested rectangle. For rows where Y*640+X+W crosses 64K, vesa_fill_rect even issues a second INT 0x10 mid-row (2911-2913); offsets >= 0x8000 land at physical C0000+ (video BIOS ROM region; ignored on real hardware, writable in some emulator/UMB configurations).

No mitigating mechanism exists: ES is not reloaded at .sa_vesa, video_mode is not rechecked, and there is no caller-side guard against scroll >= H (clear-all is the intended handling of that case). The symptom is deterministic visual corruption (spurious black bands outside the target rect) plus an undefined BIOS call on every CGA-mode API-80 call with scroll_pixels >= height. Confirmed; severity assessment (critical for graphics correctness in the default video mode) is reasonable.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, insert one instruction after line 19655 ('jnz .sa_clear_all_loop'), before the '.sa_vesa:' label at line 19657:

    jnz .sa_clear_all_loop
    jmp .sa_done                    ; CGA clear-all complete; do not fall into VESA path

.sa_vesa:

This is a near jump within the same function (mirrors the existing 'jmp .sa_done' at line 19620), fully 8086-safe, and changes no other control flow.

## [critical/high] Heap allocator region overlaps the kernel image and the launcher segment
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:9978, 9983, 9987, 9989, 10010, 10021, 10047 (allocator); 7312-7313 (API exposure); 20053 (heap_initialized, image offset 0x8BB9); 20327 (shell segment); 20488 (45056-byte pad); boot/stage2.asm:13-14 (load address/size) | area:heap allocator
DESC: The allocator hardcodes the heap at segment 0x1400 ('Heap starts at 0x1400:0x0000', line 9954; 'mov ax, 0x1400 / mov ds, ax' lines 9978-9979) with a 60KB extent ('mov word [0], 0xF000' line 9987, limit 'cmp si, 0xF000' line 10010). But boot/stage2.asm lines 13-14 load the kernel at 0x1000:0000 for 88 sectors ('KERNEL_SEGMENT equ 0x1000', 'KERNEL_SECTORS equ 88'), and kernel.asm line 20488 pads the image to 45056 bytes ('times 45056 - ($ - $$) db 0'). So the kernel occupies phys 0x10000-0x1AFFF and the heap base 0x14000 is 16KB INSIDE it. I verified via a NASM listing that image offset 0x4000 (= phys 0x14000) is live code in widget_draw_scrollbar (kernel.asm ~8953-8961, 'mov ax,[btn_w] / sub ax, SCROLLBAR_ARROW_H*2 ...'). The first mem_alloc call writes the 4-byte block header 0xF000,0x0000 over those instructions; any successful allocation lets apps write through 28KB of kernel code/data, and the heap's upper range (up to phys 0x23000) also covers the launcher shell at 0x2000:0000. The heap was clearly placed when the kernel was under 16KB and never moved as the image grew to 44KB. Currently latent (no in-tree caller of API 7/8 — verified by grep), but any app calling malloc corrupts the kernel and produces exactly the class of random crashes reported.
VERIFIER: CONFIRMED, severity critical (latent). Every factual claim verified against the source and a byte-identical NASM rebuild. (1) Heap is hardcoded at segment 0x1400 (kernel/kernel.asm:9978 'mov ax,0x1400', :10047 'mov bx,0x1400') with 0xF000 size/limit (:9987 'mov word [0],0xF000', :10010 'cmp si,0xF000'), i.e. phys 0x14000-0x22FFF. (2) The kernel loads at 0x1000:0000 for 88 sectors (boot/stage2.asm:13-14) and the image is padded to 45056 bytes (kernel.asm:20488), occupying phys 0x10000-0x1AFFF; I rebuilt kernel.bin in WSL with the Makefile's exact nasm invocation and it is byte-identical to build/kernel.bin (cmp passed), with the last nonzero byte at image offset 0xA6D9 - so ~26KB of live kernel code/data (phys 0x14000-0x1A6D9) lies inside the heap range. (3) The listing confirms image offset 0x4000 (= phys 0x14000, heap offset 0) is mid-instruction in the scrollbar drawing code at source lines 8953-8956 ('mov ax,[btn_w]' at 0x3FFB, 'sub ax,SCROLLBAR_ARROW_H*2' spanning 0x3FFE-0x4000, 'cmp ax,SCROLLBAR_MIN_THUMB' at 0x4001); the 4-byte heap-init header write clobbers these instructions. (4) The heap top (0x23000) overlaps the launcher shell segment 0x2000 (APP_SEGMENT_SHELL equ 0x2000 at kernel.asm:20327; apps/launcher.asm:6), covering 0x20000-0x22FFF. (5) mem_alloc/mem_free are exposed via kernel_api_table slots 7/8 (kernel.asm:7312-7313); grep of apps/ shows no in-tree app defines API function 7 or 8, so the bug is latent but fires on the first third-party malloc. No mitigating mechanism exists (no bounds check, no relocation, nothing else owns 0x1400). ADDITIONAL BUG the original finding missed: at kernel.asm:9983/9989 the [heap_initialized] flag is accessed AFTER DS is switched to 0x1400, so it actually reads/writes phys 0x14000+0x8BB9=0x1CBB9 (unowned RAM in the kernel-launcher gap), not the kernel variable at offset 0x8BB9 (phys 0x18BB9, listing line 23468). On QEMU (zeroed RAM) the read sees 0, init runs, and code at 0x14000 is overwritten; on real hardware the garbage flag usually skips init and the first-fit walk interprets kernel code bytes as block headers, writing 0xFFFF into code at [si+2] (line 10021) on any 'fit'. The proposed relocation alone would NOT fix this flag bug (it would move the stray access to HEAP_BASE+0x8BB9 = 0x23BB9, inside the launcher's segment); a cs: override is also required (CS=0x1000 in all dispatch paths since the API table holds near offsets, and segment-override prefixes are 8086-safe). The suggested heap target 0x1B000-0x1FFFF (20KB gap between kernel end and shell) is verified free: the kernel's memory map uses 0x0800 (stage2), 0x1000-0x1AFF (kernel), 0x2000 (shell), 0x3000-0x8000 (segment_pool, kernel.asm:20335), 0x9000 (scratch/clipboard, :20329); nothing claims 0x1B00-0x1FFF.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, add near the allocator (above mem_alloc_stub, ~line 9953):

HEAP_SEGMENT    equ 0x1B00      ; first free paragraph after kernel: 0x1000 + (88*512)/16
HEAP_LIMIT      equ 0x5000      ; 20KB heap: phys 0x1B000-0x1FFFF, below shell at 0x2000:0000

Then six line edits:
  line 9978:  mov ax, 0x1400                    ->  mov ax, HEAP_SEGMENT
  line 9983:  cmp word [heap_initialized], 0    ->  cmp word [cs:heap_initialized], 0
  line 9987:  mov word [0], 0xF000              ->  mov word [0], HEAP_LIMIT
  line 9989:  mov word [heap_initialized], 1    ->  mov word [cs:heap_initialized], 1
  line 10010: cmp si, 0xF000                    ->  cmp si, HEAP_LIMIT
  line 10047: mov bx, 0x1400                    ->  mov bx, HEAP_SEGMENT

(The cs: overrides at 9983/9989 are required because DS=heap segment at that point; CS=0x1000 (kernel) in every dispatch path since kernel_api_table holds near offsets. Segment-override prefix 0x2E is 8086-safe. Also update the stale comments at lines 9954, 9961, 10036 that mention 0x1400/60KB.)

In C:\Users\arin\Documents\Github\unodos\boot\stage2.asm line 14, add a guard comment:
KERNEL_SECTORS  equ 88   ; NOTE: kernel/kernel.asm HEAP_SEGMENT must stay >= 0x1000 + KERNEL_SECTORS*512/16 (= 0x1B00 at 88 sectors); bump HEAP_SEGMENT if the kernel grows.

## [critical/high] heap_initialized flag read/written through the wrong segment, and it lands inside the heap itself
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm lines 9983 and 9989 (faulty accesses; DS switched at 9978-9979), variable declared at line 20053, corrupting store at line 10021; offsets independently confirmed: heap_initialized at image offset 0x8BB9, kernel image size 0xB000 | area:heap allocator
DESC: mem_alloc_stub switches DS to the heap segment BEFORE testing the init flag: 'mov ax, 0x1400 / mov ds, ax ... cmp word [heap_initialized], 0' (lines 9978-9983) and 'mov word [heap_initialized], 1' (9989). heap_initialized is a kernel variable at image offset 0x8BB9 (confirmed via NASM listing, declared line 20053), so these accesses actually hit phys 0x14000+0x8BB9 = 0x1CBB9 — not the kernel variable at 0x10000+0x8BB9. Consequences: (a) whether the heap free-list root gets initialized at the first malloc depends on whatever garbage RAM at 0x1CBB9 contains at boot — if nonzero, the first-fit walk interprets kernel code bytes at 0x14000 as block headers and 'mov word [si+2], 0xFFFF' (line 10021) stamps 0xFFFF into kernel code; (b) 0x1CBB9 is inside the heap's own 0x0000-0xF000 data range, so any allocation whose payload covers offset 0x8BB9 silently resets/flips the flag, causing a spontaneous heap re-initialization that wipes the free list out from under live allocations.
VERIFIER: CONFIRMED by independent rebuild and code trace.

Mechanics verified (C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm):
1. Kernel is flat-binary, [ORG 0x0000], loaded at 0x1000:0000 (line 2; boot/stage2.asm line 13, 'jmp KERNEL_SEGMENT:0x0002' line 57), so kernel variables are reached via DS=0x1000 (or CS=0x1000).
2. mem_alloc_stub (line 9963) sets DS=ES=0x1400 at lines 9978-9980, THEN executes 'cmp word [heap_initialized], 0' (line 9983) and 'mov word [heap_initialized], 1' (line 9989). I assembled the kernel with NASM in WSL (after regenerating kernel/build_info.inc exactly as the Makefile does, Build 400 / v3.23.0): the listing shows 'heap_initialized: dw 0' at image offset 0x8BB9 (line 20053), and the two accesses encode as plain DS-relative '83 3E B98B 00' and 'C7 06 B98B 01 00' with no segment-override prefix. So they hit phys 0x14000+0x8BB9 = 0x1CBB9, not the kernel variable at 0x18BB9. These are the only 3 references to heap_initialized in the entire repo - no boot-time init with correct DS exists, nothing zeroes the 0x1400 region, and no other mechanism (cli, bounds check, caller contract) prevents it.
3. Consequence (a) verified: built kernel is 45,056 bytes (0xB000), so the image spans phys 0x10000-0x1AFFF. Phys 0x1CBB9 is past the image end and below the shell segment (APP_SEGMENT_SHELL=0x2000, line 20327) - i.e. unloaded, uninitialized RAM. If it happens to be nonzero (real hardware, warm reboot; QEMU cold boot usually zeroes it), init is skipped and the first-fit walk at lines 9993-10011 reads kernel CODE at phys 0x14000 (image offset 0x4000 = scrollbar widget code, bytes 10 83 F8 04...) as block headers; if a pseudo-block looks free and large enough, line 10021 stamps 0xFFFF into kernel code and returns a bogus pointer.
4. Consequence (b) verified: 0x8BB9 < 0xF000 (the walk limit, line 10010), and the allocator never splits blocks - the first allocation takes the whole 0xF000 block, so its payload (offsets 4-0xEFFF) covers heap offset 0x8BB9. Any caller write through that offset silently rewrites the flag at 0x1CBB9: zero triggers spontaneous heap re-init on the next malloc (free list wiped under live allocations); nonzero garbage there behaves as in (a).

Exploitability/reachability caveat: mem_alloc_stub is exposed as public INT 0x80 API 7 (dispatch table line 7312, AH selects function, line 140) and documented in docs/API_REFERENCE.md, but no in-tree app currently issues AH=7/8 (grep of apps/ found none) and the kernel never calls it directly. The bug is therefore latent today, but real and critical for any API consumer.

Compounding issue beyond the finding (the auditor's fix is correct but insufficient to make malloc safe): the kernel has grown to 0xB000 bytes, so the heap base 0x1400:0000 (phys 0x14000) lies INSIDE the loaded kernel image. Even with the flag fixed, the very first malloc's init writes 'mov word [0],0xF000 / mov word [2],0' (lines 9987-9988) overwrite 4 bytes of live kernel code at image offset 0x4000, and the heap's 0xF000 range (to phys 0x22FFF) also overlaps the shell loaded at 0x20000. docs/MEMORY_LAYOUT.md's claim that the heap is '16KB after kernel start' is stale (Makefile comment still says '28KB kernel'). The heap base must additionally be moved above the kernel image (e.g. segment 0x1B00 or higher, with the size constant adjusted) before API 7 is usable at all; that should be filed as its own finding.
FIX: Minimal fix for THIS finding (8086-safe: CS override prefix 0x2E exists on 8086; CS is always 0x1000 inside the kernel, both for direct calls and via the INT 0x80 vector installed as 0x1000:int_80_handler at kernel.asm lines 111-112):

In mem_alloc_stub, change line 9983 from
    cmp word [heap_initialized], 0
to
    cmp word [cs:heap_initialized], 0

and change line 9989 from
    mov word [heap_initialized], 1
to
    mov word [cs:heap_initialized], 1

(Equivalent alternative: hoist the cmp/jne above the 'mov ds, ax' at line 9979 and set the flag before switching DS - mov does not alter flags, so 'cmp word [heap_initialized],0 / mov ax,0x1400 / mov ds,ax / mov es,ax / jne .heap_ready' is also valid.)

NOTE: this fixes only the flag aliasing. The allocator remains unsafe to call because the heap base 0x1400:0000 overlaps the 0xB000-byte kernel image (first-block init at lines 9987-9988 stomps kernel code at phys 0x14000) and the 0xF000 heap range overlaps the shell at 0x20000. A follow-up change must move the heap base above the kernel image end and shrink/relocate the range accordingly.

## [critical/high] kernel.asm requires 186/286/386+ instructions in 357 places — kernel cannot run on 8088
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm — corrected key sites: movzx+bt dispatch at 171-175 and 202-206; push imm at 370 and 14572; popa (dummy-frame consume) at 1812; imul at 4953/4973/4987/4999; FAT16/IDE 32-bit region 11868-14250 (163 of 357 errors); scheduler pusha 14785, shl si,5 at 14794, popa 14857, dummy pusha frame 14913-14921; mov dword pairs at 13225-13226 and 16886-16887. Boot chain also affected: stage2.asm (3), mbr.asm (10), vbr.asm (1), stage2_hd.asm (56). | area:8088 compatibility
DESC: Assembling with 'CPU 8086' produces 357 errors. Breakdown: movzx x80 (386+, e.g. line 172 'movzx bx, ah'; line 4972 'movzx si, byte [kmenu_count]'), shl/shr with imm>1 x125 (186+, e.g. line 224 'shl si, 5 ; SI = handle * 32', line 2446 'shr ax, 3', line 14784 'shl si, 5' in the scheduler itself), pusha/popa x39 (186+, e.g. line 3026, and load-bearing in the scheduler: line 14775 'pusha' in app_yield_stub, line 14847 'popa', line 1812 'popa ; Consume dummy pusha frame', plus the hand-built 8-word dummy pusha frame at 14904-14912), push immediate x3 (186+, line 370 'push word int80_return_point' in the INT 0x80 dispatcher, lines 14562/14644), imul reg,imm x4 (186+, lines 4953/4973/4987/4999 'imul ax, KMENU_ITEM_H'), bt x2 (386+, lines 173/204 'bt [api_drawing_bitmap], bx' in the syscall-validity check), and 146 sites using 32-bit registers/dword operands (386+), almost all in the FAT16/IDE driver lines 11868-14250 (e.g. 11878 'push eax', 11885 'add eax, [fat16_partition_lba]', 11939 'div ebx', 12199 'imul eax, ebx', 14186 'mov eax, [esp + 12]' — ESP addressing is 386-only, 14221 'rep insw' is 186+), plus 2 trivial 'mov dword [di+4], 0' at 16876-16877. On a real 8088 the very first INT 0x80 dispatch (line 370 push imm) and every app_yield pusha execute as different instructions (opcode 0x60 is an undocumented alias of JO on 8088/8086), so the OS crashes immediately. 163 of the 357 sites are inside the FAT16/IDE region; the other 194 are mechanically fixable.
VERIFIER: CONFIRMED by independent assembly test and code reading.

Reproduction: prepended 'cpu 8086' via a wrapper and assembled C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm with NASM 2.16.01 (WSL): exactly 357 "no instruction for this cpu level" errors, first at line 172, last at 19815. Baseline (no CPU directive) assembles clean to 45,056 bytes. There is no 'CPU' directive anywhere in the repo, so nothing currently enforces the target.

Target claim is real: README.md states "just BIOS services and an Intel 8088 or later processor", "runs on an original IBM PC", and the requirements table lists minimum "Intel 8088 @ 4.77 MHz"; the Makefile header says "PC XT GUI Operating System". So 8088 compatibility is an explicit project requirement, and it is violated.

Category counts independently reproduced and matching the finding: movzx=80, shift-with-imm>1=125, pusha=17 + popa=22 = 39, bt=2 (lines 173 and 204), imul r16,imm=4 (lines 4953/4973/4987/4999, all KMENU_ITEM_H), and exactly 163 of the 357 error lines fall in the FAT16/IDE region 11868-14250. Remaining ~105 are 32-bit reg/dword/insw sites (the finding's "146" double-counts 32-bit shifts/imuls already counted in other buckets — its sub-counts sum to 401 > 357; bookkeeping quibble only).

Concrete failure path verified: int_80_handler (line 121) flows unconditionally for every valid syscall (AH 0-104) into line 172 'movzx bx, ah' and line 173 'bt [api_drawing_bitmap], bx'. On an 8088/8086, opcode 0F is POP CS, so the first INT 0x80 from the launcher loads CS from the just-pushed BX and jumps into garbage — immediate crash on first syscall. Independently, 'push word int80_return_point' (line 370, opcode 68) decodes as JS rel8 on 8088, and the scheduler's pusha/popa (0x60/0x61 decode as JO/JNO) are load-bearing: app_yield_stub pusha at line 14785, popa at 14857, 'shl si, 5' at 14794, hand-built 8-word dummy pusha frame at lines 14913-14921 (writes [es:0xFFEE] down to 0xFFE0), consumed by 'popa' at line 1812. I searched the whole kernel for any runtime CPU detection or gating (cpuid/cpu_type/detect/8088): none exists. No mechanism prevents the bug.

Corrections to the finding: (1) push-imm count is 2, not 3 — actual sites are line 370 and line 14572 ('push word APP_ERR_READ_FAILED'); cited lines 14562/14644 are a label and a comment. (2) Line numbers above ~14500 are off by ~10: pusha is 14785 not 14775, popa 14857 not 14847, shl si,5 at 14794 not 14784, dummy frame 14913-14921 not 14904-14912, the trailing 'mov dword [di+4/8], 0' pair is at 16886-16887 not 16876-16877. (3) Severity caveat: the project's own test rig (qemu-system-i386 -M isapc) uses a 486-class CPU, so this is invisible in QEMU and only manifests on real 8088/8086/286 hardware or cycle-accurate emulators (86Box/PCem) — "critical" is correct relative to the stated XT target, but nothing is broken on 386+.

Scope addendum the finding missed: the boot chain has the same defect — under CPU 8086, stage2.asm has 3 errors, mbr.asm 10, vbr.asm 1, stage2_hd.asm 56 (boot.asm is clean). Fixing kernel.asm alone leaves HD boot (and stage2 floppy boot) still 8088-incompatible.
FIX: Add 'cpu 8086' at the top of kernel.asm and fix until NASM is clean. Highest-impact minimal replacements (all 8086-legal):

1) pusha/popa (39 sites) — macros preserving the existing 8-word frame layout so the dummy frame at 14913-14921 and APP_OFF_STACK_PTR math stay valid (the SP slot is ignored on restore, matching real popa semantics):
%macro PUSHA86 0
    push ax
    push cx
    push dx
    push bx
    push sp        ; SP slot — value unused by restore
    push bp
    push si
    push di
%endmacro
%macro POPA86 0
    pop di
    pop si
    pop bp
    add sp, 2      ; skip SP slot
    pop bx
    pop dx
    pop cx
    pop ax
%endmacro
Text-replace every pusha->PUSHA86, popa->POPA86 (incl. lines 1812, 14785, 14857).

2) Dispatcher bitmap test (lines 171-175, repeat at 202-206) — replaces movzx+bt+jnc, preserves AX/BX/CX:
    push bx
    push cx
    push ax
    mov al, ah          ; AL = function number
    xor ah, ah
    mov bx, ax
    mov cl, 3
    shr bx, cl          ; BX = byte index
    mov cl, al
    and cl, 7           ; CL = bit index within byte
    mov al, [api_drawing_bitmap + bx]
    shr al, cl
    test al, 1
    pop ax
    pop cx
    pop bx
    jz .no_translate    ; was jnc

3) push imm (line 370) — push m16 is 8086-legal (FF /6), zero register impact:
    push word [cs:int80_ret_const]
    jmp word [cs:syscall_func]
...data: int80_ret_const: dw int80_return_point
Same pattern (or a dead register) for line 14572.

4) movzx r16, r/m8 (80 sites, scriptable): 'movzx bx, ah' -> 'mov bl, ah' / 'xor bh, bh'; 'movzx si, byte [x]' -> 'mov si, 0' is wrong — use 'mov al,[x] / xor ah,ah / mov si,ax' or 'xor si,si / mov byte... ' per-site with a dead 8-bit reg.

5) shl/shr reg,N (125 sites): N<=3 -> N repeated 'shl reg,1'; else 'mov cl,N / shl reg,cl' with 'push cx/pop cx' wrap wherever CL liveness is unproven (the FAT16 CHS code keeps CL live deliberately). E.g. line 14794 in app_yield_stub (CX dead after PUSHA86): 'mov cl,5 / shl si,cl'.

6) imul ax,KMENU_ITEM_H (4953/4973/4987/4999): 'push dx / mov cx,KMENU_ITEM_H / mul cx / pop dx' (mul clobbers DX) — or shift/add expansion of the constant.

7) FAT16/IDE region 11868-14250 (163 sites) is a genuine rewrite: replace EAX/EBX LBA math with DX:AX 16-bit word-pair arithmetic (XT-era partition LBAs fit in 24 bits), 'div ebx' -> two-step 16-bit division by byte-sized SPT/heads, 'rep insw' -> 'mov dx,port / .l: in ax,dx / stosw / loop .l', and the 'mov dword [..],0' sites -> two 'mov word' stores (e.g. 16886-16887).

8) Apply the same treatment to boot/stage2.asm, boot/mbr.asm, boot/vbr.asm, boot/stage2_hd.asm (3/10/1/56 errors respectively) — otherwise the kernel fix is unreachable on real XT hardware booting from HD. Verify by adding 'cpu 8086' to each file and rebuilding 'make all hd-image' clean, then boot-test in QEMU (isapc) for regressions; real-8088 behavior additionally needs 86Box/PCem.

## [critical/high] SUMMARY: 1153 non-8086 instruction sites across 21 of 22 .asm files; ~80% mechanically fixable, FAT16/boot 32-bit math needs a real rewrite
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:Genuine 32-bit math requiring redesign: kernel/kernel.asm lines 11955-14310 (144 sites, fat16_read_sector through the IDE LBA helper incl. rep insw at 14298) + 2 trivial dword stores at 16953-16954; boot/stage2_hd.asm (49 sites); boot/mbr.asm (9); apps/mkboot.asm (9 trivial dword stores). All other counts as originally cited. | area:8088 compatibility - effort sizing
DESC: Method: every .asm assembled with 'CPU 8086' prepended (NASM 2.16.01); each error is a 186+/286+/386+ instruction with an exact line. Totals: 1153 sites. By category (overlapping where a movzx/shl also uses a 32-bit reg): pusha/popa ~506 (186+; on 8088 opcode 0x60/0x61 execute as undocumented JO/JNO aliases — silent control-flow corruption, not a fault); movzx ~237 (386+); shl/shr/sar with imm>1 ~241 (186+); 32-bit register/dword operands ~213 concentrated in kernel FAT16/IDE driver (146 sites, lines 11868-14250), boot/stage2_hd.asm (49), boot/mbr.asm (9), apps/mkboot.asm (9 trivial dword stores); imul x10; push-imm x3; bt x2 (386+); 'jcc near' x3 (386+); 'rep insw' x1 (186+). Per-file: kernel 357, pacmanv 154, pacman 121, notepad 88, launcher 61, stage2_hd 56, outlastv 54, browser 49, tetris 46, outlast 44, tetrisv 44, music 17, sysinfo 12, mkboot 11, mbr 10, settings 10, clock 7, hello 4, mouse_test 4, stage2 3, vbr 1. CLEAN files: boot/boot.asm (floppy boot sector, 0 errors) and the three font data files (kernel/font4x6.asm, font8x8.asm, font8x12.asm, included into the kernel with no code). No cpuid, no protected-mode ops (cr0/smsw), no o32/a32 or raw 0x66/0x67 prefixes, and no 'push sp' semantic dependence anywhere (grep-verified). Memory fits: top user segment 0x8000 ends at 0x8FFFF (~576KB), inside 640KB. Effort: (a) ~950 sites are 1:1 mechanical (PUSHA86/POPA86 macro include, movzx->mov+xor, shift-imm->repeated/CL shifts, push-imm->scratch reg, dword stores->word pairs, bt->mask test, jcc-near->inverted short+jmp) — a sed/awk pass plus 'CPU 8086' directives and reassembly, est. 1-2 days incl. regression in QEMU with a 8086-restricted run; (b) ~210 sites of genuine 32-bit LBA/FAT arithmetic in kernel 11868-14250 + stage2_hd + mbr require redesign around 16-bit DX:AX math (feasible: XT-class LBAs < 2^24) — est. 3-5 days plus on-disk regression testing. Note: none of these explain the user's CURRENT crash/visual/input symptoms, which were observed on 186+-capable hardware/emulators where these instructions execute natively; this audit addresses the separate 8088 porting goal.
VERIFIER: Independently reproduced the entire audit with NASM 2.16.01 (the claimed version) by assembling every .asm with 'cpu 8086' injected via --before (baseline assemblies are all 0-error, so every error is attributable to CPU level). Results match the finding exactly: 1153 total unique file:line errors; per-file counts identical for all 21 affected files (kernel 357, pacmanv 154, pacman 121, notepad 88, launcher 61, stage2_hd 56, outlastv 54, browser 49, tetris 46, outlast 44, tetrisv 44, music 17, sysinfo 12, mkboot 11, mbr 10, settings 10, clock 7, hello 4, mouse_test 4, stage2 3, vbr 1); boot/boot.asm and the three font data files are clean. Category counts verified by mapping each error back to its source mnemonic: pusha 245 + popa 261 = 506; movzx = 237; shl 121 + shr 114 + sar 6 = 241; imul = 10; push-imm = 3 (kernel.asm:370, 14639, 14721); bt = 2 (kernel.asm:173, 204 - bitmap tests in the API dispatcher); jcc near = 3 (stage2_hd.asm:164, 180, 251); rep insw = 1 (kernel.asm:14298, IDE PIO sector read). 32-bit-operand sites: exactly 213, distributed kernel 146 / stage2_hd 49 / mbr 9 / mkboot 9 as claimed; the first kernel site is inside fat16_read_sector, confirming the FAT16/IDE attribution. Negative claims verified by grep: zero push sp, cpuid, smsw, cr0, o32/a32, or raw db 0x66/0x67 anywhere (one apparent hit was 'VGA320' in a comment). HEAP_SEGMENT equ 0x8000 at kernel.asm:9963 confirms the memory-layout statement (0x9000 is also used as a scratch segment, FDLG_BUF_SEG, ending exactly at the 640KB boundary - still fits). The pusha/popa-as-JO/JNO hazard is documented 8086/8088 behavior (opcodes 0x60-0x6F alias 0x70-0x7F), so 'silent control-flow corruption' is accurate, and README.md explicitly promises 'Intel 8088 or later' (minimum CPU '8088 @ 4.77 MHz'), justifying critical severity for that stated target. The note that none of this explains current crashes on 186+-capable hosts is logically sound - all 1153 instructions execute natively there. TWO MINOR IMPRECISIONS: (1) the kernel 32-bit cluster actually spans lines 11955-14310 (139 sites in the cited 11868-14250 window, 5 more at 14263-14310 in the LBA byte-extraction helper, and 2 at 16953-16954 which are trivial 'mov dword [di+4/8], 0' file-handle initializations, mechanically fixable as word pairs - not math needing redesign); (2) '21 of 22 .asm files' is only true if the 3 font data files are excluded from the denominator (21 of 25 counting all build-tree .asm). Neither changes the substance, totals, or effort split (940 mechanical + 213 32-bit, of which ~18 are trivial dword stores, matching the ~950/~210 estimate).
FIX: Phase 1 entry point (verified 8086-safe). Create kernel/cpu8086.inc and %include it from every .asm, then add 'cpu 8086' as the first directive of each file so regressions fail at assembly time (CI gate: nasm -f bin -Ikernel/ --before 'cpu 8086' -o /dev/null <file> must exit 0):

%macro PUSHA86 0        ; same 8-word frame layout as PUSHA (SP slot = junk, never read by POPA)
    push ax
    push cx
    push dx
    push bx
    push bp             ; placeholder in the SP slot (avoids 'push sp' semantic difference)
    push bp
    push si
    push di
%endmacro

%macro POPA86 0         ; flag-preserving (no 'add sp,2'), matches POPA which does not alter flags
    pop di
    pop si
    pop bp
    pop bx              ; discard SP slot
    pop bx
    pop dx
    pop cx
    pop ax
%endmacro

%macro SHL_N 2          ; SHL_N reg, count  (count 2..7; count>=8 on a 16-bit reg: use mov xH,xL / xor xL,xL first)
  %rep %2
    shl %1, 1
  %endrep
%endmacro
%macro SHR_N 2
  %rep %2
    shr %1, 1
  %endrep
%endmacro

Mechanical rewrites per category (~940 sites): pusha/popa -> PUSHA86/POPA86 (layout-compatible, so existing [bp+n] accesses into the saved frame keep working); movzx r16, r/m8 -> mov rL, r/m8 + xor rH, rH (swap order if the source addressing uses the dest reg); shift-by-imm -> SHL_N/SHR_N or mov cl, n + shl r, cl when CL is free; push imm (kernel.asm:370, 14639, 14721) -> mov ax, imm + push ax (or a free scratch reg); bt [mem], bx (kernel.asm:173, 204) -> compute byte index (mov si,bx / mov cl,bl / and cl,7) and test [mem+si_byte], mask via a 1-shifted-by-CL mask, or replace the bitmap with a byte table; jc/jz near (stage2_hd.asm:164, 180, 251) -> jnc %%skip / jmp target / %%skip: (inverted short + jmp); rep insw (kernel.asm:14298) -> .rd: in ax, dx / stosw / loop .rd; trivial 'mov dword [x], 0' (mkboot's 9 sites, kernel.asm:16953-16954) -> two 'mov word' stores.

Phase 2 (cannot be scripted): rewrite the 32-bit LBA/cluster arithmetic in kernel/kernel.asm 11955-14310, boot/stage2_hd.asm, and boot/mbr.asm as 16-bit DX:AX pair math (add/adc, sub/sbb, and a 32/16 long-division helper for cluster->LBA and FAT-offset computation; XT-era CHS-addressable disks keep LBAs under 2^24 so DX:AX covers them). Gate everything by adding the --before 'cpu 8086' assembly of all 22 code files to the Makefile as a check target before any real-8088 testing.

## [critical/high] fat12_read: reading from file position != 0 pops one word too many and returns through a corrupted stack
- src:performance | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:prologue pushes: 11721-11726; position check: 11763-11764; broken .not_supported epilogue: 11880-11890 (stray pop cx at 11881); dead .read_error duplicate: 11903-11913 | area:filesystem / FAT12
DESC: fat12_read's prologue pushes 6 registers (bx,cx,dx,si,di,bp at 11644-11649). The '.not_supported' exit — reached whenever current position != 0 ('cmp bp, 0 / jne .not_supported', 11686-11687) — executes 'pop cx' followed by the full 6-pop epilogue (11804-11813): 7 pops for 6 pushes, so RET pops the caller's saved BX as the return address and jumps to garbage. Any FAT12 app or kernel path that does two sequential fs_read calls, or fs_seek (API 75) then read, crashes the machine. The duplicate-pattern label '.read_error' (11826-11836) has the same extra 'pop cx' but appears unreferenced. This is a concrete mechanism for 'apps crash' on floppy boots. Independently, the position!=0 limitation itself ('For simplicity, only support reading from start of file', 11684) silently breaks chunked reads on FAT12 while FAT16 supports them.
VERIFIER: CONFIRMED, with corrected line numbers (the finding's were off by ~78 lines, likely from an older file revision). In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm: fat12_read's prologue pushes exactly 6 registers — bx,cx,dx,si,di,bp (lines 11721-11726) — and there are no further pushes on the path to the position check at 11763-11764 ('cmp bp, 0' / 'jne .not_supported', the only reference to that label). The '.not_supported' exit at 11880-11890 executes a stray 'pop cx' followed by the full 6-pop epilogue: 7 pops for 6 pushes. The final 'pop bx' therefore consumes the return address, and 'ret' transfers control to the next word on the stack — for the primary caller fs_read_stub (call at 10576) that word is the API client's SI value pushed at line 10553, i.e., an arbitrary code offset in real mode with SP also left misaligned by 2. No protective mechanism exists: fs_read_stub does not gate position, and fs_seek_stub (API 75, lines 18933-18966) sets position [si+8] on FAT12 handles without any FS-type restriction. Trigger paths verified concrete: (1) any two sequential fs_read calls on the same FAT12 handle — a successful read updates position at 11852-11856, so the second call enters with BP!=0; this includes the universal 'read until 0 bytes returned' EOF loop, because the CX clamp to 0 (11754-11758) happens BEFORE the position check, so even a should-return-0 read at EOF corrupts the stack; (2) fs_seek to nonzero then read. The kernel's own in-tree callers (settings loader ~1900, app loader ~14586, fs_read_header_stub) all happen to be single whole-file reads from position 0, which is why boot survives while apps crash. The dead '.read_error' label with the same stray 'pop cx' is at 11903-11913 (not 11826-11836); nothing references it — the cluster loop uses '.read_error_multi' (11869, referenced via jc at 11809). Secondary claim also verified: fat16_read (12751+) genuinely supports arbitrary read positions (derives cluster index and intra-sector offset from [si+8]), so FAT12's position!=0 rejection is a real functional gap on top of the stack bug. Severity 'critical' is justified for floppy/FAT12 boots.
FIX: Minimal critical fix — remove the stray 'pop cx' at line 11881 so .not_supported pops exactly what the prologue pushed, and delete the unreferenced dead block at 11903-11913:

.not_supported:
    mov ax, FS_ERR_READ_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; DELETE entirely (dead code, lines 11903-11913):
; .read_error:
;     pop cx
;     ... (same 6-pop epilogue + ret)

Recommended additional minimal change (8086-safe) so the standard read-until-0 EOF loop terminates instead of erroring: insert a zero-byte fast path at .read_start (line 11760), before the position check:

.read_start:
    test cx, cx                     ; nothing to read (e.g., at EOF)?
    jnz .have_bytes
    xor ax, ax                      ; AX = 0 bytes read
    clc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret
.have_bytes:
    cmp bp, 0
    jne .not_supported

Full fix (follow-up, larger): implement positional reads as in fat16_read (12751+) — advance the start cluster by position/512 get_next_cluster steps and begin copying at offset position%512 within the first sector.

## [high/high] apps/clock.asm: 7 non-8086 sites
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\clock.asm:apps/clock.asm lines 69, 96, 97, 257, 270, 335, 378 (popa is at 378, not 69; all other cited lines accurate) | area:8088 compatibility
DESC: 7 CPU-8086 errors: pusha/popa x2 (line 69), sar-imm x2, movzx x1 (line 96 'movzx ax, cl'), shl x1 (line 97 'shl ax, 3 ; * 8 chars'), shr x1.
VERIFIER: Verified by reading C:\Users\arin\Documents\Github\unodos\apps\clock.asm. The count of 7 non-8086 sites is exactly right, with one line-number refinement (the second pusha/popa site is popa at line 378, not 69):

1. Line 69 `pusha` (186+)
2. Line 378 `popa` (186+)
3. Line 96 `movzx ax, cl` (386+)
4. Line 97 `shl ax, 3` (shift-by-imm>1 is 186+)
5. Line 257 `sar ax, 5` (186+)
6. Line 270 `sar ax, 5` (186+)
7. Line 335 `shr al, 4` (186+)

I checked every other candidate and the auditor counted correctly: line 100 `shr bx, 1` is valid 8086 (D1 /5 encoding), single-operand `imul bl` (lines 256, 269), `mul cl`/`mul ah`/`div cl` are all 8086 instructions. No `cpu 8086` directive exists anywhere in the repo, so NASM silently accepts these.

Target relevance: README.md line 9 states the OS requires "an Intel 8088 or later processor", line 201 lists minimum CPU "Intel 8088 @ 4.77 MHz", and CHANGELOG line 1255 says "Target hardware: Intel 8088". So 8088 compatibility is an explicit project contract, and severity=high is justified: on a real 8088/8086 the app derails at the very first instruction — opcode 60h (pusha) executes as an alias of 70h (JO rel8) on pre-186 CPUs, taking a data-dependent conditional jump; C0/C1-group shifts (sites 4-7) execute as undocumented aliases of RET imm16/RET, and 0Fh (movzx prefix) executes as POP CS. Any of these is an immediate crash/hijack. No mitigating mechanism exists (no CPU detection, no alternate code path, no kernel-side trap for invalid opcodes — 8086 has no #UD).

One contextual caveat (does not refute the finding): kernel/kernel.asm itself contains 123 pusha/popa/movzx occurrences, so the OS cannot boot on an 8088 until the kernel is also fixed; this file-level fix is necessary but not sufficient. TODO.md line 31 ("8086-compatible boot for HP 200LX, Sharp PC-3100") confirms 8086 compat is an active, not-yet-achieved goal.

The auditor's note about sar is correct: SAR r/m,1 (D1 /7) and SAR r/m,CL (D3 /7) exist on 8086; only SAR r/m,imm8 (C1 /7) is 186+. Register-liveness check for the CL-based replacements: in .compute_endpoint, CX is pushed at line 249 and popped at line 276, and nothing between the two sar sites touches CL, so a single `mov cl, 5` can serve both. At lines 96-97 CX is dead afterwards (the main loop reloads CX from API_GET_RTC_TIME). At line 335 CL IS live (it holds the BCD ones digit consumed at line 338), so the `mov cl,N` pattern cannot be used there — use four `shr al, 1` instead.

Side observation: popa at line 378 restores entry-time AX, clobbering the exit status set at lines 369/373 (`xor ax,ax` / `mov ax,1`). The mechanical replacement below preserves this existing behavior; if the kernel actually reads the app's AX return value, the final `pop ax` should be `add sp, 2` instead — that is a separate pre-existing bug, not part of this finding.
FIX: In C:\Users\arin\Documents\Github\unodos\apps\clock.asm:

1) Line 69 — replace `pusha` with:
    push ax
    push cx
    push dx
    push bx
    push bp
    push si
    push di

2) Line 378 — replace `popa` with (reverse order):
    pop di
    pop si
    pop bp
    pop bx
    pop dx
    pop cx
    pop ax
   (Behavior-identical to popa, including clobbering the AX exit status from lines 369/373. If the exit status must survive, use `add sp, 2` instead of the final `pop ax`.)

3) Lines 96-97 — replace:
    movzx ax, cl
    shl ax, 3
   with (CX is dead after this point; main loop reloads it from API_GET_RTC_TIME):
    mov al, cl
    mov ah, 0
    mov cl, 3
    shl ax, cl                      ; * 8 chars ("HH:MM:SS")

4) Lines 257 and 270 — in .compute_endpoint (CX saved/restored by push cx at 249 / pop cx at 276, and nothing between the two sites modifies CL), insert `mov cl, 5` once before line 257 and change both `sar ax, 5` to `sar ax, cl`:
    mov cl, 5
    ...
    sar ax, cl                      ; /32   (line 257 site)
    ...
    sar ax, cl                      ; /32   (line 270 site)

5) Line 335 — in .bcd_to_bin, CL is live (holds the ones digit added at line 338), so do NOT use a CL count; replace `shr al, 4` with:
    shr al, 1
    shr al, 1
    shr al, 1
    shr al, 1

Recommended guard: add `cpu 8086` after `[BITS 16]` (line 7) so NASM rejects any future non-8086 instruction in this file.

## [high/high] apps/hello.asm (the template/sample app) uses pusha/popa
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\hello.asm:Lines as cited are accurate: pusha at apps/hello.asm:55 and :127, popa at :122 and :135 (also: README.md:375/419 and docs/APP_DEVELOPMENT.md:31/97/159/174 carry the same pattern) | area:8088 compatibility
DESC: 4 CPU-8086 errors: pusha at lines 55 and 127, popa at 122 and 135 (verified line 55 reads '    pusha'). Because hello.asm is the sample every new app copies (the README sample has the same pattern), the pusha idiom propagates to all apps. On 8088, opcode 0x60 executes as JO — registers are never saved and execution may jump if OF happens to be set, i.e. silent corruption rather than a clean fault.
VERIFIER: Confirmed, with an important severity caveat. All four cited sites are exact: pusha at apps/hello.asm:55 (entry prologue) and :127 (draw_content), popa at :122 (.exit epilogue) and :135. The failure mechanism is correctly described: on real Intel 8086/8088 silicon, opcodes 0x60-0x6F alias the conditional jumps 0x70-0x7F, so pusha (0x60) executes as JO rel8 — it consumes the next byte (the 0x1E of 'push ds' at line 56) as a displacement; OF set jumps +30 bytes into the prologue, OF clear falls through having saved nothing and having skipped 'push ds'. No fault, silent corruption. 8088 IS an explicitly stated target: README.md line 9 ("Intel 8088 or later"), line 201 (minimum CPU "Intel 8088 @ 4.77 MHz"), line 466. The propagation claim is also confirmed: the README inline sample app uses pusha/popa (README.md:375,419) and docs/APP_DEVELOPMENT.md sample does too (lines 31,97,159,174); no 'cpu 8086' directive exists anywhere, so NASM never flags these. No mitigating mechanism exists (no kernel CPU detection, no V20 check, nothing prevents execution on 8088). CAVEAT THAT REFRAMES SEVERITY: pusha/popa appears 512 times across 20 files, including 39 uses in kernel/kernel.asm and 2 in boot/stage2.asm (lines 64,138). A real 8088 corrupts itself in the stage-2 bootloader before any app loads, so hello.asm is not the gating factor — fixing it alone yields zero 8088 compatibility. The correct framing is a project-wide 8086-cleanliness gap (or an incorrect README claim; 80186/NEC V20 would be the honest minimum). Within that program-wide fix, updating hello.asm + both doc samples is the right template-level step the finding proposes. Bonus: popa at line 122 also clobbers the exit status set by 'xor ax,ax'/'mov ax,1' immediately before .exit, so excluding AX from an explicit save/restore fixes a second latent bug.
FIX: In apps/hello.asm, add 'cpu 8086' after '[BITS 16]' (line 6) so NASM rejects any future 186+ opcodes, and define 8086-safe macros near the top of the code section:

%macro PUSHA86 0
    push ax
    push cx
    push dx
    push bx
    push bp
    push si
    push di
%endmacro
%macro POPA86 0
    pop di
    pop si
    pop bp
    pop bx
    pop dx
    pop cx
    pop ax
%endmacro

Then replace line 55 'pusha' -> PUSHA86, line 122 'popa' -> POPA86, line 127 'pusha' -> PUSHA86, line 135 'popa' -> POPA86. (For the entry/exit pair, optionally drop the ax push/pop — push cx..di / pop di..cx — so the exit status in AX set at lines 113/117 actually reaches the kernel instead of being clobbered, as the current popa does.) Apply the identical change to the sample apps in README.md (lines ~375/419) and docs/APP_DEVELOPMENT.md (lines ~31/97/159/174), preferably by shipping the macros in a shared SDK include (e.g. apps/app86.inc) that the samples %include. Note: for the OS to actually run on 8088 the same pass is required in boot/stage2.asm (pusha/popa at 64/138) and kernel/kernel.asm (39 sites) — otherwise amend README.md lines 9/201/466 to state 80186/NEC V20 minimum.

## [high/high] Desktop icon labels overlap: 12px font advance x 11-char names exceeds the 80px grid column, no truncation or clipping
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\apps\launcher.asm:apps/launcher.asm:1053-1068 (label draw, no limit), 668-682 (raw 12B name copy), 60/72 (COL_WIDTH=80); kernel/kernel.asm:15883-15898 (duplicate unclipped label path), 16150-16151 (clip force-disabled during repaint), 19969-19974 + 19982-19983 (font 1 default, advance 12), 3046-3083 (opaque glyph+gap fill), 15389-15398 (raw 76B copy in desktop_set_icon_stub). Note: kernel file is kernel/kernel.asm, not repo-root kernel.asm. | area:launcher / desktop rendering
DESC: Confirmed boot-visible symptom. `draw_single_icon` draws the label at `icon_x - 8` (1054-1055 `mov bx,[cs:.dsi_x] / sub bx,8`) with API_GFX_DRAW_STRING and no length limit. The default font is font 1 (8x8) whose ADVANCE is 12 px/char (kernel.asm:19973-19974 `db 8, 8, 12, 8` and boot defaults at 19962-19963), so an 11-char header name renders 132 px wide while grid columns are only COL_WIDTH_LO=80 px (launcher.asm:60). Any name longer than ~6 chars ('NOTEPAD', 'SETTINGS', 'Mouse Test'...) runs into the neighboring icon's label region (next label starts at col*80+24). The kernel-side desktop repaint duplicates the same layout (kernel.asm:15874-15888, `sub bx, 8` then `gfx_draw_string_stub` with no clip), so the overlap reappears on every desktop redraw. Additionally, the 12-byte name copied raw from the BIN header (launcher.asm:677-682) and into the kernel icon table is not forced null-terminated, so a header that fills all 12 bytes would print past the name field into adjacent data.
VERIFIER: Confirmed by direct code trace, with one threshold correction. (1) Font: kernel/kernel.asm:19969-19974 sets current_font=1 with draw_font_advance=12; font_table entry at 19982-19983 is `db 8, 8, 12, 8`. launcher.asm contains zero font API calls, and app_start makes tasks inherit current_font (kernel.asm:14930-14931), so desktop labels render at 12 px/char. (2) Geometry: get_icon_position (launcher.asm:1540-1563) yields icon_x=col*80+32 (lo-res, COL_WIDTH_LO=80 at line 60); draw_single_icon draws the label at icon_x-8 (launcher.asm:1054-1055) with API_GFX_DRAW_STRING and no length limit, so adjacent labels are exactly 80px apart while an n-char label paints opaque 12px cells spanning 12*n px (draw_char paints bg for 0-bits AND fills the 4px advance gap, kernel.asm:3046-3083 — text is opaque, not transparent). (3) No protective mechanism exists: gfx_draw_string_stub clips only when clip_enabled=1 (kernel.asm:7546); clip_enabled defaults to 0, is zeroed on task switch (1797), and is EXPLICITLY forced to 0 during desktop repaint (kernel.asm:16150-16151). plot_pixel_* clips at screen edges (3133-3136) so there is no memory corruption — purely visual. (4) Kernel repaint duplicates the layout at kernel.asm:15883-15898 (sub bx,8 then unclipped gfx_draw_string_stub), so the defect recurs on every desktop redraw. THRESHOLD CORRECTION: overlap of actual glyphs requires >=8 chars, not >6. A 7-char name's last glyph ends at label_x+79, exactly flush with the neighbor label at +80 (only its 4px opaque advance-gap touches the neighbor's first glyph, visible only on out-of-order single-icon redraws). So 'NOTEPAD'/'Pac-Man'/'OutLast'/'Dostris' (7) are effectively safe; genuinely colliding stock names are 'Settings' (8), 'PacMan VGA' (10), 'Tetris VGA' (10), 'OutLast VGA' (11). Because icons draw left-to-right in slot order, the boot symptom is mid-cell truncation at the column boundary ('Settings' shows 'Setting'); real garbling appears when draw_single_icon redraws one icon alone (selection click, launcher.asm:746/1881) and its label overpaints the already-drawn right neighbor. (5) Null-termination sub-claim is technically true (raw 12-byte copies at launcher.asm:677-682 and kernel.asm:15397-15398 never force a NUL) but latent: every in-tree app header is NUL-terminated within 12 bytes ('OutLast VGA',0 is exactly 12); only a third-party BIN with 12 non-NUL name bytes would print bounded garbage (reads into adjacent table until a zero byte; no crash since pixel plotter clips). Severity note: real and boot-visible but cosmetic-only (no corruption, no crash) — medium rather than high is defensible.
FIX: Truncate labels at draw time to the 6 chars that fit an 80px column at advance 12 (self-contained; avoids gfx_set_font side effects — API 48 propagates the font to ALL tasks, kernel.asm:7605-7617, so an app cannot safely save/restore it).

1) apps/launcher.asm, draw_single_icon — replace lines 1058-1064 (name pointer setup) with a copy into a local buffer:
    mov al, [cs:.dsi_slot]
    xor ah, ah
    mov cl, 12
    mul cl
    add ax, icon_names
    mov si, ax
    mov di, .dsi_namebuf
    mov cx, 6                       ; 6 chars * 12px advance = 72px <= 80px column
.dsi_trunc:
    mov al, [cs:si]
    mov [cs:di], al
    test al, al
    jz .dsi_name_set
    inc si
    inc di
    loop .dsi_trunc
    mov byte [cs:di], 0             ; force terminator after 6 chars
.dsi_name_set:
    mov si, .dsi_namebuf
and add near .dsi_slot (line 1081):
.dsi_namebuf: times 7 db 0

2) kernel/kernel.asm, draw_desktop_region label path — replace lines 15893-15899 with a truncating copy (kernel DS=0x1000 here, name already in kernel data so caller_ds swap to 0x1000 stays):
    push si
    push cx
    add si, DESKTOP_ICON_OFF_NAME
    mov di, .ddr_namebuf
    mov cx, 6
.ddr_trunc:
    lodsb
    mov [di], al
    test al, al
    jz .ddr_name_set
    inc di
    loop .ddr_trunc
    mov byte [di], 0
.ddr_name_set:
    pop cx
    mov si, .ddr_namebuf
    push word [caller_ds]
    mov word [caller_ds], 0x1000
    call gfx_draw_string_stub
    pop word [caller_ds]
    pop si
plus local: .ddr_namebuf: times 7 db 0
(All 8086-safe: lodsb/loop/test, no 386 ops. DI is already pushed/popped by the surrounding code in both sites — verify register save sets, add push di/pop di if needed at the kernel site since DI is live as the icon-entry walker there: wrap the block in push di / pop di.)

3) Hardening (latent third-party-BIN issue): force a NUL after each raw 12-byte name copy.
   - apps/launcher.asm after line 682 (loop .rbh_copy_name): DI points one past the 12-byte field, so add:
       mov byte [cs:di-1], 0
   - kernel/kernel.asm after line 15398 (rep movsb in desktop_set_icon_stub): ES:DI is one past the name, so add:
       mov byte [es:di-1], 0
   Both are no-ops for conforming headers (byte 11 is already 0) and truncate a 12-char no-NUL name to 11 chars.

Alternative (nicer-looking, larger change): draw labels in font 0 (4x6, advance 6: 11 chars = 66px < 80px). This is only cleanly doable kernel-side (save [current_font], call gfx_set_font with AL=0, draw, restore) because apps cannot query the current font to restore it; the launcher path would still need the kernel to own label drawing or a new get-font API.

## [high/high] Desktop icon labels overlap: 11-char labels are 88px wide drawn left-shifted in 80px grid columns
- src:window-manager | C:\Users\arin\Documents\Github\unodos\apps\launcher.asm:apps/launcher.asm:1053-1068 (label draw, x-8 unconditional); kernel/kernel.asm:15874-15890 (kernel repaint label, x-8/x-16 mismatch), 15786-15794 + 15912-15915 (bbox under-coverage), 19963+19973-19974 (root-cause amplifier: font 1 advance=12px/char, not 8) | area:launcher / desktop icon layout
DESC: draw_single_icon draws the name at 'mov bx, [cs:.dsi_x] / sub bx, 8' (lines 1054-1055) with COL_WIDTH_LO equ 80 (line 60) and ICON_X_OFFSET_LO equ 32. App names from BIN headers are up to 11 chars (e.g. 'OutLast VGA' = 11 chars in apps/outlastv.asm line 12, 'Tetris VGA'/'PacMan VGA' = 10). At 8px/char an 11-char label spans [icon_x-8, icon_x+80] while the next column's label starts at icon_x+72 — an 8px overlap; 10-char labels butt flush against the neighbor. This is the confirmed at-boot symptom 'desktop icon labels overlap each other'. The kernel-side repaint (kernel.asm draw_desktop_region lines 15875-15882) has the same geometry, plus two inconsistencies: in hi-res the kernel draws labels at x-16 while the launcher draws at x-8 (ghost/doubled text after kernel repaints), and the kernel's repaint bounding box .icon_bbox_w=52 (kernel.asm 15786) is smaller than the real 80px right extent of long labels, so partial repaints leave stale label fragments.
VERIFIER: The core finding is REAL and the at-boot overlap symptom is fully explained, but the auditor's arithmetic understates it. The label draw position is exactly as claimed: apps/launcher.asm:1054-1055 draws each name at icon_x-8 with icons on an 80px grid (COL_WIDTH_LO=80, ICON_X_OFFSET_LO=32, launcher.asm:57-66; get_icon_position launcher.asm:1540-1554). However, characters are NOT 8px wide: API 4 -> gfx_draw_string_stub -> draw_char advances draw_font_advance per character and paints the inter-char gap with background pixels (kernel/kernel.asm:3061-3092), and the default font (index 1, 8x8) has advance=12 (font_table kernel.asm:19973-19974 'db 8,8,12,8'; default draw_font_advance=12 at 19963). The launcher never changes font and tasks inherit current_font=1 (kernel.asm:14920-14921). So an 11-char name like 'OutLast VGA' (apps/outlastv.asm:12, stored in a 12-byte field, launcher.asm:668-682) paints 132px, not 88px: at boot in 320x200 (video_mode 0x04 default, kernel.asm:19986) labels start at x=24,104,184,264 and a long label overpaints its right neighbor's label area by ~40-52px (any name >=7 chars collides, >=8 chars overlaps visible glyphs) — each later-drawn label's solid character cells erase the previous label's tail. No mechanism prevents this: clipping (clip_enabled) is only active inside window draw contexts, plot_pixel clamps at screen edges (no corruption, purely visual), and there is no truncation/centering anywhere. Both secondary kernel claims also verify: (1) draw_desktop_region draws labels at x-8 lo-res but x-16 hi-res (kernel.asm:15876 plus the second 'sub bx,8' at 15882) while the launcher uses x-8 unconditionally in both modes -> 8px-shifted ghost/seamed text after hi-res partial repaints; (2) .icon_bbox_w=52 lo-res / 84 hi-res (kernel.asm:15786,15792; statics 15912-15915) is far smaller than the true painted right extent (x+124 lo-res, x+116 hi-res at advance 12), so the overlap test at 15817-15820 skips icons whose label tail alone intersects the redraw rect — the background fill (15772-15777) erases the tail and it is never redrawn, leaving chopped labels after window moves. The auditor's claimed line numbers were accurate (launcher 1053-1057, kernel 15875-15882, 15786); only the magnitude (8px overlap from 8px/char) and therefore the suggested fix math were wrong — 'min(strlen,10)*8' centering and 'widen COL_WIDTH to 96' both fail because 11 chars at the real 12px advance is 132px > 96. The only non-truncating fix that fits an 80px cell is drawing desktop labels with font 0 (4x6, advance 6: 11 chars = 66px), centered, identically in launcher and kernel.
FIX: Draw desktop icon labels with font 0 (4x6, 6px advance; 11 chars = 66px <= 80px cell), centered on the icon, identically in launcher and kernel; save/restore the current font around the draw (same pattern the kernel titlebar code uses at kernel.asm:17508-17527).

1) apps/launcher.asm — add API equates near line 16-40:
API_GFX_TEXT_WIDTH      equ 33
API_GFX_SET_FONT        equ 48
API_GFX_GET_FONT_INFO   equ 93

2) apps/launcher.asm — replace lines 1053-1068 (the '; Draw name label below icon' block in draw_single_icon) with:
    ; Draw name label below icon — small font, centered in 80px cell
    mov al, [cs:.dsi_slot]
    xor ah, ah
    mov cl, 12
    mul cl
    add ax, icon_names
    mov si, ax                      ; SI = name string
    mov ah, API_GFX_GET_FONT_INFO   ; AL = current font index
    int 0x80
    mov [cs:.dsi_font], al
    mov al, 0                       ; Font 0: 4x6, 6px advance (11 chars = 66px)
    mov ah, API_GFX_SET_FONT
    int 0x80
    mov ah, API_GFX_TEXT_WIDTH      ; DX = label width in pixels
    int 0x80
    mov bx, [cs:.dsi_x]
    add bx, 8                       ; Lo-res: 16px icon center
    cmp word [cs:scr_width], 640
    jb .dsi_lbl_x
    add bx, 8                       ; Hi-res: 32px icon center (+16)
.dsi_lbl_x:
    shr dx, 1
    sub bx, dx                      ; Center label on icon
    mov cx, [cs:.dsi_y]
    add cx, [cs:label_y_gap]
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov al, [cs:.dsi_font]          ; Restore previous font
    mov ah, API_GFX_SET_FONT
    int 0x80
and add next to .dsi_slot/.dsi_x/.dsi_y (line ~1081):
.dsi_font: db 0

3) kernel/kernel.asm — replace lines 15874-15890 (label block in draw_desktop_region) with the identical formula:
    ; Draw icon name label below the icon (small font, centered — matches launcher)
    mov al, [current_font]
    mov [.saved_font], al
    xor al, al
    call gfx_set_font               ; Font 0: 4x6, advance 6
    mov bx, [si + DESKTOP_ICON_OFF_X]
    add bx, 8                       ; Lo-res icon center
    mov cx, [si + DESKTOP_ICON_OFF_Y]
    add cx, 20                      ; 16px icon + 4px gap
    cmp word [screen_width], 640
    jb .ddr_label_pos_ok
    add bx, 8                       ; Hi-res: 32px icon center
    add cx, 20                      ; Hi-res: +40 below icon
.ddr_label_pos_ok:
    push si
    add si, DESKTOP_ICON_OFF_NAME
    push word [caller_ds]
    mov word [caller_ds], 0x1000
    call gfx_text_width             ; DX = label pixel width
    shr dx, 1
    sub bx, dx                      ; Center label on icon
    call gfx_draw_string_stub
    pop word [caller_ds]
    pop si
    mov al, [.saved_font]
    call gfx_set_font               ; Restore font
and add next to .label_shift (line ~15915):
.saved_font: db 0

4) kernel/kernel.asm line 15788 — widen lo-res left coverage for the new centered extent (worst case label left = icon_x-25):
    mov word [.label_shift], 26     ; was 12
(hi-res .label_shift=24 already covers the new worst-case x-17; .icon_bbox_w 52/84 now covers the shrunken right extents x+41/x+49, so no other bbox change needed).

Verification: boot lo-res — 'OutLast VGA' label now spans [icon_x-25, icon_x+41], fully inside its 80px column ([icon_x-32, icon_x+48)); adjacent labels have >=14px clearance; kernel partial repaints reproduce the exact launcher geometry in both resolutions. Do NOT use the original finding's 8px/char centering or COL_WIDTH=96 — at the real 12px advance both still overlap.

## [high/high] Icon labels drawn with no truncation or centering to the grid cell width (launcher and kernel desktop repaint)
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\apps\launcher.asm:apps/launcher.asm:1053-1068 (label draw in draw_single_icon); kernel/kernel.asm:15884-15900 (label draw in draw_desktop_region; bbox constants at 15795-15804 and 15922-15925 also affected); font advance source: kernel/kernel.asm:19973, 19983-19986 | area:launcher icon labels / kernel draw_desktop_region
DESC: draw_single_icon draws the full 11-char name at a fixed offset: 'mov bx,[cs:.dsi_x] / sub bx,8 / ... / mov ah, API_GFX_DRAW_STRING / int 0x80' — no measurement against the 80px column, no truncation, no centering. The kernel-side repaint duplicates this (kernel.asm draw_desktop_region lines 15874-15889: label at icon_x-8, full string via gfx_draw_string_stub with clip disabled). Names come from the BIN header (up to 11 chars, launcher.asm 668-682), so even with a corrected 8px font advance an 11-char name spans 88 px starting at col*80+24, reaching col*80+112 where the next column's icon begins. Combined with the advance=12 font bug this is the confirmed boot symptom; independently it still overlaps for max-length names.
VERIFIER: Confirmed by direct code reading, with refined line numbers and one nuance. (1) apps/launcher.asm:1053-1068 draws the full null-terminated icon name at icon_x-8 with no truncation, measurement, or centering, exactly as claimed; names are copied verbatim (12 bytes, up to 11 chars) from the BIN header at launcher.asm:668-682, and shipping apps have 10-11 char names ("OutLast VGA", "PacMan VGA", "Tetris VGA"). (2) The kernel duplicate is real but the label block is kernel/kernel.asm:15884-15900, not 15874-15889 (those earlier lines are the icon-bitmap draw); it draws the stored 12-byte name via gfx_draw_string_stub at icon_x-8 (lo-res) / icon_x-16 (hi-res; NB: disagrees with the launcher's icon_x-8, leaving an 8px label ghost on hi-res repaints). (3) No mechanism prevents the overflow: clip_enabled defaults to 0 (kernel:19989), is only enabled by win_begin_draw (kernel:1396) for window content, and draw_desktop_region/launcher fullscreen drawing run unclipped; cga_pixel_calc has no X bounds check, so last-column labels at 320x200 (label end x=396>319) wrap pixels into adjacent scanline bytes (visual garbage only; stays inside the 16K CGA buffer, no memory corruption). (4) Geometry verified and currently WORSE than the finding states: the effective advance today is 12 (font_table entry for default font 1 at kernel:19984 is height=8,width=8,advance=12; launcher never sets a font), so an 11-char label spans 132px in the 80px cell (col*80+24..col*80+155), overlapping the next column's label (+104) and icon bitmap (+112); even 8-char "Settings" (96px) overlaps the next icon. With the companion advance fix (12->8), the finding's residual claim holds with one nuance: an 11-char label ends at col*80+111 inclusive, touching but not overwriting the next icon bitmap at +112, while still overwriting the next label's first character cell (starts +104). (5) Secondary confirmed effect: draw_desktop_region's dirty-rect bbox constants (.icon_bbox_w=52/84, .label_shift=12/24, kernel:15795-15804 and 15922-15925) underestimate the true label extent, so partial repaints can leave stale label fragments and spill label pixels over content outside the damaged rect. Severity high is fair for a stock-image, every-boot visible corruption (with current advance=12); it is purely a rendering bug, not memory-unsafe.
FIX: Clamp the label to the 80px cell and center it under the icon, in both draw sites. Kernel side (replace the label block at kernel/kernel.asm:15884-15900; SI still points at the desktop_icons entry):

    ; max_chars = (COL_W-4)/advance ; len = min(strlen(name), max_chars, 11)
    mov ax, 76
    div byte [draw_font_advance]    ; AL = max chars that fit cell
    mov dh, al
    push si
    push di
    lea di, [si + DESKTOP_ICON_OFF_NAME]
    xor cx, cx
.ddr_lbl_len:
    cmp cl, 11
    jae .ddr_lbl_copy
    cmp cl, dh
    jae .ddr_lbl_copy
    cmp byte [di], 0
    je .ddr_lbl_copy
    inc di
    inc cx
    jmp .ddr_lbl_len
.ddr_lbl_copy:                      ; copy CX chars + NUL to scratch
    lea di, [si + DESKTOP_ICON_OFF_NAME]
    mov bx, .ddr_lbl_buf
    push cx
    jcxz .ddr_lbl_term
.ddr_lbl_cp1:
    mov al, [di]
    mov [bx], al
    inc di
    inc bx
    loop .ddr_lbl_cp1
.ddr_lbl_term:
    mov byte [bx], 0
    pop cx
    ; label_x = icon_x + icon_size/2 - len*advance/2, clamped to >= 0
    mov al, [draw_font_advance]
    xor ah, ah
    mul cx                          ; AX = text width (len*advance)
    shr ax, 1
    mov bx, [si + DESKTOP_ICON_OFF_X]
    mov dx, [.icon_size]
    shr dx, 1
    add bx, dx
    sub bx, ax                      ; BX = centered label X
    jns .ddr_lbl_x_ok
    xor bx, bx
.ddr_lbl_x_ok:
    pop di
    ; CX(Y) computed as before: icon_y + 20 (lo-res) / +40 (hi-res)
    mov cx, [si + DESKTOP_ICON_OFF_Y]
    add cx, 20
    cmp word [screen_width], 640
    jb .ddr_lbl_y_ok2
    add cx, 20
.ddr_lbl_y_ok2:
    push si
    mov si, .ddr_lbl_buf
    push word [caller_ds]
    mov word [caller_ds], 0x1000
    call gfx_draw_string_stub
    pop word [caller_ds]
    pop si
    pop si
    ; (keep the existing pop bp / pop si that follow)

and add near the other locals: .ddr_lbl_buf: times 12 db 0
Also widen the repaint bbox to cover centered labels: change .label_shift lo-res 12 -> 32 (kernel:15798/15925) since a centered <=76px label can start up to 30px left of icon_x.

Launcher side (replace apps/launcher.asm:1053-1068 with the same pattern): fetch CL=advance once via int 0x80 AH=API_GFX_GET_FONT_METRICS(49)/AL=1 (the launcher's font), compute max_chars=(col_width-4)/advance, copy at most that many chars of icon_names+slot*12 into a 12-byte scratch with NUL, then BX = .dsi_x + (icon visual size/2: 8 lo-res, 16 hi-res) - (len*advance)/2 (clamp at 0), CX = .dsi_y + label_y_gap, SI = scratch, int 0x80 AH=API_GFX_DRAW_STRING. While editing, also delete the dead CX computation at launcher.asm:1056-1057 (clobbered by mul cl) and unify the hi-res label X so kernel and launcher use the same formula (the centering formula achieves this automatically). Note: this fix is independent of, and complementary to, the separate advance=12->8 font-table fix (kernel:19984); with advance 12 it truncates to 6 chars, with advance 8 to 9 chars.

## [high/high] Desktop icon labels overlap: 88px labels drawn into 80px grid columns with no truncation (confirmed boot symptom)
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\apps\launcher.asm:apps/launcher.asm 1053-1068 (draw_single_icon label draw, x-8, no truncation); kernel/kernel.asm 15874-15890 (draw_desktop_region label draw, same plus extra hi-res 'sub bx,8' at 15882); root metric: kernel/kernel.asm 19963 and 19973-19974 (font 1 advance = 12px, so 11-char labels are 132px in 80px columns; overlap threshold is 7 chars, not 11) | area:kernel data structures / desktop icons
DESC: draw_single_icon draws the name at 'mov bx, [cs:.dsi_x] / sub bx, 8' (1054-1055) with API_GFX_DRAW_STRING, which renders until NUL with no width limit. Grid columns are 80px in both modes (COL_WIDTH_LO/COL_WIDTH_HI equ 80, launcher.asm lines 60 and 72). App names are up to 11 characters ('OutLast VGA' in outlastv.asm line 12, 'PacMan VGA' in pacmanv.asm line 12), i.e. 11 x 8px = 88px wide, starting 8px left of the icon — the label spans from icon_x-8 to icon_x+80 while the next column's label begins at icon_x+72, so adjacent long names overlap by 8+ pixels. The kernel's desktop repaint path has the identical layout (kernel/kernel.asm 15874-15888: 'sub bx, 8 ... call gfx_draw_string_stub', and a further 'sub bx, 8' in hi-res at 15882, making it worse there). This exactly reproduces the confirmed 'desktop icon labels overlap each other' at boot.
VERIFIER: Confirmed, and the bug is worse than the finding states. Both cited draw sites render icon name labels with gfx_draw_string_stub (kernel.asm 7523-7592), which loops until NUL with no width limit; its only clipping is the clip_enabled rect (line 7546), which is 0 on the desktop path (tasks start with clip_enabled=0, kernel.asm 1797; it is only enabled inside win_begin_draw/widget helpers and clips to windows, not grid columns). No other mechanism truncates: read_bin_header copies the full 12-byte header name (launcher.asm 668-682) and register_icon forwards it verbatim to the kernel (launcher.asm 768-789). The finding's arithmetic, however, assumes an 8px character advance. The default font is font 1 (8x8 glyph) with advance = 12px (draw_font_advance: db 12 at kernel.asm 19963; font_table entry 'db 8, 8, 12, 8' at 19974), draw_char advances draw_x by draw_font_advance (kernel.asm 3091-3092), and the launcher never changes fonts. So an 11-char name like 'OutLast VGA' is 132px (not 88px) in an 80px column (COL_WIDTH_LO/HI equ 80, launcher.asm 60/72). Lo-res: label x = col*80+32-8 = col*80+24, span ends at col*80+156; the next column's label starts at col*80+104, giving a 52px (4+ char) overlap. Any name of 7+ chars (7*12=84 > 80) overlaps, including 8-char FAT-derived default names (96px). Same column pitch and overlap in hi-res. The kernel desktop-repaint path (kernel.asm 15874-15890) has the identical unbounded draw, plus an extra 'sub bx, 8' in hi-res (15882) that the launcher lacks, so kernel partial repaints also draw hi-res labels 8px left of launcher-drawn ones (label ghosting). This exactly matches the confirmed boot symptom. The suggested fix's max_chars=9 is wrong: at 12px advance only 6 characters fit in 80px; truncating to 9 chars would still overlap by 28px. Secondary consequence verified but not claimed: the dirty-rect bbox used to decide repaint (icon_bbox_w = 52/84, kernel.asm 15786/15792) assumes a much narrower label, so overflowing label tails can also be left erased after window moves.
FIX: Truncate labels to 6 chars (6 x 12px advance = 72px <= 80px column) at both draw sites. 8086-safe.

1) apps/launcher.asm, in draw_single_icon — replace lines 1058-1068:

    ; Point SI to name, truncated to 6 chars (80px col / 12px advance)
    mov al, [cs:.dsi_slot]
    xor ah, ah
    mov cl, 12
    mul cl
    add ax, icon_names
    mov si, ax
    mov di, .dsi_namebuf
    mov cx, 6
.dsi_trunc:
    mov al, [cs:si]
    mov [cs:di], al
    test al, al
    jz .dsi_trunc_z
    inc si
    inc di
    loop .dsi_trunc
    mov byte [cs:di], 0
.dsi_trunc_z:
    mov si, .dsi_namebuf
    mov cx, [cs:.dsi_y]
    add cx, [cs:label_y_gap]
    mov ah, API_GFX_DRAW_STRING
    int 0x80

and next to .dsi_x/.dsi_y locals (after line 1083) add:
.dsi_namebuf: times 7 db 0

(BX still holds .dsi_x-8 from lines 1054-1055; CX is reloaded after MUL exactly as the existing code already does.)

2) kernel/kernel.asm, in draw_desktop_region — replace lines 15884-15890:

    push si
    push di
    push cx
    add si, DESKTOP_ICON_OFF_NAME
    mov di, .ddr_namebuf
    mov cx, 6
.ddr_trunc:
    mov al, [si]
    mov [di], al
    test al, al
    jz .ddr_trunc_z
    inc si
    inc di
    loop .ddr_trunc
    mov byte [di], 0
.ddr_trunc_z:
    pop cx
    pop di
    mov si, .ddr_namebuf
    push word [caller_ds]
    mov word [caller_ds], 0x1000
    call gfx_draw_string_stub
    pop word [caller_ds]
    pop si

and next to the .icon_size/.label_shift locals (after line 15915) add:
.ddr_namebuf: times 7 db 0

Optionally also delete the hi-res-only 'sub bx, 8' at kernel.asm 15882 so kernel repaints place labels at the same X as the launcher (fixes hi-res label ghosting between the two paths). Do NOT change font 1's advance (kernel.asm 19974) as a fix — it would alter text metrics OS-wide.

## [high/high] apps/mkboot.asm: 11 non-8086 sites including dword stores
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\mkboot.asm:apps/mkboot.asm:932,933,945,946,947,950,951,965,966 (386+ dword stores, real); lines 104/587 pusha/popa are the mandated app ABI shared with every app and the kernel yield mechanism, not an mkboot-specific defect | area:8088 compatibility
DESC: 11 CPU-8086 errors: pusha/popa x2 (lines 104/587) and 9 dword immediate stores used to build the boot sector image, e.g. line 932 'mov dword [cs:secbuf + 3], 'UNOD'' and line 945 'mov dword [cs:secbuf + 39], 0x12345678'. The disk-imaging tool fails to run on the 8088 target.
VERIFIER: Instruction-level facts CONFIRMED, but severity and framing need correction. Verified in C:\Users\arin\Documents\Github\unodos\apps\mkboot.asm: pusha (line 104), popa (line 587), and exactly 9 'mov dword [cs:secbuf+N], imm32' stores at lines 932, 933, 945, 946, 947, 950, 951 (build_fs_bpb) and 965, 966 (build_rootdir) = 11 sites, matching the finding. build_fs_bpb and build_rootdir are called unconditionally on every floppy write (lines 327 and 396, both Full and Barebones paths), so there is no guard preventing execution. The dword stores require the 0x66 operand-size prefix, which is 386+: on 80286 it raises #UD (int 6), and on 8086/8088 byte 0x66 decodes as an alias of JBE rel8, derailing execution mid-routine. So those 9 sites actually break the README's RECOMMENDED 80286 tier too, which is worse than the finding claims. HOWEVER, the claimed impact ('the disk-imaging tool fails to run on the 8088 target', severity high) is misleading for three reasons: (1) the OS never reaches mkboot on an 8088 — boot/stage2.asm:64 (the floppy boot loader) executes pusha, and the kernel uses pusha/popa in 20+ places including the core task-switch ABI (kernel/kernel.asm:15116 'popa ; Consume pusha frame from yielded task', 14981 dummy pusha frame); (2) mkboot's entry pusha / exit popa is the documented mandatory app ABI (docs/APP_DEVELOPMENT.md app template; every app in apps/ does the same), so flagging those 2 sites as an mkboot defect is wrong, and the suggested 'shared macro' does not exist — there is no %macro anywhere in the repository; (3) the kernel's own floppy-reachable FAT12 file-create path already uses mov dword (kernel/kernel.asm:16953-16954) plus movzx/shl r,imm elsewhere, so the shipped OS has a de facto 386 floor regardless of mkboot. Net: the 9 dword stores are real, trivially fixable 8086-incompatibilities worth cleaning up as part of any codebase-wide 8086/8088 retrofit (or to honor the 286 'recommended' tier), but mkboot is not the gating factor for 8088 support and the pusha/popa pair must NOT be changed in isolation (kernel pops the app's pusha frame on yield). Effective severity: low (consistency/cleanup) unless a whole-codebase 8086 compatibility effort is undertaken; the README's 'Intel 8088 or later' claim is inaccurate codebase-wide, which is the larger finding.
FIX: Replace each 'mov dword' with two little-endian word stores (NASM stores string constants low-byte-first, so 'UNOD' splits to 'UN'+'OD'). In build_fs_bpb (apps/mkboot.asm lines 932-951):
    mov word [cs:secbuf + 3], 'UN'
    mov word [cs:secbuf + 5], 'OD'
    mov word [cs:secbuf + 7], 'OS'
    mov word [cs:secbuf + 9], '  '
    ...
    mov word [cs:secbuf + 39], 0x5678
    mov word [cs:secbuf + 41], 0x1234
    mov word [cs:secbuf + 43], 'UN'
    mov word [cs:secbuf + 45], 'OD'
    mov word [cs:secbuf + 47], 'OS'
    mov word [cs:secbuf + 49], '  '
    ...
    mov word [cs:secbuf + 54], 'FA'
    mov word [cs:secbuf + 56], 'T1'
    mov word [cs:secbuf + 58], '2 '
    mov word [cs:secbuf + 60], '  '
In build_rootdir (lines 965-966):
    mov word [cs:secbuf + 0], 'UN'
    mov word [cs:secbuf + 2], 'OD'
    mov word [cs:secbuf + 4], 'OS'
    mov word [cs:secbuf + 6], '  '
(Note: line 949 'mov byte [cs:secbuf + 53], ...' overlap is unaffected; the +49/'  ' word plus existing word at +51 and byte at +53 still produce the same 11-space volume-label field.) Leave pusha/popa at lines 104/587 unchanged: the kernel's yield/launch machinery consumes the app's pusha frame (kernel/kernel.asm:15116), every app uses this ABI, and no shared macro exists. If true 8086 support is ever pursued, pusha/popa must be replaced codebase-wide (stage2, kernel, all 14 apps) with a layout-compatible push ax/cx/dx/bx/sp/bp/si/di sequence, and add 'cpu 8086' directives to make NASM enforce it.

## [high/high] apps/mouse_test.asm: 4 non-8086 sites (pusha/popa only)
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\mouse_test.asm:Lines as cited are correct: 60 (pusha, entry), 320 (popa, .exit), 327 (pusha, draw_labels), 341 (popa, draw_labels) | area:8088 compatibility
DESC: 4 CPU-8086 errors: pusha at 60/327, popa at 320/341. Otherwise clean.
VERIFIER: Confirmed with refinements. All four cited sites are real and exactly where claimed in C:\Users\arin\Documents\Github\unodos\apps\mouse_test.asm: pusha at lines 60 (app entry) and 327 (draw_labels), popa at lines 320 (app exit, before retf) and 341 (draw_labels return). PUSHA/POPA (opcodes 60h/61h) are 80186+ instructions; on a real 8088/8086 — the project's documented minimum CPU (README.md:201, docs/FEATURES.md:7, CHANGELOG.md:1255) — opcode 60h decodes as an alias of JO rel8, consuming the next byte (1Eh, the `push ds` at line 61) as displacement. Failure path: if OF happens to be set at entry, IP jumps +0x1E into the middle of unrelated code; if clear, `push ds` is silently skipped, so the pops at lines 318-319 restore ES/DS from wrong stack slots and the retf at 321 returns to a garbage address. Either way the app crashes/corrupts state on 8088. The claim 'otherwise clean' is also verified: every other instruction in the file is 8086-valid (no push imm, no shift-by-immediate>1, no movzx). No existing mechanism prevents this — there is no CPU directive anywhere in the repo and no build-time check. Two corrections to the finding: (1) the suggested PUSHA86/POPA86 macro does not exist anywhere in the repo and apps share no include file, so the fix must define the macros locally; (2) severity 'high' is overstated for this file in isolation: pusha/popa occur ~512 times across 20 files including kernel/kernel.asm (39 sites) and boot/stage2.asm (2 sites), and docs/bootloader-architecture.md:221 states boot currently requires 386+, so an 8088 never reaches this app — the fix is only meaningful as part of the codebase-wide 8086 sweep. Minor side note: the popa at line 320 clobbers the AX exit code set at lines 311/315 (pre-existing, independent of this finding; the macro replacement preserves that behavior).
FIX: In apps/mouse_test.asm: after line 6 `[BITS 16]` add `CPU 8086` and the macro definitions, then replace the four sites.

Add near top of file (after [BITS 16]):

CPU 8086

%macro PUSHA86 0
    push ax
    push cx
    push dx
    push bx
    push bp
    push si
    push di
%endmacro

%macro POPA86 0
    pop di
    pop si
    pop bp
    pop bx
    pop dx
    pop cx
    pop ax
%endmacro

Then:
- line 60: `pusha` -> `PUSHA86`
- line 320: `popa` -> `POPA86`
- line 327: `pusha` -> `PUSHA86`
- line 341: `popa` -> `POPA86`

Notes: omitting the SP slot is safe — real POPA discards it, and nothing in this file reads the saved-SP stack slot. `CPU 8086` makes NASM reject any future 186+ instruction at assembly time. Caution: do NOT place the macro block before the icon header in a way that emits bytes — %macro definitions emit nothing, but CPU 8086 must precede line 10's header bytes only as a directive (it emits nothing either, so placement after [BITS 16] is safe). Ideally hoist PUSHA86/POPA86 into a shared include (e.g. apps/compat.inc) since kernel and all other apps need the same treatment (~512 sites repo-wide).

## [high/high] apps/music.asm: 17 non-8086 sites
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\music.asm:pusha: 127, 451, 487, 530, 563, 615; popa: 259, 480, 525, 558, 608, 728; shl-imm: 270, 286, 675; movzx: 285, 699 (17 total — finding's count is exact) | area:8088 compatibility
DESC: 17 CPU-8086 errors: pusha x6 / popa x6 (line 127), shl-imm x3 (line 270 'shl ax, 2 ; 4 bytes per entry'), movzx x2.
VERIFIER: The instruction inventory is exactly correct, but the claimed severity is wrong; this should be downgraded to low/informational. VERIFIED FACTS: apps/music.asm contains exactly 17 non-8086 instructions, matching the finding's count and categories: pusha at lines 127, 451, 487, 530, 563, 615 (6); popa at lines 259, 480, 525, 558, 608, 728 (6); shl-with-immediate>1 at lines 270 (shl ax,2), 286 (shl ax,3), 675 (shl ax,2) (3); movzx at lines 285 (movzx ax, byte [cs:cur_song]) and 699 (movzx cx, al) (2). No other 186+/386+ instructions exist in the file (no push-imm, enter/leave, 32-bit regs). There is no CPU directive anywhere in the repo, so NASM silently accepts these. On a real 8088/8086 these are not illegal-instruction faults but silent misdecodes: 0x60 (pusha) executes as JO rel8, so the app corrupts control flow at its very first instruction (entry, line 127); 0xC1 (shl r,imm8) aliases RET; 0x0F (movzx prefix) executes as POP CS. README.md does claim 8088 as the minimum CPU ("just BIOS services and an Intel 8088 or later processor"; "Minimum: Intel 8088 @ 4.77 MHz"). WHY SEVERITY IS WRONG: the failure path is unreachable on the claimed hardware. The boot chain itself is not 8086-clean: boot/mbr.asm lines 114/120 use movzx with 32-bit registers (386+), boot/stage2.asm uses pusha/popa, boot/stage2_hd.asm uses movzx eax/ebx throughout, and kernel/kernel.asm contains 119 such sites including 80 movzx/movsx. Codebase-wide there are 743 occurrences across 20 files. An 8086/8088 crashes in the MBR (POP CS at the first movzx) before the kernel, launcher, or music.bin can ever load. TODO.md line 31 explicitly lists "8086-compatible boot for HP 200LX, Sharp PC-3100" as open roadmap work, confirming 8086 support is a known unmet goal, not a regression introduced by this app. Fixing music.asm in isolation produces zero observable behavior change on any machine that can boot this OS today (de facto minimum is 386 due to 32-bit register usage in the MBR/stage2/kernel). The finding is valid only as one work item inside a codebase-wide 8086-compatibility effort; the README hardware claim is the actual defect today. The suggested remedy (mechanical replacement + CPU 8086 directive) is correct in form, and adding "CPU 8086" after [BITS 16] would make NASM enforce it at build time.
FIX: Only meaningful as part of the codebase-wide 8086 effort (TODO.md:31) — the MBR/stage2/kernel must be converted first or music.bin can never run on an 8086. Mechanical 8086-safe conversion for apps/music.asm: (1) after line 6 "[BITS 16]" add "CPU 8086" to enforce at assembly time. (2) Define macros and replace all 6 pusha / 6 popa:
%macro PUSHA86 0
    push ax
    push cx
    push dx
    push bx
    push sp
    push bp
    push si
    push di
%endmacro
%macro POPA86 0
    pop di
    pop si
    pop bp
    add sp, 2        ; discard saved SP
    pop bx
    pop dx
    pop cx
    pop ax
%endmacro
(3) line 270 and line 675: replace "shl ax, 2" with "shl ax, 1" + "shl ax, 1" (2 lines each; CX not yet live at either site). (4) line 286: replace "shl ax, 3" with "shl ax, 1" x3 (avoids clobbering CL via mov cl,3/shl ax,cl). (5) line 285: replace "movzx ax, byte [cs:cur_song]" with "mov al, [cs:cur_song]" + "mov ah, 0" (AX is saved/restored by the function). (6) line 699: replace "movzx cx, al" with "mov cl, al" + "mov ch, 0". Note PUSHA86's "push sp" pushes the post-decrement value on 8086 (differs from 286+), which is harmless here because POPA86 discards it; nothing in music.asm references the saved frame.

## [high/high] apps/notepad.asm: 88 non-8086 sites
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\notepad.asm:mov bl, [cs:mount_handle] | xor bh, bh   ; line 337 replacement: BL = mount handle, BH = 0 | area:8088 compatibility
DESC: 88 CPU-8086 errors: popa x35 / pusha x30 (line 130), movzx x23 (line 337 'movzx bx, byte [cs:mount_handle]'). All 16-bit forms.
VERIFIER: Counts verified exactly against C:\Users\arin\Documents\Github\unodos\apps\notepad.asm: pusha x30, popa x35 (first at line 130, last at line 2523), movzx x23 (line 337 is verbatim 'movzx bx, byte [cs:mount_handle]') = 88 sites. The documented minimum CPU is Intel 8088 (README.md Target Hardware table, docs/FEATURES.md), and no NASM 'cpu 8086' directive exists anywhere in the repo, so the assembler never flags these. On a real 8088/8086 these are not faulting invalid opcodes but silent misexecution: opcodes 60h/61h (pusha/popa) decode as aliases of JO/JNO (60-6F mirror 70-7F on pre-186 CPUs), and movzx's 0Fh prefix decodes as POP CS — both catastrophic. CAVEATS that refine the finding: (1) Severity context — this is not notepad-specific. kernel.asm alone has 39 pusha/popa + 80 movzx; nearly every app plus boot/mbr.asm and boot/stage2_hd.asm use them. On a real 8088 the kernel crashes long before notepad runs, so fixing notepad alone has zero observable effect; it only matters as part of a codebase-wide 8086 pass (tested hardware listed is 386SX/486 — this has never run on an 8088). (2) The suggested mechanical fix is mostly right but incomplete: movzx preserves flags while the mov/xor pair clobbers them — I checked all 23 sites and none rely on flags surviving the movzx (each is followed by mul/div/int/mov), so it happens to be safe here, but it is not universally mechanical. (3) Three of the 23 movzx target SI/DI (lines 996, 1947, 2285), which have no 8-bit halves, so 'mov sl' does not exist; these need a bounce through AX. I verified AX is dead at all three (at 996 the API_GFX_CLEAR_AREA contract is BX=X, CX=Y, DX=W, SI=H per kernel.asm:8356/9630 — AL unused; at 1947 and 2285 AX is loaded after the movzx). (4) The pusha/popa macro replacement is safe in this file: no 'mov bp,sp', no [bp+n] frame access, and no SP arithmetic anywhere in notepad.asm, so a 7-register macro omitting SP is equivalent (note 'push sp' itself differs on 8086 — omit SP from the macro).
FIX: In apps/notepad.asm: (a) add at top of file:
    cpu 8086              ; NASM now rejects any non-8086 instruction at build time
    %macro PUSHA86 0
        push ax
        push cx
        push dx
        push bx
        push bp
        push si
        push di
    %endmacro
    %macro POPA86 0
        pop di
        pop si
        pop bp
        pop bx
        pop dx
        pop cx
        pop ax
    %endmacro
(b) replace all 30 'pusha' with PUSHA86 and all 35 'popa' with POPA86 (verified safe: no pusha-frame/SP-relative access in the file). (c) For the 20 movzx sites targeting AX/BX/CX/DX (e.g. line 337):
    mov bl, [cs:mount_handle]
    xor bh, bh
(flag clobber verified harmless at all 23 sites). (d) For the 3 SI/DI sites (lines 996, 1947, 2285), AX verified dead at each:
    mov al, [cs:row_h]    ; or [cs:input_len] at 2285
    xor ah, ah
    mov si, ax            ; mov di, ax at 2285
NOTE: applying this only to notepad.asm is pointless in isolation — kernel.asm (80 movzx, 39 pusha/popa), all other apps, and boot/mbr.asm + boot/stage2_hd.asm need the same treatment (plus 'cpu 8086') before the OS can run on a real 8088.

## [high/high] apps/outlast.asm: 44 non-8086 sites
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\outlast.asm:Line 39 (and 14 matching pairs): replace `pusha` with `push ax / push cx / push dx / push bx / push bp / push si / push di` and each `popa` with the reverse pop sequence. | area:8088 compatibility
DESC: 44 CPU-8086 errors: pusha/popa x30 (line 39), shr-imm x9 (line 263 'shr ax, 2'), sar-imm x2 (line 243 'sar ax, 3 ; Drift = curve / 8', line 543 'sar ax, 5'), movzx x2 (line 426 'movzx bx, byte [cs:game_song_idx]'), shl x1 (line 505 'shl ax, 8').
VERIFIER: Verified by direct enumeration of C:\Users\arin\Documents\Github\unodos\apps\outlast.asm (1953 lines). The count of 44 non-8086 sites is exactly right and exhaustive:

- pusha/popa x30 (15 pairs): lines 39/320, 390/436, 443/456, 463/686, 693/845, 852/922, 929/960, 967/1116, 1125/1186, 1193/1253, 1260/1322, 1329/1490, 1498/1578, 1601/1639, 1646/1686.
- shr reg,imm>1 x9: lines 263, 592, 655, 670, 802, 1046, 1095, 1102, 1159.
- sar reg,imm x2: lines 243 (sar ax,3), 543 (sar ax,5).
- movzx x2: lines 426 (movzx bx, byte [cs:game_song_idx]), 1512 (movzx ax, byte [cs:title_order+si]).
- shl reg,imm>1 x1: line 505 (shl ax,8).

I scanned for additional 186+/386 forms the finding might have missed (push imm, imul r,imm, enter/leave, bound, ins/outs, movsx) — none exist; all push matches are register pushes. The numerous shift-by-1 instructions (shr/shl ax,1 etc.) are 8086-safe (NASM emits the D1 short form). There is no `cpu 8086` directive anywhere in the repo, so NASM silently accepts these opcodes.

Why it matters: README.md and docs/FEATURES.md document the minimum CPU as "Intel 8088 @ 4.77 MHz", and outlast.asm is explicitly the CGA variant (header: "Pseudo-3D Racing Game for UnoDOS (CGA version)") — i.e., the build intended for exactly that class of hardware (outlastv.asm is the VGA variant for newer machines). On a real 8086/8088 these opcodes alias to other instructions (60h/61h pusha/popa execute as Jcc; C0h/C1h shift-imm execute as RETN imm16/RETN; 0Fh movzx prefix executes as POP CS), so the app crashes immediately — the very first instruction at entry (line 39 pusha) executes as JO. No gating mechanism exists: the launcher does no CPU detection, and nothing prevents these paths from running.

Two caveats that refine but do not refute the finding: (1) Systemic, not isolated — kernel/kernel.asm itself contains 39 pusha/popa lines, and TODO.md line 31 lists "8086-compatible boot" as an open item, so an 8088 machine never even reaches app launch today; fixing outlast.asm alone does not restore 8088 support. (2) The suggested perf advice is backwards: on 8086/8088, shift-by-1 costs 2 cycles while shift-by-CL costs 8+4n cycles plus 4 for MOV CL,imm — for every count used in this file (2-5), repeated shift-by-1 sequences are several times FASTER than CL-based shifts. CL-based shifts should NOT be preferred here.

Mechanical replacement is safe: no code addresses the pusha-saved frame (no `mov bp,sp` or `[bp+n]`/`[sp...]` anywhere in the file), so explicit push/pop sequences are drop-in equivalents.
FIX: Apply per-pattern in apps/outlast.asm (all 8086-safe, verified no SP/BP frame addressing so stack-layout change is safe):

1) pusha -> "push ax \n push cx \n push dx \n push bx \n push bp \n push si \n push di"; popa -> "pop di \n pop si \n pop bp \n pop bx \n pop dx \n pop cx \n pop ax" (15 pairs: 39/320, 390/436, 443/456, 463/686, 693/845, 852/922, 929/960, 967/1116, 1125/1186, 1193/1253, 1260/1322, 1329/1490, 1498/1578, 1601/1639, 1646/1686). pusha's extra SP push is discarded by popa, so the 7-register form is equivalent.

2) Shift-by-immediate (n=2..5) -> repeat the shift-by-1 form n times (FASTER than CL form on 8088: 2 cycles/shift vs 12+ for mov cl,n + shift cl):
   line 263: shr ax,2 -> shr ax,1 x2 (same at 1046, 1095, 1159; shr dx,2 at 802; shr si,2 at 1102)
   line 592: shr ax,3 -> shr ax,1 x3
   lines 655, 670: shr ax,4 -> shr ax,1 x4
   line 243: sar ax,3 -> sar ax,1 x3
   line 543: sar ax,5 -> sar ax,1 x5

3) line 505: "shl ax, 8" -> "mov ah, al \n xor al, al" (exact equivalent: AH=old AL, AL=0, old AH discarded in both).

4) line 426: "movzx bx, byte [cs:game_song_idx]" -> "mov bl, [cs:game_song_idx] \n mov bh, 0"
   line 1512: "movzx ax, byte [cs:title_order + si]" -> "mov al, [cs:title_order + si] \n mov ah, 0"
   (use MOV reg,0 rather than XOR to preserve flags, matching movzx's no-flags behavior.)

5) Prevent regressions: add "cpu 8086" near the top of apps/outlast.asm (after [ORG 0x0000]) so NASM rejects any future non-8086 opcode.

Note: this fix alone does not make UnoDOS 8088-bootable — kernel/kernel.asm has 39 pusha/popa sites of its own (tracked implicitly by TODO.md line 31 "8086-compatible boot"). Treat outlast.asm as one file in a project-wide 8086-cleanup pass.

## [high/high] apps/outlastv.asm: 54 non-8086 sites
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\outlastv.asm:apps/outlastv.asm: 53 non-8086 sites — pusha/popa x34 (lines 40-2265), shr-imm>1 x13 (183,194,300,316,323,952,1035,1050,1184,1434,1496,1503,1566), sar-imm x2 (280,890), shl-imm x1 (849), movzx x3 (729,777,2085); unreachable on real 8088 until kernel.asm (39 pusha/popa, ~205 more sites) is also fixed | area:8088 compatibility
DESC: 54 CPU-8086 errors: pusha/popa x34 (line 40), shr-imm x14 (line 183 'shr ax, 3 ; min = scr_w / 8', 194 'shr bx, 3'), movzx x3, sar-imm x2, shl x1.
VERIFIER: Verified by direct inspection of C:\Users\arin\Documents\Github\unodos\apps\outlastv.asm. The file contains 53 (not 54) instructions that do not exist on the 8086/8088, against the project's stated target (README.md line 9: "Intel 8088 or later"; minimum spec table line 201: "Intel 8088 @ 4.77 MHz"). Exact inventory:

1) pusha/popa x34 (17 pairs) at lines 40, 376, 480, 659, 674, 739, 746, 756, 763, 808, 815, 1066, 1073, 1227, 1234, 1304, 1311, 1344, 1351, 1517, 1526, 1597, 1604, 1733, 1740, 1802, 1809, 2063, 2071, 2152, 2179, 2220, 2227, 2265. On 8086/8088 opcode 60h decodes as an undocumented alias of 70h (JO rel8), so the entry-point pusha at line 40 executes as "JO" consuming the next byte (push ds = 1Eh) as a jump displacement — instant control-flow/stack corruption.
2) shr reg,imm>1 x13 (auditor said 14; one over-count): lines 183 (shr ax,3 "min = scr_w / 8" — comment matches exactly), 194, 300, 316, 323, 952, 1035, 1050, 1184, 1434, 1496, 1503, 1566. NASM emits C1 /5 ib; on 8086, C1h aliases C3h (near RET), so each of these executes as a premature return — catastrophic.
3) sar reg,imm>1 x2: lines 280, 890. Same C1-alias failure.
4) shl reg,imm>1 x1: line 849 (shl ax,8).
5) movzx x3: lines 729, 777, 2085. 0Fh on 8086 is POP CS — loads CS from the stack, catastrophic.

No mitigation exists: there is no "cpu 8086" directive anywhere in the repo (NASM silently emits 186/386 opcodes), and no CPU-detection/gating in the kernel or launcher (grep of kernel/ and apps/launcher.asm for cpu-detect/286/386 logic: zero hits), so nothing prevents launch on an 8088. The 29 shift-by-1 instructions in the file (e.g. lines 247, 1042, 2132) are 8086-legal (D1/D0 encodings) and were correctly excluded by the auditor.

Two refinements to the finding: (a) count is 53, not 54 (shr-imm is 13, not 14); (b) severity context — this bug is currently unreachable on the only hardware where it matters, because kernel/kernel.asm itself contains 39 pusha/popa and ~205 further non-8086 shift/movzx sites, so a real 8088 cannot boot the OS to launch any app. The same pattern also exists in apps/outlast.asm (the CGA sibling) and likely every other app. TODO.md line 31 ("8086-compatible boot for HP 200LX, Sharp PC-3100") confirms 8086-compat is acknowledged pending work. So: confirmed as a real violation of the stated 8088 target, but it is one file of a codebase-wide issue; fixing this file alone yields no behavioral change on any machine that can currently boot UnoDOS (286+). The proposed fix (shared macro include, same mechanical replacements as outlast.asm) is sound. I checked replacement safety: no bp/sp-relative access to the pusha frame exists in the file (layout-preserving macros still safest); none of the 16 shift sites operate on CX (so a push cx/mov cl,n/shift/pop cx wrapper is safe); all three movzx sites are followed by instructions that overwrite or ignore flags, so a mov+xor replacement is flag-safe.
FIX: Create a shared include apps/cpu8086.inc and apply mechanical replacements (same file should be reused by outlast.asm and the other apps):

; ---- apps/cpu8086.inc ----
cpu 8086            ; build-time enforcement: NASM now rejects any remaining 186+ opcode

%macro PUSHA86 0    ; preserves PUSHA's exact 8-word layout
    push ax
    push cx
    push dx
    push bx
    push sp
    push bp
    push si
    push di
%endmacro

%macro POPA86 0
    pop di
    pop si
    pop bp
    add sp, 2       ; discard saved SP, as POPA does
    pop bx
    pop dx
    pop cx
    pop ax
%endmacro

%macro SHRI 2       ; shift right by immediate, 8086-safe (operand must not be CX/CL)
%if %2 = 1
    shr %1, 1
%else
    push cx
    mov cl, %2
    shr %1, cl
    pop cx
%endif
%endmacro

%macro SHLI 2
%if %2 = 1
    shl %1, 1
%else
    push cx
    mov cl, %2
    shl %1, cl
    pop cx
%endif
%endmacro

%macro SARI 2
%if %2 = 1
    sar %1, 1
%else
    push cx
    mov cl, %2
    sar %1, cl
    pop cx
%endif
%endmacro
; ---- end include ----

In apps/outlastv.asm add `%include "cpu8086.inc"` after the [ORG 0x0000] header, then:
1. Replace all 34 pusha -> PUSHA86 and popa -> POPA86 (lines 40,376,480,659,674,739,746,756,763,808,815,1066,1073,1227,1234,1304,1311,1344,1351,1517,1526,1597,1604,1733,1740,1802,1809,2063,2071,2152,2179,2220,2227,2265). Safe: no [bp+n]/[sp+n] access to the saved frame exists in the file.
2. Replace the 13 shr-by-imm sites with SHRI, e.g. line 183: `shr ax, 3` -> `SHRI ax, 3`; same for 194,300,316,323,952,1035,1050,1184,1434,1496,1503,1566. None operate on CX, so the CL wrapper is safe.
3. Line 280: `sar ax, 3` -> `SARI ax, 3`; line 890: `sar ax, 5` -> `SARI ax, 5`.
4. Line 849: `shl ax, 8` -> `mov ah, al` / `xor al, al` (faster and smaller than SHLI ax, 8).
5. movzx replacements (flag-safe at all three sites):
   line 729: `movzx bx, byte [cs:game_song_idx]` -> `mov bl, [cs:game_song_idx]` / `xor bh, bh`
   line 777: `movzx ax, byte [cs:sky_idx]` -> `mov al, [cs:sky_idx]` / `xor ah, ah`
   line 2085: `movzx ax, byte [cs:title_order + si]` -> `mov al, [cs:title_order + si]` / `xor ah, ah`
Do NOT touch the 29 shift-by-1 instructions (e.g. lines 247, 678, 1042, 2132) — they are valid 8086 encodings. Note `imul cx` (line ~880) and `idiv cx` are the one-operand forms and are 8086-legal; leave them. The `cpu 8086` directive in the include makes NASM fail the build if any site is missed. Caveat: this fix only matters once kernel/kernel.asm receives the same treatment — the kernel currently cannot run on an 8088 at all.

## [high/high] apps/pacman.asm: 121 non-8086 sites
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\pacman.asm:apps/pacman.asm lines 107 (first pusha) through 2248 (last popa); all 121 sites confirmed: pusha x36, popa x37, movzx x32, shr-imm3 x14, shl-imm3 x2 | area:8088 compatibility
DESC: 121 CPU-8086 errors: popa x37 / pusha x36 (line 107), movzx x32, shr-imm x14, shl-imm x2. Highest count among CGA apps; all 16-bit forms, no 32-bit registers.
VERIFIER: Independently recounted every claimed site in C:\Users\arin\Documents\Github\unodos\apps\pacman.asm and the finding is exactly correct: pusha x36, popa x37, movzx x32, shr reg,imm(>1) x14, shl reg,imm(>1) x2 = 121 sites. First site is `pusha` at line 107 (app entry point), last is `popa` at line 2248, matching the cited range 107-2248. All operands are 16-bit (the only grep hit resembling a 32-bit register was the word "Credits"/"edi" in a string at line 2562), and no other 186+ forms exist (no push-imm, no imul reg,imm, no enter/leave/bound/ins/outs — the one "enter" hit at line 1608 is a comment). "Highest among CGA apps" is also true: pacman.asm 121 vs notepad 88, launcher 61, browser 49, tetris 46 (pacmanv.asm at 154 is the VGA variant).

Failure path is concrete and unguarded: README.md states the minimum CPU is "Intel 8088 @ 4.77 MHz" / "Intel 8088 or later processor". No `cpu 8086` directive exists anywhere in the repo and the Makefile passes no such flag, so NASM silently accepts these instructions. There is no runtime CPU detection in the kernel, launcher, or boot code. On a real 8088/8086: opcode 60h (pusha) decodes as JO rel8, so the very first instruction of the app (line 107) becomes a conditional jump that swallows the following `push ds` byte and desynchronizes the instruction stream; 0Fh (movzx prefix) decodes as POP CS (wild far jump); C1h (shr/shl reg,imm8) decodes as the undocumented RET alias. The app crashes at instruction one on the stated minimum hardware.

Important severity context the finding omits: this is systemic, not pacman-specific. kernel/kernel.asm has ~244 equivalent non-8086 sites, and boot/vbr.asm line 6 explicitly says "This version requires 386+ CPU (uses EAX, push dword)" with a TODO to create an 8086-compatible version. On a real 8088 the OS never boots far enough to launch PACMAN.BIN, so fixing this file alone yields no user-visible benefit until boot+kernel are fixed. The finding is accurate as one file's inventory within a project-wide 8088-compat audit; "high" severity properly belongs to the project-wide issue. The suggested mechanical fix is sound and verified safe for this file: pacman.asm contains zero [bp+/-] or [sp+/-] stack-frame accesses, so a push/pop-sequence macro replacement for pusha/popa cannot break any frame-layout assumption, and all 16 shift-imm sites use count 3 exclusively.
FIX: Add `cpu 8086` at the top of apps/pacman.asm so NASM enforces the target, then apply three mechanical replacements (all verified safe for this file):

1) pusha/popa (73 sites) — define macros and substitute 1:1 (safe: file has zero BP/SP-relative frame accesses):
%macro PUSHA86 0
    push ax
    push cx
    push dx
    push bx
    push bp
    push si
    push di
%endmacro
%macro POPA86 0
    pop di
    pop si
    pop bp
    pop bx
    pop dx
    pop cx
    pop ax
%endmacro

2) movzx (32 sites) — all are zero-extends of a byte memory operand into a 16-bit reg, e.g. line 575:
    movzx bx, byte [cs:_dm_col]
becomes
    mov bl, [cs:_dm_col]
    xor bh, bh
(pattern: mov <low8>, <src> ; xor <high8>, <high8> — for the `movzx ax, ...` sites use mov al/xor ah,ah)

3) shr/shl reg, 3 (16 sites, all count=3, e.g. lines 608-609, 1072-1073, 1145, 1149, 1406, 1409, 1467, 1476, 1491, 1494, 1501, 1504, 2186, 2189) — replace with three shift-by-1 ops (no register clobber, unlike mov cl,3/shift):
    shr ax, 3   ->   shr ax, 1
                     shr ax, 1
                     shr ax, 1
(same pattern for shl bx,3 / shl cx,3 / shr bx,3 / shr cx,3)

Then reassemble: nasm -f bin -o build/pacman.bin apps/pacman.asm — the `cpu 8086` directive makes NASM reject any remaining non-8086 instruction. NOTE: this fix alone does not make the OS 8088-capable; kernel/kernel.asm (~244 sites) and boot/vbr.asm (386+, uses EAX/push dword per its own header comment) must be fixed too.

## [high/high] apps/pacmanv.asm: 154 non-8086 sites (worst app)
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\pacmanv.asm:apps/pacmanv.asm:146-2752 (range confirmed exact: first site = pusha at 146, last site = popa at 2752) | area:8088 compatibility
DESC: 154 CPU-8086 errors: movzx x46, pusha x41 / popa x41 (line 146), shr-imm x21, shl-imm x5. All 16-bit forms.
VERIFIER: Verified by direct count of C:\Users\arin\Documents\Github\unodos\apps\pacmanv.asm (2925 lines), with comments stripped before matching. The numbers match the finding exactly: movzx x46 (first at line 475, last at 2736), pusha x41 (first at line 146, the app entry point), popa x41 (last at line 2752), shr-with-immediate>1 x21 (all count 3 or 4: shr ax,3 x17; shr al,4 x2; shr bx,3; shr cx,3), shl-with-immediate>1 x5 (shl bx,4 x3; shl bx,3; shl cx,3). Total = 154. The claimed line range 146-2752 is exact (first pusha to last popa).

The failure is real and unmitigated:
1. Target is genuinely 8088: README.md states "Intel 8088 or later processor" and lists minimum CPU "Intel 8088 @ 4.77 MHz". No `cpu 8086` directive exists anywhere in the repo's .asm/.inc files, so NASM silently assembles these 186+/386+ instructions.
2. No gating mechanism: grep of kernel/, apps/, boot/ found no CPU detection (no cpu_detect/is_286/is_386 etc.), and the BIN header format (magic 'UI' + name + icon) has no CPU-requirement field. PACMANV.BIN is built and placed on the shipping 1.44MB floppy and launcher test image (Makefile lines 164-171, 268-270).
3. Concrete failure path on a real 8088 (with an 8-bit ISA VGA card, which the finding correctly notes is a viable XT configuration): the very first instruction at entry (line 146, pusha = opcode 60h) decodes on 8086/8088 as the undocumented alias of JO rel8 — registers are never saved and control may jump to a garbage offset. Any movzx (0F B6) decodes as POP CS + garbage bytes — wild far jump. Any shr reg,imm with count>1 (opcode C1) decodes as the undocumented RETN alias — pops a non-return-address off the stack and jumps to it. All three classes are instantly fatal, so severity "high" is justified for the stated hardware baseline.

Two refinements: (a) "worst app" claim is correct — ranking all 16 apps by the same metric gives pacmanv.asm 154, ahead of pacman.asm at 121; (b) caveat: kernel/kernel.asm itself has 244 such sites, so on a real 8088 the OS never reaches this app — fixing pacmanv only matters as part of the codebase-wide 8088 cleanup, not standalone. Also one fix-safety note the original finding missed: a naive shift rewrite via `mov cl, n / shr reg, cl` would clobber CL, which holds live data adjacent to several sites (e.g., `movzx bx, cl` at lines 2038, 2500, 2585, 2620), so repeated shift-by-1 is the safe mechanical form (all counts here are <=4, where it is also faster on 8086).
FIX: Mechanical, 8086-safe replacements in apps/pacmanv.asm (add `cpu 8086` after [ORG 0x0000] to make NASM enforce):

1) pusha/popa (82 sites) — define once near top, then replace each pusha with PUSHA86 and each popa with POPA86:
%macro PUSHA86 0
    push ax
    push cx
    push dx
    push bx
    push bp
    push si
    push di
%endmacro
%macro POPA86 0
    pop di
    pop si
    pop bp
    pop bx
    pop dx
    pop cx
    pop ax
%endmacro
(Omitting the SP slot is safe: every pusha here is paired symmetrically with popa and nothing reads the saved SP from the frame.)

2) shr/shl with immediate count >1 (26 sites, all counts are 3 or 4) — unroll to shift-by-1:
    shr ax, 3   ->   shr ax, 1
                     shr ax, 1
                     shr ax, 1
    shl bx, 4   ->   shl bx, 1  (x4)
Do NOT use `mov cl, N / shr reg, cl`: CL carries live data near several sites (lines 2038, 2500, 2585, 2620).

3) movzx (46 sites) — by pattern:
   - movzx bx, byte [cs:var]  ->  mov bl, [cs:var]
                                  mov bh, 0
     (same for ax/cx/dx destinations; `mov rH, 0` preserves flags exactly like movzx, unlike xor)
   - movzx bx, cl             ->  mov bl, cl
                                  mov bh, 0
   - movzx bx, bl (line 2121) ->  mov bh, 0
   - movzx si, dl (1463) / movzx dx, dl (1464):
                                  mov si, dx
                                  and si, 0x00FF     ; and: mov dh, 0 for line 1464
   - movzx si/di, byte [cs:var] (SI/DI have no 8-bit halves; lines 612, 628, 1547, 1629, 1659, 1691, 1777, 1784, 1793):
                                  mov si, [cs:var]
                                  and si, 0x00FF
     (word read overlaps the next byte but the mask discards it; none of these variables sit at a segment end. If flags must be preserved at a given site, route through AX instead: push ax / mov al, [cs:var] / mov ah, 0 / mov si, ax / pop ax.)

## [high/high] apps/settings.asm: 10 non-8086 sites (pusha/popa only)
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\settings.asm:apps/settings.asm — pusha: 114, 928, 1059, 1209; popa: 484, 940, 1026, 1052, 1107, 1305 (10 sites total, assembler-verified) | area:8088 compatibility
DESC: 10 CPU-8086 errors: popa x6 / pusha x4 (lines 114, 484, 928). Otherwise clean.
VERIFIER: Confirmed by direct inspection and by assembler. I prepended `CPU 8086` to C:\Users\arin\Documents\Github\unodos\apps\settings.asm and ran the project's nasm (same flags as the Makefile rule): NASM reports "no instruction for this cpu level" at exactly 10 lines and nowhere else, matching the finding precisely — pusha x4 at lines 114 (entry), 928 (apply_with_revert), 1059 (draw_countdown_ui), 1209 (draw_time_section); popa x6 at lines 484, 940, 1026, 1052, 1107, 1305. The "otherwise clean" claim also holds: no push-immediate, multi-bit shifts, movzx, or 32-bit registers in the file. Failure path is concrete: PUSHA/POPA (opcodes 60h/61h) are 80186+ instructions; on a real 8088/8086 they decode as undocumented aliases of JO/JNO rel8, so the `pusha` at the app entry point (line 114) would execute a conditional jump whose displacement is the next opcode byte — immediate crash/wild jump when Settings launches on the project's documented minimum hardware (README.md: minimum CPU "Intel 8088 @ 4.77 MHz"; docs/bootloader-architecture.md says floppy boot requires 8086). No mitigating mechanism exists: no CPU detection, no CPU directive anywhere in the repo, no PUSHA86/POPA86 macros (the suggested macros do NOT yet exist and must be defined). Safety of the macro fix verified: all 5 pusha/popa pairings are strictly balanced across all exit paths, the file has zero BP-relative or SS-relative addressing into the saved frame, and nothing depends on the 16-byte pusha frame size, so a 7-register macro (skipping SP) is semantically equivalent. SEVERITY CAVEAT: "high" is overstated in isolation — pusha/popa appear 512 times across 20 files including kernel/kernel.asm (39) and boot/stage2.asm (2), and docs/APP_DEVELOPMENT.md teaches the pattern; the kernel would crash on an 8088 long before settings.asm runs, and TODO.md lists 8086-compatible boot as unfinished. Fixing this file alone yields no benefit on real 8088 hardware; it only matters as part of a codebase-wide 8086-compat pass. Zero functional or performance impact on 186+ CPUs (including QEMU defaults).
FIX: Add to the top of apps/settings.asm (after the org/header, before `entry:`):

    CPU 8086

    %macro PUSHA86 0
        push ax
        push cx
        push dx
        push bx
        push bp
        push si
        push di
    %endmacro

    %macro POPA86 0
        pop di
        pop si
        pop bp
        pop bx
        pop dx
        pop cx
        pop ax
    %endmacro

Then replace `pusha` with `PUSHA86` at lines 114, 928, 1059, 1209 and `popa` with `POPA86` at lines 484, 940, 1026, 1052, 1107, 1305. (SP is deliberately omitted — real POPA discards the SP slot anyway, and skipping it also sidesteps the 8086 `push sp` quirk. Verified safe here: all pairs balanced, no SP/BP-relative access to the saved frame.) Note this only achieves real 8088 compatibility if the same treatment is applied to boot/stage2.asm and kernel/kernel.asm (512 occurrences repo-wide); ideally the macros belong in a shared include used by all apps, and `CPU 8086` should be added per-file to make regressions build errors.

## [high/high] apps/sysinfo.asm: 12 non-8086 sites
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\sysinfo.asm:pusha/popa: 64, 188, 195, 349; movzx: 80, 87, 254, 271, 275, 327; shr-imm: 443, 469 (lines 163/339 'shr ax, 1' are 8086-safe and correctly excluded) | area:8088 compatibility
DESC: 12 CPU-8086 errors: movzx x6 (line 80 'movzx ax, cl', 87 'movzx ax, bh'), pusha/popa x4 (line 64), shr-imm x2. Ironically the system-info app itself will not run on an 8088 to report the CPU.
VERIFIER: Verified by reading C:\Users\arin\Documents\Github\unodos\apps\sysinfo.asm in full. The count of 12 non-8086 sites is exactly right and complete: pusha/popa at lines 64, 188, 195, 349 (4 sites, 186+); movzx at lines 80, 87, 254, 271, 275, 327 (6 sites, 386+); 'shr al, 4' at lines 443 (line_puthex) and 469 (line_putbcd) (2 sites, 186+ immediate-count shift). The auditor correctly excluded 'shr ax, 1' at lines 163 and 339, which assemble to the 8086-legal D1 form. No other non-8086 instructions exist in the file (no push-imm, no shl-imm, no enter/leave). 8088 support is an explicit project requirement: README.md line 9 ("just BIOS services and an Intel 8088 or later processor") and line 201 (minimum CPU "Intel 8088 @ 4.77 MHz"), docs/FEATURES.md line 7. NASM assembles these without error (default CPU ANY), so the failure is runtime-only on real hardware: on an 8088, opcode 60h (pusha) executes as an alias of JO, 0Fh (movzx prefix) executes as POP CS, and C0h (shr r/m8,imm8) executes as an alias of RET imm16 — the app misdecodes at its very first instruction (line 64) and crashes/corrupts control flow before drawing anything. No protective mechanism exists; apps are entered via retf-style far call with no CPU gate. One severity caveat the finding omits: kernel\kernel.asm itself contains 123 movzx/pusha/popa occurrences and docs/bootloader-architecture.md line 221 says the HDD boot path is "386+ (for now)", so on a real 8088 the kernel never boots far enough to launch sysinfo. The finding is factually correct, but fixing this file alone restores nothing — it is only meaningful as part of a codebase-wide 8086 sweep, so "high" severity applies to the sweep, not this file in isolation.
FIX: Mechanical 8086-safe replacements in apps/sysinfo.asm (adds ~40 bytes; file currently fits well inside the 1536-byte 'times' pad at line 519, so padding absorbs it):

1) entry pusha/popa pair (lines 64 and 188) — replace 'pusha' with:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
and 'popa' (line 188, after 'pop es'/'pop ds') with the reverse:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax

2) draw_ui pusha/popa pair (lines 195 and 349) — same replacement as above (DS is also clobbered in draw_ui but was already not saved by pusha; caller re-loads DS, unchanged behavior).

3) movzx sites — each becomes mov + xor (no flag dependence at any site; AH is dead at each):
   line 80:  movzx ax, cl              ->  mov al, cl
                                            xor ah, ah
   line 87:  movzx ax, bh              ->  mov al, bh
                                            xor ah, ah
   line 254: movzx ax, cl              ->  mov al, cl
                                            xor ah, ah
   line 271: movzx ax, bl              ->  mov al, bl
                                            xor ah, ah
   line 275: movzx ax, bh              ->  mov al, bh
                                            xor ah, ah
   line 327: movzx ax, byte [cs:t_vmode] -> mov al, [cs:t_vmode]
                                            xor ah, ah

4) shr-imm sites (lines 443 and 469) — CL is live in both routines (holds the saved byte), so do NOT use 'mov cl,4 / shr al,cl'; use four single shifts instead:
   shr al, 4   ->   shr al, 1
                    shr al, 1
                    shr al, 1
                    shr al, 1

Optionally add 'cpu 8086' after '[BITS 16]' (line 5) so NASM rejects any future regression at assemble time. Note: this fix is only effective for actual 8088 operation once the kernel (123 similar sites in kernel\kernel.asm) gets the same treatment.

## [high/high] apps/tetris.asm: 46 non-8086 sites
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\tetris.asm:apps/tetris.asm:39,185,642,653,660,691,700,706,715,735,740,752,783,790,819,828,884,890,1025,1098,1105,1203,1211,1258,1265,1332,1339,1369,1376,1409,1477,1507,1514,1539,1547,1574,1624,1754,1761,1832,1839,1871,1905,1912,1959,1969 (46 sites; line 1959 movzx is the only one failing on 286-class hardware) | area:8088 compatibility
DESC: 46 CPU-8086 errors: popa x19 / pusha x17 (lines 39/185/642...), shl-imm x9, movzx x1.
VERIFIER: CONFIRMED with severity refinement. Mechanically verified by assembling C:\Users\arin\Documents\Github\unodos\apps\tetris.asm with an injected 'cpu 8086' directive under NASM 2.16.01: exactly 46 'no instruction for this cpu level' errors, matching the auditor's census precisely (pusha x17, popa x19 — asymmetric because several functions have multiple popa exit paths, e.g. 691/715 and 884/890 — shl-imm x9, movzx x1). No additional non-8086 instructions exist in the file (no push-imm, imul-imm, etc.).

Failure mechanics on a real 8088/8086: opcode 60h/61h (pusha/popa) alias to JO/JNO rel8, so line 39 at app entry would execute a wild conditional branch consuming the next instruction byte as a jump displacement — immediate derailment. C1h (shl r16,imm8) aliases to RET, causing premature return with a corrupted stack. 0Fh (movzx prefix) is POP CS on 8086.

Severity context the original finding missed: (1) The failure path on 8086 is currently UNREACHABLE — kernel/kernel.asm itself has 43 pusha/popa sites, boot/stage2.asm has 2, and docs/bootloader-architecture.md states the current bootloader requires '386+ (for now)'. An 8086 machine cannot boot UnoDOS at all, so fixing tetris.asm alone delivers nothing; TODO.md line 31 ('8086-compatible boot for HP 200LX, Sharp PC-3100') confirms 8086 support is acknowledged pending work, not a current contract. README.md line 201 does document 'Intel 8088' as minimum CPU, so the code violates the documented spec project-wide, not tetris-specifically. (2) The one site with real near-term impact is the movzx at line 1959 (update_score_display): movzx is 386+, so it raises invalid-opcode (#UD, int 6) on an 80286 — hardware within the documented 'Recommended: 80286+' floor — crashing Dostris the first time the score panel is drawn after starting a game. All 45 other sites are 186-safe and fine on a 286. All tested hardware in README (386SX, 486, Atom, QEMU i386) is unaffected by any of the 46 sites.

Verified the proposed macro fix is safe: grep found no 'mov bp,sp' or '[bp+...]' frame access anywhere in tetris.asm, so no code depends on the 8-slot pusha frame layout, and every popa is a paired exit of a pusha-opening function. Practical severity: medium (movzx/286 is the only reachable defect today); 'high' for 8086 is correct only as part of a project-wide 8086 effort that must include kernel.asm and the bootloader.

Exact 46 site lines: pusha 39,642,752,790,828,1025,1105,1211,1265,1339,1376,1477,1547,1624,1761,1839,1912; popa 185,691,715,783,819,884,890,1098,1203,1258,1332,1369,1409,1539,1574,1754,1832,1905,1969; shl-imm 653,660,700,706,735,740,1507,1514,1871; movzx 1959.
FIX: Priority 1 (reachable bug on documented 286 hardware) — apps/tetris.asm line 1959, replace:
    movzx dx, byte [cs:level]
with (8086-safe, same clocks class, DX result identical since level is unsigned):
    mov dl, [cs:level]
    xor dh, dh

Priority 2 (full 8086 cleanliness; only meaningful together with kernel.asm/stage2.asm cleanup) — add after line 6 ([ORG 0x0000]):
    cpu 8086

    %macro PUSHA86 0
        push ax
        push cx
        push dx
        push bx
        push bp
        push si
        push di
    %endmacro

    %macro POPA86 0
        pop di
        pop si
        pop bp
        pop bx
        pop dx
        pop cx
        pop ax
    %endmacro

    %macro SHL86 2
    %rep %2
        shl %1, 1
    %endrep
    %endmacro

Then mechanical replacement: all 17 'pusha' -> 'PUSHA86' (lines 39,642,752,790,828,1025,1105,1211,1265,1339,1376,1477,1547,1624,1761,1839,1912); all 19 'popa' -> 'POPA86' (lines 185,691,715,783,819,884,890,1098,1203,1258,1332,1369,1409,1539,1574,1754,1832,1905,1969); 'shl ax, 3' -> 'SHL86 ax, 3' (653,660,700,706,740,1507,1514); 'shl bx, 5' -> 'SHL86 bx, 5' (735); 'shl si, 2' -> 'SHL86 si, 2' (1871). Safe because no code in the file reads the saved-register frame via BP/SP (verified), and the 7-slot macro pair is always matched. Note 'cpu 8086' must be added only after the replacements, or the build breaks. Identical treatment is needed in apps/tetrisv.asm and the other 13 apps, kernel/kernel.asm (43 sites) and boot/stage2.asm (2 sites) before 8086 operation is actually possible (cf. TODO.md:31).

## [high/high] heap_initialized flag is read/written with DS=0x1400, so it actually lives in unowned RAM at linear 0x1CBB9 - and inside the heap's own allocatable range
- src:boot-memory-map | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:9983 'cmp word [cs:heap_initialized], 0' and kernel/kernel.asm:9989 'mov word [cs:heap_initialized], 1' (declaration at 20053 unchanged) | area:heap init
DESC: heap_initialized is declared as kernel data at line 20053 ('heap_initialized: dw 0'); NASM resolves it to kernel offset 0x8BB9 (verified via nasm listing). But mem_alloc_stub accesses it AFTER switching DS to the heap segment: 'mov ax, 0x1400 / mov ds, ax ... cmp word [heap_initialized], 0' (9977-9983) and 'mov word [heap_initialized], 1' (9989). So the live flag is at 0x1400:0x8BB9 = linear 0x1CBB9 - outside the kernel image (which ends at 0x1B000), in memory the OS never initializes. The kernel-segment variable at 0x1000:0x8BB9 is dead. Consequences: (1) on real hardware with non-zero residual RAM the flag can read as already-set, so the heap is never initialized and mem_alloc walks garbage block headers; (2) 0x8BB9 < 0xF000, so the flag word sits inside the heap's allocatable region - a granted allocation covering offset 0x8BB9 lets the client clobber the flag, triggering re-initialization of an in-use heap. Works by accident in QEMU only because RAM is zeroed.
VERIFIER: Every factual claim in the finding checks out against the actual code and a fresh NASM assembly. (1) heap_initialized is declared at kernel/kernel.asm:20053 and resolves to kernel offset 0x8BB9 - verified by assembling with the exact Makefile flags (nasm -f bin -Ikernel/) and reading the listing: line 20053 emits '00008BB9 0000 heap_initialized: dw 0'. (2) The two accesses (line 9983 'cmp word [heap_initialized], 0' and line 9989 'mov word [heap_initialized], 1') assemble to '83 3E B98B 00' and 'C7 06 B98B 01 00' - default DS addressing with no segment override - and both execute after lines 9977-9980 set DS=ES=0x1400. So the live flag is at 0x1400:0x8BB9 = linear 0x1CBB9. (3) That address is genuinely unowned, never-initialized RAM: build/kernel.bin is exactly 45056 bytes (0xB000); boot/stage2.asm loads exactly KERNEL_SECTORS equ 88 sectors (=45056 bytes) at KERNEL_SEGMENT equ 0x1000, so the loaded image ends at linear 0x1B000 < 0x1CBB9. heap_initialized has only 3 references in the entire repo (declaration plus the two buggy accesses) - there is no boot-time heap init, no zeroing of segment 0x1400, and no other mechanism that would mask the bug. On real hardware with nonzero residual RAM the flag can read nonzero, skipping heap init and walking garbage block headers; QEMU's zeroed RAM hides it. (4) The flag offset 0x8BB9 is inside the allocatable range: the search loop walks si from 0 to <0xF000 in segment 0x1400 and returns si+4 with no bounds carve-out, so a granted allocation can span heap offset 0x8BB9 and a client write clobbers the live flag, re-arming lazy init on an in-use heap. (5) No protective mechanism exists: mem_alloc_stub/mem_free_stub are reachable only via kernel API table slot 7/8 (kernel.asm:7312-7313, INT 0x80 dispatch), which always runs with CS=0x1000, so a cs: override is a valid and minimal fix; this is a segment-addressing bug, not a race, so cli is irrelevant. One refinement to the severity picture: the finding actually understates the brokenness of this allocator. The heap base 0x1400:0000 is linear 0x14000 = kernel offset 0x4000, which contains live kernel code (scrollbar logic at listing offset 00004000), and the heap range 0x14000-0x23000 overlaps the kernel image 0x14000-0x1B000. So even in QEMU the first mem_alloc call's init write 'mov word [0], 0xF000' corrupts kernel code. That is a separate finding (flagged for a separate task); it means the cs:-override fix below corrects this finding's specific defect but the allocator remains unsafe until the heap base is also moved above the kernel image (note: with the heap overlapping the kernel, even the kernel-segment copy of the flag at linear 0x18BB9 = heap offset 0x4BB9 remains clobberable by allocations until the base is fixed).
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, add a CS segment override to both flag accesses so they hit the kernel-segment variable (CS is always 0x1000 in kernel code; the 2Eh override prefix is 8086-safe):

Line 9983:
    cmp word [heap_initialized], 0
->
    cmp word [cs:heap_initialized], 0

Line 9989:
    mov word [heap_initialized], 1
->
    mov word [cs:heap_initialized], 1

This makes the flag live at 0x1000:0x8BB9 inside the loaded kernel image (guaranteed 0 at boot from the 'dw 0' initializer) and removes the dependence on residual RAM contents. Note this is necessary but not sufficient for a safe allocator: the heap base 0x1400:0000 overlaps the kernel image (linear 0x14000-0x1B000) and must be moved above 0x1B000 (e.g. segment 0x2000) as a separate fix, otherwise allocations can still corrupt kernel code including the flag itself.

## [high/high] mem_alloc uses signed compare (jge) on block sizes - the initial 0xF000 free block looks negative, so every allocation fails
- src:boot-memory-map | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm line 10006 ('jge .allocate'); supporting context: lines 9986-9988 (0xF000 init), 10005 (cmp ax, bx), 10009-10011 (search termination that converts the rejection into a NULL return) | area:heap allocator
DESC: First-fit check: 'cmp ax, bx / jge .allocate' (10005-10006) where AX = block size and BX = requested size. The initial free block is created with size 0xF000 (line 9987), which as a signed 16-bit value is -4096, so 'jge' is never taken for any normal request. The search then does 'add si, ax' -> SI = 0xF000, fails 'cmp si, 0xF000 / jb .search' (10010-10011) and returns NULL. Net effect: syscall 7 (malloc) always returns 0 on a fresh heap - after having already corrupted 4 bytes of kernel code at linear 0x14000-0x14003 via the lazy-init header write (see the heap-overlap finding). Any block size or request >= 0x8000 hits the same signed-compare trap.
VERIFIER: Confirmed by direct code reading of C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. mem_alloc_stub (line 9963, dispatched as kernel API slot 7 via the table at line 7312) lazily initializes the heap at DS=0x1400 with a single free block of size 0xF000 (lines 9986-9988). The first-fit check at lines 10005-10006 is 'cmp ax, bx' / 'jge .allocate' with AX=block size (0xF000) and BX=rounded request. JGE is a SIGNED comparison: 0xF000 is -4096, which is less than any normal positive request (e.g. 0xF000-0x0014=0xEFEC sets SF=1, OF=0, so SF!=OF and the jump is not taken). The block is rejected, '.next' does 'add si, ax' giving SI=0xF000, the limit check 'cmp si, 0xF000 / jb .search' (lines 10010-10011) fails (ZF=1, CF=0), and the function falls into .fail_restore returning AX=0. Subsequent calls skip init but hit the identical rejection, so syscall 7 returns NULL on every normal request, deterministically. No mitigating mechanism exists: grep shows only mem_alloc_stub/mem_free_stub touch segment 0x1400 and only they reference heap_initialized; the API table dispatch performs no size validation. Two refinements to the original finding: (1) the claim that requests >= 0x8000 'hit the same trap' is imprecise -- such a request is itself negative signed, so against the 0xF000 block JGE is actually taken and the allocation coincidentally succeeds; the real second hazard is overcommit, where a request >= 0x8000 compared against a small positive free block (e.g. 0x0100) takes JGE (256 >= -28672) and returns a too-small block, corrupting the heap. Both hazards share the same root cause and fix. (2) The lack of block splitting in .allocate (lines 10019-10025) means even after the fix only one outstanding allocation can ever exist (first caller gets all ~60KB); that is a separate functional limitation. The parenthetical about corrupting kernel code at linear 0x14000-0x14003 belongs to the separate heap-overlap finding and is not required for this bug; as corroborating context, heap_initialized (a kernel data label at line 20053) is indeed dereferenced with DS=0x1400 (lines 9983/9989), i.e. at the wrong segment. The suggested fix (jge -> jae) is correct, minimal, and 8086-safe, and is consistent with the existing unsigned 'jb' limit check at line 10011.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm line 10006, change the signed conditional jump to unsigned:

    ; Check if large enough
    cmp ax, bx
-   jge .allocate                   ; Found suitable block
+   jae .allocate                   ; Found suitable block (unsigned: sizes are 0..0xF000)

JAE (jump if CF=0) is valid 8086 and treats both block size and request as unsigned, matching the unsigned 'jb' heap-limit check at line 10011. This both makes the 0xF000 initial block allocatable for normal requests and prevents the overcommit case (request >= 0x8000 vs a small free block). Optional follow-up (separate change): add block splitting in .allocate so the first caller does not consume the entire 0xF000 block.

## [high/medium] Stale INT 0x80 dispatcher flags survive RETF app exit and corrupt the next task's registers and cursor lock
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: flags cleared at 168-169; consumed un-cleared at 388-403 (int80_return_point); task_exit_handler 14949; app_exit_stub cursor_locked reset 14984; stack switch + popa/ret 15043-15047; statics 20069-20075; also-affected resume path auto_load_launcher 1807-1813 | area:scheduler / app exit / INT 0x80 dispatcher
DESC: The dispatcher statics `_did_translate`, `_did_scale`, `_did_cursor_protect` (20060-20066) are cleared only at INT 0x80 ENTRY (168-169: `mov byte [_did_cursor_protect], 0 / mov byte [_did_translate], 0`) and are never cleared after use in `int80_return_point`. When an app exits via RETF (all 15 stock apps end with `retf`), control reaches `task_exit_handler` (14942) -> `app_exit_stub` -> stack switch to the next task (15035-15040: `mov ss,[bx+APP_OFF_STACK_SEG] / mov sp,... / popa / ret`) and that `ret` lands in `int80_return_point` WITHOUT passing the dispatcher entry. If the exiting app's last syscall before `retf` was a drawing API (bitmap APIs 0-6, 50-52, 56-62, 65-71, 80, 87, 94, 102-104), the resumed task executes 388-391 (`dec byte [cursor_locked]` - underflows to 0xFF since app_exit_stub reset it to 0 at 14975, disabling cursor draw/erase until the next task switch) and 396-403 (`mov bx,[_save_bx] / mov cx,[_save_cx]`, possibly `mov dx,[_save_dx] / mov si,[_save_si]`), overwriting the resumed task's just-restored BX/CX (and DX/SI) with the DEAD app's translated draw coordinates. The victim returns from its yield with corrupted registers - crashes/glitches in the OTHER running apps when an app exits, matching 'apps crash when several apps run' and 'visual anomalies'. Stock apps happen to end with non-drawing calls (win_destroy/theme/speaker), so this is latent for them but live for any SDK app that draws then RETFs.
VERIFIER: CONFIRMED, with two refinements (severity and one mechanism detail). The mechanism is real: the dispatcher statics _did_cursor_protect/_did_translate/_did_scale (kernel/kernel.asm:20069-20075, not 20060-20066) are written ONLY at INT 0x80 entry (168-169 clear; 194/213-216/232/316 set) and are read but never cleared at int80_return_point (388-403). When an app exits via RETF, control reaches task_exit_handler (14949) -> app_exit_stub (14956) -> stack switch to next task (15043-15047: mov ss/sp, popa, ret), and that ret lands at int80_return_point WITHOUT a dispatcher entry — verified by the initial-frame layout (14886-14899) and the matching yield path (14856-14857). Nothing in app_exit_stub or its callees (speaker_off, destroy_task_windows, win_begin_draw, gfx_set_font, redraw_affected_windows) touches the flags (grep: only dispatcher lines write them), and no IRQ handler does either. So the resumed task consumes the DEAD app's flags: (a) if _did_translate=1, lines 398-399 (and 402-403 if _did_scale=1) overwrite the resumed task's just-popa'd BX/CX (DX/SI), violating the yield ABI which explicitly preserves all GPRs via pusha (14781-14784) — real register corruption; (b) if _did_cursor_protect=1, line 390 decrements cursor_locked. Refinement to (b): if the resumed task has an active draw context, app_exit_stub already called win_begin_draw (15026) which incremented cursor_locked (1368), so the stale dec gives 0 (premature unlock, cursor drawn mid-batch-draw) instead of underflow; in the common no-context case cursor_locked was reset to 0 at 14984 (not 14975) and underflows to 0xFF, disabling mouse_cursor_hide/show (checks at 3738/3779) until the next yield-switch resets it (14824). Normal yield-to-yield switches are safe only because yield (API 34) is non-drawing (bitmap byte 4 = 0x00), so entry clears the flags; API-36 exit is safe for the same reason — only RETF exit (and the .exit_no_tasks -> auto_load_launcher relaunch at 1812-1813) bypasses the entry. SEVERITY REFUTED IN PART: I verified all 15 stock apps end with retf AND checked each one's final syscall — API 21, 32, 42, 54, 95, or 101 — none is in the drawing bitmap {0-6, 50-52, 56-62, 65-71, 80, 87, 94, 102-104}. The bug is therefore LATENT for every shipped app and CANNOT explain the observed 'apps crash when several apps run' / 'visual anomalies' symptoms with stock binaries; it is live only for SDK/third-party apps whose last INT 0x80 before retf is a drawing API (translate-arm additionally requires an active draw context and a translate-bitmap API). Downgrade severity from high to medium (real ABI bug, no in-tree trigger). The auditor's cited line numbers were uniformly off by 7-9 lines but referenced the correct code.
FIX: Make int80_return_point consume the flags one-shot (covers all three resume paths: yield 14857, app exit 15047, auto_load_launcher 1813; DS=0x1000 is guaranteed on every path). In kernel/kernel.asm replace lines 387-404 with:

    ; --- Cursor unprotect (A1 — centralized, Build 396) ---
    cmp byte [_did_cursor_protect], 0
    je .no_cursor_restore
    mov byte [_did_cursor_protect], 0   ; consume: one-shot per dispatch
    dec byte [cursor_locked]
    call mouse_cursor_show
.no_cursor_restore:

    ; Restore pre-translation BX/CX so apps keep their original coordinates.
    cmp byte [_did_translate], 0
    je .no_coord_restore
    mov byte [_did_translate], 0        ; consume: one-shot per dispatch
    mov bx, [_save_bx]
    mov cx, [_save_cx]
    cmp byte [_did_scale], 0
    je .no_coord_restore
    mov byte [_did_scale], 0            ; consume: one-shot per dispatch
    mov dx, [_save_dx]
    mov si, [_save_si]
.no_coord_restore:

All added instructions are mov byte [mem], imm — 8086-safe (note the existing dispatcher already uses 386+ movzx/bt, so no regression either way). Alternative (less robust, fixes only the exit path): insert the three clears at the top of app_exit_stub (before line 14958): mov byte [_did_cursor_protect], 0 / mov byte [_did_translate], 0 / mov byte [_did_scale], 0 — but this misses nothing today only because no other dispatcher-bypassing resume exists; the consumption-site fix is preferred.

## [high/high] ES register is not part of the task context - clobbered across every yield/context switch
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:14781-14784 (yield entry, no ES save), 14855-14857 (yield resume, no ES restore), 15048-15049 (app_exit_stub resume), 1812-1813 (boot-time switch), 14905-14925 (app_start_stub initial frame + saved SP); supporting: 144-145, 154, 406, 18813 | area:scheduler / context switch
DESC: app_yield_stub saves only PUSHA (AX,CX,DX,BX,SP,BP,SI,DI) on the task stack (14775) plus draw_context, caller_ds/es and SS:SP in the app table (14787-14798). The caller's DS is preserved via the dispatcher's `push ds` (154) / `pop ds` (406), but the actual ES register is saved NOWHERE: when task A yields and task B later yields back, A resumes with whatever ES value B (or the kernel) last had. The initial frame built by app_start_stub (14877-14912) also contains DS (FFF2) but no ES, so a freshly started task inherits a random ES from the previous task. Only the kernel-side `caller_es` variable is per-task; the register itself leaks between tasks. Any app that loads ES once and relies on it across an `int 0x80` yield/event_get will then read/write the WRONG 64KB segment (stosb/movsb into another task's memory) - silent cross-task memory corruption that only appears when multiple apps run. Stock apps mostly reload ES before each use (e.g. notepad's `push cs / pop es` at notepad.asm:334-335), which masks the bug.
VERIFIER: CONFIRMED by direct code reading of C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. (1) Mechanics: app_yield_stub (14781) saves context as PUSHA (14784, GP regs only — PUSHA excludes segment registers) on the task's own stack, plus draw_ctx/caller_ds/caller_es/SS:SP in the app table (14796-14807). DS survives because the dispatcher pushes it on the task stack (line 154) and pops it at int80_return_point (line 406), and the whole stack is swapped per-task. The physical ES register is stored NOWHERE: none of the three resume sites — yield (14851-14857), app_exit_stub (15044-15049), boot-time first switch (1808-1813) — reloads ES before popa/ret/pop ds/iret. APP_OFF_CALLER_ES only refreshes the kernel VARIABLE caller_es (14848-14849), which kernel APIs use to read app buffers (e.g. 16613 'mov es,[cs:caller_es]'); it is never written back to the physical register. (2) No hidden protection: int80_return_point (377-419) restores DS only; cli/sti (14851-14854) guards only the SS:SP swap; mouse_cursor_show/win_begin_draw preserving registers is irrelevant since the ES in force at switch time belongs to the YIELDING task, not the resuming one. (3) Contract: the kernel's own API stubs treat ES as callee-saved — e.g. gfx_draw_pixel_stub (7479-7494) documents 'Preserves: All registers' and does push es/pop es; ~115 'push es' sites kernel-wide; API_REFERENCE.md's API 34 entry lists no register clobbers. So an app keeping ES loaded across a yield is within the implied contract. (4) Concrete failure path: task A sets ES=A_seg, calls int 0x80 AH=34 (or API 73 delay_ticks, which calls app_yield_stub at 18813 — delay_ticks's own push es/pop es pairs at 18805/18809 and 18816/18820 bracket only BDA reads and do NOT protect across the yield); task B runs, leaves ES=B_seg or 0xA000; B yields back; A resumes with B's ES and its next stosb/movsb/[es:di] write silently corrupts the wrong 64KB segment. (5) Masking confirmed: stock apps reload ES immediately before each ES-consuming call (verified notepad.asm 334-335 'push cs / pop es'), so the bug is currently latent — no shipped app observably breaks, which slightly tempers the 'high' severity (it is a real contract violation and latent cross-task corruption, not an active crash). One refinement to the original finding: APP_DEVELOPMENT.md line 152 explicitly documents ES as 'Unknown - set as needed' AT APP ENTRY, so app_start_stub's frame lacking an initial ES word is technically conformant today; it only needs the extra word because the fixed resume path pops one. All five sites (yield save, yield resume, exit resume, boot resume, initial frame) must change together or the stack frames misalign and every task crashes on resume.
FIX: All five changes must land together (frame layout changes).

1) kernel.asm:14781 — save ES with the context (push es is 8086-safe; ES lands above the pusha frame on the per-task stack):
app_yield_stub:
    push es                         ; ES not covered by pusha
    pusha

2) kernel.asm:14855-14857 — restore on resume:
.same_task:
    popa
    pop es                          ; restore resuming task's ES
    ret

3) kernel.asm:15048-15049 (app_exit_stub resume path):
    popa
    pop es
    ret

4) kernel.asm:1812-1813 (boot-time first switch):
    popa
    pop es
    ret

5) kernel.asm:14911-14925 (app_start_stub) — insert initial ES word (= app segment, CX) between int80_return_point and the dummy pusha frame, shift pusha frame down 2 bytes, initial SP 0xFFE0 -> 0xFFDE:
    mov word [es:0xFFF0], int80_return_point
    mov [es:0xFFEE], cx                     ; initial ES = app segment
    mov word [es:0xFFEC], 0                 ; AX
    mov word [es:0xFFEA], 0                 ; CX
    mov word [es:0xFFE8], 0                 ; DX
    mov word [es:0xFFE6], 0                 ; BX
    mov word [es:0xFFE4], 0                 ; SP (ignored by popa)
    mov word [es:0xFFE2], 0                 ; BP
    mov word [es:0xFFE0], 0                 ; SI
    mov word [es:0xFFDE], 0                 ; DI
    ...
    mov word [bx + APP_OFF_STACK_PTR], 0xFFDE

Also update the stack-layout comment block at 14891-14900 to document the new ES word at FFEE and SP=FFDE.

## [high/high] event_wait_stub and kbd_wait_key busy-wait without yielding - one blocked task freezes the whole cooperative system
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:10298-10305, 690-696 | area:scheduler / event system
DESC: `event_wait_stub: call event_get_stub / test al,al / jz event_wait_stub` (10301-10305) and `kbd_wait_key: call kbd_getchar / test al,al / jz kbd_wait_key` (692-696) spin inside the kernel with no call to app_yield_stub and no HLT. Both are exposed as syscalls (API 10 at 7317, API 12 at 7321). Under the cooperative scheduler, a task calling API 10 while it is NOT the focused task can never receive KEY_PRESS events (focus filter at 10142-10152 leaves them queued), so it spins forever and every other task - including the focused one - is permanently starved: total system freeze. Stock apps poll API 9 + yield, but any app using the documented blocking APIs hangs the OS the moment a second app takes focus - matching 'apps crash/freeze when launching apps'.
VERIFIER: CONFIRMED by direct code reading, with refinements and one critical correction to the suggested fix.

Verified mechanism (kernel/kernel.asm):
1. Spin loops exist exactly as claimed: event_wait_stub at lines 10310-10314 (`call event_get_stub / test al,al / jz event_wait_stub / ret`) and kbd_wait_key at 692-696 (`call kbd_getchar / test al,al / jz kbd_wait_key / ret`). Neither calls app_yield_stub nor HLTs. (Finding cited 10298-10305 for event_wait_stub; those lines are actually the tail of event_get_stub's .no_event_return — the loop itself is 10310-10314.)
2. Both are exposed as syscalls: API table line 7317 (slot 10 = event_wait_stub) and 7321 (slot 12 = kbd_wait_key) — exact match.
3. The scheduler is purely cooperative: app_yield_stub (14780) is the ONLY context-switch mechanism; there is no timer-IRQ preemption anywhere (only a comment at 20065 anticipating "preemptive scheduling" in the future). The INT 0x80 dispatcher does `sti` (line 151) before dispatch, so IRQs still post events during the spin, but no task switch can occur.
4. The deadlock for a non-focused task in event_wait is real and is actually WORSE than claimed, due to head-of-line blocking: event_get_stub (10137-10164) examines only the head queue entry. If the head event is EVENT_KEY_PRESS and current_task != focused_task, it jumps to .no_event WITHOUT advancing the head and WITHOUT scanning past it (10160-10161 "leave event in queue for correct task"). The BIOS keyboard fallback (10216-10223) is also focus-gated (.bios_focus_fail). So: non-focused task calls API 10 → spins; user presses any key → KEY_PRESS lands at queue head → spinning task can never consume it, never yields → focused task never runs to consume it → permanent unrecoverable freeze. Even subsequent mouse events queued behind the keypress are unreachable. (Before a keypress arrives, a stray mouse event would unblock it — by being wrongly stolen from the focused task — but one keystroke wedges the system permanently.)
5. kbd_wait_key: kbd_getchar (664-688) reads kbd_buffer with no focus filter, so on floppy boot (custom INT 9 installed, fills buffer at 610-616) a keypress eventually unblocks it — but all other tasks are frozen until then. On HD/USB boot (use_bios_keyboard=1, custom INT 9 NOT installed; kbd_buffer_tail is written ONLY by the IRQ1 handler), kbd_buffer never fills, so API 12 spins forever regardless of focus — an even harder freeze on that boot path.
6. Yield-from-syscall is an established safe pattern: delay_ticks (API 73, line 18797) calls app_yield_stub (18812) from the identical INT 0x80/DS=0x1000 context. app_yield_stub does pusha/popa around the switch, so GP registers are preserved.

Two corrections to the original finding:
A. The suggested fix is UNSAFE as written. kbd_wait_key and event_wait_stub are also called directly from kernel boot context with current_task=0xFF and ZERO running tasks: keyboard_demo (line 1990) is the fallback when LAUNCHER.BIN fails to load (line 1853), plus test_filesystem (1486) and test_app_loader (1651). In that context app_yield_stub never returns: .no_save → scheduler_next finds no RUNNING task → .none_found returns current_task=0xFF → je .idle → sti/hlt/jmp .no_save, looping forever. The naive fix would hang the boot fallback on its first wait. The yield must be guarded with `cmp byte [current_task], 0xFF`. Also, pusha does not save segment registers, so ES should be preserved on the task's own stack around the yield (DS=0x1000 is an invariant of syscall context but is set defensively to read current_task, matching kbd_getchar/event_get_stub style).
B. Impact scope: NO stock app uses API 10 or 12 (grep across apps/*.asm finds zero INT 0x80 calls with AH=10/12; the lone `mov ah, 10` in apps/clock.asm:336 is a BCD multiply operand). So the claimed match to the existing "apps crash/freeze when launching apps" symptom is NOT supported — that symptom must have another cause. However, API 10 is the documented and RECOMMENDED blocking call in docs/API_REFERENCE.md (APIs 11/12 are marked deprecated with "use event_wait (API 10) instead"), so any third-party/SDK app following the documentation freezes the whole OS the moment it calls API 10 without focus. Severity high for the documented API surface; latent (not triggered by stock apps).

Files: C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm (lines 692-696, 7317, 7321, 10137-10164, 10212-10223, 10310-10314, 14735-14775, 14780-14862, 18794-18831, 1486, 1651, 1853, 1990, 20352); C:\Users\arin\Documents\Github\unodos\docs\API_REFERENCE.md (lines 172-193).
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, replace both spin loops. The current_task==0xFF guard is REQUIRED: these routines are called from kernel boot context (keyboard_demo fallback at 1990, test_filesystem at 1486, test_app_loader at 1651) where app_yield_stub would never return (scheduler_next finds no RUNNING task, yield loops in .idle forever).

Replace lines 10310-10314:

event_wait_stub:
    call event_get_stub
    test al, al
    jnz .ew_done                    ; Got event - return it
    push ds
    push es                         ; pusha in yield doesn't cover segregs
    push bx
    mov bx, 0x1000
    mov ds, bx
    cmp byte [current_task], 0xFF   ; Kernel/boot context (no task)?
    je .ew_skip                     ; Yes - plain poll; yield would never return
    call app_yield_stub             ; Let other tasks run (incl. focused task)
.ew_skip:
    pop bx
    pop es
    pop ds
    jmp event_wait_stub
.ew_done:
    ret

Replace lines 692-696:

kbd_wait_key:
    call kbd_getchar
    test al, al
    jnz .kw_done
    push ds
    push es
    push bx
    mov bx, 0x1000
    mov ds, bx
    cmp byte [current_task], 0xFF
    je .kw_skip
    call app_yield_stub
.kw_skip:
    pop bx
    pop es
    pop ds
    jmp kbd_wait_key
.kw_done:
    ret

Notes: flags from the cmp survive the pops (pop does not modify flags). All added instructions are 8086-safe. event_wait_stub's documented contract (preserves BX/CX/SI/DI) is kept: app_yield_stub does pusha/popa, and DS/ES/BX are saved on the calling task's own stack, which survives the context switch. This mirrors the proven delay_ticks pattern (API 73, line 18812). Caution: the kernel file uses `times 0x3320 - ($-$$) db 0` padding before the API table (line 7292) — kbd_wait_key at line 692 is before it, so the added ~16 bytes are absorbed by the padding only if pre-table code still fits; verify the build assembles (nasm will error on a negative times value if not).

## [high/high] Single global event-queue head causes head-of-line blocking: one task's pending event stalls input for all tasks
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm:10137-10210 — only-head read at 10138-10149; un-advanced-head bail-outs at 10161 (KEY_PRESS) and 10186 (WIN_REDRAW); queue-full drop in post_event at 10111-10114 (not 10104-10105 as originally cited) | area:event system / input
DESC: event_get_stub reads only the event at the global head (10129-10140). If that event is a KEY_PRESS for the focused task (10142-10152, 'leave event in queue for correct task') or a WIN_REDRAW for another task's window (10172-10177), the head is NOT advanced and every other task gets 'no event' even though events destined for them sit behind it in the queue. All input delivery for the whole OS is serialized on the focused/owning task polling promptly; if that app is in a long operation (disk write in mkboot, game logic frame) or stops polling, keyboard and mouse events for the launcher and every other app stall, and the 31-slot queue then fills and drops new events (10104-10105). This matches the reported 'keyboard and mouse input issues' when several apps run.
VERIFIER: CONFIRMED (with refinements to line numbers and impact scope).

Mechanism — verified by direct reading of kernel\kernel.asm:
- event_get_stub reads ONLY the event at the global head (10138-10149: head index loaded, event read, "DO NOT advance head yet").
- If that head event is EVENT_KEY_PRESS and the caller is not the focused task, it jumps to .no_event at 10161 WITHOUT advancing the head and WITHOUT scanning further entries.
- If the head event is EVENT_WIN_REDRAW for a visible window owned by another task, same bail-out at 10186.
- The only loop-back path (.evt_discard, 10205-10210) is for invalid/stale events. There is no forward scan: events queued behind a non-matching head are unreachable for the current task even if destined for it.
- post_event (10092-10121) drops new events when the ring is full: the full-check/skip is at 10111-10114 (the finding cited 10104-10105, which is actually the SI=tail*3 computation — minor line-number error). Capacity is 31 (one-slot-empty convention). The IRQ12 handler posts one EVENT_MOUSE per movement packet (1131-1142), so under mouse movement the queue fills in well under a second once the head is blocked, after which all newly posted events — including typed keys — are silently and permanently dropped.

Preconditions verified:
- Multiple concurrent consumers are the normal state: launcher + every app poll API 9 (event_get) in their main loops (apps/launcher.asm:269, clock.asm:225, notepad.asm:263, mkboot.asm:156/553/894, etc.), under the cooperative round-robin scheduler (scheduler_next 14731, app_yield 14777; "Cooperative multitasking scheduler state" at 20351).
- The custom INT 9 handler is installed on ALL boot types (kernel.asm:438-454, use_bios_keyboard forced to 0), so KEY_PRESS events always flow through this single global queue; there is no alternate drain path. clear_kbd_buffer (700-712) uses the same filtered event_get, so it cannot drain another task's key either.

Concrete failure trace: focused app yields but does not poll events for time T (e.g. sleeping in delay_ticks API 73, which yields in a loop at 18794-18812 without polling; or any work loop with interspersed yields). User presses a key -> KEY_PRESS for the focused task parks at head. For all of T, every other task gets AL=0 from event_get: WIN_REDRAW repaints of their windows stall and EVENT_MOUSE deliveries to apps that consume mouse via the queue stall. If the user moves the mouse meanwhile, ~31 events queue behind the key and then everything new is dropped — keystrokes lost. Symmetric case: a WIN_REDRAW for a non-polling owner's visible window blocks KEY_PRESS delivery to the focused task. This matches the reported multi-app keyboard/mouse symptoms.

Two overstatements in the original finding, worth correcting:
1. "keyboard and mouse events for the launcher and every other app stall" — the launcher's desktop mouse input does NOT go through this queue (it polls API_MOUSE_GET_STATE, launcher.asm:178), the cursor is redrawn in the IRQ12 handler itself (kernel.asm:1136), and window dragging is processed by mouse_process_drag called at the top of event_get_stub by ANY task (10135), before the head check. So the cursor keeps moving and desktop/window-drag interactions keep working during a head block; the stall is limited to queue-delivered events (KEY_PRESS, EVENT_MOUSE consumers, WIN_REDRAW).
2. The "disk write in mkboot" example is weak: mkboot's sector-write loop (apps/mkboot.asm:285-298) never yields, so under cooperative scheduling no other task runs during the write at all — the scheduler, not the event queue, is the gating factor in that exact scenario. The queue-specific bug requires the focused/owner task to be yielding-but-not-polling.

Two adjacent defects observed while verifying (same root design):
- Stale-key misdelivery: the KEY_PRESS filter checks focused_task at POLL time, so if the user refocuses another window while a key is parked, the new focused task consumes a keystroke typed at the old app. This is also the only "unblock" escape hatch.
- Latent hard-hang: event_wait_stub (10307-10314, exposed as API 10 at 7317) busy-loops event_get with no yield; if any app ever calls it while unfocused with a focused-task key at head, the cooperative system locks up permanently. No shipped app currently uses API 10, so this is latent.

Severity: high is defensible — it silently drops keyboard input and stalls redraw/mouse-event delivery whenever >=2 tasks run and the focused task polls slowly, which is the routine multi-app state.
FIX: Make event_get_stub scan forward from head to tail instead of stopping at the head, tombstoning mid-queue consumed slots and lazily advancing the head past tombstones. 8086-safe; IRQ-safe because post_event (IRQ context) only writes at tail/advances tail, while the reader only writes type bytes inside [head,tail) and advances head.

1) Add a constant next to the event types (near kernel.asm:10083):
   EVENT_CONSUMED      equ 0xFF        ; Tombstone: slot consumed mid-queue

2) Replace the body between .evt_check_next and .no_event (10137-10210) with a scan loop. BX becomes the scan index (no longer always the head):

.evt_check_next:
    mov bx, [event_queue_head]
.evt_scan:
    cmp bx, [event_queue_tail]
    je .no_event
    mov si, bx
    add si, bx
    add si, bx                      ; SI = index * 3
    mov al, [event_queue + si]      ; type
    mov dx, [event_queue + si + 1]  ; data
    cmp al, EVENT_CONSUMED
    je .evt_tombstone
    ; --- existing KEY_PRESS focus filter, but bail to .evt_skip ---
    cmp al, EVENT_KEY_PRESS
    jne .evt_not_key
    push ax
    mov al, [focused_task]
    cmp al, 0xFF
    je .evt_focus_ok
    cmp al, [current_task]
    je .evt_focus_ok
    pop ax
    jmp .evt_skip                   ; not ours: step over, do NOT stop
.evt_focus_ok:
    pop ax
    jmp .evt_consume
.evt_not_key:
    ; --- existing WIN_REDRAW filter, but bail to .evt_skip ---
    cmp al, EVENT_WIN_REDRAW
    jne .evt_consume
    cmp dl, WIN_MAX_COUNT
    jae .evt_discard                ; invalid: tombstone and continue
    push si
    push ax
    xor ah, ah
    mov al, dl
    mov si, ax
    shl si, 5
    add si, window_table
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .evt_discard_pop
    mov al, [si + WIN_OFF_OWNER]
    cmp al, [current_task]
    pop ax
    pop si
    je .evt_consume
    jmp .evt_skip                   ; other task's window: step over

.evt_skip:                          ; leave slot intact, advance scan index only
    inc bx
    and bx, 0x1F
    jmp .evt_scan

.evt_tombstone:                     ; consumed slot: reclaim if at head, else step over
    cmp bx, [event_queue_head]
    jne .evt_skip
    inc bx
    and bx, 0x1F
    mov [event_queue_head], bx      ; lazy head advance reclaims slot
    jmp .evt_scan

.evt_consume:                       ; deliverable event found at index BX
    cmp bx, [event_queue_head]
    jne .evt_mark_mid
    inc bx
    and bx, 0x1F
    mov [event_queue_head], bx      ; at head: advance directly (fast path)
    jmp .evt_return
.evt_mark_mid:
    mov byte [event_queue + si], EVENT_CONSUMED   ; mid-queue: tombstone
    jmp .evt_return

.evt_discard_pop:
    pop ax
    pop si
.evt_discard:                       ; invalid/stale: same as consume but loop for next
    cmp bx, [event_queue_head]
    jne .evt_discard_mid
    inc bx
    and bx, 0x1F
    mov [event_queue_head], bx
    jmp .evt_scan
.evt_discard_mid:
    mov byte [event_queue + si], EVENT_CONSUMED
    jmp .evt_skip

(.evt_return and .no_event remain as-is.) AL/DX already hold the event on the .evt_return path, exactly as before; register preservation (BX/SI/DS pushed at entry) is unchanged. Worst case the head stays parked behind one undeliverable event and tombstones accumulate until that task polls, but no other task is blocked anymore, which removes the head-of-line stall and the overflow-drop cascade. Follow-up (separate, lower priority): event_wait_stub at 10307-10314 should yield between polls before API 10 is ever used by an app, and the poll-time focus check misdelivers stale keys after refocus — a fix could stamp the focused task id into a spare byte at post time, but the queue entry is only 3 bytes, so that requires widening the entry and is beyond this minimal change.

## [high/high] Z-order values drift to 0 and collide: create/focus demote every window but destroy never renormalizes
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm — create demote: 15199-15209 + 15223; focus demote: 17804-17821 + 17835; destroy missing renormalization: 16428-16470 (promote-scan tie at 16447-16449, re-demoting focus call at 16470); divergent consumers: 3865-3873, 3976-3984, 16150-16266 | area:window manager / z-order bookkeeping
DESC: Z-order is stored per-window (WIN_OFF_ZORDER, 0=bottom..15=top), not as a compacted array. win_create_stub demotes EVERY visible window by 1 ('.demote_loop: ... cmp byte [si + WIN_OFF_ZORDER], 0 / je .demote_next / dec byte [si + WIN_OFF_ZORDER]', lines 15199-15209) before setting the new window to 15. win_destroy_stub never restores those levels; instead its promote path calls win_focus_stub (line 16470), which demotes every other visible window AGAIN (lines 17808-17821) before topping the survivor. Net effect: every create+destroy cycle permanently subtracts 2 z-levels from every background window. The demote loop clamps at 0, so after enough cycles (e.g. ~7 launches/closes with a window created at z=13) multiple windows collide at z=0 and their relative order is irreversibly lost: hit-testing (mouse_hittest_titlebar lines 3865-3873 keeps the FIRST equal-z slot) and painting (redraw_affected_windows draws equal-z windows in slot order, lines 16154-16266) then disagree about which window is on top, and win_destroy's promote scan ('cmp al, [.best_z] / jb .promote_skip', 16447-16449) picks the LAST equal-z slot. Explains user-visible wrong stacking, clicks landing on the wrong window, and 'visual anomalies' after repeatedly launching/closing apps.
VERIFIER: CONFIRMED by direct code reading; the finding is accurate and actually slightly understates the bug.

Verified facts (C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm):
1. The ONLY writes to WIN_OFF_ZORDER in the entire repo are: win_create_stub's demote loop (dec with clamp-at-0, lines 15199-15209) + set-new-to-15 (line 15223), and win_focus_stub's demote loop (dec with clamp-at-0, lines 17811-17821) + set-raised-to-15 (line 17835). win_destroy_stub (16378-16498) frees the slot (16428) and NEVER adjusts surviving windows' z; there is no renormalization/compaction mechanism anywhere (grep over the whole repo; WIN_STATE_HIDDEN is defined at 20377 but never used, so no hide/show path either). No bounds check or caller contract prevents the drift — it is purely algorithmic, no concurrency needed.

2. Drift trace, concrete: create A,B,C -> z = 13,14,15. Each "create D, destroy D" cycle: create demotes A,B,C by 1 (D=15); destroy's promote path (16437-16455) finds C and calls win_focus_stub (16470), whose demote loop demotes A,B by 1 AGAIN before topping C. Net -2 per cycle for every background window, exactly as claimed. After 7 cycles A(13) and B(14) both clamp to 0 and collide; their relative order is unrecoverable. ADDITIONAL leak the finding missed: merely focusing a middle window also leaks one level for every window BELOW it ({13,14,15}, focus the 14 -> {12,14,15}), so drift occurs from ordinary click-to-focus too (mouse drag-focus path at 4332-4343 goes through the same win_focus_stub).

3. Consequences of z-ties, all verified: hit-tests keep the FIRST equal-z slot (titlebar test 3865-3873 and resize-corner test 3976-3984 both use 'cmp al, bl / jbe' -> strict greater required to replace best); redraw_affected_windows (16150-16266) draws each z pass in slot order so the LAST equal-z slot paints on top — paint and hit-test therefore disagree about which overlapping window is on top whenever two windows share a z; win_destroy's promote scan tie-breaks to the LAST equal-z slot ('cmp al,[.best_z] / jb .promote_skip' at 16447-16449, >= wins). The z==15 'topmost' convention itself (draw gate at line 245, drag check at 4338, active-title style at 17531) is preserved by the bug, so corruption is confined to background stacking/click routing — matching the reported symptoms (wrong stacking, clicks landing on the wrong window after repeated app launch/close).

Refined fix rationale: the system's intended invariant is 'visible z values are a dense block ending at 15' ({16-N..15} for N visible windows). win_create's demote-all is actually CORRECT under that invariant (min z = 16-N >= 1 whenever a free slot exists, so its clamp never fires) and needs no change. The two broken operations are win_focus (demotes windows BELOW the raised one, leaking levels) and win_destroy (leaves a gap at the destroyed z, then leaks again via focus). Fix: (a) in win_focus_stub demote only windows with z GREATER than the raised window's old z — windows above shift down to fill its old level, no clamp needed; (b) in win_destroy_stub, before redraw/promote, INCREMENT z of every visible window BELOW the destroyed window's z — this closes the gap from below, keeps the surviving topmost at z=15 so the subsequent win_focus_stub call takes its existing '.already_top' early-out (line 17805-17806) and performs no demote at all. Inductively all four paths then preserve the dense-distinct invariant; ties become impossible from a clean boot. Severity 'high' is fair for a window manager: reachable through normal API use (INT 0x80 fn 23 dispatch at 7338, mouse focus, app open/close), user-visible, and irreversible without reboot.
FIX: Two changes; win_create_stub needs NO change (its demote-all is correct once the invariant below holds and its clamp then never fires).

FIX 1 — kernel/kernel.asm, win_focus_stub: replace lines 17808-17821 (the demote loop) with a loop that only demotes windows stacked ABOVE the raised window's old z:

    ; Demote only windows ABOVE this one (z > old z); demoting windows
    ; below it leaked z-levels until everything collided at z=0.
    push dx
    mov dl, [bx + WIN_OFF_ZORDER]   ; DL = raised window's old z
    mov si, window_table
    mov cx, WIN_MAX_COUNT
.demote_loop:
    cmp si, bx                      ; Skip our own entry
    je .demote_next
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .demote_next
    cmp [si + WIN_OFF_ZORDER], dl
    jbe .demote_next                ; z <= old z: leave it alone
    dec byte [si + WIN_OFF_ZORDER]  ; min result = old z, cannot underflow
.demote_next:
    add si, WIN_ENTRY_SIZE
    loop .demote_loop
    pop dx

(DX is push/popped because win_focus_stub currently preserves it; BX still points at the raised window's entry and DS=0x1000 here. All instructions 8086-safe.)

FIX 2 — kernel/kernel.asm, win_destroy_stub: insert after line 16432 ('mov byte [topmost_handle], 0xFF'), before 'call redraw_affected_windows':

    ; Close the z-order gap left by the destroyed window: every visible
    ; window BELOW it moves up one level. Keeps z a dense block ending at
    ; 15, so the surviving topmost stays z=15 and the promote path's
    ; win_focus_stub call hits .already_top (no re-demotion).
    mov al, [bx + WIN_OFF_ZORDER]   ; destroyed window's z (entry not wiped)
    mov si, window_table
    mov cx, WIN_MAX_COUNT
.znorm_loop:
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .znorm_next
    cmp [si + WIN_OFF_ZORDER], al
    jae .znorm_next                 ; only windows below the gap move up
    inc byte [si + WIN_OFF_ZORDER]  ; max result = destroyed z <= 15
.znorm_next:
    add si, WIN_ENTRY_SIZE
    loop .znorm_loop

(Safe at this point: AX is dead until the promote scan reloads AL at 16447 and redraw_affected_windows preserves AX; CX/SI were already saved into redraw_old_* at 16408-16415 and are reinitialized by the promote scan; BX still points at the destroyed entry, whose Z byte is intact since only the state byte was cleared. Doing it before redraw_affected_windows is correct — relative order of survivors is unchanged by the monotone shift. All instructions 8086-safe.)

Resulting invariant, preserved by all four paths from boot: visible windows hold distinct z values {16-N..15}; create's demote-all never clamps (min z >= 1 whenever a free slot exists), ties can no longer form, and hit-test/paint/promote agree.

## [high/high] win_resize_stub repaints only the desktop, never underlying windows, when a window shrinks
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:    call redraw_affected_windows    ; Repaints desktop region, overlapped frames, posts WIN_REDRAW | area:window manager / redraw on resize
DESC: win_resize_stub stores the old rect into redraw_old_* (19250-19258) but then calls 'call draw_desktop_region / call win_draw_stub' (19292-19293) instead of redraw_affected_windows. draw_desktop_region only paints desktop background + icons (15761-15910); it does not redraw frames/content of other windows overlapping the old rect and posts no WIN_REDRAW to them. Shrinking a window that overlapped another window erases the underlying window: the revealed strip shows desktop color and the underlying window's frame stays erased until something else forces a repaint. Direct cause of 'visual anomalies' (ghost-erased windows) after resizing. Compare with win_move_stub which correctly calls redraw_affected_windows (line 18063).
VERIFIER: Confirmed by direct code reading. win_resize_stub (kernel\kernel.asm 19230-19332) saves the old rect into redraw_old_x/y/w/h (19251-19258), writes the new width/height (19262-19263), then at 19292-19293 calls draw_desktop_region + win_draw_stub. draw_desktop_region (15761-15910) only fills the old rect with desktop_bg_color and redraws overlapping desktop icons — it never touches the window table; win_draw_stub (17458+) draws only the resized window's own chrome. So when a window shrinks while overlapping another window, the entire old rect (including the other window's overlap region) is blasted to desktop color, the resized window's smaller frame is redrawn, and the revealed strip is left showing desktop background; the underlying (or overlying, higher-z) window gets neither a frame redraw nor an EVENT_WIN_REDRAW, so the corruption persists until an unrelated action repaints it. No compensating mechanism exists: the only callers are the interactive resize-finish handler (4607-4614, which afterwards only updates the topmost cache) and API 78 via the dispatch table (7425). The sibling operations win_move_stub (18053-18063) and win_destroy_stub (16407-16435) both call redraw_affected_windows (16127-16365), which paints the desktop region, redraws overlapped frames in z-order with clipped content clears, posts EVENT_WIN_REDRAW per affected window, and handles the z=15 topmost window — win_resize_stub is the lone outlier. The suggested fix is register-safe as a drop-in: at 19292 the live state is DI (window entry pointer, used at 19298+) and AX (pushed at 19291); redraw_affected_windows preserves AX/BX/CX/DX/SI/DI/BP and saves/restores clip_enabled. It also reproduces the proven win_move_stub pattern (redraw_affected_windows then win_draw_stub of the acted-on window on top, 18063→18068), and additionally fixes a latent clipping hazard on the API-78 path: redraw_affected_windows disables clipping internally (16141-16142), whereas the bare draw_desktop_region call runs with the calling app's clip rect active (the interactive path clears clip_enabled at 4612-4613, the API path does not). One nuance: redraw_affected_windows skips the desktop fill when desktop_icon_count==0 (16145-16146), so in a no-desktop state the strip would not be bg-filled — but win_move_stub and win_destroy_stub already follow this exact convention, so the swap makes resize consistent rather than regressive. Severity 'high' is fair: every shrink of an overlapping window via mouse drag or API 78 visibly corrupts other windows.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, line 19292, inside win_resize_stub, replace:

    ; Redraw the background where old window was, then redraw frame
    push ax
    call draw_desktop_region
    call win_draw_stub              ; Redraw frame with new size

with:

    ; Redraw desktop + overlapped windows where old rect was, then redraw frame
    push ax
    call redraw_affected_windows    ; Repaints desktop region, overlapped frames, posts WIN_REDRAW
    call win_draw_stub              ; Redraw this window's frame with new size on top

redraw_old_x/y/w/h are already populated (19251-19258). redraw_affected_windows preserves all registers (AX/BX/CX/DX/SI/DI/BP) and clip_enabled, so DI (window entry pointer used at 19298+) and the pushed AX survive; this mirrors the working win_move_stub sequence at lines 18063-18068. 8086-safe (plain near call, no new instructions).

## [high/high] Focus-filtered KEY_PRESS left at queue head blocks ALL events for every other task (head-of-line blocking)
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm 10128-10201: head-only examination (10129-10140); KEY_PRESS leave-at-head at 10152; WIN_REDRAW leave-at-head at 10177; overflow drop at 10104-10105 | area:event queue
DESC: event_get_stub reads only the head event. For KEY_PRESS not belonging to the focused task it does 'jmp .no_event ; Not focused - leave event in queue for correct task' WITHOUT advancing head. The same is done for WIN_REDRAW of another task's window (line 10177). Because only the head element is ever examined, a single key event destined for a focused task that is slow or not polling makes every other task's event_get return 'no event' forever: mouse events, redraw events, everything queued behind it is unreachable. The 31-slot queue then fills (mouse generates ~40-100 EVENT_MOUSE/s while moving) and post_event silently drops all new events (line 10104-10105 'je .done ; Skip if full'). Net effect: all input appears dead until the focused task polls. Strong match for 'keyboard and mouse input issues' and apparent hangs when several apps are running.
VERIFIER: CONFIRMED, with impact refinements.

Mechanism (verified in kernel/kernel.asm):
1. event_get_stub (10117) examines ONLY the head slot (10129-10140). The KEY_PRESS focus filter (10143-10152) jumps to .no_event at 10152 WITHOUT advancing event_queue_head when focused_task != 0xFF and != current_task. The WIN_REDRAW filter does the same at 10177 for another task's visible window. Nothing ever scans past the head, so one undeliverable head event makes event_get return "no event" to every other task no matter what is queued behind it. There is no other drain mechanism: the only queue consumers are event_get_stub itself, event_wait_stub (10301, just loops on it), the modal file dialogs (5189/5873), and clear_kbd_buffer (708, called only at 1483/1648 during init paths).
2. The queue is a single shared 32-slot (31 usable) ring. post_event (10083) silently drops the newest event when full (10104-10105). EVENT_MOUSE is posted on every mouse packet from both IRQ12 paths (1142 and 1274), so with the head plugged the ring fills in well under a second of mouse movement, after which new KEY_PRESS and WIN_REDRAW events are permanently lost.
3. The plug arises easily: INT 9 posts a KEY_PRESS for every keystroke (624) in addition to the kbd ring buffer, and kbd_getchar (664) does NOT remove the event-queue copy. So any keystroke while a window is focused plugs the head until the focused task itself calls event_get.

Impact refinements (where the original finding overstates):
- "All input appears dead" is too strong for the bundled apps. Launcher and notepad read mouse position/buttons via API_MOUSE_GET_STATE (direct state polling, launcher.asm 178-183), not via EVENT_MOUSE, and window drag/focus is handled by mouse_process_drag called unconditionally at event_get entry (10126) before the queue check. So cursor movement, desktop clicks, and window drags keep working even with a plugged queue.
- The blockage is normally transient, not "forever": every bundled app polls event_get once per main-loop iteration and yields (e.g., launcher.asm 126-129, 269), so the focused task unplugs the head within one round-robin cycle. "Forever" requires a focused task that stops polling; realistic windows are games sleeping in delay_ticks between frames (~50-200 ms) and long synchronous operations (mkboot floppy writes: seconds).
- The real persistent damage is event LOSS, not just latency: while plugged, mouse packets fill the 31-slot ring and post_event then drops subsequent KEY_PRESS and WIN_REDRAW events outright - lost keystrokes and windows that never repaint. This matches the reported "keyboard and mouse input issues" / apparent unresponsiveness with several apps running.
- Latent hard-hang variant: event_wait_stub (10301-10305) busy-loops without yielding; a NON-focused task calling API 10 while a focused-task key sits at head would deadlock the whole system (the only task able to consume the head never runs). No bundled app currently uses API 10, so this is latent, but the fix below removes it too.

Severity: high is justified for the keystroke/redraw loss via overflow and the cross-task event starvation; the mouse-input-death portion of the claim is largely mitigated by the direct mouse polling in the bundled apps.
FIX: Minimal 8086-safe fix: scan past undeliverable events instead of stopping at the head, using the existing EVENT_NONE (=0) type byte as an in-place "consumed" tombstone. No event-layout change, no per-task queues. IRQ-safe because post_event (from IRQ context) only writes the slot AT tail, never slots in [head, tail); head update and tombstone write are single mov instructions.

Replace the block at kernel/kernel.asm lines 10128-10201 (.evt_check_next through .evt_discard) with:

```nasm
.evt_check_next:
    mov bx, [event_queue_head]
.evt_scan:                          ; BX = scan cursor (starts at head)
    cmp bx, [event_queue_tail]
    je .no_event                    ; Scanned everything - nothing deliverable

    ; Calculate slot position (events are 3 bytes each)
    mov si, bx
    add si, bx                      ; SI = cursor * 2
    add si, bx                      ; SI = cursor * 3

    mov al, [event_queue + si]      ; type
    mov dx, [event_queue + si + 1]  ; data (word)

    cmp al, EVENT_NONE              ; Tombstone (already consumed)?
    jne .evt_live
    ; Reclaim tombstone if it is at the head, else just step over it
    cmp bx, [event_queue_head]
    jne .evt_skip
    inc bx
    and bx, 0x1F
    mov [event_queue_head], bx      ; Single mov - atomic vs IRQ post_event
    jmp .evt_scan
.evt_skip:
    inc bx
    and bx, 0x1F
    jmp .evt_scan                   ; Keep scanning - do NOT return no_event

.evt_live:
    ; Filter keyboard events: only deliver to focused task
    cmp al, EVENT_KEY_PRESS
    jne .evt_not_key
    push ax
    mov al, [focused_task]
    cmp al, 0xFF                    ; No window focused? Deliver to current task
    je .evt_focus_ok
    cmp al, [current_task]
    je .evt_focus_ok
    pop ax
    jmp .evt_skip                   ; Not focused - skip it, keep scanning behind it
.evt_focus_ok:
    pop ax
    jmp .evt_consume

.evt_not_key:
    ; Filter: skip WIN_REDRAW events not for current task's window
    cmp al, EVENT_WIN_REDRAW
    jne .evt_consume                ; Other event types: consume and pass through
    cmp dl, WIN_MAX_COUNT
    jae .evt_discard                ; Invalid window handle: consume garbage and retry
    push si
    push ax
    xor ah, ah
    mov al, dl
    mov si, ax
    shl si, 5
    add si, window_table
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .evt_discard_pop            ; Window freed/destroyed: discard stale event
    mov al, [si + WIN_OFF_OWNER]
    cmp al, [current_task]
    pop ax
    pop si
    je .evt_consume                 ; Window belongs to current task
    jmp .evt_skip                   ; Wrong task's window - skip it, keep scanning

.evt_consume:
    ; Deliver this event. If it is at the head, advance head; otherwise
    ; tombstone the slot in place (head reclaims it later).
    cmp bx, [event_queue_head]
    jne .evt_tombstone
    inc bx
    and bx, 0x1F
    mov [event_queue_head], bx
    jmp .evt_return
.evt_tombstone:
    mov byte [event_queue + si], EVENT_NONE  ; SI still = cursor*3 here
    ; fall through to .evt_return

.evt_return:
    clc                             ; CF=0 = event available
    pop ds
    pop si
    pop bx
    ret

.evt_discard_pop:
    pop ax
    pop si
    ; fall through to evt_discard
.evt_discard:
    ; Consume invalid/stale event (head-advance or tombstone) and keep scanning
    cmp bx, [event_queue_head]
    jne .evt_discard_tomb
    inc bx
    and bx, 0x1F
    mov [event_queue_head], bx
    jmp .evt_scan
.evt_discard_tomb:
    mov byte [event_queue + si], EVENT_NONE
    inc bx
    and bx, 0x1F
    jmp .evt_scan
```

Properties: per-task event ordering is preserved (no rotation/reordering); other tasks' events become reachable immediately, fixing the starvation, the overflow-drop cascade, and the latent event_wait deadlock. Remaining bounded limitation: a key for a never-polling focused task still pins its one slot (plus tombstones ahead of it) until consumed, so capacity can still degrade in the pathological case - but delivery to all tasks keeps working, which is the actual bug. Scan terminates: cursor advances one slot per iteration toward a 31-slot-max tail.

Verification after applying: build, boot in QEMU, open notepad (focused) plus clock; type while moving the mouse and confirm clock keeps receiving WIN_REDRAW (second hand updates) and launcher still reacts, with keystrokes still going only to notepad.

## [high/high] event_wait_stub and kbd_wait_key busy-loop without yielding - freezes the cooperative scheduler
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:event_wait_stub busy loop: lines 10301-10305; kbd_wait_key busy loop: lines 692-696; API table entries: 7317 (API 10), 7321 (API 12); only task-switch point: app_yield_stub line 14771; correct pattern: delay_ticks line 18803 | area:event queue / scheduler
DESC: event_wait_stub (exposed as API 10, line 7317) is 'call event_get_stub / test al, al / jz event_wait_stub / ret' - a hard busy loop with no call to app_yield_stub. kbd_wait_key (API 12, lines 690-696) is identical. The OS is cooperative round-robin (task switch only happens in app_yield_stub, line 14771), so any app calling API 10/12 stops every other task permanently. Combined with the focus filter (finding above): if the waiting task is not focused, its key events are never deliverable, and the focused task never runs to consume them - a guaranteed, permanent system hang. delay_ticks (line 18801-18803) shows the correct pattern: it calls app_yield_stub inside its wait loop.
VERIFIER: CONFIRMED, with two refinements (one weakens the worst-case claim, one shows the suggested fix is unsafe as written).

Verified facts (C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm):
1. event_wait_stub (lines 10301-10305) is exactly 'call event_get_stub / test al,al / jz event_wait_stub / ret' - no yield. kbd_wait_key (lines 692-696) is the identical pattern around kbd_getchar. Both are app-callable: API table entries at line 7317 (API 10) and 7321 (API 12).
2. The scheduler is purely cooperative. scheduler_next is reached only from app_yield_stub (line 14802), initial boot handoff (line 1786), and app_exit (line 15003). There is no timer-IRQ preemption; the comment at line 20056 ('before preemptive scheduling') confirms cooperative-only design. So while any task spins in API 10/12, no other task ever runs - all other apps freeze. delay_ticks (line 18803) indeed shows the correct in-kernel pattern: call app_yield_stub inside the wait loop.
3. The INT 0x80 dispatcher does sti at line 151, so IRQ1/IRQ12 still fire during the spin: INT 9 fills kbd_buffer (line 610-616, no focus check) and posts EVENT_KEY_PRESS to the event queue (line 624).

Refinement A - 'guaranteed, permanent hang' is overstated for most cases, but real for one:
- kbd_wait_key is NOT subject to the focus filter (kbd_getchar at 664 reads kbd_buffer directly), so it unblocks on the next keypress regardless of focus - but until then every other task is frozen, and it steals the keystroke from the focused app.
- event_wait_stub in an unfocused task: KEY_PRESS events are left in the queue by the focus filter (lines 10142-10152), so keys cannot unblock it. However, event_get_stub calls mouse_process_drag every iteration (line 10126), which processes IRQ12-deferred focus clicks (drag_needs_focus -> win_focus_stub, lines 4315-4343). So the user CAN rescue the system by clicking the spinning task's window (it gains focus, next keypress unblocks). The hang is truly permanent only if the waiting task owns no visible/clickable window. Either way, all other tasks are frozen for the entire wait - the high-severity scheduler-stall core of the finding stands.

Refinement B - the suggested fix is broken as written: kbd_wait_key is also called from kernel/boot context with current_task=0xFF and zero RUNNING tasks - the launcher-failure fallback keyboard_demo (line 1853 -> 1990 uses event_wait_stub) and the disk-swap prompts (lines 1486, 1651). In that state app_yield_stub's scheduler_next returns 0xFF (.none_found returns current_task) and app_yield enters .idle (lines 14850-14853: sti/hlt/jmp .no_save) which loops forever and NEVER returns - an unconditional 'call app_yield_stub' would convert these working boot prompts into hard hangs. The yield must be guarded on current_task != 0xFF (see fix).
FIX: Replace event_wait_stub (lines 10301-10305) with:

event_wait_stub:
    call event_get_stub
    test al, al
    jnz .got                          ; Event available - return it
    cmp byte [cs:current_task], 0xFF  ; Kernel/boot context (no task)?
    je event_wait_stub                ;   yes: plain spin (app_yield never returns when no task is RUNNING)
    call app_yield_stub               ; Let other tasks run while waiting
    jmp event_wait_stub
.got:
    ret

Replace kbd_wait_key (lines 692-696) with:

kbd_wait_key:
    call kbd_getchar
    test al, al
    jnz .got
    cmp byte [cs:current_task], 0xFF  ; Boot prompts (1486/1651) run with no task - don't yield there
    je kbd_wait_key
    call app_yield_stub
    jmp kbd_wait_key
.got:
    ret

Notes: the cs: override is deliberate - kernel CS is always 0x1000 (INT 0x80/INT 9 vectors installed with segment 0x1000, line 453), while kbd_getchar's own defensive DS reload shows callers may not guarantee DS=0x1000; app_yield_stub itself requires DS=0x1000 but is only reached via the INT 0x80 dispatch path (which sets DS=0x1000 at lines 156-160) or kernel code, matching how delay_ticks already calls it (line 18803). app_yield_stub preserves all GPRs (pusha/popa) and per-task caller_ds/es/draw_context, so the documented register contracts of both APIs are unaffected.

## [high/high] IRQ12 can redraw the cursor between mouse_cursor_hide and cursor_locked increment (classic XOR droppings race)
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm:192-193 (dispatcher); same unprotected pair at 35 sites total, e.g. 1367-1368, 4469-4470, 7928-7929, 15117-15118, 17469-17470 | area:mouse cursor protection / INT 0x80 dispatcher
DESC: Every cursor-protection bracket uses the sequence 'call mouse_cursor_hide' then 'inc byte [cursor_locked]' (dispatcher lines 191-193; same pattern at ~30 internal sites e.g. 4469-4470, 7928-7929, 15107-15108, 17459-17460). mouse_cursor_hide (3735-3770) does pushf/cli internally but its trailing popf RE-ENABLES interrupts before returning (the dispatcher ran sti at line 151). If IRQ12 fires in the window between hide's popf and the 'inc byte [cursor_locked]', the mouse ISR (lines 1077/1136 and 1220/1268) sees cursor_locked==0, moves the mouse, and REDRAWS the cursor. The mainline then sets the lock and draws with the cursor visible on screen. For the CGA/12h XOR cursor, drawing under the visible cursor then XOR-erasing it leaves inverted blotches; for VGA/VESA save-under cursors the saved background becomes stale, so the next cursor move 'restores' a rectangle of old pixels over the new content. Note the order cannot simply be swapped because mouse_cursor_hide itself skips when cursor_locked>0 (line 3738-3739).
VERIFIER: Race confirmed by direct code reading. The INT 0x80 dispatcher executes sti at kernel\kernel.asm:151, then for drawing APIs runs 'call mouse_cursor_hide' / 'inc byte [cursor_locked]' at lines 192-193 with no cli around the pair. mouse_cursor_hide (3735-3770) uses pushf/cli internally but its popf at line 3769 restores IF=1 before ret, so IRQ12 can be taken between hide's popf/ret and the inc at 193. In that window both mouse paths — the native IRQ12 ISR (hide at 1077, show at 1136) and the BIOS PS/2 callback (hide at 1220, show at 1268) — see cursor_locked==0: hide is a no-op (cursor_visible==0), the ISR updates mouse_x/y, and mouse_cursor_show (3776-3815) passes all guards and draws the cursor, capturing a save-under buffer and setting cursor_visible=1. The mainline then sets the lock and the API draws with the cursor visible. The unlock epilogue (377-392: dec then mouse_cursor_show) does NOT repair this: show skips because cursor_visible==1 (3783-3784), so the stale save-under buffer persists; the next cursor hide restores stale pixels (VGA 0x13/VESA) or XOR-erases over changed content (CGA/Mode12h), producing exactly the claimed artifacts. No other mechanism protects the window: IRQ12 is not masked during INT 0x80 (sti is deliberate for floppy DMA), mouse_drag_update is flags-only, and the inner pushf/cli in hide/show only protects each routine's own body. The same unprotected hide+inc pair exists at 35 sites (multiline grep), e.g. 1367-1368 (win_begin_draw), 4469-4470 (drag outline), 7928-7929 (widget_draw_button), 15117-15118 (win_create_stub), 17469-17470 (win_draw_stub). The finding's notes are also correct: the order cannot be swapped because hide skips when cursor_locked>0 (3738-3739), and the unlock side (dec then show) is race-free — an IRQ between dec and show just draws the cursor itself and the mainline show then correctly skips. Nested brackets (cursor_locked already >0, e.g. inside a win_begin_draw batch) are immune; only the common outermost 0-to-1 transition is exposed. Two line-number corrections: the pair is at 192-193 (191 is a comment), and two internal examples were slightly off (15117-15118 not 15107-15108; 17469-17470 not 17459-17460). Severity caveat: the window is about 2 instruction boundaries per bracket, so this is a rare intermittent visual glitch (XOR droppings / stale save-under rectangle), cosmetic only and self-healing on the next repaint — mechanism is real but 'medium' may be fairer than 'high'. Also note a few inc byte [cursor_locked] sites (e.g. ~15504/15559/15639) are not preceded by a hide call; only the 35 true pairs should be rewritten by the fix.
FIX: Add an 8086-safe helper next to mouse_cursor_hide in kernel\kernel.asm:

; cursor_protect_begin - atomically hide cursor and take the cursor lock.
; Preserves all registers and FLAGS (including IF). 8086-safe.
cursor_protect_begin:
    pushf                           ; save caller IF
    cli                             ; close the hide->lock window
    call mouse_cursor_hide          ; its inner pushf/cli/popf restores IF=0 here, so still atomic
    inc byte [cursor_locked]
    popf                            ; restore caller IF
    ret

Then replace each of the 35 occurrences of the two-line pair
    call mouse_cursor_hide
    inc byte [cursor_locked]
with
    call cursor_protect_begin
(sites found by multiline grep; do NOT touch inc sites without a preceding hide, e.g. ~15504/15559/15639). Minimal single-site variant for the dispatcher only (lines 192-193):
    pushf
    cli
    call mouse_cursor_hide
    inc byte [cursor_locked]
    popf
The unlock side (dec byte [cursor_locked] / call mouse_cursor_show) needs no change.

## [high/high] Default 8x8 font has advance=12, making all default text 50% wider — primary cause of overlapping desktop icon labels at boot
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:19973 'draw_font_advance: db 12' and kernel/kernel.asm:19984 'db 8, 8, 12, 8' (font 1 descriptor) — the finding's cited lines 19963/19974 are off by 10 | area:font system / text rendering
DESC: font_table entry for the default medium font is 'dw font_8x8 / db 8, 8, 12, 8 ; height=8, width=8, advance=12, bpc=8' (line 19974), and the boot-time default 'draw_font_advance: db 12' (line 19963) matches. An 8-pixel-wide glyph advancing 12 px leaves a 4 px gap (the small 4x6 font uses width+2). Desktop math: launcher grid columns are 80 px wide (COL_WIDTH_LO=80, launcher.asm line 60), icons at col*80+32, labels drawn at icon_x-8 = col*80+24. The next column's label starts at col*80+104. With advance=12, a 7-char name (NOTEPAD, TETRISV, PACMANV, SYSINFO...) spans 84 px ending at col*80+108 > 104, and an 8-char name (SETTINGS, OUTLASTV) spans 96 px ending at col*80+120, overdrawing the next cell's icon (which starts at col*80+112). This exactly reproduces the confirmed 'icon labels overlap each other at boot' symptom. With advance=8 the same 8-char label ends at col*80+88 and does not collide.
VERIFIER: Core claim verified by direct code reading, with three corrections. (1) FACTS: kernel/kernel.asm line 19973 has 'draw_font_advance: db 12' and line 19984 has 'db 8, 8, 12, 8' (font 1 descriptor, advance=12) — the finding's line numbers 19963/19974 are off by 10. draw_char (lines 3025-3095) draws the 8px glyph, fills advance-width=4 gap pixels with background, and advances draw_x by [draw_font_advance] (lines 3064, 3091), so default text really is 12px/char (50% wider than glyphs). (2) NO PROTECTING MECHANISM: boot default is current_font=1; new tasks inherit it (line 14930 'mov al,[current_font] / mov [bx+APP_OFF_FONT],al'), and the per-task font restore on every task switch (lines 1798-1800) calls gfx_set_font which reloads advance=12 from the same descriptor (lines 7634-7635). SETTINGS.CFG (load_settings, line 1927) only avoids this if the user previously saved font 0. Both label-drawing paths — launcher draw_single_icon (apps/launcher.asm 1053-1068, label at icon_x-8 via API 4) and the kernel desktop repaint (kernel.asm 15884-15898, same x-8 offset via gfx_draw_string_stub) — draw with the current font; neither switches to the small font. There is no clipping (clip_enabled=0) or truncation of names. (3) GEOMETRY AT BOOT (320x200): get_icon_position puts icons at col*80+32 (launcher.asm 1540-1563, COL_WIDTH_LO=80, ICON_X_OFFSET_LO=32), so labels start at col*80+24, 80px apart, all in the same y band (icon_y+20..+27). The boot floppy (Makefile line 171) ships header names 'Notepad'(7), 'Settings'(8), 'Dostris'(7), 'Tetris VGA'(10), 'OutLast'(7), 'OutLast VGA'(11), 'Pac-Man'(7), 'PacMan VGA'(10): at 12px/char these span 84-132px > 80px pitch, so e.g. desktop row 2 ('Tetris VGA','Notepad','OutLast','OutLast VGA') overlaps at every junction (40px, 4px, 4px), and 'OutLast VGA' in column 3 also runs past x=320. With advance=8 a 10-char name spans exactly 80px and no shipped adjacent pair collides. CORRECTIONS to the finding: (a) labels never 'overdraw the next cell's icon' — icons occupy y=icon_y..+15, labels y=icon_y+20..+27, vertically disjoint; the collision is label-on-label (and screen-edge overflow), same visible symptom, wrong geometric detail; (b) the suggested alternative 'or 9 for a 1px gap' is NOT equivalent — 10-char names at 9px/char = 90px > 80px pitch, reintroducing overlap; only advance=8 fits the shipped names; (c) line numbers corrected to 19973/19984. CONSISTENCY CHECK of the fix: gfx_text_width and all kernel consumers read [draw_font_advance] dynamically (lines 6324, 6605, 6701, 7577, etc.), checkbox/radio code saves/restores it (8106-8123, 8188-8205, 8922-8950), and shipped apps query metrics at runtime via API_GET_FONT_INFO (e.g. apps/browser.asm 99-116) — no hardcoded 12px/char layout found, so the data-only fix is safe. One intent caveat: CHANGELOG Build 293 deliberately aligned gfx_text_width to 12 ('draw_char advances 12px... Fixed to return 12px') rather than fixing the advance, so 12 is long-standing canon; changing it visibly densifies ALL default-font text system-wide, not just labels. Severity is cosmetic-but-prominent (every boot), not a stability issue.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, two data-only edits (8086-safe, no code changes):

Line 19984 (font 1 descriptor in font_table):
-    db 8, 8, 12, 8                    ; height=8, width=8, advance=12, bpc=8
+    db 8, 8, 8, 8                     ; height=8, width=8, advance=8, bpc=8

Line 19973 (boot-time default, must match descriptor):
-draw_font_advance: db 12             ; Pixels to advance per character
+draw_font_advance: db 8              ; Pixels to advance per character

Use advance=8, NOT 9: the 80px desktop column pitch holds exactly ten 8px cells, and the boot disk ships 10-char names ('Tetris VGA', 'PacMan VGA'); 9px/char (90px) would still overlap. All consumers (draw_char gap fill at lines 3061-3083, advance at 3091, gfx_text_width at 6324/6605/6701, widget code that saves/restores advance, and apps via API_GET_FONT_INFO) read the descriptor/variable dynamically, so they stay consistent automatically. Note this densifies all default-font text system-wide (intended), and 11+ char icon labels (e.g. 'OutLast VGA', 88px) can still abut/overflow in non-last columns — full-proof label rendering would additionally need truncation or the 4x6 font in the launcher/kernel label paths, but that is beyond this minimal fix.

## [high/high] Per-character clipping lets glyphs bleed up to 7px past the window's right/bottom borders; char and wrap APIs ignore the clip rect entirely
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:7546-7561 (string clip block); also 4773-4785 (inverted), 3025-3095 (draw_char, no clip), 4694-4745 (draw_char_inverted, no clip), 7497-7519 (char stub, no clip), 7741-7826 (wrap, no clip), 3132-3136/3198-3202/7680-7684 (screen-only pixel clamps) | area:text clipping (gfx_draw_string_stub / draw_char / wrap)
DESC: gfx_draw_string_stub clips only on the glyph ORIGIN: 'cmp di,[clip_y2] / ja .clip_exit' and 'cmp di,[clip_x2] / ja .skip_char' (7548-7555). A char whose origin is exactly at clip_x2/clip_y2 is drawn in full: draw_char (3025-3095) has no clip checks and plot_pixel_white/black/color only clamp to the SCREEN (3133-3136, 7681-7684), so glyphs paint up to font_width-1 = 7 px past the window's right border and font_height-1 px below the bottom border, onto the desktop or windows beneath. gfx_draw_string_inverted has the same origin-only logic (4772-4785). Worse, gfx_draw_char_stub (7497-7519) performs no clip checks at all, and gfx_draw_string_wrap (7741-7826) contains zero clip logic, so wrapped text can paint anywhere on screen even with an active window clip. There is also no clip_y1 (top) check in any of the string paths. This is a direct 'text bleeds outside windows' visual-anomaly mechanism.
VERIFIER: CONFIRMED by direct code reading of C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. (1) gfx_draw_string_stub's clip block (7546-7561) tests only the glyph ORIGIN: 'cmp di,[clip_y2]/ja .clip_exit' and 'cmp di,[clip_x2]/ja .skip_char' use JA (strictly above), so a char with draw_x==clip_x2 or draw_y==clip_y2 is drawn in full. (2) draw_char (3025-3095) and draw_char_inverted (4694-4745) contain zero clip references and plot the entire width x height glyph cell plus gap-fill columns. (3) plot_pixel_white (3132-3136), plot_pixel_black (3198-3202), and plot_pixel_color (7680-7684) clamp only against screen_width/screen_height. (4) gfx_draw_char_stub (7497-7519, API 3) has no clip logic; gfx_draw_string_wrap (7741-7826, API 50) has no clip logic and advances draw_y unboundedly per wrapped line. (5) gfx_draw_string_inverted has the identical origin-only logic at 4773-4785. (6) No clip_y1 check exists in any string path (only y2), and additionally — beyond the original finding — the left test (7557-7561) skips only chars ENTIRELY left of clip_x1, so a char straddling the left edge also draws fully, bleeding up to 7px left. No hidden mitigation exists: a grep of all clip_x1/x2/y1/y2 references shows only the two string functions read the clip rect; the INT 0x80 draw-context translation (lines 196-251) purely adds the window origin and its z-order gate only suppresses NON-topmost windows, so the topmost window bleeds onto the desktop/windows beneath. Concretely reachable in normal kernel UI use: the button widget sets clip to the exact button rect (8030-8045) with the comment 'Set clip bounds to button rect so text doesn't overflow' — proving intent — and the textfield widget (8733-8769) draws horizontally scrolled text (sub bx,[wgt_scroll_off]) under a clip set to the field interior, so edge characters bleed up to 7px over the field borders during typing. Via win_begin_draw (clip_x2=win_x+win_w-2, clip_y2=win_y+win_h-2, lines 1378-1396), window text bleeds over the 1px right border plus up to 6px onto the desktop, and up to font_height-1 = 11px (8x12 font) below the bottom border. One correction to the suggested remedy: enforcing clip inside plot_pixel_white/black/color is the RISKIER fix — multiple chrome-drawing paths (4328-4330, 4612-4613, 4963-4969, 16147-16152) explicitly clear clip_enabled only around STRING draws because nothing else honors it today; unguarded border/scrollbar/frame paths running with a stale or active clip_enabled=1 would start dropping pixels, and filled-rect/hline fast paths likely bypass plot_pixel anyway. The correct minimal fix is row-level Y clipping plus pixel-level X clipping inside draw_char and draw_char_inverted — the single chokepoint shared by gfx_draw_string_stub, gfx_draw_string_inverted, gfx_draw_char_stub, and gfx_draw_string_wrap — leaving the existing origin checks as valid early-out optimizations. Severity 'high' is fair for a GUI OS: this is the direct mechanism for visible text bleed outside windows/widgets, though it is purely a visual defect (the screen clamp prevents out-of-bounds video memory writes).
FIX: Add clip enforcement inside draw_char (kernel/kernel.asm 3025-3095) and identically in draw_char_inverted (4694-4745). All callers enter with DS=0x1000 (kernel), so clip vars are directly addressable. 8086-safe (cmp/jb/ja/jmp only). Keep the existing origin-only checks in the string stubs as early-out optimizations; no changes needed in gfx_draw_char_stub or gfx_draw_string_wrap since both call draw_char.

draw_char patch (markers show inserted code; unchanged lines abbreviated):

.row_loop:
    lodsb                           ; (unchanged - MUST stay before clip so SI advances per row)
    mov ah, al
    ; --- NEW: row-level Y clip ---
    cmp byte [clip_enabled], 0
    je .y_ok
    cmp bx, [clip_y1]
    jb .skip_row
    cmp bx, [clip_y2]
    ja .skip_row
.y_ok:
    mov cx, [draw_x]
    xor dx, dx
    mov dl, [draw_font_width]
.col_loop:
    ; --- NEW: pixel-level X clip ---
    cmp byte [clip_enabled], 0
    je .x_ok
    cmp cx, [clip_x1]
    jb .next_pixel
    cmp cx, [clip_x2]
    ja .next_pixel
.x_ok:
    test ah, 0x80                   ; (rest of col body unchanged)
    ...
.next_pixel:
    shl ah, 1
    inc cx
    dec dx
    jnz .col_loop
    ; --- gap fill: wrap each plot with the same X clip ---
    push dx
    xor dx, dx
    mov dl, [draw_font_advance]
    sub dl, [draw_font_width]
    jz .no_gap
    cmp byte [draw_bg_color], 0
    jne .gap_color
.gap_black:
    cmp byte [clip_enabled], 0      ; NEW
    je .gb_plot                     ; NEW
    cmp cx, [clip_x1]               ; NEW
    jb .gb_next                     ; NEW
    cmp cx, [clip_x2]               ; NEW
    ja .gb_next                     ; NEW
.gb_plot:                           ; NEW label
    call plot_pixel_black
.gb_next:                           ; NEW label
    inc cx
    dec dl
    jnz .gap_black
    jmp .no_gap
.gap_color:                         ; (same 6-line clip guard around plot_pixel_color)
    ...
.no_gap:
    pop dx
.row_next:                          ; NEW label on existing row-end code
    inc bx
    dec bp
    jnz .row_loop
    jmp .advance                    ; NEW
.skip_row:                          ; NEW: font byte already consumed by lodsb
    jmp .row_next                   ; NEW
.advance:                           ; NEW label on existing tail
    xor ax, ax
    mov al, [draw_font_advance]
    add [draw_x], ax
    popa
    ret

Apply the identical row-Y/pixel-X guards to draw_char_inverted (.row_loop at 4702, .col_loop at 4709, gap loop at 4728 which calls plot_pixel_white). Do NOT put the clip in plot_pixel_white/black/color: chrome-drawing paths that run with clip_enabled=1 are only guarded around string calls today (see explicit clip_enabled clears at 4328-4330, 4612-4613, 4963-4969, 16147-16152), so a global per-pixel clip would regress window frame/scrollbar drawing.

## [high/high] VESA scroll corrupts rows that straddle a 64KB bank boundary
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm 19693-19705 (same-bank fast path: bank compare + unguarded rep movsb) and 19709-19730 (cross-bank per-byte loop: inc si/inc di without bank re-derivation) | area:gfx_scroll_area VESA path
DESC: The same-bank fast path only compares the banks of the row START offsets: 'mov ax,[cs:.sa_vesa_sbank] / cmp ax,[cs:.sa_vesa_dbank] / jne .sa_vesa_cross' then 'rep movsb'. In 640x480x8 a bank boundary falls mid-row every 102.4 rows (e.g. offset 65536 = row 102, x=256). When src/dst rows straddle the boundary, SI/DI wrap from 0xFFFF to 0x0000 inside the SAME bank window instead of advancing to the next bank, copying garbage from/to the start of the bank. The cross-bank per-byte path (19704-19720) has the same flaw: it increments SI/DI without re-deriving the bank when they wrap. A full-width scroll in VESA mode garbles ~4 specific rows per screen (at y≈102, 204, 307, 409). vesa_fill_rect handles mid-row crossing correctly, so only the copy is affected.
VERIFIER: CONFIRMED by reading C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. The VESA path of gfx_scroll_area (.sa_vesa, lines 19657-19746) computes per-row 32-bit src/dst offsets (DX=bank, AX=offset-in-bank) at 19675-19690, then: (1) Fast path 19693-19705 compares only the banks of the row START offsets and does an unguarded `rep movsb` with CX=[.sa_w]. Concrete failure: full-width scroll up 16px, src_y=102 -> linear 65280 (sbank=0, soff=0xFF00), dst_y=86 -> 55040 (dbank=0, doff=0xD700); banks equal, so rep movsb copies 256 bytes correctly then SI wraps 0xFFFF->0x0000 inside the still-mapped bank-0 window, reading the remaining 384 bytes from linear 0 (screen row 0) instead of linear 65536 (bank 1 = row 102 x=256). Dst row gets a garbled tail. (2) Cross path 19709-19730 (finding cited 19704-19720; actual range is 19709-19730) loads .sa_vesa_sbank/dbank once per row and only does `inc si`/`inc di` per byte; when DI wraps (e.g. dst_y=102, doff=0xFF00, src_y=118 in bank 1 -> cross path) the remaining 384 bytes are WRITTEN to bank-0 offset 0, visibly stomping screen row 0, while row 102's tail is left stale; symmetrically SI can wrap and read garbage. No protective mechanism exists: API 80 (dispatch table line 7429) takes arbitrary caller X/Y/W/H with no clipping, docs (API_REFERENCE.md:1111) impose no constraint, and vesa_set_bank (2789) is a plain window-A switch with a cur-bank cache. vesa_fill_rect (2900-2920) explicitly handles mid-row crossing (computes 0-DI bytes-to-boundary, increments bank, zeroes DI), proving the copy paths' omission is a genuine bug. Bank boundaries fall mid-row at rows 102/204/307/409 (65536k/640=102.4k) exactly as the finding claims. Severity high is fair for full-screen VESA scrolling: each straddling row corrupts once as src (garbled dst tail) and once as dst (stray writes at the top of a bank window, which is visible screen memory since all 5 banks of the 307200-byte framebuffer are on-screen).
FIX: Minimal 8086-safe fix, two edits in kernel/kernel.asm:

(1) Demote straddling rows from the fast path. Replace lines 19693-19698:

    ; Check if src and dst in same bank
    mov ax, [cs:.sa_vesa_sbank]
    cmp ax, [cs:.sa_vesa_dbank]
    jne .sa_vesa_cross
    ; Fast path only if neither row reaches past the 64KB bank end
    mov ax, [cs:.sa_vesa_soff]
    add ax, [cs:.sa_w]
    jc .sa_vesa_cross               ; src row crosses bank boundary
    mov ax, [cs:.sa_vesa_doff]
    add ax, [cs:.sa_w]
    jc .sa_vesa_cross               ; dst row crosses bank boundary

    ; Same bank: set bank and direct movsb
    mov ax, [cs:.sa_vesa_sbank]
    call vesa_set_bank

(2) Re-derive banks on pointer wrap in the per-byte path. Replace lines 19727-19728 (`inc si` / `inc di`) with:

    inc si
    jnz .sa_vc_si_ok
    inc word [cs:.sa_vesa_sbank]    ; SI wrapped 0xFFFF->0: advance src bank
.sa_vc_si_ok:
    inc di
    jnz .sa_vc_di_ok
    inc word [cs:.sa_vesa_dbank]    ; DI wrapped 0xFFFF->0: advance dst bank
.sa_vc_di_ok:
    dec cx
    jnz .sa_vc_byte

Notes: INC sets ZF on 8086, so the jnz wrap detection is safe. Mutating .sa_vesa_sbank/dbank mid-row is safe because both are recomputed from scratch at the top of .sa_vesa_copy for every row (19675-19690). A same-bank row that merely straddles now takes the per-byte path, where vesa_set_bank's early-out (cmp ax,[vesa_cur_bank]/je at 2790) keeps it cheap: only two real INT 10h bank switches occur, at the boundary byte. The exact-fit case soff+w==0x10000 sets CF and takes the slow path unnecessarily, which is harmless and still correct.

## [high/high] First-fit size comparison is signed — malloc always fails on a fresh heap
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:    jae .allocate                   ; Found suitable block (unsigned compare) | area:heap allocator
DESC: The fit test is 'cmp ax, bx / jge .allocate' (lines 10005-10006) where AX is the candidate block size and BX the rounded request. Block sizes are unsigned (the initial free block is 0xF000 = 61440), but JGE is a signed comparison: 0xF000 is -4096, so the 60KB free block compares LESS than any normal request and is skipped; '.next: add si, ax / cmp si, 0xF000 / jb .search' (10008-10011) then runs off the end and the call fails. Net effect: every allocation request returns NULL on a freshly initialized heap, so API 7 is completely non-functional even before considering the overlap bugs.
VERIFIER: Confirmed by direct reading of C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. The fit test at lines 10005-10006 is 'cmp ax, bx / jge .allocate' with AX = candidate block size (loaded at line 9996 from the block header) and BX = rounded request size (lines 9972-9974: (req+7) & 0xFFFC, minimum 8). The fresh-heap init at line 9987 writes the single free block's size as 0xF000 (61440). JGE is a signed branch (taken when SF=OF): 'cmp 0xF000, 0x0014' yields SF=1/OF=0, so the 60KB block compares as -4096 < request and is skipped for any rounded request in 8..0x7FFC. Control falls to .next (lines 10008-10011): SI becomes 0+0xF000=0xF000, and 'cmp si,0xF000 / jb .search' has ZF=1/CF=0, so JB is not taken and execution falls into .fail_restore (line 10013), returning AX=0. The failure leaves heap state unchanged, so it repeats on every call. No masking mechanism exists: mem_alloc_stub is API slot 7 directly via the dispatch table (line 7312), 0xF000 appears only in this function (no alternate heap init), and there is no size check elsewhere. One minor overstatement in the original finding: requests whose ROUNDED size lands in 0x8000..0xF000 (i.e. raw requests of roughly 32KB-60KB) accidentally succeed, because then both operands are negative as signed values and JGE gives the unsigned-correct answer. All realistic allocations (< ~32760 bytes) fail, so the high severity stands. The suggested fix (JAE, the unsigned >= branch, valid on 8086) is correct and minimal; the symmetric guard 'jb .search' at line 10011 is already unsigned and needs no change. Adjacent defect noticed while verifying (separate issue, not part of this verdict): lines 9983/9989 read/write [heap_initialized] while DS=0x1400 (the heap segment), but heap_initialized is declared in the kernel data segment at line 20053 — the flag actually lives inside heap memory at that offset and can be clobbered by allocations or contain boot-time garbage.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm line 10006, replace 'jge .allocate' with 'jae .allocate' (unsigned >=, 8086-safe). One-instruction change; the 'jb .search' loop guard at line 10011 is already unsigned and correct.

## [high/high] Allocator never splits blocks and free never coalesces
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:10005-10006 (signed jge makes 0xF000 block never match normal requests), 10019-10025 (no split), 10050-10055 (no coalesce); also 9978, 9987, 10010, 10047 (heap base 0x1400 lies inside the 44KB kernel image and overlaps the shell segment; heap_initialized accessed via wrong segment at 9983/9989) | area:heap allocator
DESC: .allocate just marks the found block: 'mov word [si+2], 0xFFFF / mov ax, si / add ax, 4' (10021-10025) — the block's size header is left unchanged and no remainder block is created. Since the fresh heap is a single 0xF000 block, the very first allocation (of any size) would consume the entire 60KB heap; every subsequent malloc fails until that one pointer is freed. mem_free_stub (10038-10061) only clears the flag word ('mov word [bx+2], 0') with no merging of adjacent free blocks, so if splitting were added, repeated mixed-size alloc/free would fragment the heap irreversibly. Together these make the heap unusable for more than one live allocation.
VERIFIER: The structural claim is TRUE as written in code, but the auditor's concrete failure trace is wrong, and the real situation is both worse and (today) latent.

CONFIRMED parts (kernel/kernel.asm):
1. No split: .allocate (10019-10025) only does 'mov word [si+2], 0xFFFF' and returns SI+4. The size header [si] is never reduced and no remainder header is written. A matched block is consumed whole.
2. No coalesce: mem_free_stub (10038-10061) only does 'mov word [bx+2], 0'. No merging.

REFUTED detail — "the very first allocation (of any size) would consume the entire 60KB heap": it does not, because line 10005-10006 is 'cmp ax, bx / jge .allocate' — a SIGNED comparison. The initial block size 0xF000 is -4096 as signed 16-bit, so for any normal request (rounded size < 0x8000, i.e. requests up to ~32KB-9) the fit check FAILS; the loop steps SI to 0xF000, the 'cmp si, 0xF000 / jb' bound fails, and mem_alloc returns NULL. So in current code malloc never succeeds at all for normal sizes. Only a request >= 0x7FF9 bytes (rounds to >= 0x8000, which is also negative signed) passes jge and would consume the whole heap — the only path where the auditor's trace occurs.

ADDITIONAL findings the auditor missed, which make the suggested fix insufficient on its own:
(a) The heap region overlaps the kernel image. kernel.bin is 45056 bytes (0xB000; pad at line 20488 'times 45056 - ($-$$)'), loaded at 0x1000:0000 (org 0x0000, line 6; confirmed by caller_ds init to 0x1000 at line 20058). Physical 0x14000 (heap base 0x1400:0000) = kernel offset 0x4000, kernel code well past the API table at 0x3320. The init write 'mov word [0], 0xF000' (line 9987) would overwrite kernel code, and a 0xF000-byte heap (0x14000-0x22FFF) also overlaps the shell segment at 0x2000:0000. The docs (MEMORY_LAYOUT.md "28 KB Kernel") are stale.
(b) heap_initialized (defined at line 20053, kernel segment 0x1000) is accessed at lines 9983/9989 AFTER 'mov ds, 0x1400' — so the check reads/writes physical 0x14000+offset, not the actual variable. The init guard reads arbitrary kernel-image bytes.

MITIGATING context (severity in practice): API 7/8 (table at lines 7312-7313) has NO in-tree caller — no 'call mem_alloc_stub' anywhere in the kernel, and no app issues int 0x80 with AH=7/8 (apps are loaded into fixed 64KB segment slots via the segment pool, not malloc). So nothing in the shipped system currently exercises this code; it is a broken-but-dead public API, documented in docs/API_REFERENCE.md. Severity should be 'high (latent)': any third-party app using the documented malloc gets NULL for all normal sizes, and a >=32KB request would silently corrupt kernel code.
FIX: Minimal correct fix (8086-safe NASM). Four parts — split/coalesce alone is NOT safe because the heap region overlaps the kernel image.

1) Relocate heap out of the kernel image and shrink it to the free gap (kernel ends at phys 0x1B000, shell starts at 0x20000 => 20KB at segment 0x1B00):
   line 9978:  mov ax, 0x1400  ->  mov ax, 0x1B00
   line 9987:  mov word [0], 0xF000  ->  mov word [0], 0x5000
   line 10010: cmp si, 0xF000  ->  cmp si, 0x5000
   line 10047: mov bx, 0x1400  ->  mov bx, 0x1B00

2) Fix wrong-segment flag access (lines 9983, 9989) — use CS override (kernel CS=0x1000):
   cmp word [cs:heap_initialized], 0
   ...
   mov word [cs:heap_initialized], 1

3) Unsigned fit compare (line 10006):
   jge .allocate  ->  jae .allocate

4) Split in .allocate (replace lines 10019-10027; AX already holds the block size from .search; BX = rounded request; DX/SI restored, ES=heap seg already set):
.allocate:
    push dx
    mov dx, ax                      ; DX = block total size
    sub dx, bx                      ; DX = remainder
    cmp dx, 8                       ; room for header + min 4-byte payload?
    jb .no_split
    mov [si], bx                    ; shrink current block to request size
    push si
    add si, bx                      ; SI -> remainder
    mov [si], dx                    ; remainder size
    mov word [si+2], 0              ; remainder free
    pop si
.no_split:
    pop dx
    mov word [si+2], 0xFFFF         ; mark allocated
    mov ax, si
    add ax, 4
    pop ds

5) Forward coalesce in mem_free_stub (replace body; note added push/pop si):
mem_free_stub:
    push ax
    push bx
    push si
    push ds
    test ax, ax
    jz .done
    mov bx, 0x1B00
    mov ds, bx
    sub ax, 4
    mov bx, ax
    mov word [bx+2], 0              ; mark free
.coalesce:
    mov si, bx
    add si, [bx]                    ; SI -> next block
    cmp si, 0x5000                  ; past heap end?
    jae .done
    cmp word [si+2], 0              ; next block free?
    jne .done
    mov ax, [si]
    add [bx], ax                    ; merge next into this
    jmp .coalesce
.done:
    pop ds
    pop si
    pop bx
    pop ax
    ret

Also update docs/MEMORY_LAYOUT.md (heap base/size, stale 28KB kernel size) to match.

## [high/medium] Event queue head-of-line blocking: one undelivered event freezes keyboard and mouse for all tasks
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:10151-10152 (KEY_PRESS leave-in-queue without head advance), 10177 (WIN_REDRAW leave-in-queue without head advance), 10104-10106 (silent drop when full); aggravating consumers: 5184-5197 and 5869-5881 (modal loops that never yield); WIN_REDRAW posters for other tasks: 4372-4375, 16256-16258, 16351-16354, 19318-19324 | area:kernel data structures / event queue
DESC: event_get_stub examines only the event at the queue head. A KEY_PRESS whose focused_task differs from current_task jumps to .no_event WITHOUT advancing head ('cmp al, [current_task] / je .evt_focus_ok / pop ax / jmp .no_event', 10149-10152), and a WIN_REDRAW for another task's window does the same ('jmp .no_event ; Wrong task's window - leave in queue', 10177). The single shared 32-slot ring (event_queue, 20215) then stalls for every OTHER task: mouse events queued behind the stuck head event are invisible to all consumers until the one target task polls. If the focused app is busy (long disk I/O, modal loop) or never polls, keyboard AND mouse appear dead system-wide, and once 31 entries accumulate post_event silently drops new events ('cmp bx, [event_queue_head] / je .done', 10104-10105). This directly matches the reported 'keyboard and mouse input issues'.
VERIFIER: CONFIRMED, with a refined scenario. The cited mechanism is exactly as described: event_get_stub (kernel/kernel.asm 10117-10203) inspects only the head slot of the single shared 32-slot ring (event_queue, 20215). For EVENT_KEY_PRESS whose focused_task != current_task it executes 'pop ax / jmp .no_event' (10151-10152) and for EVENT_WIN_REDRAW owned by another task it executes 'jmp .no_event' (10177) -- in both cases WITHOUT advancing event_queue_head. Head is advanced ONLY at 10183 (.evt_consume) and 10200 (.evt_discard, invalid handle or freed window); grep confirms no other writer, no timeout, no cleanup. post_event (10083-10112) silently drops when tail+1==head (10104-10105), so a wedged head eventually kills ALL input after 31 queued events. No protecting mechanism exists.

Refinements to the auditor's scenario: (1) The scheduler is cooperative (20341-20342), so the 'focused app busy in long disk I/O' framing is weak -- a non-yielding app freezes the system regardless of the queue. (2) The genuinely fatal path is the kernel's own modal dialogs: the file-open loop (.fdlg_loop, 5184-5197) and save loop (.sdlg_loop, 5869-5881) hlt+poll and never yield ('intentionally blocks all other tasks'). During a modal, window drag/focus still works (mouse IRQ sets flags; mouse_process_drag is called at the top of every event_get_stub, 10126) and posts EVENT_WIN_REDRAW for OTHER tasks' windows (4372-4375 deferred focus, 16256-16258 move-uncover, 16351-16354 topmost repaint, 19318-19324 resize). The owner task never runs while the modal blocks, so once that WIN_REDRAW reaches the head it sticks forever: every KEY_PRESS behind it is invisible, ESC/Enter/typing go dead, and the save dialog (which requires typing a filename) has no keyboard recovery at all. (3) Non-modal variant: any task owning a visible window that never polls events (nothing forces polling; legacy kbd_getchar at 664 exists) wedges the head for ALL tasks once any uncover/resize posts a WIN_REDRAW for its window -- system-wide dead keyboard/mouse events while the cursor still moves (drawn at IRQ time, 1136), matching the reported symptom. (4) The KEY_PRESS variant is partially self-healing in non-modal use: clicking any title bar re-focuses via the deferred drag_needs_focus mechanism (which runs inside event_get_stub even when the queue is wedged, 4316-4343, win_focus_stub sets focused_task at 17840), after which the newly-focused task consumes the stuck key. The WIN_REDRAW path has no such escape until the target window is destroyed (only then does .evt_discard at 10170-10171/10196-10200 collect it). Severity high is justified, primarily via the modal-dialog keyboard deadlock and the non-polling-owner system-wide starvation.
FIX: Make event_get_stub scan past undeliverable events instead of bailing at the head, consuming mid-queue events by tombstoning the slot with EVENT_NONE and lazily collecting tombstones when they reach the head. This is IRQ-safe without cli: post_event (IRQ side) only writes the slot at tail and advances tail; the consumer only writes head and slots strictly between head and tail, and runs only in task context (cooperative scheduler, switches only at yield). All added instructions are 8086-safe (the routine already uses 'shl si,5', so the existing 186+ baseline is unchanged). Replace lines 10128-10201 with:

.evt_check_next:
    mov bx, [event_queue_head]
.evt_scan:
    cmp bx, [event_queue_tail]
    je .no_event
    ; Calculate slot position (events are 3 bytes each)
    mov si, bx
    add si, bx                      ; SI = slot * 2
    add si, bx                      ; SI = slot * 3
    mov al, [event_queue + si]      ; type
    mov dx, [event_queue + si + 1]  ; data (word)

    cmp al, EVENT_NONE
    je .evt_tombstone               ; Slot consumed mid-queue earlier: skip/collect

    ; Filter keyboard events: only deliver to focused task
    cmp al, EVENT_KEY_PRESS
    jne .evt_not_key
    push ax
    mov al, [focused_task]
    cmp al, 0xFF                    ; No window focused? Deliver to current task
    je .evt_focus_ok
    cmp al, [current_task]
    je .evt_focus_ok
    pop ax
    jmp .evt_skip                   ; Not ours - leave it, keep scanning
.evt_focus_ok:
    pop ax
    jmp .evt_consume

.evt_not_key:
    cmp al, EVENT_WIN_REDRAW
    jne .evt_consume                ; Other event types: consume and pass through
    cmp dl, WIN_MAX_COUNT
    jae .evt_discard                ; Invalid window handle: drop and keep scanning
    push si
    push ax
    xor ah, ah
    mov al, dl
    mov si, ax
    shl si, 5
    add si, window_table
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .evt_discard_pop            ; Window freed/destroyed: drop stale event
    mov al, [si + WIN_OFF_OWNER]
    cmp al, [current_task]
    pop ax
    pop si
    je .evt_consume
    ; fall through: wrong task's window - leave it, keep scanning
.evt_skip:
    inc bx
    and bx, 0x1F
    jmp .evt_scan

.evt_tombstone:
    cmp bx, [event_queue_head]
    jne .evt_skip                   ; Mid-queue tombstone: just step over it
    inc bx                          ; Tombstone at head: collect it
    and bx, 0x1F
    mov [event_queue_head], bx
    jmp .evt_scan

.evt_discard_pop:
    pop ax
    pop si
.evt_discard:
    mov byte [event_queue + si], EVENT_NONE  ; Kill invalid/stale event in place
    jmp .evt_tombstone              ; Collect if at head, else step over

.evt_consume:
    cmp bx, [event_queue_head]
    jne .evt_consume_mid
    inc bx                          ; Consuming at head: advance head normally
    and bx, 0x1F
    mov [event_queue_head], bx
    jmp .evt_return
.evt_consume_mid:
    mov byte [event_queue + si], EVENT_NONE  ; Consume mid-queue: tombstone slot

.evt_return:
    clc                             ; CF=0 = event available
    pop ds
    pop si
    pop bx
    ret

(.no_event at 10203 and everything after it is unchanged.) Tombstones are collected as soon as they reach the head, so capacity is only transiently reduced. Note this delivers events to a task out of global order relative to other tasks' pending events, which is the intended behavior change. Longer term, per-task queues with routing at post time (KEY_PRESS to focused_task, WIN_REDRAW to the window owner) remain the cleaner design.

## [high/high] INT 9 keyboard handler never issues the XT (8255 PPI) keyboard acknowledge — keyboard dead after first keystroke on a real 8088/XT
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: handler 461-660; scancode read at 472; missing ack belongs in .done block (lines 651-654, before the EOI at 653-654) | area:BIOS/hardware assumptions (8088 target)
DESC: int_09_handler reads the scancode (line 472 'in al, 0x60') and at .done only sends a PIC EOI (lines 654-656 'mov al, 0x20 / out 0x20, al') before iret. On AT-class hardware (8042 KBC) that is sufficient, but on a genuine PC/XT the keyboard interface is an 8255 PPI: after reading port 0x60 the handler must pulse bit 7 of port 0x61 high then low to clear the keyboard latch, or the PPI never presents another scancode — the keyboard delivers exactly one key and goes silent. This directly defeats the stated goal of running on an original 8088 and is invisible in QEMU/AT testing. It is also one more piece of the user's 'keyboard input issues' symptom class for real-hardware testing.
VERIFIER: Verified directly in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. int_09_handler (lines 461-660) reads the scancode with 'in al, 0x60' (line 472) and at .done (line 651) does only a PIC EOI (mov al,0x20 / out 0x20,al at lines 653-654) before iret. There is no access to port 0x61 anywhere in the handler; the only 0x61 accesses in the entire kernel are the PC-speaker routines at lines 16600-16613, which are unrelated. No other mechanism saves it: install_keyboard (line 426, called unconditionally at kernel line 36) installs the handler 'on all boot types' (comment line 438), forces use_bios_keyboard=0 (line 454), and although the old INT 9 vector is saved (lines 447-449) it is never chained or restored — old_int9_offset/segment appear nowhere else except their data definitions (lines 20192-20193). So the BIOS keyboard ISR, which performs the XT acknowledge, never runs. The hardware claim is accurate: on a genuine PC/XT the keyboard interface is an 8255 PPI with a discrete shift register and IRQ flip-flop; reading port 0x60 does not clear them. The IBM PC/XT Technical Reference BIOS INT 9 pulses bit 7 of port 0x61 high then restores it to clear the latch; without it IRQ1 stays asserted, the edge-triggered 8259 sees no new edge, and exactly one keyboard interrupt ever fires. The project explicitly targets this hardware (README: 'IBM PC XT-compatible computers', minimum 'Intel 8088 @ 4.77 MHz', 'PC/XT keyboard'). The bug is invisible in testing because the Makefile's QEMU '-M isapc' machine still emulates an i8042 KBC, where reading 0x60 acks in hardware. The suggested read-modify-write pulse preserves bits 0-6 of port 0x61, making it harmless on AT-class machines (the standard DOS TSR idiom), and clobbering AH at .done is safe because the handler pushes/pops AX. Only nit: the EOI is at lines 652-654, not 654-656 as cited. Severity high is justified: on the stated minimum hardware the keyboard is unusable after the first keystroke.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, at .done (line 651), insert the XT keyboard acknowledge before the EOI (8086-safe; AX is saved/restored by the handler so AH clobber is fine):

.done:
    ; XT (8255 PPI) keyboard acknowledge: pulse port 0x61 bit 7 to clear
    ; the keyboard shift register and IRQ1 latch. Required on genuine
    ; PC/XT; harmless on AT (read-modify-write preserves bits 0-6).
    in al, 0x61
    mov ah, al
    or al, 0x80
    out 0x61, al
    mov al, ah
    out 0x61, al
    ; Send EOI to PIC
    mov al, 0x20
    out 0x20, al

## [high/high] Arrow/Home/End/Delete/PgUp/PgDn require E0-prefixed scancodes that XT 83-key keyboards never send — no cursor navigation on the 8088 target
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: 475-480 + 520-522 (E0-only gating), 501-517 (bare 0x47-0x53 fall through to digit tables), 524-555 (special codes 128-136 reachable only via E0), 2158-2159 and 2166-2167 (numpad digit rows in both tables), 454 + 1430 + 10280-10349 (dead INT 16h fallback that would have handled XT), 20197-20200 (kbd state vars, no NumLock); consumers: apps/launcher.asm 285-291 | area:BIOS/hardware assumptions (8088 target)
DESC: Extended keys are only recognized after an E0 prefix (line 475 'cmp al, 0xE0' sets kbd_e0_flag; .handle_extended at 524 maps 0x48->Up etc.). The XT 83-key keyboard has no dedicated cursor cluster and never emits E0; its numpad arrows produce bare scancodes 0x47-0x53, which fall through to the normal table lookup where scancode_normal (lines 2158-2159) maps them to digits: db 0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1' / '2','3','0','.'. Result: on a real XT, pressing Up yields '8', Delete yields '.', and no app or launcher list can be navigated. Works fine on AT/QEMU (101-key sends E0), so it only bites the 8088 goal.
VERIFIER: CONFIRMED by direct code trace. (1) Target hardware is real: README.md line 201/205 and docs/FEATURES.md lines 7/11 list minimum hardware as "Intel 8088 @ 4.77 MHz" with "PC/XT keyboard" as the required input device. (2) The custom INT 9 handler is installed unconditionally on ALL boot types (kernel/kernel.asm 438-454, comment at 438: "Install custom INT 9 handler on all boot types") and sets use_bios_keyboard=0 (line 454). use_bios_keyboard is only ever written as 0 (declared db 0 at line 1430; only other reference is the cmp at 10282), so the BIOS INT 16h polling fallback at lines 10280-10349 — which ironically contains a correct XT-compatible mapping (it translates AL=0/AH=0x47-0x53 extended returns to codes 128-136) — is dead code. No rescue mechanism exists. (3) In int_09_handler, special codes 128-136 are produced ONLY in .handle_extended (lines 524-555), reachable ONLY when kbd_e0_flag was set by a prior 0xE0 byte (lines 475-476, 520-522). (4) Hardware fact is accurate: the IBM XT 83-key keyboard (and the 84-key AT keyboard, which this bug also hits) has no dedicated cursor cluster and never emits 0xE0 — E0 prefixes were introduced with the 101-key Enhanced keyboard. Its cursor/nav keys are the numpad keys, emitting bare make codes 0x47-0x53 regardless of NumLock (NumLock is interpreted in software). (5) Bare 0x47-0x53 are not modifiers (483-498), pass the release test (501-502, bit 7 clear on make), and hit the table lookup (505-517). scancode_normal indices 0x47-0x53 (lines 2158-2159) = '7','8','9','-','4','5','6','+','1','2','3','0','.'; scancode_shifted (2166-2167) is identical. So on a real XT: Up→'8', Down→'2', Left→'4', Right→'6', Home→'7', End→'1', PgUp→'9', PgDn→'3', Del→'.'. (6) The kernel tracks no NumLock state at all (no 0x45 handling anywhere; scancode 0x45 maps to 0 in the table and is discarded at .store_key's null check). (7) Consumers are bricked: apps/launcher.asm 285-291 navigates the app list solely via codes 128-131; browser.asm 283-285, tetrisv.asm 127-133, outlast.asm 141-147, music.asm 334-336 likewise. On a real XT (typically no PS/2 mouse port either), the launcher list, browser, and all arrow-driven games are unusable. Severity 'high' is justified against the stated 8088/XT goal; the bug is invisible in QEMU/AT testing because 101-key keyboards send E0. Minor refinements to the finding: it also affects 84-key AT keyboards (not just XT), and the shifted table (2161-2167) has the same digit mappings, so Shift doesn't help. The suggested fix direction (track NumLock, route bare 0x47-0x53 through the special-key mapping when off) is sound, with care to exclude 0x4A ('-'), 0x4C ('5'), 0x4E ('+'), and 0x52 (Ins, which has no special code) from rerouting.
FIX: All edits in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm; all instructions are 8086-safe.

1. Add state variable after line 20200 (kbd_e0_flag: db 0):

kbd_numlock_state: db 0             ; 0=off: bare numpad scancodes act as cursor keys (XT/84-key AT)

2. In int_09_handler, insert NumLock detection with the other modifier checks, after line 498 (cmp al, 0xB8 / je .alt_release):

    cmp al, 0x45                    ; NumLock make code: toggle state
    je .numlock_toggle

3. After the release filter (lines 501-502: test al, 0x80 / jnz .done) and BEFORE 'mov bx, ax' (line 505), insert the routing block and label the existing table lookup .translate:

    ; XT 83-key / AT 84-key keyboards never send E0; their cursor keys are
    ; the numpad (bare 0x47-0x53). With NumLock off, map them to the same
    ; special codes (128-136) as the E0 path.
    cmp byte [kbd_numlock_state], 0
    jne .translate                  ; NumLock on: digits via table
    cmp al, 0x47
    jb .translate
    cmp al, 0x53
    ja .translate
    cmp al, 0x4A                    ; numpad '-' always types '-'
    je .translate
    cmp al, 0x4C                    ; numpad '5': no cursor meaning
    je .translate
    cmp al, 0x4E                    ; numpad '+' always types '+'
    je .translate
    cmp al, 0x52                    ; Ins: no special code defined
    je .translate
    jmp .map_special                ; 0x47/48/49/4B/4D/4F/50/51/53
.translate:
    mov bx, ax                      ; (existing line 505 continues here)

4. Add the .map_special label inside .handle_extended, immediately before line 535 (cmp al, 0x48 ; Up arrow):

.map_special:
    cmp al, 0x48                    ; Up arrow
    ...

(The bare path enters past the E0-flag clear and release test, both already satisfied; every value routed there matches one of the cmp/je arms, so the chain's trailing 'jmp .done' is never hit from this path. The 0x1C numpad-Enter check at 543 is upstream of .map_special and unaffected.)

5. Add the toggle handler next to the existing .shift_press/.ctrl_press handlers:

.numlock_toggle:
    xor byte [kbd_numlock_state], 1
    jmp .done

Optional polish: in install_keyboard (line 426), seed kbd_numlock_state from the BIOS keyboard flag byte (0040:0017 bit 5) so AT machines whose BIOS boots with NumLock on start in the digit state:

    mov ax, 0x0040
    mov es, ax                      ; ES is already pushed/restored in install_keyboard
    mov al, [es:0x0017]
    and al, 0x20
    jz .nl_off
    mov byte [kbd_numlock_state], 1
.nl_off:

## [high/high] 8x8 font advance is 12 px — desktop icon labels overlap (confirmed boot symptom) and text rendering is 50% slower
- src:performance | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:20040 (draw_font_advance: db 12) and kernel/kernel.asm:20051 (font 1 descriptor: db 8, 8, 12, 8); supporting code: draw_char gap fill 3061-3083, draw_char_inverted 4722-4732, gfx_set_font 7634-7635, gfx_text_width 4833; launcher.asm 60, 1054-1068, 2215 | area:fonts / desktop
DESC: font_table entry for the default font 1 is 'dw font_8x8 / db 8, 8, 12, 8' (height=8, width=8, advance=12) and the boot default is 'draw_font_advance: db 12' (19963). With a 12 px advance, an 11-char icon name is 132 px wide. The launcher draws labels at icon_x-8 on an 80 px column pitch (launcher.asm 60: COL_WIDTH_LO equ 80; label draw at 1054-1068), and the kernel desktop repaint does the same (kernel.asm 15875-15888), so any name longer than 6 chars ('NOTEPAD', 'SETTINGS', 'Refresh') runs into the neighbouring label — exactly the 'icon labels overlap each other at boot' symptom. It also makes draw_char render 12 columns per char instead of 8 (the 4 gap pixels per row are filled one-by-one via plot calls, lines 3061-3083), a ~50% slowdown of all default-font text, and gfx_text_width (4833) returns the inflated width to apps.
VERIFIER: Finding is real; only the cited line numbers were stale (off by ~77 lines, likely an older revision of kernel.asm).

VERIFIED FACTS (C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm):
1. Data: line 20040 is `draw_font_advance: db 12` and line 20051 is the font-1 descriptor `db 8, 8, 12, 8` (height=8, width=8, advance=12, bpc=8) in font_table (20047-20053). NOT at 19963/19973-19974 as cited — those lines are scroll-area code.
2. Boot path uses it: kernel boots CGA 320x200 with current_font=1 (20037). New tasks inherit it (14997-14998) and every context switch calls gfx_set_font (1798-1800, 15103-15105), which reloads advance=12 from font_table (7634-7635). The launcher (apps/launcher.asm) never calls SET_FONT, so icon labels render with advance 12.
3. Overlap geometry: launcher.asm COL_WIDTH_LO equ 80 (line 60); labels drawn at icon_x-8 (1054-1055); names up to 11 chars in 12-byte slots (2147). Next column's label starts at icon_x+72. A 7-char name ('Refresh', auto-added on floppy boot, lines 488/2215) is 7*12=84px -> ends at icon_x+76, overlapping 4px; 8-char 'SETTINGS' overlaps 16px. Kernel desktop repaint (15856-15908) uses the same 80px-pitch geometry. This exactly matches the boot-visible label-overlap symptom. With advance 8, names up to 10 chars fit cleanly (an 11-char name still bleeds 8px — minor residual, not a regression).
4. Perf: draw_char (3025-3095) plots every pixel via plot_pixel_* calls: 8 glyph cols + 4 gap cols per row (gap loop 3061-3083) = 12 plots/row vs 8, i.e. exactly 50% more plot calls per default-font character, in all video modes (no fast blit path exists). Same gap loop in draw_char_inverted (4722-4732). gfx_text_width (4821-4842) returns the inflated width to apps.
5. History supports "bug, not design": advance=12 landed in Build 205 (fd63ba8) and has since required two workaround commits (Build 270 7c99a15, Build 271 689468a) adding the gap-fill loops to hide 4px artifacts — the gap fill IS the perf cost being removed.
6. Fix safety checked: both gap loops skip cleanly when advance==width (`jz .no_gap` at 3066 and 4727). All kernel/app layout reads advance dynamically (draw_font_advance, gfx_text_width, API 49/93) and adapts. The only hard-coded 12px advance in the tree is tetris.asm:507 / tetrisv.asm:660, and both explicitly set font 2 first (tetris.asm 486-488) — unaffected since font 2's descriptor (line 20053) is unchanged. Scrollbar arrow code saves/forces/restores advance itself (8917-8950) — unaffected. Titlebar code forces font 1 or 2 per resolution (17584-17604) and restores — unaffected. 8086-safe: data-only change.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm (line numbers per current HEAD):

Line 20051 (font_table entry for font 1), change:
    db 8, 8, 12, 8                    ; height=8, width=8, advance=12, bpc=8
to:
    db 8, 8, 8, 8                     ; height=8, width=8, advance=8, bpc=8

Line 20040 (static boot default, used before first gfx_set_font), change:
    draw_font_advance: db 12             ; Pixels to advance per character
to:
    draw_font_advance: db 8              ; Pixels to advance per character

Leave font 0 (advance 6) and font 2 (advance 12, line 20053) untouched — tetris.asm:507/tetrisv.asm:660 hard-code 12px for font 2 titles.

## [high/high] gfx_fill_color falls to pixel-by-pixel (MUL per pixel) for any fill not 4-pixel-aligned — which is nearly every window fill
- src:performance | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:16046-16052 (alignment check), 16096-16115 (.gfc_slow per-pixel path), 16010-16019 (fill byte), 235-237 (dispatcher win_x+1 translation), 9700-9793 (existing hybrid template in gfx_clear_area_stub) | area:graphics / fill
DESC: gfx_fill_color (backend of API 2 filled-rect, API 67 colored rect, titlebar fills, desktop region fills) checks 'mov ax, bx / and ax, 3 / jnz .gfc_slow' and the same for width (15969-15975). The .gfc_slow path (16026-16037) calls plot_pixel_color once per pixel — each with a 5-push/5-pop, mode dispatch and the cga_pixel_calc MUL. Window content coordinates are translated by 'add bx, [si + WIN_OFF_X] / inc bx' (dispatcher lines 236-237), so content X is win_x+1 — i.e. window background and widget fills are almost never 4-aligned and almost always take the slow path. A 200x100 misaligned fill = 20,000 pixel calls (~ several million cycles on 8088, seconds of wall time). Notably gfx_clear_area_stub already contains the correct hybrid fast path (partial lead byte + rep stosb middle + partial trail byte, lines 9700-9792) — gfx_fill_color simply never got it. This is a major contributor to slow window redraws / visible repaint tearing.
VERIFIER: CONFIRMED, with corrected line numbers and one caveat on the 8088 wall-time sub-claim.

Verified facts (all read directly from C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm):

1. gfx_fill_color is at line 15997. Its CGA path checks alignment at lines 16046-16052 (mov ax,bx / and ax,3 / jnz .gfc_slow; then mov ax,dx / and ax,3 / jnz .gfc_slow). The slow path .gfc_slow is at 16096-16115 and calls plot_pixel_color once per pixel. The finding's cited line numbers (15969-15975, 16019-16037, fill byte "15933-15942") are stale/off by ~77 lines — those lines are actually inside draw_desktop_region's icon loop — but the described code exists verbatim at the corrected locations (fill byte is built at 16010-16019). The clear_area cite (9700-9792) and dispatcher cite (236-237) are accurate.

2. plot_pixel_color (line 7680) per-pixel cost is as claimed: 2 bounds cmps against [cs:...] memory, 3-way mode dispatch, 5 pushes, call to cga_pixel_calc (3111) containing a 16-bit MUL (line 3115), a read-modify-write on video memory with a stack-indexed color fetch (mov bx,sp / mov ah,[ss:bx]), 5 pops, ret — per pixel. A 200x100 fill = 20,000 such calls.

3. CGA mode 0x04 is the DEFAULT video mode (boot sets it at line 50; data default at 20063), so the slow path lives in the default rendering path. VGA(0x13)/VESA(0x01)/Mode12h branch away before the alignment check and each has a fast fill.

4. All claimed callers verified: API 2 gfx_draw_filled_rect_stub (9613, delegates at 9619), API 67 gfx_draw_filled_rect_color (18251-18252), draw_desktop_region (15854 — fills the arbitrary redraw_old_x/y/w/h rect with desktop_bg_color on every window move/close/redraw), and titlebar fills in win_draw_stub (17624 active 3D, 17627 active flat, 17701 inactive 3D) at BX=win_x, DX=win_width.

5. Misalignment is structural, as claimed: the INT 0x80 dispatcher translates window-content coordinates with content_x = win_x + 1 (lines 235-237), so app fills via API 2/67 hit the fast path only when win_x ≡ 3 (mod 4) AND fill width % 4 == 0. No window-position snapping exists anywhere (grep confirms), and win_create_stub (15196) stores the app-supplied X verbatim in 320x200. Titlebar/desktop fills use raw win_x — aligned only by luck. So the great majority of GUI fills in the default mode take the per-pixel path.

6. gfx_clear_area_stub (9630) indeed already contains the correct hybrid algorithm at 9700-9793 (lead-byte AND mask, rep stosb middle, trail-byte AND mask, width<4 pixel fallback); gfx_fill_color never got it. The only delta needed for arbitrary color is AND-then-OR on the edge bytes using the replicated fill byte already built at 16010-16019.

Caveats / refinements:
- The "seconds of wall time on 8088" framing is moot: the kernel uses movzx and bt (lines 172-173, 203-204 and ~100 other movzx sites) with no 'cpu 8086' directive, so it requires a 386+ and cannot run on an 8088. The relative cost is still real: the slow path does ~3 ISA video-memory accesses plus call/push/pop/MUL/dispatch overhead per pixel vs one stosb write per 4 pixels — a genuine ~10-40x on real slow hardware or IPS-throttled emulators. High severity for default-mode GUI redraw latency is defensible; it is a major contributor to slow window redraws in CGA mode.
- Important for the fix: the slow path currently gets implicit per-pixel screen clipping from plot_pixel_color's bounds checks; the aligned fast path and gfx_clear_area_stub's hybrid have NO clipping. A straight port would regress partially-off-screen fills into memory corruption (the VGA path needed exactly this clamp — "Bounds clamp (Build 397)" at 16134-16156 — proving off-screen fills occur in practice). The fix below therefore adds the same clamp before the CGA fill.
FIX: In gfx_fill_color, replace the alignment check at lines 16046-16052 with a screen clamp (preserves the clipping the slow path implicitly had via plot_pixel_color) followed by the same check, but fall through to a new hybrid path instead of .gfc_slow; keep .gfc_slow only for width < 4.

Replace lines 16046-16052:

    ; Clamp to screen (slow path relied on plot_pixel_color per-pixel clipping)
    cmp bx, [screen_width]
    jae .gfc_cursor_done
    cmp cx, [screen_height]
    jae .gfc_cursor_done
    mov ax, bx
    add ax, dx
    cmp ax, [screen_width]
    jbe .gfc_cga_w_ok
    mov dx, [screen_width]
    sub dx, bx
.gfc_cga_w_ok:
    mov ax, cx
    add ax, bp
    cmp ax, [screen_height]
    jbe .gfc_cga_h_ok
    mov bp, [screen_height]
    sub bp, cx
.gfc_cga_h_ok:
    ; Check for byte-aligned fast path (BX % 4 == 0 AND DX % 4 == 0)
    mov ax, bx
    and ax, 3
    jnz .gfc_hybrid
    mov ax, dx
    and ax, 3
    jnz .gfc_hybrid

Then insert immediately before .gfc_slow (line 16096), modeled on gfx_clear_area_stub .opt_row (9706-9792):

.gfc_hybrid:
    ; Hybrid CGA fill: masked lead/trail pixels + rep stosb middle
    cmp dx, 4
    jb .gfc_slow                    ; Very narrow: pixel path is fine
.gfc_hrow:
    push cx                         ; Save Y
    push bx                         ; Save start X
    push dx                         ; Save width
    ; CGA row base
    mov ax, cx
    shr ax, 1
    push dx
    mov di, 80
    mul di                          ; AX = (Y/2) * 80
    pop dx
    mov si, ax                      ; SI = row base
    test cl, 1
    jz .gfc_h_even
    add si, 0x2000
.gfc_h_even:
    ; --- Leading partial byte ---
    mov ax, bx
    and ax, 3
    jz .gfc_h_no_lead
    mov di, bx
    shr di, 1
    shr di, 1
    add di, si                      ; DI = first byte in video mem
    push cx
    mov cx, 4
    sub cx, ax                      ; CX = lead pixels to fill (1-3)
    sub dx, cx                      ; Reduce remaining width
    add bx, cx                      ; BX = first aligned X
    shl cl, 1                       ; CL = bits to replace (2,4,6)
    mov al, 0xFF
    shl al, cl                      ; AL = keep-mask (pixels left of fill)
    mov ah, [.fill_byte]
    and [es:di], al                 ; Clear pixels being filled
    not al
    and al, ah                      ; Color bits for filled pixels
    or [es:di], al
    pop cx
.gfc_h_no_lead:
    ; --- Middle full bytes (BX now 4-aligned) ---
    mov ax, dx
    shr ax, 1
    shr ax, 1                       ; AX = full bytes
    jz .gfc_h_no_mid
    mov di, bx
    shr di, 1
    shr di, 1
    add di, si
    push cx
    mov cx, ax
    mov al, [.fill_byte]
    rep stosb
    pop cx
    mov ax, dx
    and ax, 0xFFFC                  ; Middle pixels (multiple of 4)
    add bx, ax
.gfc_h_no_mid:
    ; --- Trailing partial byte ---
    mov ax, dx
    and ax, 3                       ; AX = trailing pixels (0-3)
    jz .gfc_h_no_trail
    mov di, bx
    shr di, 1
    shr di, 1
    add di, si
    push cx
    mov cl, al
    shl cl, 1                       ; CL = bits to replace (2,4,6)
    mov al, 0xFF
    shr al, cl                      ; AL = keep-mask (pixels right of fill)
    mov ah, [.fill_byte]
    and [es:di], al
    not al
    and al, ah
    or [es:di], al
    pop cx
.gfc_h_no_trail:
    pop dx                          ; Restore width
    pop bx                          ; Restore start X
    pop cx                          ; Restore Y
    inc cx                          ; Next Y
    dec bp
    jnz .gfc_hrow
    jmp .gfc_cursor_done

Notes: SI is free here (height was copied to BP at line 16023; SI is saved/restored by the function prologue/epilogue). All instructions are 8086-safe (shifts by 1 or CL only) — though the kernel already requires 386+ (movzx/bt in the dispatcher) and gfx_clear_area_stub itself uses 'shr di, 2', so matching that style is also acceptable. Mask polarity matches the proven gfx_clear_area_stub code: lead keeps high bits (0xFF<<cl), trail keeps low bits (0xFF>>cl); CGA pixel 0 occupies bits 7:6. The clamp also fixes a latent corruption in the existing aligned fast path for off-screen rects (same class of bug the VGA path fixed in Build 397).

## [high/high] event_wait_stub (API 10) and kbd_wait_key (API 12) busy-spin without yielding — starves all other tasks in the cooperative scheduler
- src:performance | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:10378-10382 (event_wait_stub), 692-696 (kbd_wait_key); scheduler_next at 14802; app_yield_stub at 14848; delay_ticks yield pattern at 18880 | area:scheduler / event system
DESC: event_wait_stub is 'call event_get_stub / test al, al / jz event_wait_stub / ret' — a hard spin inside the kernel with no app_yield_stub and no HLT. The scheduler is cooperative (scheduler_next at 14725 only runs on explicit yield), so any app that calls the documented blocking wait API freezes every other running task indefinitely (they never get a timeslice) while burning 100% CPU; on an 8088 the spin also runs ~18 INT-dispatch round trips per event poll. kbd_wait_key (692-696) has the identical pattern. delay_ticks (18801-18803) shows the correct pattern: it calls app_yield_stub inside its loop. This directly explains 'apps hang/crash when several apps are running' for any app using API 10/12.
VERIFIER: CONFIRMED, with corrections to line numbers and one severity caveat. (1) The spin loops are real: kernel/kernel.asm event_wait_stub is at lines 10378-10382 (NOT 10301-10305 as cited — those lines are inside event_get_stub's BIOS extended-key translation), and is exactly 'call event_get_stub / test al,al / jz event_wait_stub / ret' with no yield and no HLT. kbd_wait_key at 692-696 (correctly cited) is the identical pattern via kbd_getchar. (2) The scheduler is purely cooperative: scheduler_next (line 14802, not 14725) is reached only via app_yield_stub (API 34, line 14848), the task-exit path (line 15080), and the initial boot switch (line 1786). There is NO timer preemption anywhere — a comment at line 20133 explicitly treats preemptive scheduling as future work. So while a task spins in API 10/12, no other RUNNING task gets any CPU until an event arrives for the spinner; the INT 0x80 dispatcher's sti (line 151) keeps ISRs alive so the focused task does eventually unblock, but a NON-focused task calling event_wait can spin indefinitely (event_get_stub filters KEY_PRESS to the focused task at 10219-10229 and leaves other tasks' WIN_REDRAW in the queue at 10254), freezing the whole system. (3) The contrast cases prove this is a defect, not a design choice: delay_ticks (18865) calls app_yield_stub at 18880 inside its wait loop, and file_dialog_open's modal loop (5183-5186) explicitly comments 'intentionally blocks all other tasks' AND uses hlt — event_wait_stub has neither the yield, nor the comment, nor the hlt. APIs 10/12 are public documented blocking calls (docs/API_REFERENCE.md:172-193) and API 10 is the recommended replacement for deprecated API 12. CAVEATS: (a) No bundled app actually calls API 10 or 12 (the 'mov ah, 10' in apps/clock.asm:336 is a MUL operand, not a syscall), so the claim that this 'directly explains' the observed multi-app hangs is overstated — current exposure is third-party apps following the docs; the bundled-app hang likely has a different cause. (b) The suggested fix needs one guard the finding missed: kernel-internal callers (kbd_wait_key from lines 1486/1651, event_wait_stub from keyboard_demo at 1990) run in the pre-scheduler fallback path where current_task=0xFF and no task is RUNNING; in that state app_yield_stub's .idle path (14927-14930: sti/hlt/jmp .no_save) never returns, so an unconditional yield would turn the boot-failure fallback demo into a hard hang. Guarding on current_task != 0xFF preserves those paths. Severity: high for any app using the documented blocking APIs; the fix is safe and follows the proven delay_ticks pattern (which already calls app_yield_stub from inside an API function dispatched via INT 0x80, with identical stack shape).
FIX: Replace event_wait_stub (kernel/kernel.asm:10378-10382) with:

; event_wait_stub - Wait for event (blocking, yields to other tasks)
; Output: AL = event type, DX = event data
; Preserves: BX, CX, SI, DI
event_wait_stub:
    call event_get_stub
    test al, al
    jnz .got_event
    ; Yield so other tasks keep running while we wait (same pattern as
    ; delay_ticks, line 18880). Guard: in pre-scheduler context
    ; (current_task=0xFF, e.g. keyboard_demo boot fallback) app_yield_stub's
    ; .idle path hlt-loops forever — keep the plain spin there.
    cmp byte [current_task], 0xFF
    je event_wait_stub
    call app_yield_stub
    jmp event_wait_stub
.got_event:
    ret

Replace kbd_wait_key (kernel/kernel.asm:692-696) with:

; Wait for keypress (blocking, yields to other tasks)
; Output: AL = ASCII character
kbd_wait_key:
    call kbd_getchar
    test al, al
    jnz .got_key
    cmp byte [current_task], 0xFF   ; pre-scheduler context: plain spin
    je kbd_wait_key
    call app_yield_stub
    jmp kbd_wait_key
.got_key:
    ret

Notes: 8086-safe (no 186+ instructions added). DS is the kernel segment (0x1000) in both functions for every caller — the INT 0x80 dispatcher sets DS=0x1000 at line 158 before dispatch, and the internal callers at 1486/1651/1990 run with DS=0x1000 — so the [current_task] access is valid. app_yield_stub preserves all GP registers across the context switch (pusha at 14852 / popa at 14924), so both functions' register-preservation contracts still hold. Optionally, 'sti / hlt' could be inserted before the jmp when yield returns to the same task (single-task case) to stop burning 100% CPU; interrupts are enabled in this path (dispatcher sti at line 151) and all event sources (IRQ1 keyboard, IRQ12 mouse, BIOS INT 16h buffer fill on HD/USB boot) are interrupt-driven, so hlt cannot deadlock — but the minimal correctness fix is the yield alone.

## [medium/high] Launcher registers up to 40 icons but kernel desktop table holds 16 — icons 17+ vanish after window operations in hi-res
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\apps\launcher.asm:apps/launcher.asm:71 (MAX_ICONS_HI equ 40), :926-932 (setup_layout hi-res), :418-420 (scan bound), :784-796 (register_icon ignores CF after int 0x80 at :789); kernel/kernel.asm:15362-15363 (slot reject), :20359 (DESKTOP_MAX_ICONS equ 16), :15761 (draw_desktop_region), :16145-16147 and :19292 (callers that erase) | area:kernel data structures / desktop icons
DESC: In hi-res the launcher allows 40 icons ('MAX_ICONS_HI equ 40', line 71; scan loop bound 'mov al, [cs:max_icons] / cmp [cs:icon_count], al', 419-420) and registers each via API 37. The kernel table only has 16 slots ('DESKTOP_MAX_ICONS equ 16', kernel.asm 20359) and desktop_set_icon_stub rejects slot >= 16 with CF=1 (15362-15363) — which register_icon never checks. So icons 17-40 exist only in the launcher's own initial paint; the kernel's draw_desktop_region knows nothing about them, and the first window move/close/redraw over that area erases them permanently (until a manual rescan). The bounds check itself is correct — this is a capacity mismatch, not corruption.
VERIFIER: CONFIRMED with one refinement. Every load-bearing claim checks out in the code:

1. Capacity mismatch is real. apps/launcher.asm:71 'MAX_ICONS_HI equ 40'; setup_layout (launcher.asm:926-932) sets max_icons=40 whenever screen width >= 640. The scan loop bound (launcher.asm:418-420) only checks the launcher-side max_icons, so up to 40 slots are filled. kernel/kernel.asm:20359 'DESKTOP_MAX_ICONS equ 16' and the table at 20366 is 16*80 bytes.

2. API 37 silently fails for slots 16-39. Dispatch table (kernel.asm:7362) maps API 37 to desktop_set_icon_stub; it rejects slot >= 16 at kernel.asm:15362-15363 (cmp al, DESKTOP_MAX_ICONS / jae .dsi_invalid -> stc, no store). register_icon (launcher.asm:737-796) executes int 0x80 at line 789 and immediately pops/rets with no jc — the CF error is discarded. register_all_icons and add_refresh_icon inherit the same silent failure.

3. The erase path is exactly as described. draw_desktop_region (kernel.asm:15761) fills the affected rect with desktop_bg_color, then redraws only icons from the kernel's 16-slot table (iterates desktop_icon_count entries, capped at 16). It is called from redraw_affected_windows (kernel.asm:16145-16147, the window move/close z-order repaint) and from win_resize (kernel.asm:19292). So any window move/close/resize that uncovers desktop area erases icons 17-40 there; the kernel cannot redraw them.

4. No hidden protection exists. I checked for other mechanisms: none. The launcher only repaints icons 17-40 itself via draw_all_icons during full desktop repaints.

REFINEMENT (finding overstated here): the loss is NOT 'permanent until a manual rescan'. The launcher auto-repaints the full desktop (all 40 icons from its own tables) when the last user app exits (had_apps transition, launcher.asm:159-165), on empty-desktop click deselect (launcher.asm:1275), on context-menu dismiss, and on sort/arrange operations. So icons 17-40 vanish only while at least one app window is open and reappear automatically when the last app closes or the user clicks empty desktop. Still a genuine, user-visible rendering bug.

SEVERITY note: medium is fair-to-generous. Trigger requires (a) hi-res mode AND (b) 17+ icons. The stock 1.44MB floppy ships 14 apps + Refresh icon = 15 icons (Makefile:165-172), so out-of-the-box builds never exceed 16; a user must add 2+ .BIN files. Suggest medium-low.

FIX FEASIBILITY verified by assembly: current kernel content ends at 0xA6EE (42734 bytes), leaving 2322 bytes of slack before the 'times 45056 - ($-$$)' pad (kernel.asm:20488). Raising DESKTOP_MAX_ICONS to 40 adds 24*80 = 1920 bytes — fits within the existing 45056-byte / 88-sector budget with 402 bytes to spare, no bootloader change needed. All kernel uses of the constant remain 8086/8-bit safe at 40 (cmp al,40; dec al loop counter; clear count cx = 40*80 = 3200).
FIX: Preferred (kernel capacity raise — preserves the 8x5 hi-res grid; verified to fit the existing 45056-byte pad with 402 bytes slack):

kernel/kernel.asm:20359
-DESKTOP_MAX_ICONS       equ 16
+DESKTOP_MAX_ICONS       equ 40

(The desktop_icons table at kernel.asm:20366, the clear loop at 15444, the count loop at 15397, and the slot check at 15362 all derive from this constant and are 8086-safe at 40. Also update the stale header comment at kernel.asm:15354 'AL = slot (0-7)'.)

Defensive hardening in the launcher (optional but cheap), apps/launcher.asm register_icon after the int 0x80 at line 789 — surface the kernel reject instead of discarding CF, e.g.:

    mov ah, API_DESKTOP_SET_ICON
    int 0x80
    jnc .ri_ok                      ; kernel accepted the slot
    ; kernel table full: nothing to do, icon remains launcher-drawn only
.ri_ok:

Alternative minimal fix if kernel growth is unwanted: clamp the launcher in setup_layout (apps/launcher.asm:932) — change 'mov byte [cs:max_icons], MAX_ICONS_HI' to 'mov byte [cs:max_icons], 16' (or redefine MAX_ICONS_HI equ 16 at line 71) — at the cost of capping hi-res desktops to 16 icons.

## [medium/high] Kernel load size is three hand-synced constants at exactly zero headroom; stage2 verifies only the head signature, so a future size bump silently truncates the kernel tail
- src:boot-memory-map | C:\Users\arin\Documents\Github\unodos\boot\stage2.asm:boot/stage2.asm:14 (KERNEL_SECTORS equ 88) and :41-45 (head-only signature check); kernel/kernel.asm:20488 (times 45056 pad); boot/boot.asm:19 (bpb_rsvd dw 94); tools/add_floppy_fs.py:20-21 (FS_START_SECTOR=94, no overlap check); Makefile:160/169/358 (seek=5) | area:boot chain / image layout
DESC: stage2.asm line 14: 'KERNEL_SECTORS equ 88' (44KB). kernel.asm line 20488 pads the image with 'times 45056 - ($ - $$) db 0' - and build/kernel.bin is exactly 45056 bytes, i.e. the kernel currently fills 100% of the 88 loaded sectors with zero free bytes. The BPB reserves 94 sectors (boot.asm line 19 'bpb_rsvd: dw 94') and tools/add_floppy_fs.py puts the FAT at sector 94 (FS_START_SECTOR = 94), so the kernel may only grow by ONE more sector (to LBA 93) before colliding with the filesystem. Today nothing is truncated (NASM errors out if code exceeds the times pad), but the failure mode is armed: the natural response to that build error is bumping the 45056 pad, and if KERNEL_SECTORS in stage2 (a different file) is not bumped in lockstep, the tail sectors are silently never loaded - stage2's only integrity check is the 2-byte 'UK' signature at the START of the image ('mov ax, [es:0] / cmp ax, KERNEL_SIG', lines 43-44), so boot proceeds and the system crashes whenever a tail-resident API (high syscall numbers, fonts, late data) is called. The Makefile comments are already two generations stale ('kernel (16KB)' line 154, 'Assemble kernel (28KB)' line 101), demonstrating that these constants do drift.
VERIFIER: Every factual claim checks out against the code. (1) boot/stage2.asm:14 has 'KERNEL_SECTORS equ 88' and the only post-load integrity check is lines 41-45: 'mov ax,[es:0] / cmp ax,KERNEL_SIG' - a 2-byte head signature that passes even if the tail was never loaded. (2) kernel/kernel.asm:20488 pads with 'times 45056 - ($-$$) db 0' (88*512 exactly) and build/kernel.bin is exactly 45056 bytes. (3) boot/boot.asm:19 has 'bpb_rsvd: dw 94' and tools/add_floppy_fs.py:21 has FS_START_SECTOR=94 with NO overlap check - it unconditionally writes the FS boot sector at LBA 94. (4) Makefile writes the kernel at seek=5, so it occupies LBA 5-92; only LBA 93 is spare before the FS - headroom is exactly 1 sector, as claimed. (5) The drift evidence is real and worse than stated: Makefile:101 says '28KB', Makefile:154 says 'sectors 6-37 = kernel (16KB)', and add_floppy_fs.py's own docstring (lines 9-12) says 'Sectors 6-61: Kernel (28KB = 56 sectors)' / 'starting at sector 62' - three stale generations across three files. The failure path is concrete: NASM hard-errors when code exceeds the times pad ('TIMES value is negative'), the developer bumps 45056 to 45568+, stage2 (a different file, no shared include - verified stage2.asm contains no %include) still loads 88 sectors, the head 'UK' signature at offset 0 still matches, and boot proceeds with the last sector(s) of the kernel never loaded into RAM at 0x1000:xxxx - crashing only when tail-resident code/data is touched. No other mechanism prevents this: no checksum, no length field, no BPB-derived count (stage2 uses the hardcoded equ). Refinements: (a) it is FIVE hand-synced locations, not three - stage2.asm:14 (88), kernel.asm:20488 (45056), boot.asm:19 (94), add_floppy_fs.py:20-21 (94), Makefile seek=5 (lines 160/169/358); (b) there is a second silent-corruption path: at >=90 sectors, add_floppy_fs.py overwrites the kernel tail at LBA 94 with the FS boot sector even if stage2 IS in sync; (c) the HD boot path (stage2_hd.asm) is unaffected - it loads KERNEL.BIN by FAT16 directory-entry size; only the floppy chain is exposed; (d) the suggested tail magic is feasible today: kernel content ends at offset 42713, leaving 2342 zero pad bytes, so the 2-byte magic fits without growing the image. Severity medium is fair: zero runtime fault today, but the trap is armed, headroom is one sector, and constant drift is already demonstrated.
FIX: 1) New file boot/layout.inc (equ-only, safe to include anywhere):
; Single source of truth for floppy boot-chain layout
KERNEL_SECTORS  equ 88          ; kernel size in 512-byte sectors (must stay <= 89)
KERNEL_LBA      equ 5           ; kernel image start LBA (= CHS sector 6)
FS_RSVD_SECTORS equ 94          ; BPB reserved sectors = FAT12 FS start (keep in sync with tools/add_floppy_fs.py)
KERNEL_TAIL_SIG equ 0x4B45      ; 'EK' end-of-kernel magic
%if (KERNEL_LBA + KERNEL_SECTORS) > FS_RSVD_SECTORS
  %error Kernel overruns reserved area: raise FS_RSVD_SECTORS in layout.inc, boot.asm BPB, and add_floppy_fs.py together
%endif

2) boot/stage2.asm: replace line 14 with '%include "boot/layout.inc"' (nasm runs from repo root, so the path resolves; KERNEL_SECTORS stays <= 255 so the byte-wide sectors_left counter is fine). After line 45's 'jne kernel_error', add a tail check (ES is already KERNEL_SEGMENT; offset 0xAFFE fits the segment; plain 8086 encoding):
    mov ax, [es:KERNEL_SECTORS*512 - 2]
    cmp ax, KERNEL_TAIL_SIG
    jne kernel_error

3) kernel/kernel.asm: replace line 20488 with:
%include "boot/layout.inc"
times (KERNEL_SECTORS*512 - 2) - ($ - $$) db 0
dw KERNEL_TAIL_SIG
(2342 bytes of pad slack exist today, so the 2-byte magic fits; output stays exactly 45056 bytes.)

4) boot/boot.asm: add '%include "boot/layout.inc"' above the BPB (emits no bytes) and change line 19 to 'bpb_rsvd: dw FS_RSVD_SECTORS'.

5) Optional hardening for the second corruption path: in tools/add_floppy_fs.py add before writing -
KERNEL_LBA = 5
kernel_end = KERNEL_LBA + os.path.getsize('build/kernel.bin') // SECTOR_SIZE
assert kernel_end <= FS_START_SECTOR, f"kernel ends at LBA {kernel_end-1}, collides with FS at {FS_START_SECTOR}"
Also fix the stale comments: Makefile lines 101/154 and add_floppy_fs.py docstring lines 5-12.

## [medium/medium] Default 360KB floppy target cannot boot: stage2 and kernel hardcode 1.44MB geometry (18 SPT / 2 heads) while make's default image is 720 sectors
- src:boot-memory-map | C:\Users\arin\Documents\Github\unodos\boot\stage2.asm:boot/stage2.asm lines 121 and 127 (hardcoded 19/2 CHS limits); also kernel/kernel.asm 11318, 11323, 11366, 11371; Makefile 53, 155-161, 178, 194 (and stale comments 101, 154) | area:boot chain / disk geometry
DESC: stage2's CHS walk hardcodes 1.44MB geometry: 'cmp byte [current_sector], 19 ... cmp byte [current_head], 2' (121-127). The kernel's floppy_read_sector likewise hardcodes 'mov bx, 18 ; Sectors per track (1.44MB floppy)' and 'mov bx, 2' heads (kernel.asm 11318-11323), and boot.asm's BPB declares 2880 sectors / 18 SPT unconditionally (lines 22, 25). But the DEFAULT Makefile target builds a 360KB image ('dd ... count=720', Makefile line 157), which QEMU/BIOS present as 40 cyl / 2 heads / 9 SPT. Loading the kernel from CHS(0,0,6) onward, stage2 requests sector 10 of a 9-sector track, gets INT 13h error 4, exhausts 3 retries and halts with 'Disk error!'. Even if it loaded, the 88-sector kernel plus 94 reserved sectors leave no FAT12 filesystem on the 360K image (the FS pass is only applied to the 1.44MB image), so auto_load_launcher would halt anyway. The 'make run' default target is therefore dead; only run144/HD images work. Not the cause of the user's runtime symptoms (those occur on the 1.44MB image) but a real boot-chain inconsistency.
VERIFIER: Confirmed by direct code reading. (1) Makefile line 53 makes the 360KB image (line 157: dd count=720) the default, and `run`/`debug` (lines 178, 194) boot it; only the 1.44MB image gets the FAT12 filesystem pass (line 171). (2) stage2.asm lines 120-132 hardcode 1.44MB geometry: `cmp byte [current_sector], 19` (line 121) and `cmp byte [current_head], 2` (line 127), with KERNEL_SECTORS=88 starting at CHS(0,0,6). (3) kernel.asm hardcodes 18 SPT / 2 heads in floppy_read_sector (lines 11318/11323) and floppy_write_sector (11366/11371). (4) boot.asm's BPB unconditionally declares 2880 sectors / 18 SPT / 2 heads / 94 reserved (lines 19-26). QEMU detects a 368,640-byte raw image as 40 cyl / 2 heads / 9 SPT (720 sectors exactly matches the 360K entry in its fd_formats table). Trace: boot sector loads stage2 from C0/H0 sectors 2-5 (valid under 9 SPT), stage2 reads kernel sectors 6-9 successfully, then requests CHS(0,0,10) which does not physically exist on a 9-SPT track; INT 13h fails, 3 retries exhaust, stage2 halts at .disk_error with 'Disk error!'. No mechanism compensates: stage2 and the kernel never read the BPB back or call INT 13h AH=08. There is also a second independent break the finding understates: the image is written LBA-linearly by dd, so even in-range CHS requests under stage2's 18-SPT model address the wrong physical sectors on 9-SPT media (its 'LBA 18' = CHS(0,1,1) = physical LBA 9). And the secondary claim holds: the 360K image is zero-filled beyond LBA 92 (add_floppy_fs.py never runs on it), so a hypothetically loaded kernel would fail auto_load_launcher and hlt-loop (kernel.asm lines 94-99). Minor inaccuracies, none load-bearing: the exact BIOS error code ('error 4') is presumed, not proven (SeaBIOS may return a different code, but carry is set either way, which is all stage2 checks), and the '88-sector kernel plus 94 reserved sectors' phrasing double-counts (the 94 reserved already include the 88 kernel sectors). Stale comments confirmed: Makefile line 101 says '28KB' kernel and line 154 says 'sectors 6-37 = kernel (16KB)' vs the real 88-sector/44KB kernel. Severity medium is fair: the default `make`/`make run` workflow is dead, but run144/run-hd work and this does not explain runtime symptoms seen on the 1.44MB image.
FIX: Minimal fix (Makefile only — make the working 1.44MB image the default and retire the broken 360K image as a bootable target):

--- Makefile
@@ line 53
-all: $(FLOPPY_IMG)
+all: $(FLOPPY_144)
@@ lines 178-183 (run target)
-run: $(FLOPPY_IMG) check-qemu
+run: $(FLOPPY_144) check-qemu
 	$(QEMU) -M isapc \
 		-m 640K \
-		-drive file=$(FLOPPY_IMG),format=raw,if=floppy \
+		-drive file=$(FLOPPY_144),format=raw,if=floppy \
@@ lines 194-200 (debug target)
-debug: $(FLOPPY_IMG) check-qemu
+debug: $(FLOPPY_144) check-qemu
 	$(QEMU) -M isapc \
 		-m 640K \
-		-drive file=$(FLOPPY_IMG),format=raw,if=floppy \
+		-drive file=$(FLOPPY_144),format=raw,if=floppy \

(test-fat12/test-fat12-multi/test-app at lines 203-251 also boot $(FLOPPY_IMG) and need the same substitution, or deletion along with the $(FLOPPY_IMG) rule at lines 155-161). Also fix stale comments: line 101 '(28KB)' -> '(44KB)'; line 154 'sectors 6-37 = kernel (16KB)' -> 'sectors 6-93 = kernel (88 sectors / 44KB)'.

If a real 360K target is wanted instead, three coordinated changes are required (not minimal): a 360K BPB variant in boot.asm (bpb_sectors 720, bpb_spt 9, bpb_media 0xFD), geometry read from the in-memory boot sector instead of constants in stage2.asm (8086-safe; boot sector is still intact at 0:0x7C00 because the kernel loads at 0x1000:0000):

    ; once at load_kernel entry (DS=0x0800):
    push es
    xor ax, ax
    mov es, ax
    mov al, [es:0x7C00+24]      ; BPB sectors/track (low byte)
    inc al
    mov [spt_plus1], al         ; compare limit = SPT+1
    mov al, [es:0x7C00+26]      ; BPB head count (low byte)
    mov [num_heads], al
    pop es
    ...
    ; replace line 121:  cmp byte [current_sector], 19
    mov al, [current_sector]
    cmp al, [spt_plus1]
    ; replace line 127:  cmp byte [current_head], 2
    mov al, [current_head]
    cmp al, [num_heads]

plus the same parameterization in kernel.asm floppy_read_sector/floppy_write_sector (lines 11318/11323 and 11366/11371) and running tools/add_floppy_fs.py on the 360K image with 360K FAT parameters.

## [medium/high] EVENT_MOUSE is consumed by whichever task polls first - launcher silently eats mouse events meant for apps
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm:10166-10169 (.evt_not_key: 'cmp al, EVENT_WIN_REDRAW / jne .evt_consume' - the unconditional-consume fallthrough); KEY_PRESS focus filter at 10151-10164; consume point 10188-10192; launcher discard at apps\launcher.asm:269-273 | area:event system / mouse input
DESC: The per-task filtering in event_get_stub covers only KEY_PRESS (focus check, 10142-10152) and WIN_REDRAW (owner check, 10161-10177). At 10157-10160 (`cmp al, EVENT_WIN_REDRAW / jne .evt_consume`) every other type - notably EVENT_MOUSE - is consumed unconditionally by the first task that calls event_get. The launcher polls API 9 every loop iteration and discards anything that is not KEY_PRESS (apps/launcher.asm:269-273 `jc .no_event / cmp al, EVENT_KEY_PRESS / jne .no_event`), so with N running tasks each mouse event reaches the focused app only ~1/N of the time; click edges delivered via EVENT_MOUSE are randomly lost (mouse_test.asm and other EVENT_MOUSE-driven apps). Apps that poll API_MOUSE_GET_STATE instead can also miss short click edges between their turns. This directly produces 'mouse input issues' that get worse the more apps run.
VERIFIER: Confirmed by direct code reading. (1) EVENT_MOUSE (type 4) is posted to the single global 32-slot event queue on EVERY mouse packet - movement and button changes - from both IRQ paths (kernel\kernel.asm:1138-1142 native IRQ12, 1270-1274 BIOS callback), with DL=buttons. (2) In event_get_stub, per-task filtering exists only for EVENT_KEY_PRESS (focus check, 10151-10164, leaves event queued for the focused task) and EVENT_WIN_REDRAW (owner check, 10166-10186). Every other type falls through 'cmp al, EVENT_WIN_REDRAW / jne .evt_consume' at 10168-10169 (the finding's cited 10157-10160 is slightly off) and is consumed unconditionally - head advanced at 10188-10192 - by whichever task polls first. (3) The launcher's cooperative main loop polls API_EVENT_GET every iteration whenever a WINDOWED app has focus (apps\launcher.asm:171-172 routes bl!=0xFF to .input_ok; only fullscreen apps skip via line 174), and discards anything that is not KEY_PRESS (269-273). Since the launcher reads its own mouse input via API_MOUSE_GET_STATE polling (178-183), every EVENT_MOUSE it consumes is pure loss. (4) Real victims: apps\music.asm (windowed, WIN_CREATE at 143) does click edge detection off the EVENT_MOUSE payload (305-314, 362-367: test dl,1 + prev_btn edge); a stationary click produces exactly two packets (down, up), so if the launcher eats the button-down event the click is lost entirely. docs\APP_DEVELOPMENT.md:212 documents EVENT_MOUSE as the standard app pattern, so this is an API-contract bug. With N event-polling tasks the focused app sees ~1/N of mouse events, exactly as claimed. (5) No other mechanism prevents it: event_queue_head/tail (20226-20227) have no other consumer, there is no per-task routing or re-posting for type 4. The secondary claim (API_MOUSE_GET_STATE pollers can miss click edges shorter than one scheduler round, since mouse_buttons is a live byte overwritten in IRQ context) is also technically true but a much weaker effect. Severity medium is fair. Caveat for the fix: mirroring the KEY_PRESS leave-in-queue behavior inherits the existing head-of-line property - if the focused task stops draining events, high-rate mouse-movement events will sit at queue head and stall WIN_REDRAW delivery to other tasks until the queue fills (post_event then drops new events at 10113-10114, no corruption). All in-tree windowed apps drain events every loop, but this fix should ideally land together with the companion head-of-line/scan-past fix.
FIX: In kernel\kernel.asm event_get_stub, route EVENT_MOUSE through the same focus filter as KEY_PRESS. Replace lines 10166-10169:

.evt_not_key:
    ; Filter: skip WIN_REDRAW events not for current task's window
    cmp al, EVENT_WIN_REDRAW
    jne .evt_consume                ; Other event types: consume and pass through

with (all 8086-safe, DS=0x1000 already set):

.evt_not_key:
    ; Filter mouse events: only deliver to focused task (like KEY_PRESS)
    cmp al, EVENT_MOUSE
    jne .evt_not_mouse
    push ax
    mov al, [focused_task]
    cmp al, 0xFF                    ; No window focused? Deliver to current task
    je .evt_mouse_ok
    cmp al, [current_task]
    je .evt_mouse_ok
    pop ax
    jmp .no_event                   ; Not focused - leave queued for focused task
.evt_mouse_ok:
    pop ax
    jmp .evt_consume
.evt_not_mouse:
    ; Filter: skip WIN_REDRAW events not for current task's window
    cmp al, EVENT_WIN_REDRAW
    jne .evt_consume                ; Other event types: consume and pass through

This is behavior-preserving when no window is focused (focused_task==0xFF: first poller still gets it, which is the desktop/launcher-only case) and delivers mouse events exclusively to the focused windowed app otherwise. The launcher needs no change - it already uses API_MOUSE_GET_STATE for desktop clicks and merely discarded these events. Should be combined with the separate head-of-line fix (scan past non-deliverable events instead of returning .no_event) so queued mouse events for a slow focused task cannot stall WIN_REDRAW delivery to other tasks.

## [medium/high] Mouse packet flood fills 31-slot event queue and drops keyboard events; no mouse-event coalescing
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm 10112-10115 (drop-newest in post_event), 1138-1142 (int_74_handler post), 1270-1274 (mouse_bios_callback post), 619-624 (INT 9 KEY_PRESS post into same queue) | area:event system / input / performance
DESC: int_74_handler posts one EVENT_MOUSE per 3-byte PS/2 packet (1138-1142), i.e. 40-200 events/sec while the mouse moves. post_event (10091-10106) drops the NEWEST event when the 32-entry (31 usable) queue is full (`cmp bx,[event_queue_head] / je .done`). Because EVENT_MOUSE events carry only the button state (DL) - position is read separately via mouse_get_state - consecutive move events are pure duplicates, yet they are all queued. Moving the mouse while any app is slow to drain events fills the queue, and subsequently typed KEY_PRESS events are silently discarded: keystrokes vanish while the mouse moves. Combined with the head-of-line issue this is a concrete cause of the reported keyboard problems.
VERIFIER: Confirmed by direct code reading of C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. (1) post_event (10093-10122) drops the NEWEST event when full: after storing at the tail slot it does `inc bx / and bx,0x1F / cmp bx,[event_queue_head] / je .done` (10112-10115) and never advances the tail, so the just-posted event is lost. Queue is `times 96 db 0` = 32 x 3-byte events, 31 usable (20225-20227). (2) One EVENT_MOUSE is posted per complete 3-byte packet with data = button state only (`xor dx,dx / mov dl,[mouse_buttons]`) at 1138-1142 (KBC int_74_handler path) AND at 1270-1274 (mouse_bios_callback, the INT 15h/C2 path the finding missed; install_mouse at 745-890 installs exactly one of the two). Position is never carried in the event (apps poll mouse_get_state at 1294), so consecutive move-only events are bit-identical duplicates. No coalescing exists anywhere (only these two EVENT_MOUSE post sites; no last-event tracking). (3) Keyboard shares the same queue: the custom INT 9 handler is installed on ALL boot types (438-454) and posts EVENT_KEY_PRESS via post_event (619-624); use_bios_keyboard is never set to 1 anywhere (only init to 0 at 454 and `db 0` at 1430), so the INT 16h fallback in event_get_stub is dead code and the event queue is the primary key path. (4) Failure path is concrete: KBC path sends 0xF6 (defaults = 100 Hz sample rate), so continuous motion produces ~100 packets/sec and fills the 31-slot queue in ~310 ms whenever the consumer is slow - and the consumer IS slow exactly while the mouse moves, because event_get_stub runs mouse_process_drag (10136, window move/redraw) before consuming each event; additionally a KEY_PRESS at the head is left in queue when a non-focused task polls (10162), letting mouse events pile up behind it. Once full, INT 9's post_event hits `je .done` and the keystroke is silently discarded from the event path (the legacy 16-byte kbd_buffer at 609-616 still gets it, but event-driven apps - the primary path per the comment at 438-441 - never see it). Corrections to the finding: the drop is at 10112-10115 (not 10091-10106), and 1270-1274 is a second posting site that must also be covered - fixing inside post_event covers both. Medium severity is fair. The suggested coalescing fix is safe: EVENT_MOUSE is posted only from IRQ12 context (no nesting, IF=0 in handler), and mainline post_event callers only post other event types (all EVENT_WIN_REDRAW), which skip the coalesce branch; coalescing only when the data word is IDENTICAL preserves every button press/release edge while collapsing the move flood to at most one pending duplicate.
FIX: In post_event (kernel\kernel.asm), insert a duplicate-EVENT_MOUSE check between `mov ds, bx` (10099) and the tail-offset calculation (10101-10105), and change the offset calc to start from the already-loaded BX. All instructions are 8086-safe; BX/SI/DS are already saved by the existing prologue. Replace lines 10101-10105:

    ; Calculate tail position (events are 3 bytes each)
    mov bx, [event_queue_tail]
    mov si, bx
    add si, bx                      ; SI = tail * 2
    add si, bx                      ; SI = tail * 3

with:

    mov bx, [event_queue_tail]

    ; Coalesce duplicate EVENT_MOUSE events: if the newest queued event
    ; is already EVENT_MOUSE with identical data (button state), drop
    ; this one. Move-only packets carry no position (apps poll
    ; mouse_get_state), so they are pure duplicates. Prevents 100Hz
    ; packet floods from filling the 31-slot queue and silently
    ; discarding KEY_PRESS events posted by the INT 9 handler.
    cmp al, EVENT_MOUSE
    jne .store
    cmp bx, [event_queue_head]      ; Queue empty?
    je .store                       ; Yes - nothing to coalesce with
    mov si, bx
    dec si
    and si, 0x1F                    ; SI = index of newest queued event
    push bx
    mov bx, si
    add si, bx
    add si, bx                      ; SI = index * 3
    pop bx
    cmp byte [event_queue + si], EVENT_MOUSE
    jne .store
    cmp dx, [event_queue + si + 1]  ; Identical button state?
    je .done                        ; Pure duplicate - drop new event

.store:
    ; Calculate tail position (events are 3 bytes each)
    mov si, bx
    add si, bx                      ; SI = tail * 2
    add si, bx                      ; SI = tail * 3

The rest of post_event (store at 10107-10109, advance/full-check at 10111-10116, .done epilogue) is unchanged. Coalescing only on an IDENTICAL data word preserves every button press/release edge (a buttons-changed event is never merged away) while collapsing motion floods to a single pending event. This one change covers both IRQ12 paths (int_74_handler and mouse_bios_callback) since both post via post_event. Optionally, the same head-room could be improved further by drop-oldest-EVENT_MOUSE-when-full, but with coalescing the queue can no longer be flooded by motion alone, so that is not required.

## [medium/high] app_load_stub has no file-size validation: high word ignored, no check against the 0xFFE0 stack frame area, partial reads and zero-size files accepted
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: size fetch ignoring high word at 14500-14509 (mov cx,[bx+4]); read with CF-only check and AX (bytes-read) discarded at 14514-14523; unchecked 0xFFE0-0xFFFE frame writes in app_start_stub at 14906-14926 | area:app loading
DESC: Step 5 reads only the LOW word of the file size: `mov cx,[bx+4]` with the comment 'Size is at offset 4 (low) and 6 (high)' (14491-14499) - the high word at offset 6 is never checked, so a >=64KB .BIN loads only size&0xFFFF bytes and then executes a truncated image. There is also no check that the size fits below 0xFFE0: app_start_stub later writes the 32-byte initial stack frame at 0xFFE0-0xFFFE (14896-14916), silently overwriting the tail of any app larger than 65504 bytes, and the running app's stack then grows down over its own code/data. A zero-length or partially read file (fs_read_stub's CF is checked but the returned byte count is not compared to file_size at 14508-14510) leaves stale bytes from a previously freed segment to be executed - the segment is never cleared on alloc. Each case produces an immediate crash 'when launching apps'.
VERIFIER: All four sub-claims hold after tracing app_load_stub (C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:14391-14599), app_start_stub (14868-14948), and the FS callees.

1) High word ignored - CONFIRMED. Line 14508 `mov cx, [bx + 4]` reads only the low word; [bx+6] is never consulted. The high word CAN be nonzero: fat12_open stores the full 32-bit directory size at file_table offsets 4/6 (11104-11106, 11137-11138) and fat16_open does the same (12392-12393, 12428-12429). For a file of N >= 65536 bytes the load requests N & 0xFFFF bytes; fat12_read clamps to the low-word remaining (11676-11691, with explicit comment 'files > 64KB not supported yet' - DX high word loaded then discarded) and fat16_read clamps its 32-bit remaining to the 16-bit request (12712-12724). Both return CF=0, so a truncated image is marked LOADED and later executed. Degenerate case: N == 65536 exactly gives CX=0, zero bytes read, success, and execution of whatever stale bytes are in the segment.

2) No check against 0xFFE0 - CONFIRMED. app_start_stub writes the 32-byte initial frame at ES:0xFFE0-0xFFFE (14906-14922) and sets initial SP=0xFFE0 (14926) without ever consulting the stored code size (app_table offset 6, written at 14538-14539 but never read for bounds). A .BIN of size 0xFFE1-0xFFFF has its tail silently overwritten at start; anything near the limit is then eaten by normal stack growth. One refinement vs. the finding: segment_pool entries are 0x1000 paragraphs (64KB) apart (20346: 0x3000..0x7000; shell fixed at 0x2000, kernel at 0x1000), so corruption is confined to the app's own segment - the kernel and neighbor apps are NOT hit, which supports the 'medium' severity (reliability bug, not kernel memory corruption).

3) Partial read accepted - CONFIRMED. fs_read_stub returns actual bytes read in AX with CF=0 (10477-10524); app_load_stub checks only CF (14520) and clobbers AX with the file handle at 14523 without comparing to .file_size. fat12_read exits its cluster loop with success when get_next_cluster signals end-of-chain (11779-11780) even if fewer than the requested bytes were copied, so a corrupted/inconsistent FAT chain (dir entry size larger than chain) produces a silently short load that is then executed.

4) Zero-size file accepted, segment never cleared - CONFIRMED. file_size=0 makes CX=0; fat12_read's cluster loop exits immediately returning AX=0/CF=0 (11705-11709, 11785-11800); fat16_read hits .eof returning AX=0/CF=0 (12712-12715, 12836-12838). The app is marked LOADED and app_start IRETs to seg:0000. alloc_segment (14328-14352) only flips an owner byte and free_segment only marks 0xFF - neither touches segment memory, so the executed bytes are stale content of a previously freed segment.

No compensating mechanism exists: app_load_stub is INT 0x80 API 18 (dispatch table line 7331) reachable with arbitrary filenames from the shell or any app, and app_start_stub (API 35) validates only handle range and LOADED state. Refined locations: size fetch 14500-14509; read + CF-only check 14514-14523; frame writes 14906-14926.

One nitpick on the suggested fix: the original finding proposed `[bx+4] > 0xFF00` as the cap; the hard correctness boundary is 0xFFE0 (frame base) - anything stricter is stack-headroom policy. Also note the fix must close the already-open file handle on the size-failure path (the existing .read_failed label shows the required pattern). 'push word imm' used in the fix is 186+, but this exact function already uses it (line 14572) and fat16_read uses 386 instructions (movzx/eax), so it matches the codebase's real CPU baseline.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, three insertions inside app_load_stub:

(A) After line 14509 (`mov [.file_size], cx` - DS is already 0x1000 here, BX points at the file_table entry):

    ; Validate file size: high word must be 0 and 1 <= size <= 0xFFE0
    ; (app_start_stub builds the initial stack frame at FFE0-FFFE)
    cmp word [bx + 6], 0            ; 32-bit size high word
    jne .bad_size
    test cx, cx                     ; reject zero-length files
    jz .bad_size
    cmp cx, 0xFFE0
    ja .bad_size

(B) After line 14520 (`jc .read_failed`), verify the actual byte count:

    cmp ax, [.file_size]            ; fs_read_stub returns bytes read in AX
    jne .read_failed                ; short read = truncated/corrupt FAT chain

(.read_failed at 14570 already closes the handle and frees the segment, so it is safe to reuse.)

(C) Add the size-error label next to .read_failed (file is open at this point, must close it):

.bad_size:
    push word APP_ERR_ALLOC_FAILED  ; or define APP_ERR_TOO_BIG equ 8 at line 14322
    mov ax, [.file_handle]
    call fs_close_stub
    pop ax
    jmp .error_free_seg

Optional hardening (not required for correctness): zero the segment after alloc_segment succeeds (xor di,di / mov es,bx / xor ax,ax / mov cx,0x8000 / rep stosw) so stale bytes from a freed segment can never execute even via future load paths.

## [medium/medium] Cursor hide/lock race: dispatcher hides cursor BEFORE incrementing cursor_locked, IRQ12 can redraw it in the gap
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm: 192-193 (dispatcher prologue, primary race); 1136 (int_74_handler redraw, correct as cited); 15117-15118 (win_create_stub, not 15107-15108); 16389-16390 (win_destroy_stub, not 16378-16380) | area:mouse cursor / graphics
DESC: The drawing-API prologue does `call mouse_cursor_hide` then `inc byte [cursor_locked]` (192-193) with interrupts enabled (sti at 151). mouse_cursor_hide/show are individually atomic (pushf/cli, 3735-3770) but the PAIR is not: if IRQ12 fires between the hide and the inc, int_74_handler's mouse_cursor_show (1136) redraws the XOR cursor (lock still 0), then the lock is taken and the API draws with the cursor sprite on screen. The later erase XORs stale cursor pixels over freshly drawn content, leaving inverted-rectangle garbage - classic intermittent 'visual anomalies' while moving the mouse during redraws. The same hide-then-lock pattern is repeated at every drawing entry point (win_create_stub 15107-15108, win_destroy_stub 16379-16380, widget draws at 4469/4532/4580/4640, etc.), so the race window occurs on every protected draw.
VERIFIER: Confirmed by reading C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. The dispatcher's drawing-API prologue (lines 192-193) executes `call mouse_cursor_hide` then `inc byte [cursor_locked]` with interrupts enabled (sti at line 151, intentionally re-enabled for floppy IRQ6/DMA). mouse_cursor_hide (3735-3770) and mouse_cursor_show (3776-3810) are each internally atomic via pushf/cli/popf, but interrupts are re-enabled the moment hide's popf/ret executes, leaving a ~2-instruction window before the inc. If the 3rd byte of a PS/2 packet arrives in that window, int_74_handler (1019) runs: its mouse_cursor_show at line 1136 sees cursor_locked=0, mouse_enabled=1, cursor_visible=0 (just erased), so it redraws the cursor and sets cursor_visible=1. The dispatcher then takes the lock and the API draws with the cursor sprite live on screen. At the epilogue (388-391) dec+show does NOT repaint correctly: mouse_cursor_show skips because cursor_visible is already 1 (3783-3784). The damage surfaces on the next mouse move: in VGA mode 0x13/VESA, mouse_cursor_hide restores the cursor save buffer captured BEFORE the API drew, stamping stale background pixels over fresh content (cursor_restore_vga/vesa); in CGA/Mode12h the second XOR over API-overwritten pixels leaves inverted-rectangle garbage. No other mechanism prevents it: IRQ12 is unmasked during dispatch, there is no cli around the pair, and simply reversing the order would make hide a no-op because it skips when cursor_locked != 0 (3738-3739). The same exposed pair exists at ~35 sites; pairs reached under the dispatcher's outer lock are shielded (IRQ show is a no-op when lock>=1), but the dispatcher pair is always outermost and exposed, and several stubs (win_create/win_destroy/taskbar/menu draws) are also reachable from kernel event-loop code with lock=0. Minor corrections: cited 15107-15108 is actually 15117-15118 (win_create_stub) and 16378-16380 is actually 16389-16390 (win_destroy_stub). The BIOS PS/2 callback path (mouse_cursor_show at 1268) has the same exposure as int_74_handler. Severity medium is fair: intermittent visual corruption only, no memory unsafety. The unlock side (dec then show, e.g. 390-391) is race-free as ordered and needs no change.
FIX: Add an atomic protect helper next to mouse_cursor_hide (after line 3770) and replace every two-instruction pair `call mouse_cursor_hide` / `inc byte [cursor_locked]` with `call mouse_cursor_protect`:

; mouse_cursor_protect - Atomically hide cursor AND take the lock.
; The hide+inc pair must not be interruptible: with IF=1, IRQ12 can
; fire between them and int_74_handler's mouse_cursor_show redraws
; the cursor while cursor_locked is still 0, leaving a live cursor
; under the protected draw (stale save-buffer restore / XOR garbage
; on the next mouse move).
; Preserves all registers and the caller's IF state. 8086-safe.
mouse_cursor_protect:
    pushf                           ; Save interrupt state
    cli                             ; Atomic: hide + lock
    call mouse_cursor_hide          ; Nested pushf/cli is harmless (popf restores IF=0)
    inc byte [cursor_locked]
    popf                            ; Restore caller's IF
    ret

Replace the pair at each site (the immediate hide+inc pairs found at: 192-193 dispatcher prologue; 1367-1368 win_begin_draw; 4469-4470, 4531-4532, 4579-4580, 4639-4640 widget draws; 7498-7499, 7524-7525, 7928-7929, 8166-8167, 8250-8251, 8298-8299, 8401-8402, 8591-8592, 8672-8673, 8868-8869, 9214-9215, 9391-9392, 9539-9540, 9644-9645, 9828-9829, 9844-9845, 9859-9860; 15117-15118 win_create_stub; 15503-15504, 15558-15559, 15638-15639, 15964-15965; 16389-16390 win_destroy_stub; 17469-17470, 17885-17886, 18193-18194, 18284-18285, 18323-18324, 18530-18531), e.g. dispatcher lines 192-193 become:

    call mouse_cursor_protect

Semantics are unchanged for nested protects: when cursor_locked is already >0 the inner hide still skips (cursor already hidden by the outer protect) and the counter still increments. The unlock sequence (dec byte [cursor_locked] / call mouse_cursor_show) is already safe in that order and needs no change: if IRQ12 fires between dec and show, the IRQ draws the cursor and the epilogue show correctly skips via the cursor_visible check.

## [medium/high] event_get_stub clobbers CX (and DX) in the no-event path despite documented 'Preserves: BX, CX, SI, DI'
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: header 10124-10126; .no_event_return label 10296; offending CX writes 10300-10301 (DX writes 10298-10299 are contract-legal) | area:event system / syscall ABI
DESC: The header at 10114-10116 documents 'Preserves: BX, CX, SI, DI', but the no-event exit writes debug data into CX and DX: `mov dh,[focused_task] / mov dl,[current_task] / mov ch, byte [event_queue_head] / mov cl, byte [event_queue_tail]` (10288-10291). Any app keeping a counter/pointer in CX across an API 9 poll loop gets it silently corrupted on every empty poll - a plausible contributor to sporadic app misbehavior since event_get is the hottest syscall in every app main loop. These look like leftover debug outputs.
VERIFIER: CONFIRMED for CX (the core claim), REFUTED for the DX half, with corrected line numbers and a downgraded real-world impact.

Verified facts (C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm):
1. The header at lines 10124-10126 reads "Output: AL = event type (0 if no event), DX = event data / Preserves: BX, CX, SI, DI". The finding cited 10114-10116; actual is 10124-10126.
2. event_get_stub's prologue (10128-10130) pushes only BX/SI/DS — CX is never saved anywhere in the function.
3. The .no_event_return block is at 10296; the four diagnostic MOVs are at lines 10298-10301 (finding cited 10286-10292): mov dh,[focused_task] / mov dl,[current_task] / mov ch,[event_queue_head] / mov cl,[event_queue_tail]. So CX is clobbered with (head<<8)|tail on every empty poll.
4. The clobber escapes to apps. The INT 0x80 dispatcher restores BX/CX only when _did_translate=1, which requires the API to be in api_drawing_bitmap (lines 162-176, 386-404). API 9 falls in bitmap byte 1, which is 0x00 (line 20091, "Non-drawing APIs (events, filesystem, yield) skip all of this"), so the dispatcher restores only DS and propagates the corrupted CX to the caller via IRET.
5. DX is NOT a violation: both the source header (10125) and docs/API_REFERENCE.md (lines 152-158) declare DL/DH as outputs of API 9, and DX is absent from the preserve list. Writing diagnostics into DX when CF=1 is contract-legal (output is simply undefined/diagnostic in the no-event case). Only the CH/CL writes violate the documented contract.

Impact calibration (why "medium / plausible contributor to sporadic app misbehavior" is overstated for current code): I inspected every in-tree consumer. App main loops (hello.asm:90, pacman.asm:170, tetris.asm:78, outlast.asm:100, plus notepad/browser/clock/music/launcher patterns) keep all loop state in CS-relative memory variables, never in CX across the INT 0x80 poll. Kernel-internal callers (clear_kbd_buffer at 708, modal file dialogs at 5189/5873, event_wait_stub at 10312) also don't hold live CX across the call. So nothing in-tree observably misbehaves today; this is a real but latent ABI bug that bites any future/third-party app written against the kernel header's "Preserves: CX" promise. Note the public API_REFERENCE.md makes no preservation promise at all for API 9.

Two aggravating details found while verifying:
- event_wait_stub (API 10, lines 10308-10314) carries the same "Preserves: BX, CX, SI, DI" header and just loops on event_get_stub; any wait that iterates at least once returns SUCCESS with CX already corrupted from the last empty poll. So API 10's success path violates its contract too — fixed automatically by fixing event_get_stub.
- Independent of the debug MOVs, mouse_process_drag (called unconditionally at event_get_stub entry, line 10135) clobbers CX without saving it in its Phase-3 active-drag path (mov cx,[drag_target_y], ~line 4456 onward). During a live window drag, CX is corrupted even on the event-available path. Deleting the debug MOVs does not cover this; only a push/pop cx in event_get_stub does.

The four MOVs look like exactly what the finding says: leftover debug taps (CHANGELOG shows a history of adding/removing diagnostic output; no app or kernel code reads these values after a CF=1 return).
FIX: Minimal contract-restoring fix (covers the drag-path clobber too, 8086-safe): save/restore CX in event_get_stub and delete the two CX debug MOVs.

In kernel/kernel.asm:

1. Delete lines 10300-10301 in .no_event_return:
    mov ch, byte [event_queue_head] ; CH = queue head index
    mov cl, byte [event_queue_tail] ; CL = queue tail index
(The DH/DL MOVs at 10298-10299 are contract-legal since DX is an output register; delete them as well only if the diagnostic is unwanted.)

2. To also honor "Preserves: CX" during active window drags (mouse_process_drag Phase 3 clobbers CX unsaved), add CX to the prologue/epilogues of event_get_stub:

event_get_stub:
    push bx
    push cx            ; ADD
    push si
    push ds
...
; all three exit sequences (.evt_return at ~10195, .bios_key_ready exit at ~10291, .no_event_return at ~10302) become:
    pop ds
    pop si
    pop cx             ; ADD
    pop bx
    ret

If only the cited bug is to be fixed with zero risk, step 1 alone (deleting the two CH/CL MOVs) is sufficient and is the truly minimal change.

## [medium/high] win_focus syscall (API 23) raises a window logically but never repaints it: title stays inactive, stale pixels on top
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:17833-17857 — after `.already_top:` sets z=15 and updates focused_task/topmost cache, the function returns without ever repainting the raised window; the only repaint on this path is of the OLD topmost at 17823-17829. Syscall exposure: line 7338. Compensating internal callers: 4332-4381 (mouse_process_drag) and 16457-16477 (win_destroy promote). | area:window manager / focus repaint
DESC: win_focus_stub demotes others and redraws the OLD topmost as inactive (17823-17831 'call win_draw_stub ; Now draws with inactive style'), then sets the target z=15 (17835) and updates focused_task/topmost cache — but it never redraws the newly focused window's frame (its title bar remains in inactive style), never clears its content, and never posts EVENT_WIN_REDRAW. Kernel-internal callers compensate (mouse_process_drag lines 4341-4375 does focus+draw+clear+post; win_destroy promote path 16470-16477 does focus+draw+post), but an app calling API 23 directly (it is exposed in the syscall table, line 7338 'dw win_focus_stub ; 23: Bring window to front') gets a window that is logically topmost yet still rendered as an inactive background window with the previous topmost's pixels still covering it. Spec check item 6: only the OLD window is repainted on this path, not the new one.
VERIFIER: Confirmed by direct reading of C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. Every factual claim in the finding checks out:

1. win_focus_stub (lines 17784-17867) does: validate handle (17791-17802), demote all other visible windows (17808-17821), redraw ONLY the old topmost frame (17823-17831, now inactive style since its z dropped to 14), set target z=15 (17835), update focused_task (17837-17841) and the topmost bounds cache (17843-17854), then return. There is no win_draw_stub call for the TARGET window, no content clear, and no post_event(EVENT_WIN_REDRAW) anywhere in the function.

2. The title style is z-dependent: win_draw_stub chooses active vs inactive at lines 17531-17532 (`cmp byte [bx + WIN_OFF_ZORDER], 15 / jne .draw_inactive_titlebar`). Since the target's frame is never redrawn after z=15 is set, its title bar stays in whatever (inactive) style it last had, and the old topmost's pixels remain over it where they overlap.

3. It is reachable by apps: syscall table entry at line 7338 (`dw win_focus_stub ; 23: Bring window to front`). The INT 0x80 dispatcher (lines 121-371) adds nothing for API 23: it is NOT in api_drawing_bitmap (bytes covering APIs 8-23 are 0x00 at lines 20081-20082), and there is no post-dispatch repaint hook. docs/API_REFERENCE.md lines 412-420 document API 23 simply as "Bring a window to the front (raise z-order to topmost)" with no caller-must-redraw contract.

4. Both kernel-internal callers compensate exactly as claimed: mouse_process_drag focus path lines 4332-4381 (win_focus_stub + win_draw_stub + gfx_clear_area_stub on content + post EVENT_WIN_REDRAW, with clip_enabled saved/zeroed) and the win_destroy promote path lines 16457-16477 (win_focus_stub + win_draw_stub + post EVENT_WIN_REDRAW). No other mechanism (dispatcher, event system, timer) repairs the screen after a bare API 23 call.

Concrete failure path: an app issues INT 0x80 with AH=23, AL=handle of its visible non-topmost window. Result: the OLD topmost is demoted and its frame actively repainted in inactive style (painting OVER the target where they overlap), the target becomes z=15 / focused_task / topmost-cache owner, but not one pixel of it is repainted. Net visual state: NO window shows an active title bar, keyboard focus has silently moved to a window still buried under stale pixels. This is worse than a no-op visually. The app can only repair it by also calling API 22 + repainting its content (its draws now pass the dispatcher z-clip at lines 242-251), but that obligation is undocumented.

Mitigating context (severity calibration): grep across the repo (apps/SDK/docs) found NO in-tree app that calls API 23, so the bug is latent today - nothing shipped misbehaves. "Medium" is defensible for an exposed, documented syscall with a broken visual contract; "medium-low" would also be fair given zero current callers.

One caveat on the suggested fix: any added repaint block must save/zero clip_enabled (both kernel callers do this because a stale app clip rect clips title text) and should wrap draw+clear in mouse_cursor_hide/cursor_locked (gfx_clear_area_stub's mode-specific paths at 9637-9642 branch away BEFORE its internal cursor hide at 9644, so it is not self-protecting in VGA/VESA modes - win_move_stub sets the precedent at 17875-17876). Also DX must be preserved (win_focus_stub's prologue does not save DX, and post_event needs DX). The fix below handles all three, plus skips the repaint in the .already_top case via a static flag (note: static locals are the established pattern in this kernel, e.g. win_draw_stub's .win_ptr at 17777, with the same pre-existing re-entrancy caveats). After applying, the two kernel callers (4341-4375, 16464-16477) can optionally be reduced to a bare win_focus_stub call to avoid a double clear/redraw/event (cosmetic flicker only, not a correctness issue).
FIX: Three edits to kernel/kernel.asm, all 8086-safe (post_event: AL=type DX=data, preserves all; gfx_clear_area_stub: BX=X CX=Y DX=W SI=H, preserves all).

EDIT 1 - replace lines 17804-17806 (DS=0x1000 already set at 17795-17796):

    ; Already on top? Skip demotion and repaint
    mov byte [.raised], 0
    cmp byte [bx + WIN_OFF_ZORDER], 15
    je .already_top
    mov byte [.raised], 1

EDIT 2 - insert after line 17854 (`pop si`, end of topmost-cache update), before `clc` (17856):

    ; Repaint the newly raised window (frame now draws active: z=15),
    ; clear its content, and post EVENT_WIN_REDRAW so the owning app
    ; repaints. Mirrors mouse_process_drag (4341-4375) / win_destroy
    ; promote (16464-16477) so a bare API 23 call is visually complete.
    cmp byte [.raised], 0
    je .no_repaint
    push ax                         ; preserve handle / caller AX
    push dx                         ; prologue does not save DX

    call mouse_cursor_hide          ; keep draw+clear atomic vs cursor
    inc byte [cursor_locked]        ; (gfx_clear VGA/VESA paths don't self-hide)

    ; Stale clip rect from calling task can clip the title text
    push word [clip_enabled]
    mov byte [clip_enabled], 0
    push ax
    call win_draw_stub              ; AL = handle, active-style frame
    pop ax
    pop word [clip_enabled]

    ; Clear content area (inside border / below title bar when framed)
    mov cx, [bx + WIN_OFF_Y]
    mov dx, [bx + WIN_OFF_WIDTH]
    mov si, [bx + WIN_OFF_HEIGHT]
    test byte [bx + WIN_OFF_FLAGS], WIN_FLAG_TITLE | WIN_FLAG_BORDER
    push bx                         ; push/mov leave flags intact
    mov bx, [bx + WIN_OFF_X]
    jz .clear_ready                 ; frameless: clear full rect
    inc bx                          ; inside left border
    add cx, [titlebar_height]       ; below title bar
    sub dx, 2                       ; inside both borders
    sub si, [titlebar_height]
    dec si                          ; above bottom border
.clear_ready:
    test si, si
    jz .skip_clear
    call gfx_clear_area_stub        ; preserves all registers
.skip_clear:
    pop bx                          ; restore window-table pointer

    dec byte [cursor_locked]
    call mouse_cursor_show

    ; Notify owning app to repaint its content
    xor dx, dx
    mov dl, al                      ; DX = window handle (AL intact)
    mov al, EVENT_WIN_REDRAW
    call post_event                 ; preserves all
    pop dx
    pop ax
.no_repaint:

EDIT 3 - add static local after the final `ret` (after line 17867):

.raised: db 0

OPTIONAL CLEANUP (avoids double clear/draw/event, cosmetic only): in mouse_process_drag, replace lines 4344-4375 with nothing (keep just `call win_focus_stub`; the .focus_already_top frame redraw at 4378-4381 must stay since win_focus_stub now skips repaint when already top); in win_destroy's promote path, drop lines 16471 and 16473-16477 (keep the clip save/restore around the focus call or drop it too, since win_focus_stub now guards clip itself).

## [medium/high] Resize-handle hit-test ignores occlusion: click in topmost window's body can start resizing a window underneath
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm 3935-3996 (mouse_hittest_resize scan lacking occlusion check); triggered via 4054-4058 (.try_resize) and 4090-4094 (.start_resize raises+resizes occluded window) | area:window manager / mouse hit-testing
DESC: mouse_hittest_titlebar correctly does a two-step test: first find the topmost window containing the point (full area, 3835-3878), then check whether the point is in THAT window's title bar (3884-3897). mouse_hittest_resize does not: it returns the highest-z window whose bottom-right 10x10 corner contains the point (3941-3989), without checking whether a higher-z window's body covers that point. mouse_drag_update tries titlebar first, and on miss goes to '.try_resize: call mouse_hittest_resize' (4054-4058). So clicking inside the focused window's content where a background window's resize corner happens to lie underneath starts a resize drag of the hidden window (which is then also raised via drag_needs_focus, line 4093). User-visible misbehavior matching 'mouse input issues'.
VERIFIER: Confirmed by reading C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. mouse_hittest_titlebar (3824-3914) is occlusion-correct: it first finds the topmost visible window whose FULL area contains the point (3841-3878), then requires the point to be in THAT window's title bar (3884-3897). mouse_hittest_resize (3924-4008) is not: its single scan (3941-3989) considers only each visible WIN_FLAG_BORDER window's bottom-right 10x10 zone and picks the highest z-order among corner candidates; the actually-topmost window at the point is never consulted unless the point also lies in its own corner zone. mouse_drag_update calls it on titlebar miss (.try_resize, 4054-4058) and on hit unconditionally starts a resize (.start_resize, 4090-4116), setting resize_window/drag_window and drag_needs_focus=1; mouse_process_drag (4314+) then raises the hidden window to z=15, and the resize is tracked (4147-4195) and applied on release (4199-4207). mouse_hittest_resize has exactly one caller (line 4056) and no downstream occlusion/validation exists. Concrete repro: background window A at (100,100) 200x150 (corner zone x in [290,300), y in [240,250)) fully covered there by topmost window B at (50,50) 400x300; clicking (295,245) — visually inside B's body, below B's titlebar, away from B's own corner — misses the titlebar test (correctly), then mouse_hittest_resize returns A, so A is raised and a resize drag of A starts from a click inside B's content. The proposed fix (two-step test like the titlebar path) also preserves legitimate behavior: a genuinely exposed corner of a background window is still hit, because at that point the background window IS the topmost window containing the point. Refined lines: flaw in 3941-3989 (scan) with consequences via 4054-4058 and 4090-4094; severity medium is appropriate.
FIX: Rewrite the body of mouse_hittest_resize (kernel/kernel.asm lines 3935-3999, between the 'mov dx, [mouse_y]' at 3933 and '.rht_done:' at 4001) to use the same two-step structure as mouse_hittest_titlebar: step 1 finds the topmost window whose full area contains the point; step 2 tests only that window's corner. Step 1 already guarantees mouse < right/bottom edges, so step 2 needs only the two lower-bound checks. Registers/stack frame unchanged; shl-by-5 matches existing codebase usage (e.g. line 3886).

    ; Step 1: find topmost visible window whose FULL AREA contains click
    xor si, si                      ; SI = window index
    mov di, window_table
    mov bp, 0xFFFF                  ; BP = best handle (0xFFFF = none)
    mov bl, 0                       ; BL = best z-order so far
.rht_find:
    cmp si, WIN_MAX_COUNT
    jae .rht_found
    cmp byte [di + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .rht_next
    mov ax, [di + WIN_OFF_X]
    cmp cx, ax
    jb .rht_next
    add ax, [di + WIN_OFF_WIDTH]
    cmp cx, ax
    jae .rht_next
    mov ax, [di + WIN_OFF_Y]
    cmp dx, ax
    jb .rht_next
    add ax, [di + WIN_OFF_HEIGHT]
    cmp dx, ax
    jae .rht_next
    mov al, [di + WIN_OFF_ZORDER]
    cmp bp, 0xFFFF
    je .rht_new_best
    cmp al, bl
    jbe .rht_next
.rht_new_best:
    mov bp, si
    mov bl, al
.rht_next:
    add di, WIN_ENTRY_SIZE
    inc si
    jmp .rht_find

.rht_found:
    cmp bp, 0xFFFF
    je .rht_no_hit                  ; Click not inside any window

    ; Step 2: click must be in THAT window's bottom-right 10x10 corner
    mov ax, bp
    shl ax, 5
    add ax, window_table
    mov di, ax
    test byte [di + WIN_OFF_FLAGS], WIN_FLAG_BORDER
    jz .rht_no_hit                  ; Frameless: no resize handle
    mov ax, [di + WIN_OFF_X]
    add ax, [di + WIN_OFF_WIDTH]
    sub ax, 10
    cmp cx, ax                      ; mouse_x >= win_x + win_w - 10?
    jb .rht_no_hit
    mov ax, [di + WIN_OFF_Y]
    add ax, [di + WIN_OFF_HEIGHT]
    sub ax, 10
    cmp dx, ax                      ; mouse_y >= win_y + win_h - 10?
    jb .rht_no_hit
    ; Upper bounds already proven by step 1 (point inside window)
    mov ax, bp
    clc
    jmp .rht_done

.rht_no_hit:
    stc

## [medium/high] Background windows get content cleared + WIN_REDRAW, but z-clipping silently drops their repaint: persistent holes
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm: defect spans 16255-16258 (premature WIN_REDRAW post to z<15 window), 10157-10177 (filter lacks z-order check), 242-251 (z-clip that discards the resulting repaint); fix lands after line 10171. | area:window manager / redraw protocol
DESC: redraw_affected_windows clears the intersection of each overlapped background window's content with the damaged rect and posts EVENT_WIN_REDRAW to it (16205-16258). The event filter delivers that event to the owner regardless of z (10158-10176). But the INT 0x80 dispatcher drops ALL draws from non-topmost windows: 'cmp byte [si + WIN_OFF_ZORDER], 15 / je .zclip_ok / ... clc / jmp int80_return_point' (245-251). So the background app consumes its WIN_REDRAW, repaints, and every pixel is discarded — the cleared rectangle remains a desktop-colored hole in the background window until the user clicks it (the click-focus path re-clears and re-posts, 4346-4375). Window moves/destroys over background windows therefore leave blank patches in them — another concrete source of the reported 'visual anomalies'. The posted event is also pure waste (app does full repaint work that is discarded).
VERIFIER: Confirmed by direct code reading; all three cited mechanisms exist and interact exactly as claimed, with two corrections to the finding's framing.

CONFIRMED MECHANICS (kernel\kernel.asm):
(1) redraw_affected_windows z-loop handles only z=0..14 (16150-16156); for each visible background window intersecting the damage rect it draws the frame via internal win_draw_stub (not z-clipped, kernel call), clears content-rect intersection to color 0 via gfx_clear_area_stub (16210-16252), and posts EVENT_WIN_REDRAW (16256-16258). The comment at 16255 says "so app can redraw when focused" - but nothing defers delivery.
(2) event_get_stub's WIN_REDRAW filter (10157-10177) checks only WIN_STATE_VISIBLE and WIN_OFF_OWNER == current_task; no z-order check. The occluded owner consumes the event on its next event_get poll.
(3) INT 0x80 z-clip (245-251) silently drops ALL translated drawing APIs (0-6, 50-52, 56-62, 65-71, 80, 87, 94, 102 per api_translate_bitmap at 20094-20108) when the draw-context window's ZORDER != 15, returning success (clc / jmp int80_return_point). So the background app's entire repaint paints zero pixels.
(4) No backing store or per-window dirty flag exists anywhere (window entry uses offsets 0-24 of 32). The cleared patch therefore persists until the window is next promoted to z=15: click-focus (mouse_process_drag 4332-4376: full content clear + fresh WIN_REDRAW) or destroy-promote (win_destroy_stub 16457-16477: fresh WIN_REDRAW). win_focus_stub itself (17784) posts nothing.

NET EFFECT (real, user-visible): moving/resizing/destroying a window that overlapped a background window leaves a blank (color-0) patch in the background window's content until the user clicks it, and the background app performs a full repaint whose output is entirely discarded (wasted CPU + one wasted slot in the 32-entry shared event ring, which is head-blocking for cross-task events).

TWO CORRECTIONS TO THE FINDING:
(a) The blank patch itself is partially inherent to the architecture: with no backing store the kernel cannot restore occluded window content, so blank-until-refocus cannot be fully eliminated by any small fix. The code-level defects that ARE fixable: the premature event consumption and the wasted repaint.
(b) The finding's alternative fix ("skip the content clear for z<15 windows; their old pixels were already overdrawn") is WRONG: the damaged rect is where the vacating window USED to be, so the pixels there belong to the moved/destroyed window, not the background window. Skipping the clear would leave the other window's stale pixels inside the background window's content - worse garbage than blank. The clear must stay.

CORRECT MINIMAL FIX: in event_get_stub's WIN_REDRAW filter, DISCARD (not leave - leaving would head-block the shared ring queue for all tasks, since .no_event does not advance head) events targeting a window whose ZORDER != 15. The z-clip guarantees such a repaint is a no-op, and both promotion paths (click-focus 4371-4375, destroy-promote 16473-16477) post a fresh WIN_REDRAW at promotion time, so nothing is lost. SI already points at the window entry at that point and .evt_discard_pop performs the correct stack unwinding. This is strictly better than current behavior in every traced case (occluded poll: saves a full wasted repaint; unpolled-then-focused: identical, event passes filter once z=15; double-post after click: identical). Severity medium is fair: visible artifact source + measurable wasted work, but not a crash/corruption.
FIX: In kernel\kernel.asm, event_get_stub WIN_REDRAW filter: after the visibility check at line 10170-10171, add a z-order check that discards redraw events for occluded windows (8086-safe, registers already set up):

    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .evt_discard_pop            ; Window freed/destroyed: discard stale event
+   ; Occluded window: z-clip (int80 dispatcher) would drop every draw of the
+   ; repaint anyway; focus/promote paths post a fresh WIN_REDRAW at z=15.
+   ; Discard (not leave) so the shared ring never head-blocks on it.
+   cmp byte [si + WIN_OFF_ZORDER], 15
+   jne .evt_discard_pop
    mov al, [si + WIN_OFF_OWNER]
    cmp al, [current_task]

Optionally (pure waste removal, safe because click-focus 4371-4375 and destroy-promote 16473-16477 re-post on promotion): delete the post in the redraw_affected_windows z-loop, lines 16255-16258 ("mov al, EVENT_WIN_REDRAW / mov dx, bp / call post_event"), since that loop only ever processes z<15 windows whose redraws are guaranteed to be discarded. Do NOT remove the content clear at 16210-16252 - the damaged rect holds the vacating window's pixels and must be blanked.

## [medium/medium] post_event is not interrupt-safe: syscall-context posts race with IRQ1/IRQ12 posts, losing events
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:10092-10106 (unguarded critical section in post_event); racing producers: IRQ posts at 624, 1142, 1274 vs IF=1 kernel posts at 4375, 4680, 5043, 16258, 16354, 16477, 19324 (IF=1 due to sti at line 151) | area:event queue / concurrency
DESC: post_event reads event_queue_tail, writes the 3-byte event, then increments and stores tail (10092-10106) with no CLI guard. The INT 0x80 dispatcher executes 'sti' on entry (line 151), so kernel-side posts (EVENT_WIN_REDRAW from redraw_affected_windows 16256-16258, win_destroy 16474-16477, win_resize 19318-19324) can be interrupted between the tail read and the tail store by the keyboard or mouse IRQ posting its own event into the SAME slot — one of the two events is overwritten or its tail advance is lost. Result: sporadically missing key/mouse/redraw events, matching the reported intermittent input problems. Also note the queue-full check drops the newest event silently (10104-10105) with a usable capacity of 31.
VERIFIER: CONFIRMED by direct code reading of C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. post_event (10083-10112) performs an unguarded read-modify-write of event_queue_tail: read tail (10092), store 3-byte event (10098-10099), store tail+1 (10106). There is no pushf/cli/popf in the function and none at any kernel-context call site (verified by grepping every cli/sti/pushf in the file). The INT 0x80 dispatcher executes sti at line 151, so all syscall-reachable posts run with IF=1: mouse_process_drag at 4375 and 4680 (reached from event_get_stub line 10126, API 9), menu_close at 5043 (API 88), redraw_affected_windows at 16258 and 16354 (reached from win_move_stub), win_destroy_stub promote path at 16477 (API 21), and win_resize_stub at 19324 (API 78). Meanwhile IRQ1 (int_09_handler, posts EVENT_KEY_PRESS at 624) and IRQ12 (int_74_handler, posts EVENT_MOUSE at 1142; BIOS callback at 1274) call the same post_event. The IRQ handlers never sti, so IRQ-vs-IRQ posts cannot race each other; the race is exactly IF=1 kernel posts vs IRQ posts, as claimed. Failure trace: kernel post reads tail=T; IRQ12 preempts before 10106, posts EVENT_MOUSE at slot T, advances tail to T+1; kernel resumes with stale BX=T, overwrites slot T and stores tail=T+1 -> one of the two events is silently lost. Additionally (stronger than the original finding): if the interrupt lands between the type store (10098) and data store (10099), the result is a hybrid event - EVENT_MOUSE type with a window handle as its data word, which the consumer decodes as button state -> phantom mouse clicks. The race window is widest during window drag/resize, when the kernel is posting WIN_REDRAW while the moving mouse generates 100-300 IRQ12/s, matching the reported intermittent input symptoms. No other mechanism prevents this: the cli/sti guards at 4452-4457 and 4517-4520 protect only drag-state variable reads (and prove IRQ12 is live in these paths); the consumer (event_get_stub) is safe as-is since it only does a single-store head advance and never touches the tail slot. The queue-full observation (10104-10105, usable capacity 31 of 32, newest event dropped) is accurate but is standard ring-buffer behavior, minor. The suggested pushf/cli/popf fix is correct and necessary (popf rather than sti, because post_event is also called from IF=0 IRQ context and must not re-enable interrupts there); it matches the existing codebase pattern in mouse_cursor_hide/show (3736-3814). All instructions involved are 8086-safe.
FIX: In post_event (kernel/kernel.asm line 10083), insert pushf+cli after the register saves and popf at .done before the register restores:

post_event:
    push bx
    push si
    push ds
    pushf                           ; Save caller's IF state (IRQ callers have IF=0)
    cli                             ; Atomic tail read-modify-write vs IRQ1/IRQ12

    mov bx, 0x1000
    mov ds, bx
    ; ... existing body lines 10091-10106 unchanged ...

.done:
    popf                            ; Restore IF (NOT sti - must stay 0 for IRQ-context callers)
    pop ds
    pop si
    pop bx
    ret

Concretely: insert "pushf" and "cli" between line 10086 ("push ds") and line 10088 ("mov bx, 0x1000"), and insert "popf" immediately after the ".done:" label at line 10108, before "pop ds". All instructions are 8086-safe; pattern identical to mouse_cursor_hide/show at 3736-3814.

## [medium/high] Single global event queue causes head-of-line blocking across tasks
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm 10128-10201 (no-advance bail-outs at 10151-10152 and 10177; head-advance only at 10181-10183/10198-10200; silent drop on full queue at 10102-10106; single shared queue state at 20215-20217) | area:event queue / scheduling
DESC: event_get_stub leaves events 'in queue for correct task' without advancing head: KEY_PRESS for a non-current focused task → 'jmp .no_event' (10152), WIN_REDRAW for another task's window → 'jmp .no_event ; Wrong task's window - leave in queue' (10177). Because head never advances past a foreign event, every other task is starved of ALL its queued events until the destined task polls. If the focused app stops polling (busy loop, long file I/O, or hung), keyboard/mouse/redraw delivery freezes system-wide and the 32-slot queue fills, silently dropping all new input (10104-10105). This matches 'keyboard and mouse input issues' when 'too many apps running'.
VERIFIER: CONFIRMED with refined impact analysis. The code matches the finding exactly. In kernel/kernel.asm, event_get_stub (10117-10201) reads only the event at event_queue_head (10129-10140) and advances head only in .evt_consume (10181-10183) or .evt_discard (10198-10200, for invalid handle / destroyed window). Two paths bail to .no_event WITHOUT advancing head: (1) KEY_PRESS when a different task has focus — pop ax / jmp .no_event at 10151-10152; (2) WIN_REDRAW whose window owner != current_task — jmp .no_event at 10177. Since head is the only read position, one event destined for task X at the head blocks ALL queued events (including EVENT_MOUSE, which any task may consume per 10159-10160's fall-through) for every other task until X polls. post_event (10083-10112) silently drops new events when the 32-slot (31 usable) ring is full (10102-10106), and it is called from IRQ context (INT 9 keyboard ~line 1142/1274, mouse paths 4375/4680/5043, window expose 16258/16354/16477/19324), so input arriving during a blocked period is lost once 31 events accumulate. Nothing else drains the queue: clear_kbd_buffer (700-712) just loops on event_get_stub and stops at the first foreign event.

IMPACT CORRECTIONS to the original finding: (a) Scheduling is purely cooperative (app_yield_stub, 14718-14860; "Cooperative Multitasking v3.14.0"). If the focused app busy-loops, hangs, or sits in long synchronous file I/O, NO other task runs at all — the system-wide freeze in that scenario is the scheduler's doing, not the queue's. The queue only adds dropped-input symptoms there. (b) The genuinely queue-caused failure is cross-task: a LIVE task that yields but polls events rarely/never. Concrete trace: background task B's window is uncovered → WIN_REDRAW(B) posted (e.g., mouse_process_drag:4375); it reaches head; well-behaved focused task A polls → owner check at 10172-10177 → .no_event with head frozen → A's own KEY_PRESS/mouse events queued behind it are undeliverable; keyboard appears dead system-wide until B polls. Focus changes do NOT unblock this case (the WIN_REDRAW filter checks WIN_OFF_OWNER, not focused_task); only B polling or B's window being destroyed (.evt_discard via the WIN_STATE_VISIBLE check at 10170-10171) clears it. (c) Worst case the finding missed: the modal file dialogs (.fdlg_loop 5183-5197, .sdlg_loop 5868-5881) "intentionally block all other tasks" and poll event_get_stub in a hlt loop without yielding. If a foreign WIN_REDRAW is at head when a modal opens, its owner can never run to consume it → keyboard (ESC/Enter/typing) in the modal is permanently dead; only direct-polled mouse handling (.fdlg_check_mouse 5223+) still works. (d) Existing partial mitigations: stale WIN_REDRAW for destroyed/invisible windows is discarded (10161-10171), so exited apps don't wedge the queue; KEY_PRESS blocking heals on focus change because the filter compares live focused_task (10146-10150), not a stamped owner; window drag/focus-on-click keeps working because mouse_process_drag (10126) runs outside the queue. Severity: medium is fair — confirmed mechanism, real user-visible input freezes/drops with multiple background apps, but not the "hung focused app freezes everything" story as originally framed.
FIX: Minimal fix (no per-task queues): make event_get_stub scan past foreign events with a cursor, consuming mid-queue events via EVENT_NONE tombstones; head advances only over consumed/tombstoned slots. 8086/186-safe (matches existing codebase use of shl reg,imm). post_event is untouched; IRQ-posted events only append at tail, and the scanner writes only single bytes within [head,tail) plus the aligned head word, so no cli needed. Replace lines 10128-10201 with:

.evt_scan_start:
    mov bx, [event_queue_head]      ; BX = scan cursor (starts at head)
.evt_check_next:
    cmp bx, [event_queue_tail]
    je .no_event

    ; Calculate cursor position (events are 3 bytes each)
    mov si, bx
    add si, bx                      ; SI = cursor * 2
    add si, bx                      ; SI = cursor * 3

    mov al, [event_queue + si]      ; type
    mov dx, [event_queue + si + 1]  ; data (word)

    cmp al, EVENT_NONE              ; Tombstone (already consumed mid-queue)?
    je .evt_tombstone

    ; Filter keyboard events: only deliver to focused task
    cmp al, EVENT_KEY_PRESS
    jne .evt_not_key
    push ax
    mov al, [focused_task]
    cmp al, 0xFF                    ; No window focused? Deliver to current task
    je .evt_focus_ok
    cmp al, [current_task]
    je .evt_focus_ok
    pop ax
    jmp .evt_skip                   ; Not focused - skip past, leave for correct task
.evt_focus_ok:
    pop ax
    jmp .evt_consume

.evt_not_key:
    cmp al, EVENT_WIN_REDRAW
    jne .evt_consume                ; Other event types: consume and pass through
    cmp dl, WIN_MAX_COUNT
    jae .evt_discard                ; Invalid window handle: drop and retry
    push si
    push ax
    xor ah, ah
    mov al, dl
    mov si, ax
    shl si, 5
    add si, window_table
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .evt_discard_pop            ; Window freed/destroyed: drop stale event
    mov al, [si + WIN_OFF_OWNER]
    cmp al, [current_task]
    pop ax
    pop si
    je .evt_consume
    jmp .evt_skip                   ; Wrong task's window - skip past, leave in queue

.evt_consume:
    cmp bx, [event_queue_head]
    jne .evt_consume_mid
    inc bx                          ; At head: advance head normally
    and bx, 0x1F
    mov [event_queue_head], bx
    jmp .evt_return
.evt_consume_mid:
    mov byte [event_queue + si], EVENT_NONE  ; Mid-queue: tombstone, head untouched
    ; fall through

.evt_return:
    clc                             ; CF=0 = event available
    pop ds
    pop si
    pop bx
    ret

.evt_tombstone:
    cmp bx, [event_queue_head]      ; Tombstone at head? Collapse it
    jne .evt_skip
    inc bx
    and bx, 0x1F
    mov [event_queue_head], bx
    jmp .evt_check_next

.evt_skip:
    inc bx                          ; Step cursor past foreign event; head unchanged
    and bx, 0x1F
    jmp .evt_check_next

.evt_discard_pop:
    pop ax
    pop si
.evt_discard:
    cmp bx, [event_queue_head]      ; Invalid/stale event: consume at cursor, retry
    jne .evt_discard_mid
    inc bx
    and bx, 0x1F
    mov [event_queue_head], bx
    jmp .evt_check_next
.evt_discard_mid:
    mov byte [event_queue + si], EVENT_NONE
    inc bx
    and bx, 0x1F
    jmp .evt_check_next

Notes: (1) Tombstones behind an unconsumed foreign event hold their slots until that event is consumed and head sweeps past, so a never-polling task can still pin at most its own events + tombstones (bounded; queue full still drops at post_event 10104-10105) — per-task queues remain the complete fix, but this removes all cross-task head-of-line blocking including the modal-dialog keyboard deadlock. (2) EVENT_NONE equ 0 already exists (line 10068), and post_event never posts type 0, so the tombstone value is safe. (3) Register contract preserved (BX/SI pushed; CX only written on the .no_event path, as before).

## [medium/high] destroy_task_windows triggers a full promote/focus/redraw cycle per window, repainting windows about to be destroyed
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: destroy_task_windows 15059-15098; win_destroy_stub 16387 (clear+redraw ~16426-16444, promote block ~16446-16487); redraw_affected_windows 16136; stale-event discard ~10179-10180 (line numbers volatile - file under concurrent edit) | area:window manager / app-exit performance
DESC: destroy_task_windows loops over all slots calling win_destroy_stub per owned window (15060-15081). Each win_destroy_stub does a full gfx_clear_area + redraw_affected_windows pass (desktop fill + icon redraw + frame redraws, 16417-16435) AND promotes/focuses/redraws the next-topmost window with a posted WIN_REDRAW (16437-16477) — even when that 'next-topmost' is a sibling window of the same dying task that the very next loop iteration will destroy. An app exiting with 3 windows performs 3 desktop repaints, 2 pointless focus promotions, frame redraws and WIN_REDRAW posts for windows that are dead microseconds later (the stale events are then discarded at 10170-10171). Iteration itself is safe (slot indices are stable; spec item 4 checked), but exit of multi-window apps causes visible flicker and wasted full-screen repaints on CGA hardware.
VERIFIER: CONFIRMED in mechanism, with two factual corrections that lower severity from medium to low-medium.

What I verified in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm (note: the file is being concurrently edited this session; line numbers below are from my final read and may drift):

1. destroy_task_windows (15059-15098) loops all 16 slots and calls win_destroy_stub once per window owned by the dying task. Iteration is indeed safe (SI/CX advance over fixed 32-byte slots; win_destroy_stub only flips state in place and preserves the registers destroy_task_windows re-pushes).

2. win_destroy_stub (16387+) per call: mouse_cursor_hide + cursor lock, gfx_clear_area_stub over the window rect, marks slot FREE, invalidates topmost_handle, calls redraw_affected_windows, then runs an inline promote pass (.promote_scan ~16453, .promote_focus ~16473) that calls win_focus_stub (which runs a z-demote loop over all windows and redraws the OLD topmost titlebar as inactive), win_draw_stub (full frame redraw), and posts EVENT_WIN_REDRAW.

3. During the loop, sibling windows of the dying task are still WIN_STATE_VISIBLE, so (a) redraw_affected_windows' z-loop redraws their frames and posts WIN_REDRAW for them if they overlap the destroyed rect, and (b) the promote pass picks the highest-z visible window — almost certainly a sibling of the dying task since its windows were focused last — promotes it to z=15, redraws its frame, and posts WIN_REDRAW... and the very next iteration destroys it. Pure waste, exactly as claimed.

4. The stale WIN_REDRAW events are indeed discarded later at the event-delivery filter (cmp byte [si+WIN_OFF_STATE], WIN_STATE_VISIBLE / jne .evt_discard_pop, now ~10179-10180). They also waste slots in the 32-entry event queue in the interim.

5. No existing mechanism prevents this: there is no batch/no-redraw flag anywhere in win_destroy_stub or destroy_task_windows.

CORRECTIONS to the finding:
- "full-screen repaints" / "3 desktop repaints" is overstated. redraw_affected_windows and draw_desktop_region (15768+) are strictly region-limited to redraw_old_x/y/w/h = the destroyed window's rect, with rect-intersection tests on every window and every desktop icon. The desktop fill per destroyed window covers only that window's area — work that must happen once per region anyway. The genuinely wasted work is: the sibling frame redraws, the pointless win_focus_stub z-shuffle + old-topmost titlebar repaint, the win_draw_stub full frame paint of a window dead one iteration later, the discarded WIN_REDRAW posts, and per-window cursor hide/show. Visible as flicker, but it is not N full-screen repaints.
- The "app exiting with 3 windows" scenario is hypothetical today: all 12 in-tree apps (apps/*.asm) create exactly one window. The only realistic multi-window owner is an app with a kernel-created file dialog open (win_create_stub at 5162/5847 sets WIN_OFF_OWNER = current_task, 15235), giving 2 windows on the force-kill paths (close-button kill ~4409-4431; segment-reuse kill ~14466-14473). So the waste fires at most once per kill in practice, though the API permits worse for future apps.

Callers' contract to preserve when fixing: app-exit at ~14995-15000 relies on ZF from destroy_task_windows (test bx,bx executed before the pops, which don't alter flags) to decide whether to do a fullscreen repaint for windowless apps.
FIX: Minimal 8086-safe batch fix (three edits):

1. Add one byte of kernel state next to redraw_old_*:
   dtw_batch: db 0   ; 1 = destroy_task_windows batch in progress

2. In win_destroy_stub, extract the existing promote block (from "mov si, window_table / mov cx, WIN_MAX_COUNT / mov byte [.best_z],0 ..." through ".promote_done", including the clip_enabled save/restore and .best_z/.best_handle locals) verbatim into a standalone routine, and gate the per-window repaint on the batch flag:

   ; after: mov byte [bx + WIN_OFF_STATE], WIN_STATE_FREE
   ;        mov byte [topmost_handle], 0xFF
       cmp byte [dtw_batch], 0
       jne .batch_skip              ; batch: caller does one repaint+promote
       call redraw_affected_windows
       call win_promote_next
   .batch_skip:
       clc
       jmp .done

   win_promote_next:                ; promote highest-z visible win, focus+draw+post
       push ax
       push bx
       push cx
       push dx
       push si
       push ds
       mov bx, 0x1000
       mov ds, bx
       ; <existing promote code moved here unchanged:
       ;  scan for best z -> .best_handle; if none: focused_task=0xFF,
       ;  topmost_handle=0xFF; else clip-save, win_focus_stub, win_draw_stub,
       ;  clip-restore, post EVENT_WIN_REDRAW with DL=.best_handle>
       pop ds
       pop si
       pop dx
       pop cx
       pop bx
       pop ax
       ret

3. In destroy_task_windows: set the flag, accumulate the union rect while destroying, then do exactly one repaint + one promote. ZF contract for the app-exit caller is preserved by re-executing test bx,bx after the calls:

   destroy_task_windows:
       push ax
       push bx
       push cx
       push si
       mov byte [dtw_batch], 1
       mov word [.un_x1], 0x7FFF
       mov word [.un_y1], 0x7FFF
       mov word [.un_x2], 0
       mov word [.un_y2], 0
       xor bx, bx
       mov si, window_table
       xor cx, cx
   .dtw_loop:
       cmp cx, WIN_MAX_COUNT
       jae .dtw_done
       cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
       jne .dtw_next
       cmp [si + WIN_OFF_OWNER], al
       jne .dtw_next
       push ax                      ; merge window rect into union
       mov ax, [si + WIN_OFF_X]
       cmp ax, [.un_x1]
       jge .ux1ok
       mov [.un_x1], ax
   .ux1ok:
       add ax, [si + WIN_OFF_WIDTH]
       cmp ax, [.un_x2]
       jle .ux2ok
       mov [.un_x2], ax
   .ux2ok:
       mov ax, [si + WIN_OFF_Y]
       cmp ax, [.un_y1]
       jge .uy1ok
       mov [.un_y1], ax
   .uy1ok:
       add ax, [si + WIN_OFF_HEIGHT]
       cmp ax, [.un_y2]
       jle .uy2ok
       mov [.un_y2], ax
   .uy2ok:
       pop ax
       push ax
       push cx
       mov al, cl
       call win_destroy_stub        ; batch mode: clears area only, no repaint
       pop cx
       pop ax
       inc bx
   .dtw_next:
       add si, WIN_ENTRY_SIZE
       inc cx
       jmp .dtw_loop
   .dtw_done:
       mov byte [dtw_batch], 0
       test bx, bx
       jz .dtw_ret                  ; nothing destroyed: no repaint, ZF=1
       push ax
       mov ax, [.un_x1]
       mov [redraw_old_x], ax
       mov ax, [.un_y1]
       mov [redraw_old_y], ax
       mov ax, [.un_x2]
       sub ax, [.un_x1]
       mov [redraw_old_w], ax
       mov ax, [.un_y2]
       sub ax, [.un_y1]
       mov [redraw_old_h], ax
       call redraw_affected_windows ; one region repaint over the union
       call win_promote_next        ; one focus reassignment
       pop ax
       test bx, bx                  ; re-establish ZF for caller (POPs keep flags)
   .dtw_ret:
       pop si
       pop cx
       pop bx
       pop ax
       ret
   .un_x1: dw 0
   .un_y1: dw 0
   .un_x2: dw 0
   .un_y2: dw 0

Notes: per-window gfx_clear_area_stub still runs inside win_destroy_stub (covers the desktop_icon_count==0 case where draw_desktop_region is skipped, and avoids over-filling when windows are disjoint). Union rect can exceed the sum of two disjoint window rects, but draw_desktop_region/redraw_affected_windows clip everything by intersection so it stays correct; for the realistic case (cascaded dialog over its parent) it is strictly less work. All instructions are 8086-safe.

## [medium/high] event_get_stub no-event path clobbers CX and DX with debug data despite documented 'Preserves: BX, CX, SI, DI'
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm lines 10288-10291: .no_event_return clobbers CX (and writes diagnostic data into output register DX) on every empty event poll, violating the documented "Preserves: BX, CX, SI, DI" contract of API 9/10; leftover Build-341 diagnostic with no remaining consumer. | area:event queue / syscall ABI
DESC: The .no_event_return path executes 'mov dh, [focused_task] / mov dl, [current_task] / mov ch, byte [event_queue_head] / mov cl, byte [event_queue_tail]' - leftover debug instrumentation. The function header (line 10116) promises 'Preserves: BX, CX, SI, DI', and the INT 0x80 dispatcher does not restore CX for non-drawing APIs. Every app polling API 9 in a loop gets CX trashed on every empty poll. Any app keeping a loop counter, coordinate, or state in CX across the poll misbehaves - a plausible contributor to erratic app behavior and input-loop bugs.
VERIFIER: CONFIRMED, with two refinements. The four mov instructions exist at kernel/kernel.asm lines 10288-10291 (finding cited 10286-10292; the label and xor/stc bracket them). They were introduced by commit 2d60cd8 "Event queue state diagnostic (Build 341)" so sysinfo.asm could display focus/queue state; sysinfo.asm has since been rewritten and no longer reads them, so they are dead diagnostic code with zero consumers (verified: all kernel-internal callers at lines 708, 5189, 5873, 10302 only test AL/CF/DL, and no app reads CX after API 9). The CX clobber is a real ABI violation: the header (line 10116) promises "Preserves: BX, CX, SI, DI" but only BX/SI are pushed (lines 10118-10119). The INT 0x80 dispatcher confirms no rescue: CX is restored at int80_return_point only when _did_translate=1 (lines 396-399), which requires the API to be set in api_drawing_bitmap (bt at line 173); API 9 falls in bitmap byte 1 = 0x00 (line 20081), so jnc .no_translate is taken and the clobbered CX reaches the app via IRET. The clobber fires on every empty poll (queue empty + use_bios_keyboard=0, the default since install_keyboard line 454 always installs the custom INT 9 handler) and also on the leave-event-in-queue paths (lines 10152, 10177). event_wait_stub (API 10, line 10300) makes the same broken promise since it loops through this path. REFINEMENTS: (1) the DX clobber is NOT a contract violation - DX is a documented output register ("Output: AL = event type, DX = event data"), so only CX matters; (2) impact is overstated: I sampled in-tree app event loops (tetris, pacman, notepad, sysinfo, settings) and all keep loop state in memory ([cs:vars]), none holds live state in CX across the poll, so this is a latent ABI trap for future/third-party apps rather than a demonstrated cause of current erratic behavior. Severity: medium as a contract bug, low as an observed defect.
FIX: In kernel/kernel.asm, delete the four diagnostic movs (lines 10288-10291), leaving:

.no_event_return:
    xor al, al                      ; AL = 0 (no event, keeps event_wait working)
    stc                             ; CF=1 = no event available
    pop ds
    pop si
    pop bx
    ret

This is 8086-safe (removes instructions only). No caller depends on the diagnostic values: sysinfo.asm (the original consumer, added in commit 2d60cd8 / Build 341) was rewritten and no longer reads CX/DX on the no-event path, and kernel-internal callers (lines 708, 5189, 5873, 10302) only test AL/CF. Minimal alternative if the DH/DL task IDs are deemed intentional (DX is a documented output register): delete only the two CX lines (10290-10291), since only CX violates the documented preserve list.

## [medium/high] INT 09h scan-code translation reads out of bounds for scancodes 0x60-0x7F
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:501-517 (insert fix after line 502; OOB loads at lines 513 and 517; 96-byte tables at lines 2153-2167) | area:keyboard
DESC: After 'test al, 0x80 / jnz .done', AL can be any make code 0x00-0x7F, and the handler indexes 'mov al, [scancode_shifted + bx]' / 'mov al, [scancode_normal + bx]' unchecked. Both tables (lines 2153-2167) are only 96 bytes (0x00-0x5F). Scancodes 0x60-0x7F (international keys, F13+, some laptop Fn emissions) read past the table: scancode_normal+0x60 lands inside scancode_shifted (wrong chars posted as KEY_PRESS), scancode_shifted+0x60 lands in code bytes of setup_graphics (line 2174) producing garbage key events.
VERIFIER: Confirmed by direct code trace. In int_09_handler (kernel/kernel.asm line 461), after the release filter 'test al, 0x80 / jnz .done' (lines 501-502), AL can be any make code 0x00-0x7F. Lines 505-506 set BX = zero-extended AL, and lines 513/517 index 'scancode_shifted + bx' / 'scancode_normal + bx' with no bounds check. Both tables (lines 2153-2159 and 2161-2167) are exactly 96 bytes (6 x 16 db rows, indices 0x00-0x5F), and grep confirms they are defined once and referenced only at those two sites. For scancodes 0x60-0x7F: scancode_normal+0x60..0x7F reads into scancode_shifted (posts wrong shifted chars like ESC/!/@/Q/W), and scancode_shifted+0x60..0x7F reads machine-code bytes of setup_graphics immediately following the table (line 2174: 31 C0 B0 04 CD 10 80 3E ...). The .store_key path (lines 587-590) filters only AL==0, so nonzero garbage is queued into kbd_buffer and posted as EVENT_KEY_PRESS; opcode bytes >= 0x80 collide with the OS's special key codes 128-136 (arrows/Home/End/etc., lines 557-585), so fake navigation events can be injected. Triggerable on real hardware: set-1 make codes 0x60-0x7F are emitted by Japanese keyboards (0x70 kana, 0x73 Ro, 0x79 henkan, 0x7B muhenkan, 0x7D yen), Brazilian ABNT2 (0x73, 0x7E), and F13+ on some keyboards; they simply never occur with a US layout under emulation, which is why testing missed it. No other mechanism prevents it: this is the raw INT 09h hardware vector (installed at line 452), the extended-key path (.handle_extended) never indexes the tables, and there is no bounds check anywhere on the path. Read-only overrun within the kernel segment, so no corruption/crash - wrong input events only; medium severity is appropriate. The suggested fix (cmp al, 0x60 / jae .done after the release filter) is correct, minimal, and 8086-safe.
FIX: In kernel/kernel.asm, immediately after the release filter at lines 501-502 (before 'mov bx, ax' at line 505), insert:

    ; Translation tables only cover scancodes 0x00-0x5F; ignore the rest
    cmp al, 0x60
    jae .done

Resulting code:

    ; Check if it's a key release (bit 7 set)
    test al, 0x80
    jnz .done
    cmp al, 0x60                    ; Bounds check: tables are 96 bytes
    jae .done

    ; Translate scan code to ASCII
    mov bx, ax
    xor bh, bh

Both 'cmp al, imm8' and 'jae rel8' are 8086 instructions. The extended-key path (.handle_extended) compares against constants and never indexes the tables, so it needs no change.

## [medium/medium] IRQ12 handler swallows keyboard bytes when AUX bit is clear
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:.not_mouse:                         ; line 1154 'in al, KBC_DATA' removed — fall through to .send_eoi, leaving any keyboard byte in the 8042 output buffer for IRQ1/int_09_handler | area:mouse / keyboard controller
DESC: '.not_mouse: ; Not mouse data - chain to keyboard or ignore / in al, KBC_DATA ; Clear the byte'. If IRQ12 fires (or is serviced) while a KEYBOARD byte sits in the 8042 output buffer (AUXB clear), the handler reads and discards it without chaining to INT 09h, so the scancode is lost. A lost key-release byte (e.g. 0xAA Shift-up) leaves kbd_shift_state stuck at 1 - all subsequent typing comes out shifted until Shift is pressed again; a lost make code is a dropped keystroke. This matches 'keyboard input issues', especially under simultaneous typing + mouse movement when IRQ1/IRQ12 service order gets skewed.
VERIFIER: Verified by reading kernel\kernel.asm. int_74_handler (line 1019, installed at IVT 0x74 on the KBC fallback path, lines 868-874) reads KBC_STATUS once (1034) and on AUXB clear jumps to .not_mouse (1152), which executes 'in al, KBC_DATA' (1154) with no OBF check and no chain to the saved INT 09 vector (old_int9_offset is saved/restored only, never invoked) before EOI. If a keyboard byte is in the 8042 output buffer, this read consumes and discards it. There is no recovery mechanism: int_09_handler (461) is the sole keyboard consumer (use_bios_keyboard=0) and itself reads port 0x60 blindly at line 472. After the discard the 8042 deasserts IRQ1; on a real edge-triggered 8259 an unacknowledged request whose line drops is lost (or becomes spurious IRQ7), so the scancode is gone; on QEMU's PIC (IRR stays latched on edge) int_09 still runs but reads an empty/stale buffer or, on pre-7.0 QEMU's kbd-priority i8042 model, the next MOUSE byte misinterpreted as a scancode. The trigger window is real: e.g. on QEMU <7.0, a keyboard byte arriving between the IRQ12 INTA and the handler's status read (IF=0 prologue, ~10 instructions) flips AUXB to 0 with the kbd byte readable at 0x60 — exactly simultaneous typing + mouse movement; on real hardware SMM/USB-legacy and chipset quirks produce the same AUXB=0/OBF=1 state (Linux i8042 routes by AUXDATA for this reason, never blind-discarding). Consequence chain confirmed: int_09 clears kbd_shift_state only on 0xAA/0xB6 (lines 487-490) and selects the shifted table whenever nonzero (509-513); typematic repeat never resends break codes, so a swallowed Shift-up leaves typing stuck shifted. Caveats refining severity: the path is dead when BIOS INT 15h/C2 mouse services succeed (mouse_diag='B', line 776 — likely the case under SeaBIOS/QEMU), and per-event the race window is narrow, so 'medium' is the ceiling; but on the KBC path the bug is real and the suggested fix is sound. EOI-without-read is safe: the unread keyboard byte keeps IRQ1 asserted/latched so int_09 consumes it normally, and no IRQ12 storm is possible since IRQ12 is edge-latched with its line already low in this branch.
FIX: In kernel\kernel.asm, delete line 1154 ('in al, KBC_DATA') so .not_mouse falls through to .send_eoi without touching port 0x60:

.not_mouse:
    ; Not mouse data - leave byte for IRQ1/int_09_handler; just EOI
.send_eoi:
    mov al, 0x20
    out 0xA0, al                    ; EOI to slave PIC
    out 0x20, al                    ; EOI to master PIC

(8086-safe; the pending keyboard byte keeps IRQ1 asserted and int_09_handler reads it via its existing 'in al, 0x60'. If the IRQ12 was spurious with OBF=0, nothing is read and nothing is lost.)

## [medium/medium] Mouse packet stream has weak desync recovery - one lost byte can mis-frame packets for a long stretch
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm:1041-1053 (sync check, as cited); also relevant: 1144-1150 (.reset_packet/.resync), 1067-1074 (buttons stored before overflow discard), 924 and 20129 (init/storage of mouse_packet_idx). No idle re-arm exists anywhere (only writes: 924, 1047, 1145, 1149). | area:mouse
DESC: Sync is only validated on byte 0 ('cmp bl, 0 / jne .check_complete / test al, 0x08 / jz .resync'). If a byte is lost mid-packet (SMI, KBC flush, overrun), the stream shifts by one: a delta byte then lands at index 0, and since movement bytes very often have bit 3 set (any delta with magnitude bit 3, i.e. half of all values), the bad framing passes the check and parses garbage - cursor jumps wildly and buttons read from a delta byte cause phantom clicks/drags. Recovery only happens when a misaligned index-0 byte happens to have bit 3 clear. There is also no idle-gap timeout to re-arm mouse_packet_idx to 0. Classic 'erratic cursor' bug.
VERIFIER: Confirmed by direct code read of C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. In int_74_handler (line 1019, the direct-KBC fallback path installed at line 872 when BIOS INT 15h/C2 services are unavailable), packet framing is exactly as the finding describes: sync is validated only when mouse_packet_idx==0, via 'test al, 0x08 / jz .resync' (lines 1050-1053). mouse_packet_idx is written in only four places (verified by grep): init (924), increment (1047), .reset_packet (1145), .resync (1149) — there is no idle-gap/timer re-arm anywhere. Concrete failure trace: if one byte (e.g. B1 of packet N) is lost, the next two bytes complete a garbage packet [B0(N), B2(N), B0(N+1)]; idx resets and B1(N+1) — an X-delta byte — becomes the idx-0 candidate. Bit 3 of a delta byte is set for half of all values, and crucially for ALL slow negative deltas (-1..-8 = 0xF8..0xFF bit 3 = 1), so a slow leftward/upward drag keeps the stream misframed for the whole gesture: buttons are taken from a delta byte (and al,0x07 at 1067-1069 — stored BEFORE the 0xC0 overflow discard at 1072-1074, so even 'discarded' garbage packets update mouse_buttons → phantom clicks), and deltas are taken from status/Y bytes → wild cursor jumps. The missing idle re-arm is independently sufficient: if idx is 1 or 2 when reporting pauses, the first packet after resume is misparsed even with no further loss. Byte loss is plausible here: besides SMI/KBC overrun, the kernel's own int_09_handler reads port 0x60 unconditionally (line 472) without checking the AUXB status bit, so an IRQ1 arriving while an aux byte is staged can eat a mouse byte. Nothing else mitigates: 'cmp bl,3/jae' (1044-1045) is only buffer-bounds protection, the 0xC0 check does not resync, and the immune mouse_bios_callback path (1184) is a separate code path that does not cover the KBC fallback. Severity medium is fair (fallback path only). One correction to the auditor's secondary suggestion: a byte-0 candidate with bit 3 CLEAR is already dropped by .resync; the only meaningful extra heuristic would be dropping idx-0 candidates with both overflow bits (0xC0) set even when bit 3 is set. The primary tick-gap fix is correct and safe: the 0040:006C word read is atomic inside this handler (IF=0 throughout, no sti), and if the BIOS timer is not ticking (USB-boot caveat noted at line 966) elapsed stays 0 and the guard is a no-op — no regression.
FIX: In int_74_handler, insert an idle-gap re-arm between 'in al, KBC_DATA' (line 1039) and the 'Store byte in packet buffer' block (line 1041). BX/CX/ES are already saved by the handler prologue; AL (the mouse byte) is preserved. 8086-safe:

    ; Read mouse byte
    in al, KBC_DATA

    ; Desync guard: bytes within a packet arrive <2ms apart.
    ; >=2 BIOS ticks (~110ms) since the previous byte means a
    ; new packet is starting - re-arm framing at index 0.
    push es
    mov bx, 0x0040
    mov es, bx
    mov bx, [es:0x006C]             ; Current BIOS tick (atomic: IF=0 here)
    pop es
    mov cx, bx
    sub cx, [mouse_last_byte_tick]  ; Elapsed ticks (wrap-safe)
    mov [mouse_last_byte_tick], bx
    cmp cx, 2
    jb .store_byte
    mov byte [mouse_packet_idx], 0  ; Idle gap - restart framing
.store_byte:

And add next to mouse_packet_idx (line 20129):

mouse_last_byte_tick: dw 0          ; BIOS tick of last mouse byte (desync guard)

Optional hardening (not required): after the existing 'test al, 0x08 / jz .resync' for the idx-0 byte, also drop candidates with both overflow bits set ('mov ah, al / and ah, 0xC0 / cmp ah, 0xC0 / je .resync') - a genuine status byte virtually never has XO and YO simultaneously set, while large delta bytes often do. Additionally, moving the mouse_buttons store (lines 1067-1069) to after the 0xC0 overflow check (1072-1074) prevents discarded packets from injecting phantom button states.

## [medium/high] Two-instruction race between mouse_cursor_hide and 'inc cursor_locked' lets IRQ12 redraw the cursor, baking stale background into the screen
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:192-193 ('call mouse_cursor_hide' / 'inc byte [cursor_locked]'); same pattern at 34 other sites listed in explanation | area:mouse cursor / video
DESC: The dispatcher's cursor protection is 'call mouse_cursor_hide / inc byte [cursor_locked]' (same unprotected pair at win_begin_draw 1367-1368, mouse_process_drag 4469-4470, gfx_clear_area_stub 9644-9645, win_draw_stub 17459-17460, and other drawing stubs). mouse_cursor_hide is internally CLI-protected, but between its RET and the 'inc', IF=1 (dispatcher does STI at line 151). If IRQ12 fires in that gap it calls mouse_cursor_show (cursor_locked still 0), saves the under-cursor background and draws the cursor; the lock then engages, the API draws over/around the drawn cursor, and the trailing 'dec/mouse_cursor_show' is a no-op because cursor_visible is already 1. The next cursor move restores the STALE saved background over freshly drawn app pixels (VGA/VESA) or leaves XOR ghosts (CGA). With IRQ12 firing 40-100x/s during movement and every drawing syscall opening this window, this is a credible source of the reported 'visual anomalies' (droppings/ghost rectangles near the cursor).
VERIFIER: CONFIRMED by direct code trace in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. (1) IF=1 at the gap: int_80_handler executes STI at line 151; mouse_cursor_hide (3735-3770) is internally pushf/cli/popf so it returns with IF=1. (2) The unprotected pair is lines 192-193 (finding said 191-194; 191 is a comment, 194 sets _did_cursor_protect): 'call mouse_cursor_hide' / 'inc byte [cursor_locked]'. An interrupt can be taken at the popf->ret and ret->inc boundaries while cursor_visible=0 and cursor_locked=0. (3) IRQ12 (int_74_handler line 1019, and mouse_bios_callback line 1184) calls mouse_cursor_hide (no-op, visible=0), updates mouse_x/y, then mouse_cursor_show (line 1136/1268), whose lock check at 3779 passes (cursor_locked still 0) -> it saves the under-cursor background and draws the cursor, setting cursor_visible=1. (4) Line 193 then locks with the cursor still on screen; the drawing API runs and can overwrite the cursor footprint, making the save buffer stale. (5) No compensating mechanism exists: int80_return_point (377-391) does dec + mouse_cursor_show, but show no-ops at 3783-3784 because cursor_visible is already 1 — the background is never re-saved; nothing else checks for 'visible while locked'. Subsequent IRQ12s during the locked API skip hide/show entirely (3738-3739, 3779-3780). (6) The next cursor move's mouse_cursor_hide (locked=0, visible=1) restores the STALE save buffer over freshly drawn pixels (cursor_restore_vga/vesa), or XOR-erases over changed pixels leaving garbage in CGA/Mode12h (cursor_xor_sprite, 3755). Exactly the claimed dropping/ghost mechanism. The identical unprotected pair exists at 35 sites, all in IF=1 main-loop/syscall context: 192-193, 1367-1368 (win_begin_draw), 4469-4470/4531-4532/4579-4580/4639-4640 (drag/resize XOR outline — note the author DOES cli/sti-protect drag-state reads at 4452-4457 but not this pair), 7498-7499, 7524-7525, 7928-7929, 8166-8167, 8250-8251, 8298-8299, 8401-8402, 8591-8592, 8672-8673, 8868-8869, 9214-9215, 9391-9392, 9539-9540, 9644-9645 (gfx_clear_area_stub), 9828-9829, 9844-9845, 9859-9860, 15107-15108, 15493-15494, 15548-15549, 15628-15629, 15954-15955, 16379-16380, 17459-17460 (win_draw_stub), 17875-17876, 18183-18184, 18274-18275, 18313-18314, 18520-18521. The drag-outline sites (4469+) are the most likely visible offenders since drawing there always overlaps the cursor position. The unlock side (dec + show) was verified safe as claimed: an IRQ between dec and show draws the cursor itself with a fresh, correct background save, and the dispatcher's show then no-ops harmlessly. Two refinements to the finding: the pair is at 192-193 (not 191-194), and the per-call window is only ~2 instruction boundaries, so this yields intermittent rather than constant artifacts — consistent with 'occasional visual anomalies' and a medium severity rating. POPF has no architectural STI-style interrupt shadow, and even if it did, the ret->inc boundary remains interruptible, so nothing closes the window.
FIX: Add an atomic helper immediately after mouse_cursor_hide (after line 3770 in kernel/kernel.asm):

; cursor_protect - Atomically hide cursor and take the render lock.
; Closes the IRQ12 race where the cursor could be redrawn between
; mouse_cursor_hide returning and 'inc byte [cursor_locked]'.
; Preserves all registers and flags; safe in any IF state (PUSHF/CLI/POPF
; nests correctly with mouse_cursor_hide's own PUSHF/CLI/POPF).
cursor_protect:
    pushf
    cli
    call mouse_cursor_hide
    inc byte [cursor_locked]
    popf
    ret

Then replace every adjacent two-instruction pair
    call mouse_cursor_hide
    inc byte [cursor_locked]
with
    call cursor_protect
at all 35 sites: lines 192-193, 1367-1368, 4469-4470, 4531-4532, 4579-4580, 4639-4640, 7498-7499, 7524-7525, 7928-7929, 8166-8167, 8250-8251, 8298-8299, 8401-8402, 8591-8592, 8672-8673, 8868-8869, 9214-9215, 9391-9392, 9539-9540, 9644-9645, 9828-9829, 9844-9845, 9859-9860, 15107-15108, 15493-15494, 15548-15549, 15628-15629, 15954-15955, 16379-16380, 17459-17460, 17875-17876, 18183-18184, 18274-18275, 18313-18314, 18520-18521.

Leave the unlock side ('dec byte [cursor_locked]' / 'call mouse_cursor_show') unchanged — it is race-free as-is. The popf also discards the INC's flag effects, preserving the caller's CF (matching mouse_cursor_hide's existing register/flag-preservation contract, important because the INT 0x80 return path propagates CF as error status). All instructions are 8086-safe.

## [medium/medium] IRQ12 draws the cursor via VESA BIOS INT 0x10 bank switches inside the hardware interrupt - very long IF=0 windows and BIOS reentrancy risk
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm:1077 and 1136 (int_74_handler), 1220 and 1268 (mouse_bios_callback) — cost centers: 2789-2804 (vesa_set_bank INT 0x10), 3594-3691 and 3695-3728 (448-pixel VESA cursor walk), all under IF=0 | area:mouse / video / interrupt latency
DESC: int_74_handler (and mouse_bios_callback at 1220/1268) calls mouse_cursor_hide + mouse_cursor_show in interrupt context. In VESA mode these walk up to 448 pixels each through vesa_read_pixel/vesa_plot_pixel (lines 2809-2860), each of which may invoke 'mov ax, 0x4F05 / int 0x10' in vesa_set_bank (lines 2796-2799). That means: (a) hundreds of microseconds to milliseconds with interrupts disabled on EVERY mouse packet, delaying IRQ1 and stalling 8042 keyboard delivery (input lag / lost-feeling keys while moving the mouse); (b) the video BIOS is invoked from inside a hardware IRQ - if the IRQ interrupted a task that was itself inside INT 0x10 (mode set, font ops, another bank switch), the non-reentrant BIOS state can be corrupted, producing wrong-bank writes and screen garbage. Also vesa_cur_bank (line 2790-2792) is updated from IRQ context under a task that may be mid-row in vesa_fill_rect.
VERIFIER: PARTIALLY CONFIRMED — the interrupt-latency half is real; the BIOS-reentrancy/bank-corruption half is refuted by an existing mechanism the auditor missed.

CONFIRMED (latency, part a): int_74_handler (kernel\kernel.asm:1019) is entered with IF=0 and never executes STI. On every completed 3-byte packet it calls mouse_cursor_hide (1077) and mouse_cursor_show (1136); mouse_bios_callback (1220/1268) does the same from inside the BIOS's own IRQ12 handler (installed via INT 15h/C207, line 764). In VESA mode (video_mode==0x01), hide runs cursor_restore_vesa (3695-3728: 28 rows x 16 px = up to 448 vesa_plot_pixel calls) and show runs cursor_save_and_draw_vesa (3594-3691: 448 vesa_read_pixel + up to 448 vesa_plot_pixel). Every pixel op performs a 16-bit MUL plus a vesa_set_bank check (2789-2804) that issues 'mov ax,0x4F05 / int 0x10' on any bank change — typically 1-2 INT 0x10 per redraw thanks to the vesa_cur_bank cache, but ~100 per hide+show when the cursor straddles a 64K bank boundary (rows ~102/204/307/409, since 65536/640=102.4). The pushf/cli inside hide/show (3736-3737, 3777-3778) plus the interrupt gate keep IF=0 for the entire ~1300-pixel walk, blocking IRQ1/IRQ0 — plausibly 0.5-2 ms per packet on period ISA-video hardware at 40-200 packets/s (tens of µs in QEMU). The mode-0x13 path also draws in IRQ but is a cheap direct copy with no INT 0x10; CGA is a trivial XOR sprite. So the finding is correctly scoped to VESA.

REFUTED (corruption/reentrancy, part b): the auditor missed cursor_locked (20142) and the dispatcher's centralized 'A1' cursor protection. The INT 0x80 dispatcher wraps EVERY drawing API with 'call mouse_cursor_hide / inc byte [cursor_locked]' before dispatch (lines 191-194, gated by api_drawing_bitmap at 20078 covering APIs 0-6, 50-52, 56-62, 65-71, 80, 87, 94, 102-104) and 'dec / call mouse_cursor_show' at int80_return_point (388-391). All ~40 kernel-internal drawing regions (window manager, taskbar, menus) and set_video_mode (18520-18521 ... 18763-18764) hold the same lock. When cursor_locked>0, mouse_cursor_hide/show return immediately (3738-3741, 3779-3784) WITHOUT touching VRAM, vesa_cur_bank, or INT 0x10. A task can only be inside INT 0x10 (vesa_set_bank from drawing, or set_video_mode) or mid-rep-stosb in vesa_fill_rect while cursor_locked>0 — so IRQ12's cursor calls no-op in exactly the windows the finding worries about: no nested video BIOS entry, no wrong-bank writes, no vesa_cur_bank movement under a task mid-row. Task-context cursor draws themselves run fully under pushf/cli, so they can't be interleaved either. Residual holes are minor: the 1-instruction gap between 'call mouse_cursor_hide' and 'inc [cursor_locked]' (192-193, 1367-1368, 18520-18521) can at worst leave a cosmetic ghost (stale save buffer), not corruption — the bank cache stays coherent because vesa_set_bank updates cache and hardware together with the IRQ unable to split a task that isn't in a locked region (no VESA drawing happens outside locked regions). The only surviving reentrancy path would be a video BIOS that executes STI inside its own 4F05 handler — not the case for QEMU/SeaBIOS vgabios, and speculative on real cards.

NET: real perf/latency issue (medium on real hardware in VESA mode, minor in QEMU); the screen-corruption claim should be dropped. The suggested fix direction is valid and its anchor exists: event_get_stub (10117) already runs deferred IRQ12 work via mouse_process_drag (10126, 'Deferred Drag Processing' 4309-4314). Deferral is safe because mouse_cursor_hide erases at cursor_drawn_x/y recorded at draw time (3748-3749, 3793-3794), not at current mouse_x/y, and clearing the dirty flag before redrawing avoids lost updates. Behavioral note: cursor motion becomes tied to event-poll frequency, the same dependency mouse_process_drag already has (apps poll API 9 in their main loops); a more conservative variant gates the deferral on video_mode==0x01 only, keeping the cheap synchronous CGA/VGA13 draws.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm (all 8086-safe):

1) int_74_handler — delete the hide at 1076-1077:
   replace
       ; Erase cursor before updating position
       call mouse_cursor_hide
   with
       ; Cursor redraw deferred to task context (mouse_cursor_sync)

2) int_74_handler — replace the show at 1135-1136:
   replace
       ; Redraw cursor at new position
       call mouse_cursor_show
   with
       ; Mark cursor dirty; event_get_stub redraws in task context
       mov byte [cursor_dirty], 1

3) mouse_bios_callback — delete 1219-1220:
       ; Hide cursor before updating position
       call mouse_cursor_hide

4) mouse_bios_callback — replace 1268:
       call mouse_cursor_show
   with
       mov byte [cursor_dirty], 1

5) Add after mouse_cursor_show's ret (line 3815):

; mouse_cursor_sync - Deferred cursor redraw (task context)
; IRQ12 only sets cursor_dirty; this erases the cursor at its old
; position (cursor_drawn_x/y) and redraws at current mouse_x/y.
; Flag is cleared BEFORE drawing so a concurrent IRQ12 update
; re-marks it (no lost redraws). hide/show are internally cli-safe.
mouse_cursor_sync:
    cmp byte [cursor_dirty], 0
    je .mcy_done
    mov byte [cursor_dirty], 0
    call mouse_cursor_hide
    call mouse_cursor_show
.mcy_done:
    ret

6) event_get_stub — after 'call mouse_process_drag' (line 10126) add:
    call mouse_cursor_sync          ; Deferred cursor redraw (from IRQ12)

7) Data — next to cursor_locked (line 20142) add:
cursor_dirty:       db 0            ; 1 = IRQ12 moved mouse, cursor redraw pending

This shrinks the IRQ12 IF=0 window to the few-instruction packet/state update and removes all INT 0x10 and VRAM walking from interrupt context. Optional conservative variant: in steps 1-4 branch on 'cmp byte [video_mode], 0x01 / jne <keep synchronous hide/show>' to defer only in VESA mode, preserving existing per-packet cursor draw in CGA/VGA modes.

## [medium/high] EVENT_MOUSE carries no coordinates and no press/release edge - click position is read after the fact (race), and any task can consume another task's mouse events
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm:1138-1142 and 1270-1274 (buttons-only EVENT_MOUSE posts, both PS/2 and BIOS paths); kernel\kernel.asm:10157-10160 (EVENT_MOUSE consumed by any task); apps\launcher.asm:177-215 (poll-time hit test) and 269-273 (discards stolen mouse events); apps\music.asm:362-401 (affected consumer) | area:mouse / event routing
DESC: The IRQ posts 'xor dx, dx / mov dl, [mouse_buttons] / mov al, EVENT_MOUSE / call post_event' - buttons only, no X/Y, no edge information. Consumers must call mouse_get_state (API 28) later, so the click coordinates they hit-test are wherever the cursor is at POLL time, not at press time (launcher.asm lines 177-215 does exactly this). Fast click-and-move lands clicks on the wrong icon/widget. Additionally event_get_stub line 10157-10160 consumes EVENT_MOUSE for ANY current task ('.evt_not_key ... jne .evt_consume ; Other event types: consume and pass through') - the launcher's poll loop (launcher.asm 269-272) consumes and discards every non-KEY event it sees, so a focused app that does rely on EVENT_MOUSE receives only a random subset of packets.
VERIFIER: Every element of the finding checks out against the code.

1) No coordinates/edge in the event — CONFIRMED. Both mouse paths post buttons-only: the PS/2 IRQ at kernel\kernel.asm:1138-1142 and the BIOS-callback path at 1270-1274 both do `xor dx,dx / mov dl,[mouse_buttons] / mov al,EVENT_MOUSE / call post_event`. DH is zeroed; events are 3 bytes (type + data word, lines 10076-10078), so there is no room used for X/Y, and an event is posted for EVERY completed packet (motion included), not just button changes — so consumers cannot even infer edges reliably from event arrival.

2) Poll-time coordinate race — CONFIRMED. launcher.asm:177-215 reads position+buttons via API 28 (mouse_get_state, kernel.asm:1294-1299, which returns live [mouse_x]/[mouse_y]) once per main-loop iteration and synthesizes a rising edge by comparing to the previous iteration's buttons. The press happened anywhere inside the inter-poll window (one full cooperative scheduling round: API_APP_YIELD at launcher.asm:128 plus GET_SCREEN_INFO/GET_TASK_INFO calls and any other task's slice), during which IRQ12 keeps updating mouse_x/mouse_y at packet rate. The hit test therefore uses wherever the cursor is at poll time. The race also affects event-driven consumers: widget_hit_test (API 53, kernel.asm:9037, used by music.asm/settings.asm) reads live [mouse_x]/[mouse_y] at lines 9073/9081, so even a task that receives the press event hit-tests the drifted position.

3) Cross-task consumption — CONFIRMED. In event_get_stub, only EVENT_KEY_PRESS is focus-filtered (10142-10155) and only EVENT_WIN_REDRAW is owner-filtered (10157-10177); EVENT_MOUSE falls through `.evt_not_key` / `jne .evt_consume` (10159-10160) and is dequeued by whichever task calls event_get first. The launcher keeps calling API_EVENT_GET every loop iteration even while a window has focus (launcher.asm:171-172 jumps to .input_ok when BL!=0xFF; only the fullscreen-app case at 173-174 skips input), and discards anything that is not EVENT_KEY_PRESS (launcher.asm:269-273 `jc .no_event / cmp al,EVENT_KEY_PRESS / jne .no_event`). A real in-tree victim exists: apps\music.asm relies on EVENT_MOUSE for clicks (poll_events at 305-314, .mouse at 362-401) with its own prev_btn edge detection — if the launcher dequeues the packet carrying the 0→1 transition, music either loses the click entirely (press+release both stolen or release-only seen) or registers it late at the drifted cursor position when a subsequent held-motion packet arrives. settings.asm survives only because it falls back to polling API_MOUSE_STATE every iteration regardless of events (settings.asm:173, 198-205).

4) No hidden mitigation. I looked for one: there is none. The fix premise is also sound — mouse_drag_update runs in IRQ context (called at 1133 and 1267) and already latches press-time coordinates for window drag/resize (drag_offset_x/y at 4071-4077, resize_start_mx/my at 4109-4112), so press-time latching for app clicks is consistent with the existing design.

One caveat on the suggested fix: blindly routing EVENT_MOUSE to the focused task the way KEY_PRESS is filtered would be risky as-is, because EVENT_MOUSE is posted per packet (high rate) and the queue uses head-blocking semantics — a focused app that never drains events would stall the 32-entry queue for everyone. Posting mouse events only on button-state change (no in-tree consumer needs motion events; music only wants edges, settings polls anyway) makes the focus-routing safe.

Severity medium is fair: user-visible dropped/misplaced clicks in any windowed app using EVENT_MOUSE (music.asm today), plus wrong-icon desktop clicks under fast click-and-move or scheduler lag.
FIX: Three-part minimal fix (8086-safe, matches existing kernel idioms):

(1) Latch press-time state in the IRQ. In mouse_drag_update (kernel\kernel.asm:4024-4028 already computes the rising edge), add a latch for ALL buttons right after the edge computation at line 4022:

    mov al, [mouse_buttons]
    mov ah, [drag_prev_buttons]
    mov [drag_prev_buttons], al
+   ; Latch press-time coordinates on any newly-pressed button
+   push ax
+   not ah
+   and ah, al                  ; AH = rising-edge buttons
+   jz .no_click_latch
+   mov [click_buttons], ah
+   mov ax, [mouse_x]
+   mov [click_x], ax
+   mov ax, [mouse_y]
+   mov [click_y], ax
+   inc byte [click_seq]        ; consumers detect new presses by seq change
+.no_click_latch:
+   pop ax

(restore AH from the pop before the existing `test ah, 0x01` — i.e., place the block so the original AL/AH pair is intact afterwards; the push/pop ax shown does that). Add kernel vars near mouse_buttons: `click_x dw 0`, `click_y dw 0`, `click_buttons db 0`, `click_seq db 0`.

(2) Expose it: extend mouse_get_state (kernel.asm:1294) to also return SI=click_x, DI=click_y, AH=click_seq (its API slot 28 has SI/DI free), or add a new syscall slot `mouse_get_click` returning BX=click_x, CX=click_y, DL=click_buttons, DH=click_seq. Launcher then detects a press by `click_seq != last_seq` (instead of prev_buttons compare) and hit-tests click_x/click_y — this fixes both the missed-press window and the coordinate race in launcher.asm:196-238.

(3) Stop the cross-task theft: post EVENT_MOUSE only on button-state change, and focus-route it. At kernel.asm:1138-1142 (and the identical 1270-1274):

 .post_event:
     call mouse_drag_update
     call mouse_cursor_show
+    mov al, [mouse_buttons]
+    cmp al, [last_posted_buttons]
+    je .reset_packet            ; no button change: no event (motion is pollable)
+    mov [last_posted_buttons], al
     xor dx, dx
     mov dl, [mouse_buttons]
     mov al, EVENT_MOUSE
     call post_event

(add `last_posted_buttons db 0`). Then in event_get_stub at 10157, mirror the KEY_PRESS focus filter for EVENT_MOUSE before the WIN_REDRAW check:

 .evt_not_key:
+    cmp al, EVENT_MOUSE
+    jne .evt_not_mouse
+    push ax
+    mov al, [focused_task]
+    cmp al, 0xFF
+    je .evt_mouse_ok            ; no focus: current task may consume
+    cmp al, [current_task]
+    je .evt_mouse_ok
+    pop ax
+    jmp .no_event               ; leave in queue for focused task
+.evt_mouse_ok:
+    pop ax
+    jmp .evt_consume
+.evt_not_mouse:
     cmp al, EVENT_WIN_REDRAW
     jne .evt_consume

The edge-only posting in (3) is what makes the focus filter safe: button-change events are low-rate, so a focused task that drains events slowly cannot flood the 32-entry head-blocking queue the way per-motion-packet events would. In-tree consumers are compatible: music.asm only acts on button edges, settings.asm polls state every iteration regardless of events, launcher discards mouse events anyway.

## [medium/high] Keyboard focus is evaluated at consume time, not press time - keystrokes leak across focus changes and app launches
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:10142-10155 (consume-time focus filter, as cited); contributing lines: 619-625 (unstamped post in INT 9), 10097-10099 (3-byte event records), 15227 / 16461 / 17840 (focus mutations with no queue flush) | area:event routing / focus
DESC: Events carry no destination: the filter compares [focused_task] to [current_task] when the event is READ. Keys typed while app A is focused but still sitting in the queue when focus moves to app B (window close promotes a new focus at 16461/17840, win_create sets it at 15227) are delivered to B. Keys typed during an app launch (focused_task only set when the new window is created at line 15227) are delivered to whichever task polls first once focus flips - e.g. the Enter of a double-click launch leaks into the new app as a keystroke. With focused_task reset to 0xFF (line 16461), 'cmp al, 0xFF / je .evt_focus_ok' hands queued keys to ANY polling task.
VERIFIER: CONFIRMED, with one example corrected and a smaller fix than suggested. Verified in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm: (1) Events are 3 bytes with no destination (post_event lines 10097-10099); the INT 9 handler posts EVENT_KEY_PRESS with 'xor dx,dx / mov dl,al' (lines 619-625), so DH is always 0 in queued key events. (2) event_get_stub filters at consume time (lines 10142-10155) by comparing the CURRENT [focused_task] to [current_task]; a non-focused task leaves the key queued (jmp .no_event, line 10152). (3) focused_task mutates at win_create (15227), win_focus (17840), and window-close promotion (16470 -> 17840) / reset-to-0xFF (16461), and NO code path ever flushes the queue on focus change: the only writes to event_queue_head in the whole kernel are event_get's own consume (10183) and discard (10200). So keys typed while task A is focused but still queued when focus moves to B are consumed by B - real, no hidden protection. (4) The 0xFF wildcard (lines 10147-10148) delivers queued keys to ANY polling task; note the launcher (apps/launcher.asm) never creates a window, so the desktop runs with focused_task=0xFF by design - the wildcard is the launcher's only key source and must be preserved by any fix. (5) Launch leak: the auditor's specific example is wrong - the Enter/double-click that triggers the launch IS consumed by the launcher before launch_app (launcher.asm lines 283-284, 1250-1252). But the substance holds: keys typed after the gesture during the multi-second floppy load queue up; once the new app's win_create sets focused_task (15227), the app consumes them (e.g. a rapid double-Enter leaks the second Enter into the new app). Severity medium is fair: input misrouting, max 32 queued events, realistic triggers are click-to-switch while typing, app busy with disk I/O, and app launch. The suggested 4-byte record grow is unnecessary: since queued key events always have DH=0 (null keys dropped at line 589; the DH=scancode convention applies only to the non-queued BIOS INT 16h synth path at 10273-10277, and that fallback is dead anyway because install_keyboard sets use_bios_keyboard=0 at line 454), DH is a free destination-stamp slot. Stamp [focused_task] into DH at press time in the INT 9 handler (DS=0x1000 there, line 468-469, so [focused_task] is addressable), filter on the stamp in event_get, discard stale stamps via the existing .evt_discard path (lazy flush - no changes needed at focus-change sites), and zero DH on delivery to preserve the app-visible DH=0 contract.
FIX: Two edits in kernel/kernel.asm, 8086-safe, no record-size change (uses the always-zero DH of queued key events as the press-time focus stamp).

EDIT 1 - int_09_handler .skip_buffer (lines 618-626), stamp focus at press time. Replace:
    push ax
    xor dx, dx
    mov dl, al                      ; DX = ASCII character
    mov al, EVENT_KEY_PRESS         ; AL = event type
    call post_event
    pop ax
with:
    push ax
    mov dl, al                      ; DL = ASCII character
    mov dh, [focused_task]          ; DH = focus owner at PRESS time (0xFF = none)
    mov al, EVENT_KEY_PRESS         ; AL = event type
    call post_event
    pop ax
(DS is already 0x1000 here, set at line 468.)

EDIT 2 - event_get_stub key filter (lines 10142-10155). Replace:
    cmp al, EVENT_KEY_PRESS
    jne .evt_not_key
    push ax
    mov al, [focused_task]
    cmp al, 0xFF
    je .evt_focus_ok
    cmp al, [current_task]
    je .evt_focus_ok
    pop ax
    jmp .no_event
.evt_focus_ok:
    pop ax
    jmp .evt_consume
with:
    ; Route key events by the focus stamp captured at PRESS time (DH)
    cmp al, EVENT_KEY_PRESS
    jne .evt_not_key
    cmp dh, 0xFF                    ; Pressed while nothing focused?
    jne .evt_key_stamped
    cmp byte [focused_task], 0xFF   ; Still nothing focused?
    je .evt_key_deliver             ; Yes: deliver to poller (launcher path)
    jmp .evt_discard                ; Focus gained since press: stale, drop
.evt_key_stamped:
    cmp dh, [focused_task]
    jne .evt_discard                ; Focus moved since press: stale, drop
    cmp dh, [current_task]
    jne .no_event                   ; Focused task's key, not us: leave queued
.evt_key_deliver:
    xor dh, dh                      ; Restore DH=0 contract for apps
    jmp .evt_consume

Notes: .evt_discard (line 10196) already advances head and loops to .evt_check_next, so stale keys are flushed lazily by whichever task polls next - no changes needed in win_focus_stub/win_destroy. Keys typed during an app launch are stamped 0xFF and dropped once the new window takes focus, fixing the launch leak while preserving launcher keyboard input on the bare desktop (launcher has no window, focused_task stays 0xFF). Zeroing DH on delivery keeps app-visible data identical to today (queued events always had DH=0; the DH=scancode case exists only in the non-queued BIOS INT 16h fallback, untouched by this change).

## [medium/high] gfx_blit_rect copies forward regardless of overlap in CGA, and within-row overlap is wrong in the VGA reverse path
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: CGA path 6986-7017 (unconditional forward loops, inc at 7012/7015); VGA direction check 7035-7036 correct, but reverse path within-row forward copy at 7078-7080 (`mov cx,[cs:_blit_width]` / `cld` / `rep movsb`) | area:gfx_blit_rect (API 103)
DESC: The CGA/fallback path (6986-7017) iterates row 0..h-1, col 0..w-1 top-down/left-right unconditionally. For overlapping copies with dst below/right of src (the common 'scroll content down' use), destination rows are written before they are read as source: once row >= (dst_y - src_y) it re-reads already-overwritten pixels, smearing the first stripe across the rest. The VGA 13h path does check direction ('cmp ax,[_blit_src_off] / ja .blit_vga_reverse', 7034-7036) and copies rows bottom-up, but within each row it still uses forward 'cld / rep movsb' (7079-7080), so a same-row overlap with dst_x > src_x (horizontal scroll right) corrupts within the row.
VERIFIER: Both halves of the finding are real, verified by reading the code in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm.

(1) CGA/fallback path (lines 6986-7017): the nested loops iterate _blit_row 0..h-1 and _blit_col 0..w-1 unconditionally (inc at 7012/7015, no direction check anywhere in the path). Each iteration reads the source pixel from LIVE video memory via read_pixel_internal (line 6790: `mov al,[es:di]` from the video segment; bounds-checked but no shadow buffer) and writes via plot_pixel_color (line 7709 / 7726, also direct VRAM). Therefore for an overlapping blit with dst_y > src_y and dst_y - src_y < height, once row r >= (dst_y - src_y), the source pixel at row src_y+r was already overwritten by the iteration r' = r - (dst_y - src_y) (r' < r, processed earlier) — the top stripe is smeared down the whole rect. Same defect for dst_y == src_y with dst_x > src_x (within-row left-to-right). Note one refinement: this slow path only fully functions in CGA mode 0x04 — in modes 0x01/0x12 read_pixel_internal returns 0 (lines 6770-6772), so the blit just fills black there (a separate limitation).

(2) VGA 13h reverse path (7060-7082): the direction check at 7035-7036 (`cmp ax,[_blit_src_off]` with AX = dst_off, `ja .blit_vga_reverse`) correctly routes dst_off > src_off to bottom-up row order, which fixes inter-row overlap. But within each row it still does `cld / rep movsb` (7079-7080). When the per-row src and dst byte ranges overlap with dst > src — i.e. dst_y == src_y and 0 < dst_x - src_x < width (horizontal scroll right), or the edge case 0 < dst_off - src_off < width with dst one row down and dst_x far left of src_x — the forward movsb replicates the first (dst_off - src_off) bytes across the row. I verified that dst_off > src_off implies dst_y >= src_y (since |dst_x - src_x| < 320), so the bottom-up row order itself is correct; only the within-row direction is wrong. I also verified the proposed fix's ordering: with width <= 255 < 320, reverse-row + reverse-column writes destination bytes in strictly decreasing linear address, which is overlap-safe for every dst_off > src_off.

No mechanism elsewhere prevents the bug: API 103 is dispatched straight through the syscall table (line 7472) with no clipping or overlap rejection; API_REFERENCE.md does not document API 103 at all (no no-overlap contract); the function header (6931-6936) is silent on overlap; no kernel-internal or in-tree app caller exists yet, so the bug is currently latent — but README.md line 175 explicitly advertises the blit as "used for smooth scrolling" (the overlapping case), and the VGA path's own direction check proves overlapping copies were intended to be supported. Severity medium is fair: visual corruption only (both pixel helpers bounds-check, no memory-safety impact).
FIX: Two minimal 8086-safe patches in kernel/kernel.asm:

--- Patch 1: VGA reverse path, replace lines 7078-7080 ---
Old:
    mov cx, [cs:_blit_width]
    cld
    rep movsb
New:
    mov cx, [cs:_blit_width]
    add si, cx
    dec si                          ; SI -> last source byte of row
    add di, cx
    dec di                          ; DI -> last dest byte of row
    std                             ; copy right-to-left (overlap-safe for dst>src)
    rep movsb
    cld                             ; restore default direction flag

(Width is validated nonzero at 6949-6950, so SI/DI never underflow. Restoring CLD matters because the kernel relies on forward string ops elsewhere.)

--- Patch 2: CGA/fallback path, add direction selection. Replace lines 6986-6989 ---
Old:
    ; --- CGA / fallback: pixel-by-pixel copy ---
    mov ax, [video_segment]
    mov es, ax
    mov word [_blit_row], 0
New:
    ; --- CGA / fallback: pixel-by-pixel copy ---
    mov ax, [video_segment]
    mov es, ax
    ; Overlap-safe direction choice (memmove semantics)
    mov ax, [_blit_dst_y]
    cmp ax, [_blit_src_y]
    ja .blit_slow_rev               ; dst below src: copy bottom-up
    jb .blit_slow_fwd
    mov ax, [_blit_dst_x]
    cmp ax, [_blit_src_x]
    ja .blit_slow_rev               ; same row, dst right of src: copy right-to-left
.blit_slow_fwd:
    mov word [_blit_row], 0

...and insert before line 7017 (.blit_slow_done), after the existing `jmp .blit_slow_row`:
.blit_slow_rev:
    mov ax, [_blit_height]
    dec ax
    mov [_blit_row], ax             ; start at last row
.blit_rev_row:
    cmp word [_blit_row], 0
    jl .blit_slow_done              ; wraps to 0FFFFh after row 0 (h<=255, signed safe)
    mov ax, [_blit_width]
    dec ax
    mov [_blit_col], ax             ; start at last column
.blit_rev_col:
    cmp word [_blit_col], 0
    jl .blit_rev_col_done
    mov cx, [_blit_src_x]
    add cx, [_blit_col]
    mov bx, [_blit_src_y]
    add bx, [_blit_row]
    call read_pixel_internal        ; AL = color
    mov dl, al
    mov cx, [_blit_dst_x]
    add cx, [_blit_col]
    mov bx, [_blit_dst_y]
    add bx, [_blit_row]
    call plot_pixel_color
    dec word [_blit_col]
    jmp .blit_rev_col
.blit_rev_col_done:
    dec word [_blit_row]
    jmp .blit_rev_row

(Bottom-up rows handle dst_y > src_y for any dx, since reads and writes within one iteration touch different screen rows; right-to-left columns additionally handle the dst_y == src_y, dst_x > src_x case. The wrap-to-0xFFFF / `jl` exit idiom matches the existing VGA reverse loop at 7069-7070.)

## [medium/high] gfx_blit_rect produces black fills in VESA and mode 12h because read_pixel_internal returns 0 for those modes
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm 6766-6772 (mode dispatch + zero fallback in read_pixel_internal); contributing: 6984-6985 (blit fast-path check), 7004 (slow-loop read), 6813 (gfx_read_pixel also affected) | area:gfx_blit_rect / read_pixel_internal
DESC: read_pixel_internal handles only VGA 13h and CGA: 'cmp byte [cs:video_mode],0x13 / je .rpi_vga / cmp byte [cs:video_mode],0x04 / je .rpi_cga / ; VESA / Mode12h fallback: return 0 / xor al,al / ret'. gfx_blit_rect uses the pixel-by-pixel fallback for every mode except 13h (6984-6985), so in VESA (mode 0x01) and 12h a blit reads 0 for every source pixel and writes a solid black rectangle instead of copying the region. Apps using API 103 for scrolling/moving content in 640x480 modes blank the area.
VERIFIER: Confirmed by direct code reading of C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm.

Failure path, traced concretely:
1. Modes are reachable: set_video_mode (API 95) sets video_mode=0x12 for mode 12h (line 18606) and video_mode=0x01 as the internal VESA marker (line 18652); both set video_segment=0xA000, 640x480.
2. gfx_blit_rect (API 103, syscall table line 7472, function at 6937) checks only `cmp byte [video_mode],0x13 / je .blit_vga` (6984-6985). Every other mode — CGA, 12h, VESA — falls into the pixel-by-pixel slow loop (6986-7017), which calls read_pixel_internal (7004) then plot_pixel_color (7011) per pixel.
3. read_pixel_internal (6761) dispatches only on 0x13 (.rpi_vga) and 0x04 (.rpi_cga); for 0x01 and 0x12 it falls through to `xor al,al / ret` at 6770-6772 (the comment itself says "VESA / Mode12h fallback: return 0").
4. plot_pixel_color (7680) DOES handle 0x01 (tail call to vesa_plot_pixel, 7732-7733) and 0x12 (tail call to mode12h_plot_pixel, 7730-7731), so the write side succeeds: every destination pixel is written with color 0 = black. The function returns CF=0 (success), so the caller has no indication of failure.

No mitigating mechanism exists: nothing in the INT 0x80 dispatcher or gfx_blit_rect blocks API 103 in VESA/12h, and there is no alternate blit path for those modes. The same root cause also silently breaks gfx_read_pixel (API 104, line 6803, calls read_pixel_internal at 6813) — it returns AL=0 with CF=0 success in VESA and mode 12h, which the original finding understated.

The suggested fix direction is sound: vesa_read_pixel (2842) exists with the exact same calling convention (CX=X, BX=Y, ES=0xA000, returns AL, preserves BX/CX/DI/DX) and is already used elsewhere (3649, 3669). Its callee vesa_set_bank (2789) accesses [vesa_cur_bank] via DS, but both call sites (gfx_read_pixel and the blit slow path) run with DS=0x1000, the same invariant vesa_plot_pixel already relies on in that same loop, so the tail call is safe. Mode 12h has no existing read helper (grep confirms only fill/plot use the GC at 0x3CE); a planar read via GC index 4 (Read Map Select) is needed. Interleaving the GC read state with mode12h_plot_pixel's write state in the blit loop is safe because each helper programs the GC registers it needs on every call and Read Map Select does not affect writes.

Severity "medium" is fair: wrong output (black fill instead of copy) in the two 640x480 modes for APIs 103/104, no crash or memory corruption.
FIX: In read_pixel_internal, replace the fallback at lines 6770-6772 with dispatch to a VESA tail call and a new 8086-safe mode 12h planar read:

```nasm
    cmp byte [cs:video_mode], 0x13
    je .rpi_vga
    cmp byte [cs:video_mode], 0x04
    je .rpi_cga
    cmp byte [cs:video_mode], 0x01
    je .rpi_vesa
    cmp byte [cs:video_mode], 0x12
    je .rpi_m12
    xor al, al                          ; unknown mode: return 0
    ret
.rpi_vesa:
    jmp vesa_read_pixel                 ; CX=X, BX=Y, ES=0xA000 -> AL (line 2842)
.rpi_m12:
    push bx
    push cx
    push dx
    push di
    ; byte offset = Y*80 + X>>3
    mov ax, bx
    mov di, 80
    mul di                              ; AX = Y*80 (clobbers DX)
    mov di, cx
    shr di, 1
    shr di, 1
    shr di, 1                           ; DI = X/8 (8086: shift by 1 only)
    add di, ax                          ; DI = byte offset in plane
    ; CL = 7 - (X & 7) = bit position
    and cl, 7
    mov ch, 7
    sub ch, cl
    mov cl, ch
    xor bl, bl                          ; BL = assembled 4-bit color
    mov bh, 3                           ; plane 3 down to 0 (plane n = color bit n)
.rpi_m12_plane:
    mov dx, 0x3CE
    mov al, 4                           ; GC index 4 = Read Map Select
    out dx, al
    inc dx
    mov al, bh
    out dx, al                          ; select plane BH
    mov al, [es:di]
    shr al, cl
    and al, 1                           ; isolate pixel bit
    shl bl, 1
    or bl, al                           ; accumulate MSB-first (plane3 = bit3)
    dec bh
    jns .rpi_m12_plane
    mov al, bl
    pop di
    pop dx
    pop cx
    pop bx
    ret
```

This fixes both gfx_blit_rect's slow path (7004) and gfx_read_pixel/API 104 (6813) at once; the existing slow blit loop then copies correctly in VESA and mode 12h with no other changes. (Optional follow-up, not required for correctness: dedicated fast blit paths for VESA/12h, since the per-pixel loop does 4 OUTs per read in 12h and a bank check per pixel in VESA.)

## [medium/high] CGA byte-aligned fast paths of gfx_fill_color and gfx_clear_area_stub have no screen-bounds clamping
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:gfx_fill_color CGA fast path: kernel/kernel.asm 15979-16027 (VGA clamp actually at 16067-16089, not 16057-16075); gfx_clear_area_stub unchecked CGA paths: 9659-9698 (fast) and 9700-9793 (hybrid); safe pixel-path check at 9902-9905 | area:fill/clear primitives
DESC: gfx_fill_color's VGA path got a bounds clamp ('Bounds clamp (Build 397)', 16057-16075), but the CGA fast path (15977-16016) computes DI=(Y/2)*80+X/4 and 'rep stosb' width/4 bytes per row with X, width, Y, height completely unchecked. A rect whose right edge passes 320 writes into the following row's bytes; Y>=200 lands in the opposite interlace field or past the 16KB field, painting stripes elsewhere on screen. gfx_clear_area_stub's fast and hybrid CGA paths (9667-9792) have the same hole (only its narrow pixel path bounds-checks, 9902-9905). These are reachable from apps via API 67/2/5 with window-translated coordinates near the screen edges (e.g. a window dragged partially off-screen then cleared).
VERIFIER: Confirmed by direct reading of C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. (1) gfx_fill_color (15930) performs only zero-width/height checks (15959-15962); its CGA byte-aligned fast path (alignment check 15979-15985, row loop .gfc_row 15987-16027) computes DI=(Y/2)*80 (+0x2000 for odd Y) + X/4 and rep stosb's DX/4 bytes per row with X, Y, W, H completely unclamped. The VGA path has an explicit 'Bounds clamp (Build 397)' (16067-16089, slightly different lines than cited); the CGA slow path is safe because plot_pixel_color bounds-checks per pixel (7681-7684) — so only the fast path is exposed, exactly as claimed. (2) gfx_clear_area_stub (9630): byte-aligned fast path (9659-9698) and hybrid edge-byte+rep-stosb path (9700-9793) have no bounds checks; only .pixel_path is safe via .plot_bg's check (9902-9905). (3) Reachability: INT 0x80 API 2 (gfx_draw_filled_rect_stub, 9613, delegates to gfx_fill_color), API 5 (gfx_clear_area_stub), and API 67 (gfx_draw_filled_rect_color, 18184, wraps gfx_fill_color) are app-callable. The dispatcher (lines 196-352) translates BX/CX by window origin and doubles DX/SI for content_scale=2 windows, but never clips the rect to the window content area or screen — apps fully control W/H. Concrete failure: X=316,W=8,Y=0 fills CGA bytes 79-80, byte 80 being row 2's first byte (wrong-row pixels); even Y>=206 gives DI>=0x2030, inside the odd interlace field at 0x2000 (stripes on other scanlines), matching the finding. No other mechanism prevents it: only z-order clipping exists in the dispatcher, and win_move_stub's on-screen clamping (17925-17939) merely weakens the specific 'window dragged off-screen' example (it keeps windows fully on-screen unless larger than the screen — then clamping is skipped via the js branches); the trivial trigger of oversized W/H or scale-2 doubling needs no off-screen window. Severity medium is right: 16-bit mul/offset wraparound confines writes to the 64KB ES=0xB800 segment (physical B8000-C7FFF video aperture/ROM), so it is app-triggerable display corruption, not kernel memory corruption. Side note: the existing VGA clamp itself has a 16-bit overflow hole (add ax,dx wraps when BX+DX>65535, e.g. BX=319, DX=65300, bypassing the width clamp); the suggested fix below uses the wrap-free formulation and could also be retrofitted to the VGA path.
FIX: Two insertions, both 8086-safe and immune to 16-bit ADD wrap (uses screen_width-X subtraction instead of X+W addition).

1) In gfx_fill_color, insert immediately after line 15977 ('je .gfc_vga'), before the alignment check at 15979, so both CGA fast and slow paths are covered (AX is scratch here; DS=kernel as elsewhere in this function; .gfc_cursor_done correctly undoes the cursor lock taken at 15964-15965):

    ; CGA bounds clamp: reject off-screen origin, clip W/H to screen edge
    cmp bx, [screen_width]
    jae .gfc_cursor_done
    cmp cx, [screen_height]
    jae .gfc_cursor_done
    mov ax, [screen_width]
    sub ax, bx                      ; AX = max width from X (no 16-bit wrap)
    cmp dx, ax
    jbe .gfc_cga_w_ok
    mov dx, ax
.gfc_cga_w_ok:
    mov ax, [screen_height]
    sub ax, cx                      ; AX = max height from Y
    cmp bp, ax
    jbe .gfc_cga_h_ok
    mov bp, ax
.gfc_cga_h_ok:

(Clamped DX stays 4-aligned when BX is 4-aligned since screen_width=320 in this mode, so the fast-path alignment invariant is preserved; if ever unaligned it just falls into the bounds-checked slow path.)

2) In gfx_clear_area_stub, insert immediately after line 9657 ('mov bp, si'), before the alignment check at 9659, covering fast, hybrid, and pixel paths (.clear_done at 9812 pops the registers pushed at 9646-9653 and releases the cursor lock; use cs: overrides to match this function's existing convention at 9637/9655):

    ; CGA bounds clamp: reject off-screen origin, clip W/H to screen edge
    cmp bx, [cs:screen_width]
    jae .clear_done
    cmp cx, [cs:screen_height]
    jae .clear_done
    mov ax, [cs:screen_width]
    sub ax, bx                      ; AX = max width from X
    cmp dx, ax
    jbe .gca_w_ok
    mov dx, ax
.gca_w_ok:
    mov ax, [cs:screen_height]
    sub ax, cx                      ; AX = max height from Y
    cmp bp, ax
    jbe .gca_h_ok
    mov bp, ax
.gca_h_ok:

Optional hardening (same bug class): replace the VGA clamp's 'mov ax, bx / add ax, dx / cmp ax, [screen_width]' (16072-16074) and the height equivalent (16079-16081) with the same subtraction formulation to close the 16-bit wrap bypass.

## [medium/high] vesa_fill_rect skips an entire bank when a row starts exactly on a 64KB boundary (DI=0)
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:2900-2920 | area:vesa_fill_rect
DESC: Bytes-until-boundary is computed as 'mov dx,0 / sub dx,di' (= 0x10000-DI mod 65536). When DI==0 (row begins exactly at a bank start, e.g. y=102/x=256 → linear 65536) DX becomes 0, so 'cmp si,dx / jbe .vf_no_cross' takes the cross path with 0 bytes before the boundary: it fills nothing, increments the bank, and paints the whole row at offset 0 of the NEXT bank — 64KB (102 rows) below where it belongs, leaving the intended row unpainted and corrupting a distant one.
VERIFIER: Confirmed by direct code trace of vesa_fill_rect at C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm lines 2880-2925. Per-row, DX:AX = Y*640+X is computed (line 2884-2886), DI = low word (in-bank offset, line 2889), and the correct bank k is set (lines 2893-2894). Bytes-until-boundary is then computed as 'mov dx,0 / sub dx,di' (lines 2903-2904), which yields DX=0 when DI=0 (true value 65536 is unrepresentable in 16 bits). 'cmp si,dx / jbe .vf_no_cross' (lines 2905-2906) then falls into the cross path for any width >= 1: CX=DX=0 so the first 'rep stosb' (line 2909) writes nothing, the bank is incremented to k+1 (lines 2911-2913, vesa_cur_bank was just set to k), 'sub si,dx' leaves SI = full width, and the entire row is painted at offset 0 of bank k+1 (lines 2914-2918) -- 65536 bytes (~102 rows) below the intended location, while the intended row is left unpainted. Reachable in-bounds trigger pairs (640*Y+X = 65536*k, Y<480, X<640): (102,256), (204,512), (307,128), (409,384). No other mechanism prevents it: gfx_fill_color (line 15930), the general fill primitive used by desktop/window drawing, passes arbitrary X/Y/W/H straight to vesa_fill_rect at .gfc_vesa (line 16054) with only zero-width/zero-height guards (lines 15959-15962); the scroll-area clear callers at lines 19745 and 19754 also pass arbitrary coordinates. vesa_set_bank (line 2789) does no validation. The full-screen clear caller (line 9835, X=0) happens to be immune since 640*Y is never a multiple of 65536 for Y<480, but that does not protect the general callers. Impact is confined to the 0xA000 banked video window (display corruption: missing row plus stray painted row ~102 rows lower), so medium severity is appropriate. Minimal fix: 'sub dx,di' sets ZF exactly when DI=0, so insert a single 'jz .vf_no_cross' after line 2904; a row of width <= 65535 starting at offset 0 of a bank can never cross out of it, so the no-cross path (which does not use DX) is exactly correct, and both paths pop the saved DX identically.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, vesa_fill_rect, insert one instruction after 'sub dx, di' (line 2904):

    mov dx, 0
    sub dx, di                     ; DX = bytes until boundary (0x10000 - DI)
    jz .vf_no_cross                ; DI=0: full 64KB remains in this bank; row (width <= 0xFFFF) cannot cross
    cmp si, dx                     ; width <= remaining?
    jbe .vf_no_cross

8086-safe (SUB sets ZF; JZ short reaches .vf_no_cross). .vf_no_cross does not read DX and pops the saved bank DX the same as the cross path, so no other change is needed.

## [medium/high] vesa_set_bank ignores VESA window granularity (vesa_gran captured but never used)
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:mov dx, ax / mov cl, [vesa_bank_shift] / shl dx, cl   ; convert 64KB-unit bank to granularity units before INT 10h AX=4F05 | area:VESA bank switching
DESC: vesa_set_bank passes the 64KB-unit bank number directly to INT 0x10 AX=4F05 ('mov dx,ax ... int 0x10'), and vesa_plot_pixel's own comment admits it: 'Need to convert to granularity units: bank = DX * 64 / granularity / For most cards, granularity = 64KB' (2823-2824). The mode-set code actually reads the real granularity into vesa_gran at 18628-18632, but grep shows vesa_gran is never read anywhere. On real hardware where granularity is 4KB or 16KB (common for VESA 1.2 cards), every bank switch positions the window at the wrong address and the whole VESA mode renders garbage. Works only on emulators/cards reporting 64KB.
VERIFIER: Confirmed by direct code reading. vesa_set_bank (kernel\kernel.asm:2789-2804) passes the caller's bank number unconverted to INT 10h AX=4F05 (mov dx,ax / xor bx,bx / int 0x10); VBE function 05h expects DX in WINDOW-GRANULARITY units. Every caller supplies 64KB units: vesa_plot_pixel (2826), vesa_read_pixel (2854), vesa_fill_rect (2893, 2911-2913), the VESA XOR-pixel path (3316-3318), the window-scroll copy loop (19698, 19718, 19724), and the mode-clear code (2230-2258, explicitly labeled 64KB banks) - all take the bank from the high word of the 32-bit linear offset Y*640+X. The comment at 2823-2824 admits the missing conversion. The mode-set path correctly reads WinGranularity (mode-info offset 4, in KB) into vesa_gran at 18636-18642, but grep over the whole repo shows vesa_gran has exactly two references: the write at 18642 and the definition at 20004 (dw 64). It is never read; no validation or fallback exists for granularity != 64 (18615-18660 only checks 4F01/4F02 success and ModeAttributes bit 0). No other mechanism masks the bug: the vesa_cur_bank cache stores the same wrong unit, and there is no far-call window-function path. Concrete failure: on a card reporting 4KB granularity (e.g. Trident 8900/9000-class VBE 1.2 hardware) bank N maps window A to N*4KB instead of N*64KB, so only bank 0 (scanlines 0-101) renders correctly and everything below is written/read at the wrong VRAM address - full-screen garbage plus corrupted scrolls. QEMU/Bochs/VirtualBox report 64KB granularity, so emulators never show it. Severity medium is appropriate (real-hardware-only, but total breakage when triggered). One refinement vs. the original finding: the conversion must be applied inside vesa_set_bank AFTER the cache compare, keeping vesa_cur_bank in 64KB units, because vesa_fill_rect does 'mov ax,[vesa_cur_bank] / inc ax' (2911-2912) assuming 64KB units. The header comment at 2787 ('AX = bank number (in granularity units)') is also wrong and should say 64KB units.
FIX: Three edits, all 8086-safe NASM.

1) kernel\kernel.asm, in set_video_mode, insert AFTER line 18642 (mov [vesa_gran], ax) and BEFORE the 4F02 call at 18643, so unsupported granularity falls back to mode 12h (AX holds granularity in KB here; BX/CX are dead):

    ; Compute vesa_bank_shift = log2(64/gran); reject gran that is 0,
    ; >64, or not a power-of-two divisor of 64
    test ax, ax
    jz .svm_vesa_fail
    xor cx, cx                     ; shift count
    mov bx, 64
.svm_gran_loop:
    cmp bx, ax
    je .svm_gran_ok
    jb .svm_vesa_fail              ; gran not a divisor of 64 (or >64)
    shr bx, 1
    inc cx
    jmp .svm_gran_loop
.svm_gran_ok:
    mov [vesa_bank_shift], cl

2) Replace vesa_set_bank body (lines 2789-2804) - conversion goes after the cache check so vesa_cur_bank stays in 64KB units (required by vesa_fill_rect lines 2911-2912):

vesa_set_bank:
    cmp ax, [vesa_cur_bank]
    je .vsb_done
    mov [vesa_cur_bank], ax
    push ax
    push bx
    push cx
    push dx
    mov dx, ax                     ; DX = bank in 64KB units
    mov cl, [vesa_bank_shift]
    shl dx, cl                     ; convert to granularity units
    xor bx, bx                     ; BH=0 set window, BL=0 window A
    mov ax, 0x4F05
    int 0x10
    pop dx
    pop cx
    pop bx
    pop ax
.vsb_done:
    ret

(Also fix the header comment at line 2787 to 'AX = bank number (in 64KB units)'.)

3) Data section, after line 20004 (vesa_gran):

vesa_bank_shift: db 0             ; shl count converting 64KB banks to granularity units (log2(64/gran))

Note: 'shl dx, cl' is valid 8086. The fix assumes WinSize >= 64KB, which holds for the common deviant case (4KB/16KB granularity cards still expose a 64KB window); cards with WinSize < 64KB now at least hit the explicit mode-12h fallback path only if their granularity is also invalid - checking mode-info offset 6 (WinSize) >= 64 at 18636-18642 would make the fallback complete and costs two more instructions if desired.

## [medium/high] Dispatcher restores stale DX/SI on the z-clip early exit for scaled windows, and unconditionally clobbers API 50's CX return value for windowed apps
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: 213-216, 232, 245-251 (early exit skips saves at 261-263), 396-403 (restore); CONTENT_SCALE writers all =1: 15137/15202, 5172, 5857, 18715; API 50 contract: 7737-7738, 7817, docs/API_REFERENCE.md:682; bitmap: 20111 | area:INT 0x80 dispatcher translation/restore
DESC: Two related defects in the translate/restore logic. (1) _did_scale is set at line 232 (when WIN_OFF_CONTENT_SCALE==2) BEFORE the z-order clip check at 245-251, but _save_dx/_save_si are only written at 262-263 AFTER it. A background (non-topmost) scaled window's draw takes the early 'jmp int80_return_point' at 251, where lines 400-403 restore DX and SI from the stale _save_dx/_save_si of some PREVIOUS call — corrupting two of the app's registers, which is crash-class for the app (matches 'apps crash when several run'). Currently latent because every writer sets CONTENT_SCALE=1 (15127, 5172, 5857, 18705), but it is an armed trap for re-enabling 2x scaling. (2) For any window-context call, lines 396-399 unconditionally restore CX from _save_cx after the API returns; gfx_draw_string_wrap (API 50) documents 'Output: CX=final Y after last line' (7737-7738, set at 7812-7817), so windowed apps always get their input Y back instead of the result, breaking flowed-text layout.
VERIFIER: Both code-level defects are REAL as described; the cited line numbers are accurate. However, the impact framing needs two corrections.

DEFECT 1 (stale DX/SI on z-clip exit) — CONFIRMED, but strictly LATENT. Trace in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm: .do_translate saves only BX/CX (213-214), sets _did_translate=1 (215), clears _did_scale (216). If WIN_OFF_CONTENT_SCALE==2, line 232 sets _did_scale=1 BEFORE the z-order check. A non-topmost window then takes the early exit at 245-251 (pop ax/pop si, clc, jmp int80_return_point), skipping the _save_dx/_save_si writes at 262-263. At the return point, _did_translate=1 restores BX/CX (fine — saved this call), but _did_scale=1 at 400-403 loads DX and SI from _save_dx/_save_si left over from a PREVIOUS scaled topmost draw (data at 20074-20075). SI is typically a pointer, so this is crash-class — IF it can fire. It cannot fire today: I verified every writer of WIN_OFF_CONTENT_SCALE writes 1 — kernel.asm 15137 ('Always 1: no content scaling') flowing through .save_scale to 15202, literal 1 at 5172 and 5857 (dialogs), and 18715 which actively reverses legacy scale-2 windows to 1. No other write exists (also grepped raw '+ 24' offsets). So line 232 is dead code in current builds, _did_scale stays 0, and the early exit restores only BX/CX correctly. CORRECTION: this CANNOT explain any currently observed 'apps crash when several run' symptom — that part of the finding's narrative is refuted. It is an armed trap only for re-enabling 2x scaling.

DEFECT 2 (API 50 CX return clobbered) — CONFIRMED as a documented-contract violation. API 50 is in api_translate_bitmap (line 20111, byte 6 = 0x1C = APIs 50-52). gfx_draw_string_wrap documents 'Output: CX=final Y after last line' (7737-7738), sets it at 7812-7817, and preserves AX (push 7744 / pop 7823). The public doc repeats the contract (docs/API_REFERENCE.md:682 'CX Out: Final Y position after wrapped text'). For any windowed app (draw_context set via win_begin_draw, kernel.asm:1369), lines 396-399 unconditionally restore CX from _save_cx, so the app receives its INPUT Y, never the final Y. Two nuances the finding missed: (a) simply skipping the restore would NOT fix it — the API's CX is the screen-ABSOLUTE final Y, so the app would get a value off by win_y+titlebar_height; the fix must convert back to window-relative (the finding's suggested fix does say this). (b) The only in-tree caller, apps/settings.asm:664, never reads CX after the call, so nothing in-tree currently misbehaves — this breaks third-party apps that follow the published docs, not current code. Fullscreen apps (draw_context==0xFF) are unaffected since no translation occurs.

No hidden protection mechanism exists for either: the pushf at 385 only preserves CF, and nothing re-validates _save_dx/_save_si or exempts return-by-CX APIs. Severity: defect 1 is low-now/high-if-scaling-returns; defect 2 is medium for the public API contract.
FIX: Fix 1 — make the DX/SI snapshot valid on every exit path. In kernel/kernel.asm replace lines 213-216:

    mov [_save_bx], bx
    mov [_save_cx], cx
    mov [_save_dx], dx               ; hoisted: must be valid on z-clip early exit
    mov [_save_si], si               ; SI still caller's value here (before push si at 219)
    mov byte [_did_translate], 1
    mov byte [_did_scale], 0

and DELETE lines 261-263 ('; Save originals for restore after API call' / 'mov [_save_dx], dx' / 'mov [_save_si], si'). The _did_scale=0 clear at line 316 still correctly skips the restore for non-dimension APIs. (Smaller alternative: insert 'mov byte [_did_scale], 0' between the 'clc' at 250 and the 'jmp int80_return_point' at 251 — but the hoist also protects future early exits.)

Fix 2 — exempt API 50 from the CX restore and convert its absolute final Y back to window-relative. AH is intact at int80_return_point (gfx_draw_string_wrap preserves AX), CF is already saved by the pushf at 385, and DS is still kernel. Replace lines 396-399:

    cmp byte [_did_translate], 0
    je .no_coord_restore
    mov bx, [_save_bx]
    cmp ah, 50                       ; API 50 returns final Y in CX — don't clobber
    jne .restore_cx
    push si                          ; convert absolute Y -> window-relative
    push ax
    xor ah, ah
    mov al, [draw_context]
    mov si, ax
    shl si, 5
    add si, window_table
    sub cx, [si + WIN_OFF_Y]
    sub cx, [titlebar_height]
    pop ax
    pop si
    jmp .cx_done
.restore_cx:
    mov cx, [_save_cx]
.cx_done:

(all ops 8086-safe; the dispatcher itself already uses 386 movzx/bt so this is conservative). On the z-clip early exit with AH==50 this degrades gracefully: CX was translated but the API never ran, so the conversion hands back the app's original input Y — uncorrupted. If 2x content scaling is ever re-enabled, add 'shr cx, 1' when _did_scale==1 after the two subs (app works in logical coordinates), and also update docs/API_REFERENCE.md:682 if the semantics change instead.

## [medium/high] Window title text is not truncated to the window width (overwrites [X] button and bleeds past the frame)
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:17568-17588 (active title draw) and 17645-17662 (inactive title draw); close button drawn after at 17590-17617 / 17664-17688; clip disabled by callers at 16151-16152 and 4329-4330; no width validation in win_create_stub 15109+ | area:win_draw_stub title bar
DESC: The title is drawn with gfx_draw_string_stub/inverted at win_x+4 with no length limit: 'mov si,bx / add si,WIN_OFF_TITLE / mov bx,cx / add bx,4 ... call gfx_draw_string_inverted'. Titles are up to 11 chars (15255), i.e. 132 px at the current advance-12 font. For windows narrower than ~150 px the title runs over the [X] close button (drawn at win_x+width-10, 17586-17588) and past the right window border. Kernel redraw paths deliberately run with clip_enabled=0 (16141-16142, 4329-4330), so nothing stops the bleed onto the desktop or other windows. Same code in the inactive-titlebar branch (17635-17652).
VERIFIER: Core defect is real and code-verified, but two claims need correction. CONFIRMED: win_draw_stub draws the title from WIN_OFF_TITLE at win_x+4 with no length limit (active branch kernel/kernel.asm 17568-17588, inactive 17645-17662); titles are up to 11 chars (copy capped at 15265 'mov cx, 11'); the titlebar font is forced to font 1 or 2, both advance=12 (font_table 19983-19986), so a max title is 132 px of fully opaque pixels (draw_char/draw_char_inverted paint fg, bg AND the 4px inter-char gap, 3025-3095/4694-4745); plot_pixel_color clips only to screen edges (7681-7684); the per-character clip in gfx_draw_string_stub (7546-7561) and gfx_draw_string_inverted (4773-4785) is inert because kernel redraw paths force clip_enabled=0 (redraw_affected_windows 16151-16152, mouse_process_drag focus path 4329-4330); and win_create_stub (15109+) accepts any width with no minimum. CORRECTIONS: (1) the '[X] gets overwritten' part is wrong for the final frame — in BOTH branches the close button is drawn AFTER the title (active 17590-17617, inactive 17664-17688) with an opaque 12px cell, and the border is drawn after that (.draw_border 17699+), so the X always survives; the only persistent artifact is title pixels beyond win_x+width+1 bleeding onto the desktop/underlying windows. (2) The trigger threshold is width < 12*len+2 (<=133 px for an 11-char title), not ~150, because the X cell repaints through win_x+width+1 and the border repaints the edge column. IMPACT: latent — no in-tree app or kernel dialog currently triggers visible bleed (narrowest framed windows: clock W=110 with 5-char title=64px; Open/Save dialogs FDLG_W=152 with 9-char titles=112px; sysinfo's 11-char 'System Info' computes W>=140 which is just under the bleed threshold's safe side at 135<140). Any third-party app via INT 0x80 API 20 can trigger it, so the fix is worthwhile; severity is better rated low/latent than medium. Side observation: the [X] cell itself (start win_x+width-10, opaque advance 12) always paints 2px past the right window edge — a separate cosmetic bug the per-char clip cannot fix. Note the clip semantics for the fix: a char is drawn in full if its start x <= clip_x2 (cell extends advance-1=11px past start), so clip_x2 must be win_x+width-13 to guarantee the last drawn cell ends inside the window; the clip must wrap ONLY the title draw (not the [X] draw, which would be skipped since win_x+width-10 > clip_x2), and all clip vars must be saved/restored because app tasks and kernel callers (which only save clip_enabled) rely on their own clip rects.
FIX: Wrap each of the two title-draw blocks in win_draw_stub with a saved/restored clip rect. Register state at both insertion points: BX=window entry ptr, CX=win X, DX=win Y, SI=width (DS=0x1000).

1) ACTIVE branch — insert immediately BEFORE 'push bx' at line 17569 (comment '; Draw title text'):

    ; Clip title to titlebar so long titles can't bleed past the frame
    push word [clip_enabled]
    push word [clip_x1]
    push word [clip_y1]
    push word [clip_x2]
    push word [clip_y2]
    push ax
    mov [clip_x1], cx               ; left = win_x
    mov ax, cx
    add ax, si                      ; AX = win_x + width
    sub ax, 13                      ; last 12px char cell ends at win_x+width-2
    mov [clip_x2], ax
    mov [clip_y1], dx               ; top = win_y
    mov ax, dx
    add ax, [titlebar_height]
    mov [clip_y2], ax
    mov byte [clip_enabled], 1
    pop ax

and insert immediately AFTER the matching 'pop bx' at line 17588 (label .active_text_done block end):

    pop word [clip_y2]
    pop word [clip_x2]
    pop word [clip_y1]
    pop word [clip_x1]
    pop word [clip_enabled]

2) INACTIVE branch — insert the identical setup block immediately BEFORE 'push bx' at line 17646 (comment '; Draw title text - normal white on black...'), and the identical restore block immediately AFTER the matching 'pop bx' at line 17662.

Why clip_x2 = win_x+width-13: gfx_draw_string_stub/inverted draw a char in full whenever draw_x <= clip_x2 and the cell spans [draw_x, draw_x+11], so -13 guarantees the last painted pixel is win_x+width-2 (inside the border). Do NOT extend the clip around the close-button draw: the X starts at win_x+width-10 > clip_x2 and would be skipped entirely. Saving/restoring all five clip variables is required because app tasks own the clip state and the kernel callers at 16151/4329 only save clip_enabled. All instructions are 8086-safe.

## [medium/medium] CGA scroll/copy uses full-byte granularity, smearing up to 3 pixel columns outside the region when X or X+W is not 4-aligned
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: bpr calc 19504-19514; row-copy rep movsb 19572-19577; strip clear 19612-19614; clear-all 19648-19650; mode-12h copy 19776-19823 (mode-12h clear at 19838-19844 is already pixel-exact via mode12h_fill_rect) | area:gfx_scroll_area CGA path
DESC: Bytes per row are computed as ceil((X+W)/4) - X/4 (19495-19504) and rows are copied/cleared with whole-byte rep movsb/stosb (19563-19567, 19602-19604). When X%4 != 0 or (X+W)%4 != 0, the shared edge bytes contain up to 3 pixels belonging to neighboring content (window border, adjacent window): the copy overwrites those pixels with content from a different row and the exposed-strip clear zeroes them. Visible as flickering/eaten 1-3px columns at the edges of any scrolled area whose window x position isn't 4-aligned (windows are dragged to arbitrary x). The mode-12h path has the same issue at 8-pixel granularity (19766-19812).
VERIFIER: CONFIRMED, with refinements to mechanism, visibility, and the mode-12h claim.

Mechanism (verified in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm):
1. CGA path: bytes/row = ceil((X+W)/4) - X/4 (19504-19514). Rows are copied with whole-byte `rep movsb` (19572-19577) and the exposed strip / clear-all are zeroed with whole-byte `xor al,al / rep stosb` (19612-19614 and 19648-19650). E.g. X=5,W=8: bytes cover pixels 4..15 while the region is 5..12 — up to 3 outside pixels per edge are touched. No clipping, masking, or alignment exists anywhere in the function.
2. Unaligned X is the NORMAL case, not an edge case. API 80 is in api_translate_bitmap (20104-20115), so the INT 0x80 dispatcher (lines 196-251) rewrites BX = app_x + win_x + 1. Window drag sets win_x = mouse_x - drag_offset_x with no 4-pixel snapping (4129-4136), so the translated X is 4-aligned only when win_x % 4 == 3 (~25% of positions). An app cannot avoid this — translation is automatic and the alignment limitation is only in a source comment (19476), not in docs/API_REFERENCE.md:1111.
3. No protective mechanism exists. Z-order clipping (242-251) only suppresses background windows' draws; it does not protect pixels adjacent to the topmost window. clip_enabled is not consulted by gfx_scroll_area.

Visibility refinement (the finding slightly overstates the copy, understates the clear):
- The COPY smear is usually invisible: the adjacent outside pixels are the window's own vertical border (win_color=3, white — kernel.asm:20011, drawn by win_draw_stub) and black desktop (desktop_bg_color=0, 20379), both vertically uniform, so copying them from a different row is a no-op. It IS visible when an overlapping background window or desktop icon sits within 3px of the region edge.
- The CLEAR is the unambiguous visible bug: `rep stosb` zeroes the outside bits, blackening up to 3 pixel columns of the white window border / neighboring content for scroll_pixels rows. Worse, on the NEXT scroll the whole-byte copy propagates those blackened edge bytes upward, so repeated scrolling progressively erases the border column along the entire region height. Damage to the window's own chrome persists until the next win_draw.
- Mode 12h refinement: the finding is only HALF right there. The byte-granular copy (byte calc 19776-19785, write-mode-1 `rep movsb` 19795-19823) does smear up to 7 px per edge, but the exposed-strip clear is pixel-exact — it calls mode12h_fill_rect (19838-19844), which handles partial bytes with the VGA bit-mask register (2601-2774). So in mode 12h only the copy smears, and only where adjacent content is not vertically uniform. VGA 13h and VESA paths are 1 byte/pixel and exact.

Severity refinement: medium impact when triggered, but currently LATENT — no in-tree app calls API 80 (verified: only references are the dispatch table at 7429 and docs). It is a documented public API, so any external/future CGA app scrolling a text area will hit it at ~75% of window positions. The kernel already contains the exact masking pattern needed (gfx_clear_area_stub hybrid path, 9700-9793), confirming the suggested fix approach is idiomatic for this codebase.
FIX: Three patches to gfx_scroll_area's CGA path in kernel/kernel.asm (all 8086-safe; shifts use CL). Mirrors gfx_clear_area_stub's hybrid mask path (9726-9784). CGA 2bpp: leftmost pixel of a byte = bits 7:6, so first-byte inside mask = 0xFF>>(2*(X&3)), last-byte inside mask = 0xFF<<(8-2*((X+W)&3)).

(1) After `mov [cs:.sa_bpr], ax` (line 19514), compute the masks once:

    ; Inside-region masks for partial edge bytes
    push cx
    mov cx, [cs:.sa_x]
    and cl, 3
    shl cl, 1                       ; CL = 2*(X & 3)
    mov al, 0xFF
    shr al, cl                      ; 0xFF when aligned
    mov [cs:.sa_lmask], al
    mov cx, [cs:.sa_x]
    add cx, [cs:.sa_w]
    and cx, 3
    mov al, 0xFF
    jz .sa_rmask_done               ; aligned: mask = 0xFF
    shl cl, 1
    neg cl
    add cl, 8                       ; CL = 8 - 2*((X+W) & 3)
    shl al, cl
.sa_rmask_done:
    mov [cs:.sa_rmask], al
    cmp word [cs:.sa_bpr], 1        ; single-byte region: fold masks
    jne .sa_masks_done
    and [cs:.sa_lmask], al
    mov byte [cs:.sa_rmask], 0xFF
.sa_masks_done:
    pop cx

(2) Replace the copy block (19572-19578: `mov cx,[cs:.sa_bpr]` ... `pop ds`) with:

    mov cx, [cs:.sa_bpr]
    push ds
    push es
    pop ds                          ; DS = ES = video segment
    jcxz .sa_cp_row_done
    mov al, [cs:.sa_lmask]
    cmp al, 0xFF
    je .sa_cp_no_lead
    call .sa_merge_byte             ; merge first byte under mask
    dec cx
    jz .sa_cp_row_done
.sa_cp_no_lead:
    mov al, [cs:.sa_rmask]
    cmp al, 0xFF
    je .sa_cp_all
    dec cx                          ; reserve last byte
    rep movsb                       ; middle full bytes
    call .sa_merge_byte             ; merge last byte under mask
    jmp .sa_cp_row_done
.sa_cp_all:
    rep movsb
.sa_cp_row_done:
    pop ds

and add this helper next to .sa_done:

.sa_merge_byte:                     ; AL=inside mask, DS:SI=src, ES:DI=dst
    mov ah, [si]
    and ah, al                      ; inside bits from src row
    not al
    and al, [es:di]                 ; outside bits kept from dst row
    or al, ah
    mov [es:di], al
    inc si
    inc di
    ret

(3) Replace BOTH clear blocks (19612-19614 and 19648-19650: `mov cx,[cs:.sa_bpr]` / `xor al,al` / `rep stosb`) with a shared masked clear (callable with DI = row start):

    mov cx, [cs:.sa_bpr]
    jcxz .sa_cl_row_done
    mov al, [cs:.sa_lmask]
    cmp al, 0xFF
    je .sa_cl_no_lead
    not al
    and [es:di], al                 ; zero inside bits only
    inc di
    dec cx
    jz .sa_cl_row_done
.sa_cl_no_lead:
    mov al, [cs:.sa_rmask]
    cmp al, 0xFF
    je .sa_cl_all
    dec cx
    push ax
    xor al, al
    rep stosb                       ; middle full bytes
    pop ax
    not al
    and [es:di], al                 ; zero inside bits of last byte
    jmp .sa_cl_row_done
.sa_cl_all:
    xor al, al
    rep stosb
.sa_cl_row_done:

(use distinct local labels for the second instance, or factor into one near-call helper). Add storage next to .sa_bpr (line 19948):

.sa_lmask:   db 0
.sa_rmask:   db 0

Mode 12h copy (lower priority, smear only visible over non-uniform neighbors): keep write-mode-1 `rep movsb` for the aligned middle bytes only (left byte = ceil(X/8) when X&7 != 0, right byte exclusive = (X+W)>>3), and for each partial edge byte do a per-plane CPU merge per row: for plane p in 0..3 { GC reg 4 (Read Map) = p; AL=[es:si_edge]; AH=[es:di_edge]; merge with inside mask (0xFF>>(X&7) left, 0xFF<<(8-((X+W)&7)) right); SC reg 2 (Map Mask) = 1<<p; write merged byte in write mode 0 with bit mask 0xFF }. Alternatively, document the 8-pixel granularity in docs/API_REFERENCE.md API 80 and accept it.

## [medium/medium] draw_char renders text one pixel at a time through full mode-dispatch read-modify-write (hot-path performance)
- src:graphics-anomalies | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:3025-3095 (draw_char per-pixel loop), 3111-3164 (cga_pixel_calc MUL + plot_pixel_white CGA RMW), 3198-3227 (plot_pixel_black), 4694-4745 (draw_char_inverted, same pattern — the CGA titlebar path), 7680-7716 (plot_pixel_color CGA RMW), 7523 (gfx_draw_string_stub caller) | area:text rendering performance
DESC: draw_char calls plot_pixel_white/plot_pixel_black/plot_pixel_color per pixel; each call re-checks screen bounds, re-dispatches on video_mode, and in CGA does cga_pixel_calc (a MUL) plus a read-modify-write of the same video byte up to 4 times (3132-3164, 7680-7716). An 8x8 glyph costs ~96 calls (incl. gap fill) with ~6 memory touches each; the title bar, desktop labels, menus and every app string go through it (gfx_draw_string_stub 7523). In CGA a glyph row spans at most 3 bytes that could be composed in registers and written once — roughly an order of magnitude fewer VRAM accesses; text-heavy redraws (window drag repaints, file dialogs) are visibly slow because of this.
VERIFIER: Confirmed by direct code reading in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. Every factual element of the finding checks out:

1. Per-pixel rendering through full dispatch: draw_char (3025-3095) calls plot_pixel_white (3043), plot_pixel_black (3048), or plot_pixel_color (3053) for every pixel, plus gap-fill calls (3070, 3078). Each plot routine re-checks screen bounds (plot_pixel_white 3133-3136, plot_pixel_black 3199-3202, plot_pixel_color 7681-7684) and re-dispatches on video_mode with up to 3 compares (3137-3142, 3203-3208, 7685-7690).

2. CGA cost per pixel: the CGA branch pushes 5 registers, calls cga_pixel_calc (3111-3130) which executes a MUL (mul dx by 80 at 3114-3115) per pixel, then does a read-modify-write of [es:di] (3149-3157, 3215-3220, 7698-7709). Since CGA is 2bpp (4 pixels/byte), 4 horizontally adjacent pixels RMW the same VRAM byte 4 times — exactly as claimed.

3. The ~96-call arithmetic is exact: the default font (current_font=1, font_table 19983-19984) is 8x8 with advance=12, so each glyph row = 8 glyph pixels + 4 gap pixels = 12 plot calls × 8 rows = 96 calls. "~6 memory touches each" is conservative; counting the bounds/mode-byte reads, 10 stack ops, and VRAM RMW it is well above 6.

4. Hot path confirmed: gfx_draw_string_stub is at 7523 as cited, with 72 call sites of gfx_draw_string_* in the kernel. The titlebar text is drawn via gfx_draw_string_stub/gfx_draw_string_inverted in win_draw_stub (17581, 17585, 17607, 17611), and win_draw_stub is invoked from the drag/focus handler (4344, 4381), so window drags re-render title text per repaint. gfx_draw_string_inverted goes through draw_char_inverted (4694-4745), which has the identical per-pixel structure — in flat/CGA style this is the exact titlebar path. No alternative fast text path exists anywhere in the kernel (verified by grep; gfx_blit_rect's fast path is VGA-only and unrelated to glyphs).

Two minor refinements to the finding: (a) "a glyph row spans at most 3 bytes" — the 8 glyph pixels span 2-3 bytes, but the full 12-pixel advance row spans 3-4 bytes when unaligned; the order-of-magnitude conclusion is unchanged (≤8 VRAM accesses/row vs 24, plus eliminating 12 calls, 12 MULs, 12 mode dispatches, and ~120 stack ops per row). (b) "visibly slow" is an inference that cannot be verified statically — on emulators it may be imperceptible, on real 8088+CGA it is certainly significant — so severity "medium" as a perf finding is fair. This is purely a performance finding; there is no correctness bug (bounds are checked per pixel, the mouse cursor is locked/hidden by the stubs, and string-level clipping in gfx_draw_string_stub 7546-7561 skips whole chars).

One caveat for the suggested fix: a static 256-entry expansion table only works for fixed fg/bg; the codebase has variable draw_fg_color/draw_bg_color, and the 4x6 font (width 4) may carry garbage in the low nibble of each row byte, so a row blitter must mask glyph bits to the font width and combine fg/bg patterns per pixel. The fix below addresses both and needs no lookup table.
FIX: Add a CGA row compositor to draw_char that hoists the mode dispatch, bounds check, and address MUL out of the pixel loop and accumulates each row's 2bpp bits in registers, flushing one RMW per touched VRAM byte. Fall back to the existing per-pixel path for non-CGA modes or when the glyph is clipped by the screen edge (identical behavior there). No lookup table needed; only 8086 ops (plus pusha, which draw_char already uses).

1) Add locals next to draw_x/draw_y (~line 19966):

dc_fgpat:  db 0        ; fg color replicated in all 4 slots of a byte
dc_bgpat:  db 0        ; bg color replicated
dc_gmask:  db 0        ; keep only top draw_font_width bits of glyph row
dc_rows:   db 0
dc_npix:   db 0

2) In draw_char, immediately after "pusha" (line 3026), insert:

    cmp byte [video_mode], 0x01
    je .pp
    cmp byte [video_mode], 0x12
    je .pp
    cmp byte [video_mode], 0x13
    je .pp
    xor ax, ax
    mov al, [draw_font_advance]
    add ax, [draw_x]
    cmp ax, [screen_width]
    ja .pp                          ; right-clipped -> per-pixel path
    xor ax, ax
    mov al, [draw_font_height]
    add ax, [draw_y]
    cmp ax, [screen_height]
    ja .pp                          ; bottom-clipped -> per-pixel path
    jmp .cga_fast
.pp:
    ; ... existing code continues (xor bx,bx / mov bl,[draw_font_height] ...)

3) After the existing popa/ret (line 3094-3095), append:

.cga_fast:
    ; per-glyph setup: color patterns (c*0x55 replicates 2-bit c into 4 slots)
    mov al, [draw_fg_color]
    and al, 3
    mov dl, 0x55
    mul dl
    mov [dc_fgpat], al
    mov al, [draw_bg_color]
    and al, 3
    mov dl, 0x55
    mul dl
    mov [dc_bgpat], al
    mov cl, 8
    sub cl, [draw_font_width]
    mov al, 0xFF
    shl al, cl
    mov [dc_gmask], al              ; mask off unused low glyph bits (4x6 font)
    mov al, [draw_font_height]
    mov [dc_rows], al
    mov bx, [draw_y]                ; BX = current Y
.cf_row:
    lodsb                           ; glyph row byte
    and al, [dc_gmask]
    mov ch, al                      ; CH = glyph bits, MSB = leftmost
    ; row base: DI = (Y>>1)*80 + (X>>2) (+0x2000 odd rows) — ONE mul per row
    mov ax, bx
    shr ax, 1
    mov dx, 80
    mul dx
    mov di, ax
    mov ax, [draw_x]
    mov cl, 2
    shr ax, cl
    add di, ax
    test bl, 1
    jz .cf_even
    add di, 0x2000
.cf_even:
    mov ax, [draw_x]
    and al, 3
    mov cl, 3
    sub cl, al
    shl cl, 1                       ; CL = bit pos of first pixel slot
    xor dx, dx                      ; DH = accum bits, DL = accum mask
    mov al, [draw_font_advance]
    mov [dc_npix], al               ; pixels incl. gap (gap bits in CH are 0 = bg)
.cf_pix:
    mov al, [dc_bgpat]
    shl ch, 1                       ; CF = leftmost remaining glyph bit
    jnc .cf_have
    mov al, [dc_fgpat]
.cf_have:
    mov ah, 3
    shl ah, cl                      ; AH = 2-bit slot mask
    and al, ah                      ; AL = color bits in slot
    or dh, al
    or dl, ah
    sub cl, 2
    jnc .cf_next                    ; still inside this byte
    mov al, [es:di]                 ; flush: ONE RMW per VRAM byte
    not dl
    and al, dl
    or al, dh
    mov [es:di], al
    inc di
    xor dx, dx
    mov cl, 6
.cf_next:
    dec byte [dc_npix]
    jnz .cf_pix
    or dl, dl                       ; flush trailing partial byte
    jz .cf_rowdone
    mov al, [es:di]
    not dl
    and al, dl
    or al, dh
    mov [es:di], al
.cf_rowdone:
    inc bx
    dec byte [dc_rows]
    jnz .cf_row
    xor ax, ax
    mov al, [draw_font_advance]
    add [draw_x], ax
    popa
    ret

This cuts an 8x8 glyph from 96 plot calls / 96 MULs / ~192 VRAM accesses to 8 MULs and at most 32 VRAM RMWs (typically 24), with zero per-pixel calls or dispatches. Optionally apply the same fast path to draw_char_inverted (4694) by entering .cga_fast with dc_fgpat/dc_bgpat swapped (fg=0, bg=3), which covers the flat-style titlebar drag-repaint hot path.

## [medium/high] mem_free_stub performs no pointer validation — free(garbage) writes into the kernel image
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm 10038-10061 (guard at 10043-10044 insufficient; unchecked write at 10051-10055) | area:heap allocator
DESC: mem_free_stub only rejects NULL ('test ax, ax / jz .done', 10043-10044), then does 'sub ax, 4 / mov bx, ax / mov word [bx+2], 0' (10051-10055) with DS=0x1400. Any other AX value — a dangling pointer, a double-converted offset, or plain garbage — clears a word at an arbitrary offset in segment 0x1400, which (per the overlap finding) is the live kernel image for offsets < 0x7000. A pointer < 4 even wraps (e.g. AX=2 writes at 0xFFFE+2). There is also no allocated-flag check, so double-free goes undetected (currently benign because free only clears a flag, but it becomes corruption once splitting/coalescing exists).
VERIFIER: Confirmed by direct code reading. mem_free_stub (kernel\kernel.asm 10038-10061) validates nothing but NULL: 'test ax, ax / jz .done' (10043-10044), then with DS=0x1400 (10047-10048) executes 'sub ax,4 / mov bx,ax / mov word [bx+2],0' (10051-10055), writing a zero word at caller-controlled offset AX-2 in segment 0x1400. No other layer protects it: the INT 0x80 dispatcher (lines 121-372) passes AX through unvalidated for API 8 (slot 8 is not in api_drawing_bitmap, so it skips all translation/checks and jumps straight to the function), and mem_free_stub has no internal kernel callers (only the API table entry at line 7313). The kernel-image overlap is independently verified: stage2.asm loads the kernel at 0x1000:0000 with KERNEL_SECTORS=88 = 45056 bytes (matches build\kernel.bin), so the image spans linear 0x10000-0x1AFFF; heap segment 0x1400 = linear 0x14000, so heap offsets 0x0000-0x6FFF are live kernel code/data. The wrap claim is correct in effect: AX=2 gives BX=0xFFFE and [bx+2] truncates mod 64K to offset 0, clobbering the first heap header's size field (turns it into an end-of-heap marker, permanently breaking mem_alloc). The missing allocated-flag/double-free check is also confirmed (currently benign, as the finding says). One refinement the original auditor missed, which makes it WORSE: through the only existing call path (INT 0x80), the function number occupies AH, so AX at mem_free_stub entry is always 0x0800-0x08FF. Consequently (a) the NULL check can never trigger via the syscall, and (b) EVERY call to API 8 as documented in docs\API_REFERENCE.md (AH=8, AX=pointer - a self-contradictory convention) writes a zero word at heap offset 0x07FE-0x08FD = linear 0x147FE-0x148FD, i.e. kernel image file offset ~0x47FE-0x48FD, regardless of pointer validity. Mitigating factor for severity: grep shows no shipped app and no kernel code currently calls API 8, so the bug is latent until any app uses free(); 'medium' severity is fair. The AH/AX parameter conflict and the heap-base-inside-kernel-image overlap are separate root-cause findings; the fix below hardens mem_free_stub itself as the finding requests.
FIX: Replace mem_free_stub (kernel\kernel.asm lines 10038-10061) with the following 8086-safe version. It keeps free(NULL) as a silent no-op, rejects out-of-range/misaligned/non-allocated pointers with CF=1 (the INT 0x80 dispatcher already propagates CF to the caller's FLAGS), and returns CF=0 on success:

mem_free_stub:
    push ax
    push bx
    push ds

    test ax, ax
    jz .ok                          ; NULL pointer: no-op success (POSIX-style)

    cmp ax, 4
    jb .bad                         ; 1..3 would wrap below heap start
    cmp ax, 0xF000                  ; heap limit (matches mem_alloc's 0xF000)
    jae .bad
    test al, 3
    jnz .bad                        ; mem_alloc pointers are 4-byte aligned

    ; Set up heap segment
    mov bx, 0x1400
    mov ds, bx

    ; Get block header
    sub ax, 4                       ; Point to header
    mov bx, ax

    cmp word [bx+2], 0xFFFF         ; must be a currently-allocated block
    jne .bad                        ; (also catches double-free)
    cmp word [bx], 0                ; size sanity: nonzero...
    je .bad
    cmp word [bx], 0xF000           ; ...and within heap
    ja .bad

    ; Mark as free
    mov word [bx+2], 0              ; Clear allocated flag
.ok:
    clc
    jmp .out
.bad:
    stc
.out:
    pop ds
    pop bx
    pop ax
    ret

Also update the header comment ('Output: CF=0 success, CF=1 invalid pointer') and the API 8 row in docs\API_REFERENCE.md. Note: this hardening makes the latent INT 0x80 corruption path safely fail instead (AX=0x08xx will be rejected unless it coincidentally matches an allocated block), but the underlying design bugs remain and need separate fixes: (1) API 8's calling convention is self-contradictory (AH=function number overlaps AX=pointer - the pointer should move to BX or another register), and (2) the heap segment 0x1400 overlaps the loaded kernel image (linear 0x14000-0x1AFFF), so even valid alloc/free traffic corrupts kernel code - the heap base must move above the kernel (e.g. segment 0x1B00+) or the limit logic reworked.

## [medium/high] malloc failure returns CF=0 (success) with AX=0, violating the kernel's CF error convention
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm lines 10015-10017 (.fail: xor ax, ax / jmp .done - missing stc); secondary: line 10027 (success path, add clc after pop ds) | area:heap allocator
DESC: On exhaustion/invalid size the code runs '.fail: xor ax, ax / jmp .done' (10015-10017); XOR clears CF, and the INT 0x80 trampoline faithfully propagates CF into the caller's FLAGS (int80_return_point lines 377-419: 'jc .set_carry / and word [bp+6], 0xFFFE'). Every other API in this kernel signals errors with CF=1 (clip_copy, win_create, app_load, fs_*). An app that follows the documented convention ('int 0x80 / jc error') will treat a failed allocation as success and use pointer 0 — which is the heap's first block header, so the app then corrupts the allocator metadata (and with the current 0x1400 overlap, kernel code).
VERIFIER: Confirmed by direct code trace in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm. mem_alloc_stub (line 9963) is exposed as INT 0x80 API 7 via kernel_api_table (line 7312). Both failure paths reach .fail (line 10015) with CF=0: the zero-size path via 'test ax, ax / jz .fail' (9968-9969, TEST clears CF) and the exhaustion path via 'jz .fail_restore' (9998, preceded by 'test ax, ax') or the 'cmp si, 0xF000 / jb .search' fall-through (10010-10011, where SI >= 0xF000 means CF=0 from the CMP). 'xor ax, ax' (10016) then clears CF again, and the subsequent jmp/pop/ret (10017, 10029-10033) do not modify FLAGS. The INT 0x80 trampoline (int80_return_point, lines 377-419) captures the stub's flags with pushf (385), restores them with popf (405), and since CF=0 executes 'and word [bp+6], 0xFFFE' (413), clearing CF in the caller's IRET FLAGS. So an app doing 'int 0x80 / jc error' sees success with AX=0. This violates the kernel's own documented contract: docs/API_REFERENCE.md line 131 explicitly states for API 7 'CF Out: 0 = success, 1 = out of memory (AX=0)', and line 3 states the global CF convention. No compensating mechanism exists: grep found no internal kernel callers of mem_alloc_stub and no in-tree app currently calls API 7, so nothing relies on the buggy behavior and the fix is safe. The consequence claim is also accurate: offset 0 in segment 0x1400 is the first heap block header (initialized at lines 9987-9988), so writing through the bogus NULL pointer corrupts allocator metadata; additionally the kernel loads at 0x1000:0000 (line 2) and build/kernel.bin is 45,056 bytes (0xB000), so heap offset 0 (linear 0x14000) lies at kernel offset 0x4000, inside kernel code - making the corruption a kernel-code overwrite under the current layout. Minor refinement: only 'stc' on the .fail path is strictly required; the success path already returns CF=0 because 'add ax, 4' (10025) cannot carry (SI < 0xF000) and 'pop ds' preserves flags - but an explicit 'clc' is a worthwhile 1-byte robustness addition. One stc/clc pair fixes both failure modes since the zero-size path also lands on .fail.
FIX: In kernel.asm, mem_alloc_stub. At lines 10013-10017 change:

.fail_restore:
    pop ds
.fail:
    xor ax, ax                      ; Return NULL
    jmp .done

to:

.fail_restore:
    pop ds
.fail:
    xor ax, ax                      ; Return NULL
    stc                             ; CF=1: out of memory / invalid size
    jmp .done

And at lines 10019-10027 (.allocate path), after 'pop ds' add an explicit success flag:

.allocate:
    mov word [si+2], 0xFFFF
    mov ax, si
    add ax, 4
    pop ds
    clc                             ; CF=0: success

(The 'stc' MUST come after 'xor ax, ax', since XOR clears CF. Both instructions are 8086-safe, 1 byte each. The clc is defensive - add ax,4 already leaves CF=0 since SI < 0xF000 - but makes the contract explicit.)

## [medium/high] VESA mode query clobbers the system clipboard at 0x9000:0000
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:    mov di, 0x2000                 ; ES:DI = 0x9000:0x2000 (VESA scratch; clipboard owns 0x0000-0x0FFF, fdlg owns 0x1000-0x133F) | area:clipboard / scratch segment ownership
DESC: Clipboard data lives at SCRATCH_SEGMENT 0x9000 offsets 0x0000-0x0FFF (clip_copy: 'mov ax, SCRATCH_SEGMENT / mov es, ax / xor di, di / rep movsb', lines 4861-4865; layout comment at 5092 'clipboard uses 0x0000-0x0FFF'). But set_video_mode's VESA path uses the very same address as the INT 10h AX=4F01 buffer: 'mov ax, 0x9000 / mov es, ax / xor di, di ; ES:DI = 0x9000:0000 (scratch buffer) / mov ax, 0x4F01 ... int 0x10' (18609-18614), and reads granularity from 0x9000:0x0004 (18628-18630). The BIOS writes a 256-byte ModeInfoBlock there, destroying the first 256 bytes of clipboard content while clip_data_len (20419) still claims data is present — a subsequent paste (clip_paste 4880-4906) returns BIOS garbage. set_video_mode is app-reachable as API 95 (table line 7456), e.g. from the Settings app, so copy → change video mode → paste yields corrupted data. The file dialog correctly avoids this region by using offset 0x1000+ (FDLG_BUF_OFF, line 5092), so dialogs do NOT clobber the clipboard — only the VESA path does.
VERIFIER: Confirmed by direct code reading. Clipboard data is stored at SCRATCH_SEGMENT 0x9000 offset 0x0000, up to CLIP_MAX_SIZE=4096 bytes (kernel/kernel.asm lines 20329-20330; clip_copy 4851-4875 writes to 0x9000:0000; clip_paste 4880-4914 reads from 0x9000:0000; layout comment at 5092 reserves 0x0000-0x0FFF for the clipboard). set_video_mode's VESA path (.svm_try_vesa, lines 18605-18632) sets ES:DI=0x9000:0000 and issues INT 10h AX=4F01, which makes the VBE BIOS write a 256-byte ModeInfoBlock over the first 256 bytes of clipboard data; the code then reads the attributes word at 0x9000:0x0000 (line 18623) and granularity at 0x9000:0x0004 (line 18630), proving the BIOS write lands there. clip_data_len (line 20419) lives in the kernel data segment, is never cleared by set_video_mode, and there is no save/restore of the scratch bytes, no bounds check, and no other protecting mechanism (cli is irrelevant - this is sequential clobbering, not a race). Failure path: (1) app calls clip_copy (API 84); (2) anything calls set_video_mode (API 95, dispatch table line 7456) with AL=0x01 on a VESA-capable BIOS - reachable from apps/settings.asm (API_SET_VIDEO_MODE equ 95, cur_video_mode can be 0x01) and also from outlastv.asm/pacmanv.asm which save/restore the video mode on exit, so on a VESA desktop merely quitting a game triggers it; (3) clip_paste (API 85) then returns min(256, clip_data_len) bytes of ModeInfoBlock garbage. Note the clobber happens even if the VESA mode switch ultimately fails (4F01 succeeds, then the attributes-bit test or 4F02 fails and falls back to mode 12h) because the buffer write precedes those checks. The boot-time caller (line 1946) is harmless since the clipboard is empty at boot. The suggested relocation target 0x9000:0x2000 is verified free: the only other scratch-segment user is the file dialog at FDLG_BUF_OFF 0x1000 with max extent 0x1000+64*13=0x1340 (FDLG_MAX_FILES/FDLG_ENTRY_SIZE, lines 5088-5092), and linear 0x92000 is well below the EBDA. Severity medium is appropriate: silent user-data corruption, contained within the scratch segment.
FIX: In kernel/kernel.asm, .svm_try_vesa, move the VESA ModeInfoBlock buffer from 0x9000:0x0000 to 0x9000:0x2000 (3-line change, all 8086-safe):

1. Line 18611: change
       xor di, di                     ; ES:DI = 0x9000:0000 (scratch buffer)
   to
       mov di, 0x2000                 ; ES:DI = 0x9000:0x2000 (above clipboard 0x0000-0x0FFF and fdlg list 0x1000-0x133F)

2. Line 18623: change
       test byte [0x0000], 1          ; Mode attributes bit 0
   to
       test byte [0x2000], 1          ; Mode attributes bit 0

3. Line 18630: change
       mov ax, [0x0004]               ; Window granularity in KB
   to
       mov ax, [0x2004]               ; Window granularity in KB

Optionally add next to SCRATCH_SEGMENT (line 20329) a segment-map comment and constant:
   ; 0x9000 segment map: 0x0000-0x0FFF clipboard (CLIP_MAX_SIZE),
   ;                     0x1000-0x133F file-dialog list (FDLG_BUF_OFF),
   ;                     0x2000-0x20FF VESA ModeInfoBlock scratch
   VESA_INFO_OFF       equ 0x2000
and use VESA_INFO_OFF / VESA_INFO_OFF+4 in the three lines above instead of literals.

## [medium/medium] post_event is not reentrancy-safe; task-context posts race with IRQ posts and lose events
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm lines 10083-10112 (racy window: 10092-10106); task-context callers with IF=1: 4375, 4680, 5043, 16258, 16354, 16477, 19324 (IF=1 due to sti at line 151 in the INT 0x80 dispatcher); IRQ-context callers: 624, 1142, 1274 | area:kernel data structures / event queue
DESC: post_event reads the tail, stores 3 bytes, then advances tail with no interrupt protection: 'mov bx, [event_queue_tail] ... mov [event_queue + si], al ... inc bx / and bx, 0x1F ... mov [event_queue_tail], bx' (10092-10106). It is called both from hardware IRQ handlers (keyboard, mouse IRQ12) and from task/kernel context with IF=1 — e.g. win_destroy_stub posts WIN_REDRAW (16474-16477) and mouse_process_drag posts after focus changes (4371-4374). If an IRQ fires between a task-context tail read and the tail store, both writers use the same slot: the IRQ's event is overwritten by the task's event and only one tail increment survives, so one event is silently lost (dropped clicks/keys).
VERIFIER: CONFIRMED. post_event (kernel/kernel.asm lines 10083-10112) performs a non-atomic read-modify-write of event_queue_tail: read tail (10092), store 3-byte event (10099-10100), inc/wrap (10103-10104), full-check (10105), store tail (10106) — with no cli protection and no lock. Both halves of the claimed race exist: (1) IRQ-context producers: the INT 09h keyboard handler calls post_event at line 624 and the INT 74h IRQ12 mouse handler at line 1142 (plus the BIOS mouse callback at 1274); neither handler executes sti, so IRQ-vs-IRQ nesting is impossible. (2) Task-context producers run with IF=1 because the INT 0x80 API dispatcher executes sti at line 151 (with an explicit comment that API functions need hardware interrupts enabled for floppy DMA). Verified task-context call sites with IF=1 and no surrounding cli: mouse_process_drag posts WIN_REDRAW at 4375 (focus change) and 4680 (drag finish) — the cli at 4452/4517 are closed by sti at 4457/4520 before these posts; win_destroy_stub posts at 16477 with no cli anywhere in its body (global cli scan: only 47, 869, 1808, 3737, 3778, 4452, 4517, 14842, 15035); menu close posts at 5043; window ops at 16258/16354/19324. Concrete failure: a task-context post reads tail=T at 10092; a keyboard/mouse IRQ fires (IF=1), posts its event into slot T and advances tail to T+1; the task resumes, overwrites slot T with its own event and stores tail=T+1 — the IRQ's keypress/click is silently lost. If two IRQ posts land in the window (keyboard then mouse, tail goes to T+2), the task's tail store moves tail backward to T+1, losing two events. No other mechanism prevents this: the full-queue check at 10105 only handles wrap, and the consumer side (event_get_stub, single cooperative consumer, atomic 16-bit moves) is unaffected. The suggested pushf/cli...popf fix is correct, minimal, and 8086-safe; pushf/popf preserves caller IF state so IRQ callers stay IF=0 and task callers regain IF=1. Severity medium is fair: sporadic dropped keypresses/clicks, most likely during window drags/focus changes when IRQ12 traffic is heavy.
FIX: In post_event (kernel/kernel.asm line 10083), disable interrupts across the read-store-advance of event_queue_tail and restore the caller's IF on exit. Both the success path and the queue-full path already converge at .done, so only two insertions are needed:

post_event:
    push bx
    push si
    push ds
    pushf                           ; ADD: save caller's IF (IRQ callers are IF=0, task callers IF=1)
    cli                             ; ADD: make tail read/store-event/advance atomic vs IRQ posts

    mov bx, 0x1000
    mov ds, bx
    ; ... existing body unchanged (lines 10089-10106) ...

.done:
    popf                            ; ADD: restore caller's IF (replaces nothing; insert before pops)
    pop ds
    pop si
    pop bx
    ret

All instructions (pushf/cli/popf) are 8086-valid; popf restores the exact entry flags so IRQ-context callers remain IF=0 and task-context callers return to IF=1. No change needed in event_get_stub: it is the sole consumer (cooperative scheduler), and its 16-bit aligned reads of head/tail are atomic on 8086.

## [medium/high] desktop_set_icon_stub does not force NUL termination of the 12-byte icon name
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:15387-15389 (unterminated copy; insert fix after 15389), rendered unbounded at 15884-15888 via gfx_draw_string_stub loop 7538-7541 | area:string ops / kernel tables
DESC: The API copies the caller's 76 bytes verbatim: 'add di, DESKTOP_ICON_OFF_BITMAP / mov cx, 76 / rep movsb' (15387-15389) — 64 bitmap + 12 name bytes — and never writes a terminator. The desktop repaint then hands the name field straight to gfx_draw_string_stub (15885-15888), whose loop stops only at a NUL ('lodsb / test al, al / jz .done'). A BIN icon header (or any app calling API 37) supplying 12 non-NUL name bytes makes the kernel read past the field into the next desktop_icons entry (X/Y words, bitmap bytes) and render a long garbage label — corrupted-looking desktop text that fits the 'visual anomalies' report. All current in-tree apps happen to NUL-terminate, but the kernel must not rely on that for an app-supplied buffer.
VERIFIER: Every link in the claimed failure path verified against the code at C:\Users\arin\Documents\Github\unodos.

1) The copy is verbatim with no terminator. desktop_set_icon_stub (kernel/kernel.asm:15352-15430) does 'add di, DESKTOP_ICON_OFF_BITMAP / mov cx, 76 / rep movsb' (15387-15389), copying 64 bitmap + 12 name bytes from caller_ds:SI into desktop_icons + slot*80 + 4. Nothing afterwards touches the name field (it goes straight to the slot-count loop at 15394-15409). The field is *documented* as null-terminated (line 20364: 'DESKTOP_ICON_OFF_NAME equ 68 ; 12 bytes: display name (null-terminated)') but never enforced. The INT 0x80 dispatch table (line 7362) jumps straight to the stub with no sanitization.

2) The kernel itself renders the raw field. The desktop repaint (15884-15888) does 'add si, DESKTOP_ICON_OFF_NAME / mov word [caller_ds], 0x1000 / call gfx_draw_string_stub', and gfx_draw_string_stub's loop (7538-7541) is 'lodsb / test al, al / jz .done' — NUL is the only terminator. The clipping logic (7546-7561) never stops on a long string horizontally; X past the right edge just advances and loops (7574-7580), so a non-terminated name reads on into the next desktop_icons entry (X/Y words, bitmap) until it hits a zero byte. The read is bounded (zero-initialized slots / bitmap zeros / desktop_bg_color db 0 at 20368 guarantee a stop within kernel data), so the impact is a garbage label render plus out-of-font-table glyph reads ('sub al, 32' at 7563 underflows for bytes < 32) — visual corruption, no OOB write, no crash. Medium severity is fair.

3) The trigger is real, and the finding is actually slightly too generous to the in-tree apps: the launcher's own BIN-header path does NOT terminate the name. apps/launcher.asm read_bin_header copies the 12 name bytes at BIN header offset 0x04 verbatim (lines 668-682, 'mov cx, 12' loop, no NUL forced — contrast the FAT-filename fallback at 700-719 which does 'mov byte [cs:di], 0'), and register_icon (750-789) passes those 12 bytes verbatim to API 37. So any BIN file whose icon header uses all 12 name bytes (a 12-char name, or a malformed/third-party header) puts a non-terminated name in the kernel table today. Only the convention that current header authors zero-pad short names masks the bug — exactly the kind of caller contract the kernel must not rely on.

4) The suggested fix is correct. After 'rep movsb', DI = entry+4+76 = entry+80 and ES = 0x1000 (set at 15385-15386), so 'mov byte [es:di-1], 0' writes entry+79 = name[11]. [di-8-bit-disp] with ES override is valid 8086 addressing; the following code reloads DS so nothing is disturbed. It caps names at 11 visible chars, consistent with the field's documented null-terminated convention.

Minor side note: docs/API_REFERENCE.md:563-575 describes API 37 as taking DS:SI=name and ES:DI=bitmap separately, but the implementation takes one 76-byte block at SI — a documentation mismatch, separate from this bug.
FIX: In C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, insert one instruction after the 'rep movsb' at line 15389 in desktop_set_icon_stub:

    mov cx, 76                      ; 64 bitmap + 12 name
    rep movsb
    mov byte [es:di-1], 0           ; Force NUL in name[11]; DI is one past the 76-byte copy, ES=0x1000 (kernel)

(8086-safe: ES-override + [di+disp8] addressing; ES is already 0x1000 from line 15385-15386 and the code below reloads DS, so no other changes needed.)

Defense-in-depth (optional, same root cause at the producer): in C:\Users\arin\Documents\Github\unodos\apps\launcher.asm read_bin_header, after the 12-byte name copy loop ending at line 682, force termination of the last byte: 'mov byte [cs:di-1], 0'.

## [medium/medium] Killing a task leaks its open file handles — file_table exhaustion eventually blocks app loading
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: leak sites 4416-4431 (deferred close-kill), 14949-14999 (app_exit_stub), plus missed third path 14450-14468 (app_load_stub .kill_existing); handle-entry fill sites needing owner byte: 11119-11130 (fat12_open), 12414-12421 (fat16_open), 16870-16884 (fat12_create), 13209-13222 (fat16_create); table definition 20232-20242. Owner byte must be entry offset 24, NOT 12 (12-13 = FAT16 current cluster). | area:kernel data structures / file table
DESC: The close-button kill path frees the app slot, segment and windows ('mov byte [si + APP_OFF_STATE], APP_STATE_FREE ... call free_segment ... call destroy_task_windows', 4423-4431) and app_exit_stub does the same for self-exit (14949-14999), but neither closes file handles the task had open. file_table has only 16 entries ('FILE_MAX_HANDLES equ 16', 20240-20242) and entries carry no owner-task field, so nothing can reclaim them. Repeatedly killing an app that keeps a file open (e.g. an editor mid-save) permanently consumes handles until fs_open fails for everyone — at which point app_load_stub's fs_open_stub call fails and no new apps can launch (user sees load errors). Contributes to the 'crashes/failures when launching apps after running many apps' symptom.
VERIFIER: CONFIRMED as a real kernel bug, with three refinements. (1) Core claim verified by reading C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm: file_table entries (20232-20242) have no owner field; the ONLY code that ever frees an entry is fat12_close (11842-11860, used for both FAT12 and FAT16 via fs_close_stub) which requires an explicit handle. Neither kill path closes handles: the deferred close-button kill (4416-4431) frees only app slot + segment + windows, and app_exit_stub (14949-14999) does the same. No hidden reclamation exists: fat12_mount/fat16_mount (run on every app load via fs_mount_stub at 14477) never touch file_table, and alloc_file_handle (11157-11178) only scans the status byte. Once all 16 status bytes are 1, fat12_open/fat16_open return FS_ERR_NO_HANDLES, fs_open_stub sets CF, and app_load_stub (14485-14486 -> .file_not_found 14556) fails every subsequent app launch (misreported as FILE_NOT_FOUND) until reboot. (2) The finding MISSED a third kill path with the same leak: app_load_stub's .kill_existing loop (14450-14468) force-terminates a running app occupying the shell segment without closing its files. (3) Severity nuance: the concrete narrative ('killing an editor mid-save') cannot happen with the bundled apps. Scheduling is cooperative and the deferred kill executes only inside event_get_stub (mouse_process_drag called at kernel.asm:10126), so a victim task can only die while parked at an event_get/yield point; notepad (apps/notepad.asm 2309-2394), browser (apps/browser.asm 568-619) and mkboot hold handles only across straight-line int 0x80 sequences with no yields and close on all paths, including error paths. The leak actually triggers when an app (a) exits via RETF/API 36 with handles open, or (b) holds a handle across event_get/app_yield - both legal under the public app API contract (UnoDOS has third-party/SDK apps), so the kernel must reclaim. Medium severity for API robustness is fair; for the stock image alone it is low. (4) The suggested fix is WRONG in one detail: the 'reserved' bytes 12-31 are not all free. Offset 12-13 is the FAT16 current-cluster field (12421, 12729-12814) and fat12_read scratch (11690-11782); offsets 18-23 are used by the create/write paths (13218-13222, 16880-16884). The owner byte must go at offset 24 (verified unused). Also fat12_create (16646) and fat16_create (13007) allocate handles too (notepad saves via FS_CREATE) and must set the owner byte, not just the two opens.
FIX: 1) Document owner byte in the entry format comment (kernel.asm ~20238): ";   Byte 24: Owner task handle (0xFF = kernel)".

2) Set owner at all four entry-fill sites (current_task is 0xFF in kernel context per line 1785/20342, so kernel-opened handles are never reaped):

; fat12_open - insert after line 11130 (mov word [di + 10], 0):
    push ax
    mov al, [current_task]
    mov [di + 24], al               ; Owner task (0xFF = kernel)
    pop ax

; fat16_open - insert after line 12421 (mov [si + 12], ax):
    push ax
    mov al, [current_task]
    mov [si + 24], al               ; Owner task (0xFF = kernel)
    pop ax

; fat12_create - insert after line 16884 (mov [di + 22], cx):
    push ax
    mov al, [current_task]
    mov [di + 24], al               ; Owner task (0xFF = kernel)
    pop ax

; fat16_create - insert after line 13222 (mov [di + 22], cx):
    push ax
    mov al, [current_task]
    mov [di + 24], al               ; Owner task (0xFF = kernel)
    pop ax

3) Add helper near alloc_file_handle (e.g. after line 11178):

; close_task_files - Free all file handles owned by a dying task
; Input: AL = task handle
; Preserves: all registers
close_task_files:
    push cx
    push si
    mov si, file_table
    mov cx, FILE_MAX_HANDLES
.ctf_next:
    cmp byte [si], 1                ; Entry open?
    jne .ctf_skip
    cmp [si + 24], al               ; Owned by this task?
    jne .ctf_skip
    mov byte [si], 0                ; Mark free (same semantics as fat12_close)
.ctf_skip:
    add si, FILE_ENTRY_SIZE
    loop .ctf_next
    pop si
    pop cx
    ret

4) Call it from all three kill paths (AL already holds the victim task handle at each point; free_segment preserves AX):

; (a) deferred close-kill - insert after line 4423 (mov byte [si + APP_OFF_STATE], APP_STATE_FREE):
    call close_task_files           ; AL = victim task handle (from WIN_OFF_OWNER, line 4409)

; (b) app_exit_stub - insert after line 14962 (mov byte [si + APP_OFF_STATE], APP_STATE_FREE):
    call close_task_files           ; AL = current_task (loaded at 14954)
; (the .close_kill_self path needs nothing - it jmps to app_exit_stub)

; (c) app_load_stub .kill_existing - insert after line 14463 (mov al, cl):
    call close_task_files           ; AL = task handle of app squatting on shell segment

## [medium/medium] PS/2 mouse KBC fallback pokes ports 0x60/0x64 without verifying an 8042 exists — on XT it blindly reads the keyboard latch
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm:780 (.try_kbc entry - missing 8042-existence gate); stall: 839-847 retry loop x kbc_wait_read_long 968-995 (called from mouse_send_cmd:1012); blind port I/O spans 792-914 | area:BIOS/hardware assumptions (8088 target)
DESC: install_mouse correctly gates the BIOS path (INT 15h AX=C205/C207/C200 with 'jc .try_kbc' at lines 759/767/773 — an XT BIOS sets CF, good). But the .try_kbc fallback (line 780 onward) assumes an 8042 exists: it polls KBC_STATUS (port 0x64) in kbc_wait_write/read (lines 937-961) and reads/writes KBC_DATA (port 0x60). On a genuine PC/XT, port 0x64 is not decoded (reads float, typically 0xFF, so both OBF and IBF appear set): kbc_wait_read returns 'data ready' immediately and the code then executes 'in al, KBC_DATA' (e.g. lines 795, 809, 852) — on the XT that reads the 8255 keyboard latch, consuming/garbling any keystroke pending during boot, and the 3x mouse-reset retry loop (lines 839-847) repeatedly hammers it before finally failing. The timeouts (256-iteration loops, 1s tick timeout at 968-989) prevent a hang, but boot-time keystrokes can be eaten and init wastes ~seconds.
VERIFIER: CONFIRMED in substance, with two factual corrections that lower the severity to low-medium (perf-only).

What is correct: install_mouse (kernel\kernel.asm:745) is called unconditionally at kernel entry (line 33) before install_keyboard. The only gate on the KBC fallback is the three INT 15h C2xx calls with 'jc .try_kbc' (759/767/773); an XT BIOS returns CF=1/AH=86h for AH=C2h, so .try_kbc (line 780) executes on every XT-class boot. From there the code does blind 8042 I/O on ports 0x64/0x60 (lines 792-914) via kbc_wait_write (937-947), kbc_wait_read (949-961), kbc_wait_read_long (968-995) and mouse_send_cmd (1000-1016). No model-byte check, no probe, no other mechanism prevents this, and the project's own README/FEATURES state the minimum target is an 8088 IBM PC/XT, so the scenario is in-scope.

Corrections to the auditor's hardware model:
1. On a genuine IBM 5150/5160, port 0x64 does NOT float to 0xFF. The 74LS138 I/O decode selects the 8255 PPI for the entire 0x60-0x7F range, so 0x64 ALIASES PPI port A (0x60), the keyboard scancode latch. Idle reads return 0x00 (latch cleared by the BIOS IRQ1 handler), not 0xFF. Floating 0xFF only occurs on some clone XTs with fuller decode.
2. The keystroke-eating claim is REFUTED. On the XT, reading port 0x60 is a non-destructive read of the 8255 port A latch; a scancode is consumed only by pulsing port 0x61 bit 7, which this code never touches. The BIOS keyboard ISR still reads the same latched value, so no boot keystrokes are lost. Writes to 0x60/0x64 hit the output latch of a port configured as input (electrically inert), and the PPI control register (0x63) and port B (0x61) are never written - no hardware misconfiguration occurs.

The real, confirmed impact: with status reads returning 0x00 (genuine IBM decode, idle keyboard), OBF (bit 0) never appears set, so each kbc_wait_read_long runs its full ~1.1 s timeout (20 BIOS ticks, line 985; the DX=0xFFFF raw fallback is also ~1 s on a 4.77 MHz 8088). The 3x reset retry loop (839-847) calls mouse_send_cmd three times, each ending in one kbc_wait_read_long, giving ~3.3 seconds of dead time on EVERY boot of a real XT before .fail_reset -> .no_mouse -> clean stc exit. (Note the auditor's own scenario is internally inconsistent: if the bus floated 0xFF, OBF would appear set and the long waits would return immediately - failure in milliseconds, not seconds. The seconds-long stall happens precisely in the alias-to-latch 0x00 case he didn't consider.) Minor latent issue: the .no_mouse restore path (903-908) writes back saved_kbc_config captured from a bogus latch read, but the write is a no-op on the PPI.

Net: real perf/robustness bug (multi-second boot stall on the project's stated minimum hardware, plus undefined-port pokes), but no input loss and no correctness damage. Refined locations: gate insertion point kernel\kernel.asm:780 (.try_kbc); stall source 839-847 + 968-995 via 1012; all blind port I/O 792-914.
FIX: Insert a BIOS model-byte check at the top of .try_kbc, before any port I/O (kernel\kernel.asm:780). Machines without an 8042 are model bytes 0xFF (PC), 0xFE (XT/Portable), 0xFD (PCjr), 0xFB (XT-2). Note 0xFB < 0xFC (AT) numerically, so a single 'ja' compare is NOT sufficient - 0xFB needs an explicit test. Do not route the bail through .no_mouse (it performs KBC port writes and uses uninitialized saved_kbc_config); exit directly. 8086-safe, ES/AX already saved by install_mouse's prologue:

.try_kbc:
    ; Pre-AT machines have no 8042; on the IBM PC/XT ports 0x60-0x7F all
    ; decode to the 8255 PPI (0x64 aliases the keyboard latch), so the KBC
    ; probe below would stall ~3s in kbc_wait_read_long. Check the BIOS
    ; model byte at F000:FFFE first.
    mov ax, 0xF000
    mov es, ax
    mov al, [es:0xFFFE]             ; BIOS model byte
    cmp al, 0xFC                    ; 0xFC = AT (has 8042)
    ja .skip_kbc                    ; 0xFD/0xFE/0xFF = PCjr/XT/PC: no 8042
    cmp al, 0xFB                    ; 0xFB = XT model 2: no 8042
    je .skip_kbc
    ; ... fall through to existing code (save INT 0x74 vector, line 782)

and add before .no_mouse (e.g. after .fail_enable's mov at line 899):

.skip_kbc:
    mov byte [mouse_diag], 'X'      ; X = pre-AT machine, no 8042
    mov byte [mouse_enabled], 0
    stc
    jmp .done

(Optionally, for clone XTs whose model byte claims AT: additionally treat a status read of 0xFF from port 0x64 persisting across the line-790 flush loop as 'no controller' and jmp .skip_kbc - but the model-byte check alone fixes all genuine IBM-class hardware and is the minimal change.)

## [medium/medium] Kernel FAT16 INT 13h path calls AH=42h without the AH=41h presence check; floppy FS hardcodes 1.44MB geometry
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel.asm: AH=42h without probe at 11984-11985 (fat16_read_sector, starts 11954); CHS fallback with per-sector AH=08h re-query at 11989-12036; same pattern AH=43h in fat16_write_sector at 12092; hardcoded 18 SPT / 2 heads at 10698/10704, 11395/11400, 11443/11448; fat12_mount hardcodes BPB (no BPB read) at 10899-10925; mbr.asm correct probe at 70-77 | area:BIOS/hardware assumptions (8088 target)
DESC: fat16_read_sector issues extended read directly (lines 11906-11908 'mov ah, 0x42 / int 0x13') with only a CF fallback to CHS — unlike boot/mbr.asm which properly probes AH=41h/BX=0x55AA first (mbr.asm lines 70-77). Most old BIOSes return CF=1/AH=01 for unknown functions so the CHS fallback (AH=08h then AH=02h, lines 11916-11958) usually rescues it, but the CHS conversion itself uses 386 32-bit math (covered in the kernel CPU finding) and some pre-1995 BIOSes are known to leave registers/DAP-reserved bytes mangled by unknown AH values. Separately, the floppy filesystem hardcodes 1.44MB geometry: lines 10621 'mov bx, 18 ; sectors per track (1.44MB floppy)' and 10627 'mov bx, 2 ; num heads' — a stock XT's 360KB drives (9 SPT, 40 cyl) can never be read, so on a true 8088 the OS must boot from XT-IDE/HD or a retrofit 1.44MB controller.
VERIFIER: CONFIRMED (core finding), with corrected line numbers and one wrong premise in the suggested fix.

VERIFIED FACTS (C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm):
1. No AH=41h probe exists anywhere in kernel.asm — a grep for 0x41/0x55AA finds only the 0xAA55 boot-signature checks at lines 12200/12237. fat16_read_sector (11954) issues AH=42h directly at 11984-11985, and fat16_write_sector issues AH=43h directly at 12092. By contrast boot/mbr.asm lines 70-77 do the proper AH=41h/BX=0x55AA/cmp BX,0xAA55 probe, exactly as the finding cited.
2. The perf claim is real and actually UNDERSTATED: on a no-extensions BIOS, every sector read pays not one but TWO wasted INT 13h round-trips — the failed AH=42h (11985) plus a redundant AH=08h Get-Drive-Parameters call (11993-11995) executed inside the fallback on EVERY sector. The geometry is stored into ide_sectors/ide_heads (12003/12005) but never reused as a cache; it is re-queried per sector. fat16_read_sector has 12 call sites and is called once per sector in file-read loops.
3. A subtle correctness hazard the finding only gestured at: CF=0 is guaranteed at the AH=42h INT (the last flag-writing instruction before it is `xor si, si` at 11981, which clears CF). On old BIOSes whose INT 13h dispatcher handles unknown AH by plain IRET (restoring caller flags) instead of fixing CF in the stacked flags, the kernel sees CF=0, takes `.success` (11987), and returns a never-read buffer as good data. The classic defense is `stc` immediately before the INT.
4. Floppy geometry IS hardcoded 18 SPT / 2 heads — at lines 10698/10704 (fs_readdir_stub FAT12 path), 11395/11400 (floppy_read_sector), 11443/11448 (floppy_write_sector). Cited lines 10621/10627 were stale; content matches.

CORRECTIONS TO THE FINDING:
- The suggested floppy fix ("read SPT/heads from the BPB already loaded into bpb_buffer") is based on a false premise: fat12_mount (10877-10954) NEVER reads a boot sector or BPB. It hardcodes all BPB values (512 bps, 1 spc, 224 root entries, 9 spf) and a fixed filesystem layout at absolute sector 94 ("HARD-CODE BPB values instead of reading sector 78", line 10899). bpb_buffer is just a scratch sector buffer. The floppy is a custom-layout 1.44MB UnoDOS boot diskette (kernel image at sectors 0-93, FS at 94+), so 360KB/720KB support is a build-level design change, not a geometry read — README explicitly states the OS "fits on a single 1.44MB floppy disk". This half is true-but-by-design; severity informational, not medium.
- The 8088 framing is moot in practice: the kernel pervasively uses 386 instructions (push eax, movzx, div ebx, imul r32 — e.g. 11955, 12015-12016, 12276), and docs/bootloader-architecture.md:221 admits "CPU requirement ... 386+ (for now)". The realistic affected hardware is 386/486-era machines with pre-EDD (~pre-1995) BIOSes — common — where the per-sector double round-trip and the CF hazard are concrete.

NET: the FAT16 half (missing cached AH=41h probe, per-sector wasted INT 13h calls including the uncached AH=08h, CF=0-in hazard) is confirmed at medium severity. The floppy half is factually accurate but misclassified as a bug, and its suggested fix cannot work as written.
FIX: In fat16_mount (kernel.asm:12167), after the drive reset (after line 12181 `jc .read_error`) and BEFORE the first fat16_read_sector call at 12233, probe once and cache:

    ; Probe INT 13h extensions once at mount (AH=41h), cache result
    mov byte [fat16_has_ext], 0
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [fat16_drive]
    int 0x13
    jc .no_ext
    cmp bx, 0xAA55
    jne .no_ext
    test cl, 1                      ; Bit 0 = fixed-disk access subset (42h-44h)
    jz .no_ext
    mov byte [fat16_has_ext], 1
.no_ext:

(BX/CX/DX are scratch at that point; DL is reloaded from [fat16_drive] at 12195.)

Add the variable next to fat16_mounted (~line 20334):
    fat16_has_ext: db 0

In fat16_read_sector, immediately after `mov [.saved_lba], eax` (line 11963) — must be after saved_lba is set because the CHS path reads it:
    cmp byte [fat16_has_ext], 0
    je .chs_fallback                ; No extensions: skip 42h entirely
and label the existing fallback block at line 11989 (the `push es` before AH=08h) as `.chs_fallback:`.

Defensive hardening at line 11984-11985 (and the AH=43h site at 12092-12094): set CF before the INT so BIOSes that IRET on unknown AH fail safe:
    mov ah, 0x42
    stc                             ; Old BIOSes may IRET without touching CF
    int 0x13

Apply the same fat16_has_ext gate + stc to fat16_write_sector (12062+).

Optional perf follow-up: hoist the AH=08h geometry query into fat16_mount (populate ide_sectors/ide_heads once, guarded by a fat16_geom_valid flag) so the no-extensions path does exactly one INT 13h per sector instead of two.

All added instructions (mov/int/cmp/test/jc/stc) are 8086-safe; the surrounding 32-bit code is the separate kernel-CPU finding.

For the floppy half: no code fix as suggested is possible (there is no BPB read at mount — fat12_mount hardcodes the layout at sector 94). Either document 1.44MB-only as a design constraint, or — if 720KB/360KB support is actually wanted — query geometry once via INT 13h AH=08h DL=0 in fat12_mount into two variables (floppy_spt/floppy_heads) and replace the three hardcoded `mov bx, 18`/`mov bx, 2` pairs (10698/10704, 11395/11400, 11443/11448) with loads from those variables; note this still requires redesigning the 1.44MB-sized disk image layout (FS at absolute sector 94) at build time, so it is a design change, not a bug fix.

## [medium/high] draw_char plots every glyph pixel via a far-flung per-pixel call that recomputes the CGA address with a 16-bit MUL
- src:performance | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel.asm: draw_char 3025-3095 (inner loop 3040-3059, gap loop 3061-3083); cga_pixel_calc MUL 3114-3115; plot_pixel_white CGA path 3132-3164; ALSO draw_char_inverted 4694-4745 (same per-pixel pattern, omitted by the finding) and the per-pixel MUL in the VGA13h path at 3169-3170; default font advance=12/width=8 at 20050-20051 confirms 96 calls/glyph | area:graphics / text rendering
DESC: draw_char's inner loop (lines 3040-3059) does 'test ah, 0x80 / call plot_pixel_white' or plot_pixel_black for every one of the 64-96 pixels of a glyph. Each plot_pixel_* call performs 2 bounds compares with segment overrides, 3 video-mode compares, 5 pushes, then calls cga_pixel_calc which does 'mov dx, 80 / mul dx' (line 3114-3115) — a 16-bit MUL costing ~120-133 cycles on an 8088 — plus a read-modify-write and 5 pops, per pixel. The gap-fill loop (3061-3083, advance>width) adds 4 more plot calls per row per char. A single 8x8 character costs ~96 full pixel-call round trips (~15-25k cycles); a 20-char string is on the order of 0.1 s at 4.77 MHz. This is the dominant cost of all text on screen (labels, titlebars, notepad text) and a direct contributor to the sluggish/flickery visuals reported.
VERIFIER: CONFIRMED as a real performance finding; every structural claim checks out against C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm, with two refinements (scope is actually WIDER than stated; absolute timing on 8088 is moot because the kernel cannot run on an 8088).

Verified claims:
1. draw_char (3025-3095): inner .col_loop (3040-3059) does `test ah,0x80` then a far call per pixel: plot_pixel_white (bit=1), plot_pixel_black (bit=0, bg=0 fast path), or plot_pixel_color with an extra push/pop dx wrapper (3050-3054, bg!=0). Confirmed verbatim.
2. Gap-fill loop (3061-3083): confirmed, and the "4 extra plot calls per row" claim is exactly right for the default font — font_table entry 1 (line 20050-20051) is `db 8, 8, 12, 8` (height=8, width=8, advance=12), so gap = 12-8 = 4. A default glyph = 8 rows x 12 = 96 plot-pixel call round trips, matching the finding's "~96".
3. plot_pixel_white (3132-3164): 2 bounds cmps against [cs:screen_width]/[cs:screen_height], 3 video-mode cmps, 5 pushes, call cga_pixel_calc, read-modify-write of [es:di], 5 pops — confirmed. plot_pixel_black (3198-3227) and plot_pixel_color (7680-7716) are identical in structure.
4. cga_pixel_calc (3111-3130): `mov dx,80 / mul dx` at 3114-3115 — confirmed; mul r16 is 118-133 cycles on 8088/8086. The VGA 13h path has the same per-pixel MUL (`mul word [cs:screen_pitch]`, line 3170), so the problem is not CGA-only.
5. This is the only text path. gfx_draw_string_stub (7523-7592) calls draw_char per character (7570); gfx_draw_string_wrap (7795) and the widget/button label code (8049-8052, 8116, 8198) funnel into the same routines. No fast blit path exists; the string-loop clipping (7546-7561) only skips fully-clipped characters. Boot default mode is CGA 0x04 (lines 50 and 20063), so the MUL-per-pixel CGA path is the default text path.
6. The finding UNDERSTATES scope: draw_char_inverted (4694-4745) has the identical per-pixel-call structure (calls plot_pixel_black/white per pixel, 4709-4732) and renders all titlebar/menu/inverted text via gfx_draw_string_inverted (4752).

Refinement/caveat on magnitude: the README targets "Intel 8088 or later", but the kernel uses pusha/popa (186+) and movzx extensively (386+, e.g. lines 172, 4942, 5113), so the binary as assembled cannot execute on an 8088 — the "0.1 s per 20-char string at 4.77 MHz" figure describes hardware the current build can't reach. On the de-facto minimum CPU (386) MUL is ~12-25 cycles and on QEMU the absolute cost is small; visible flicker on emulators is more attributable to the mouse_cursor_hide/XOR-show wrapped around every string (7524/7591) plus per-pixel RMW. Nevertheless, for the project's stated vintage-hardware goal the analysis and the 10-25x improvement estimate are sound, and the per-pixel call + bounds + mode dispatch + push/pop + MUL overhead is genuinely the dominant cost of text in all 4 video modes on any CPU. Severity medium is fair.
FIX: Stage 1 (drop-in, low-risk, removes the 118-133 cycle MUL from every CGA pixel; contract-compatible — all 6 callers already save AX/BX/DX):

Replace cga_pixel_calc (kernel.asm 3111-3130) with:

cga_pixel_calc:
    mov di, bx
    and di, 0xFFFE                   ; (Y/2)*2 = byte index into word LUT (Y & ~1)
    mov di, [cs:cga_row_table+di]    ; DI = (Y/2)*80, no MUL
    mov ax, cx
    shr ax, 1
    shr ax, 1                        ; AX = X/4
    add di, ax
    test bl, 1
    jz .even
    add di, 0x2000                   ; odd scanline: +8K interlace bank
.even:
    mov ax, cx
    and ax, 3
    mov cx, 3
    sub cl, al
    shl cl, 1                        ; CL = (3-(X&3))*2
    ret

and add a 200-byte table in the data area (near line 20045):

cga_row_table:
%assign y 0
%rep 100
    dw y*80
%assign y y+1
%endrep

Stage 2 (the 10-25x claimed by the finding): give draw_char a CGA row-blit fast path, keeping the existing per-pixel loop as the fallback for other modes and edge clipping. At draw_char entry (after pusha, line 3026): if [cs:video_mode]!=0x04, or draw_x+draw_font_advance>screen_width, or draw_y+draw_font_height>screen_height, jmp to the existing .row_loop (semantics preserved — per-pixel bounds checks currently provide edge clipping). Otherwise, per glyph row: compute DI once via cga_row_table (as above), expand the 8 font bits to a 16-bit 2bpp pattern with a 256-entry word LUT built for fg=3 (font2bpp_table: %rep 256 expanding each bit b to bits 2b+1..2b; for fg/bg colors AND the pattern with fg-replicated mask and OR bg into the complement), then shift the 16-bit pattern+0xFFFF mask right by (draw_x&3)*2 into a 24-bit window (3 registers, max 6 single-bit shifts on 8086) and apply 2-3 read-AND-OR-write byte stores at ES:DI..DI+2. One mode dispatch and one address computation per row instead of 12 full call round-trips. Apply the same restructure to draw_char_inverted (4694) or fold both into one routine parameterized by fg/bg.

## [medium/high] Mouse cursor is fully erased and redrawn around every drawing syscall, and the sprite itself uses cga_pixel_calc (MUL) per pixel
- src:performance | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:Real hotspots: kernel/kernel.asm:3348-3385 (CGA per-pixel loop, cga_pixel_calc call at 3367) reached from IRQ12 at :1077/:1136. Dispatcher :192/:391 are no-ops for windowed apps due to cursor_locked (checks at :3738-3739/:3779-3780, bracket at :1361-1415, per-task restore at :14890-14906); only fullscreen non-windowed apps pay full hide/show per drawing syscall. | area:mouse cursor / dispatcher
DESC: For every API in api_drawing_bitmap the INT 0x80 dispatcher unconditionally calls mouse_cursor_hide (line 192) and mouse_cursor_show on return (line 391), with no test of whether the draw rect intersects the cursor. Each hide/show pair runs cursor_xor_sprite twice; the CGA path (3348-3385) calls cga_pixel_calc — with its 16-bit MUL — separately for every non-transparent pixel of the 8x14 sprite (~80-100 MULs per pass, so ~200 per drawing syscall as pure overhead). The same per-pixel sprite cost is paid twice per IRQ12 mouse packet (hide at 1077, show at 1136) — at PS/2 sample rates (40-100 pkt/s) that alone can consume a large fraction of a 4.77 MHz CPU while the mouse moves, which matches the reported input sluggishness and cursor-related visual artifacts.
VERIFIER: PARTIALLY CONFIRMED — the IRQ12 per-pixel-MUL sprite cost is real, but the headline per-syscall claim is wrong for the dominant case, and the severity premise (4.77 MHz 8088) is invalid.

CONFIRMED parts:
1. IRQ12 path (kernel/kernel.asm:1077 mouse_cursor_hide, :1136 mouse_cursor_show): every completed 3-byte PS/2 packet performs a full sprite erase + redraw. In CGA modes, cursor_xor_sprite's row/col loop (:3348-3385) calls cga_pixel_calc (:3111, 16-bit MUL at :3115) plus a call + 8 push/pops per non-transparent pixel. The sprite (:20223-20237) has 54 non-transparent pixels (not 80-100 as claimed), so ~108 cga_pixel_calc invocations per packet — a genuine, easily-removed inefficiency in interrupt context.
2. Fullscreen non-windowed apps (tetris.asm "Fullscreen (non-windowed) game", pacman, outlast) issue drawing syscalls with draw_context=0xFF and cursor_locked=0, so each drawing INT 0x80 does pay a real hide+show pair (2 sprite passes, ~108 MULs) while the cursor is visible.

REFUTED parts:
1. "Mouse cursor is fully erased and redrawn around every drawing syscall" is FALSE for windowed apps — the finding missed the Build 397 cursor_locked bracket. win_begin_draw (API 31, :1361-1368) hides the cursor once and holds cursor_locked>0 until win_end_draw (:1406-1415); mouse_cursor_hide/show early-out on cursor_locked at :3738-3739 and :3779-3780 (~6 instructions, no sprite work). The comment at :1364-1366 states this explicitly. All windowed apps in apps/ call API 31, and the scheduler re-establishes the bracket per task (:14890-14906: resets cursor_locked then re-calls win_begin_draw for tasks with a saved draw context). So for windowed apps the dispatcher hide/show at :192/:391 are near-free no-ops.
2. The numbers: 54 visible pixels per pass, ~108 MULs per hide+show pair — roughly half the claimed "~200 per drawing syscall".
3. The "large fraction of a 4.77 MHz CPU" framing: the kernel cannot run on an 8088/8086 at all — the INT 0x80 dispatcher uses movzx (:172) and bt (:173), and cursor_xor_sprite uses pusha (:3338), all 386+/186+ instructions (no NASM cpu directive anywhere). On the realistic slowest target (386SX-16 PS/2 L40, README:210), 100 pkt/s costs single-digit % CPU — worth fixing, not saturating. (Side note: the README's "Intel 8088 or later" claim is contradicted by the code.)
4. Suggested fix (1) (dispatcher bbox test on BX/CX/DX/SI) is UNSAFE as stated: for several APIs in api_drawing_bitmap, SI/DX are pointers or non-dimension values (the dispatcher's own scaling code documents this, :287-290 "SI is pointer or non-dimension"), so comparing them against a cursor bbox is meaningless and would wrongly skip hide for draws that do overlap the cursor. It is also redundant for windowed apps (already no-ops via cursor_locked). Do not apply it.

NET: downgrade to low-medium. The one worthwhile fix is rewriting cursor_xor_sprite's CGA path to XOR the raw 2bpp row pattern as 2-3 bytes per row (per-pixel XOR of color<<shift is bit-identical to byte-XOR of the sprite row, since transparent pixels are 00 and XOR 0 is a no-op). This cuts MULs from 54 to 14 per pass and removes ~54 call/push-pop sequences, speeding both IRQ12 cursor movement and fullscreen-app syscalls in CGA modes. VGA 0x13 / mode 0x12 / VESA paths are unaffected (they use save-restore or mode12h_xor_pixel).
FIX: Replace only the CGA row/col loop of cursor_xor_sprite (kernel/kernel.asm:3348-3385) with a per-row byte-XOR fast path; keep the original per-pixel loop as a fallback for rows near the right screen edge (X > screen_width-8, where per-pixel clipping is needed). Equivalence: per-pixel `xor [es:di], color<<shift` over a row is bit-identical to XORing the raw 2bpp row pattern bytes (transparent pixels are 00).

.row_loop:
    push cx
    cmp bx, [screen_height]
    jae .skip_row
    mov ax, [screen_width]
    sub ax, 8
    cmp cx, ax
    ja .slow_row                ; near right edge: per-pixel clipped fallback
    ; --- fast path: row base once, then 2-3 byte XORs ---
    mov ax, bx
    shr ax, 1
    mov dx, 80
    mul dx                      ; AX = (Y/2)*80   (1 MUL per row, was 1 per pixel)
    mov di, ax
    mov ax, cx
    shr ax, 1
    shr ax, 1
    add di, ax                  ; DI = row base + X/4
    test bl, 1
    jz .fr_even
    add di, 0x2000              ; odd-scanline interlace bank
.fr_even:
    and cx, 3                   ; CX = X & 3 (original X is on stack)
    mov ah, [si]                ; pixels 0-3, leftmost in bits 7-6 (CGA layout)
    mov al, [si+1]              ; pixels 4-7
    xor dh, dh                  ; DH = spill byte (third byte when misaligned)
    jcxz .fr_xor
.fr_shift:                      ; shift AH:AL:DH right 2 bits, X&3 times
    shr ax, 1
    rcr dh, 1
    shr ax, 1
    rcr dh, 1
    loop .fr_shift
.fr_xor:
    xor [es:di], ah
    xor [es:di+1], al
    test dh, dh
    jz .skip_row
    xor [es:di+2], dh
    jmp .skip_row
.slow_row:
    mov ah, [si]                ; ---- original per-pixel path, unchanged ----
    mov al, [si+1]
    mov di, 8
.col_loop:
    mov dl, ah
    shr dl, 6
    test dl, dl
    jz .skip_pixel
    cmp cx, [screen_width]
    jae .skip_pixel
    mov [cs:cursor_color], dl
    push ax
    push bx
    push cx
    push di
    call cga_pixel_calc
    mov al, [cs:cursor_color]
    shl al, cl
    xor [es:di], al
    pop di
    pop cx
    pop bx
    pop ax
.skip_pixel:
    shl ax, 2
    inc cx
    dec di
    jnz .col_loop
.skip_row:
    pop cx
    add si, 2
    inc bx
    dec bp
    jnz .row_loop

Safety notes: fast path is taken only when X <= screen_width-8, so all 8 pixels and the third byte (written only when X&3 != 0, i.e. X <= 311, byte offset <= 79 within the row) stay inside the 80-byte row — no clipping needed. X is already clamped >= 0 by the IRQ12 handler (:1094-1096). All instructions are 8086-safe (the surrounding kernel already requires 386 due to movzx/bt/pusha, so this is not a constraint in practice). Do NOT apply the suggested dispatcher bbox test — SI/DX are pointers/non-dimensions for several drawing APIs, and windowed apps already skip cursor work via cursor_locked.

## [medium/high] Floppy reads are issued one sector per INT 13h call — sequential cluster reads lose a disk revolution per sector
- src:performance | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:11405 (mov ax,0x0201 in floppy_read_sector 11386-11425); fat12_read .cluster_loop 11772-11850; rep movsb bounce 11827; sectors_per_cluster=1 at 20300 (and 10909); FAT cache 11298-11302/20326-20327; app-load call chain 14586 -> 10576 -> 11800 | area:filesystem / floppy I/O
DESC: floppy_read_sector always issues 'mov ax, 0x0201' (AH=02 read, AL=01 one sector, line 11328) and fat12_read's cluster loop (11695-11773) calls it once per 512-byte cluster, bouncing each sector through bpb_buffer with a byte-wise 'rep movsb' (11750). With sectors_per_cluster=1 (20223), loading a 20 KB app = 40 separate INT 13h transactions; by the time one returns and the next is issued the target sector has typically passed under the head, costing up to a full revolution (~200 ms at 300 RPM) per sector. App launch from floppy can take 5-8 s when a track-at-a-time read would take well under 1 s. FAT chain lookups are cached (fat_cache_sector, 11221-11233 — that part is fine), so the BIOS call pattern is the bottleneck.
VERIFIER: Confirmed, with stale line numbers (auditor's refs are offset ~75-80 lines from the current file). Verified chain in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:

1. floppy_read_sector (11386-11425) hardcodes `mov ax, 0x0201` at line 11405 (not 11328) — always exactly 1 sector per INT 13h call. Geometry is hardcoded 18 sectors/track, 2 heads (11395, 11400), so track-sized batching is feasible.

2. fat12_read is 11720-11913; the per-cluster loop `.cluster_loop` is at 11772-11850 (not 11695-11773). Each iteration: cluster→LBA (11782-11786), single-sector read into bpb_buffer at segment 0x1000 (11788-11800), byte-wise `rep movsb` bounce to caller's ES:DI at line 11827 (not 11750), then get_next_cluster (11846).

3. sectors_per_cluster = 1: `db 1` at line 20300 and set at 10909 (not 20223). Note the loop is also implicitly hard-wired to spc=1 — it reads only the first sector of each cluster and caps the copy at 512 bytes (11813-11815).

4. FAT-cache claim correct: get_next_cluster checks fat_cache_sector at 11298-11302 (buffer at 20326-20327), so chain walking adds only ~1 disk read per 341 clusters. The INT 13h call pattern is indeed the bottleneck.

5. App-load path confirmed: app_load_stub (14458) reads the whole binary in one fs_read_stub call at 14586 → fat12_read (10576) with ES:DI = app_segment:0000, CX = file size. A 20 KB app = 40 single-sector INT 13h transactions (plus fat12_open's directory scan at 11037-11048, also one sector per call, up to 14 more).

6. No mitigating mechanism exists: all 12 `call floppy_read_sector` sites and every INT 13h AH=02 site in the kernel are single-sector (the FAT16/IDE path at 12032-12033 is also AL=1, but hard disks are out of scope). There is no track cache or read-ahead anywhere.

Mechanism check: standard 1.44MB formatting is 1:1 interleave, so between INT 13h returning for sector N and the re-issued read for N+1 (ret path + 512-byte movsb + FAT walk + CHS recompute + BIOS command setup), the ~1ms inter-sector gap at 300 RPM is long gone — the head waits nearly a full 200ms revolution per sector instead of ~11.1ms streamed. 40 sectors ≈ 8s vs <0.5s with track-run reads; the claimed 5-15x gain is realistic.

Two caveats that refine (not refute) the finding: (a) the cost is real-hardware-only — QEMU services INT 13h instantly, so the project's QEMU test rig (and the planned dynamic stress tests) will never reproduce it; (b) any direct-to-ES:DI multi-sector fix must respect two INT 13h constraints the single-sector code never hit: reads must not cross a track boundary, and the DMA transfer must not cross a physical 64KB boundary (BIOS error 0x09). The fix below handles both.
FIX: Three-part 8086-safe patch to kernel/kernel.asm:

(1) Add a multi-sector reader after floppy_read_sector (after line 11425). It chunks at track ends and 64KB DMA boundaries, 3 retries per chunk, same house style (cs-local vars):

floppy_read_sectors:            ; In: AX=start LBA, CX=count>=1, ES:BX=buffer
    mov [cs:.lba], ax           ; Out: CF=0 ok, BX advanced. Clobbers AX,CX,DX
    mov [cs:.left], cx
.chunk:
    cmp word [cs:.left], 0
    je .ok
    mov ax, [cs:.lba]           ; sectors to end of track = 18 - (lba mod 18)
    xor dx, dx
    push bx
    mov bx, 18
    div bx
    pop bx
    mov ax, 18
    sub ax, dx
    cmp ax, [cs:.left]
    jbe .t1
    mov ax, [cs:.left]
.t1:
    mov [cs:.cnt], ax
    mov ax, es                  ; whole sectors before 64KB DMA boundary
    mov cl, 4
    shl ax, cl
    add ax, bx                  ; AX = linear & 0xFFFF
    neg ax                      ; AX = bytes to boundary (0 = full 64KB)
    jz .dma_ok
    mov cl, 9
    shr ax, cl
    jz .fail                    ; <512B headroom: caller must bounce (see (2))
    cmp ax, [cs:.cnt]
    jae .dma_ok
    mov [cs:.cnt], ax
.dma_ok:
    mov byte [cs:.try], 3
.retry:
    push bx
    mov ax, [cs:.lba]           ; LBA -> CHS (same math as floppy_read_sector)
    xor dx, dx
    mov bx, 18
    div bx
    inc dx
    mov cl, dl                  ; CL = sector
    xor dx, dx
    mov bx, 2
    div bx
    mov ch, al                  ; CH = cylinder
    mov dh, dl                  ; DH = head
    pop bx
    mov ax, [cs:.cnt]           ; AL = sector count (1..18)
    mov ah, 0x02
    mov dl, 0x00
    int 0x13
    jnc .adv
    dec byte [cs:.try]
    jz .fail
    xor ah, ah
    mov dl, 0
    int 0x13
    jmp .retry
.adv:
    mov ax, [cs:.cnt]
    add [cs:.lba], ax
    sub [cs:.left], ax
    mov cl, 9
    shl ax, cl
    add bx, ax                  ; advance buffer (max 18*512=9216/chunk)
    jmp .chunk
.fail: stc
    ret
.ok:  clc
    ret
.lba: dw 0
.left: dw 0
.cnt: dw 0
.try: db 0

(2) In fat12_read, before the existing per-cluster body (insert at .cluster_loop, line 11772): if bytes remaining >= 512, walk the FAT chain (get_next_cluster preserves all regs but AX, and is RAM-cached so this walk costs no I/O) counting how many clusters starting at [si+12] are physically consecutive (next == cur+1), capped at (bytes_remaining >> 9) clusters; also cap by DMA headroom ((-((ES<<4)+DI)) & 0xFFFF) >> 9 computed as in (1). If the resulting run >= 1 sector: mov bx, di / call floppy_read_sectors with AX = (first_cluster-2)+[data_area_start], CX = run; on success add run*512 to DI and [si+16], subtract from [si+14], store the continuation cluster (the first non-consecutive next, or end-of-chain flag) into [si+12], jmp .cluster_loop. If run caps to 0 (buffer within 512B of a 64KB boundary) or fewer than 512 bytes remain, fall through to the existing single-sector bounce path unchanged — it is DMA-safe and handles the partial tail.

(3) Replace the byte-wise bounce copy at lines 11823-11827:
    mov ax, cx
    mov si, bpb_buffer
    mov bx, 0x1000
    mov ds, bx
    shr cx, 1
    rep movsw                   ; word copy, ~2x on 8086
    jnc .copy_done
    movsb                       ; odd trailing byte
.copy_done:
(the existing 'pop ds / mov cx, ax' at 11829-11830 already restores the byte count).

Result: a contiguous 20 KB app loads in ceil(40/18)+boundary = ~3-4 INT 13h calls instead of 40. Files written by fat12_alloc_cluster (11482, first-fit ascending scan) are near-always contiguous, so the run path is the common case. Verify on real hardware or 86Box/PCem with accurate floppy timing — QEMU will show no difference.

## [medium/high] rep stosb used where rep stosw is available in all row-fill fast paths, and row addresses recomputed with MUL per row
- src:performance | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel\kernel.asm: 9670-9697 (clear fast path: MUL 9674-9680, rep stosb 9691-9692); 9706-9792 (.opt_row: MUL 9712-9719, rep stosb 9756-9759); 16054-16093 (.gfc_row: MUL 16061-16067, rep stosb 16085-16086); 16158-16177 (.gfc_vga_row: MUL 16163-16166, rep stosb 16171); 15579-15612 (.icon_row: MUL 15585-15591) | area:graphics / fill
DESC: The CGA fast paths in gfx_clear_area_stub ('xor al, al / rep stosb', 9691-9692), gfx_fill_color ('mov al, [.fill_byte] / rep stosb', 16008-16009) and the VGA row fill (16093-16094, up to 320 bytes/row) all store bytes. On 8086/286/386 'rep stosw' moves twice the data per iteration and is still faster on the 8088's 8-bit bus (fewer instruction iterations); alignment penalties don't exist on 8088. Additionally every one of these row loops recomputes the row base with '(Y/2)*80' via 'mul di' on each iteration (9674-9680, 15984-15990; same pattern in gfx_draw_icon_stub rows 15506-15523) instead of strength-reducing: consecutive rows differ only by toggling +0x2000 and adding 80 after odd rows. For a full-screen clear that is 200 MULs plus 16,000 byte-stores that could be 100 adds plus 8,000 word-stores.
VERIFIER: CONFIRMED as a real performance finding (not a correctness bug — no misbehavior path; the code is functionally correct). Verified by reading C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:

1) gfx_clear_area_stub CGA fast path (9670-9697): per-row recompute of (Y/2)*80 via 'mov di,80 / mul di' at 9674-9680, then 'xor al,al / rep stosb' at 9691-9692. Cited lines were accurate. The hybrid .opt_row path (9706-9792) has the SAME pattern: per-row MUL at 9712-9719 and 'rep stosb' for the middle bytes at 9756-9759 — an additional site the finding only mentioned obliquely.

2) gfx_fill_color CGA fast path: the finding's line numbers were off by ~77 lines (16008-16009 is actually 'mov [.fill_color], al'). The real code: per-row MUL at 16061-16067, 'mov al,[.fill_byte] / rep stosb' at 16085-16086 inside the .gfc_row loop (16054-16093).

3) gfx_fill_color VGA path: cited 16086-16094, actually 16158-16177 (.gfc_vga_row): per-row 'mul word [screen_pitch]' at 16163-16166 and 'rep stosb' at 16171 with CX = width (up to 320 in mode 13h). Bounds are already clamped at 16135-16156, so the stosw rewrite cannot run past the 64000-byte framebuffer.

4) gfx_draw_icon_stub CGA path: cited 15506-15523, actually .icon_row at 15579-15612, per-row MUL at 15585-15591 for only 4 movsb per row — here the ~120-cycle 8088 MUL dwarfs the 4-byte copy, so strength reduction is the dominant win, not stosw.

Performance claims verified against documented cycle counts: REP STOSB is 9+10n on 8086/8088; REP STOSW is 9+10n_w on 8086 (2x throughput) and 9+14n_w on 8088 (8-bit bus, still ~1.4x). 286: 4+3n per element, 2x for words. So 'stosw wins on all targets including 8088' is correct, and README.md line 201 confirms minimum target is 'Intel 8088 @ 4.77 MHz', so this matters on real hardware. The '~2-3x combined' estimate holds for 8086/286+ full-screen fills (~2.6x by my cycle math) and for narrow fills where the per-row MUL dominates; on 8088 full-screen it is closer to ~1.6x — slightly optimistic but the right order of magnitude.

Correctness of the suggested fix idiom verified: 'shr cx,1 / rep stosw / adc cx,cx / rep stosb' is sound because neither STOS nor REP modifies flags, so CF from the SHR survives the rep and 'adc cx,cx' (CX=0 after rep) yields the 0/1 trailing-byte count. DF handling is unchanged vs the existing stosb code. Word stores to CGA/VGA memory write exactly the same bytes to the same addresses as the byte version (in the CGA fast path DI+count maxes at 0x1F8F, never straddling the 0x2000 bank). The strength-reduction algebra is correct: CGA row Y address = (Y/2)*80 + (Y&1)*0x2000, so Y->Y+1 is 'xor base,0x2000' plus 'add base,80' only when leaving an odd row.

One caveat on the '8086-safe' framing: the kernel already contains 125 multi-bit immediate shifts (e.g. 'shr si, 2' at 9669), which NASM assembles as 186+ opcodes (0xC1) that execute as RET aliases on a real 8088 — so the binary as built already cannot run on the advertised 8088 minimum. That is a separate pre-existing issue; the fix below uses only 8086-valid forms regardless.
FIX: Three word-fill replacements (8086-safe; REP STOSW preserves CF from the SHR):

(A) kernel.asm 9690-9692, gfx_clear_area_stub .fast_even — replace
    mov cx, si
    xor al, al
    rep stosb
with
    mov cx, si                      ; CX = byte count
    xor ax, ax                      ; AH must be 0 too
    shr cx, 1                       ; CF = odd-byte flag
    rep stosw
    adc cx, cx                      ; CX = 0/1 trailing byte
    rep stosb

(B) kernel.asm 16084-16086, gfx_fill_color .gfc_row — replace
    mov al, [.fill_byte]
    rep stosb
with
    mov al, [.fill_byte]
    mov ah, al
    shr cx, 1
    rep stosw
    adc cx, cx
    rep stosb

(C) kernel.asm 16169-16171, gfx_fill_color .gfc_vga_row — replace
    mov cx, dx
    mov al, [.fill_color]
    rep stosb
with
    mov cx, dx
    mov al, [.fill_color]
    mov ah, al
    shr cx, 1
    rep stosw
    adc cx, cx
    rep stosb
(better: hoist 'mov ah, al' next to the existing 'mov al,[.fill_color]' at 16157 and delete the in-loop reload at 16170; AL/AH are constant across rows.)

(D) Strength-reduce the per-row MUL. Representative rewrite of the gfx_clear_area_stub fast path (9667-9698); DX (width) is dead in this loop after SI=DX/4, so reuse it as the running row base:

    ; Fast path: byte-aligned, rep stosw per row
    mov si, dx
    shr si, 1
    shr si, 1                       ; SI = bytes per row
    ; one-time row base: DX = (Y/2)*80 + X/4 (+0x2000 if Y odd)
    mov ax, cx
    shr ax, 1
    mov di, 80
    mul di                          ; the ONLY MUL
    mov dx, ax
    mov ax, bx
    shr ax, 1
    shr ax, 1
    add dx, ax
    test cl, 1
    jz .fast_row
    add dx, 0x2000
.fast_row:
    mov di, dx
    push cx
    mov cx, si
    xor ax, ax
    shr cx, 1
    rep stosw
    adc cx, cx
    rep stosb
    pop cx
    xor dx, 0x2000                  ; toggle interlace bank
    test cl, 1
    jz .fast_next                   ; even->odd: same base
    add dx, 80                      ; odd->even: next row pair
.fast_next:
    inc cx
    dec bp
    jnz .fast_row
    jmp .clear_done

Apply the same hoist pattern to .opt_row (9712-9719), .gfc_row (16061-16067), .icon_row (15585-15591), and for .gfc_vga_row replace the per-row 'mul word [screen_pitch]' (16163-16168) with one pre-loop MUL plus 'add base,[screen_pitch]' per row.

## [medium/medium] Desktop dirty-rect repaint uses a label bounding box (52 px) far narrower than actual labels — stale/truncated label pixels after window moves
- src:performance | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: 15862-15871 (bbox constants; the two 'mov word [.icon_bbox_w], ...' at 15863 and 15869), 15893-15897 (left-cull test that misfires), 15951-15965 (label draw at icon_x-8 / icon_x-16) | area:window manager / desktop repaint
DESC: draw_desktop_region culls icons against the dirty rect using '.icon_bbox_w dw 52' (15786) — icon X-12 to X+52. But the label is drawn at X-8 (15876) and an 11-char name at the current 12 px advance spans to X+124 (X+80 even after the advance is fixed to 8). Icons whose label tail lies inside the cleared rect but whose bbox test fails are skipped (15817-15820), so after dragging/closing a window across a label, the overlapped part of the label is erased by the background fill (15772-15777) and never redrawn — leaving chopped-off label text. This is a concrete mechanism for the reported 'visual anomalies'.
VERIFIER: Confirmed, with corrected line numbers (the finding's cited lines 15786/15875-15888 actually fall inside fs_read_header_stub; the real code is at kernel.asm 15862-15871, 15893-15897, 15951-15965). Verified facts: (1) draw_desktop_region background-fills the dirty rect (15849-15854) then culls icons with .icon_bbox_w=52 lo-res / 84 hi-res (15862-15871); the left-cull test at 15893-15897 skips any icon with icon_x + bbox_w <= redraw_old_x. (2) The label is drawn at icon_x-8 lo-res / icon_x-16 hi-res (15952-15959) via gfx_draw_string_stub, which advances draw_font_advance per char; the default and only font in use is 8x8 with advance=12 (draw_font_advance db 12 at 20040, font_table entry at 20051; apps/launcher.asm never sets a font). (3) Names are up to 11 chars (12-byte field, 20442); the built-in 'Refresh' icon is 7 chars. An n-char label spans pixels [icon_x-8, icon_x+12n-12): n=6 reaches icon_x+59 > icon_x+52, n=11 reaches icon_x+120; in hi-res n>=9 exceeds icon_x+84. (4) All trigger paths reach this code with only the vacated rect as the dirty region: win_destroy (16484-16512), window move (18122-18140, fires per intermediate drag position), and resize (19369). (5) No masking mechanism: redraw_affected_windows only repaints windows after the desktop pass, there is no desktop-redraw event, and the launcher has no redraw handler — kernel-side draw_desktop_region is the sole repainter of icons. Concrete failure: icon at x=40 named 'Refresh' has label pixels x=32..111; closing/dragging a window whose left edge is at x=100 erases label pixels 100..111 and the cull test (40+52=92 <= 100) skips the icon, leaving the label visibly truncated indefinitely. Vertical extents and the left-shift margin (label_shift) are adequate; only the right extent is undersized. The finding's severity (medium, cosmetic-but-persistent) and 8086-safe constant fix are appropriate. One nuance: drawn icons paint their full label unclipped, which can over-paint background windows that do not intersect the dirty rect — that over-draw class already exists with the current constant; the cull box is supposed to over-approximate the painted area, and today it under-approximates, which is the bug.
FIX: Widen the cull box to the true worst-case label extent (11 chars max, current 12px advance, label start icon_x-8 lo-res / icon_x-16 hi-res; right edge exclusive = start + (11-1)*advance + 8). Two one-word constant changes, 8086-safe:

Line 15863 (lo-res), change:
    mov word [.icon_bbox_w], 52     ; Lo-res: icon + label right extent
to:
    mov word [.icon_bbox_w], 120    ; Lo-res: label (x-8) + 11 chars * 12px advance

Line 15869 (hi-res), change:
    mov word [.icon_bbox_w], 84     ; Hi-res: 32px icon + label right extent
to:
    mov word [.icon_bbox_w], 112    ; Hi-res: label (x-16) + 11 chars * 12px advance

(120 = -8 + 10*12 + 8; 112 = -16 + 10*12 + 8; the test 'icon_x + bbox_w <= rect_left -> skip' then matches the exclusive right edge of the widest possible label exactly. If draw_font_advance is later fixed to 8, these remain safe over-approximations — they can be tightened to 80/72 then. The static initializer '.icon_bbox_w: dw 52' at 15990 is dead (always overwritten at 15863/15869) and may be updated for consistency only.)

## [medium/high] Dispatcher and kernel use 386+ instructions (movzx, bt) — incompatible with the stated 8088 target
- src:performance | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm:172-173 and 203-204 (correct as cited). Secondary examples corrected: popa at 1812, pusha at 3026, 'shl si, 5' at 224, 'shl bx, 5' at 1375 (NOT 14740/14775). Contradicted docs: README.md:9, README.md:201, README.md:466. Pre-existing 386+ admissions: boot/mbr.asm:9, boot/stage2_hd.asm:5. | area:CPU compatibility / dispatcher
DESC: The INT 0x80 hot path executes 'movzx bx, ah / bt [api_drawing_bitmap], bx' (172-173, again at 203-204). MOVZX and BT are 80386 instructions; PUSHA/POPA (e.g. 14775) and multi-bit immediate shifts ('shl si, 5', 224; 'shl bx, 5', 14740) are 80186+. On an 8088/8086 the very first syscall raises invalid-opcode behavior (on 8086, executes garbage), so the OS as written cannot run on the 8088 performance target at all — every '8088-safe' optimization is moot until this is resolved. On 286/386 'bt mem,reg' is also slower than a simple table byte test. If the real floor is 386/486 this is informational; if 8088 support is required it is blocking.
VERIFIER: CONFIRMED, with corrections to scope and remedy. The primary citation is exact: kernel/kernel.asm:172-173 and 203-204 execute 'movzx bx, ah' / 'bt [api_drawing_bitmap/api_translate_bitmap], bx' in int_80_handler, the hot path of every syscall (reached for any AH < 105 after the bounds check at lines 140-141). MOVZX and BT are 80386+ instructions; on an 8088/8086 the 0F byte executes as POP CS (instruction-stream desync, garbage execution), on a 186 it is undefined, on a 286 it raises #UD. No mechanism prevents this: there is no 'cpu 8086' NASM directive anywhere in the repo, no runtime CPU detection/guard in any boot stage, and the failure fires on the very first INT 0x80. README.md contradicts this in three places: line 9 ('Intel 8088 or later processor'), line 201 (minimum CPU 'Intel 8088 @ 4.77 MHz'), line 466 (diagram '8088 / 8086 / 286 / 386 / 486').

However, the finding understates how deep the 386 dependency goes, which changes the correct fix. kernel.asm contains 125 occurrences of movzx/bt/pusha/popa AND 139 uses of 32-bit registers (eax/ebx/ecx/edx/esi/edi) — 32-bit registers have no 8086-safe rewrite short of major surgery. boot/mbr.asm:9 and boot/stage2_hd.asm:5 already carry explicit comments 'requires 386+ CPU', and the floppy-path boot/stage2.asm uses pusha/popa (186+). All tested hardware listed in the README (486DX4-75, 386SX, Atom, QEMU) is 386+. So patching lines 172-173/203-204 alone would NOT restore 8088 capability; the de facto floor of the codebase is 80386, and the real defect is documentation, not these two lines. Two details in the finding are wrong: (1) the secondary line numbers — line 14775 is 'jae .invalid' (no pusha) and 14740 is a comment (no 'shl bx, 5'); actual examples are popa at 1812, pusha at 3026, 'shl si, 5' at 224, 'shl bx, 5' at 1375; (2) the implied remedy (8086-safe rewrite) is impractical given the 32-bit register usage. One non-issue verified while checking: the BT bit offset is bounds-safe (AH <= 104, and both bitmaps are 14 bytes covering bit offsets 0-111), so there is no out-of-bounds read. Severity: low-medium documentation/requirements contradiction (per the finding's own framing: 'if the real floor is 386/486 this is informational'), not a blocking code bug — but any '8088-safe' optimization work elsewhere in the audit is indeed moot.
FIX: Minimal correct fix is documentation plus a boot-time guard, NOT an 8086 rewrite (139 uses of 32-bit registers make the kernel irreducibly 386+):

1. README.md — change line 201 from '| CPU | Intel 8088 @ 4.77 MHz | 80286+ |' to '| CPU | Intel 80386 | 80486+ |'; change line 9 'an Intel 8088 or later processor' to 'an Intel 80386 or later processor'; update the line 466 diagram '8088 / 8086 / 286 / 386 / 486' to '386 / 486 / Pentium'.

2. Add a CPU guard early in boot/stage2.asm (8086-safe code, runs before any 386 instruction) so pre-386 machines halt with a message instead of executing garbage:

    ; --- Require 386+: FLAGS bits 15:12 are forced to 1 on 8086/186, forced 0 on 286 ---
    pushf
    pop ax
    and ax, 0x0FFF          ; try to clear bits 15:12
    push ax
    popf
    pushf
    pop ax
    and ax, 0xF000
    cmp ax, 0xF000
    je .cpu_too_old         ; bits stuck at 1 -> 8086/8088/186
    mov ax, 0x7000          ; try to set NT|IOPL (bits 14:12)
    push ax
    popf
    pushf
    pop ax
    test ax, 0x7000
    jz .cpu_too_old         ; bits stuck at 0 -> 80286
    ; fall through: 386+
    ...
.cpu_too_old:
    mov si, msg_386         ; 'UnoDOS requires a 386 or later CPU'
    call print_string       ; (existing BIOS teletype routine in stage2)
    cli
    hlt

3. Optional (only if true 8086 support were ever mandated — not recommended): replace each movzx/bt pair at 172-173 and 203-204 with the 8086-safe sequence below plus an 8-byte mask table, and change 'jnc .no_translate' to 'jz .no_translate':

    push bx
    push ax
    mov bl, ah
    xor bh, bh
    push cx
    mov cl, 3
    shr bx, cl              ; BX = AH / 8 (byte index)
    pop cx
    mov al, [api_drawing_bitmap + bx]   ; bitmap byte
    mov bl, ah
    and bl, 7
    xor bh, bh
    test al, [bitmask_tab + bx]         ; bit set?
    pop ax
    pop bx
    jz .no_translate

    bitmask_tab: db 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80

This alone does not make the OS 8088-capable; options 1+2 are the actionable fix.

## [medium/medium] KEY_PRESS head-of-line blocking in the shared event queue can wedge all event delivery
- src:performance | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel/kernel.asm: 10220-10229 (KEY_PRESS head-of-line block), 10234-10254 (WIN_REDRAW head-of-line block), 10181-10182 (post_event silent drop when full), 609-626 (INT 9 dual-posting keys into event queue) | area:event system / input
DESC: event_get_stub reads only the queue head; if the head is a KEY_PRESS for the focused task, every other task gets 'no event' without advancing the head (10149-10152 'jmp .no_event — leave event in queue'). The same holds for WIN_REDRAW destined for another task (10176-10177). If the focused task is not draining events (e.g. it reads keys via kbd_getchar API 11, is stuck in a long operation, or focused_task points at a task that polls mouse state only), key events pile up at the head; the 32-entry queue fills, and post_event then silently drops all new events including mouse and redraw events ('cmp bx, [event_queue_head] / je .done', 10104-10105). Result: system-wide keyboard/mouse event loss while each task spins — a plausible mechanism for the reported intermittent input failures with multiple apps running.
VERIFIER: CONFIRMED, severity medium, with corrected line numbers and one softening of the impact claim.

Verified mechanism in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:
(1) event_get_stub (10194-10280) inspects ONLY the queue-head slot. A KEY_PRESS at the head with focused_task != current_task (and focused_task != 0xFF) takes 'jmp .no_event' at line 10229 WITHOUT advancing event_queue_head; a WIN_REDRAW owned by another task's window does the same at line 10254. Every event behind that head entry is unreachable by every other task.
(2) post_event (10160-10189) silently drops new events when full: lines 10181-10182 'cmp bx, [event_queue_head] / je .done' skip the tail advance (32 slots, 31 usable).
(3) The INT 9 handler dual-posts every keystroke into both the 16-byte kbd_buffer AND the event queue (lines 609-626). So a focused task that reads input via kbd_getchar (API 11) or kbd_wait_key (API 12) never drains its KEY_PRESS events from the event queue — the auditor's API-11 scenario is structurally real.
(4) No rescue mechanism exists: event_queue_head/tail are written nowhere else; there is no flush on focus change, window destroy, or task exit, and no aging. clear_kbd_buffer (line 700) drains via event_get_stub itself, so it is subject to the same focus filter and cannot clear another task's pending keys. The scheduler is cooperative-only (scheduler_next is reached only via yield/app_start/app_exit), so a focused task busy in a long operation cannot be preempted to drain. event_wait_stub (10378-10382) additionally spins without yielding — a latent hard-hang if any non-focused task ever blocks on API 10 (no bundled code does in multi-task context).

Concrete failure trace: app W owns the focused window (focused_task set at win_create, line 15304). W enters a long operation (file I/O, decode loop) or is a third-party app using API 11/12. User presses one key -> INT 9 posts KEY_PRESS; it sits at the head. Launcher/clock/etc. polling API 9 now get AL=0 forever (line 10229 path). PS/2 mouse movement posts EVENT_MOUSE per packet (~40-200/s, IRQ12 handler lines 1138-1142), filling the remaining ~30 slots in under a second; post_event then drops ALL new events — clicks, WIN_REDRAW from drags (e.g. 4371-4375, 16427-16431) — system-wide, until W polls API 9 again. This matches 'intermittent input failures with multiple apps running'.

Two corrections to the original finding: (a) the cited line numbers were stale — actual locations are 10220-10229 (KEY_PRESS leave-in-queue), 10254 (WIN_REDRAW leave-in-queue), 10181-10182 (post_event drop-when-full), 609-626 (INT 9 dual-post); (b) the wedge is not a total input freeze and is usually transient with the bundled apps: all 16 in-tree apps poll API 9 in their main loops, mouse cursor movement / window dragging / focus-by-click still work during the wedge (handled in IRQ12 + mouse_process_drag, outside the event queue), and clicking a window of a draining task reassigns focused_task (line 17917) which un-wedges the head (KEY_PRESS events carry no task stamp). A permanent wedge requires a hung focused app or a third-party app built on API 11/12 — which the public API explicitly offers. Medium severity is correct.
FIX: Replace head-only inspection in event_get_stub with a forward scan + EVENT_NONE tombstones. Safe without cli: event_get_stub runs only in task context (single consumer — call sites 708, 1990, 5189, 5873, INT 0x80 table); IRQ producers write only the tail slot, never the live region. All new instructions are 8086-safe. Replace lines 10205-10278 (.evt_check_next through .evt_discard) with:

.evt_check_next:
    mov bx, [event_queue_head]
.evt_scan:
    cmp bx, [event_queue_tail]
    je .no_event

    ; Calculate slot position (events are 3 bytes each)
    mov si, bx
    add si, bx                      ; SI = idx * 2
    add si, bx                      ; SI = idx * 3

    mov al, [event_queue + si]      ; type
    mov dx, [event_queue + si + 1]  ; data (word)

    cmp al, EVENT_NONE              ; Tombstone (consumed out of order)?
    je .evt_tombstone_slot

    ; Filter keyboard events: only deliver to focused task
    cmp al, EVENT_KEY_PRESS
    jne .evt_not_key
    push ax
    mov al, [focused_task]
    cmp al, 0xFF                    ; No window focused? Deliver to current task
    je .evt_focus_ok
    cmp al, [current_task]
    je .evt_focus_ok
    pop ax
    jmp .evt_next_slot              ; Not focused - scan PAST it, leave for owner
.evt_focus_ok:
    pop ax
    jmp .evt_consume

.evt_not_key:
    cmp al, EVENT_WIN_REDRAW
    jne .evt_consume                ; Other event types: consume and pass through
    cmp dl, WIN_MAX_COUNT
    jae .evt_discard                ; Invalid window handle: drop slot and retry
    push si
    push ax
    xor ah, ah
    mov al, dl
    mov si, ax
    shl si, 5
    add si, window_table
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .evt_discard_pop            ; Window freed/destroyed: discard stale event
    mov al, [si + WIN_OFF_OWNER]
    cmp al, [current_task]
    pop ax
    pop si
    je .evt_consume                 ; Our window: consume and return
    jmp .evt_next_slot              ; Wrong task's window - scan PAST it

.evt_next_slot:
    inc bx
    and bx, 0x1F
    jmp .evt_scan

.evt_tombstone_slot:
    cmp bx, [event_queue_head]      ; Reclaim tombstone only at head
    jne .evt_next_slot
    inc bx
    and bx, 0x1F
    mov [event_queue_head], bx
    jmp .evt_check_next

.evt_consume:
    cmp bx, [event_queue_head]
    jne .evt_mark_consumed
    inc bx                          ; Consumed at head: advance head
    and bx, 0x1F                    ; Wrap at 32 events
    mov [event_queue_head], bx
    jmp .evt_return
.evt_mark_consumed:
    mov byte [event_queue + si], EVENT_NONE  ; Mid-queue: tombstone, head reclaims later

.evt_return:
    clc                             ; CF=0 = event available
    pop ds
    pop si
    pop bx
    ret

.evt_discard_pop:
    pop ax
    pop si
.evt_discard:
    cmp bx, [event_queue_head]
    jne .evt_discard_mid
    inc bx                          ; Invalid event at head: advance head
    and bx, 0x1F
    mov [event_queue_head], bx
    jmp .evt_check_next
.evt_discard_mid:
    mov byte [event_queue + si], EVENT_NONE  ; Invalid mid-queue: tombstone
    jmp .evt_next_slot

Notes: (1) .evt_return/.no_event labels and everything after line 10280 are unchanged. (2) Loop terminates: BX strictly advances toward a tail that is at most 31 slots ahead. (3) Residual (acceptable): undeliverable KEY_PRESS entries still occupy slots until the focused task drains or focus changes, so an extreme key backlog can still fill the queue — but mouse/redraw/key events for all other tasks now flow past, eliminating the system-wide wedge. If desired later, cap KEY_PRESS backlog by having the INT 9 .skip_buffer path also skip post_event when the queue is more than half full of KEY_PRESS — not required for this fix.


# OBSERVED (25)

## [critical/high] 8th concurrent task launch fails silently and wipes all windows from screen (user's #1 symptom)
- src:dyn-stress-launch | C:\Users\arin\Documents\Github\unodos\test-artifacts\sessionD\d7.png:n/a (dynamic finding; evidence also in test-artifacts/sessionD/d6.png, d8.png, test-artifacts/stress/t6.png, t7.png, t8.png) | area:task manager / launcher failure path
DESC: The OS supports exactly 7 running tasks (desktop + 6 apps... precisely: SysInfo reports 'Tasks 7 running' in sessionD/d6.png with desktop + 4x Mouse Test + Clock + SysInfo). Launching an 8th task does NOT show any error: the launch silently fails AND triggers a full desktop repaint that erases every open window's frame and contents (sessionD/d7.png: SysInfo, Clock and 4 Mouse Test windows all gone; only the Clock app's dynamic drawing persists frameless over bare desktop; PacMan never started). The same signature occurred in the same-app stress test: 6 Mouse Test instances launched fine (stress/t6.png), the 7th app double-click wiped the UI (stress/t7.png), and a subsequent SysInfo launch failed silently with no repaint (stress/t8.png). The system is NOT hung - the clock keeps ticking (d7: 13:15:37 -> d8: 13:15:41) - but to a user the desktop looks destroyed/crashed. This precisely reproduces and explains the reported 'launching many apps crashes' symptom.
FIX: In the app-launch path, check for a free task slot BEFORE any UI teardown/repaint; on failure show an error dialog (or at least leave the screen untouched) instead of running the desktop full-repaint routine. Separately make the full-repaint routine re-render open windows (see z-order finding). Consider raising the task table size or making it configurable.

## [critical/high] Single left-click in a window body destroys the window frame while the app keeps running
- src:dyn-window-zorder | C:\Users\arin\Documents\Github\unodos\test-artifacts\winz\e1.png:n/a (dynamic QEMU test) | area:window manager / mouse click dispatch
DESC: With only the Clock window open, one left-click on its body (guest 160,100) removed the entire frame (title bar, border, [X]) while the Clock app kept running and painting its face and digital time frameless onto the desktop (e1.png, time still ticking 13:02:26). Desktop under the former frame was only partially repainted, leaving ':' and '|' fragments and erased icons (MkBoot, Notepad, Settings partially). Same with two windows: in w1.png SysInfo+Clock were stacked; ONE click on the Clock's exposed bottom strip (160,145) made BOTH window frames vanish (w3.png), leaving the Clock drawing frameless at a wrong position and the desktop with missing SysInfo/Music icons and half-erased Clock/MkBoot/Tetris/Notepad icons. Reproduced again in d3.png. This makes mouse interaction with windows essentially unusable and is the root of most other symptoms.
FIX: In the mouse click handler, hit-test the click against window regions and only forward it to the owning window's controls ([X], OK); a click in the client area must not close/destroy the window record. If 'click dismisses dialog' is intended for SysInfo's modal box, it must not also tear down other windows' frames, and the app task must exit (or keep a valid window) instead of being orphaned to paint frameless.

## [high/high] Clicking empty desktop repaints desktop OVER all open windows, destroying them visually
- src:dyn-stress-launch | C:\Users\arin\Documents\Github\unodos\test-artifacts\focus\f2.png:n/a (also focus/f1.png before-state, focus/f3.png, sessionA\launch4.png) | area:desktop shell / repaint logic
DESC: With the Clock window open (focus/f1.png), one left-click on an empty desktop area redraws all desktop icons over the window and erases its frame (focus/f2.png); the clock app keeps animating frameless on the bare desktop (time advances 12:58:15 -> 12:58:18 in f3). Same phenomenon visible in sessionA/launch4.png where desktop icons are painted inside the Mouse Test window. This is the same buggy 'repaint desktop without re-rendering windows' routine used by the failed-launch path, triggerable by a single stray click.
FIX: After repainting the desktop background/icons, iterate the window list and re-render every open window in z-order (or only repaint desktop regions not covered by windows).

## [high/high] No way to launch another app by keyboard once any window has focus; click on desktop does not transfer keyboard focus
- src:dyn-stress-launch | C:\Users\arin\Documents\Github\unodos\test-artifacts\focus\f4.png:n/a (also focus/f3.png, sessionA/launch2.png) | area:input focus management
DESC: Once an app window is open it swallows ALL keyboard input: arrows/Enter never reach the desktop again. Clicking empty desktop does not give the desktop keyboard focus (focus/f3.png: no selection box appears after 'key down'; focus/f4.png: 8 navigation keys + Enter produced no Files window and Clock kept running). In sessionA/launch2.png, Enter intended for the desktop instead pressed the focused SysInfo window's OK button and closed it. The only working multi-launch path is mouse double-click on an icon not covered by a window; icons covered by windows (most of the grid once 1-2 windows are open) become unlaunchable.
FIX: Make a click on empty desktop (or a dedicated hotkey, e.g. Tab/Alt-Esc) move keyboard focus back to the desktop shell so icon navigation and Enter-launch work while windows are open.

## [high/high] No window clipping/z-order in rendering: apps draw through overlapping windows
- src:dyn-stress-launch | C:\Users\arin\Documents\Github\unodos\test-artifacts\sessionA5\launch3.png:n/a (also sessionA5/launch2.png, launch4.png, launch5.png, sessionD/d6.png) | area:window manager / graphics compositing
DESC: Windows do not clip each other. The Clock app keeps drawing its hands, tick marks and digital time directly to screen coordinates regardless of what is on top: it overwrites the Mouse Test window interior (sessionA5/launch2.png - Pos/Buttons text destroyed, only 'M:--' left), wipes the filename column of the File Manager list (sessionA5/launch3.png - names reduced to 'BIN/IN/N' fragments with '13:08:25' painted across them), erases SysInfo's Font/Time/Uptime values (sessionD/d6.png), and even draws over the fullscreen CGA Pac-Man title screen (sessionA5/launch5.png). Conversely a newly opened window erases the content underneath permanently because occluded apps never get a repaint when uncovered.
FIX: Either clip app drawing to the app's own window rect minus overlying window rects, or adopt dirty-rectangle/expose events so occluded windows repaint when revealed and topmost windows are redrawn after anything paints beneath them. Minimum viable fix: route all app drawing through a per-window framebuffer or enforce draw-only-when-topmost.

## [high/high] Incomplete/incorrect expose repaint: missing menu bar, half-erased icons, ghost frame fragments persist
- src:dyn-window-zorder | C:\Users\arin\Documents\Github\unodos\test-artifacts\winz\r1c.png:n/a (dynamic QEMU test) | area:window manager / desktop repaint
DESC: After mouse-initiated window closes the desktop is never fully restored: top menu bar 'UnoDOS 3' stays erased, SysInfo and Music icons missing, Clock/MkBoot icons half-drawn, a pink fragment of the old SysInfo [X]/title region remains at the top-right corner, and a stale cursor-shaped fragment sits near the Files icon (r1c.png, r2c.png, final.png, w3.png, e1.png). After drag, an empty rectangle of the old Clock frame is left behind and the menu bar text is shredded (d2.png). The corruption is stable across three open/close cycles (r1c≈r2c≈final — it does not accumulate further), and a clean app exit repaints the whole desktop perfectly (e2.png), so a full-desktop repaint routine exists but is not invoked/clipped correctly on expose.
FIX: On window close/move, invalidate the union of the old window rect (frame included, not just client area) plus the menu bar row if overlapped, then redraw desktop background, menu bar, all intersecting icons, and any underlying windows' frames+content — or reuse the proven full-desktop repaint used on app exit (e2.png) as a fallback.

## [high/high] Background window paints into the foreground window's client area (no per-window clipping/translation)
- src:dyn-window-zorder | C:\Users\arin\Documents\Github\unodos\test-artifacts\winz\w1.png:n/a (dynamic QEMU test) | area:window manager / drawing API
DESC: With Clock open and SysInfo launched on top, the Clock's periodic redraw renders its analog face and digital time INSIDE the SysInfo window's client area, erasing SysInfo's label column (Video/Boot/Tasks/Font/Time/Uptime captions gone, only the values column remains) — w1.png, reproduced in d1.png and r1b.png. Proof it draws window-relative into the wrong window: after dragging the SysInfo window +40,+30, the clock face moved along with the dragged window (d2.png). The background app's drawing appears to be applied to the topmost/current window's coordinate space instead of its own, and is not clipped by z-order.
FIX: Make drawing calls resolve coordinates against the calling task's own window (store window handle per task, not a global 'current window'), and clip background-window output to its visible region (or simply suppress drawing for non-topmost windows until they are raised/exposed).

## [high/medium] Click-to-raise (z-order) not functional; cannot bring a background window to front
- src:dyn-window-zorder | C:\Users\arin\Documents\Github\unodos\test-artifacts\winz\w3.png:n/a (dynamic QEMU test) | area:window manager / z-order
DESC: Scenario steps 2-3 (click background window to raise it, active/inactive title bars swap) could not be observed because any click on a window destroys frames instead of raising (w3.png: clicking Clock's exposed strip below the SysInfo window removed both frames rather than bringing Clock to front). No active-vs-inactive title bar styling was ever visible: in every two-window state (w1.png, d1.png) both title bars render identically (the lower one is simply overdrawn). Raise-on-click appears unimplemented or unreachable.
FIX: Implement raise-to-front on title-bar/body click: reorder the window list, repaint the raised window above others, and render a dimmed title bar for unfocused windows so focus is visible.

## [medium/high] Desktop icon highlight painted over open window (z-order violation)
- src:dyn-input | C:\Users\arin\Documents\Github\unodos\apps\launcher.asm:1835-1867 | area:launcher / window manager drawing
DESC: With the Mouse Test window open, a left click on the desktop just below the window selected the OutLast icon and drew the highlighted icon plus its label ON TOP of the window's interior and bottom border. Compare test-artifacts/input-mouse3/m3.png (clean window) with m4.png (OutLast bell icon and 'utLast' label scribbled across the window's lower half); the corruption persists in m5.png. Cause: select_icon calls draw_single_icon directly with no occlusion check and no repaint_all_windows afterwards, so an icon located under (or partially under) a window is painted straight over the window content. This explains user reports of windows getting 'corrupted' when clicking around the desktop.
FIX: In select_icon (and clear_icon_area/draw_single_icon callers), skip drawing if the icon rect intersects any window (e.g. test the icon rect corners with API_POINT_OVER_WINDOW), or follow the highlight draw with repaint_all_windows plus a content-repaint notification to the owning app, as launch_app's refresh path already does (lines 1927-1928).

## [medium/high] Notepad status bar (Ln/Col/byte count) stale while typing
- src:dyn-input | C:\Users\arin\Documents\Github\unodos\apps\notepad.asm:461-488 | area:notepad UI
DESC: The status bar does not update on character insertion. After typing 10 characters ('helloA 123') it still read 'Ln1 Co1 / 0 B' (test-artifacts/input-notepad/n2.png, n3.png, n4.png). It refreshed only after arrow keys: n5.png shows 'Co9 / 10 B', which is the state BEFORE the 'x' that was just inserted (buffer actually had 11 chars, cursor col 10). n6.png shows 15 bytes in the buffer but still 'Co9 / 10 B'. A Ctrl+V event refreshed it to correct values (input-clip/c3.png 'Co3 / 2 B'). Cause: the .type_insert fast path uses draw_typed_char ('Ultra-fast: draw just the typed char + cursor', line 1076) and never calls draw_status, which is only invoked from the cursor-movement/edit paths (lines 351, 892-963, 1145, 1905).
FIX: Have the typed-char fast path also redraw just the numeric fields of the status bar (a one-line text draw is cheap), or set a status-dirty flag flushed on the next idle/main-loop pass so typing latency is unaffected.

## [medium/high] Mouse driver does not clamp internal position accumulator; cursor sticks at screen edge after overshoot
- src:dyn-stress-launch | C:\Users\arin\Documents\Github\unodos\test-artifacts\mousediag\d7.png:n/a (also mousediag/d6.png, d1.png, d4.png, d5.png) | area:PS/2 mouse driver
DESC: Normal deltas track perfectly (start exactly at 160,100; +10/+100 moves correct, mousediag/d1.png Pos170,100, d4.png Pos265,199). But after a large overshooting negative move (-2000,-2000 -> Pos0,0, d6.png), a following +200,+120 move leaves the position at Pos0,0 (d7.png): the driver accumulates the out-of-range position internally and only clamps the displayed value, so the cursor ignores opposite motion until the overshoot unwinds. On real hardware fast mouse swipes will make the cursor stick at screen edges and feel dead.
FIX: Clamp the internal X/Y position to [0,319]/[0,199] immediately after adding each PS/2 delta packet, not just at draw time.

## [medium/high] Icon selection rectangle not erased when selection moves - leaves a trail of boxes
- src:dyn-stress-launch | C:\Users\arin\Documents\Github\unodos\test-artifacts\sessionA\launch3.png:n/a | area:desktop shell / icon rendering
DESC: After moving the selection right three times (SysInfo -> Clock -> Files -> Mouse) and launching, all four row-1 icons show selection rectangles simultaneously (sessionA/launch3.png). The previous icon's highlight box is not erased when selection moves; only a full desktop repaint clears the stale boxes.
FIX: When the selection changes, redraw the previously selected icon cell without the highlight before drawing the new one.

## [medium/high] Stale desktop icon selection highlight: previous icon stays boxed when selection moves
- src:dyn-window-zorder | C:\Users\arin\Documents\Github\unodos\test-artifacts\winz\b0.png:n/a (dynamic QEMU test) | area:desktop shell / icon selection
DESC: Pressing right,right selects Clock, but the selection rectangle previously drawn around SysInfo is not erased — b0.png shows BOTH the Sys and Clock icons with selection boxes simultaneously (also visible in w0.png). Single right press correctly boxes only SysInfo (k1.png), so the deselect/erase step is missing or draws at the wrong cell when the selection moves.
FIX: When the selection index changes, redraw the previously selected icon cell without the highlight (or XOR the old rectangle away) before drawing the new highlight.

## [medium/high] Window drag works but allows dragging off-screen (close button unreachable) and corrupts the menu bar
- src:dyn-window-zorder | C:\Users\arin\Documents\Github\unodos\test-artifacts\winz\d2.png:n/a (dynamic QEMU test) | area:window manager / drag
DESC: Press-and-hold on the SysInfo title bar then moving +40,+30 did move the window with its content (d2.png) — drag is implemented. Defects: the window's right edge (with the [X] button) went past the 320px screen edge with no clamping, making the close button unreachable; the vacated region repaint shredded the 'UnoDOS 3' menu bar; a ghost rectangle of the underlying Clock window frame remained near the bottom. (Drag was made possible by adding a 'btn' press/hold command to tools/qemu_test.sh; the stock driver only has atomic click.)
FIX: Clamp window x/y during drag so the title bar (at least the [X]) stays on-screen, and repaint the exposed region including the menu bar row and underlying windows.

## [medium/high] SysInfo 'Uptime' is wrong: ~3000s shown seconds after a fresh boot, value persists across reboots
- src:dyn-window-zorder | C:\Users\arin\Documents\Github\unodos\test-artifacts\winz\k3.png:n/a (dynamic QEMU test) | area:SysInfo app / time-keeping
DESC: About 17s after a cold boot SysInfo shows 'Uptime 3054s M:04' (k3.png, Time 12:50:20). In later, independent boots it shows 3370s at 12:55:31 (w1.png), 3390s at 12:55:53 (r1b.png), 3634s at 12:59:51 (d1.png) — the deltas track the RTC wall clock exactly (e.g. 311s of wall time vs 316s of 'uptime' between two separate boots), proving uptime is derived from time-of-day rather than time-since-boot (QEMU runs with -rtc base=localtime; every boot continues the same counter).
FIX: Latch the tick counter (or RTC time) at kernel init and display current_ticks - boot_ticks; do not derive uptime from the wall-clock/BIOS midnight counter.

## [medium/high] Mouse driver permanently dies after a large PS/2 movement burst; ghost cursor left behind
- src:dyn-window-zorder | C:\Users\arin\Documents\Github\unodos\test-artifacts\winz\m3.png:n/a (dynamic QEMU test) | area:PS/2 mouse driver
DESC: QEMU monitor 'mouse_move -2000 -2000' (which the guest receives as overflow/large PS/2 packets) makes the cursor jump to (0,0) while the old cursor sprite at mid-screen is NOT erased (ghost arrow visible in a2.png and m1.png), and every subsequent mouse move and click is ignored for the rest of the session (m2.png, m3.png unchanged; dblclick in a2 had no effect). Normal moves up to +/-200 work perfectly with clean erase/redraw and exact 1:1 pixel mapping (c1-c5.png). Likely the driver mishandles PS/2 overflow bits or loses 3-byte packet alignment and never resynchronizes. Real hardware can generate overflow packets on fast motion, so this can kill the mouse until reboot.
FIX: In the PS/2 interrupt handler, validate the first packet byte (bit 3 must be set) to resynchronize the 3-byte stream, clamp/ignore deltas when the X/Y overflow bits (6-7 of byte 0) are set, and always erase the cursor at the old position before drawing at the clamped new one.

## [medium/low] Launching Files erased the menu bar and a large desktop region with no window appearing
- src:dyn-window-zorder | C:\Users\arin\Documents\Github\unodos\test-artifacts\winz\r1a.png:n/a (dynamic QEMU test) | area:Files app / window manager
DESC: During the repeat cycles the Files icon got selected by a stray click; pressing Enter then blanked the 'UnoDOS 3' menu bar and the upper-left desktop quadrant without any visible Files window (r1a.png) — consistent with the app opening and instantly exiting/crashing without a repaint, or its window being created then destroyed by the pre-existing corruption. Observed once, in an already-corrupted session, so confounded; SysInfo's task count afterwards (3 running in r1b.png) suggests the Files task did not stay alive.
FIX: Retest Files from a clean boot (keyboard: arrows to Files, Enter). If it exits immediately, fix its startup error and ensure app exit always triggers the full-desktop repaint that clean exits perform (e2.png).

## [low/high] No icon selected at boot; first arrow keypress is consumed selecting icon 0
- src:dyn-input | C:\Users\arin\Documents\Github\unodos\apps\launcher.asm:104, 360-362 | area:launcher keyboard navigation
DESC: At boot selected_icon is 0xFF (no visible selection, input-mouse2/s0.png). The first arrow press of any kind selects icon 0 (SysInfo) instead of moving (.kb_select_first), so 'right right right' lands on Files, not Mouse: s1.png = SysInfo highlighted, s2.png = Clock, s3.png = Files, and Enter opened File Manager (input-mouse/m1.png) instead of the intended Mouse app. Reaching Mouse requires four presses (input-mouse3/sel.png). Not a scancode bug — every press registered exactly once — but the invisible initial state makes keyboard navigation off-by-one versus user expectation and contradicted the documented nav formula.
FIX: Select and highlight icon 0 when the desktop finishes drawing (set selected_icon=0 and call select_icon after redraw_desktop), so the first arrow press moves the selection instead of creating it.

## [low/high] Desktop icon labels overlap neighbours and leave stale artifacts after deselection
- src:dyn-input | C:\Users\arin\Documents\Github\unodos\apps\launcher.asm:1873-1908 | area:launcher icon rendering (cosmetic)
DESC: Icon labels are wider than the grid cell, so adjacent labels truncate each other ('Sys InfClock', 'OutLastOutLa', 'Pac-ManPacMan' — input-mouse/desktop.png). Selecting an icon redraws its full label over the neighbour ('Sys Infolock' in input-mouse2/s1.png), and because clear_icon_area only clears a 24x24 box around the icon bitmap (not the label strip), the corrupted label text persists after the selection moves away — input-notepad/nsel.png still shows 'Infolock' while Notepad is the selected icon. Final label appearance depends on draw order.
FIX: Clip/truncate label rendering to the icon cell width (GRID column width), and include the label strip in clear_icon_area so deselection restores a clean cell before redrawing both affected labels.

## [low/medium] Mouse pointer sprite intermittently partial or missing while an app redraws
- src:dyn-input | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:3730-3776 | area:kernel mouse cursor rendering
DESC: In 4 of the Notepad-session screenshots the mouse cursor (untouched at screen centre the whole time) is rendered as a partial fragment or is absent entirely: input-notepad/n3.png and n4.png show a broken sprite, input-clip/c2.png a fragment, c3.png no cursor at all, versus the full arrow in c1.png/n2.png. The kernel hides/shows the cursor (mouse_cursor_hide/mouse_cursor_show) around every draw API call regardless of whether the drawing overlaps the cursor rect, so the cursor blinks out during Notepad's periodic text-cursor/status redraws; QEMU screendumps repeatedly catch it mid hide/show, implying user-visible cursor flicker. Some of this may be screendump timing, but the hit rate (4/10 shots) indicates the cursor is hidden a noticeable fraction of the time.
FIX: Only call mouse_cursor_hide when the pending draw rectangle actually intersects the cursor's bounding box (compare against mouse X/Y plus sprite size); otherwise leave the cursor on screen.

## [low/high] SysInfo Uptime wildly wrong right after boot (e.g. 2978s when uptime is ~20s)
- src:dyn-stress-launch | C:\Users\arin\Documents\Github\unodos\test-artifacts\recon\s1.png:n/a (also sessionA/launch1.png '3103s', sessionA5/launch4.png '514s') | area:SysInfo app / time-keeping
DESC: Roughly 20 seconds after power-on, SysInfo reports 'Uptime 2978s M:04' (recon/s1.png, wall time 12:49:04); a later boot showed 3103s (sessionA/launch1.png) and another 514s (sessionA5/launch4.png). The value appears to be derived from the RTC/BIOS tick of day rather than time since boot. The mysterious constant 'M:04' field also never changes across boots.
FIX: Latch the BIOS tick count (or PIT-based counter) at kernel init and compute uptime as current minus boot value, handling midnight rollover; clarify or fix the 'M:' field.

## [low/high] Icon labels overlap and corrupt each other on redraw ('Sys InfClock' becomes 'Infolock')
- src:dyn-stress-launch | C:\Users\arin\Documents\Github\unodos\test-artifacts\recon\sel1.png:n/a (compare recon/baseline.png) | area:desktop shell / icon label layout
DESC: Labels are wider than the icon grid cell and overwrite neighbours: baseline shows 'Sys InfClock', 'SettingDostr', 'OutLastOutLa' (recon/baseline.png). Redrawing one icon re-clips the neighbour differently - after selecting icon 0 the row reads 'Sys Infolock' (recon/sel1.png). Purely cosmetic but makes several labels unreadable.
FIX: Truncate or center labels to the grid cell width (about 9-10 chars at 8x8 font in a 80px cell), or shorten app names.

## [low/medium] Mouse Test shows 'L:ON' after the button has been released
- src:dyn-stress-launch | C:\Users\arin\Documents\Github\unodos\test-artifacts\sessionD\d5.png:n/a | area:Mouse Test app / button event delivery
DESC: After double-clicks used to launch apps (button long released), the partially-visible Mouse Test window still displays 'L:ON' (sessionD/d5.png). Either the release event was never delivered to the app (consumed by the desktop click handling) or the app fails to repaint on release. Minor, but suggests button release events may be lost when clicks are handled by another consumer.
FIX: Ensure button-up events are broadcast to apps that saw the corresponding button-down, or have Mouse Test poll current button state instead of relying on edge events.

## [low/high] Enter on freshly booted desktop does nothing until an arrow key establishes selection
- src:dyn-window-zorder | C:\Users\arin\Documents\Github\unodos\test-artifacts\winz\a1.png:n/a (dynamic QEMU test) | area:desktop shell / keyboard navigation
DESC: Immediately after boot, pressing Enter has no effect (a1.png identical to desktop.png) because no icon is selected; the first arrow key press selects the first icon (k1.png) and Enter then launches it (k3.png). Minor usability nit; possibly intended.
FIX: Pre-select the first icon at desktop startup so Enter works immediately, or treat Enter with no selection as selecting the first icon.

## [low/high] Desktop icon labels overlap adjacent cells and change with redraw order
- src:dyn-window-zorder | C:\Users\arin\Documents\Github\unodos\test-artifacts\winz\desktop.png:n/a (dynamic QEMU test) | area:desktop shell / icon labels
DESC: Labels longer than the grid cell overwrite their right-hand neighbour: 'Sys Inf'+'Clock' renders as 'Sys InfClock', also 'SettingDostr', 'OutLastOutLa', 'Pac-ManPacMan' (desktop.png). After the SysInfo icon is re-highlighted its label is redrawn on top and the text changes to 'Sys Infolock' (k1.png), so the visible text depends on paint order.
FIX: Truncate or ellipsize labels to the icon cell width (or center a max-8-char label per cell) so neighbouring labels never overlap.


# UNCERTAIN (0)


# UNVERIFIED-LOW (10)

## [low/high] launch_app ignores app_start failure (CF) - app left in LOADED state and segment leaked
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\apps\launcher.asm:1944-1948 | area:launcher / launch path
DESC: After a successful API_APP_LOAD, the launcher issues `mov ah, API_APP_START / int 0x80` and falls straight to `.la_done` with no `jc` check (1944-1948). app_start_stub sets CF on invalid handle/state (kernel.asm:14931-14932). If start fails, the app stays APP_STATE_LOADED forever: its pool segment (allocated by app_load) is never freed because app_exit/free paths only run for RUNNING tasks, and the app_table slot stays occupied. Repeated occurrences exhaust the 6-segment pool, after which every launch fails with error 4 - consistent with 'crashes/failures when too many apps have been launched'. (The error path for app_load itself is handled at 1942/.la_error.)
FIX: Add `jc .la_start_error` after the API_APP_START int 0x80; in the error handler display the error and unload the app (add/call an app_unload API that sets the slot FREE and calls free_segment on its code segment) so the slot and segment are reclaimed.

## [low/high] Kernel never establishes its own stack - runs indefinitely on the boot sector's SS:SP = 0000:7C00 in unowned, undocumented low memory
- src:boot-memory-map | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:15-36 | area:stack layout
DESC: boot.asm sets 'mov ss, ax / mov sp, 0x7C00' with AX=0 (boot.asm 43-47); neither stage2 nor the kernel entry ever loads SS:SP again - kernel entry (kernel.asm 15-36) sets only DS/ES, and the only 'mov ss' instructions in the whole kernel are the app-context restores at lines 1809, 14843 and 15036. So the kernel main context (boot, launcher auto-load, scheduler idle, every ISR that fires while the kernel context is current) runs on a stack at linear 0x7C00 growing down through memory the README's memory map (line 217-232) does not reserve for a stack. It currently collides with nothing - the next kernel-owned object below is the INT 13h DAP scratch at 0x0000:0x0600-0x0610 (kernel.asm 11893-11898), ~29KB below - but it silently overlaps the region marked 'Boot Sector (temporary)' and is one design change away from a collision (anything else placed in 0x0500-0x7BFF). App stacks are fine: each app gets SS=its own 64KB segment, SP=0xFFE0 (lines 14915-14916), top of slot, below the 0x9000 scratch segment.
FIX: At kernel entry, explicitly claim a stack in the otherwise-unused gap between kernel end (0x1B000) and the launcher (0x20000): 'cli / mov ax, 0x1000 / mov ss, ax / mov sp, 0xFFFE / sti' gives the kernel a ~20KB private stack at linear 0x1B000-0x1FFFE (top of the kernel's own 64KB segment, with SS=DS=CS simplifying any future stack-pointer arithmetic). Document it in the README memory map.

## [low/medium] auto_load_launcher reads the launcher filename via stale caller_ds when invoked from the last-task-exit path; failure path returns into a freed stack
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:15042-15046, 14393-14396, 1752-1758, 1847-1854 | area:app exit / launcher reload
DESC: app_exit_stub's `.exit_no_tasks` (15043-15044) calls auto_load_launcher when the last task exits. auto_load_launcher sets DS=0x1000 and SI=.launcher_filename (1753-1755) and calls app_load_stub directly, but app_load_stub fetches the filename SEGMENT from the global `caller_ds` (14394-14396 'Use caller_ds since DS is now kernel segment after INT 0x80 dispatch'), which at that moment still holds the EXITING app's data segment - so fs_open parses garbage from the dead app's segment instead of 'LAUNCHER.BIN'. The load fails, and the `.failed` path (1847-1854) runs keyboard_demo and then `ret` - executing on the freed exiting app's stack at ~0xFFFE, where the return address is garbage. Only reachable when the launcher itself exits/dies, but then it guarantees a wedge instead of recovery.
FIX: Set `mov word [caller_ds], 0x1000` at the top of auto_load_launcher (mirroring lines 1766/1819), and make `.exit_no_tasks` not return: after auto_load_launcher succeeds it context-switches away (1808-1813); on failure jump to a kernel halt/panic loop rather than `ret` on the dead task's stack.

## [low/medium] Keyboard scancode lookup can read past the 96-byte translation tables
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:500-517, 2153-2167 | area:keyboard driver
DESC: int_09_handler filters releases (`test al,0x80` at 501) and then indexes `scancode_normal + bx` / `scancode_shifted + bx` with the raw make code (504-517), but both tables are only 96 bytes (6 x 16 rows, 2153-2167). Non-extended make codes 0x60-0x7F (some non-US keyboards, ACPI/multimedia keys without E0 prefix on certain controllers) index past scancode_normal into scancode_shifted (wrong character inserted) and past scancode_shifted into following data. No crash (read-only), but spurious characters can appear - a minor contributor to 'keyboard input issues' on real hardware.
FIX: Bounds-check before lookup: `cmp al, 0x60 / jae .done` after the release test, or extend both tables to a full 128 bytes of zeros.

## [low/medium] win_create publishes the slot as VISIBLE before coordinates and z-order are initialized (IRQ12 hit-test race)
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:15180-15223 | area:window manager / create ordering
DESC: win_create_stub sets 'mov byte [bx + WIN_OFF_STATE], WIN_STATE_VISIBLE' (15180) BEFORE writing X/Y/W/H (15183-15190) and ZORDER (15223 — dozens of instructions later, after a demote loop and a win_draw_stub call). The slot may contain stale values from a previously destroyed window (win_destroy only clears STATE, 16428). Interrupts are enabled (dispatcher does sti at line 151), and IRQ12's mouse_drag_update → mouse_hittest_titlebar/resize walk window_table checking only STATE==VISIBLE (3845, 3945). A click landing during window creation can hit the half-initialized window at its OLD coordinates/z (possibly stale z=15), starting a drag/close of the wrong window or focusing garbage. Window of vulnerability is small but it is exercised exactly when the user clicks while an app is launching — consistent with 'apps crash/misbehave when launching apps'.
FIX: Initialize all fields (X/Y/W/H, flags, ZORDER, OWNER, title) first and set WIN_OFF_STATE = WIN_STATE_VISIBLE as the LAST store; or bracket the initialization in pushf/cli..popf.

## [low/high] EVENT_WIN_MOVED defined but never posted anywhere
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:10073 | area:event queue / API contract
DESC: EVENT_WIN_MOVED equ 5 (10073) is the only reference to the event in the entire kernel — no post_event call ever emits type 5 (drag finish posts EVENT_WIN_REDRAW instead, 4677). Apps written against the documented event set (docs list WIN_MOVED as a queue event type) that track their window position via WIN_MOVED will never see it and will render with stale coordinates after the user drags their window.
FIX: Post EVENT_WIN_MOVED (DX = handle) from win_move_stub after the position update (near line 18063), or remove the event type from the constants and docs.

## [low/high] Same keystroke is delivered twice: stored in kbd_buffer AND posted as KEY_PRESS event, with no focus filtering on the buffer path
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:609-624 | area:keyboard
DESC: int_09_handler stores every translated key into the legacy 16-byte kbd_buffer (lines 610-616) and then also posts EVENT_KEY_PRESS (619-624). kbd_getchar/kbd_wait_key (APIs 11/12, table lines 7320-7321) read the buffer with no focus check, while API 9 reads the event. Two different tasks can each receive the same physical keystroke (one via API 11, one via API 9), and a background task polling API 11 silently steals keys from the focused app - the exact 'launcher steals keystrokes' problem the comment at line 439-441 says the event path was built to prevent.
FIX: Make kbd_getchar focus-aware (return 0 unless current_task==focused_task or focused_task==0xFF), or stop double-stuffing: only write kbd_buffer when use_bios_keyboard mode needs it, and reimplement kbd_getchar on top of event_get_stub.

## [low/high] Right Alt (E0 38) press/release not tracked - AltGr never registers as Alt modifier
- src:input-events | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:524-533 | area:keyboard
DESC: .handle_extended maps E0 1D/9D to Ctrl press/release but has no case for E0 38/B8: 'test al, 0x80 / jnz .done' discards the release and the make code 0x38 falls through the extended-key compares to '.done'. Right Alt therefore never sets kbd_alt_state (left Alt does at lines 644-649). Consistent (no stuck state) but Alt-based shortcuts fail with the right Alt key. Verified left/right Shift, left Ctrl, E0-Ctrl and left Alt are otherwise tracked symmetrically.
FIX: In .handle_extended add: 'cmp al, 0x38 / je .alt_press / cmp al, 0xB8 / je .alt_release' before the release filter.

## [low/medium] desktop_icon_count counts occupied slots but redraw iterates the first N slots — sparse tables skip trailing icons
- src:memory-allocator | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:15394-15409, 15797-15805 | area:kernel data structures / desktop icons
DESC: desktop_set_icon_stub recomputes the count as the number of slots whose X or Y is nonzero (15399-15404: 'cmp word [si + DESKTOP_ICON_OFF_X], 0 / jne .dsi_has_icon / cmp word [si + DESKTOP_ICON_OFF_Y], 0 ...'), but draw_desktop_region uses that count as a sequential slot bound ('cmp bp, ax / jae .ddr_done', 15802-15805). If slots become non-contiguous (an icon cleared or re-registered out of order), trailing occupied slots beyond index count-1 are never repainted, and a legitimate icon positioned at exactly (0,0) is invisible to the count. Currently masked because the launcher always fills slots 0..n-1 contiguously and grid positions are never (0,0), but any other API-37 client breaks it.
FIX: Drop the occupancy heuristic: add an explicit in-use flag byte to each desktop_icons entry (there is room — entry is 80 bytes with 76 used), have draw_desktop_region loop over all DESKTOP_MAX_ICONS slots skipping unused ones, and delete the X/Y-nonzero counting loop.

## [low/high] Dead ide_read_sector writes sector data to ES:DI but the documented buffer is ES:BX (DI never initialized)
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:14167-14234 | area:disk driver (latent bug found during 8086 scan)
DESC: ide_read_sector's contract is 'Input: EAX = LBA, ES:BX = buffer' (line 14168), but the data transfer is 'mov dx, IDE_DATA / mov cx, 256 / rep insw' (lines 14219-14221) — INSW stores to ES:DI, and DI is never loaded from BX anywhere in the function. If ever called, it would dump 512 bytes at whatever ES:DI happens to be (memory corruption) and leave the intended buffer untouched. Currently dead code: grep shows no call sites (only the definition at 14170), so it does not explain the user's symptoms today. Also 'rep insw' is 186+ and 'mov eax, [esp + 12]' (14186) is 386-only, so this routine is doubly unusable on the 8088 target.
FIX: Either delete ide_read_sector/ide_write-side siblings as dead code, or fix to 8086-compatible PIO: 'mov di, bx' then a 'in ax, dx / stosw / loop' transfer loop, and pass the LBA in DX:AX instead of EAX.


# REFUTED (8)

## [critical/high] boot/stage2_hd.asm: 56 non-8086 sites — 32-bit FAT math plus 386 near-conditional jumps
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\boot\stage2_hd.asm:60-528 | area:8088 compatibility
DESC: 56 CPU-8086 errors. 49 sites use 32-bit registers/dwords for FAT12/16 geometry math: line 60 'mov eax, [es:0x7C00 + 0x1C]', 69-86 'movzx eax, word [reserved_sects] / add eax,[partition_lba] / imul eax, ebx / shr ebx, 4', 154-160 'mov eax,[root_start_lba] / movzx ecx... / push ecx / push eax', 214 'mov eax, [si + 28]', 236-240 cluster-to-LBA math. Additionally lines 164 'jc near .disk_error' and 180 'jz near .not_found' use 386-only 16-bit-displacement conditional jumps (opcode 0F 8x). Hard-disk boot cannot reach the kernel on an 8088.
WHY REFUTED: The instruction-level observations are accurate but the finding is refuted as a bug: the 386+ requirement of the hard-disk boot path is explicit, documented, intentional design — not a latent defect. Evidence: (1) boot/stage2_hd.asm line 5 header comment: "NOTE: This version requires 386+ CPU (uses EAX, EBX, ECX, EDX, movzx, etc.)". (2) docs/bootloader-architecture.md line 221 documents the per-path CPU contract: floppy boot = 8086, HDD/USB boot = "386+ (for now)". 8088/8086 machines are supported via the floppy path (boot/boot.asm contains no 186+/386+ instructions). (3) TODO.md line 31 alrea

## [high/high] apps/browser.asm: 49 non-8086 sites (movzx-heavy)
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\browser.asm:Docs claim 8088 minimum (README.md:201, docs/FEATURES.md:7) but boot/mbr.asm:9, boot/stage2_hd.asm:5 and kernel/kernel.asm:11907-11974 require 386+; browser.asm:83-1142 merely matches that existing baseline. | area:8088 compatibility
DESC: 49 CPU-8086 errors: movzx x27 (line 105 'movzx ax, cl', 208 'movzx dx, byte [scroll_top]', 209 'movzx ax, byte [file_count]'), pusha x9 / popa x9 (first at line 83), shl-imm x4. All 16-bit forms, no 32-bit registers.
WHY REFUTED: The instruction inventory is factually correct but the finding is refuted as a high-severity bug. Verified counts in C:\Users\arin\Documents\Github\unodos\apps\browser.asm: 27 movzx (first at line 105), 9 pusha (first at 83), 9 popa (first at 633), 4 shl-with-immediate>1 (723, 979, 1105, 1142) = exactly 49 non-8086 sites, all 16-bit forms as claimed. However, the implied failure mode (crash on 8086/8088) is unreachable: the system cannot get anywhere near browser.asm on such a CPU. (1) The bootloaders explicitly require 386+ and document it: boot/mbr.asm:9 and boot/stage2_hd.asm:5 say "require

## [high/high] apps/tetrisv.asm: 44 non-8086 sites
- src:cpu8088-compat | C:\Users\arin\Documents\Github\unodos\apps\tetrisv.asm:45-1993 | area:8088 compatibility
DESC: 44 CPU-8086 errors: popa x21 / pusha x19 (lines 45/236/480...), shl-imm x3, movzx x1.
WHY REFUTED: The instruction COUNT is accurate but the finding is wrong as a high-severity bug. Verified inventory in apps/tetrisv.asm: 19 pusha (lines 45,480,799,837,999,1028,1059,1224,1283,1360,1400,1466,1500,1579,1633,1726,1837,1897,1947), 21 popa (236,494,829,911,966,1021,1050,1096,1101,1276,1353,1393,1459,1493,1526,1625,1684,1830,1890,1940,1993), 3 shl-imm (984,988,1916), 1 movzx (1984) = 44 sites, all genuinely 186+/386+ instructions. However, the claimed failure (8088 incompatibility) is UNREACHABLE: (1) kernel/kernel.asm itself contains 123 occurrences of pusha/popa/movzx (movzx is 386+; e.g. line 

## [high/medium] Keyboard events broadcast to any polling task when no window has focus (fullscreen apps lose keys to launcher)
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:10142-10155 | area:event queue / input routing
DESC: event_get_stub's KEY_PRESS filter: 'mov al, [focused_task] / cmp al, 0xFF ; No window focused? Deliver to current task / je .evt_focus_ok' (10146-10148). focused_task is 0xFF whenever no window exists (set in win_destroy at 16461). Fullscreen apps (pacman/tetris VGA variants) create no window, so while they run, every keystroke is delivered to WHICHEVER task polls event_get first — including the launcher, which keeps polling API_EVENT_GET every loop iteration (apps/launcher.asm line 268-269). Keys randomly vanish into the launcher (which may even act on them, e.g. Enter launching another app — plausibly contributing to the 'crash when launching apps' reports). The same file already fixed this exact problem for the BIOS-keyboard fallback path ('Without this, the launcher steals keystrokes meant for the focused app', 10207-10208) but not for the queue path. Related: EVENT_MOUSE has no routing at all — '.evt_not_key: ... Other event types: consume and pass through' (10157-10160) — so mouse events are consumed by a random task; the launcher consumes them even when an app is focused.
WHY REFUTED: The finding misreads both halves of the system. The quoted kernel code (C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:10142-10155) is real, and focused_task=0xFF does mean "deliver to whichever task polls" — but the claimed failure path cannot occur, for two independent reasons.

(1) The fullscreen VGA apps DO create windows. pacmanv.asm:162-175, tetrisv.asm:62-75, and outlastv.asm:~63-75 all call API_WIN_CREATE with a fullscreen frameless window (flags 0x04, comment in tetrisv.asm:62: "Create fullscreen frameless window (prevents launcher from redrawing desktop)"). win_create sets f

## [medium/high] Launcher main loop issues 5+ INT 0x80 syscalls per iteration even when idle, stealing time from foreground apps every scheduler round
- src:performance | C:\Users\arin\Documents\Github\unodos\apps\launcher.asm:apps/launcher.asm:126-174 — steady-state battery is YIELD(128-129) + GET_SCREEN_INFO(132-133) + GET_TASK_INFO(156-157) = 3 syscalls when a fullscreen app runs; mouse/event polling (178-270) is already skipped for fullscreen apps by lines 171-174; the second GET_TASK_INFO (144-145) runs only on an actual mode change | area:launcher / event loop
DESC: Every pass of .main_loop performs API_APP_YIELD (128-129), API_GET_SCREEN_INFO (132-133), API_GET_TASK_INFO twice (144-145 and 156-157), API_MOUSE_GET_STATE (178-179) and API_EVENT_GET (269-270). Each INT 0x80 round-trip costs the full dispatcher path (bitmap tests, caller_ds bookkeeping, flag fixup ~150-300 cycles on a 486, far more on 8088), so the idle launcher burns ~1,000+ cycles of every scheduler rotation; with a foreground app running, the launcher still executes this whole battery once per yield cycle, directly slowing the foreground app on slow CPUs. The screen-mode poll only matters after a mode switch and the task-info calls only after task count changes.
WHY REFUTED: The finding's core claims are materially wrong, though a small kernel-of-truth inefficiency remains. Verified against C:\Users\arin\Documents\Github\unodos\apps\launcher.asm and C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:

1) WRONG: "GET_TASK_INFO twice (144-145 and 156-157)" per iteration. The call at 144-145 is inside the .mode_changed branch, reached only when GET_SCREEN_INFO's returned width/height differ from the cached scr_width/scr_height (lines 134-137). Lines 139-140 update the cache even when the repaint is skipped, so it cannot recur. Steady state = exactly one GET_TASK

## [medium/high] mem_free performs no validation - arbitrary AX zeroes a word anywhere in 0x14000-0x23FFB (kernel code / launcher)
- src:boot-memory-map | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:kernel.asm:10043-10055 (mem_free_stub body); dispatch constraint at kernel.asm:140,359-363 | area:heap allocator
DESC: mem_free_stub (syscall 8) takes AX from the app, only checks for 0, then: 'mov bx, 0x1400 / mov ds, bx / sub ax, 4 / mov bx, ax / mov word [bx+2], 0' (10047-10055). There is no range check (AX can be up to 0xFFFF -> writes at 0x1400:0xFFFD = linear 0x23FFD) and no check that the header's flag word is actually 0xFFFF (allocated). A bad or double free from any app silently zeroes a word inside kernel code or the launcher segment. Because the heap base itself overlaps the kernel (separate finding), even a 'correct' free corrupts kernel memory bookkeeping-wise.
WHY REFUTED: The finding's two load-bearing claims are both false; the headline (no validation) is technically true but its stated impact is not reachable.

WHAT IS TRUE: mem_free_stub (C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:10038-10061) does no range check, no 4-byte-alignment check, and no [bx+2]==0xFFFF check before zeroing the flag word. The cited instructions (lines 10047-10055) are accurate, and the net write lands at offset AX-2 within segment 0x1400.

WHY THE IMPACT CLAIM IS REFUTED: The finding asserts an attacker can supply "arbitrary AX up to 0xFFFF" and thereby zero a word anyw

## [medium/medium] app_exit_stub repaints the desktop with the cursor visible and unprotected (XOR ghost artifacts on app exit)
- src:scheduler-applaunch | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:14974-14998, 16127-16148 | area:app exit / graphics
DESC: On task exit, app_exit_stub resets the lock and SHOWS the cursor (14975-14976 `mov byte [cursor_locked],0 / call mouse_cursor_show`) and then, for windowless/fullscreen apps, calls redraw_affected_windows over the whole screen (14992-14998). redraw_affected_windows (16127+) performs direct kernel drawing (desktop region, icons, window frames) with NO cursor hide/lock of its own - it only saves clip state (16141-16142). In CGA modes the cursor is an XOR sprite: repainting the desktop under a visible cursor erases its XOR trace while cursor_visible stays 1 and cursor_drawn_x/y stay stale, so the next mouse_cursor_hide XORs garbage into the freshly painted desktop, and IRQ12 can concurrently move/redraw the cursor mid-repaint. Result: inverted cursor-shaped artifacts on the desktop after closing fullscreen apps.
WHY REFUTED: The finding's factual premise is wrong: redraw_affected_windows (kernel/kernel.asm:16137) does NOT write any pixels itself, and every drawing routine it invokes is individually protected by the standard hide+lock/unlock+show idiom. The claimed failure path therefore cannot occur.

Verified protections (all in C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm):
1. Desktop background fill: draw_desktop_region (15771) fills via gfx_fill_color, which calls mouse_cursor_hide / inc [cursor_locked] at 15964-15965 before touching the framebuffer and dec / mouse_cursor_show at 16114-16115 after.


## [medium/medium] Close-button kill of another task leaks global cursor_locked/drag state (app_exit path resets it, kill path does not)
- src:window-manager | C:\Users\arin\Documents\Github\unodos\kernel\kernel.asm:4416-4431 | area:window manager / task teardown
DESC: app_exit_stub explicitly repairs global state on self-exit: 'mov byte [draw_context], 0xFF ... mov byte [cursor_locked], 0 / call mouse_cursor_show' (14971-14976). The deferred close-button path that kills a DIFFERENT task (mouse_process_drag .close_kill, 4416-4431) frees the app slot/segment and destroys its windows but never resets cursor_locked. win_begin_draw increments cursor_locked and only win_end_draw decrements it (1367-1368, 1410-1413); the per-task switch code saves draw_context/font but not cursor_locked (15011-15028). If the victim task is suspended inside a begin_draw/end_draw bracket (it yielded or polled events mid-draw) when the user clicks its [X], cursor_locked stays >0 forever and mouse_cursor_show never draws again (3779-3780) — permanently invisible mouse cursor. The kill path also leaves drag_active/resize_active untouched if the victim's window was mid-drag.
WHY REFUTED: The central claim — that killing a suspended task via the close-button path (.close_kill, kernel/kernel.asm:4416-4431) leaks the victim's cursor_locked increment and permanently hides the mouse cursor — is wrong, because cursor_locked is NOT a cross-task accumulator. The auditor missed the Build 397 reset in the context-switch code.

Key evidence:

1. app_yield_stub (the only way a task becomes suspended) explicitly zeroes cursor_locked on EVERY task switch, kernel.asm:14813-14816: "; Reset cursor state before switching tasks (Build 397) / ; Previous task may have cursor_locked > 0 from win_be
