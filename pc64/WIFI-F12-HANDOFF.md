# Intel AX201 WiFi (F12) bring-up — handoff

Status: 2026-07-28 (round 24 - **THE WPA2 CONNECTION COMPLETES ON METAL**.
auth -> assoc (AID 1) -> full 4-way handshake -> CCMP pairwise key + GTK
installed -> station authorized. Remaining: no data path demonstrated yet, and
the AP choice must be forced with `iwl pick`).

## Round 24 (2026-07-28) — TX fixed; the WPA2 connection COMPLETES on metal

Five independently-necessary defects were found and fixed this round. Each one
alone would have blocked association, and each was confirmed on metal.

### 1. TX never transmitted — the missing `iwl_cmd_header`

`tx_enqueue()` built the TX buffer starting at `iwl_tx_cmd_gen2`. The real gen2
layout (`iwl_txq_gen2_build_tx`, which copies from `&dev_cmd->hdr`) is:

```
[ iwl_cmd_header 4 B ][ iwl_tx_cmd_gen2 20 B ][ 802.11 hdr ][pad][ body ]
```

Without the header the fw read the frame LENGTH as `{cmd, group_id}` - a 30-byte
auth frame arrived as cmd `0x1e` group 0 = **TXPATH_FLUSH**. The card was being
told to flush its TX path every time we asked it to transmit. `send_cmd()` has
always built this header correctly, which is exactly why every command worked
and TX never did.

Four more TX defects rode behind it, all from `iwl_mvm_set_tx_params` +
`iwl_txq_gen2_tx`:
- `hdr.sequence` must carry `QUEUE_TO_SEQ(qid) | INDEX_TO_SEQ(idx)`.
- the byte-count entry is `tx_cmd->len`, the OVER-THE-AIR length, not the TFD total.
- `IWL_TX_FLAGS_CMD_RATE` must be set or the fw consults a rate-scaling table we
  never configure (no `TLC_MNG_CONFIG_CMD`).
- `IWL_TX_FLAGS_ENCRYPT_DIS` must be set until keys exist.
- this fw reports **TX_CMD cmd_ver 9**, so `rate_n_flags` is the **v2** encoding
  (bits 0-3 a rate INDEX, 8-10 modulation type, 14-15 antenna). 1 Mbps CCK on
  ant A is `0x4000`; the v1 value `0x420a` decodes as HT garbage.

Success signal: `rxpkt grp=0 cmd=1c` (REPLY_TX), which had never once appeared.

### 2. `g_mac` was never populated — our MAC was 00:00:00:00:00:00

Declared and read in six places (auth frame addr2, `MAC_CONFIG.local_mld_addr`,
`LINK_CONFIG.local_link_addr`, the supplicant's own-address, `mgmt_capture`'s
addr1 match) and **written nowhere**.

This produced a genuinely misleading success signal: the AP's hardware ACKs any
unicast frame addressed to it regardless of source, so `TX_STATUS_SUCCESS` was
real but meant only "the radio heard you", not "the AP accepted your identity".
No AP runs an auth exchange with an all-zero station, and `mgmt_capture` could
never have matched a reply anyway.

`read_mac_addr()` mirrors `iwl_set_hw_address_from_csr`: STRAP pair first, OTP
fallback, `iwl_flip_hw_address` byte order. **The base is Linux's per-config
`cfg->mac_addr_from_csr` = 0x380 on AX200/AX201-class 22000 parts**, and 0x30380
only from AX210 on - keying it off "family >= 22000" reads an undecoded address
and returns 0xffffffff. Verified live with `iwl csr` before reflashing:
`0x380=0x18264971`, `0x384=0x00009157` -> **18:26:49:71:91:57**, matching the
card identity. STRAP (0x388/0x38c) is empty on this part.

### 3. EAPOL was handed the wrong pointer

`wpa_sm_rx_eapol()` parses from the **EAPOL header** (first guard:
`frame[1] != 0x03`). iwlwifi passed the whole 802.11 frame, so `frame[1]` was the
second byte of the frame control and it returned 0 instantly - "sm state 0,
reply 0" while the AP retried and then deauthed. Fixed by skipping the MAC
header plus the 8-byte LLC/SNAP. **`rtwifi.c` has the identical bug at two call
sites** (`handle_eapol(frame,plen)`); `mrvlwifi.c` is correct. rtwifi was left
alone - no hardware here to verify against - but it is a real latent bug.

### 4. The supplicant was armed AFTER the assoc request

The AP sends EAPOL 1/4 the instant it has sent the association response, so
message 1 is handled **inside `mvm_assoc()`'s own RX wait**. Arming afterwards
re-ran `wpa_sm_init()`, resetting the state machine and generating a fresh
SNonce - invalidating the 2/4 already on the air. Split into `wpa_arm()` (called
BEFORE `mvm_assoc()` at all three call sites) and `assoc_mark_associated()`
(fw-side only, never touches the supplicant).

