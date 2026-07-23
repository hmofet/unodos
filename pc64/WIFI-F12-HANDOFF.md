# Intel AX201 WiFi (F12) bring-up — handoff

Status: 2026-07-23 (round 17). Target: Lenovo ThinkPad X13 Yoga, Intel **AX201** (CNVi,
gen2 22000-family, QuZ-a0-hr-b0). Firmware `QuZ-a0-hr-b0-77.ucode`. Ethernet is
fully solved; this is the WiFi tail.

## TL;DR — the diagnosis was REFRAMED (Round 15)

For 14 rounds the failure was read as *"the boot ROM never starts / the device
does zero DMA"* — based only on `FH_INT` / `UCODE_LOAD_STATUS` / `CPU_INIT_RUN`
all reading 0. **That was wrong.** The internal-CPU registers were never read.
Live reads over the URC `iwl prr` verb show the boot-ROM processors **run and
then PARK**:

| PRPH reg | value | meaning |
|---|---|---|
| `0xa038c0` UMAG_SB_CPU_1_STATUS | `0x00005754` | secure-boot CPU executing (nonzero) |
| `0xa038c4` UMAG_SB_CPU_2_STATUS | `0x00000003` | nonzero |
| `0xa05c18` UMAC_CURRENT_PC | `0x8047378e` | real code addr, **stable** across samples |
| `0xa05c1c` LMAC1_CURRENT_PC | `0x004bf5da` | real code addr, **stable** |
| `0xa05c20` LMAC2_PC | `0` | expected (single-LMAC part) |

Distinct, stable, real addresses (NOT the `a5a5a5a0`/`d55555d5` garbage a dead
PRPH block returns). So the CPU started, executed, and is **stuck at a fixed PC
before completing the firmware-load handshake**. `fh_after_kick=0` is a red
herring (it watches one FH channel, not the ROM's context-info DMA). This is a
**firmware-image / secure-boot** failure class, not a transport problem.

## Round 17 (2026-07-23) — three standing suspects RETIRED, method changed

Every remaining "prime suspect" in this document has now been checked against
ground truth rather than recalled source, and all three are **correct in our
driver**. Do not re-chase them:

1. **`UREG_CPU_INIT_RUN` (PRPH `0xa05c44`) = 1 is REQUIRED, not spurious.**
   Round 16 proposed removing it as a gen3-only register absent from
   `iwl_pcie_ctxt_info_init()`. The working ftrace issues it on this exact card,
   right after the HPM LTR pair, exactly where we issue it. It lives in the
   *caller* tail (`iwl_trans_pcie_gen2_start_fw`, Linux 6.12
   `pcie/trans-gen2.c`), which is why reading `ctxt-info.c` alone makes it look
   bogus. Our whole kick tail (BA → `a0348c=0xf` → `a03480=88fa88fa` →
   `a05c44=1`) matches the trace op-for-op.
2. **MSI-X fw-load masks already match** `iwl_enable_fw_load_int_ctx_info()`:
   HW `~ALIVE` = `0xfffffffe`, FH = `0x0000fe00`. Confirmed both in the trace
   and in our own boot log.
3. **The ucode image and its section split are correct.**
   `fw-blobs/IWLAX201.UCO` is **byte-identical** to Debian's
   `iwlwifi-QuZ-a0-hr-b0-77.ucode` (md5 `df80001381d87035b7d270220cb73bd5`), and
   an independent parse (`tools/iwl_ucode_sections.py`) gives RT `num_sec=51`,
   `lmac=14 umac=15 paging=20`, separators at section index 14 and 30 — exactly
   what `place_fw_dram()` logs on metal. `struct iwl_context_info_dram` really is
   `umac[64], lmac[64], virtual_img[64]` and `ci_dram` matches. **This retires
   the "firmware-image / secure-boot" leading hypothesis above.**

One genuine divergence was found and is **benign**: Linux sets
`CSR_GP_CNTRL |= 0x04000000` (`RFKILL_WAKE_L1A_EN`) before the kick and we do
not (ours reads `0x08040005`, the trace `0x0c040005`). It is set inside
`iwl_enable_rfkill_int()` purely so a powered-down device can wake the PCIe bus
for RF-kill interrupts — it cannot park running firmware. Worth closing for
fidelity, not as a fix.

### Why the method changed

Sixteen rounds audited this load path one register at a time against recalled
Linux source. Each round "verified" a subsystem, moved the diagnosis, and missed
the divergence — and two of round 16's own suspects were wrong. Everything
reachable by inspection is now verified correct, so the divergence is in the
~250 accesses *before* the kick, where no one has looked systematically.

So: stop reading, start recording.

- **`iwl iotrace`** (new URC verb, UNO_DEBUG builds only) dumps a run-length
  folded, in-order log of every BAR32/BAR8/PRPH access the driver made during
  the last bring-up attempt. Armed fresh by each `iwl rerun`. ~250 lines.
- **`tools/iwl_iodiff.py`** aligns that dump against the ground-truth ftrace and
  prints MISSING / EXTRA / VALUE rows:

  ```
  python3 tools/iwl_iodiff.py ~/urc-yoga/session.log ~/iwl_from_yoga.txt
  ```

  It folds both sides the same way, windows around the chosen `CSR_CTXT_INFO_BA`
  kick (`--kick N`, default last), and aligns on kind+offset so a data
  difference surfaces as one VALUE row instead of desynchronising the diff.

### Concrete NEXT step

Boot a `UNO_DEBUG=1` build on the Yoga, `iwl rerun`, `iwl iotrace`, then run
`iwl_iodiff.py`. Work the MISSING rows before the kick first — those are things
Linux does to this silicon that we never do, and one of them is the park.

## Ruled out — do NOT re-chase

- **MSI-X config** — REQUIRED and DONE. Proven necessary via the ground-truth
  ftrace; the CPU only runs *because* of it (UREG_CHICK bit25 + IVAR table + FH/HW
  masks + PCI-config MSI-X).
- **IOMMU / VT-d** — TES + PMR both disabled on metal (`iwl dmar`), no effect.
- **Bus mastering** — confirmed on.
- **Cache coherency** — `wbinvd`+`mfence` before the kick (v7), no effect
  (discarded).
- **Context-info struct + control_flags** — byte-correct vs Linux master
  (`pcie/iwl-context-info.h`): `control_flags = 0x980` = `TFD_FORMAT_LONG(0x100)
  | CB_SIZE(8<<4) | RB_SIZE_4K(4<<9)`; struct layout matches field-for-field;
  dram order `umac/lmac/virtual` matches.
- **MMIO kick sequence** — matches the working Linux ftrace: interrupt masks
  (`io[0x280c]=fffffffe`, `io[0x2804]=fe00`) → `CTXT_INFO_BA` (`io[0x40]`, 64-bit)
  → LTR prph (`a0348c=0xf`, `a03480=88fa88fa`) → doorbell (`a05c44=1`).

## RETIRED in round 17 — former leading hypothesis (fw image / secure boot)

`SB` = Secure Boot; SB-CPU status nonzero + CPUs parked in *distinct* regions ⇒
the ROM runs secure-boot over the firmware sections we place in
`ctxt_info.dram` and halts. Prime suspect: **`place_fw_dram()`** (iwlwifi.c
~L1199) and the `.ucode` **section split** (LMAC / UMAC / CSS / paging via
`CPU_SEP`/`PAGE_SEP`). A wrong section boundary, a missing/misplaced **CSS
(secure-boot certificate) section**, or wrong section DATA fails auth exactly
like this.

## DONE in round 17 — the ucode-parse step this asked for

Dump our `.ucode` parser's section offsets/sizes/counts (the TLV parse feeding
`place_fw_dram`) and **diff against a Linux-side parse of the SAME
`iwlwifi-QuZ-a0-hr-b0-77.ucode`** (write a small TLV parser on devbuntu; compare
to `iwl_pcie_ctxt_info_init_fw_sec` in Linux `pcie/ctxt-info.c`). Confirm the
LMAC/UMAC/CSS split and each section's dram address match. The context-info and
fw sections live in host RAM (device DMA), so they are **invisible in the MMIO
ftrace** — a source/parse diff is the only way to see them. Note: `SB=0x5754`
and the parked PC are Intel-internal codes with **no open decode** — don't chase
those values directly.

