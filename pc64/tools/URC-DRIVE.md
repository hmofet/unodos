# Driving a pc64 box over the URC bridge

Operational howto for driving a live UnoDOS/pc64 machine from the dev host
(devbuntu) over the unoautomate remote channel (URC). This complements
[REMOTE.md](../REMOTE.md) (the wire protocol) with the day-to-day workflow the
Zimaboard r8169 bring-up used.

## The pieces

- **`urc_bridge.py`** — a headless, **file-driven** URC driver. It listens on a
  TCP port, logs every frame the board sends, and sends whatever you append to a
  command file. This is the primary interface: you never hold an interactive
  session, you just tail a log and echo commands into a file.
- **`zima_drive.py`** — a one-shot alternative: listens, waits for the board to
  dial in, runs a fixed r8169 discovery sweep (`eth status`/`rerun`/`link`/`mac`
  + register/PHY reads), prints the results, and exits (freeing the port).
- Both import `UnoAutoLink` from [`unoauto_remote.py`](unoauto_remote.py), so run
  them from this `tools/` directory (or keep `urc_bridge.py` next to it).

The board dials **out** to the dev host — put the host's address in the stick's
`STRESS.CFG`: `remote=<host-ip>:<port>` (static) or `discover` (zero-config).
Debug builds only (`UNO_DEBUG`); in production every URC entry point compiles away.

## Using the bridge

```bash
# on the dev host (e.g. devbuntu), from a persistent session:
python3 urc_bridge.py 5099
```

It creates `~/urc/` with two files:

- **`~/urc/session.log`** — every `LOG`/`MSG`/`RSP` frame + connect/disconnect
  events, timestamped. Tail this to watch the board and read command replies.
- **`~/urc/cmd.txt`** — append one command per line; the bridge sends each and
  logs the reply. It only reads *new* lines (tracks its position), so leftover
  lines from an earlier session never replay.

Drive it by appending to `cmd.txt` and reading `session.log`:

```bash
tail -f ~/urc/session.log                      # in one terminal
echo 'probe'      >> ~/urc/cmd.txt             # in another
echo 'eth status' >> ~/urc/cmd.txt
```

`cmd.txt` line forms:

| line | effect |
|------|--------|
| `/msg <text>` | free-form message to the board |
| `push <vol> <path> <localfile>` | chunked A/B OS update (e.g. push a rebuilt `EFI\BOOT\BOOTX64.EFI`) |
| anything else | a URC command verb (see REMOTE.md's verb table) — `probe`, `vols`, `uptime`, `launch`, `test`, `py`, `reboot`, `poweroff`, `iwl …`, `eth …`, `disc`, … |

## Driving the r8169 (the `eth` verb)

Once the board is up on the Realtek and dialed in, `eth <subcmd>` pokes the live
driver via `r8169_dbg_cmd`:

| subcmd | what |
|--------|------|
| `eth status` | `present`/`up`/`dev`/`is8125`/**`rxcur`**/**`rxkicks`**/`mmio`/`ChipCmd`/`IntrStatus`/`PHYstatus`/`link` |
| `eth link` | PHYstatus decode + negotiated speed |
| `eth mac` | MAC from MAC0/MAC4 |
| `eth reg <hexoff>` / `eth wreg <hexoff> <hexval>` | MMIO dword peek / poke |
| `eth phy <reg>` / `eth wphy <reg> <hexval>` | PHY (MDIO) peek / poke |
| `eth rerun` | force a fresh PCI map + `hw_start()` bring-up, report link |

`rxcur` is frames received; `rxkicks` is RX-overflow recoveries. On the Zimaboard
a healthy link shows `rxcur` climbing into the thousands with `rxkicks`
incrementing — that's the RX-FIFO-overflow fix (`r8169.c`) doing its job. If
`rxcur` freezes at 64 (= RXN), RX has stalled.

## Two devbuntu gotchas

- **Flashing sticks:** a udev rule (`/etc/udev/rules.d/99-usb-storage-readonly.rules`)
  forces every USB disk read-only. Move it aside + `udevadm control --reload-rules`
  + `blockdev --setrw /dev/sdX` to write, then restore it.
- **Background processes:** logind kills nohup'd background processes on SSH
  logout — run the bridge from a session you keep open (tmux/screen, or a
  foreground shell), not a detached `nohup … &`.

## If commands time out but the board still pings

The board's first URC TCP connection can go half-open (the board reconnects on a
new source port). The bridge accepts one connection; if it's holding the dead
one, restart the bridge so it accepts the board's fresh connection.