### 5. Assoc request and EAPOL 2/4 carried DIFFERENT RSN IEs

```
assoc request (mvm_assoc)      30 12 ... = RSNE body 18, NO capabilities field
EAPOL 2/4 (wpa_build_rsn_ie)   30 14 ... = RSNE body 20, WITH capabilities
```

802.11 requires them to be byte-identical; the authenticator compares them and
deauthenticates on any difference. `mvm_assoc()` now calls `wpa_build_rsn_ie()`
so they cannot drift. Confirmed on metal: the assoc frame grew 74 -> 76 bytes.

### Where it stands

Intermediate state after fixes 1-4, with the RSN IEs still mismatched - the
handshake started but died the moment our 2/4 arrived:

```
auth  -> 0    Open-System accepted   (mgmt subtype=11, mine=1)
assoc -> 1    AID 1                  (mgmt subtype=1, 197 bytes)
EAPOL in (99 bytes, type=03 desc=02 ki=008a 1/4) -> reply 121   (2/4 built + TXed)
...then the AP deauths (mgmt subtype=12)
```

**VERIFIED COMPLETE** once the RSN IE fix (5) was tested end to end:

```
EAPOL in (99 bytes,  ki=008a 1/4) -> state 1, reply 121   2/4 sent
EAPOL in (163 bytes, ki=13ca 3/4) -> state 2, reply 99    3/4 verified, 4/4 sent
SEC_KEY idx=0 mcast=0 flags=02                            pairwise CCMP key
SEC_KEY idx=2 mcast=1 flags=4a                            GTK
STA_CONFIG sta=0 link=0 aid=1 auth=1                      station authorized
4-way handshake DONE - CCMP keys installed (gtk_len=16 idx=2)
```

`ki=13ca` = version 2 | Pairwise | Install | ACK | MIC | Secure | Encrypted, a
genuine 3/4. The RSN IE mismatch really was the last blocker.

### What is NOT done

1. **No data path has been demonstrated.** Association and key install are
   proven; nothing has been sent or received over the encrypted link. The box
   runs `net-eth-only`, so the IP stack is not bound to the WiFi NIC. Getting a
   DHCP lease and a ping over WiFi is the next slice.
2. **`iwl status` still prints the pre-`mvm` gate string** even when joined -
   a stale status message, not a connection fault, but it should report the
   real association state.
3. **A stock boot is still a coin flip on AP choice** (see the BSSID trap
   below). `find_and_join()` uses `scan_pick()`, which picks by beacon count.

### Traps and rig notes learned this round

- **`iwl pick <n>` must be used BEFORE `mld 1..6`, never after.** Re-running
  `mld 3`/`mld 4` on existing contexts (PHY_CONTEXT ADD on a live context, a
  phy-bind on an active link) asserts the fw. The struct comment is explicit:
  phy id / link address / listen_lmac "can be modified only until the link
  becomes active".
- **Which NimmuNet BSSID the scan picks varies run to run, and it matters.**
  Auth has only ever succeeded against `e8:d3:eb:51:8c:8f`; it failed twice
  against `e8:d3:eb:47:4e:cf`. Control the AP choice with `iwl pick`, do not let
  `scan_pick()` decide.
- **Batch `auth`/`assoc`/`eapol` as consecutive URC lines.** One command at a
  time is ~10-20 s apart and the AP times out its auth state; batching is what
  first produced a successful assoc response.
- **Never send SESSION_PROTECTION twice** (a second ADD gives LMAC
  `error_id=0x4216`), and after ANY LMAC assert `iwl rerun` does not restore the
  scanner - only a full `reboot` does.
- The ethernet dongle stalls its DHCP lease on most reboots and needs a physical
  re-seat; on 2026-07-28 it progressed to "eth failed" and would not initialise.

### Correct metal procedure

`rerun` -> `mvm 1`/`2`/`e`/`4` -> `scan` -> **`pick <n>`** -> `mld 1`..`6` ->
then `auth`, `assoc`, `eapol` appended together. `iwl caps` dumps capability bits,
antenna masks and our MAC on a running box; `iwl fwerr` after any assert.
Target: Lenovo ThinkPad X13 Yoga, Intel **AX201** (CNVi, gen2 22000-family,
QuZ-a0-hr-b0). Firmware `QuZ-a0-hr-b0-77.ucode`. Ethernet is fully solved; this
is the WiFi tail.

## Round 23 (2026-07-27) — the link API is IMPLEMENTED (branch `iwlwifi-linkapi`), metal-pending

Round 22's plan is written. `pc64/iwlwifi.c` now has the MAC_CONF-group
association path and picks it whenever the fw advertises **capa 110**
(`IWL_UCODE_TLV_CAPA_MLD_API_SUPPORT`), falling back to the legacy chain
otherwise (AX200 and older still work).

