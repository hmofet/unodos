# UnoDOS pc64 — Behavioral Specification (SPEC)

This document is the testable behavioral contract for the pc64 port (UEFI
x86-64) plus the shared `../unoui` toolkit. It drives two consumers:

- **Runtime asserts** — contracts tagged `[assert]` name the code site that
  should enforce them (`uno_dbg_panic`/`uno_dbg_note` in debug builds).
- **SPECTEST** — a boot-runnable conformance test that executes every
  contract tagged `[auto]` on bare metal and writes one line per contract,
  `S-XXX-NN PASS` / `S-XXX-NN FAIL <detail>`, to `CRASH\<MACHINE>\SPECTEST.TXT`
  (same SMBIOS machine-tag folder the debug harness uses).

**Numbering**: `S-<AREA>-<NN>`. Numbers are stable once assigned; retired
contracts keep their number with a `RETIRED` note rather than being reused.

**Tags** (exactly one per contract):
- `[auto]` — verifiable by an in-OS self-test with no human present.
- `[assert]` — best enforced as a runtime assert at a named code site.
- `[manual]` — needs a human or specific physical hardware (looks/sounds
  right, real link partner, real panel).

**Language**: MUST / NEVER statements only. `check:` lines are hints for the
SPECTEST author, not normative. `NOTE:` lines flag places where current code
is suspected to violate the intended contract; all such notes are collected
in the final section, **Suspected divergences**.

Areas: BOOT, FB, FONT, UUI, SHELL, FAT, FS, BLK, USB, INP, NET, NIC, WIFI,
TLS, HTTP, SND, ACPI, MOD, DBG, STRESS, NTST, WRITE, FILES, MUSIC, CLOCK,
BROWSER, JS, STUDIO, PHOTOS, PAINT, GAME, INST, MAC, PY, LIBC.

---

## S-FAT — native FAT16/32 driver (`fat.c`)

- **S-FAT-01** [auto] `uno_fat_init()` MUST be idempotent: a second call
  MUST NOT rescan disks or change `uno_fat_volumes()`.
  check: call twice, compare volume count and serials.
- **S-FAT-02** [auto] `mount_at` MUST reject a BPB whose bytes-per-sector is
  not 512, whose sectors-per-cluster is 0, whose FAT count is 0, or whose
  sector lacks the 0x55AA signature — the volume MUST NOT appear in the table.
  check: RAM-backed fake bdev with each corruption; expect volume count 0.
- **S-FAT-03** [auto] `mount_at` MUST reject a crafted BPB whose TotalSectors
  is ≤ the metadata span (reserved + FATs + root) — the unsigned underflow to
  ~4G clusters MUST NOT occur (`fat.c` guard at the `tot <= meta` check).
- **S-FAT-04** [auto] `mount_at` MUST reject volumes classifying as FAT12
  (non-FAT32 with < 4085 clusters) and volumes with > 0x0FFFFFF6 clusters.
- **S-FAT-05** [auto] Partition discovery per disk MUST probe in the order
  GPT (0xEE at MBR entry 0 + "EFI PART" at LBA 1) → MBR type codes
  {0x01,0x04,0x06,0x0B,0x0C,0x0E,0xEF} → superfloppy (BPB at LBA 0), and MUST
  stop at the first scheme that yields any mount.
- **S-FAT-06** [assert] The volume table MUST NEVER exceed MAXVOL (8);
  `mount_at` MUST return 0 rather than write past `g_vol`.
  site: `fat.c mount_at` (`g_nvol >= MAXVOL` early-out).
- **S-FAT-07** [auto] `uno_fat_label`/`uno_fat_serial` MUST return ""/0 for
  any out-of-range volume index (negative or ≥ volume count).
- **S-FAT-08** [auto] The sector cache MUST hold exactly CH_N (8) lines whose
  512-byte buffers are 128-byte aligned (EFI IoAlign + AHCI DMA requirement);
  SPECTEST MUST verify `((uintptr)buf & 127) == 0` for every line.
- **S-FAT-09** [assert] Evicting a dirty cache line MUST write it to disk
  before reuse (`cache_flush_line` before repopulation in `cache_get`); a
  dirty line MUST NEVER be silently discarded except via `uno_fat_remount`.
- **S-FAT-10** [auto] A failed device read in `cache_get` MUST invalidate the
  line and propagate failure (callers return 0/-1/short); it MUST NEVER
  serve stale or uninitialized buffer contents.
- **S-FAT-11** [auto] Every cluster-chain walk (read, free, directory cursor)
  MUST terminate within MAXCLUS_WALK (0x100000) steps even on a cross-linked
  or circular FAT; a crafted loop image MUST NOT hang the machine.
  check: build a 2-cluster cycle in a RAM image; read must return.
- **S-FAT-12** [auto] `uno_fat_write` MUST make the file durable before
  returning 1: it ends with `cache_sync()` + `cache_drop()`, so a read-back
  through a fresh cache (or another consumer) sees exactly `len` bytes equal
  to the input. check: write N random bytes, read back, memcmp — the
  canonical FAT round-trip test, run for len ∈ {0, 1, 511, 512, 513, one
  cluster, cluster+1, ~1 MB}.
- **S-FAT-13** [auto] Overwriting an existing file MUST free the old cluster
  chain (FAT entries return to 0) and reuse the directory entry — repeated
  overwrite of one path MUST NOT leak clusters.
  check: write/overwrite 100×, then count free FAT entries — stable.
- **S-FAT-14** [auto] On allocation failure mid-write (disk full),
  `uno_fat_write` MUST return 0 and free the partial chain it built.
- **S-FAT-15** [assert] A freshly created directory entry MUST be marked
  dirty in the cache immediately after the name is stamped, BEFORE any
  `fat_alloc` scan can evict it (the documented eviction trap in
  `uno_fat_write` / `uno_fat_mkdir`); losing the name produces an invisible
  file + leaked chain. site: `fat.c uno_fat_write` (`cache_put(c)` after
  name memcpy).
- **S-FAT-16** [auto] `uno_fat_write` on a read-only device (`dev->write ==
  0`) MUST return 0 and change nothing; same for delete/mkdir/rename.
- **S-FAT-17** [auto] `uno_fat_read_at` MUST return -1 for negative offset or
  missing file, 0 at/past EOF, and MUST clamp reads to `size - off`; it MUST
  NEVER return bytes past the directory-entry size even if the chain is
  longer.
- **S-FAT-18** [auto] Sequential streaming MUST be O(1) per continuation:
  after `uno_fat_read_at(v,p,off,...)`, a following read at `off' ≥ off` of
  the same vol+path resumes from the cached cursor rather than rewalking the
  chain from cluster 0. check: time 1000 sequential 4 KB reads of a multi-MB
  file vs 1000 alternating-file reads; sequential must not be quadratic.
- **S-FAT-19** [auto] Any write, delete, rename, or remount MUST invalidate
  the sequential-read cursor (`uno_fat_seq_flush`); a stale cursor MUST NEVER
  serve bytes from a moved chain. check: stream half a file, overwrite it,
  continue streaming — bytes must match the NEW content.
