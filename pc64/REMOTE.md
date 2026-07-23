# unoautomate remote channel (REMOTE.md)

A bidirectional link between a running UnoDOS pc64 machine and the PC you
develop from: **remote logging** out, **remote control** in, and free-form
**messages + commands in either direction**, driven from a simple text command
language *or* a Python API on each end. Debug builds only (same `UNO_DEBUG` gate
as the rest of unoautomate); in production every entry point compiles away.

## Shape

**pc64 dials OUT** to a listener on the dev PC. You put the dev PC's address in
the stick's `STRESS.CFG`:

```
remote=<ip>:<port>       # static address (TCP over the LAN)
discover                 # OR zero-config: find the dev PC by broadcast
remote-serial            # OR NIC-independent: URC over a 16550 UART (COM1)
remote-serial=3e8        #   ...on a specific UART base in hex (see "serial" below)
```

On a debug boot, once the boot net test releases the connection (`automate_start`
in `pc64_nettest.c`), `unoauto_remote_boot()` reads the key, brings the NIC up
(`pc64_net_up`), and dials. The link is pumped every shell frame by
`unoauto_remote_tick()` (next to `pc64_nettest_tick` in `pc64_uui.c`). If the
connection drops it reconnects with a short backoff.

> **Zero-config discovery.** With `discover` set and no `remote=` key, pc64
> broadcasts a UNODISC PROBE on the LAN (`netdisc`, see `NETSTACK.md`); a dev PC
> running the listener answers with an OFFER carrying its ip:port, and pc64
> dials it automatically — no address to type. (The old ARP/broadcast limitation
> that deferred this is fixed: `net_udp_broadcast` now hand-builds a true
> broadcast frame, and `ip_recv` accepts inbound broadcast.)

> **Multiple connections.** The remote link now runs on its **own socket**
> (`net_socket`, from the multi-connection layer in `netsock.h`), not the shared
> legacy `net_tcp_*` slot — so the Browser / AI apps can hold a TCP connection
> at the same time as an active link. The stack (`net.c`) supports many
> simultaneous connections and can `listen`/`accept` inbound ones.

> **Security.** Plaintext, **LAN-only by intent**. Do not expose the listener
> to an untrusted network; it can drive input, launch apps, run Python, and
> power the machine off.

## NIC-independent transport (serial / UART)

The link normally rides TCP over the LAN. But when the machine's **only NIC is
the one you're debugging** (e.g. a ZimaBlade whose onboard Realtek is down), URC
can't ride it. For that, the same URC line protocol runs over a **16550 UART**
instead — a serial cable to the dev PC, no working network required. This is a
transport backend (`unoauto_serial.c`) behind the same framing/dispatch/queue
layer; every verb works identically over serial.

Arm it from `STRESS.CFG` (instead of `remote=` / `discover`):

```
remote-serial            # URC over COM1 (0x3F8) @ 115200 8N1
remote-serial=2f8        # ...or another UART base in hex (2f8 = COM2, 3e8 = COM3)
```

On the dev PC (needs `pyserial`):

```bash
python tools/unoauto_remote.py --serial COM3           # Windows
python tools/unoauto_remote.py --serial /dev/ttyUSB0   # Linux (optional :baud)
```

As a library: `link.attach_serial('/dev/ttyUSB0'); link.wait_hello()` then drive
it exactly as over TCP. `attach_stream(obj)` drives over any connected byte
stream with `recv()`/`sendall()` (e.g. a QEMU serial TCP socket).

> **Pick a UART the firmware isn't using as its console.** The debug build runs
> *attached* (UEFI stays alive so it can write its USB stick), and UEFI's serial
> console driver polls its console UART for input — **stealing bytes from URC's
> RX FIFO** and corrupting frames. On QEMU+OVMF that console is COM1 *and* COM2,
> so an attached-firmware box must use **COM3 (`remote-serial=3e8`)** or another
> non-console port. If your firmware only consoles COM1, COM2 is fine; if unsure,
> use COM3/COM4, or disable serial console redirection in the firmware setup.

> **Not for multi-MB pushes.** A 16550 has a 16-byte RX FIFO, so a sustained
> inbound flood (an A/B kernel `put`) can overrun it. Serial is for interactive
> control — `eth`/`iwl` register debug, `probe`, the drive verbs. Do big `put`
> pushes over TCP.