**Command versions actually advertised by `IWLAX201.UCO`** (`tools/iwl_cmd_versions.py`):
`MAC_CONF 0x08` = **ver 1**; `0x09`, `0x0a` and `0x05` are **absent from the
version TLV**, i.e. default ver 1. So the VER_1 layouts (kernel v6.7) are the
right ones, and each struct carries a compile-time size assert because this fw
length-checks every command:

| cmd | struct | size |
|---|---|---|
| `MAC_CONFIG_CMD` 0x03/0x08 | `iwl_mac_config_cmd` | 52 B |
| `LINK_CONFIG_CMD` 0x03/0x09 | `iwl_link_config_cmd` | 208 B |
| `STA_CONFIG_CMD` 0x03/0x0a | `iwl_mvm_sta_cfg_cmd` | 96 B |
| `SEC_KEY_CMD` 0x05/0x18 | `iwl_sec_key_cmd` | 80 B |

**New `assoc_setup()` order** (mirrors iwlmvm `mld-mac.c` / `link.c` / `mld-sta.c`):
`MAC_CONFIG(ADD)` → `LINK_CONFIG(ADD, phy=FW_CTXT_INVALID)` → `PHY_CONTEXT(ADD)`
→ `LINK_CONFIG(MODIFY: active + phy + rates + qos + beacon timing)` →
`STA_CONFIG(AP peer)` → `SCD_QUEUE_CONFIG`. **No BINDING, no TIME_QUOTA** — the
link replaces both.

**Correction to round 22's step 4.** `iwl_mvm_get_session_prot_id` uses the raw
`mvmvif->id` for SESSION_PROTECTION_CMD **ver < 2** and the `fw_link_id` only
from ver 2 up. This fw's 0x05 is ver 1, so the mac id is correct — and with one
mac and one link both values are **0** anyway. The id was never the bug: the
missing LINK was. Also worth knowing: the driver *chooses* `fw_link_id` itself
(`iwl_mvm_get_free_fw_link_id`, ffz over a driver-side bitmap); the fw does not
hand one back, so ours is always 0.

Other pieces that had to move with it:
- the 4-way handshake installs CCMP keys through **`SEC_KEY_CMD`** (DATA_PATH
  0x18) and re-sends `STA_CONFIG` authorized, instead of `ADD_STA_KEY` +
  `ADD_STA` MODIFY (those belong to the ADD_STA world).
- `MAC_CONFIG`'s filter flags are a **different enum** from the legacy command:
  ACCEPT_BEACON is BIT(3) here where the legacy `MAC_CONTEXT_CMD` used
  IN_BEACON BIT(6).
- the scan now records **beacon interval + DTIM period** so `LINK_CONFIG` carries
  real beacon timing.
- `find_and_join()` is no longer MLME-scaffolded: real scan → pick → setup →
  auth window → Open-System auth → assoc → supplicant armed.

**New bisect verb: `iwl mld <1-9>`** (mirrors `iwl mvm <n>`, same
one-command-per-round-trip rationale — a wedge eats in-flight URC log frames):
`1` MAC_CONFIG, `2` LINK_CONFIG ADD, `3` PHY_CONTEXT, `4` LINK_CONFIG
MODIFY(active), `5` STA_CONFIG, `6` txq_alloc, `7` SESSION_PROTECTION, `8` auth,
`9` assoc. Run `iwl scan` first so steps 3-5 have a real BSSID/channel.

### METAL RESULT — SESSION_PROTECTION is ACCEPTED; the cause was RLC_CONFIG_CMD

Driven live over URC. Every command is now accepted (`csr2808=00000000`):

| step | command | result |
|---|---|---|
| `mld 1` | `MAC_CONFIG_CMD` 52 B | ok |
| `mld 2` | `LINK_CONFIG_CMD` ADD 208 B | ok |
| `mld 3` | `PHY_CONTEXT_CMD` + **`RLC_CONFIG_CMD`** | ok |
| `mld 4` | `LINK_CONFIG` phy-bind **then** activate | ok |
| `mld 5` | `STA_CONFIG_CMD` 96 B | ok |
| `mld 6` | `SCD_QUEUE_CONFIG` | qid=1 |
| `mld 7` | **`SESSION_PROTECTION_CMD`** | **ok — `csr2808=0`** |

**The fix was `RLC_CONFIG_CMD` (DATA_PATH 0x08, 32 B, cmd ver 2 in the wide
header), sent immediately after every `PHY_CONTEXT_CMD`.** It binds the radio
chain config to the PHY context - the layer between "a PHY context exists" and
"the LMAC can schedule on it", which is exactly where the `data1=0x400` assert
sat. Proof it is the airtime fix, not a coincidence: auth without any session
protection saw **0 RX packets in 800 ms** before RLC and **20 RX packets** after.
The radio was never on-channel; now it is.