- **S-FAT-20** [auto] `pack83` MUST uppercase names, pad to 8+3, and reject a
  name whose packed first byte is a space; path walking accepts both `\` and
  `/` separators and skips leading separators.
  NOTE: current code may violate the intent for over-length components —
  `fat.c pack83` silently truncates >8-char stems / >3-char extensions, so
  "LONGNAME1.TXT" and "LONGNAME2.TXT" alias to the same 8.3 entry; intended
  behavior is to reject over-length components.
- **S-FAT-21** [auto] `uno_fat_delete` MUST return 1 only when the entry
  existed; it marks the entry 0xE5, frees the full chain, and syncs — a
  subsequent `uno_fat_size` of that path MUST return -1.
- **S-FAT-22** [auto] `uno_fat_mkdir` MUST fail (return 0, touch nothing
  visible) when the entry already exists, when the parent is missing, or on
  disk-full — the disk-full path MUST un-create the just-allocated slot.
  On success the new directory MUST contain valid "." (self) and ".."
  entries, with ".." cluster 0 when the parent is the root (FAT spec).
- **S-FAT-23** [auto] `uno_fat_rename` MUST reject a `newname` containing a
  path separator, MUST fail when the target name already exists in that
  directory, and MUST NOT move data clusters (only the 11-byte name changes).
- **S-FAT-24** [auto] `uno_fat_list` MUST skip deleted (0xE5), LFN
  (attr 0x0F), volume-label, and subdirectory entries; `uno_fat_list_ex`
  MUST include subdirectories, skip "." / "..", write at most `maxn` entries,
  and return the TOTAL count (which may exceed `maxn`).
- **S-FAT-25** [auto] `uno_fat_selftest` MUST be inert when no `WRTEST.REQ`
  exists on any writable volume; when armed it MUST write `WRTEST.OK`
  containing the request text + `[native-fat-rw]` and then delete
  `WRTEST.REQ`, returning the number of volumes hit.
- **S-FAT-26** [auto] `uno_fat_native_eligible` MUST return 1 only when a
  mounted volume (a) sits on a controller of PCI class AHCI/NVMe/SDHCI that
  a native driver binds AND (b) carries a UnoDOS system marker
  (`EFI\UNODOS\BOOTX64.EFI` or `EFI\BOOT\BOOTX64.EFI` beginning "MZ"); a
  foreign FAT partition alone MUST NEVER enable detach.
- **S-FAT-27** [auto] `uno_fat_sync` MUST flush every dirty cache line;
  `uno_fat_remount` MUST drop all cache lines WITHOUT flushing (the old
  transport is gone), reset the volume table, and rescan the current device
  set.

- **S-FAT-28** [auto] Overwriting an existing file MUST NOT lose the old
  contents when the disk fills: the directory entry must reference either the
  complete old chain or the complete new chain, never freed clusters.
  NOTE: current code may violate this — `fat.c uno_fat_write` frees the old
  chain BEFORE allocating the new one; on ENOSPC it returns 0 with the entry
  still pointing at now-free clusters (dangling chain, stale size). Intended:
  allocate+write new chain first, then swap and free.
- **S-FAT-29** [auto] `uno_fat_write` and `uno_fat_delete` MUST refuse a path
  that resolves to a DIRECTORY entry (attr 0x10).
  NOTE: current code may violate this — `uno_fat_write` performs no attribute
  check after `dir_find`, so writing a file over a subdirectory name frees the
  directory's own cluster chain and rewrites the entry as a file (silent
  directory destruction).
- **S-FAT-30** [auto] Writing into a full FAT16 SUBDIRECTORY MUST grow its
  cluster chain (only the FAT16 fixed root is legitimately un-growable).
  NOTE: current code may violate this — `fat.c dir_alloc_slot` gates the
  grow branch on `v->fat32`, so full FAT16 subdirs fail creation.
- **S-FAT-31** [auto] Volume metadata addressing MUST either use 64-bit LBAs
  or refuse to mount a partition whose data region extends past 2^32 sectors.
  NOTE: current code may violate this — `fat_start`/`data_start`/`clus_lba`
  are computed in `uint32_t` from `(uint32_t)start`, mis-addressing volumes
  beyond 2 TiB; the `data_start >= dev->sectors` sanity check compares the
  truncated value.
- **S-FAT-32** [auto] A transient sector-read failure while walking a chain
  MUST surface as an error/short-read, NEVER as a clean end-of-chain.
  NOTE: current code may violate this — `fat.c fat_get` returns the EOC
  sentinel when `cache_get` fails, so an IO error silently truncates the file.
- **S-FAT-33** [auto] A FAT entry pointing outside [2, clusters+2) that is
  not an EOC value MUST terminate the walk (treated as corruption), not
  index FAT sectors out of range; today only the MAXCLUS_WALK guard bounds
  this (`dir_next` checks `next < 2` and EOC but no upper bound).
- **S-FAT-34** [auto] The sequential cursor MUST only ever hit on an exact
  vol+path match. NOTE: current code truncates the stored path at 79 chars
  (`g_seq.path[80]`), so two paths sharing a 79-char prefix could
  false-match; paths that long do not occur with 8.3 components but the
  compare should still be exact.

## S-BLK — block-device layer (`blkdev.c`, `ahci.c`, `nvme.c`, `sdhci.c`)

- **S-BLK-01** [assert] The device registry MUST NEVER exceed MAXBLK (12);
  `uno_blk_register` returns 0 when full. site: `blkdev.c uno_blk_register`.
- **S-BLK-02** [auto] `uno_blk_get` MUST return NULL for out-of-range
  indices; every registered device MUST expose 512-byte sectors and a
  non-NULL `read`.
- **S-BLK-03** [auto] While firmware-attached, `uno_blk_init` MUST NOT bring
  up native AHCI/NVMe/SDHCI (the firmware owns the controllers; reprogramming
  them corrupts firmware Block IO mid-write) — attached storage goes through
  `fw_scan` wrappers only (exception: `UNO_SDHCI_TEST` QEMU builds).
- **S-BLK-04** [auto] `fw_scan` MUST wrap only whole-disk handles
  (`LogicalPartition == 0`), present media, BlockSize 512, and MUST skip any
  disk whose controller PCI dev/fn a native driver already claimed.
- **S-BLK-05** [auto] `fw_write` MUST refuse (return 0) when the firmware
  media is marked read-only.
- **S-BLK-06** [auto] After `uno_blk_detach` the registry MUST contain only
  native devices (firmware Block IO handles are dead post-ExitBootServices);
  callers MUST follow with `uno_fat_remount()`.
- **S-BLK-07** [auto] `dp_pci` (device-path walk) MUST terminate within its
  64-node guard on a malformed device path (length < 4 or missing
  end-node).
- **S-BLK-08** [manual] Native AHCI/NVMe/SDHCI reads of a sector written
  pre-detach through firmware Block IO MUST return identical bytes
  (same-disk transport equivalence) — verified on metal via the detach
  storage round-trip.

- **S-BLK-09** [auto] AHCI MUST bind by PCI class 01/06 via BAR5, run
  single-slot polled commands (PxIE=0, PRDTL=1), transfer at most
  CHUNK_SECT (128) sectors per command, and bound every wait by SPIN_MAX —
  a dead port MUST fail the probe, never hang boot.
- **S-BLK-10** [auto] AHCI port bring-up MUST require device-present
  (PxSSTS.DET==3) and a SATA-disk signature (PxSIG==0x00000101, no ATAPI);
  command error paths (TFES, CI timeout, TFD.ERR) MUST return 0 to the
  caller.
- **S-BLK-11** [manual] A write acknowledged by the storage stack MUST be
  durable across power-off once `uno_fat_sync` has run.
  NOTE: current code may violate this — neither `ahci.c` (no FLUSH CACHE
  0xE7) nor `nvme.c` (no Flush opcode 0x00) issues a drive cache flush after
  writes, so a volatile write cache can lose synced telemetry on power cut.
- **S-BLK-12** [assert] AHCI SHOULD set FRE and wait for PxCMD.FR before
  setting ST (spec ordering). NOTE: current `ahci.c` sets SUD|POD|FRE then
  ST immediately — tolerated polled, latent on slow HBAs.
- **S-BLK-13** [auto] NVMe MUST bind by PCI class 01/08, use one admin + one
  I/O queue pair (QDEPTH 16) with correct CQE phase tracking, honour the
  doorbell stride from CAP.DSTRD, and register only 512-byte-LBA namespaces
  (LBADS==9); inactive namespaces (identify fail / NSZE 0) MUST be skipped.
- **S-BLK-14** [auto] NVMe PRP construction MUST be exact: ≤1 page → PRP1
  only; ≤2 pages → PRP2 = second page; else PRP2 → PRP list whose entries
  are page-aligned continuations; transfer size MUST respect MDTS.
- **S-BLK-15** [assert] An NVMe command timeout MUST NOT leave the
  submission queue desynchronized. NOTE: current `nvme.c` returns 0 on
  timeout with the SQ tail/doorbell already advanced and no reset path —
  subsequent commands mismatch phase. site: `nvme.c` completion wait.
- **S-BLK-16** [assert] The NVMe Set-Features (Number of Queues) result MUST
  be checked before creating the I/O queues. NOTE: current code ignores the
  admin status. site: `nvme.c` init.
- **S-BLK-17** [auto] SDHCI MUST bind by PCI class 08/05, probe SD first
  (CMD8 echo 0x1AA + ACMD41 ≤1000 loops) then MMC (CMD1 ≤1000 loops),
  derive capacity from CSD (v2 SD: (C_SIZE+1)*1024; MMC 0xFFF → EXT_CSD
  SEC_COUNT), refuse a zero-sector medium, and issue CMD16 for
  byte-addressed cards.
- **S-BLK-18** [auto] SDHCI byte-addressed argument math MUST NOT truncate:
  `lba*512` computed in 64-bit for any card the driver accepts.
  NOTE: current `sdhci.c` computes `(uint32_t)(lba*SECT)` — wraps past 4 GB
  on byte-addressed media.
- **S-BLK-19** [auto] All three native drivers MUST be idempotent
  (`*_init` re-callable; `sdhci` re-registers the live medium on the M3
  re-init) and MUST respect the global MAXBLK cap when registering.
- **S-BLK-20** [assert] Firmware-handle dedup MUST not double-register a
  disk. NOTE: current `blkdev.c fw_scan` dedups only when the device path
  yields a PCI node (`pdev >= 0`); a handle without one can double-register
  a natively-claimed disk (masked downstream by the FAT-serial dedup in
  `pc64_fs.c`).

## S-FS — unified filesystem facade (`pc64_fs.c`)

- **S-FS-01** [auto] Volume 0 MUST always exist and be the RAM disk
  (`uno_fs_volumes() >= 1`, `uno_fs_kind(0) == 0`).
- **S-FS-02** [auto] `uno_fs_kind` MUST return -1 for out-of-range volumes,
  and one of {0 RAM, 1 native FAT, 2 firmware SFS} otherwise;
  `uno_fs_fat_index` MUST return a valid `fat.c` index iff kind == 1, else
  -1.
- **S-FS-03** [auto] `uno_fs_read`/`uno_fs_size`/`uno_fs_read_at` MUST agree:
  read of a file returns exactly `min(max, uno_fs_size())` bytes;
  `uno_fs_read_at` returns 0 at/past EOF and -1 for a missing file.
- **S-FS-04** [auto] `uno_fs_write` MUST return 0 (and change nothing) on a
  volume where `uno_fs_writable()` is 0, and 1 with a byte-exact read-back
  where it is 1. check: RAM-disk round trip is always runnable.
- **S-FS-05** [auto] `uno_fs_list_begin`/`uno_fs_list_get` MUST snapshot:
  `get(i)` for `0 ≤ i < begin()` fills a NUL-terminated name and returns
  success; out-of-range i fails without UB.
- **S-FS-06** [auto] `uno_fs_remap` MUST rebuild the volume map after the
  block-device set changes (detach), preserving invariant S-FS-01.

- **S-FS-07** [auto] A FAT volume reachable both natively and through
  firmware SFS MUST appear once. NOTE: current `pc64_fs.c` dedups by BPB
  serial and skips the check when the serial is 0 — a zero-volume-id disk
  lists twice; intended: fall back to a secondary key (label + size).
- **S-FS-08** [auto] After detach, stale firmware-SFS volumes MUST answer
  -1/0 on every call (`fw_dead` guard), including the cached listing.
  NOTE: `uno_fs_list_get` serves the pre-detach snapshot cache without a
  `fw_dead` check until remap — reads-after-detach see a stale listing.
- **S-FS-09** [auto] `uno_fs_list_begin` MUST report truncation somehow
  detectable by the caller (count > cache capacity). NOTE: current code
  silently caps the snapshot at 64 names of ≤12 chars.

## S-FB — software framebuffer (`fb.c`, `fb.h`)

- **S-FB-01** [auto] Every drawing primitive MUST clip to the intersection of
  the active clip window and the screen: for any (x,y,w,h) including
  negative origins, w/h ≤ 0, and coordinates past the edges, no write may
  land outside `fb[0 .. FB_W*FB_H)`. check: guard-band pattern around a
  sub-rect; draw pathological rects with clip set; verify guard intact.
- **S-FB-02** [auto] `clip()` MUST be overflow-safe: rect sums are computed
  in 64-bit, so a module passing x ≈ INT_MAX with large w MUST NOT wrap into
  an accepted rect (long is 32-bit on this LLP64 target).
- **S-FB-03** [auto] `fb_set_clip`/`fb_reset_clip`/`fb_get_clip` MUST
  round-trip; the default clip is the whole screen and callers that never
  touch it are unaffected.
- **S-FB-04** [auto] `fb_blit` MUST reject NULL src, stride ≤ 0, w/h ≤ 0, and
  MUST offset INTO src by exactly the clipped left/top cut so the visible
  pixels are the same ones an unclipped blit would have placed there.
  check: blit a numbered gradient half off-screen; verify visible half.
- **S-FB-05** [auto] `fb_pixel` and `fb_blend_pixel` MUST honour the clip
  window; `fb_blend_pixel(a≤0)` is a no-op and `a≥255` a plain store.
- **S-FB-06** [auto] Pixel format MUST be 0xAABBGGRR (R low byte);
  `FB_RGB(r,g,b)` sets alpha 0xFF; `fb_invert_rect` XORs 0x00FFFFFF (RGB
  only), so double-invert restores the original pixel exactly.
- **S-FB-07** [auto] `blend_px` MUST be exact at the endpoints: a=0 keeps dst,
  a=255 yields src, and `div255` equals round(x/255) for all x in 0..65025.
  check: exhaustive div255 loop is cheap at boot.
- **S-FB-08** [auto] `fb_grad_v` MUST interpolate over the ORIGINAL band
  height even when clipped: clipping the top rows off must not change the
  colours of the surviving rows.
- **S-FB-09** [auto] `fb_round_rect_a` MUST clamp rad to min(w/2, h/2) and
  treat rad<0 as 0; its three body rects plus four corners MUST cover the
  rect exactly once (no double-blend seams at a<255).
  check: blend at a=128 over white; scan for seam rows/cols.
- **S-FB-10** [auto] `fb_glyph` (bitmap path) MUST map any codepoint outside
  [UNO_FONT_FIRST, UNO_FONT_FIRST+UNO_FONT_COUNT) to ' ', clip per pixel, and
  return x+8; bg < 0 MUST leave background pixels untouched.
- **S-FB-11** [auto] With no provider registered, `fb_text_w(s) ==
  8*strlen(s)` and `fb_text_h() == 8`; with a provider, `fb_text` MUST route
  through `provider->text` when non-NULL else the per-glyph loop, and
  measurement (`text_w`) MUST match what `text`/`glyph` advances.
- **S-FB-12** [auto] `fb_set_font(NULL)` MUST restore the built-in bitmap
  font (provider deregistration is total — draw, measure, and height all
  fall back together).
- **S-FB-13** [auto] `fb_width()`/`fb_height()` MUST equal the current
  runtime desktop size (`uno_fb_w`/`uno_fb_h`) and never exceed
  FB_MAX_W×FB_MAX_H (1920×1200).
- **S-FB-14** [auto] `fb_big_text` MUST advance 8*scale per glyph and render
  each font pixel as a scale×scale block through the clipped
  `fb_fill_rect` path (it never touches the TTF provider).

## S-NET — TCP/IP core (`net.c`, `net.h`)

- **S-NET-01** [auto] `net_init` MUST fully reset stack state: ARP cache,
  UDP queues, the TCP block, DHCP state, and ticks — a re-init after a NIC
  swap MUST NOT leak connections or cached MACs.
- **S-NET-02** [auto] With no NIC (`g_nic == NULL`), `net_poll` and
  `net_link` MUST be safe no-ops (link reports 0).
- **S-NET-03** [auto] Static defaults MUST match QEMU SLIRP
  (10.0.2.15/24, gw 10.0.2.2, dns 10.0.2.3) so the stack works with no DHCP
  under `-netdev user`.
- **S-NET-04** [auto] The ARP cache MUST hold ARP_N (8) entries, update in
  place on a known IP, fill a free slot else evict slot 0; `net_arp_resolve`
  of an off-subnet IP MUST resolve the GATEWAY's MAC instead, and MUST kick
  a request (returning 0) when unknown.
- **S-NET-05** [auto] The stack MUST answer an ARP request for MYIP with a
  correctly-addressed reply, and MUST learn the requester's mapping.
- **S-NET-06** [auto] `ip_recv` MUST clamp the IP total-length field to the
  received frame length (attacker-controlled `tot` must never drive an L4
  parse past the RX buffer) and MUST drop packets with header length < 20 or
  beyond the frame.
- **S-NET-07** [auto] Unicast packets not addressed to MYIP MUST be dropped
  (no promiscuous L4) except limited broadcast and any packet while DHCP is
  mid-handshake (no IP yet).
- **S-NET-08** [auto] ICMP echo requests MUST be answered with the payload
  echoed (truncated to 64 bytes) and a correct checksum; `net_ping` /
  `net_ping_replied` MUST match on the 0x4944 id, and a ping sent before ARP
  resolution MUST auto-retry from `net_poll` once ARP answers.
  NOTE: current code may violate memory-safety intent — `net.c net_ping`
  stores the caller's `ip` POINTER in `g_ping_dst` for the retry; a caller
  passing a stack buffer that dies before ARP resolves makes the deferred
  retry read freed stack. Intended: copy the 4 bytes.
- **S-NET-09** [auto] UDP payloads MUST be limited to 512 bytes on send
  (silent truncation is the documented behavior) and receive (excess
  dropped); `net_udp_recv` MUST return 0 when nothing is queued and MUST
  clear the one-deep queue slot on delivery.
- **S-NET-10** [auto] A UDP datagram for a bound port MUST fill src IP and
  src port; datagrams for unbound ports MUST be dropped; port 68 MUST always
  route to `dhcp_input`.
- **S-NET-11** [auto] UDP checksums MUST be computed over the IPv4
  pseudo-header, with 0 transmitted as 0xFFFF per RFC 768.
- **S-NET-12** [auto] TCP is single-connection by contract: `net_tcp_connect`
  resets the block, sends SYN, and enters SYN_SENT; the state machine is
  exactly {CLOSED, SYN_SENT, ESTABLISHED, FIN_WAIT, DONE}; RST from the peer
  MUST force DONE from any non-CLOSED state.
- **S-NET-13** [auto] `net_tcp_send` MUST refuse (-1) unless ESTABLISHED with
  no unacked segment outstanding (one-in-flight discipline), cap payload at
  512, and advance snd_nxt by exactly len.
- **S-NET-14** [auto] Unacked SYN or data MUST be retransmitted every 30
  ticks of `net_poll` with the ORIGINAL sequence number until acked
  (`tcp_tick` seq arithmetic must leave snd_nxt unchanged).
  check: loopback NIC that drops the first transmission.
- **S-NET-15** [auto] In-order data MUST be appended to the rxq (8192 B) and
  only the bytes actually stored ACKed — `rcv_nxt` MUST never advance past
  the queue cut, so a truncated tail is recovered from the peer's retransmit
  (partial-overlap segments are trimmed at rcv_nxt). `net_tcp_recv` MUST
  drain FIFO with partial reads preserved (memmove of the remainder).
  Beyond-rcv_nxt segments MUST be ignored (no reassembly by design); stale
  duplicates re-ACKed. FIXED 2026-07-21 (was: full-dlen ACK lost the tail).
- **S-NET-16** [auto] `net_tcp_close` in ESTABLISHED MUST send FIN|ACK and
  enter FIN_WAIT; a received FIN MUST be ACKed with rcv_nxt+1 and the state
  set to DONE.
- **S-NET-17** [auto] DHCP MUST follow DISCOVER→OFFER→REQUEST→ACK with the
  fixed xid 0x554E4F21, sending via broadcast Ethernet + 0.0.0.0 source
  (never via ARP), and on ACK MUST install yiaddr, option 3 (gw) and option
  6 (dns).
  NOTE: current code may violate liveness intent — there is NO
  retransmit/timeout for a lost DISCOVER/OFFER/REQUEST (`net.c` has no dhcp
  timer), so one dropped packet stalls `net_dhcp_done()` forever. Intended:
  periodic re-DISCOVER while unbound.
- **S-NET-18** [auto] `dhcp_input` MUST ignore replies whose xid mismatches,
  non-BOOTREPLY ops, and option walks MUST stay within the received length.
- **S-NET-19** [auto] `net_dns_query` MUST encode labels (rejecting any label
  > 63 bytes), retry up to 4 sends with a bounded (~1 s each) synchronous
  wait, parse compressed names without running off the buffer, and return 1
  only for a type-A rdlength-4 answer.
  NOTE: intended behavior is to also verify the DNS transaction ID of the
  response; current `net.c net_dns_query` accepts any datagram arriving on
  the query port.
- **S-NET-20** [auto] `net_poll` MUST drain the NIC RX queue to exhaustion
  each call, dispatch by EtherType (0x0806 ARP / 0x0800 IPv4), and ignore
  runt frames (< 14 bytes).
- **S-NET-21** [auto] All wire fields MUST be big-endian via the rd16/rd32/
  wr16/wr32 helpers; IPs are u8[4] end to end (no host-order u32 anywhere).
- **S-NET-22** [auto] The IPv4 header checksum MUST validate on every frame
  the stack emits (SPECTEST recomputes over the 20-byte header = 0).

- **S-NET-23** [auto] The advertised TCP window MUST NOT exceed the receive
  buffer's free space, and a window that collapsed below one MSS MUST be
  re-announced when `net_tcp_recv` drains the queue (else the peer sits in
  zero-window probes). FIXED 2026-07-21 (was: fixed 4096 over a 2048 rxq).
  site: `net.c tcp_out` window field / `net_tcp_recv` update ACK.
- **S-NET-24** [auto] The limited-broadcast test MUST match exactly
  255.255.255.255. NOTE: current `net.c ip_recv` checks only the first two
  octets (255.255.x.x accepted).
- **S-NET-25** [auto] TCP retransmission MUST eventually give up (bounded
  retries or a caller-visible error); ARP cache eviction SHOULD not always
  clobber slot 0. NOTE: current code retransmits forever with no backoff,
  and a full ARP cache always evicts entry 0.
- **S-NET-26** [auto] `net_tcp_connect`'s return value MUST be meaningful or
  documented as always-0 — callers checking `< 0` (e.g. `pc64_http.c`) are
  dead code today; connection failure is only observable via state timeout.
- **S-NET-30** [auto, live] With a bound NIC and link up, a DHCP lease MUST
  bind, and one plain-TCP round-trip (a DNS query for example.com over TCP,
  RFC 7766 framing, to the lease's resolver or gateway on :53) MUST complete
  connect → send → recv → close against the real peer. SKIP (named reason)
  when no NIC binds or the link is down; a live link that cannot lease is a
  FAIL carrying the tx/rx/arp/ip frame counters — the AX88179 failure class.
  (This is the boot-time S-NIC-12 chain on whatever NIC is present. The old
  netstub line called it "S-NET-20", which collides with the RX-drain
  contract above — renumbered to 30.)

## S-MOD — .UNO module loader (`pc64_modload.c`, `app_loader.c`)

- **S-MOD-01** [auto] A module MUST be refused whole (entry pointer NULL, no
  partial link) when ANY of: file shorter than the 48-byte header, size ≥
  MODBUF_MAX (2 MB), bad magic 'UNO1', ABI ≠ UNO_ABI_VERSION, header/file/
  reloc size inconsistency, CRC32 mismatch, entry ≥ mem_size, file_size >
  mem_size, or mem_size > 64 MB.
  check: SPECTEST corrupts each field of a scratch copy of a real module and
  asserts load failure; then loads the pristine copy and asserts success.
- **S-MOD-02** [auto] The mem_size cap (64 MB) MUST hold: a crafted
  mem_size near 0xFFFFFFFF MUST NOT wrap the page-count math into a tiny
  allocation followed by a ~4 GB memset (the documented CVE-class guard).
- **S-MOD-03** [auto] Every relocation RVA MUST satisfy rva+8 ≤ mem_size in
  64-bit math (rel[i]=0xFFFFFFF8 MUST be rejected, not wrap); every import
  record MUST satisfy imp_rva + 32*(i+1) ≤ mem_size.
- **S-MOD-04** [auto] Import names MUST be forcibly NUL-terminated at byte
  23 before lookup; an import absent from `kExports[]` MUST refuse the whole
  module and (on the non-slot path) free its pages.
- **S-MOD-05** [auto] The BSS tail (mem_size − file_size) MUST be zeroed
  before entry runs.
- **S-MOD-06** [auto] Search order MUST be `APPS\<NAME>.UNO` on every volume
  (0..n), then `EFI\UNODOS\APPS\<NAME>.UNO` on volumes 1..n; the first
  readable hit wins.
- **S-MOD-07** [auto] Tier flags MUST gate the entry signature: a
  UNO_MODF_UUI module MUST be refused in a classic slot, a non-UUI module
  refused by `uno_mod_load_uui`, a non-PY module refused by
  `uno_mod_load_pyrt`, and a user app (Studio build-run) MUST be classic
  tier only.
  NOTE: current code may violate the resource intent on these refusals —
  `pc64_modload.c mod_load` / `uno_mod_load_uui` / `uno_mod_load_pyrt`
  return 0 AFTER a successful `mod_instantiate` without freeing the
  instantiated pages (leak per refused load). Intended: free on refusal.
- **S-MOD-08** [auto] Module pages MUST be allocated as EfiLoaderCode while
  attached (executable under firmware NX policy); once detached, loads MUST
  come from the pre-reserved 4 MB arena and MUST fail cleanly (NULL) when
  the arena is exhausted — never fall back to non-executable heap.
- **S-MOD-09** [auto] `uno_modload_reserve` MUST be called before
  ExitBootServices so the arena + 512 KB user slot exist post-detach;
  detached user-app loads MUST land in the SAME fixed slot every rebuild
  (no arena growth across Studio's build-run loop).
- **S-MOD-10** [auto] `uno_load_module(proc)` MUST cache per app id: exactly
  one load attempt per boot per id (`gTried`), NULL cached on failure, and
  out-of-range ids MUST return NULL.
- **S-MOD-11** [auto] `uno_mod_peek_flags` MUST return 0 unless the first 48
  bytes read cleanly and carry the UNO1 magic.
- **S-MOD-12** [auto] A PYAPP container MUST be validated (magic, ABI, PYAPP
  flag, size, CRC over file_size) before its payload pointer is handed to
  PYRT; the payload pointer is transient (valid only until the next
  modload/gModBuf use).
  NOTE: current code may violate the size check — `pc64_modload.c
  uno_mod_load_pyapp` computes `(long)sizeof *h + h->file_size > n` where
  `long` is 32-bit, so file_size ≈ 0xFFFFFFFF wraps and passes, then
  `mod_crc32(..., (long)file_size)` runs with a negative length (loop skipped)
  and `*len` becomes -1. Intended: `file_size <= n - 48` in 64-bit math, as
  the mod_instantiate path already does.
- **S-MOD-13** [assert] `mod_read` path assembly MUST stay within its 64-byte
  buffer: 8.3 module filenames plus the longest prefix
  (`EFI\UNODOS\APPS\`, 16 chars) must fit; assert `strlen(file) <= 12` at
  entry. site: `pc64_modload.c mod_read`.
- **S-MOD-14** [auto] `uno_mod_unload_user` MUST return arena/user-slot
  bookkeeping to the empty state; only the most recent bump-arena
  allocation is ever physically reclaimed (documented arena semantics), and
  fixed-slot loads stay resident by design.

## S-BOOT — boot, present path, detach (`uefi_main.c`)

- **S-BOOT-01** [auto] `efi_main` MUST disable the firmware watchdog
  (`SetWatchdogTimer(0,...)`) and install the debug early hooks
  (`uno_dbg_early`) BEFORE any other init, so an init crash still reports.
- **S-BOOT-02** [manual] Boot MUST NOT call GOP SetMode: the firmware's
  native mode is kept (eDP panels accept 640×480 then stop scanning out);
  the desktop is a logical `modeW/2 × modeH/2` surface fill-scaled at
  present. F10 mode cycling is the only SetMode and MUST be refused once
  detached (GOP dies with boot services).
- **S-BOOT-03** [auto] Present-path selection MUST set `gUseBlt` when the
  pixel format is neither 8-bpp RGB nor BGR or there is no linear
  framebuffer, and MUST set `gBltFast` only when a boot-time benchmark
  measures Blt >= 2x faster than direct stores (bench rows erased after).
- **S-BOOT-04** [auto] `uno_pc64_present` MUST NOT touch VRAM for an
  unchanged frame: pass 1 diffs each composited row against `gShadow`,
  and a fully-clean frame returns after `delay_ms(1)` with zero VRAM
  writes. check: present twice with no drawing; time the second call.
- **S-BOOT-05** [auto] Dirty-span present MUST be conservative-correct: the
  output span covers the changed source span with +-1 guard pixel, the
  scaled row cache is rebuilt when the span widens, and after present the
  screen MUST byte-match a full-frame blit of the same scene.
  check: randomized partial redraws vs a reference full present.
- **S-BOOT-06** [auto] The wide-store path MUST only require DESTINATION
  8-byte alignment (unaligned lead pixel fixed up singly); source pixels
  are read individually.
- **S-BOOT-07** [auto] `try_detach` MUST refuse when ANY of: already
  detached; Blt-only display (`gUseBlt`/no linear fb); no native keyboard
  (PS/2, I2C-HID, or USB-HID); detach would strand the pointer (firmware
  pointer in use, LPSS controllers present, no native pointer); native TSC
  not calibrated; `uno_fat_native_eligible()` false.
- **S-BOOT-08** [assert] Before ExitBootServices, detach MUST
  `uno_fat_sync()` (cache flushed while firmware Block IO is alive) and
  `uno_modload_reserve()` (post-detach arena). site: `uefi_main.c
  try_detach`, ordering.
- **S-BOOT-09** [auto] ExitBootServices MUST be retried exactly once with a
  freshly fetched memory map (map-key staleness), using the static 64 KB
  map buffer; on success the code MUST set `gDetached`, clear `gBltFast`,
  drop all firmware input handles, init PS/2 + USB HID as applicable, and
  run `uno_blk_detach` -> `uno_fat_remount` -> `uno_fs_remap` in that order.
- **S-BOOT-10** [auto] Keyboard polling MUST drain at most 32 events per
  frame (firmware phantom-key guard) with source priority native-HID >
  PS/2 (detached) > SimpleTextInputEx > ConIn; pointer priority is I2C-HID
  abs > first firmware abs > firmware relative > PS/2 > USB HID, with
  latched button state (UEFI GetState NOT_READY semantics).
- **S-BOOT-11** [auto] Pointer speed scaling MUST use a 64-bit intermediate
  (the X1 wide-desktop int32-overflow fling regression test).
- **S-BOOT-12** [auto] `uno_pc64_delay_ms` MUST use firmware Stall while
  attached and the calibrated-TSC native delay once detached; TSC
  calibration happens over a 50 ms Stall during init.
- **S-BOOT-13** [manual] Detach MUST NOT strand the volume the system
  actually booted from.
  NOTE: current code may violate this (METAL-FINDINGS F8) —
  `try_detach` gates on `uno_fat_native_eligible()` ("some UnoDOS volume is
  natively reachable"), not "the BOOT volume survives"; a USB-booted system
  with UnoDOS also on the internal disk detaches away its own boot volume.
- **S-BOOT-14** [auto] At the end of init exactly one watchdog MUST be
  armed: `uno_dbg_on_detach()` when detached (own IDT + LAPIC), else
  `uno_dbg_watchdog_start()` (firmware timer event).

## S-DBG — debug harness (`uno_debug.c`, `uno_debug.h`)

- **S-DBG-01** [auto] The RAM stash MUST carry magic 'UDBG' + version 1 in
  a 64 KB block claimed at a fixed physical address ({0x01F00000,
  0x03F00000, 0x07F00000} first fit) so it survives warm reset; the .bss
  fallback MUST still work (minus warm-reset survival).
- **S-DBG-02** [auto] A stashed CRASH report MUST be CRC32-gated before the
  next boot trusts it; the log ring is deliberately NOT CRCed (torn tails
  acceptable). A magic/version mismatch MUST reset the stash, never parse
  garbage.
- **S-DBG-03** [auto] Crash reports MUST self-symbolize: rbp-chain
  backtrace bounded to 32 frames, within +-1 MiB of RSP, strictly
  increasing, return addresses inside the image; plus a 48-qword raw stack
  scan symbolizing in-image values. Symbol lookup is binary search over the
  generated rva table, printing `name+0xoff`, `[rva 0x..]` when no table,
  and `(outside image)` otherwise.
- **S-DBG-04** [auto] A #PF report MUST decode CR2 and the error bits; a #UD
  at a `0F 0B` (ud2) site MUST be labeled as a UBSAN TRAP.
- **S-DBG-05** [auto] Every report MUST include: build id, uptime ms,
  detached flag, image base, heap used/free/largest/oom count, note count,
  and the LAST CHECKPOINT (`uno_dbg_check` tag + TSC timestamp).
- **S-DBG-06** [assert] Trap nesting MUST degrade monotonically: first
  fault builds + stashes the report, a fault inside the handler goes
  straight to reset (stash already holds the report), a third-level fault
  hard-resets with no further work. site: `uno_debug.c uno_dbg_trap`
  `g_in_trap` guard.
- **S-DBG-07** [assert] Disk writes from a fault context MUST be gated:
  only when not already inside a disk operation (`g_in_disk`) and the
  faulting RIP is inside our image; otherwise stash-only, flushed by the
  NEXT boot (`uno_dbg_flush_residue`). site: `uno_dbg_trap`.
- **S-DBG-08** [auto] While attached, only IDT vectors
  {0,4,5,6,8,10,11,12,13,14,17} are hooked (firmware timer/event vectors
  >=32 untouched); once detached the OS owns a full 256-entry IDT with
  spurious vectors counted, PICs masked, and a LAPIC periodic timer.
- **S-DBG-09** [auto] The watchdog MUST fire when the main-loop heartbeat
  is >20 s stale, checked every 2 s attached (firmware timer event) /
  ~1 s detached (LAPIC); `uno_dbg_heartbeat()` once per shell frame is the
  ONLY reset. On fire it writes an HG report — to disk only when detached
  (attached timer TPL cannot touch Block IO: stash + reset instead).
- **S-DBG-10** [auto] `uno_dbg_net_trace` MUST feed the watchdog heartbeat
  (WiFi bring-up legitimately blocks >20 s; a watchdog reset mid-join would
  destroy the evidence the trace exists to collect).
- **S-DBG-11** [auto] Residue flush MUST run right after `uno_fat_init` and
  before anything else can crash: a stashed report is persisted as its own
  kind, and a non-clean previous boot (no report, `boot_count > 1`) MUST
  salvage the surviving RAM log tail as an RS report.
- **S-DBG-12** [auto] `uno_dbg_mark_clean()` on a deliberate
  shutdown/restart MUST suppress the next boot's residue report.
- **S-DBG-13** [auto] The machine tag MUST derive from SMBIOS Type 1
  (SMBIOS3 preferred), map known machines via the fixed table, else a
  sanitized <=8-char product/version, else "UNKNOWN"; ALL telemetry lands
  under `CRASH\<TAG>\` (fallback plain `CRASH` if mkdir fails), so one
  stick serves a fleet without collisions.
- **S-DBG-14** [auto] Report families and paths are fixed:
  `CRASH\<TAG>\CR###.TXT` (exception), `HG###` (hang), `RS###` (residue),
  `PN###` (panic), `PF###` (perf), plus `BOOTENV.TXT` (root + tag dir,
  every boot), `CRASH\<TAG>\BOOTLOG.TXT` (every boot + 30 s heartbeat) and
  `CRASH\<TAG>\BOOTS.TXT` (earliest proof-of-boot append). 8.3 names only.