## Protocol (URC)

Newline-delimited UTF-8 text frames, **symmetric both directions**. One line is
`TYPE [payload]`:

| Frame | Meaning |
|-------|---------|
| `HELLO <name> <api>` | handshake, sent once by each end on connect |
| `LOG <chan> <text>` | a log line streamed from pc64 (chan = `KERNEL`/`NET`/`UI`/`STORAGE`/`TEST`/`SCRIPT`) |
| `MSG <text>` | free-form message, either direction |
| `CMD <id> <verb> <args…>` | a command request, either direction (`id` correlates the reply) |
| `RSP <id> ok <text>` | a result line (zero or more) |
| `RSP <id> err <text>` | an error line (marks the command failed) |
| `RSP <id> end` | terminates the response for `id` |
| `BYE` | graceful close |

Remote logging is just the LOG spine: `unoauto_remote.c` registers an
`unoauto_sink_add` sink over all channels, so every `unoauto_log(...)` line any
subsystem emits becomes a `LOG` frame while the link is up - producers are
unchanged.

### Command verbs executed on pc64

| Verb | Effect | Reply |
|------|--------|-------|
| `probe` | `unoauto_probe()` snapshot | one `ok` line per row: `kind state v1 v2 name` (name last, may contain spaces) |
| `log <text>` | `unoauto_log(SCRIPT, …)` | `ok logged` |
| `key <scan> <uni> [ctrl]` | inject a keypress | `ok` |
| `pointer <x> <y> <btn>` | inject a pointer event | `ok` |
| `apps` | count of launchable apps | `ok <n>` |
| `launch <n>` | launch app `n` | `ok launched` / `err no-app` |
| `close` | close the top window | `ok` |
| `uptime` | ms since boot | `ok <ms>` |
| `test [suite]` | run a conformance suite (`storage`/`system`/…, empty = all) | the report, line by line, then `ok rc=<n>` |
| `py <source>` | exec Python on-device (one line; shares the VM with any running Python app) | captured stdout, line by line |
| `vols` | list volumes | one `ok` line per volume: `vol kind writable name` (kind `0`=RAM `1`=native-FAT `2`=firmware-SFS) |
| `put <vol> <path> <off-hex> <b64>` | base64-decode the chunk into a RAM staging buffer at `<off>` (`0` = start a new upload) | `ok <bytes-decoded>` |
| `put <vol> <path> done <total-hex>` | finalize: write the staged buffer to disk in one `uno_fs_write`, then verify the on-disk size == total | `ok verified <total>` / `err size-mismatch…` |
| `poweroff` | shut the machine down after the queue drains | `ok bye` |
| `reboot` | reset the machine after the queue drains (`uno_native_reset`) | `ok bye` |
| `bootnext <n>` | set the UEFI `BootNext` variable to `Boot####` = `n` (needs runtime SetVariable — attached only) | `ok set` / `err unavailable` |
| `disks` | list raw disks | one `ok` line per disk: `idx name sectors writable is_boot` |
| `readsec <disk> <lba-hex> [n]` | read `n` (≤4) raw sectors | base64 of the sectors, streamed as `ok` lines |
| `arm <disk>` | arm destructive ops for `<disk>` this session (auto-disarms after ONE); **refuses the boot disk** | `ok armed <name> <sectors> sectors` / `err refused…` |
| `disarm` | clear the armed disk | `ok disarmed` |
| `writesec <disk> <lba-hex> <b64>` | *(armed)* write whole 512 B sectors | `ok <sectors>` |
| `gptinit <disk>` | *(armed)* write a fresh empty GPT | `ok gpt` |
| `mkpart <disk> <first-hex> <last-hex> esp <name>` | *(armed)* add one ESP partition | `ok part` |
| `mkfs <disk> <first-hex> <sectors-hex> <label>` | *(armed)* format a region FAT32 (`uno_fat_mkfs`) + remount | `ok formatted` |
| `prepdisk <disk> <label>` | *(armed)* the one-shot: fresh GPT + one ESP + FAT32 format + remount | `ok prepared` |
| `mkdir <vol> <path>` | create a directory on a native-FAT volume (so files can be pushed into it) | `ok mkdir` |
| `makeboot <disk> [desc] [efi-path]` | author a UEFI boot entry for the ESP on `<disk>` (defaults: `UnoDOS`, `\EFI\BOOT\BOOTX64.EFI`, made default); attached only | `ok boot-entry added` |
| `iwl <subcmd…>` | live Intel-WiFi register/bring-up debug (F12) — `csr`/`csw`/`prr`/`prw`/`rerun`/`status` (pass-through to `iwl_dbg_cmd`) | the report, then `ok`/`err` |
| `eth <subcmd…>` | live wired-NIC (Realtek r8169) register/bring-up debug — the wired sibling of `iwl`: `status`/`reg`/`wreg`/`phy`/`wphy`/`rerun`/`link`/`mac` (pass-through to `r8169_dbg_cmd`) | the report, then `ok`/`err` |
| `disc` | query zero-config discovery state (netdisc) — is it armed, did pc64 record a host OFFER, and which host it latched | `ok active=<0/1>`, `ok have_host=<0/1>`, `ok host=<ip>:<port>` (only when found), `ok link=<state>` |

