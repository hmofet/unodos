# pc64 networking — NICs, USB Ethernet and WiFi

pc64 ships a compact TCP/IP stack ([net.c](net.c)) over a one-function link
service (`uno_nic_t`: `send`/`recv`/`link`, Ethernet frames). Any driver that
publishes that service gives the whole stack — ARP, DHCP, DNS, ICMP, UDP, a
single-connection TCP, and TLS (BearSSL) — a link to run on. Drivers are probed
in order the first time the network is used ([pc64_http.c](pc64_http.c),
`pc64_net_up`):

| order | driver | device | bus | QEMU-verified |
|------:|--------|--------|-----|:-------------:|
| 1 | [e1000.c](e1000.c)   | Intel 8254x Gigabit (incl. the QEMU `e1000` = 82540em) | PCI | ✅ `e1000` |
| 2 | [e1000e.c](e1000e.c) | Intel e1000e: 82571–4/82583, I217/I218/I219 LOM (incl. QEMU `e1000e` = 82574L) | PCI | ✅ `e1000e` |
| 3 | [igb.c](igb.c)       | Intel igb: I210/I211, 82575/82576/82580, I350/I354 (incl. QEMU `igb` = 82576) | PCI | ✅ `igb` |
| 4 | [r8169.c](r8169.c)   | Realtek RTL8168/8111/8101/8125 Gigabit/2.5G (the common consumer NIC) | PCI | — (QEMU has no rtl816x) |
| 5 | [ax88179.c](ax88179.c) | ASIX AX88179/179A USB Gigabit | USB (xHCI) | — |
| 6 | [rtl8152.c](rtl8152.c) | Realtek RTL8152/8153/8155/8156 USB Ethernet (most docks/dongles) | USB (xHCI) | — |
| 7 | [iwlwifi.c](iwlwifi.c) | Intel AC/AX/BE WiFi (7260…AX210, WiFi-7 Bz/Sc best-effort) | PCI | — |
| 8 | [rtwifi.c](rtwifi.c) | Realtek WiFi (rtw88: 8822/8821/8723/8814; rtw89: 8852/8851/8922) | PCI | — |
| 9 | [mrvlwifi.c](mrvlwifi.c) | Marvell/NXP WiFi (mwifiex: 88W8897/8997/8766) | PCI | — |

The three Intel PCI GbE drivers (e1000/e1000e/igb) are exercised end-to-end by
`nettest.py` (swap `-device`): link, DHCP, ICMP, UDP/TFTP, TCP echo and TLS all
pass on each. e1000e owns the 82574L (device `0x10D3`) so it, not e1000, drives
the QEMU `e1000e`; igb needs a PHY autoneg kick over MDIC before QEMU raises
`STATUS.LU` and un-gates RX. r8169 and all WiFi parts have no QEMU model and are
verified by reference/inspection only (hardware-pending).

The USB NIC drivers have **two transports**. While the OS is firmware-attached
they drive the adapter through the firmware's own `EFI_USB_IO` handles
([usbio.c](usbio.c)) - no xHCI takeover, so the firmware keeps the USB boot
stick and keyboard alive (taking the controller while boot services are up is
the F8 stranding failure, just earlier). Once detached (or for development,
`-DUNO_XHCI`) they use the native xHCI stack; with it off, `uno_usb_*` are
inert stubs. e1000 is always present. WiFi requires firmware files and a
config file on the ESP (below).

## USB Ethernet (ASIX + Realtek)

Both are register-over-control-transfer, frames-over-bulk drivers modelled on the
Linux `ax88179_178a` and `r8152` drivers. `ax88179` matches ASIX VID `0x0b95`;
`rtl8152` matches the Realtek + common-dock VIDs (`0x0bda`, plus Lenovo `0x17ef`,
Microsoft `0x045e`, Dell `0x413c`, TP-Link `0x2357`, …) and then confirms the
chip from its version register — so a Lenovo/Dell dock's built-in Realtek NIC is
recognised even though the dongle's USB vendor id is the laptop maker's.

`rtl8152` covers the 8-byte-descriptor chips (8152/8153/8155/8156, up to 2.5G);
the 16-byte-descriptor 5G/10G parts (8157/8159) are recognised but declined. The
heavy analog PHY tuning tables and the optional Realtek firmware patches are
omitted — the link trains without them. RX uses the async bulk-IN path added to
the xHCI stack (`uno_usb_bulk_in_arm`/`_poll`) so `net_poll` never blocks waiting
for a packet that hasn't arrived.

`ax88179` programs the MAC medium (speed / duplex / gigabit 125 MHz clock) from
the PHY's *negotiated* status (`ax_apply_medium`, called from `ax_link` once the
link is up), not a hardcoded gigabit. A medium that doesn't match the real
negotiated speed leaves the MAC RX clock wrong and silently kills reception —
the X13 Yoga's first eth boot hit exactly that (`tx>0 rx=0`), which is what the
per-`net_init` frame counters (`net_tx/rx/arp/ip_frames`, on the NETLOG DHCP-fail
line) exist to localize.