- **S-DBG-15** [assert] Every telemetry file write MUST be followed by
  `uno_fat_sync()` so a power cut loses at most the current line.
  site: `uno_debug.c disk_write_report` / bootlog writers.
- **S-DBG-16** [auto] Report sequence numbering MUST NOT overwrite an
  existing report. NOTE: current `next_seq` returns dir-count+1, which
  collides (overwrites) after any earlier report file is deleted. Intended:
  scan for the first unused number.
- **S-DBG-17** [auto] `uno_dbg_force(k)` MUST produce the matching report
  family end-to-end (0 #PF -> CR, 1 #UD -> CR/UBSAN, 2 #DE -> CR, 3 stack
  smash -> PN, 4 hang -> HG, 5 panic -> PN) followed by reset — this is
  SPECTEST's own meta-test of the harness.
- **S-DBG-18** [auto] OOM injection (`uno_dbg_oom_every=N`) MUST make every
  Nth malloc return NULL without crashing the shell (paired with the
  stress OOM phase).
- **S-DBG-19** [auto] `uno_dbg_hud` MUST return 0 when the HUD is off; perf
  counters keep EMA render/present us, max values, and a hitch count with
  the >100 ms (HITCH_US) threshold, and PF snapshots carry
  `uno_dbg_perf_line`.