## A/B OS update (push a new BOOTX64.EFI over the link)

Iterating on a driver (e.g. WiFi) against a live machine normally means physically
reflashing a USB stick each round. Instead, run **two** UnoDOS sticks — **A** (the
running, known-good OS) and **B** (a spare) — and push only the rebuilt
`EFI\BOOT\BOOTX64.EFI` (~1.5 MB) to stick B over the link, then reboot into B. A
driver change touches only that one file; firmware / apps / config on the stick are
untouched, and A stays as the fallback.

The upload is **RAM-staged and written in one shot at `done`**, so a partial or
interrupted transfer never touches stick B — it stays a valid boot disk. The write
goes through `uno_fs_write`, which now writes **firmware-SFS volumes too** (via
`uno_efifs_write`) — so an *attached* machine (the driver box builds
`-DUNO_NO_DETACH`) can write its USB stick, which appears as a `kind 2` volume.

From the dev PC:

```bash
# find which volume is stick B (look for a writable kind-2 volume)
python tools/unoauto_remote.py --listen 0.0.0.0:5099   # then type: vols

# push a fresh build to stick B (vol 2 here) and reboot into it
python tools/unoauto_remote.py --push 2 'EFI\BOOT\BOOTX64.EFI' build/BOOTX64.EFI --reboot
# add --bootnext <N> to also set BootNext so it boots B without the F12 menu
```

Or from the library: `link.push_file(2, r'EFI\BOOT\BOOTX64.EFI', 'build/BOOTX64.EFI')`
returns `True` when verified; then `link.bootnext(n)` / `link.reboot()`.

> **Security — `put`/`reboot`/`bootnext` widen the blast radius.** They are arbitrary
> file write + reset + boot-target change, and (like the whole channel) are
> **UNO_DEBUG-only** and **plaintext, LAN-only**. Never expose the listener to an
> untrusted network. `put` caps a single upload at 8 MB (the staging buffer).

`probe` row kinds: `0` module (`.UNO` file), `1` window (title), `2` subsystem
(`heap`/`net`/`fs`/`shell`) - see `unoauto.h` for the `v1`/`v2` meanings.

Because either end can send `CMD`/`MSG`, the dev PC can also *register handlers*
so pc64 can drive it back (e.g. an on-device script asking the host to save a
file) - see `on_command` below.

## Preparing a fresh disk (disk B)

The A/B push above needs a *formatted* stick. To go further - move UnoDOS off the
UEFI stick and onto an internal disk - the channel can **partition and format a
raw disk** over the wire, so a blank disk B becomes a bootable FAT32 ESP you then
`put` the OS files onto. unoautomate implements none of this itself: the verbs
wrap the **`unostorage`** framework (GPT authoring over `blkdev`) and
**`uno_fat_mkfs`** (the FAT formatter), which the installer shares.