## WiFi (Intel iwlwifi)

Intel WiFi cards are firmware-driven: the host loads a signed Intel `.ucode`
image (and, on AX210, a `.pnvm`), then speaks a command protocol to scan, join
and move frames. The driver ([iwlwifi.c](iwlwifi.c)) implements the PCIe
transport (reset, APM, the three firmware-load mechanisms, TFD/TFH queues and RB
rings, all polled — no MSI), the .ucode TLV parser, the MVM command layer, the
WPA2-PSK 4-way handshake ([wifi_wpa.c](wifi_wpa.c)) with keys installed into the
card for hardware CCMP, and Ethernet↔802.11 translation. It publishes the same
`uno_nic_t`, so once joined it is just another link to the stack.

### Firmware is NOT in the repo — you supply it

Intel's firmware licence forbids redistribution, so the blobs are not committed.
Copy them from a Linux `linux-firmware` tree (or `/lib/firmware/` on any recent
Linux box) onto the **ESP root** under a `FIRMWARE/` directory, renamed to 8.3
(the UEFI FAT reader the driver uses is 8.3). The driver reads `CSR_HW_REV` +
`CSR_HW_RF_ID` to identify the card and looks for these names:

The driver classifies the card by its PCI device id into a family, matching the
full Linux `iwlwifi` MVM device table (older 5000–6000/1000/2000 "iwldvm" cards
are recognised and cleanly declined — they need a different driver).

| card family | upstream firmware base | copy to ESP as | PNVM? |
|-------------|------------------------|----------------|:-----:|
| 7260 / 7265 / 3160 | `iwlwifi-7260` (per-device -3160/-7265/-7265D) | `FIRMWARE/IWL7260.UCO` | — |
| 3168 | `iwlwifi-3168` | `FIRMWARE/IWL3168.UCO` | — |
| 8260 / 8265 / 4165 | `iwlwifi-8000C` / `iwlwifi-8265` | `FIRMWARE/IWL8000.UCO` | — |
| 9260 (discrete) | `iwlwifi-9260-th-b0-jf-b0` | `FIRMWARE/IWL9260.UCO` | — |
| 9461 / 9462 / 9560 | `iwlwifi-9000-pu-b0-jf-b0` | `FIRMWARE/IWL9000.UCO` | — |
| AX200 (discrete) | `iwlwifi-cc-a0` | `FIRMWARE/IWLAX200.UCO` | — |
| AX201 (Qu/QuZ CNVi) | `iwlwifi-Qu-b0-hr-b0` / `-QuZ-a0-hr-b0` | `FIRMWARE/IWLAX201.UCO` | — |
| AX210 (Ty) | `iwlwifi-ty-a0-gf-a0` | `FIRMWARE/IWLAX210.UCO` | **yes** |
| AX211 / AX411 (So/Ma) | `iwlwifi-so-a0-gf-a0` | `FIRMWARE/IWLAX211.UCO` | **yes** |
| BE200 (Gl, WiFi 7 †) | `iwlwifi-gl-*-fm-*` | `FIRMWARE/IWLBE200.UCO` | **yes** |
| BE201 (Bz, WiFi 7 †) | `iwlwifi-bz-*-fm-*` | `FIRMWARE/IWLBE201.UCO` | **yes** |
| BE211 (Sc, WiFi 7 †) | `iwlwifi-sc-a0-*` | `FIRMWARE/IWLBE211.UCO` | **yes** |

Use the highest API revision (the number before `.ucode`) your file set has; the
driver only cares about the contents. AX210 and every WiFi-7 part also need the
matching `.pnvm` (regulatory/PHY data) — without it the firmware refuses to leave
init. AC cards and AX200/AX201 have no PNVM.

† **WiFi 7 (Bz/Gl/Sc) is best-effort / not yet working**: those cards boot through
the gen3 path but add a TOP-reset + ROM-start handshake the driver does not yet
perform. They're recognised and their firmware fetches, but bring-up is a further
metal task. Don't rely on WiFi 7 yet — AX201 (the X1 Carbon Gen 8 target) and the
other AX/AC parts are the ones to test.

The [`uno-wifi-fw.py`](tools/uno-wifi-fw.py) tool downloads and installs the right
file automatically — see below — so this table is mostly for reference.

The primary metal target is the **ThinkPad X1 Carbon Gen 8** (AX201, a Qu/QuZ
CNVi part) — the same machine the trackpad/keyboard work targets.

### WIFI.CFG — your network

Put a `WIFI.CFG` file at the ESP root with your SSID and passphrase
(**`WIFI.TXT` is accepted under the same rules** - that is the name the
flasher's developer-options folder copy stages from the NAS creds template at
`\\behemoth\unreplicated\unodos\pc64\testkit\wifi.txt`):

```
ssid=MyNetwork
psk=my-wpa2-passphrase
```

- One `key=value` per line; leading/trailing spaces are trimmed, `#` starts a
  comment. `ssid` is required; `psk` may be omitted for an open network.