- **S-DBG-20** [auto] The crash-volume lookup MUST negative-cache with a
  64-call backoff (the F10/P2.2 fix) so a stickless machine does not
  rescan storage on every log line.
- **S-DBG-21** [manual] The watchdog MUST allow slow first paints: arming
  seeds one heartbeat (20 s grace).
  NOTE: possible violation (METAL-FINDINGS F9) — on a machine with ~250 ms
  present the first shell heartbeat can land >20 s after arming, producing
  a spurious HG + reset loop.
- **S-DBG-22** [auto] Boot-loop protection: `uno_dbg_crash_count()` MUST
  reflect the report files present at boot, and consumers (stress) damp
  behavior at >=8.

## S-STRESS — stress driver (`pc64_stress.c`)

- **S-STRESS-01** [auto] The driver MUST stay unarmed unless `STRESS.CFG`
  exists on some volume; config keys match whole tokens on NON-comment
  lines only (a key inside a `#` comment must not arm anything — the F1
  regression).
- **S-STRESS-02** [auto] Arming MUST wait ~120 frames for storage to
  settle; thereafter one action runs every `speed` frames (fast=1, default
  4, slow=12).
- **S-STRESS-03** [auto] Recognized keys are exactly: `passes=N`, `once`,
  `fast`, `slow`, `allow-force`, `force-hang`, `noshutdown`, `nonet`;
  unknown keys are inert.
- **S-STRESS-04** [auto] With >=8 prior crash reports on the volume the
  driver MUST self-damp (speed 20, forced faults disabled) to break boot
  loops.
- **S-STRESS-05** [auto] The 8 phases (smoke, pointer storm, key storm,
  window-cap, deep dir tree, OOM injection, corpus, close) MUST run
  weighted round-robin; one pass = 48 actions and writes one `PF###.TXT`
  snapshot.
- **S-STRESS-06** [auto] The forced-fault self-test runs ONLY when
  `allow-force` is set and only on pass 1, and MUST yield a matching crash
  report after reboot (harness meta-test).
- **S-STRESS-07** [auto] A bounded run (passes=N) MUST wind down: close to
  one window, idle, then auto-shutdown after ~4 s unless `noshutdown`;
  F12 MUST disarm at any time and hand back a usable desktop.
- **S-STRESS-08** [auto] The window-cap phase MUST drive the shell to the
  UNOUI_MAX_WINDOWS cap without a crash, leak, or lost taskbar state.
- **S-STRESS-09** [auto] The stress driver MUST NOT start acting before the
  one-shot network test has run (nettest ticks first in the shell loop).

## S-NTST — boot network self-test (`pc64_nettest.c`)

- **S-NTST-01** [auto] The test runs at most ONCE per boot, ~30 frames in;
  later `pc64_nettest_tick` calls MUST be free.
- **S-NTST-02** [auto] It MUST be skipped entirely when no `STRESS.CFG`
  exists (flag probe returns -1: not a test stick) or when `nonet` is set.
- **S-NTST-03** [auto] All progress MUST stream to `CRASH\NETLOG.TXT`
  line-by-line (flushed per line) so a mid-test hang still leaves the
  trail; every trace line feeds the watchdog heartbeat.
- **S-NTST-04** [auto] Device selection order MUST be: USB ethernet
  (ax88179/rtl8152) present -> "eth" suite (8 s link) and WiFi SKIPPED;
  else Intel WiFi PCI function or creds present -> "wifi" suite (15 s
  link) with full bring-up trace; else wired PCI NIC -> 5 s suite; else
  log "NOTHING TO TEST".
- **S-NTST-05** [auto] Suite PASS is defined as exactly "DHCP lease bound
  within 12 s"; gateway ping x3 and the `example.com` DNS query are logged
  but MUST NOT gate the verdict.
- **S-NTST-06** [auto] WiFi credentials MUST be read from `WIFI.CFG` /
  `WIFI.TXT` (`ssid=`/`psk=`) at any volume root; absence of both the card
  and creds skips the WiFi suite.
- **S-NTST-07** [auto] The inventory step MUST enumerate and log USB
  handles and PCI NICs (class 02/00 wired, 02/80 WiFi) before binding
  anything.

## S-FONT — TrueType text engine (`pc64_font.c`)