Because this authors a fresh GPT, it must run where firmware sector writes work -
**while ATTACHED** (the debug build's default). Disk B shows up as a writable
`fw*` disk in `disks`.

```bash
# see the disks; find the one that is writable and NOT is_boot
python tools/unoauto_remote.py --listen 0.0.0.0:5099        # then type: disks

# one-shot: partition + format the blank disk (index 1 here) as a FAT32 ESP
python tools/unoauto_remote.py --prepdisk 1 UNODOS
# then push the OS tree onto the new volume with --push, and set a boot entry
```

> **Safety.** Every destructive verb (`writesec`/`gptinit`/`mkpart`/`mkfs`/
> `prepdisk`) is inert until you `arm <disk>`, which **auto-disarms after one
> op** and **refuses the disk UnoDOS booted from** (`is_boot`). `arm` echoes the
> disk name + size so you can confirm the target before committing. Like the rest
> of the channel these verbs are **UNO_DEBUG-only** and **LAN-only**. `prepdisk`
> erases the whole disk.

### Installing to an internal disk

`prepdisk` works on **any** writable disk - while attached, an internal SATA/NVMe
disk shows up as a writable `fw*` disk in `disks`, so the same flow installs
UnoDOS onto it. The full install is: partition + format → create the directory
tree → copy the OS files → author a UEFI **boot entry** so the machine boots the
disk. One command does all of it:

```bash
# install the built OS tree onto internal disk 1 (DESTRUCTIVE)
python tools/unoauto_remote.py --install 1 build/esp
# then: reboot   (the new disk is now the default boot entry)
```

Under the hood (`UnoAutoLink.install_dir`): `arm` + `prepdisk`, then `mkdir` for
each directory and `put` for each file under `build/esp`, then `makeboot`. The
boot entry is authored with a hand-built HD() device-path node (the firmware
hasn't re-read the new GPT yet), the same technique the on-disk installer uses -
`makeboot` wraps `uno_pc64_add_boot_entry` (uefi_main.c) + `unostorage_find_esp`.
`makeboot` needs firmware runtime services, so it is **attached-only**.

## The dev-PC tool - `tools/unoauto_remote.py`

Run it on the machine you develop from; its LAN IP goes in the stick's
`remote=` key.

```bash
python tools/unoauto_remote.py --listen 0.0.0.0:5099
```

It prints incoming `LOG`/`MSG` lines and lets you type command lines that go to
pc64 (`probe`, `launch 0`, `py print(6*7)`, `uptime`; prefix `/msg` for a
free-form message).

As a library:

```python
from unoauto_remote import UnoAutoLink
link = UnoAutoLink(port=5099); link.listen()
link.on_log(lambda ch, t: print(ch, t))
link.wait_connected()
print(link.probe())            # [{'kind':2,'state':.., 'name':'heap', ...}, ...]
link.launch(0)
print(link.eval("print(6*7)")) # ['42']
link.on_command("save", lambda args: "saved " + args)  # pc64 -> host commands
```

## On-device Python (`import unoauto`)

The `unoauto` module (in `PYRT.UNO`) gains, alongside the existing
log/probe/key/launch/… surface:

- `unoauto.remote_active()` → `bool`
- `unoauto.remote_send(text)` → send a `MSG` to the dev PC
- `unoauto.remote_recv()` → next inbound `MSG` (or `RSP`) string, else `None`
- `unoauto.remote_stop()` → tear the link down

so an automation script can exchange messages with the dev PC as it runs. (In
production PYRT these are inert stubs, like the rest of `unoauto`.)

## Verification

- **`tools/remote_proto_test.py`** - pure-Python protocol unit test (parser,
  correlation, both-direction command dispatch). No device needed.
- **`tools/remote_qemu.py`** - end-to-end in QEMU: boots the debug image with
  `remote=10.0.2.2:<port>` on a SLIRP NIC, runs the host listener, and asserts
  the log stream, a `probe` round-trip, `py print(6*7)`→`42`, and `launch`→
  window. From a SLIRP guest the host is `10.0.2.2`, so that address reaches
  the listener on the host's loopback.
- **`tools/serial_qemu.py`** - the same round-trip with **no network at all**:
  boots with a `remote-serial` STRESS.CFG and **no NIC device**, driven over the
  guest's COM3 bridged to a TCP socket. Proves the NIC-independent transport
  (the ZimaBlade r8169 case). Uses COM3, not COM1/COM2 — see the console-UART
  caveat under "NIC-independent transport" above.
