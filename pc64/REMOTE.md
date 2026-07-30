# unoautomate remote channel (REMOTE.md)

A bidirectional link between a running UnoDOS pc64 machine and the PC you
develop from: **remote logging** out, **remote control** in, and free-form
**messages + commands in either direction**, driven from a simple text command
language *or* a Python API on each end. Debug builds only (same `UNO_DEBUG` gate
as the rest of unoautomate); in production every entry point compiles away.

## Shape

**pc64 dials OUT** to a listener on the dev PC. You put the dev PC's address in
the stick's `DEBUG.CFG` (renamed from `STRESS.CFG` on 2026-07-26; the build ships
a `DEBUG.CFG`, so put keys there, a `STRESS.CFG` is now shadowed by it):

```
remote=<ip>:<port>       # static address (TCP over the LAN)
discover                 # OR zero-config: find the dev PC by broadcast
listen                   # OR SERVER mode: the dev PC dials INTO us (port 5099)
listen=<port>            #   ...on a specific port
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
> dials it automatically, no address to type. (The old ARP/broadcast limitation
> that deferred this is fixed: `net_udp_broadcast` now hand-builds a true
> broadcast frame, and `ip_recv` accepts inbound broadcast.)

> **Multiple connections.** The remote link now runs on its **own socket**
> (`net_socket`, from the multi-connection layer in `netsock.h`), not the shared
> legacy `net_tcp_*` slot, so the Browser / AI apps can hold a TCP connection
> at the same time as an active link. The stack (`net.c`) supports many
> simultaneous connections and can `listen`/`accept` inbound ones.

> **Server / listen mode (`listen`).** URC normally has the box dial OUT, which
> suits a headless box with a changing DHCP address (it calls home when ready).
> But because the stack can `listen`/`accept`, the box can instead be a **server**
> that the dev PC dials INTO, the "browse the LAN and connect to a box" model.
> With `listen` (bare = port 5099) or `listen=<port>` the box binds+listens
> (`net_listen`/`net_accept`, the `TP_TCP_LISTEN` transport in `unoauto_remote.c`)
> and accepts one inbound URC connection; the listener **persists** across client
> reconnects (a dropped client just returns it to waiting), so there is no
> connect timeout and no address to dial. `listen` is mutually exclusive with
> `remote=`/`discover`/`remote-serial` and takes precedence. It also arms
> `netdisc` as a **responder** advertising the box's `ip:listen-port` (via
> `netdisc_listen`), so a scanning client can **discover** which boxes it can
> dial in to (a dial-out `discover` box advertises port `0`: nothing to dial).
> The WinForms client's **Scan…** button broadcasts a PROBE, lists the listening
> boxes, and dials the one you pick. Gate: `tools/listen_qemu.py`.

> **Security.** Plaintext, **LAN-only by intent**. Do not expose the listener
> to an untrusted network; it can drive input, launch apps, run Python, and
> power the machine off.

## NIC-independent transport (serial / UART)

The link normally rides TCP over the LAN. But when the machine's **only NIC is
the one you're debugging** (e.g. a ZimaBlade whose onboard Realtek is down), URC
can't ride it. For that, the same URC line protocol runs over a **16550 UART**
instead, a serial cable to the dev PC, no working network required. This is a
transport backend (`unoauto_serial.c`) behind the same framing/dispatch/queue
layer; every verb works identically over serial.

Arm it from `DEBUG.CFG` (instead of `remote=` / `discover`):

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
> console driver polls its console UART for input, **stealing bytes from URC's
> RX FIFO** and corrupting frames. On QEMU+OVMF that console is COM1 *and* COM2,
> so an attached-firmware box must use **COM3 (`remote-serial=3e8`)** or another
> non-console port. If your firmware only consoles COM1, COM2 is fine; if unsure,
> use COM3/COM4, or disable serial console redirection in the firmware setup.

> **Not for multi-MB pushes.** A 16550 has a 16-byte RX FIFO, so a sustained
> inbound flood (an A/B kernel `put`) can overrun it. Serial is for interactive
> control, `eth`/`iwl` register debug, `probe`, the drive verbs. Do big `put`
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
| `screen [info]` | desktop geometry for a remote-desktop client | `ok <w> <h> rgba` |
| `screen grab [scale]` | snapshot + QOI-encode the WHOLE framebuffer (optional nearest-neighbour downscale by `scale`) and **stage** it on-device. The OUT half of remote desktop; pairs with `key`/`pointer` (the IN half) | `ok frame <w> <h> qoi <n>` then `end` / `err too-big (raise scale)` |
| `screen grab delta [scale]` | like `grab`, but encodes only the **tiles that changed** since the previous grab (per-tile hashing), so a mostly-static desktop streams at a fraction of the bytes. Stages `[QOI strip of changed tiles][manifest]`. Falls back to a full `frame` keyframe on the first grab, a scale change, or when too much changed | `ok delta <ew> <eh> <cols> <tw> <th> <nch> <strip> <total>` then `end`, **or** the `frame …` keyframe reply above |
| `screen read <off-hex> [len]` | read `len` (≤2880) bytes of the staged QOI frame at `<off>`, base64, streamed as `ok` lines (like `readsec`). A whole frame is far larger than one URC response, so the client pulls it in slices | base64 `ok` lines then `end` / `err no-frame` |
| `screen record start [scale] [fps]` | begin **server-side capture**: the device records keyframe+delta frames into a RAM ring on its own shell tick at `fps` (1..60, default 10), decoupled from the client's poll rate, using its own delta snapshot so the live view is undisturbed | `ok frames <n> bytes <b> dropped <d> ew <ew> eh <eh> cols <c> tw <t> th <t> fps <f> on <0/1>` then `end` / `err already recording` |
| `screen record stop` \| `screen record status` | stop (ring retained for reading) / query without stopping | the same `ok frames …` stat line then `end` |
| `screen record read <off-hex> [len]` | read `len` (≤2880) bytes of the recorded ring at `<off>`, base64 (like `screen read`); the client pulls the whole ring and reconstructs each frame | base64 `ok` lines then `end` / `err no-data` |
| `apps` | count of launchable apps | `ok <n>` |
| `launch <n>` | launch app `n` | `ok launched` / `err no-app` |
| `close` | close the top window | `ok` |
| `uptime` | ms since boot | `ok <ms>` |
| `test [suite]` | run a conformance suite (`storage`/`system`/…, empty = all) | the report, line by line, then `ok rc=<n>` |
| `py <source>` | exec Python on-device (one line; shares the VM with any running Python app) | captured stdout, line by line |
| `vols` | list volumes | one `ok` line per volume: `vol kind writable name` (kind `0`=RAM `1`=native-FAT `2`=firmware-SFS) |
| `put <vol> <path> <off-hex> <b64>` | base64-decode the chunk into a RAM staging buffer at `<off>` (`0` = start a new upload) | `ok <bytes-decoded>` |
| `put <vol> <path> done <total-hex>` | finalize: write the staged buffer to disk in one `uno_fs_write`, then verify the on-disk size == total | `ok verified <total>` / `err size-mismatch…` |
| `mkdir <vol> <path>` | create ONE directory on a mounted volume (parent must already exist; native-FAT only). No `arm` gate, volume-level like `put`. Lets `put` target a nested path (e.g. `\EFI\BOOT\`) a level at a time. Idempotent. | `ok created` / `ok exists` / `err mkdir failed…` |
| `install <disk> [default]` | *(armed)* clone the running OS onto `<disk>` in one op: prepdisk (GPT+ESP+FAT32) + native clone of the boot ESP's whole tree. Disk boots via the firmware removable-media path `\EFI\BOOT\BOOTX64.EFI`. Writes **no** NVRAM `Boot####` entry (runtime SetVariable is refused post-detach), so `default` is inert here. | `ok prepared`, `ok cloning`, `ok installed <n> files <bytes> bytes…` / `err…` |
| `poweroff` | shut the machine down after the queue drains | `ok bye` |
| `reboot` | reset the machine after the queue drains (`uno_native_reset`) | `ok bye` |
| `guard <timeout-s> [reboot]` | arm the dead-man's switch: if the box can't service an inbound URC command within `<timeout-s>`, the debug watchdog hard-resets it (and it re-dials home). Any later command refreshes the deadline; `safe` stands it down. v1 action = reboot | `ok armed <t>s action=reboot token=<n>` / `err usage…` |
| `pet` | explicit keep-alive (any command refreshes implicitly; this is the no-op one for a long op) | `ok petted` / `ok not-armed` |
| `safe [token]` | disarm the guard (the op returned). If a token is given it must match the one from `guard` | `ok disarmed` / `err bad-token` |
| `bootnext <n>` | set the UEFI `BootNext` variable to `Boot####` = `n` (needs runtime SetVariable, attached only) | `ok set` / `err unavailable` |
| `disks` | list raw disks | one `ok` line per disk: `idx name sectors writable is_boot` |
| `readsec <disk> <lba-hex> [n]` | read `n` (≤4) raw sectors | base64 of the sectors, streamed as `ok` lines |
| `arm <disk>` | arm destructive ops for `<disk>` this session (auto-disarms after ONE); **refuses the boot disk** | `ok armed <name> <sectors> sectors` / `err refused…` |
| `disarm` | clear the armed disk | `ok disarmed` |
| `writesec <disk> <lba-hex> <b64>` | *(armed)* write whole 512 B sectors | `ok <sectors>` |
| `gptinit <disk>` | *(armed)* write a fresh empty GPT | `ok gpt` |
| `mkpart <disk> <first-hex> <last-hex> esp <name>` | *(armed)* add one ESP partition | `ok part` |
| `mkfs <disk> <first-hex> <sectors-hex> <label>` | *(armed)* format a region FAT32 (`uno_fat_mkfs`) + remount | `ok formatted` |
| `prepdisk <disk> <label>` | *(armed)* the one-shot: fresh GPT + one ESP + FAT32 format + remount | `ok prepared` |
| `makeboot <disk> [desc] [efi-path]` | author a UEFI boot entry for the ESP on `<disk>` (defaults: `UnoDOS`, `\EFI\BOOT\BOOTX64.EFI`, made default); attached only | `ok boot-entry added` |
| `iwl <subcmd…>` | live Intel-WiFi register/bring-up debug (F12), `csr`/`csw`/`prr`/`prw`/`rerun`/`status` (pass-through to `iwl_dbg_cmd`) | the report, then `ok`/`err` |
| `eth <subcmd…>` | live wired-NIC (Realtek r8169) register/bring-up debug, the wired sibling of `iwl`: `status`/`reg`/`wreg`/`phy`/`wphy`/`rerun`/`link`/`mac` (pass-through to `r8169_dbg_cmd`) | the report, then `ok`/`err` |
| `disc` | query zero-config discovery state (netdisc), is it armed, did pc64 record a host OFFER, and which host it latched | `ok active=<0/1>`, `ok have_host=<0/1>`, `ok host=<ip>:<port>` (only when found), `ok link=<state>` |
| `devices` | read-only PCI device listing (pass-through to unodevices' `devmgr_list_str`). Mutates nothing, no `arm` gate | one `ok` line per device, e.g. `01:00.0 8086:5A85 03/00 display`; `err device manager not built…` until unodevices lands |
| `hwwdt <subcmd…>` | PCH TCO hardware watchdog (unodevices' `uno_hw_wdt_cmd`), the guard's IRQs-off backstop. `status` (present/gen/TCOBASE + raw `GEN_PMCON_A` `fw=0x..` dump); `arm <s>`/`pet`/`disarm` drive the TCO directly (**safe**: an armed-but-unpetted TCO resets in ~`<s>`, and if NO_REBOOT wasn't truly cleared it simply doesn't, never a hard hang); `selftest <s>`/`wedge` cli-spin to trigger the IRQs-off wedge (never returns; only the TCO recovers) | the report, then `ok`/`err` |

> **Durability.** The native FAT cache is write-back, and post-detach nothing
> flushes it on its own. `poweroff`/`reboot` therefore `uno_fat_sync()` (flush all
> dirty lines to disk) **before** powering off, so remote `put`/`mkdir` writes
> survive the power cycle, essential when the next step is booting the disk you
> just wrote. Don't cut power without one of these verbs, or unflushed writes are
> lost.

> **`devices`: the format is unodevices', not URC's.** The verb is a pure
> pass-through: it calls `devmgr_list_str()` and splits the returned dump on
> newlines, one `ok` line per device. It does not parse, reorder, or reformat
> those lines, so when unodevices phase 2 appends a bound-driver / `UNCLAIMED`
> column, it appears over the link with no change to `unoauto_remote.c`. Until
> that subsystem lands on master a **weak stub** answers, so the verb is always
> wired and always dispatches, it replies `err device manager not built
> (unodevices pending)` rather than `err unknown-verb`, and upgrades itself the
> moment the strong symbol links in. The listing is capped at the 4 KB report
> buffer. Read-only by construction: no `arm` gate, nothing is written.

## Remote desktop (`screen` + `key`/`pointer`)

URC already injects input (`key`, `pointer`); `screen` is the missing OUT half,
so the whole loop, see the device screen, click and type on it, rides the one
channel. `screen grab` snapshots the software framebuffer (`fb.h` `fb[]`),
QOI-encodes it (lossless, tiny on UnoDOS's flat-colour desktop; encoder in
`unoauto_screen.c`), and streams it base64 exactly like `readsec`. A client polls
it VNC-style at a target FPS and maps view coordinates back to framebuffer
coordinates for `pointer`/`key`.

```python
link = UnoAutoLink(port=5099); link.listen(); link.wait_connected()
w, h = link.screen_info()                 # (640, 480)
W, H, rgba = link.screen_grab(scale=1)    # decoded to raw RGBA, 4 bytes/pixel
link.pointer(W // 2, H // 2, 1)           # click the middle of the screen
```

`scale` downsamples nearest-neighbour (`2` => half w/h, a quarter of the pixels)
so a busy or hi-res screen still fits the device's 2 MB encode buffer; an
overflow replies `err too-big (raise scale)`. **TCP only**: a frame is far too
large for the 16-byte serial FIFO (see the transport note above).

**Delta streaming (`screen grab delta`).** Polling a full frame every tick is
wasteful on UnoDOS's near-static desktop. `grab delta` keeps a **per-tile hash
snapshot** of the previous grab (32×32 emitted-pixel tiles; a hash array, *not* a
multi-MB previous-frame buffer, `fb[]` is up to 1920×1200) and encodes only the
tiles whose hash changed, as one vertical QOI strip, with a trailing manifest of
their row-major indices (`col = idx % cols`, `row = idx / cols`). A static frame
sends `nch 0` and zero payload. It **auto-sends a full `frame` keyframe** when it
can't delta, the first grab, a scale change, or a change so large the strip
won't fit, so the client's single reader handles both `frame …` and `delta …`
replies, and the client keeps a persistent canvas it composites onto. Because the
device refreshes its snapshot on every grab (full or delta), the client must seed
its canvas with a full `grab` right after connecting (it can't delta against a
snapshot it doesn't share), the WinForms client does this automatically.

The GUI client is **`pc64/remote/`** (`UnoRemote.exe`, a WinForms single-exe built
by `build-remote.ps1`): live view, mouse/keyboard forwarding, session recording
(ffmpeg → MP4, else a PNG frame sequence), and a raw-command box. See
`pc64/remote/README.md`.

**Server-side capture (`screen record`).** Client-side recording is limited to
the poll FPS and sends every frame over the link. `screen record` instead records
**on the device**, on its shell tick at a steady requested fps, into a RAM ring
(keyframe+delta, same encoding as `grab delta` but with an independent snapshot,
so recording never perturbs the live view). The client starts it, keeps viewing,
then on stop pulls the ring once and reconstructs every frame locally (feeding the
same `Recorder`). The recording fps is thus decoupled from the network: a smooth
capture even when the live view is polling slowly. The ring is a fixed 4 MB
budget; when it fills, capture stops and the stat line reports `dropped`. In the
WinForms client, tick the **"on device"** box next to Record.

## The guard (dead-man's switch for risky verbs)

Some verbs push the device CPU into code that has never run before, the classic
case is driving a NIC bring-up interactively (`iwl mvm` then `iwl rerun` into the
never-executed post-ALIVE sequence). When that wedges, the URC server stops
answering and the box needs a physical power cycle. The **guard** turns that into
an automatic recovery: arm it before the risky op, and if the box can't call home
before the deadline, it hard-resets and re-dials on its own.

```
guard 15 reboot        # ok armed 15s action=reboot token=…
iwl mvm                # arm the untested sequence
iwl rerun              # ← if this wedges, no RSP comes back
   … host stops petting; ~15 s later the box resets and dials home again …
safe                   # if it RETURNED instead, stand the guard down
```

- **"Call home" is any inbound command.** Reaching the URC dispatcher proves the
  box is alive end to end (NIC RX + net + dispatch + main loop), so *any* command
  refreshes the deadline, a strictly stronger liveness proof than "the main loop
  ticked." Refresh is on receipt, so the risky verb gets its full window. `pet` is
  the explicit no-op keep-alive for a legitimately long op. **Crucially, the
  guard's deadline is NOT the freeze-watchdog heartbeat** (`uno_dbg_net_trace()`
  feeds that during WiFi bring-up, so it can't detect a wedge in the very path
  being debugged), the guard has its own deadline that only inbound URC activity
  refreshes.
- **Three firing paths, so whichever context is still alive fires it:** the
  main-loop heartbeat (a *healthy* box whose host went silent, host crash /
  network partition), our own LAPIC timer ISR (detached + wedged main loop), and
  the firmware timer event + UEFI `SetWatchdogTimer` (attached box on real
  hardware). All converge on the existing `wd_fire` → `trap_reset` hard reset.
- **Not covered (v1):** a tight spin with interrupts disabled (no ISR, no main
  loop, no TPL cycle) needs the PCH TCO hardware watchdog, separate silicon,
  filed as a request, out of scope here.
- **v1 action is `reboot`.** The arg slot accepts `reboot` explicitly (and is
  where a future `revert`: roll back to known-good on next boot, will go). An
  armed guard with no host traffic **will reset a healthy box** after the timeout:
  that is the intended semantics, so arm it around a specific op and `safe` it (or
  use the host `with link.guarded(15): …` helper, which arms then stands down on
  return).

## A/B OS update (push a new BOOTX64.EFI over the link)

Iterating on a driver (e.g. WiFi) against a live machine normally means physically
reflashing a USB stick each round. Instead, run **two** UnoDOS sticks, **A** (the
running, known-good OS) and **B** (a spare), and push only the rebuilt
`EFI\BOOT\BOOTX64.EFI` (~1.5 MB) to stick B over the link, then reboot into B. A
driver change touches only that one file; firmware / apps / config on the stick are
untouched, and A stays as the fallback.

The upload is **RAM-staged and written in one shot at `done`**, so a partial or
interrupted transfer never touches stick B, it stays a valid boot disk. The write
goes through `uno_fs_write`, which now writes **firmware-SFS volumes too** (via
`uno_efifs_write`), so an *attached* machine (the driver box builds
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

> **Security, `put`/`reboot`/`bootnext` widen the blast radius.** They are arbitrary
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

### Laying down a bootable tree (headless, no console)

`prepdisk` gives a formatted volume; `put` pushes files; **`mkdir`** creates the
directories in between, the piece that was missing before. A USB stick boots via
the firmware's **removable-media fallback** `\EFI\BOOT\BOOTX64.EFI`, so no NVRAM
`Boot####` entry is needed (it is exactly how the boot USB itself boots):

```bash
# after prepdisk, the new FAT volume shows up in `vols` (say it is vol 2)
mkdir 2 \EFI                 # -> ok created
mkdir 2 \EFI\BOOT            # -> ok created   (parent \EFI must exist first)
put   2 \EFI\BOOT\BOOTX64.EFI …      # stream the loader in (see `put`)
mkdir 2 \APPS ; put 2 \APPS\… …      # and the app modules
reboot                       # flushed to disk first (see the durability note)
```

`mkdir` creates ONE level at a time, so build nested paths parent-first. It is
idempotent (`ok exists` if the dir is already there), so re-running the recipe is
safe. All of this runs post-detach on the native FAT stack, so it needs no
firmware, unlike the on-device Install app, which requires booting to firmware.

### One-shot: `install <disk>`

The manual recipe above is what `install` automates on-device. It **clones the
running OS onto the disk in a single armed verb**: prepdisk, then a native
copy of the whole boot ESP tree (loader + `APPS\` + fonts + everything) straight
disk-to-disk, so no OS bytes cross the network:

```bash
python tools/unoauto_remote.py --listen 0.0.0.0:5099   # then, over the link:
disks                 # find the writable, non-boot target (say idx 1)
arm 1                 # echoes the disk's size; refuses the boot disk
install 1             # prepdisk + clone -> ok installed <n> files <bytes> bytes
reboot                # (writes are flushed first)
```

**What it does NOT do:** write an NVRAM `Boot####`/`BootOrder` entry. Runtime
`SetVariable` is refused once detached (same constraint as the `bootnext` verb),
and URC is always post-detach. So the disk is made bootable via the firmware
**removable-media path** `\EFI\BOOT\BOOTX64.EFI`: a **USB stick auto-boots** from
it (this is the ZimaBlade Kingston case), and an **internal disk** boots via the
firmware's fallback or a one-time boot-menu pick. To get a first-class NVRAM boot
entry you must use the on-device Install app while booted to firmware (attached).

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
print(link.devices())          # [{'loc':'01:00.0','vendor':'8086','driver':None, ...}, ...]
with link.guarded(15):         # arm the dead-man's switch around a risky op;
    link.command("iwl", "rerun")  # a wedge here resets the box, which re-dials
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
- **`tools/screen_qemu.py`** - end-to-end gate for the `screen` verb: boots the
  debug image (reusing `remote_qemu.py`'s harness), asserts `screen info` returns
  a sane size and `screen grab` decodes to a non-blank `w*h*4` RGBA frame, and
  drops it to `/tmp/urc_screen.ppm` to eyeball. It then exercises **delta
  streaming**: `screen grab delta` returns a delta with in-range tile indices
  that suppresses most tiles, compositing that exact delta onto a seeded canvas
  reproduces a fresh full grab (tolerant of live clock/cursor drift), and a scale
  change forces a keyframe. Finally it exercises **server-side capture**: records
  for ~1.5 s, confirms a live grab still works mid-recording (snapshot
  independence), stops, then pulls + reconstructs every recorded frame to a full
  RGBA image. `qoi_decode` / `screen_grab_delta` / `screen_stream` /
  `screen_record_*` in `unoauto_remote.py` are the pure-Python decoder + delta +
  capture helpers it (and any script) uses.
- **`tools/serial_qemu.py`** - the same round-trip with **no network at all**:
  boots with a `remote-serial` DEBUG.CFG and **no NIC device**, driven over the
  guest's COM3 bridged to a TCP socket. Proves the NIC-independent transport
  (the ZimaBlade r8169 case). Uses COM3, not COM1/COM2, see the console-UART
  caveat under "NIC-independent transport" above.
- **`tools/listen_qemu.py`** - end-to-end gate for **listen mode**: boots a
  `listen=5099` DEBUG.CFG, forwards a host port to the guest's listener with QEMU
  hostfwd, dials INTO the box, drives it (uptime/probe), and reconnects to prove
  the listener persists. The reverse-direction sibling of `remote_qemu.py`.