- **S-FONT-01** [auto] Font loading MUST fill one of 4 slots (Chicago,
  Sans, Mono, Ubuntu) from a volume root then `EFI\UNODOS\`, and MUST
  refuse a font file that fills the whole FONT_BUF staging buffer
  (truncation guard: stb_truetype must never chase table offsets past the
  buffer).
- **S-FONT-02** [auto] With no TTF active (`uno_font_active() < 0`), the fb
  provider hooks MUST fall back to 8-px-per-char bitmap metrics.
- **S-FONT-03** [auto] Measurement and drawing MUST agree exactly: the same
  pen walker (kerning applied before the draw/measure branch) produces
  `uno_font_text_w_styled(s) == final_pen_x - start_x` for every string —
  caret positioning depends on this. check: random ASCII strings, compare
  draw-advance vs text_w.
- **S-FONT-04** [auto] The glyph cache MUST cover exactly ASCII 32..126 in
  6 LRU banks keyed by (slot, px, subpixel); switching between two faces
  repeatedly MUST NOT re-rasterize every glyph each switch (bank reuse).
- **S-FONT-05** [auto] Codepoints outside 32..126 MUST render as a blank
  advance (space width), never index the cache out of range; glyph boxes
  are capped at 40x40 px.
  NOTE: intended behavior for shipped documents is at least Latin-1
  visibility; bytes 128..255 currently vanish silently (no UTF-8 decode,
  no .notdef box) — flagged as a divergence, not a crash.
- **S-FONT-06** [auto] Effective pixel size MUST clamp to 8..40 after UI
  scale; the bitmap-native Chicago face at 100% scale disables subpixel
  rendering.
- **S-FONT-07** [assert] The per-glyph raster arena (512 KB) is reset per
  glyph and MUST satisfy every stb allocation or the glyph is skipped —
  never a partial raster from a stale pointer. site: `pc64_font.c` arena
  alloc fail path.
- **S-FONT-08** [auto] Bold rendering (+1 px second strike) MUST also widen
  the reported advance so measure/draw stay in lockstep; italic shear MUST
  NOT change the advance.
- **S-FONT-09** [assert] Text drawing MUST be safe for any on-screen x,
  including negative pen positions from centered strings wider than the
  surface. NOTE: METAL-FINDINGS F7 — a `x<<6` on a negative x in the
  renderer is UB (UBSan #UD in debug builds); callers are only partially
  audited. site: `pc64_font.c` pen setup.

## S-UUI — unoui toolkit (`../unoui/unoui.c`, `unoui_input.c`)

- **S-UUI-01** [auto] The window list holds at most UNOUI_MAX_WINDOWS (24);
  `unoui_ui_add` MUST return 0 (window not added, no state change) when
  full. check: add 25 windows; the 25th returns 0 and nwin stays 24.
- **S-UUI-02** [auto] Z-order MUST respect the three pin bands at all
  times: BOTTOM windows behind all normal windows behind all TOP windows;
  a click-raise moves a window only within its band; `unoui_ui_add`
  inserts band-sorted.
  check: after any event sequence, assert the band ordering invariant over
  `ui->win[]` — a cheap every-frame assert.
- **S-UUI-03** [auto] Focus MUST always be valid: `focus_win` in
  [-1, nwin) and `focus_wi` in [-1, win->nw); adding a normal window
  focuses it (`focus_wi = -1`).
- **S-UUI-04** [auto] Widgets per window are capped at UNOUI_MAX_WIDGETS
  (64). Adding beyond the cap MUST fail safely.
  NOTE: current `unoui.c push()` returns `&win->w[63]` on overflow WITHOUT
  incrementing nw or clearing the slot — every further add silently
  corrupts slot 63 with stale `id`/`edit`/`canvas` fields. Intended: return
  a scratch widget or refuse; at minimum memset the returned slot.
- **S-UUI-05** [auto] The toolkit MUST be a pure function of the event
  stream: an identical `unoui_event` sequence fed to an identically-built
  UI MUST produce identical widget state and identical `unoui_action`
  results (the portability contract). check: golden event-script replay
  comparing final state hashes.
- **S-UUI-06** [auto] MOUSE_DOWN dispatch order MUST be: open popup
  (commit inside / dismiss outside) -> topmost window at point -> raise
  within band + focus -> resize grip -> titlebar (close box, then drag)
  -> topmost interactive widget at point.
- **S-UUI-07** [auto] A button activation fires ONLY when the release lands
  on the same widget that was pressed (capture-then-verify); dragging off
  and releasing MUST NOT activate.
- **S-UUI-08** [auto] Mouse capture (`cap_mode`) MUST route all MOVE events
  to the captured widget and suppress hover updates until MOUSE_UP.
- **S-UUI-09** [auto] The title-bar close box MUST produce
  `unoui_action{kind = UI_ACT_CLOSE, value = z-index}` and MUST NOT itself
  remove the window (the app owns removal).
- **S-UUI-10** [auto] Window drag MUST be outline-only (`drag_active` +
  `unoui_draw_drag_outline`), committing geometry on release, then
  clamping to the screen.
  NOTE: the commit clamp uses `screen_h - 16`, not the shell work area, so
  a window can be parked under the always-on-top taskbar; `clamp_to_workarea`
  only runs on open/reflow. Intended: clamp to the work area.
- **S-UUI-11** [auto] Editable text MUST respect `unoui_text.cap`: no
  insertion may write at or past `buf[cap-1]` (NUL always preserved);
  caret and sel stay in [0, len]; multiline stores '\n' only.
  check: type/paste into a cap-8 field until full; verify buf[7]=='\0'.
- **S-UUI-12** [auto] Tab traversal cycles the FOCUSED window's focusable
  widgets with wraparound (deliberately not the front window, so a TOP
  taskbar cannot trap Tab); ESC closes an open popup.
- **S-UUI-13** [auto] Popups (menubar menus, dropdown lists) are
  mouse-modal: the next MOUSE_DOWN either commits an item inside the popup
  rect or dismisses it; popup hit rows MUST match the drawn rows exactly.
  NOTE: hit-test anchors rows at `popup_r.y+2` while drawing anchors at
  `+3` — a 1-px band where hover highlight and committed item disagree.
- **S-UUI-14** [auto] `unoui_fullscreen(ui, win)` MUST route ALL input to
  that window's first UI_CANVAS and suppress desktop/chrome rendering;
  `unoui_fullscreen(ui, NULL)` restores the normal desktop.
- **S-UUI-15** [auto] Widget rendering MUST be clipped to the window
  content rect (BARE windows: the full rect) via fb_set_clip, so no widget
  can paint outside its window; the clip is restored after each window.
- **S-UUI-16** [auto] Every theme vtable entry is optional: a theme with
  `draw == NULL` or any NULL member MUST render via the built-in defaults
  (PICK fallback) — themes can never null-deref the renderer.
- **S-UUI-17** [auto] Per-window font overrides (`font_slot !=
  UI_FONT_INHERIT`) MUST wrap BOTH drawing and hit-testing in
  font push/pop so clicks are measured with the face that was drawn.
- **S-UUI-18** [assert] Removing or inserting a window MUST fix up ALL
  positional indices: focus, hot, capture, and popup owner.
  NOTE: the shell's `remove_win` only clamps `focus_win` at the top end and
  never adjusts `cap_win/hot_win/popup_win` (and `unoui_ui_add` adjusts
  only focus) — after a close, stale indices can point at a different
  window (focus lands on the taskbar; a live capture dereferences the
  wrong window). Intended: shift every index > removed position down by
  one and clear any that pointed at the removed window.
  site: `pc64_uui.c remove_win` / `unoui_input.c unoui_ui_add`.
- **S-UUI-19** [auto] Radio grouping is defined over CONTIGUOUS runs of
  UI_RADIO widgets; checking one clears exactly its contiguous neighbours
  (documented layout constraint — a non-radio widget splits the group).
- **S-UUI-20** [auto] `unoui_reflow_window` MUST stretch only UI_WF_FILL
  widgets to the content rect; resize respects `min_w/min_h`.
- **S-UUI-21** [auto] Caret blink derives from TICK events only
  ((ticks/18)&1) — no wall clock in the toolkit.

## S-SHELL — desktop shell (`pc64_uui.c`, `pc64_uui_apps.c`)

- **S-SHELL-01** [auto] The app roster is fixed at NAPPS = 7 native + 6
  bridge + 6 extra slots; `.UNO`-backed entries (Studio, Photos, PYRT) are
  probed lazily with an ABI check (`abi != UNO_UUI_ABI` -> hidden), and
  absent modules hide their launcher entries rather than erroring.
- **S-SHELL-02** [auto] `open_app` MUST be idempotent per app: windows are
  built once (`g_built`), reopened windows are clamped to the work area,
  and when `unoui_ui_add` fails (table full) the launch MUST abort cleanly
  — no `g_open` flag, no open hooks, retryable later.
- **S-SHELL-03** [auto] Closing an app MUST run its class teardown hook
  (game close / music stop / module `closed` / python unload / bridge
  close), drop fullscreen if held, remove the window, and rebuild the
  taskbar; the desktop and taskbar (BARE windows) MUST refuse close.
- **S-SHELL-04** [auto] The taskbar MUST show one chip per OPEN app laid
  out from live font metrics, truncating (not overflowing) at the tray;
  chip hit-testing MUST mirror the layout code exactly.
- **S-SHELL-05** [auto] The launcher opens on desktop right-click (at the
  pointer) or Ctrl-Esc, lists all non-hidden apps plus Restart / Shut
  Down, and scrolls beyond 11 visible entries.
- **S-SHELL-06** [auto] F2 / Ctrl-Tab MUST cycle focus through open app
  windows only (skipping desktop/taskbar); Ctrl-W closes the focused app
  window (same path as the close box).
- **S-SHELL-07** [auto] A bridge app whose module fails to load MUST render
  a "module not found: APPS\<FILE>.UNO" placeholder in its canvas rather
  than crash, and MUST retry-once semantics per boot (gTried).
- **S-SHELL-08** [auto] Theme / font / UI-scale / resolution changes MUST
  reflow or rebuild every open shell window (rebuild_shell / reflow) and
  invalidate the desktop background cache; no window may keep stale
  metrics.
- **S-SHELL-09** [auto] Desktop icon drag MUST be intercepted before the
  toolkit (a completed drag never also launches); icons snap to the free
  grid cell nearest the drop.
  NOTE: a sub-threshold (<=3 px) click-drag nudges the icon's rect without
  restoring it — icons can creep by up to 3 px per click. Intended:
  restore the rect when the drag never crossed the threshold.
- **S-SHELL-10** [auto] The shell present policy MUST skip repaint+present
  entirely on frames where nothing marked `g_dirty` (cursor-only moves
  re-composite the cursor without a scene repaint); window drags use
  scene_save/restore + outline drawing, never full repaints per drag
  frame.
- **S-SHELL-11** [auto] The main loop MUST call, every frame:
  `uno_dbg_heartbeat`, then input pump, then `pc64_nettest_tick` (one-shot
  inside), then `pc64_stress_tick`, and present; the HUD line renders only
  when enabled.
- **S-SHELL-12** [auto] Shutdown/Restart from the launcher MUST call
  `uno_dbg_mark_clean()` before resetting so the next boot writes no
  residue report.
- **S-SHELL-13** [auto] Module shell services exported to .UNO apps
  (`pc64_shell_add_window` / `_del_window` / `_focus_window` / `_dirty` /
  workarea queries / theme pointer) MUST stay valid across a theme or
  resolution change (module windows are reflowed like native ones).
- **S-SHELL-14** [manual] Lid-close sleep MUST blank the framebuffer,
  enter the low-power loop, and wake on lid-open/key/click with the scene
  intact.
- **S-SHELL-15** [auto] Fullscreen native games MUST restore the desktop
  (and `uno_pc64_lowres(0)` where used) on close — no stuck fullscreen
  after a game exits.

## S-USB — xHCI + USB HID (`xhci.c`, `usbio.c`, `usbhid.c`)

- **S-USB-01** [auto] xHCI init MUST bind PCI class 0C/03/30, disconnect
  the firmware driver first, and retry the bringup up to 5 times to clear
  the firmware-handoff HCE race; success requires !HCE and (no ports or
  >=1 enumerated device).
- **S-USB-02** [auto] Bringup order is fixed: halt -> HCRST -> wait CNR ->
  slots -> scratchpad -> DCBAA -> command ring (cycle 1) -> event ring +
  ERST -> run -> port power/reset -> enumerate; all waits bounded.
- **S-USB-03** [auto] Port reset MUST ack ALL change bits (CSC/PLC/PRC)
  without touching PED/PP (SuperSpeed PSC-storm guard); command and
  transfer polls bound stray events at 4096.
- **S-USB-04** [auto] Event routing MUST deliver async (interrupt-IN)
  completions to their registered device slots — a synchronous waiter MUST
  NEVER consume another device's completion.
- **S-USB-05** [auto] HID claiming walks the config descriptor for
  class-3 boot interfaces (protocol 1 kbd / 2 mouse), requires an
  interrupt-IN endpoint, and issues SET_IDLE(0) + SET_PROTOCOL(boot);
  keyboard reports below 8 bytes and mouse reports below 3 are ignored.
- **S-USB-06** [auto] Interrupt-IN pipes keep exactly one TRB outstanding,
  re-armed on completion.
- **S-USB-07** [assert] EP0 max-packet MUST match the device: full-speed
  devices advertising MPS 64 SHOULD have the EP0 context updated after the
  first 8/18-byte descriptor read. NOTE: current `xhci.c` keeps
  mps_for_speed(8) for FS devices — fine for boot HID, a divergence from
  full enumeration. site: `xhci.c enumerate_port`.
- **S-USB-08** [auto] `usbio.c` (attached EFI_USB_IO transport) MUST return
  failure for every call once detached — USB NICs must observe a dead
  transport, not a hang.

## S-INP — keyboards and pointers (`hid_kbd.c`, `i2c_hid.c`)

- **S-INP-01** [auto] Boot-keyboard reports MUST be edge-detected against
  the previous report (no key repeats from level state); the 0x01 rollover
  code discards the whole report; Caps Lock inverts shift for letters
  only.
- **S-INP-02** [auto] Usage->ASCII mapping MUST cover the boot page tables
  (unshifted/shifted), route Enter/Backspace/Tab as unicode and
  Esc/Del/arrows as EFI scan codes; modifier byte bits 0x22 = shift,
  0x11 = ctrl.
- **S-INP-03** [auto] I2C-HID probing MUST validate a HID descriptor
  (wHIDDescLength == 30, bcdVersion == 0x0100) before classifying, parse
  the report descriptor for X/Y vs digitizer usages, and cap total probe
  cost (PROBE_BUDGET) so a bad controller cannot stall boot.
- **S-INP-04** [auto] The LPSS timing table MUST try the legacy 133 MHz
  values first (shipping machines unchanged) and the 216 MHz Comet/Ice
  Lake values next (the X1 SCL-overclock fix); the chosen entry is
  reported via `uno_i2c_hid_timing`.
- **S-INP-05** [manual] I2C-HID MUST bind the trackpad on the X1 Carbon
  Gen 8 and Surface Laptop Go.
  NOTE: current code fails on every tested machine (METAL-FINDINGS F4 —
  controllers found, no device); FIX-PLAN P1.1 calls for ACPI PNP0C50
  enumeration instead of PCI BDF guessing, which is unimplemented.
- **S-INP-06** [auto] Absolute-to-relative conversion MUST swallow >4000-
  unit jumps (finger lift/replace), and bitfield extraction clamps widths
  to 31 bits.
- **S-INP-07** [auto] BAR mapping MUST go through `uintptr_t` (Surface >4 GB
  BARs must not truncate to 32-bit).
- **S-INP-08** [assert] LPSS reset release writes only the legacy 0x804
  offset by design (documented); if `DW_IC_COMP_TYPE` does not read
  0x44570140 the controller MUST be skipped, never blind-poked.
  site: `i2c_hid.c` reset-release path.

## S-NIC — wired NIC drivers (`e1000.c`, `e1000e.c`, `igb.c`, `r8169.c`, `rtl8152.c`, `ax88179.c`, `uno_nic.h`)

- **S-NIC-01** [assert] Every published `uno_nic_t` MUST have all three
  function pointers (send/recv/link) non-NULL — `net.c` calls them
  unconditionally. site: each driver's `*_nic()` before returning, or a
  check in `net_init`.
- **S-NIC-02** [auto] `send` MUST return >= 0 on acceptance and reject
  (-1) frames larger than the driver buffer (2048 for the Intel trio);
  `recv` MUST return 0 when the ring is empty, never block.
- **S-NIC-03** [auto] The Intel drivers use 16-descriptor rings with
  identity-mapped .bss buffers, polled (interrupts masked, IMC all-ones);
  TX MUST wait for DD on the slot before reusing it.
- **S-NIC-04** [auto] Device matching is by exact PCI IDs: e1000
  8086:100E/100F; e1000e the 82571..I219 table (PCH parts take the
  SWFLAG/ULP path); igb I210/I211/82575..I350. A NIC absent from the table
  MUST leave `*_present() == 0` and register nothing.
- **S-NIC-05** [auto] igb bring-up MUST kick PHY autoneg via MDIC —
  QEMU's igb model gates RX on STATUS.LU (the documented gotcha); without
  the kick, link never rises.
- **S-NIC-06** [auto] MAC address MUST come from EEPROM/RAL-RAH (e1000),
  SHRAL/SHRAH fallback (I219), or the MAC0/MAC4 registers (r8169); a
  driver MUST NOT publish an all-zero MAC as valid.
- **S-NIC-07** [assert] e1000 unicast filter setup: the RAH0 write that
  sets AV MUST target 0x5404. NOTE: `e1000.c` defines REG_RAH0 as 0x5408
  (RAL1 on 8254x) while e1000e/igb use 0x5404 — masked today only by
  promiscuous mode. site: `e1000.c` REG_RAH0.
- **S-NIC-08** [auto] r8169: TX frames MUST be padded to 60 bytes; RX
  strips the 4-byte FCS; the 8125-family register relocation is selected
  by device id + TxConfig XID; DescOwn handshakes use compiler barriers.
  (End-to-end TX/RX on real silicon remains metal-pending — no QEMU model.)
- **S-NIC-09** [auto] ax88179: link state MUST be read from the SECOND
  BMSR read (latching register — the documented always-up bug); RX
  descriptor counts and offsets MUST be re-validated against the received
  bulk length on every packet pop (device-supplied data is untrusted).
- **S-NIC-10** [auto] rtl8152: family/version MUST be confirmed via the
  version register after the VID match; 8157/8159 (16-byte descriptor)
  parts are declined; early-RX MUST be sized from our buffer so an
  aggregated frame never splits across two bulk-INs; a descriptor
  `plen < 60` terminates the aggregation walk.
- **S-NIC-11** [auto] Both USB NIC drivers MUST work on either transport
  (attached EFI_USB_IO vs native xHCI) chosen at bind time, and MUST
  report link down rather than hang when the transport dies.
- **S-NIC-12** [auto] After `net_init(nic, mac)` with a loopback or QEMU
  e1000 NIC, the full boot chain MUST pass: DHCP bind, ARP gateway
  resolve, ICMP echo to the gateway, DNS query, single TCP
  connect/send/recv/close (this is the QEMU-verified regression suite).

## S-WIFI — WiFi drivers + WPA2 (`iwlwifi.c`, `rtwifi.c`, `mrvlwifi.c`, `wifi_wpa.c`)

- **S-WIFI-01** [auto] All three drivers MUST be inert-safe with no
  hardware, no firmware file, or no credentials: `*_present() == 0` /
  NULL nic, status string explains which prerequisite is missing, and boot
  continues.
- **S-WIFI-02** [auto] Firmware files load from `FIRMWARE\` on any volume
  (IWL*.UCO/.PNV, RTL88xx.FW, W88xx.FW); credentials from `WIFI.CFG` /
  `WIFI.TXT` (`ssid=`, `psk=`). A missing file MUST be reported in
  `*_status_str`, never faulted on.
- **S-WIFI-03** [auto] `wpa_pmk_from_psk` MUST be PBKDF2-HMAC-SHA1, 4096
  rounds, SSID as salt — byte-exact against the published WPA2 test
  vectors. check: the IEEE "password"/"IEEE" vector at boot.
- **S-WIFI-04** [auto] The supplicant state machine MUST follow
  IDLE -> (msg1: derive PTK, reply msg2+RSN IE) -> SENT_2 -> (msg3: verify
  MIC, verify ANonce unchanged, unwrap GTK via RFC 3394) -> reply msg4 ->
  DONE; any MIC/unwrap failure -> FAILED, never DONE.
- **S-WIFI-05** [auto] Only descriptor version 2 (HMAC-SHA1 MIC + AES key
  wrap, WPA2-PSK/CCMP) is accepted; WPA1/TKIP and WPA3 frames MUST be
  rejected cleanly.
- **S-WIFI-06** [assert] The caller MUST transmit the msg4 reply BEFORE
  installing keys (documented handshake ordering).
  site: each driver's `handle_eapol`.
- **S-WIFI-07** [auto] iwlwifi bring-up MUST reach firmware ALIVE (status
  0xCAFE within the 2 s wait) on supported silicon before any MVM
  commands; RF-kill MUST abort the bring-up with a clear status.
- **S-WIFI-08** [manual] A real join (scan -> auth -> assoc -> EAPOL ->
  DHCP) MUST complete on metal.
  NOTE: unimplemented today on all three drivers — iwlwifi stops after
  ALIVE/MVM init (`find_and_join` assumes bssid=broadcast/chan 1 and says
  so), rtwifi's firmware download body and power-on tables are stubbed,
  mrvlwifi stubs scan/associate. This is the known metal ceiling.
- **S-WIFI-09** [assert] A driver MUST NOT source 802.11 frames from an
  all-zero MAC. NOTE: iwlwifi never populates g_mac (NVM read stub) and
  rtwifi has no MAC read; only mrvlwifi reads permanent_addr from
  GET_HW_SPEC. site: `iwlwifi.c mvm_init_unified` / `rtwifi.c`.
- **S-WIFI-10** [auto] mrvlwifi RX MUST re-validate device-supplied
  pkt_off/pkt_len against the receive length before copying (untrusted
  DMA descriptor data).
- **S-WIFI-20** [auto, live] When an Intel WiFi PCI function AND WiFi
  credentials (`WIFI.CFG`/`WIFI.TXT`) are both present, the driver MUST
  bring the nic up, reach link (join) within 15 s, and bind a DHCP lease
  within 12 s. SKIP (named reason) when either prerequisite is absent;
  a bring-up that stops early FAILs with `iwl_status_str` so the report
  names the real blocker (firmware missing / no ALIVE / RF-kill / MLME
  ceiling — see S-WIFI-08). This is the boot-time automation of S-WIFI-08's
  join contract, expected red until the MLME tail lands.

## S-TLS — TLS client (`tls.c`, `tls_ca.c`)

- **S-TLS-01** [auto] `tls_version()` MUST report TLS 1.2 (0x0303) on an
  established session; the cipher roster is BearSSL's full client profile.
- **S-TLS-02** [auto] `tls_connect` (pinned mode) MUST validate the peer
  against the compiled-in P-256 public key ONLY (no chain/time/name
  checks — dev servers); `tls_connect_ca` MUST validate the full chain
  against the embedded trust store with SNI name checking and the UEFI
  RTC as validity time.
- **S-TLS-03** [auto] The embedded trust store MUST contain exactly the 14
  generated anchors (mktrust.py output) — SPECTEST asserts
  `uno_tls_tas_num == 14`.
- **S-TLS-04** [auto] All TLS failures MUST fail closed with a negative
  return and a BR_ERR_* reason via `tls_last_error()`; connect enforces a
  ~3 s SYN deadline, reads ~4 s, writes ~8 s.
- **S-TLS-05** [auto] Session resumption MUST be disabled (client_reset
  with resume=0) — every connect is a full handshake.
- **S-TLS-06** [auto] Entropy MUST come from RDRAND when CPUID advertises
  it; the TSC-LCG fallback is permitted only when RDRAND is absent and
  `tls_have_rdrand()` reports which one is live.
- **S-TLS-07** [assert] One TLS session at a time: the single global
  context means `tls_connect` while `g_open` MUST refuse or tear down
  first, and `tls_cipher/version/last_error` MUST NOT be trusted after
  close. site: `tls.c tls_connect` entry.
- **S-TLS-08** [manual] `tls_connect_ca` against a real public HTTPS host
  MUST succeed with a correct RTC and fail with a skewed RTC (fails
  closed, whole-web-down symptom documented).
- **S-TLS-09** [auto] TLS reads/writes MUST pump `net_poll` internally and
  never busy-hang past their deadlines when the peer goes silent.

## S-HTTP — HTTP client (`pc64_http.c`)

- **S-HTTP-01** [auto] `pc64_net_up()` MUST probe NICs in the fixed order
  e1000, e1000e, igb, r8169, ax88179, rtl8152, iwl, rtwifi, mrvl; bind the
  first present one; then wait for DHCP up to ~2 s. It is idempotent.
- **S-HTTP-02** [auto] `pc64_http_get` speaks HTTP/1.0 GET with
  Connection: close; https URLs use `tls_connect_ca` (443), http port 80;
  the URL parser accepts IPv4 literals and DNS names, host <= 128 and
  path <= 512 chars, request buffer 1024 with overflow -> -8.
- **S-HTTP-03** [auto] Error codes are fixed: -2 empty host, -3 no link,
  -4 DNS fail, -5 connect/TLS fail, -6 timeout, -7 TLS write fail, -8 URL
  too long; body length >= 0 on success.
- **S-HTTP-04** [auto] The response splits at the first blank line; the
  status line is returned (truncated) via `status`; redirects are NOT
  followed and chunked transfer coding is NOT decoded (documented — the
  browser shows what arrived).
- **S-HTTP-05** [auto] Responses larger than the 48 KB buffer MUST be
  truncated safely (no overflow), receive idle timeout ~3 s.
- **S-HTTP-06** [assert] Connect failure detection MUST NOT rely on
  `net_tcp_connect`'s return (always 0, see S-NET-26) — only on the state
  timeout. site: `pc64_http.c` connect loop.
- **S-HTTP-07** [auto] `pc64_http_get` MUST leave the single TCP
  connection closed (state CLOSED/DONE) on every exit path.

## S-SND — audio (`snd_pcm.c`, `hdaudio.c`, `ac97.c`)

- **S-SND-01** [auto] `uno_snd_init` MUST probe HDA (PCI class 04/03)
  first, then AC'97 (04/01), else leave the PC-speaker path active;
  `uno_snd_name()` reports which backend won.
- **S-SND-02** [auto] The DMA ring is 48 kHz s16 stereo looping forever;
  `uno_snd_poll` keeps writes LEAD_FRAMES (~200 ms) ahead of the hardware
  read pointer, with sfence before DMA-visible writes.
- **S-SND-03** [auto] Underrun MUST be benign: a starved stream FIFO plays
  silence and recovers on the next tick — the shell never stalls on audio.
- **S-SND-04** [auto] If DMA overtakes the write pointer, the writer MUST
  resync just ahead of the read pointer (one audible glitch, no permanent
  offset).
- **S-SND-05** [auto] Opening a sample stream MUST mute the square voice
  and take the ring; `uno_snd_stream_end` returns it; `stream_space` /
  `stream_write` are in INPUT frames at the decoder's rate, resampled
  16.16 linear to 48 kHz.
- **S-SND-06** [auto] `uno_snd_stream_flush` MUST drop queued audio
  without killing the stream (seek support); pause holds the DAC on
  silence.
- **S-SND-07** [auto] Note attack/release MUST ramp over ~8 ms (RAMP 24)
  — no clicks on note on/off; volume 0..100 scales AMP_MAX.
- **S-SND-08** [auto] All controller waits are SPIN_MAX-bounded: a dead
  codec/controller fails the probe rather than hanging boot; HDA falls
  back to immediate-command mode when CORB is dead, and to LPIB when the
  position buffer is dead.
- **S-SND-09** [manual] Audio MUST keep playing across detach (static
  identity-mapped DMA buffers, polled) — verified by ear on metal.

## S-ACPI — ACPI host (`acpi_host.c` + `../unoacpi`)

- **S-ACPI-01** [auto] `uno_acpi_start` MUST be idempotent, find the RSDP
  from the EFI config table (ACPI 2.0 GUID preferred), and run entirely
  from an 8 MiB arena — interpreter OOM is contained, never a kernel heap
  exhaustion.
- **S-ACPI-02** [auto] The host is single-threaded by contract:
  mutex/event/spinlock callbacks are no-ops and deferred work runs
  synchronously — SPECTEST asserts no callback ever blocks.
- **S-ACPI-03** [auto] Address spaces supported are SystemMemory
  (identity map), SystemIO (port I/O), and PCI_Config via mechanism #1
  segment 0 only; EC/SMBus waits are TSC-bounded (timeout, not hang).
- **S-ACPI-04** [auto] ACPI mode is read-only (no SCI/GPE enable): battery
  and lid state are polled on demand by the shell; a machine with no
  battery MUST report "no battery" rather than garbage.
- **S-ACPI-05** [manual] Battery percentage MUST track the real charge
  within a few percent on the calibrated machines (per-machine table).

## S-WRITE — the editor (`pc64_write.c`)

- **S-WRITE-01** [auto] The document model caps at WR_MAX-1 (32767) chars
  with a parallel per-char u16 style array; insertion at the cap MUST
  truncate safely (buffer and style array stay in sync, no OOB).
- **S-WRITE-02** [auto] UWD round-trip MUST be exact: save then load of any
  document <= 32767 chars reproduces text AND styles byte-for-byte
  (magic "UNOWR1\r\n" + length + text + RLE style runs).
  check: randomized styled documents, save/load/compare.
- **S-WRITE-03** [auto] `.TXT` save (case-insensitive extension) MUST write
  plain text only; reloading yields the same text with default styles
  (documented lossy path). Loading a .TXT normalizes CRLF -> LF, tab ->
  space, and drops non-printable bytes.
- **S-WRITE-04** [auto] The save buffer is worst-case sized
  (8 + WR_MAX + 4*WR_MAX + 16) — a maximally-fragmented style run set MUST
  NOT overflow it (statically provable, SPECTEST does the arithmetic).
- **S-WRITE-05** [auto] Saving to a read-only volume MUST fail visibly
  (status "Save FAILED"), never silently discard.
- **S-WRITE-06** [auto] Replace-all MUST terminate on every input,
  including empty-match and shrinking replacements.
- **S-WRITE-07** [auto] The wrap layout stops at WR_MAXL (4096) visual
  lines. NOTE: content past 4096 wrapped lines is silently unreachable in
  the view (though preserved in the buffer/save); intended behavior is a
  visible "document truncated in view" indicator.
- **S-WRITE-08** [auto] Clipboard operations clamp at CLIP_MAX (8192)
  without corrupting the document.

## S-FILES — file manager (`pc64_files.c`)

- **S-FILES-01** [auto] Each pane lists at most FM_MAXE (96) entries; FAT
  volumes get subdirectory navigation, flat stores are root-only.
- **S-FILES-02** [auto] Delete MUST be double-armed: the first activation
  arms (keyed to pane+selection), only a second activation on the SAME
  item deletes; changing selection disarms.
- **S-FILES-03** [auto] Directory delete MUST refuse non-empty directories
  (probe via `uno_fat_list_ex`).
- **S-FILES-04** [auto] Copy caps at FM_COPYMAX (2 MB) and refuses larger
  files; same-directory copy auto-prefixes "C_"; folder copy is refused;
  Move = copy then delete-source and MUST NOT delete the source when the
  copy failed.
- **S-FILES-05** [auto] Rename/mkdir/delete require a native-FAT pane and
  MUST be refused (no-op with status) elsewhere.
- **S-FILES-06** [auto] Activating a `.UNO` file MUST route to
  `pc64_shell_run_user` (the user-app slot), not the module roster.
- **S-FILES-07** [auto] All pane path buffers are fixed (120/136 bytes);
  descending directories MUST stop (not truncate-and-corrupt) when the
  joined path would overflow.

## S-MUSIC — music player (`pc64_music.c`, `pc64_media.c`)

- **S-MUSIC-01** [auto] Source slot 0 is the 8 built-in square-voice tunes;
  file playback accepts only names `um_audio_is()` recognizes (WAV / MIDI
  / MP3 / AAC via unomedia).
- **S-MUSIC-02** [auto] A malformed or undecodable file MUST surface
  `um_error()`'s reason in the status line and leave the player idle —
  never crash, never garbage audio. check: corpus of truncated/corrupted
  headers.
- **S-MUSIC-03** [auto] The decode pump MUST be bounded per tick (fill
  FIFO while space, 4096-frame chunks) so a slow decode underruns to
  silence instead of stalling the shell (pairs with S-SND-03).
- **S-MUSIC-04** [auto] `pc64_media` streams through one 64 KB sliding
  window over `uno_fs_read_at` — no whole-file allocation; a short read
  invalidates the window and returns partial cleanly.
- **S-MUSIC-05** [auto] Seek maps the 0..1000 slider through
  `duration_ms`; end-of-track auto-advances (or stops on the last track).

## S-CLOCK — clock app (`pc64_clock.c`)

- **S-CLOCK-01** [auto] Time source is the firmware RTC via
  `uno_pc64_time()`, treated as local; UTC = RTC - offset(minutes); the
  20-city table includes half-hour offsets (Mumbai +330). No DST by
  contract.
- **S-CLOCK-02** [auto] The day/night terminator MUST use real spherical
  geometry per map pixel (declination model, sin-phi test).
- **S-CLOCK-03** [auto] Repaint happens only when the displayed second
  changes.
- **S-CLOCK-04** [auto] The midnight carry is deliberately coarse (d=31/1
  regardless of month, solar-declination use only); the DISPLAYED
  date near month boundaries under large offsets may be off by one day —
  documented limitation, SPECTEST only asserts no crash across the
  rollover.

## S-BROWSER — browser (`pc64_browser.c`)

- **S-BROWSER-01** [auto] The document buffer caps at DOC_MAX (32768);
  fetch fills it via `pc64_http_get`, truncating oversized bodies safely.
- **S-BROWSER-02** [auto] Rendering is Markdown for `.md`/`.txt` URLs and
  the fixed HTML subset otherwise (h1-h6, b/strong, i/em, code/tt, a, pre,
  p/div/section, br, hr, ul/ol/li); head/style/script/title content MUST
  not render; entities beyond lt/gt/amp/nbsp pass through literally.
- **S-BROWSER-03** [auto] The style stack is 24 deep; deeper nesting MUST
  flatten silently, never overflow.
- **S-BROWSER-04** [auto] `<script>` bodies run ONLY for HTML documents,
  truncated at 8191 bytes, with document.write output spliced in and
  console.log collected into a rendered panel; script re-runs on every
  open (no persistence between pages).
- **S-BROWSER-05** [auto] Malformed HTML (unclosed tags, bogus entities,
  binary garbage) MUST render best-effort without a crash. check: corpus
  fuzz page set.
- **S-BROWSER-06** [auto] HTTPS pages go through the CA-validated TLS path
  only (no pinned-key escape hatch from a URL).

## S-JS — the JS engine (`js.c`)

- **S-JS-01** [auto] `js_run` MUST be self-contained per run: 512 KB bump
  arena reset each run, no GC, no state carryover between runs.
- **S-JS-02** [auto] Runaway scripts MUST be stopped by the loop guard
  (1e6 iterations per loop) and the stack probe (48 KB limit trips
  "nesting too deep" before the real stack faults) — no script may hang or
  fault the shell.
  check: `while(1);` and deep recursion both return with an error string.
- **S-JS-03** [auto] Supported types are exactly undefined/null/bool/
  number(double)/string/array/function; no object literals, prototypes,
  exceptions, or mutable closures — scripts using them get parse/eval
  errors, not UB.
- **S-JS-04** [auto] The stdlib surface is fixed: console.log,
  document.write, Math.{floor,ceil,round,abs,sqrt,max,min,random},
  String, Number, parseInt, array push/length, string length/index;
  Math.random is xorshift32 (deterministic given the seed).
- **S-JS-05** [auto] Structural limits (32 args, 16 params, 64-element
  array literals, 256 stmts/block, 512 stmts/program, 96 bindings/scope,
  1023-char string literals) MUST be enforced without memory unsafety.
  NOTE: today every one of these limits SILENTLY truncates or drops
  (string literals clipped, `push` past capacity no-ops, the 97th binding
  vanishes, indexed stores beyond capacity are ignored) — intended
  behavior is a reported script error; memory safety holds either way.
- **S-JS-06** [assert] Arena exhaustion MUST abort parsing/eval promptly.
  NOTE: `ar_alloc` on OOM returns the arena base and only sets `g_oom`,
  which the parser loops don't check — a huge script builds overlapping
  AST nodes (bounded, but garbage results) before the deferred OOM
  message. site: `js.c ar_alloc` callers / parse loops.
- **S-JS-07** [auto] Frame reclamation for scalar returns keeps deep
  call chains O(depth) in arena use, and results survive the reclaim
  (relocation correctness). check: 1000-deep recursion returning a
  string.
- **S-JS-08** [auto] `parseInt` truncates via numeric prefix parse (no
  radix support, "0x10" -> 0) — documented, tests pin the behavior.

## S-STUDIO — IDE + UnoC compiler (`apps/studio*.c`, `apps/ucc*.c`)

- **S-STUDIO-01** [auto] Buffers are fixed: 192 KB source, 3 MB compiler
  arena, 256 KB module output, 32 KB clipboard, 128 project files;
  insertion clamps at capacity without corruption.
- **S-STUDIO-02** [auto] Build/run are Ctrl-B / Ctrl-R (F-keys die with
  boot services); `.py` sources pack a PYAPP container (byte-identical to
  mkuno.py's output, zlib CRC32) instead of invoking ucc.
- **S-STUDIO-03** [auto] ucc MUST collect up to 16 diagnostics with
  file:line:col and return -1 — a compile error never longjmps past
  cleanup into the shell; clicking a diagnostic jumps the editor to it.
- **S-STUDIO-04** [auto] The UnoC subset is enforced by clean errors, not
  crashes: no floats, varargs, bitfields, goto, function-like macros, or
  `#if`/`#elif`; `#include` depth 4; recursion guard 200 on the parser.
  check: a corpus of rejected programs each names its diagnostic.
