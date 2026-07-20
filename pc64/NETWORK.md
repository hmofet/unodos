# pc64 networking — NICs, USB Ethernet and WiFi

pc64 ships a compact TCP/IP stack ([net.c](net.c)) over a one-function link
service (`uno_nic_t`: `send`/`recv`/`link`, Ethernet frames). Any driver that
publishes that service gives the whole stack — ARP, DHCP, DNS, ICMP, UDP, a
single-connection TCP, and TLS (BearSSL) — a link to run on. Drivers are probed
in order the first time the network is used ([pc64_http.c](pc64_http.c),
`pc64_net_up`):

| order | driver | device | bus |
|------:|--------|--------|-----|
| 1 | [e1000.c](e1000.c)   | Intel 8254x Gigabit (incl. the QEMU `e1000`) | PCI |
| 2 | [ax88179.c](ax88179.c) | ASIX AX88179/179A USB Gigabit | USB (xHCI) |
| 3 | [rtl8152.c](rtl8152.c) | Realtek RTL8152/8153/8155/8156 USB Ethernet (most docks/dongles) | USB (xHCI) |
| 4 | [iwlwifi.c](iwlwifi.c) | Intel AC/AX WiFi (7260…AX210) | PCI |

The USB drivers need the xHCI stack, which is behind `-DUNO_XHCI` (see
[xhci.h](xhci.h)); when it is off, `uno_usb_*` are inert stubs and both USB NIC
drivers compile to "no device found". e1000 is always present. WiFi requires
firmware files and a config file on the ESP (below).

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

| card | Linux firmware file | copy to ESP as |
|------|--------------------|----------------|
| 7260 / 7265         | `iwlwifi-7260-17.ucode` / `iwlwifi-7265D-29.ucode` | `FIRMWARE/IWL7260.UCO` / `IWL7265D.UCO` |
| 3160 / 3165 / 3168  | `iwlwifi-3160-17.ucode` / `iwlwifi-3168-29.ucode`  | `FIRMWARE/IWL3160.UCO` / `IWL3168.UCO` |
| 8260 / 8265         | `iwlwifi-8000C-36.ucode` / `iwlwifi-8265-36.ucode` | `FIRMWARE/IWL8000.UCO` / `IWL8265.UCO` |
| 9260                | `iwlwifi-9260-th-b0-jf-b0-46.ucode`                | `FIRMWARE/IWL9260.UCO` |
| 9461 / 9462 / 9560  | `iwlwifi-9000-pu-b0-jf-b0-46.ucode`                | `FIRMWARE/IWL9000.UCO` |
| AX200               | `iwlwifi-cc-a0-77.ucode`                           | `FIRMWARE/IWLAX200.UCO` |
| AX201 (Qu/QuZ CNVi) | `iwlwifi-Qu-b0-hr-b0-77.ucode` / `-QuZ-a0-hr-b0-77`| `FIRMWARE/IWLAX201.UCO` |
| AX210 / AX211       | `iwlwifi-ty-a0-gf-a0-89.ucode` **+** `iwlwifi-ty-a0-gf-a0.pnvm` | `FIRMWARE/IWLAX210.UCO` **+** `IWLAX210.PNV` |

Use the highest API revision (the number before `.ucode`) your file set has; the
driver does not care about the exact number, only the contents. The AX210 also
needs its `.pnvm` companion (regulatory/PHY platform data) — without it the
firmware refuses to leave init. AC cards and AX200/AX201 have no PNVM.

The primary metal target is the **ThinkPad X1 Carbon Gen 8** (AX201, a Qu/QuZ
CNVi part) — the same machine the trackpad/keyboard work targets.

### WIFI.CFG — your network

Put a `WIFI.CFG` file at the ESP root with your SSID and passphrase:

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

### Getting the firmware onto a stick — `tools/uno-wifi-fw.py`

Rather than copy files by hand, run the cross-platform helper. It downloads the
right blob from the Debian firmware repo (or your own `/lib/firmware`), finds the
UnoDOS USB, and drops the file under `FIRMWARE\` with the exact name the driver
loads, plus a starter `WIFI.CFG`:

```
python3 tools/uno-wifi-fw.py                      # auto-detect card + USB
python3 tools/uno-wifi-fw.py --card ax201 --dest E:\        (Windows)
python3 tools/uno-wifi-fw.py --card ax210 --dest /Volumes/UNODOS   (macOS)
python3 tools/uno-wifi-fw.py --list-cards
python3 tools/uno-wifi-fw.py --source local       # copy from this Linux box's /lib/firmware
```

Runs on Windows, macOS and Linux with only the Python 3 standard library (it
unpacks the `.deb` in-process — `ar` + `tar` + `lzma`). Double-click launchers:
`uno-wifi-fw.cmd` (Windows), `uno-wifi-fw.command` (macOS). The card→file map
matches the driver: `ax200→IWLAX200.UCO`, `ax201→IWLAX201.UCO`,
`ax210→IWLAX210.UCO + IWLAX210.PNV`, `9560→IWL9000.UCO`, etc.

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