**How it was found (method note).** Round 22 summarised the working Linux trace
as per-command COUNTS. Reading the same file's **ordered** sequence showed what
really precedes the first `0x03.0x05`: `0x01.0x08` PHY_CONTEXT is immediately
followed by `0x05.0x08` RLC_CONFIG, both times. The artifact had the answer for
five rounds; nobody had read it in order.

Caveat worth keeping: v6.7's `iwl_mvm_phy_send_rlc` gates RLC on the fw
advertising DATA_PATH 0x08 at cmd_ver >= 2, and this ucode's version TLV does
NOT list it - yet the working driver sends it on this exact firmware. Device
ground truth beat the version gate.

### Three hypotheses DISPROVEN on metal this round — do not re-chase

1. **Round 22's stated root cause was WRONG.** SESSION_PROTECTION failed
   identically with a real, active, phy-bound link (same `0x101f` / `0x400`).
   The missing LINK was never the cause. The `id_and_color` was never wrong
   either: `iwl_mvm_get_session_prot_id` uses the raw mac id for cmd ver < 2
   (this fw), and mac id == fw_link_id == 0 here anyway.
2. **Antenna masks were not the cause** (though they WERE wrong and are fixed):
   `phy_sku=0x00330018` -> valid_tx/rx_ant = 0x3 on this 2x2 part; we hardcoded
   0x1 and sent `rxchain=0x0402` where Linux sends `0x1406`.
3. **BT coex and regulatory were not the cause.** `BT_CONFIG` is accepted and
   changes nothing. `MCC_UPDATE` queried with `MCC_SOURCE_GET_CURRENT` returns
   **status 1 = MCC_RESP_SAME_CHAN_PROFILE**: the fw already HAD a regulatory
   profile. (Sending `MCC_SOURCE_DEFAULT` instead returns status 4 = ILLEGAL -
   that was a driver bug, not a fw state.)

### NEW BLOCKER — TX has never actually transmitted

With session protection accepted and the radio on-channel, `iwl auth` TXes the
Open-System auth frame and gets **no response, and no TX-status notification
either** (no `grp=0 cmd=0x1c` REPLY_TX comes back; the only RX is a beacon).
Nothing in this driver has ever successfully transmitted, so the TX path itself
is unproven: `tx_enqueue`'s TFD build, the byte-count table format, and the
doorbell for the fw-assigned gen2 queue (qid 1 from `SCD_QUEUE_CONFIG`) are all
suspect. **That is the next slice** - and it is a real slice, not a tweak.

Start by making TX observable: log every notification the fw returns after an
enqueue, and check the TFD/bc-table against `iwl_pcie_gen2_build_tfd` +
`iwl_pcie_gen2_update_byte_tbl` for a device with `TFD_FORMAT_LONG` (our
context-info `control_flags = 0x980` sets it).

### Two more traps found this round

- **Do not send SESSION_PROTECTION twice.** A second ADD while one is registered
  gives a DIFFERENT LMAC assert (`error_id=0x4216`, `data1=0x1e data2=0x1900`).
  Linux skips the command when a time event is already running, or cancels with
  action REMOVE first. So `iwl mld 7` followed by `iwl auth` always asserts -
  use `iwl mld 1`..`6` then `iwl auth` (which does session-prot + auth back to
  back, which is also the only way the ~900 TU window is still open when the
  frame goes out; two separate URC commands are ~20 s apart and the window is
  long gone).
- **After ANY LMAC assert, `iwl rerun` does NOT restore the scanner** - two
  scans in a row return 0 APs / 0 RBs. A full `reboot` is required.

### Scan channel data is unreliable (open, affects AP selection)

Successive scans disagree about the channel of the SAME BSSID (`e8:d3:eb:51:8c:8f`
reported ch=11 in one scan and ch=1 in another), and one run put 16 of 24 APs on
ch=1 with several at ch=0. `scan_record_beacon` takes the channel from the DS
Parameter Set (IE id 3), which 5 GHz APs often omit - and NimmuNet is a
multi-BSSID mesh spanning both bands (the working Linux trace associated on
**chan 149 / 5 GHz**). If the picked channel is wrong the auth frame goes out on
the wrong channel and nothing answers, which is indistinguishable from the TX
bug above. Prefer the channel from the RX descriptor (the channel the scanner
was actually parked on when the beacon arrived) over the DS IE.

### Metal procedure for this branch

`iwl rerun` → `iwl mvm 1`/`2`/`e`/`4` → `iwl scan` → `iwl mld 1`..`6` →
`iwl auth`. `iwl caps` dumps the fw capability bits + antenna masks on a running
box (no reflash needed). `iwl fwerr` after any assert.

## Round 22 (2026-07-27) — ROOT CAUSE: this fw drives association with the NEW link-based command API; UnoDOS uses the legacy MVM one