- **S-STUDIO-05** [auto] Codegen follows the MS x64 ABI exactly (RCX/RDX/
  R8/R9, 32-byte shadow, RSP 16-aligned at calls): a compiled module
  calling kernel exports MUST interoperate (params <= 8, args <= 8,
  by-value structs only of size 1/2/4/8).
- **S-STUDIO-06** [auto] The emitted `.UNO` MUST pass the S-MOD-01
  validation gauntlet (magic, ABI, CRC, sizes) and export entry
  `uno_app_main`; the compile -> load -> run loop in the user slot MUST
  work repeatedly without exhausting memory (fixed user slot when
  detached).
- **S-STUDIO-07** [auto] Runtime integer division in generated code
  follows hardware semantics: INT64_MIN / -1 raises #DE (module faults,
  crash-reported).
  NOTE: divergence from the compile-time folder, which guards the same
  expression — intended: emit the guard at runtime too (or document UB).
  site: `ucc_x64.c alu` ND_DIV/ND_MOD.
- **S-STUDIO-08** [auto] `studio_json` emission MUST bound-check and escape
  control characters; the AI client MUST de-chunk HTTP in place,
  null-check every malloc, and never allow disk config to disable cert
  validation (host override redirects the IP only — SNI/cert unchanged).
- **S-STUDIO-09** [auto] AI keys live in plaintext AI.CFG (documented);
  absence of the file disables the assistant cleanly.