- WPA2-PSK (CCMP) only. No WPA3/SAE, no enterprise/802.1X, no WPA1/TKIP.
- The passphrase is stored in plaintext on the ESP — treat the stick accordingly.

The WiFi driver is bound lazily (on the first network use), not at boot: a scan +
join + 4-way handshake takes a few seconds, which does not belong on the boot
path. If no `WIFI.CFG`/firmware is present, or the join fails, `iwl_nic()`
returns NULL and the probe chain simply falls through — a machine with a wired or
USB NIC is unaffected.

## Realtek and Marvell PCIe WiFi

Two more WiFi families, same shape as the Intel driver (firmware on the ESP,
software WPA2 via [wifi_wpa.c](wifi_wpa.c), publishes `uno_nic_t`, inert when
absent) but each speaking its vendor's command interface:

- **[rtwifi.c](rtwifi.c)** — Realtek `rtw88` (WiFi 5: RTL8822BE/CE, 8821CE,
  8723DE, 8814AE) and `rtw89` (WiFi 6/6E/7: RTL8852AE/BE/CE, 8851BE, 8922AE).
  Firmware `FIRMWARE\RTL8822C.FW` etc. Keys go into the chip's security CAM
  (a direct MMIO write on rtw88, a SEC-CAM H2C on rtw89).
- **[mrvlwifi.c](mrvlwifi.c)** — Marvell/NXP `mwifiex` (88W8897, 88W8997,
  88W8766). Firmware `FIRMWARE\W8897.FW` etc. A HostCmd/event interface over PFU
  DMA rings; keys installed via `HostCmd_CMD_802_11_KEY_MATERIAL`.

**These are more metal-pending than the Intel driver.** Everything that is fully
specified in the vendor drivers is implemented exactly — chip identification, the
firmware-download state machines, the ring/descriptor formats (incl. the
mandatory rtw88 TX-descriptor checksum), the command interfaces, and the CCMP key
install. But the large chip-specific tables Linux carries per chip — the Realtek
`pwr_on_seq` / rtw89 DMAC-CMAC init, and the exact Marvell `PCIE_DESC_DETAILS`
body — are **not** reproduced, so MAC bring-up is scaffolded, not complete. They
recognise their cards and load firmware; full association is the metal tail.
`uno-wifi-fw.py` fetches their firmware too (see below).

### Getting the firmware onto a stick — `tools/uno-wifi-fw.py`

Rather than copy files by hand, run the cross-platform helper. It downloads the
right blob from the Debian firmware repo (or your own `/lib/firmware`), finds the
UnoDOS USB, and drops the file under `FIRMWARE\` with the exact name the driver
loads, plus a starter `WIFI.CFG`:

```
python3 tools/uno-wifi-fw.py                      # auto-detect card + USB
python3 tools/uno-wifi-fw.py --card ax201 --dest E:\        (Intel, Windows)
python3 tools/uno-wifi-fw.py --card rtl8852b --dest /Volumes/UNODOS  (Realtek, macOS)
python3 tools/uno-wifi-fw.py --card w8897 --dest /media/me/UNODOS    (Marvell, Linux)
python3 tools/uno-wifi-fw.py --list-cards
python3 tools/uno-wifi-fw.py --source local       # Intel: copy from this box's /lib/firmware
```

Runs on Windows, macOS and Linux with only the Python 3 standard library.
**Intel** firmware comes from the Debian `firmware-iwlwifi` package (unpacked
in-process — `ar` + `tar` + `lzma`); **Realtek and Marvell** firmware is pulled
straight from the kernel.org `linux-firmware` tree. Double-click launchers:
`uno-wifi-fw.cmd` (Windows), `uno-wifi-fw.command` (macOS). `--list-cards` prints
every supported card key; auto-detect knows the Intel, Realtek (`0x10ec`) and
Marvell (`0x11ab`/`0x1b4b`) PCI IDs.

For a **test image** that already carries the firmware, populate `fw-blobs/`
(gitignored) with `sh tools/fetch-fw.sh`, then `UNO_STUDIO=0 ./build.sh` copies
them into `build/esp/FIRMWARE/` (see the `[fw]` step) so a flashed stick boots
WiFi-ready.

### Checking status — the Network app

The **Network** app shows an "Intel WiFi" section: the card's presence, the
driver state (via `iwl_status_str()` — card model, firmware loaded, joined SSID,
or the step that failed), and a **Connect WiFi** button that runs the scan + join
+ 4-way handshake (press `W`, or click). With no Intel card it simply reads "No
Intel WiFi card on the PCI bus."

### Status / limitations

`iwl_status_str()` reports the current state (card model, firmware loaded, joined
SSID + RSSI, or the step that failed) for the Network app and diagnostics.

This driver is **hardware-pending**: QEMU has no Intel-WiFi model, so it cannot be
exercised in the CI harness the way e1000 is (`nettest.py`) — it is verified only
to be *inert* when no supported card is on the PCI bus (so the e1000 regression
still passes). Bring-up on real silicon, and the firmware-version-specific command
struct variance that comes with it, is the metal tail. See
[METAL-CHECKLIST.md](METAL-CHECKLIST.md).