Captured a **working Linux ASSOCIATION** command trace of THIS Yoga's AX201
connecting to NimmuNet, and diffed the command IDs against what UnoDOS sends.
This is the decisive artifact rounds 19-21 were missing.

**Rig for the capture (reusable):** wrote Arch Linux (lightweight, has
`CONFIG_IWLWIFI_DEVICE_TRACING`) to a USB stick (`devbuntu:~/archlinux.iso`, dd),
booted it on the Yoga with the eth dongle. Gotcha: **archiso auto-loads the
Broadcom `wl` module, which conflicts with iwlwifi and blocks the wlan netdev**
(phy0 appears, no `wlanN`). Fix in the capture script: `modprobe -r wl`, reload
iwlwifi, then `wpa_supplicant` to NimmuNet. Capture harness on devbuntu:
`assoc_server.py` (:8080, serves `assoc_capture*.sh`, saves POSTs to
`iwl_assoc_from_yoga.txt`); the Yoga runs `curl .../assoc_capture3.sh | bash`.
The `iwlwifi_dev_hcmd` tracepoint logs `hcmd 0x<group>.0x<cmd>` (command IDs, not
payloads). **Saved trace: `devbuntu:~/iwl_assoc_working_2026-07-27.txt`** (86
hcmds; connection reached `type managed / ssid NimmuNet`, chan 149 / 5 GHz).

**The working association sequence (MAC_CONF group = 0x03):**
```
0x03.0x08  MAC_CONFIG_CMD    (×10)
0x03.0x09  LINK_CONFIG_CMD   (×10)
0x03.0x0a  STA_CONFIG_CMD    (×4)
0x03.0x05  SESSION_PROTECTION_CMD  (×2)  <- accepted, NO assert
0x05.0x17  SCD_QUEUE_CONFIG_CMD          (TX queue - same as us)
```
**ZERO** legacy `0x01.0x18` ADD_STA, `0x01.0x28` MAC_CONTEXT, or `0x01.0x2b`
BINDING in the entire trace. The working driver (iwlmvm on kernel 7.0 for this
QuZ-77 fw) drives the whole mac/link/sta setup through the **new link-based
command API**, not the legacy MVM contexts UnoDOS uses.

**Why SESSION_PROTECTION LMAC-FATALs in UnoDOS (data1=0x400):** its
`id_and_color` references a **fw_link_id** created by `LINK_CONFIG_CMD`. UnoDOS
never sends `LINK_CONFIG_CMD` (it set up the mac via the legacy `MAC_CONTEXT_CMD`
0x28), so the session-protection request points at a link the fw does not have →
LMAC assert. The command, capability (capa54) and 24-byte length are all correct;
what is missing is the link the new-API setup would have created. So airtime was
never really the bug — the whole legacy association path is. The fw ACCEPTS the
legacy mac_ctxt/add_sta (csr2808=0) but they don't build the link state the rest
of the association (session-prot, and real auth/assoc RX) needs.

### The real work (next session) — port the association path to the new command API

Replace the legacy join commands with the MAC_CONF-group ones, in this order:
1. **`MAC_CONFIG_CMD` (0x03/0x08)** — `struct iwl_mac_config_cmd`. Creates the MAC.
   CMD_VERSIONS: ver 1 on this fw.
2. **`LINK_CONFIG_CMD` (0x03/0x09)** — `struct iwl_link_config_cmd`. Creates the
   link and returns/uses a **fw_link_id** — the id SESSION_PROTECTION needs.
3. **`STA_CONFIG_CMD` (0x03/0x0a)** — `struct iwl_sta_config_cmd`. The AP peer STA
   (replaces ADD_STA 0x18).
4. **`SESSION_PROTECTION_CMD` (0x03/0x05)** — keep the 24-byte struct, but set
   `id_and_color` to the **fw_link_id** from step 2 (not the raw mac id 0). This is
   almost certainly what clears `data1=0x400`.
5. `SCD_QUEUE_CONFIG_CMD` (0x05/0x17) — unchanged, already works.

Structs live in Linux `drivers/net/wireless/intel/iwlwifi/fw/api/mac-cfg.h`
(`iwl_mac_config_cmd`, `iwl_link_config_cmd`, `iwl_sta_config_cmd`) - fetch via
raw.githubusercontent.com/torvalds/linux. Cross-check field/version handling in
iwlmvm `mld-mac.c` / `mld-sta.c` (or iwlmld). The hcmd tracepoint did NOT log
payloads, so the structs come from source, not the trace; the trace gives the
exact command SEQUENCE + that it works. NOTE: this is a substantial rewrite of the
`iwl join` path (`mvm_mac_ctxt`/`mvm_binding`/`mvm_add_sta` -> the three new cmds),
not a one-liner - budget a focused session + a few metal reflash iterations.

### Rig note carried forward — the box boots a READ-ONLY USB ESP