- **S-STUDIO-10** [manual] The blocking AI request stalls the shell for
  its duration (stated v1 limitation) — watchdog must not fire (the TLS
  path pumps under the 20 s ceiling or feeds heartbeat).

## S-PHOTOS — photo viewer (`apps/photos.c`)

- **S-PHOTOS-01** [auto] Every decode failure surfaces `um_error()`'s
  precise reason (including named declines: progressive JPEG, WebP) —
  never a crash or a generic message.
- **S-PHOTOS-02** [auto] Image sizing MUST reject w/h < 1 and w*h*4 >
  256 MB computed in u64 (LLP64 overflow guard), and OOM-check the frame
  allocation.
- **S-PHOTOS-03** [auto] Scale math runs in long long (no 32-bit overflow
  on pan/zoom of large images); GIF animation pumps frame-by-frame.
- **S-PHOTOS-04** [auto] The checkerboard-under-alpha is pre-composited
  into the scaled cache (fb_blit contract: no blending in the hot loop).

## S-PAINT — paint app (`apps/paint.c`)

- **S-PAINT-01** [auto] The canvas is exactly 408x240 byte-per-pixel;
  every tool writes through the bounds-checked `pt_set_px`; save/load is
  the raw 97920-byte block (a size-mismatched file MUST be rejected,
  since a partial read leaves the canvas inconsistent).
- **S-PAINT-02** [auto] Flood fill uses a bounded 1024-entry explicit
  stack; saturation drops pushes (may under-fill) but MUST NOT overflow
  or hang.
- **S-PAINT-03** [auto] All 10 tools MUST stay inside the canvas for any
  drag path including off-canvas coordinates.
- **S-PAINT-04** [auto] Brush sizes are pencil 1 / brush 4 / eraser 8;
  color port uses the 256-entry palette, mono port the 10 dither patterns.

## S-GAME — games (`pc64_games.c`, `apps/dostris|pacman|outlast|runner.c`)

- **S-GAME-01** [auto] Dostris: 10x20 board; `dt_fits` MUST bounds-check
  every candidate cell before any write (spawn, rotate, drop, lock);
  line clear shifts rows and scores kLineScore[n]*(level+1) for n in 0..4;
  level = lines/10+1 capped at 15.
- **S-GAME-02** [auto] Pac-Man: movement only through `pm_walkable`
  (X-tunnel wrap, walls block, house gate conditional on eaten state);
  collision is the 6-px box test; ghost-eat score 200<<kills with kills
  capped at 3; eating all dots advances the level.
- **S-GAME-03** [auto] OutLast: all drawing goes through the clamped
  virtual-320x200 mapper; crash conditions are off-road or traffic-box
  overlap; 60 s time limit.
- **S-GAME-04** [auto] Runner3D MUST enter fullscreen + lowres on open and
  restore both on close (pairs with S-SHELL-15).
- **S-GAME-05** [auto] No game may write outside its board/canvas arrays
  for ANY input sequence — the stress key-storm phase is the harness for
  this.
- **S-GAME-06** [auto] Built-in games pace by frame counters (~30 Hz
  gates); the Toolbox-tier variants pace by TickCount deltas (see
  S-MAC-05 for the TickCount caveat).

## S-INST — installer (`installer.c`)

- **S-INST-01** [auto] `uno_inst_scan` MUST refuse to run detached, cap
  targets at 12, and NEVER offer the boot USB (or its sibling partitions)
  as a target — excluded by device-path prefix match.
