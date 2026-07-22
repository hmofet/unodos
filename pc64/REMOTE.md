# unoautomate remote channel (REMOTE.md)

A bidirectional link between a running UnoDOS pc64 machine and the PC you
develop from: **remote logging** out, **remote control** in, and free-form
**messages + commands in either direction**, driven from a simple text command
language *or* a Python API on each end. Debug builds only (same `UNO_DEBUG` gate
as the rest of unoautomate); in production every entry point compiles away.

## Shape

pc64's TCP stack is single-connection and **client-only** (`net_tcp_connect`,
no listen), so **pc64 dials OUT** to a listener on the dev PC. You put the dev
PC's address in the stick's `STRESS.CFG`:

```
remote=<ip>:<port>
```

On a debug boot, once the boot net test releases the single TCP connection
(`automate_start` in `pc64_nettest.c`), `unoauto_remote_boot()` reads that key,
brings the NIC up (`pc64_net_up`), and dials. The link is pumped every shell
frame by `unoauto_remote_tick()` (added next to `pc64_nettest_tick` in
`pc64_uui.c`). If the connection drops it reconnects with a short backoff.

> **Auto-discovery is deferred.** pc64 can't yet *send* an L2 broadcast (ARP
> routes `255.255.255.255` to the gateway MAC; only the DHCP path hand-builds a
> true broadcast frame). Broadcast discovery arrives with the fuller ARP/UDP
> stack - see the request in `UNOAUTOMATE-REQUESTS.md`. Until then, set the IP.

> **One connection.** The remote link shares the single TCP connection with the
> Browser / AI apps, so they are mutually exclusive with an active link.

> **Security.** Plaintext, **LAN-only by intent**. Do not expose the listener
> to an untrusted network; it can drive input, launch apps, run Python, and
> power the machine off.

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
| `poweroff` | shut the machine down after the queue drains | `ok bye` |

`probe` row kinds: `0` module (`.UNO` file), `1` window (title), `2` subsystem
(`heap`/`net`/`fs`/`shell`) - see `unoauto.h` for the `v1`/`v2` meanings.

Because either end can send `CMD`/`MSG`, the dev PC can also *register handlers*
so pc64 can drive it back (e.g. an on-device script asking the host to save a
file) - see `on_command` below.

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