The Yoga boots a USB stick whose ESP is firmware-SFS = **read-only in pc64_fs**, so
the URC A/B kernel push CANNOT update the live boot kernel (`push 1 ...` hits the
internal SSD, a non-boot disk). Every driver-code change is a full **USB reflash**:
build on devbuntu (`~/unodos-yoga`, UNO_DEBUG=1) -> `tools/mkuefi.py 1024` ->
dd `build/unodos-uefi.img` to the stick (model-guarded `/tmp/flash_ab.sh`;
`blockdev --setrw` first - the write-blocker sets USB ro). Bake DEBUG.CFG
(`remote=192.168.2.100:5098`, `net-eth-only`, NimmuNet creds) into `build/esp`
before imaging. To get fast A/B push instead, boot a writable-ESP install (internal
disk). Device nodes shuffle between sessions - ALWAYS re-identify by model.

## Round 21 (2026-07-27) — SESSION_PROTECTION verified on metal: right cmd, right length (24 B), but LMAC-FATALs (data1=0x400). Airtime is the wall.

Reflashed the boot sticks (both, A/B) with each fix and re-drove the full chain.
Two metal iterations pinned the airtime command precisely:

1. **20-byte SESSION_PROTECTION → length assert.** `iwl auth` logs
   `session-prot: MAC_CONF 0x5 len=20 capa54=1` then asserts; `iwl fwerr`: UMAC
   ADVANCED_SYSASSERT `201002fd` on cmd `0x0305`, **`data2=0x18` (expected 24) /
   `data3=0x14` (got 20)** — a pure length mismatch. So this fw wants **24 bytes**,
   not the 20 of SESSION_PROTECTION_CMD_API_S_VER_1 as I'd first read. Linux
   `struct iwl_session_prot_cmd` (VER_1 **and** VER_2) is in fact 6 u32 = 24 B
   (id_and_color, action, conf_id, duration_tu, repetition_count, interval).
2. **24-byte SESSION_PROTECTION → LMAC ADVANCED_SYSASSERT, `data1=0x400`.**
   `len=24 capa54=1`, then LMAC `error_id=0x101f` (ADVANCED_SYSASSERT) pc
   `004c0a3c`, cascading to UMAC `NMI_INTERRUPT_LMAC_FATAL` on cmd `0x0305` with
   **`data1=0x00000400`**. This is the SAME `0x400` signature the original
   session-prot attempts hit (85aeff9) — so the full real-join context did NOT
   fix it, and the id was already ruled out then (both raw 0 and color-encoded
   `g_mac_id` give `0x400`). Length + command + capability are all now correct;
   the fw still rejects the request itself. `0x101f`/`0x400` are Intel-internal,
   no open decode.

**Where this leaves it:** the entire association SETUP is solid on metal (F12
ALIVE, scan→24 APs→real BSSID, phy/mac/binding/add_sta/TX-queue all `csr2808=0`).
The ONE remaining wall is reserving the on-channel auth window: TIME_EVENT_CMD
is absent on this fw (SYSASSERT), and SESSION_PROTECTION_CMD — correct command,
correct 24-byte length, capa54=1 — LMAC-FATALs with `data1=0x400`. Committed
state: `iwlwifi.c` sends the correct 24-byte session-prot; it asserts.

**Two candidate next steps (both need a fresh session; each metal iteration is a
full USB reflash — the box boots a read-only USB ESP, see the rig note below):**
1. **Test whether the auth window is even needed.** Make `iwl auth` SKIP
   session-prot and just TX the auth frame, logging join-state RX counts. If the
   radio is already parked on-channel by phy_ctxt+binding and auth draws a
   response, session-prot was a red herring and association can proceed. If RX=0,
   the window is genuinely required.
2. **Ground-truth a full Linux ASSOCIATION trace on this Yoga** (not just the fw
   load we already have): boot Ubuntu, arm `events/iwlwifi` (command trace, not
   just iwlwifi_io), connect to NimmuNet, and capture the exact
   SESSION_PROTECTION_CMD bytes + the mac-context/link state around it. Diff the
   conf_id / id_and_color / duration and the preceding command sequence. The
   `0x400` almost certainly means the mac-context or link is in a state the fw
   won't schedule protection for — the association trace is the only way to see
   what the working driver does differently. (git.kernel.org is Anubis-blocked;
   use raw.githubusercontent.com/torvalds/linux for source.)

## Round 20 (2026-07-27) — association setup all works on metal; the wedge is the airtime command; SESSION_PROTECTION struct fixed (UNVERIFIED)

Drove the Yoga live over URC (:5098). Reproduced the whole chain end to end:
`iwl rerun` -> ALIVE (F12 confirmed solved), `iwl mvm 1/2/4` (init/tx_ant/power)
-> `iwl join`: a UMAC **passive scan returns 24 APs / ~60 beacons**, picks the
REAL NimmuNet BSSID `e8:d3:eb:51:4d:6f` chan 11, and phy_ctxt / mac_ctxt /
binding / **add_sta / txq_alloc all return with `csr2808=0`** (no assert; TX
queue qid=1). So every setup command works against real firmware with a real AP.
This is well past round 19 (which wedged at ADD_STA on a broadcast BSSID — the
real-scan path fixes that).