## How to drive the Yoga (no reflash for register work)

- Yoga is booted on a debug build, on ethernet, dialed into the URC bridge on
  **devbuntu `192.168.2.100:5099`**.
- Bridge (must be a PERSISTENT run_in_background ssh; nohup/setsid die on close):
  `ssh devbuntu 'exec python3 ~/urc_bridge.py 5099'`. It tails `~/urc/cmd.txt`,
  logs frames to `~/urc/session.log`.
- Send a command: append a line, e.g.
  `ssh devbuntu 'echo "iwl prr a05c18" >> ~/urc/cmd.txt'`; read the reply in
  `~/urc/session.log`.
- **`iwl` verb** (`pc64/iwlwifi.c` `iwl_dbg_cmd`, ~L2292): `rerun` (full bring-up
  retry), `prr <hex>` / `prw <hex> <val>` (PRPH read/write), `csr <hex>` /
  `csw <hex> <val>` (CSR read/write), `dmar` (IOMMU probe), `status`. Live
  register archaeology, no reflash per experiment.
- Reflash only for driver-CODE changes: A/B kernel push over URC (`put` verb)
  works now (the `fat_alloc` O(1) fix); or physical reflash from
  `\\behemoth\unreplicated\unodos\pc64\`.

## Ground-truth artifacts

- **Working Linux `iwlwifi_io` ftrace** of a WORKING AX201 load on THIS Yoga:
  `devbuntu:~/iwl_from_yoga.txt` (950 reg ops). Capture tooling on devbuntu:
  `~/capture.sh` + `~/yoga_server.py` + `~/agent.py`. Recapture: boot Ubuntu on
  the Yoga, `unbind` → arm `events/iwlwifi_io` → `bind` (NOT module reload).
- Card identity: crf-id `0x3617`, cnv-id `0x20000302`, RF HR B3, hw_rev `0x351`,
  MAC `18:26:49:71:91:57`. Ubuntu uses API-77 ucode.

## Key files

- `pc64/iwlwifi.c` — the driver. `load_fw_gen2()` ~L1299 (kick sequence),
  `place_fw_dram()` ~L1199 (fw section placement — **prime suspect**), the
  ALIVE-timeout autopsy ~L1500s, `iwl_dbg_cmd()` ~L2292.
- `pc64/iwlwifi.h` — register/struct defs.
- `pc64/NETWORK.md` — firmware file layout + `WIFI.CFG`/`WIFI.TXT` creds.
- ESP firmware: `FIRMWARE\IWLAX201.UCO` (from Debian `firmware-iwlwifi`).

## Known remaining gap after ALIVE (for later)

Even once the ROM loads firmware, `find_and_join` is still MLME-scaffolded
(broadcast BSSID, no beacon parse / auth / assoc). A real association can't
complete until that tail is written. First get past F12 (ALIVE), then the MLME.
