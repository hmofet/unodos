# UnoDOS pc64 - storage stack + code/module loader review (2026-07-20)

Threat model: freestanding C, ring0, no memory protection. FS contents and `.UNO`
modules are semi-trusted→untrusted. The module CRC32 is an **integrity** check only
- a crafter computes a matching CRC trivially, so every header field is
attacker-controlled once magic/abi/size/crc are consistent.

## CRITICAL

### C1. Module loader: `mem_size` unvalidated → page-count overflow → massive OOB memset/memcpy
**`pc64/pc64_modload.c:335,:340,:348-349` - `mod_instantiate()`. Confirmed.**
Reachable from every `.UNO` load. Validation bounds `file_size` but never bounds
`h->mem_size`; only `entry >= mem_size` and `file_size > mem_size` checked.
```c
np = (h->mem_size + 4095u) >> 12;                           // :340 32-bit add, overflows
memcpy(base, gModBuf + sizeof *h, h->file_size);            // :348
memset(base + h->file_size, 0, h->mem_size - h->file_size); // :349
```
`mem_size=0xFFFFFFFF`: `(0xFFFFFFFF+4095u)` wraps to `0x00000FFE`, `>>12 → np=0`.
`mod_alloc(0)` returns the arena pointer; memset then zero-writes ~4 GB. Values in
`>0xFFFFF000` make `memcpy` overrun an undersized alloc.
**Fix:** `if (h->mem_size < h->file_size || h->mem_size > (unsigned)MODBUF_MAX*8u)
return 0;` and compute `np` in 64-bit.

### C2. Module loader: relocation-RVA bounds check defeated by 32-bit overflow → 8-byte OOB write
**`pc64/pc64_modload.c:353-357`. Confirmed.** Independent of C1.
```c
if (rel[i] + 8u > h->mem_size) { ... return 0; }            // :354 rel[i]+8u overflows
*(unsigned long long *)(base + rel[i]) += (u64)base - h->pref_base;  // :355
```
`rel[i]=0xFFFFFFF8, mem_size=0x10000`: `rel[i]+8u` wraps to 0 → passes; writes 8
bytes at `base+0xFFFFFFF8`. Arbitrary-offset OOB write. Same class at :363 (imports).
**Fix:** `if (rel[i] > h->mem_size - 8u) return 0;`; rewrite :363 as subtraction and
bound `imp_count`.

### C3. unofs: `fat_get`/`fat_set` unbounded cluster index → OOB read + write on crafted FAT12
**`unofs/unofs_core.c:17-29`, via `:124,:128-134,:222-223,:255`. Confirmed vs
shipped geometry.** `fs->fat` is `FAT12_SECTORS_PER_FAT*512` = 9*512 = 4608 bytes.
Chain walks accept any on-disk `2<=cl<EOC(0xFF8)`. For `cl=0xFF7`, offset =
`0xFF7 + 0x7FB = 6130` → reads `fs->fat[6130]/[6131]`, ~1.5 KB past the buffer.
`unofs_read` = OOB read; `unofs_delete:255` / `unofs_write:222-223` call `fat_set` =
**OOB heap write** (2 bytes).
**Fix:** clamp in accessors: `maxcl = fs->fat_bytes*2/3; cl<2||cl>=maxcl` = end-of-
chain; validate `dirent.start_cluster` on open.

## HIGH

### H1. Installer: `num*esz` GPT multiply overflows the buffer guard → OOB read; unclamped HeaderSize CRC
**`pc64/installer.c:229-240`; also `:603-605,:611-627`. Confirmed (boot-medium GPT).**
`g_gpt`=16896 B. `num=0x10000, esz=0x10000` → `num*esz` overflows to 0, passes
`0>16384`; loop indexes past the buffer. `crc32(hdr, rd32(hdr+12))` uses source
HeaderSize unclamped.
**Fix:** validate `esz∈[128,512]` multiple-of-8, `num<=(sizeof g_gpt-512)/esz`,
64-bit multiply, clamp HeaderSize to `[92,512]`.

### H2. FAT: BPB TotalSectors underflow → `v->clusters`≈4 billion → `fat_alloc` scan hangs
**`pc64/fat.c:117-118` (`mount_at`), consumed at `:292-300`. Confirmed.**
```c
data_sectors = tot - (resv + nfats*fatsz + rootdir_sectors); // :117 unsigned underflow
v->clusters  = data_sectors / v->sec_per_clus;               // :118
```
`tot` < reserved region → `data_sectors` underflows to ~0xFFFFFFxx → `v->clusters`
≈4e9; first write → `fat_alloc` scans ~4 billion iters → effective hang.
**Fix:** reject if `tot < resv+nfats*fatsz+rootdir_sectors`, or clusters exceed FAT
addressing, or `data_start+clusters*spc > dev->sectors`.

## MED

- **M1. FAT: disk cluster values never bounded vs `v->clusters`; `clus_lba` overflow.**
  `fat.c:288-289,:347-349,:592-616,:302-310`. Disk-bounded (lands in the 512-byte
  cache line), but `(clus-2)*sec_per_clus` overflows uint32 → reads wrong sector
  (cross-file info leak). Fix: add `clus < v->clusters+2` checks.
- **M2. FAT: 32-bit LBA fields truncate volumes beyond 2 TiB.** `fat.c:31-33,:112-116`.
  Fix: widen volume LBA fields to `uint64_t`.
- **M3. FAT: `fat_alloc` O(clusters) scan per cluster → O(n^2) large-file writes.**
  `fat.c:292-300`. Fix: per-volume next-free cursor (FSInfo hint).
- **M4. Storage drivers: single-block PIO + no controller reset after timeout.**
  `sdhci.c:277-295` one 512 B block/command. `ahci.c:116-123`, `nvme.c:102-104`
  fail on timeout but don't reset the port / resync `gCqH`/`gPhase` → a late
  completion wedges later I/O (bounded-fail, not hang). SDHCI `recover()` is the
  pattern to mirror.
- **M5. Module loader: `p[64]` path assembly has no length guard on module name.**
  `pc64_modload.c:236-243,:401-408`. Safe for the fixed roster; overflows if a
  future caller routes a long name. (Suspected - not currently reachable.)

## Checked and OK
- Modload size equation `:333` uses `4ll*nreloc` in 64-bit - correct.
  `uno_mod_load_pyapp:449-463` bounds file_size before CRC.
- FAT dir parsing iterates 32-byte records strictly inside the 512-byte line; **LFN
  entries are skipped, not reassembled → no LFN overflow buffer exists.**
- `pc64_write.c` UWD parse (`:599-635`) clamps `tl` before memcpy.
- Installer excludes boot medium by device-path prefix, gates whole-disk on
  512B/sector+size+UI double-confirm.
- NVMe/AHCI enumeration bounded by `MAXNS`/`MAXDRV`; `blkdev.c` dedups controllers.