**The blocker is now precisely the auth-window airtime command, and BOTH prior
approaches were wrong for this fw:**

- **TIME_EVENT_CMD (LONG 0x29) — what commit `3cd66d3` switched to — SYSASSERTs.**
  `iwl auth` -> `csr2808=02000000`; `iwl fwerr` shows UMAC `error_id=201002ff`
  (ADVANCED_SYSASSERT), **`cmd=000c0129`** (group 1 / opcode 0x29 = TIME_EVENT),
  cascading to LMAC `NMI_INTERRUPT_UMAC_FATAL`. Crucially, **CMD_VERSIONS for
  this ucode does NOT list TIME_EVENT_CMD at all** (`tools/iwl_cmd_versions.py`;
  LONG group has 0x2c TIME_QUOTA but no 0x29), while it DOES advertise
  `MAC_CONF 0xfb SESSION_PROT` (notif ver 2). This is a SESSION_PROTECTION-based
  fw; the legacy TIME_EVENT_CMD is unsupported and the dispatcher asserts on it.
- **SESSION_PROTECTION_CMD (MAC_CONF 0x5) is the right command; its earlier
  LMAC-FATAL (85aeff9/2e740b2, data1=0x400) was a STRUCT-LENGTH bug, not the id.**
  `iwl_mvm_session_prot_cmd` = SESSION_PROTECTION_CMD_API_S_VER_1 = exactly 5 u32
  (20 B). `mvm_assoc_window` had a spurious 6th field and sent 24 B; the fw
  validates command length. The prior attempts only toggled the id field, never
  the length.

**Fix committed (`iwlwifi.c`):** `mvm_assoc_window` session-prot struct -> 20 B
(id raw 0 for cmd ver<=2); the `auth` path uses `mvm_assoc_window()` when capa 54
and NEVER falls back to TIME_EVENT. **NOT yet metal-verified** (see rig note).