- **S-INST-02** [auto] ESP install is non-destructive by contract: it
  creates/overwrites only under `\EFI\UNODOS\` plus `\EFI\BOOT\BOOTX64.EFI`
  — and the latter ONLY if it does not already exist (protects other
  OSes' fallback loaders).
- **S-INST-03** [auto] Every installed file MUST be copy-verified
  chunk-by-chunk (written == read count) and flushed; missing aux files
  (fonts/docs) are soft-skipped, a missing source BOOTX64.EFI is a hard
  fail.
- **S-INST-04** [auto] The whole-disk clone MUST validate before writing:
  source GPT present ("EFI PART"), entry size in 128..512 and multiple of
  8, num*esz bounded in 64-bit, target 512 B/s, not read-only, and large
  enough (last used LBA + 33).
- **S-INST-05** [auto] The clone MUST relocate the backup GPT to the
  target's real end and recompute both header CRC32s; boot entries are
  written via runtime SetVariable (Boot#### + BootOrder).
- **S-INST-06** [manual] A cloned disk MUST boot on the target machine
  (end-to-end, real firmware NVRAM).
- **S-INST-07** [assert] `write_boot_entry` MUST bound the device-path
  prefix against its 1024-byte buffer. NOTE: current code memcpy's a
  firmware-supplied prefix with no `losz <= sizeof lo` check — a long
  legitimate path overflows. site: `installer.c write_boot_entry`.
- **S-INST-08** [assert] GPT header CRC recomputation MUST range-check
  HeaderSize (<= 512) before crc32 over it. NOTE: current code trusts the
  source header's HeaderSize field — a bogus value reads past the 512-byte
  buffer. site: `installer.c` clone header patch.
- **S-INST-09** [auto] The clone SHOULD read back and verify the written
  region (a controller once corrupted a clone mid-write). NOTE: current
  code trusts WriteBlocks success — no verify pass exists. Intended:
  optional verify pass, at minimum over GPT + first FAT.

## S-MAC — Toolbox bridge (`mac_compat.c`)

- **S-MAC-01** [auto] Rect semantics are half-open (`PtInRect`: pt.h <
  right, pt.v < bottom); SetRect/OffsetRect/InsetRect are pure.
- **S-MAC-02** [auto] Drawing traps route to fb primitives with XOR modes
  via fb_invert_rect; FillRect's direct fb writes MUST stay
  bounds-checked; FrameArc is a documented no-op.
- **S-MAC-03** [auto] DrawText/TextWidth MUST agree (both walk real glyph
  widths, <= 255-char chunks) — Toolbox apps position carets by
  TextWidth.
- **S-MAC-04** [auto] The event queue holds 32 events and drops on
  overflow (never blocks); NewPtr(0) allocates 1 byte; DisposePtr frees.
- **S-MAC-05** [auto] `TickCount()` in this shim is a per-call counter,
  NOT wall time. NOTE: divergence from Toolbox semantics — Toolbox-tier
  apps pacing by TickCount deltas run at call-frequency speed; intended:
  back TickCount with the shell frame clock (60ths of a second).
- **S-MAC-06** [auto] `Random()` is the documented LCG; RGBForeColor
  quantizes 16-bit components via >>8.

## S-PY — Python runtime (`apps/pyrt.c`, `pyhost.h`, `upy/`)

- **S-PY-01** [auto] PYRT MUST set a 64 KB interpreter stack limit and run
  every app callback (build/draw/action/key/tick/opened/closed) inside an
  NLR fence — a raising Python app prints a traceback to the output pane
  and NEVER unwinds into the kernel.
- **S-PY-02** [auto] A PYAPP container is data, not code: it is never
  instantiated/executed as native (S-MOD-12), only handed to PYRT as
  source bytes.
- **S-PY-03** [auto] The loaded app must subclass `uno.App` and bind
  module-global `app`; a script failing this contract reports cleanly.
- **S-PY-04** [auto] tick() return semantics: repaint unless an explicit
  falsey non-None is returned.
- **S-PY-05** [auto] Cached mp_obj_t values held across calls MUST be
  registered as GC roots (MP_REGISTER_ROOT_POINTER — the documented
  collection bug class).

## S-LIBC — freestanding libc/math (`pc64_libc.c`, `pc64_math.c`)

- **S-LIBC-01** [auto] `snprintf`/`vsnprintf` MUST always NUL-terminate
  when cap > 0 and return the would-be length (C99), supporting
  d/i/u/x/X/o/c/s/p/%% with l/ll/z width and 0/-/+/# flags; float
  conversions print the literal "<flt>" (no dtoa by design); integer
  precision is parsed but ignored (documented).
- **S-LIBC-02** [auto] `malloc` is first-fit over a single static 32 MB
  arena, 16-byte-aligned payloads; malloc(0) returns a valid 1-byte
  allocation; OOM returns NULL (plus injected failures under
  uno_dbg_oom_every).
- **S-LIBC-03** [auto] `free` MUST reject out-of-arena pointers and
  double-frees (no-op, not corruption), and coalesce both neighbours;
  `calloc` is overflow-checked.
- **S-LIBC-04** [auto] `realloc` grows/shrinks correctly; NOTE: the copy
  uses the old BLOCK size (>= the caller's logical size), so grown buffers
  can carry stale tail bytes — harmless but pinned by test.
- **S-LIBC-05** [auto] `memcpy` word-copies only when src/dst share
  alignment phase; `memmove` is byte-wise but overlap-safe in both
  directions; `memset` word-fills after aligning.
- **S-LIBC-06** [auto] `pc64_math.c` is demo-grade (~1e-5): sinf returns
  0.0 for |x| >= 1e6 or non-finite input; floorf/ceilf pass through
  |x| > ~2.1e9 (int-cast UB guard); powf with negative base is
  integer-exponent-only correct. Contracts pin these documented bounds.
- **S-LIBC-07** [auto] The RAM store (`pc64_io.c`) holds at most 24 files
  of 256 KB with 32-char names; FSClose commits the whole buffer and
  mirrors to the first writable native FAT volume.
- **S-LIBC-08** [auto] Toolbox square-voice audio is most-recent-note-wins
  on the single PIT ch2 voice, owner-tracked.

---

## S-3D — uno3d software 3D pipeline (`../uno3d`)

Matrix/vertex transform state is file-static with no accessors, so contracts
are checked end-to-end through the software backend + `u3d_last_tris()`.

- **S-3D-01** [auto] `u3d_use_backend(&u3d_backend_soft)` MUST make
  `u3d_backend_name()` report `"soft"`.
- **S-3D-02** [auto] The software backend's caps MUST be exactly
  `U3D_CAP_ZBUFFER|U3D_CAP_GOURAUD` (no `U3D_CAP_HW`).
- **S-3D-03** [auto] A front-facing triangle in front of the near plane MUST
  rasterise (`u3d_last_tris()==1`).
- **S-3D-04** [auto] Back-face culling MUST hold: of a triangle and its
  reversed winding, exactly one rasterises.
- **S-3D-05** [auto] A triangle entirely behind the near plane MUST contribute
  zero rasterised triangles (near-clip rejects it).
- **S-3D-06** [manual] Gouraud interpolation and depth ordering produce the
  right pixels (needs fb inspection / a display).

## S-MEDIA — unomedia audio decoders (`../unomedia`, kernel-linked half)

The audio decoders (WAV/MIDI/MP3/AAC) are linked into the kernel; the image
decoders ship inside PHOTOS.UNO and are host-tested only.

- **S-MEDIA-01** [auto] A valid WAV MUST open with correct `format`/`rate`/
  `channels` metadata. check: hand-built 16-bit mono 8 kHz header.
- **S-MEDIA-02** [auto] Integer-PCM WAV decode MUST be sample-exact (lossless).
- **S-MEDIA-03** [auto] A minimal type-0 SMF MUST open as MIDI, 44.1 kHz stereo.
- **S-MEDIA-04** [auto] MIDI synthesis MUST actually sound (peak amplitude
  above a floor, not silence).
- **S-MEDIA-05** [auto] A real MPEG-1 Layer III clip MUST open as MP3 and
  decode >0 frames with no error. (dB-accuracy vs ffmpeg is host-only.)
- **S-MEDIA-06** [auto] A real ADTS AAC-LC clip MUST open as AAC and decode
  >0 frames.
- **S-MEDIA-07** [auto] Garbage input MUST be rejected cleanly (open returns 0,
  no crash, no hang).
- **S-MEDIA-08** [manual] Image decoders (PNG/JPEG/…) live in PHOTOS.UNO
  (module), not the kernel — host-tested.

## S-AI — Studio AI assistant (live checks; SKIP without a link)

- **S-AI-01** [auto, live] The assistant's transport MUST reach the provider:
  DNS-resolve `api.anthropic.com` and complete a CA-validated TLS ≥ 1.2
  handshake to :443 with the real SNI (`tls_connect_ca` — the exact call
  `studio_ai.c` makes). Proves end-to-end reachability including certs.
  SKIP with a named reason when no NIC/link/lease exists.
- **S-AI-02** [auto, live] A request → response round-trip MUST complete
  through that same pipe: an HTTPS exchange with the provider returning an
  HTTP status line and a non-empty body. No API key is on a test stick, so
  the expected answer is the provider's 401 + JSON error body — which is the
  round-trip proof; only transport failures fail the contract. (The render
  half lives in STUDIO.UNO and stays app-tested; keys live in plaintext
  `AI.CFG`, S-STUDIO-09.)

## S-INT — interactive checks (operator-confirmed; `interactive` opt-in)

The paths synthetic injection cannot prove. Run only when the STRESS.CFG
`interactive` key is set (the flasher's "include interactive tests" box), so an
unattended batch never blocks. Each wait is bounded (~25 s) and records SKIP on
a timeout rather than hanging.

- **S-INT-01** [interactive] The PHYSICAL keyboard MUST reach the OS on this
  machine: a prompted keypress arrives through `poll_keyboard`→`map_key` (the
  same funnel injection uses, so only a human press proves the firmware/HID
  bring-up works here — the class of bug that hit the Surface + Intel-Mac
  keyboards). Latched via the debug capture hook `uno_pc64_dbg_key_wait`.
- **S-INT-02** [interactive] The DISPLAY MUST show correct colour + legible
  text: labelled red/green/blue bars drawn straight to the framebuffer (the fb
  colour byte order + the text path, both per-machine trouble this session),
  confirmed Y/N by the operator.
- **S-INT-03** [interactive, STUB] Audible output. Deferred: the PCM DMA ring is
  fed by the main-loop audio pump, which doesn't run during the blocking suite,
  and the codecs are metal-pending. Becomes real once audio is confirmed on metal.

> **SPECTEST coverage (2026-07-21):** the conformance suite is organised into
> selectable AREAS — **storage · system · frameworks · apps · network** (plus the
> opt-in **interactive** area) — armed as bare `spec` (all) or `spec=<areas>`.
> It runs 58 [auto] checks + deliberate SKIPs across FAT/NET/FONT/LIBC/JS/UUI/3D/
> SND/MEDIA/WRITE/MUSIC/STUDIO/PY/DBG, exercising the toolkits (unoui widget
> lifecycle + events, uno3d raster, unosound sequencer), the audio decoders (WAV
> sample-exact, MIDI/MP3/AAC), the Editor model (insert/wrap/find/replace + UWD &
> TXT round-trips via test hooks in `pc64_write.c`), Music decode via
> `pc64_media_open`, Studio file save/load, and **Python-on-metal** (loads
> PYRT.UNO, runs a `uno.App` snippet, verifies its `uno.write` side effect).
> UnoC compile/build are UI-only in STUDIO.UNO → host-tested (`tools/ucc_test.c`);
> WiFi/LAN/AI are SKIP-pending-networking; the interactive area (S-INT) adds the
> human-confirmed keyboard + display checks.

---

## Fix status (2026-07-21 autonomous pass)

Several divergences below were FIXED this session and are now regression-checked
by SPECTEST on real hardware (`pc64_spectest.c`, STRESS.CFG `spec`):
- **S-FAT-28** FIXED — old chain freed only after the new chain + entry commit.
  SPECTEST S-FAT-28 (overwrite-bigger, read-back-exact) passes.
- **S-NET-08** FIXED — `net_ping` copies the dst into a static buffer.
- **S-NET-19** FIXED — DNS response transaction id verified.
- **S-FONT-09 / F7** FIXED — `text_pen`/`prov_glyph` multiply instead of `x<<6`.
- **S-MOD-12** FIXED — 64-bit size compare in `uno_mod_load_pyapp` + the .UNO loader.
- **S-UUI-04** FIXED — widget overflow returns a scratch cell, never a live slot.
- **S-INST-07** FIXED — boot-entry device-path length checked before memcpy.

New divergences FOUND this session (SPECTEST caught them on metal/QEMU):
- **S-LIBC-06** FIXED (2026-07-21) — `pc64_libc.c vsnprintf`: a TRUNCATING `%s`
  hung the machine. Root cause was NOT UBSan: the `PUT(ch)` macro gated the
  whole `buf[o]=ch; o++` on `o+1<cap`, and `%s` called it `PUT(*s++)`, so once
  the buffer filled the `s++` side effect stopped firing and `while(*s)` spun
  forever on the same byte (the `%f` "<flt>" path had the same latent bug). Fix:
  `PUT` now evaluates its argument exactly once, before the space check. SPECTEST
  S-LIBC-06 now EXECUTES the truncating case (reaching the assert proves no hang);
  verified on host and in QEMU.
- **S-JS-05/07** — `js.c`: `document.write(number)` outputs correctly but a
  string literal / `"a"+"b"` does not reach `out` (numeric coercion or the
  string-literal truncation noted below). Only the safety contract (clean
  return, no fault) is asserted.

## Suspected divergences

Every `NOTE:` above, collected. Line numbers are as-read on branch
`pc64-debug-stress` and may drift; the function name is the anchor.
(Items marked FIXED above are struck from active status but kept for history.)

### Storage
- **S-FAT-20** — `fat.c pack83` (~365): over-length 8.3 components silently
  truncate, so distinct long names alias to one entry. Intended: reject.
- **S-FAT-28** — `fat.c uno_fat_write` (~651-674): old chain freed BEFORE
  the new one is allocated; ENOSPC mid-write leaves the directory entry
  pointing at freed clusters (file corrupted, size stale).
- **S-FAT-29** — `fat.c uno_fat_write` (~651): no directory-attribute check
  after `dir_find`; writing a file over a subdirectory name destroys the
  directory.
- **S-FAT-30** — `fat.c dir_alloc_slot` (~715): grow-branch gated on
  `v->fat32`, so a full FAT16 SUBDIRECTORY can never grow.
- **S-FAT-31** — `fat.c` mount fields (~112-143, 298): fat_start /
  data_start / clus_lba computed in uint32 from `(uint32_t)start`; volumes
  past 2 TiB mis-address; the device-bounds sanity check compares the
  truncated value.
- **S-FAT-32** — `fat.c fat_get` (~270,275): cache_get failure returns the
  EOC sentinel — a transient IO error silently truncates a file instead of
  erroring.
- **S-FAT-33** — `fat.c dir_next`/chain walkers (~357): no upper-bound
  validation of a next-cluster value below the EOC threshold; only
  MAXCLUS_WALK bounds a corrupt chain.
- **S-FAT-34** — `fat.c seq_hit/seq_save` (~564-577): sequential-cursor
  path compare truncates at 79 chars (false-match possible in principle).
- **S-BLK-11** — `ahci.c` (~137-147) and `nvme.c` (~168-178): no
  FLUSH-CACHE / NVMe Flush after writes; a drive's volatile write cache can
  lose synced telemetry on power cut. The single most impactful gap for a
  crash-report OS.
- **S-BLK-12** — `ahci.c` port start (~178-180): sets FRE then ST without
  waiting for PxCMD.FR (spec ordering); tolerated polled, latent on slow
  HBAs.
- **S-BLK-15** — `nvme.c` completion wait (~104): a timed-out command
  leaves SQ tail/doorbell advanced with no reset path — subsequent
  commands desync phase.
- **S-BLK-16** — `nvme.c` init (~234-237): Set-Features (Number of Queues)
  status ignored; a controller granting 0 I/O queues would still proceed.
- **S-BLK-18** — `sdhci.c` (~264): byte-addressed argument computed as
  `(uint32_t)(lba*512)` — wraps past 4 GB.
- **S-BLK-20** — `blkdev.c fw_scan` (~111-114): controller dedup skipped
  when the firmware device path has no PCI node (pdev = -1); a disk can be
  double-registered (masked by pc64_fs serial dedup).
- **S-FS-07** — `pc64_fs.c` (~70): native-vs-firmware volume dedup keys on
  BPB serial and skips serial==0 — such a volume lists twice.
- **S-FS-08** — `pc64_fs.c uno_fs_list_get` (~133): no fw_dead check; a
  pre-detach firmware listing snapshot stays readable until remap.
- **S-FS-09** — `pc64_fs.c list_begin` (~116-119): listing silently capped
  at 64 names of <= 12 chars, no truncation signal.

### Boot / debug harness
- **S-BOOT-13** — `uefi_main.c try_detach` (~730) [METAL-FINDINGS F8]:
  detach gate tests "some UnoDOS volume natively reachable", not "the
  volume we BOOTED from survives" — a USB-booted system with an internal
  install detaches away its own boot volume.
- **S-DBG-16** — `uno_debug.c next_seq` (~628-635): report numbering is
  dir-count+1; deleting an old report makes the next one OVERWRITE an
  existing number.
- **S-DBG-21** — `uno_debug.c uno_dbg_watchdog_start` (~1034-1051)
  [METAL-FINDINGS F9]: only one seeded heartbeat at arm; a machine with
  ~250 ms presents can take > 20 s to the first shell heartbeat →
  spurious HG + reset loop.

### Module loader
- **S-MOD-07** — `pc64_modload.c mod_load` (~396-402), `uno_mod_load_uui`
  (~426), `uno_mod_load_pyrt` (~441): tier-flag refusals happen AFTER a
  successful instantiate and never free the pages — leak per refused
  load.
- **S-MOD-12** — `pc64_modload.c uno_mod_load_pyapp` (~465): size check
  `(long)sizeof *h + h->file_size > n` wraps in 32-bit `long` (LLP64);
  file_size ~0xFFFFFFFF passes, mod_crc32 runs with a negative length
  (skipped), `*len` becomes -1. Intended: 64-bit `file_size <= n - 48` as
  the mod_instantiate path already does.

### Graphics / UI
- **S-FONT-05** — `pc64_font.c` (~276-311): bytes 128..255 render as blank
  advances (ASCII-only cache, no UTF-8, no .notdef) — accented text
  silently vanishes in TTF-skinned UI.
- **S-FONT-09** — `pc64_font.c` (~307) [METAL-FINDINGS F7]: `x<<6` on a
  negative pen x is UB (UBSan #UD in debug); centered strings wider than
  the surface reach it; callers only partially audited.
- **S-UUI-04** — `unoui/unoui.c push` (~57-67): widget overflow returns
  `&win->w[63]` without nw++ or memset — silent slot-63 corruption with
  stale id/edit/canvas fields.
- **S-UUI-10** — `unoui/unoui_input.c clamp_win` (~162-168): drag-commit
  clamps to screen_h-16, not the work area — windows can be parked under
  the always-on-top taskbar.
- **S-UUI-13** — `unoui_input.c` (~625,637) vs `unoui.c d_popup` (~817):
  popup hit rows anchored at y+2, drawn at y+3 — 1-px hover/commit
  disagreement band.
- **S-UUI-18** — `pc64_uui.c remove_win` (~820-829) and
  `unoui_input.c unoui_ui_add` (~48-53): window removal/insert fixes only
  focus_win (and only at the top end); cap/hot/popup indices (and
  mid-list focus) go stale — focus lands on the taskbar after closing the
  focused app; a live capture can dereference the wrong window.
- **S-SHELL-09** — `pc64_uui.c` icon drag (~1670-1726): a sub-3-px
  click-drag nudges the icon rect and never restores it — icons creep up
  to 3 px per click.

### Input / USB
- **S-INP-05** — `i2c_hid.c` [METAL-FINDINGS F4]: the PCI-BDF-guess probe
  binds the trackpad on NO tested machine (X1: 2 controllers/0 devices,
  Surface: 3/0, Yoga: 0 controllers); the ACPI PNP0C50 enumeration the fix
  plan calls for is unimplemented. Knock-on: F6 — no laptop detaches.
- **S-USB-07** — `xhci.c enumerate_port` (~283): EP0 MPS stays
  mps_for_speed(8) for full-speed devices; the descriptor's real MPS is
  never programmed back (fine for boot HID, short of full enumeration).

### Network
- **S-NET-08** — `net.c net_ping` (~163): stores the caller's POINTER for
  the deferred post-ARP retry; a dead stack buffer is re-read in
  net_poll. Intended: copy 4 bytes.
- **S-NET-15** — FIXED 2026-07-21 (`net.c tcp_input`): rcv_nxt now advances
  only past bytes actually stored, the truncated tail is taken from the
  peer's retransmit (with overlap trim), and FIN counts only at exactly
  rcv_nxt. Was: full-dlen ACK — bytes past the buffer acknowledged-and-lost;
  surfaced as every real-host TLS handshake dying (S-AI-01) once the live
  checks landed.
- **S-NET-17** — `net.c` DHCP (~454-460): no retransmit/timeout anywhere
  in the DISCOVER/REQUEST exchange — one lost packet stalls
  net_dhcp_done() forever.
- **S-NET-19** — `net.c net_dns_query` (~580): response transaction ID
  never verified (only the source port filters).
- **S-NET-23** — FIXED 2026-07-21 (`net.c tcp_out`): the window is now the
  rxq's actual free space (and `net_tcp_recv` sends a window update when a
  collapsed window reopens); rxq grown 2048 → 8192 so a CA certificate
  flight fits. Was: fixed 4096 over a 2048-byte rxq.
- **S-NET-24** — `net.c ip_recv` (~514): limited-broadcast test checks
  only the first two octets (255.255.x.x accepted).
- **S-NET-25** — `net.c tcp_tick` (~392-407) / `arp_put` (~61): TCP
  retransmits forever with no backoff or give-up; a full ARP cache always
  evicts slot 0.
- **S-NET-26** — `net.c net_tcp_connect` (~307): always returns 0, making
  `pc64_http.c`'s `< 0` check (~109) dead code.
- **S-NIC-07** — `e1000.c` (~48): REG_RAH0 defined as 0x5408 (RAL1 on
  8254x) vs 0x5404 in e1000e/igb — the AV bit lands in the wrong register;
  masked today only by promiscuous mode.
- **S-WIFI-08** — `iwlwifi.c find_and_join` (~1318-1330), `rtwifi.c`
  (~244, 639), `mrvlwifi.c connect` (~460): scan/auth/assoc unimplemented
  on all three drivers — a real join cannot complete; iwlwifi's ceiling is
  firmware ALIVE + MVM init.
- **S-WIFI-09** — `iwlwifi.c mvm_init_unified` (~1068-1071) and
  `rtwifi.c`: g_mac never populated from NVM/hardware — 802.11 frames
  would source from 00:00:00:00:00:00 (mrvlwifi does read a real MAC).

### Apps
- **S-WRITE-07** — `pc64_write.c lay_push` (~140): layout silently stops
  at 4096 wrapped lines; content beyond is invisible in the view with no
  indicator (buffer/save unaffected).
- **S-JS-05** — `js.c` (~158-163 string literals, ~385 sc_def, ~413
  BT_PUSH, ~439 indexed store): every structural limit silently
  truncates/drops instead of raising a script error (memory-safe but
  wrong results).
- **S-JS-06** — `js.c ar_alloc` (~25) + parse loops (~337, ~555): arena
  OOM returns the arena base and only sets g_oom, which the parser loops
  do not check — overlapping AST nodes (bounded garbage) before the
  deferred OOM report.
- **S-STUDIO-07** — `ucc_x64.c alu` (~217-229): runtime idiv has no
  INT64_MIN/-1 guard while the constant folder guards the same expression
  (`ucc.c` ~842-848) — compile-time/runtime divergence; the module faults
  with #DE.
- **S-MAC-05** — `mac_compat.c TickCount` (~248): returns a per-call
  counter, not 60ths of a second — Toolbox-tier apps pace by call
  frequency, not time.
- **S-CLOCK-04** — `pc64_clock.c` (~129-131): coarse midnight carry
  (d=31/1 regardless of month) can misreport the displayed date by one
  day near month boundaries under large UTC offsets (documented,
  declination-only intent).
- **apps/notepad.c** (~202, no contract): insert filter `ch >= 32` on a
  SIGNED char drops bytes >= 128 (non-ASCII input silently ignored).

### Installer
- **S-INST-07** — `installer.c write_boot_entry` (~451-468): the
  firmware-supplied device-path prefix is memcpy'd into `lo[1024]` with no
  size check — a long legitimate path overflows.
- **S-INST-08** — `installer.c` clone header patch (~623-631): crc32 runs
  over the source GPT's own HeaderSize field unchecked — a bogus
  HeaderSize reads past the 512-byte buffer.
- **S-INST-09** — `installer.c` clone (~614-634): no read-back verify of
  the cloned region despite a known mid-write corruption incident;
  WriteBlocks success is trusted.

### libc
- **S-LIBC-04** — `pc64_libc.c realloc` (~220-230): copies the old BLOCK
  size (not the logical size), so grown buffers carry stale tail bytes;
  in-place grow within the block returns uninitialized tail. Harmless
  today; pinned by test.

### Minor code-health notes (no contract)
- `iwlwifi.c` (~606, ~965, ~1162): dead/garbled expressions — a seq
  computed with `CMDQ_N & 0`, an always-true `|| 1` condition (PNVM base
  never actually programmed on gen3), and a precedence-broken queue-mask
  write that always stores 0.
- `igb.c` (~218): dead `len < 0` check on a u16-derived value.
- `blkdev.c` (~119): `sectors = LastBlock + 1` unguarded (wraps only on an
  absurd LastBlock of UINT64_MAX).
- `hdaudio.c cmd_corb` (~122): no solicited-response validation — an
  unsolicited codec response can be taken as a command reply (bounded by
  SPIN_MAX).