**Rig gotcha that blocked verification (important for next session):** the Yoga
booted from a **USB stick** this session. A USB stick's ESP enumerates as a
**firmware-SFS volume, which pc64_fs makes READ-ONLY** — so the A/B kernel push
CANNOT update the live boot kernel. `push 1 …` writes native-FAT vol 1 (the
INTERNAL SSD's UNODOS, a non-boot disk here), which VERIFYs and persists but does
not change what boots. `disks`: fw0 `is_boot=1` (the boot stick), fw1, fw2
(238 GB internal). **For the A/B push loop to work, boot a writable-ESP install
(the internal disk), OR physically reflash the stick** with the new build.
Creds+STRESS.CFG+the fixed BOOTX64 are already staged on internal vol 1
(remote=192.168.2.100:5098, net-eth-only). **Verify:** boot writable ESP ->
rerun -> mvm 1/2/4 -> join -> auth; expect `csr2808=0` after session-prot (not
02000000), then `assoc` / `eapol` for the 4-way handshake.

Other rig notes: eth dongle DHCP frequently stalls (amber LAN) on each reboot and
needs a physical re-seat to get a lease; the URC link also flaps. The software
`guard <s> reboot` fires if no command is serviced within `<s>` — don't leave it
armed while reading output (it rebooted the box mid-analysis once).

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

## Round 19 (2026-07-24) — post-ALIVE MVM bisected; wedge = ADD_STA (scaffold, not transport)

F12 (reaching ALIVE) is closed. The next slice is the post-ALIVE MVM/join
sequence, gated behind `iwl mvm`. Using a stepped `iwl mvm <n>` verb (mirrors
`iwl alive <n>`) driven under the URC **guard**, the sequence was walked one
command at a time against the fw parked at the ALIVE gate:

| step | op | result |
|---|---|---|
| 1 | mvm_init_unified (SYSTEM/NVM/PHY cfg/INIT_COMPLETE) | ok |
| 2 | mvm_tx_ant | ok |
| 3 | mvm_dqa_enable | skipped (no capa 12) |
| 4 | mvm_power_table | ok |
| 5 | mvm_scan_cfg | ok |
| 6 | mvm_phy_ctxt ADD | ok |
| 7 | mvm_mac_ctxt ADD | ok |
| 8 | mvm_binding | ok |
| 9a | mvm_time_quota | ok |
| 9b | mvm_assoc_window (SESSION_PROTECTION) | ok |
| **9c** | **mvm_add_sta** | **WEDGES HARD** |
| 9d | wpa PMK/init (host only) | (not reached) |

So the entire command layer works against real firmware. The wedge is precisely
**ADD_STA**, and it is a **scaffold** problem, not transport: `find_and_join()`
has no beacon parse, so it calls `mvm_add_sta()` with a **broadcast BSSID**
(`0xFF..`). ADD_STA for a broadcast peer is malformed and the fw wedges on it.

**The real work = the association MLME** that must precede ADD_STA: SCAN_REQ_UMAC
-> collect beacons -> real BSSID + channel -> auth frames -> assoc -> THEN
ADD_STA with the real AP MAC, MAC_CONTEXT MODIFY(assoc), then the 4-way handshake
via handle_eapol(). `find_and_join()` today is explicitly scaffolded for all of
that (it says so in its own trace).

**Operational:** the ADD_STA wedge is the **interrupts-off / tight-spin class the
software guard CANNOT catch** — steps 1..9b were each auto-recovered by the guard
(~80 s, no hands), but 9c did not reset and needed a physical power cycle. This is
a concrete on-metal repro for the PCH **TCO hardware watchdog** (branch
`hwwdt-tco`); merging that would make even this class self-recovering.

**Latent bug to fix in the ADD_STA rework:** `*(u32*)(c+40) =
(1u<<g_data_qid>0?0:0)` in `mvm_add_sta` — nonsensical expression + UB shift by
`g_data_qid == -1`. Evaluates to 0 on x86 so it is not the wedge, but wrong.

**How to reproduce / continue:** flash a UNO_DEBUG build, `iwl rerun` (parks at
the ALIVE gate), `guard 40 reboot`, then `iwl mvm 1..8` + `iwl mvm a` + `iwl mvm
b` all return; `iwl mvm c` wedges. Verbs: `iwl mvm <1-9>` and `iwl mvm <a-d>`
(9-split). Bare `iwl mvm` arms the inline full run for a stock boot.

## Round 18 (2026-07-23) — SOLVED (diagnosis): the firmware was ALIVE the whole time

F12 was a **measurement error**, not a load failure. This device runs in MSI-X
mode, where causes are reported in `CSR_MSIX_HW/FH_INT_CAUSES_AD` and the legacy
`CSR_INT` stays 0 forever. We polled `CSR_INT` and the RX ring, saw zeroes, and
concluded "the firmware never starts". Measured on metal, after a load the
driver had just declared failed:

```
CSR_MSIX_HW_INT_CAUSES_AD (0x2808) = 0x00000001   <- bit 0 = ALIVE
CSR_MSIX_AUTOMASK_ST_AD   (0x2810) = 0x00000200   <- vector 9 automasked
```

Both match the ground-truth ftrace exactly. There was never a pre-ALIVE park —
which is precisely why every load-path suspect kept checking out correct. The
"frozen PCs" of round 16 are firmware idling *after* ALIVE because the host
never answered.

Corroborating (opt-in `iwl msix`): the device delivered a real MSI-X message to
host RAM, `msix_scratch = 0x4d510009` — vector 9, the same vector Linux takes
ALIVE on. DMA, bus mastering and IOMMU state were all fine. **The cause bit is
set with or without that table**, so the PCI Function Mask blocked only message
*delivery*; `msix_table_setup()` is a diagnostic, not the fix.

### The remaining work, and exactly where it is

Driven live over URC with `iwl csw`/`iwl prr` — no reflash, one register at a
time — the three post-ALIVE steps are each **safe and effective**:

| write | result |
|---|---|
| `iwl csw 1c80 7f8` (open RX ring) | **`RFH_Q0_FRBDCB_RIDX` 0 -> 0x20**: the fw consumed 32 RBDs and is DMAing |
| `iwl csw 2808 1` (ack ALIVE) | `0x2808` 1 -> 0, write-1-to-clear confirmed |
| `iwl csw 2810 200` (release automask) | `0x2810` -> 0 |

The machine stayed healthy through all three. So the wedge that killed two
builds this round was **not** the register sequence — it is the host-side RX
*read* path that runs straight after: `wait_notif()` walking the ring and
`rx_process_rb()` parsing firmware-written RB contents for the first time ever
(plus `rx_restock()` looping once `g_alive` is set). That code has never seen
real data. Harden the RB parse (bounds-check every length/offset before
following it) before re-enabling the path in `wait_alive()`.

`0x7f8` is `(2048-1) & ~7`, so the 2048-entry ring is required — the earlier
256-entry ring could not have produced the doorbell value the ftrace shows.

### Rig notes learned the hard way

- **URC log frames in flight are LOST when the machine wedges**, so a crash in
  the code under test tells you nothing about where it died. Prefer driving
  single registers with `iwl csw`/`prr` over flashing a build with a new code
  path — it isolates the failure and costs seconds instead of ~5 minutes.
- A yellow **"LAN?"** systray means it booted fine and only ethernet is down;
  that is not a hang. A real wedge shows as `ss -tn | grep 5098` ESTAB with a
  non-zero Send-Q.

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
