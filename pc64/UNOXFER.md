# unoxfer - UnoTransfer

The file-transfer subsystem: one protocol-agnostic seam, several backends
behind it, a recursive transfer engine on top, and three front ends - the
windowed app, the terminal, and the `xfer` URC verb.

Ported from **Portage** (`hmofet/Portage`, C# / WinUI), whose architecture is
the reason this document is short: Portage's whole design is one interface
(`ITransferClient`) that the UI, the queue and the sync engine talk to, so
adding a protocol is one new implementation and one registration. That idea
survives the port intact. What did not come across is deliberate and listed
under **What was left out**.

## Why it exists (the part that is not "Portage but in C")

URC's `put` verb caps a single upload at **8 MB** - the RAM staging buffer -
and every byte crosses the URC link, which is a plaintext LAN channel with a
512 B/tick TX pump. That is right for pushing a 1.5 MB `BOOTX64.EFI` at a box
and wrong for everything else: a WAD, a video, a source tree, a disk image.

So the `xfer` verb does not carry bytes at all. It tells the box to **fetch
them itself**, over a real transfer protocol, straight from the machine that
has them. The URC link carries a request and a progress line; the payload
takes the shortest path to the disk it is landing on. The cap goes away
because the buffer does.

## The seam

```c
typedef struct unoxfer_client unoxfer_client;

unoxfer_client *unoxfer_open(const unoxfer_site *site, char *err, int errcap);
int  unoxfer_list (unoxfer_client *c, const char *path, unoxfer_ent *out, int max, int *total);
int  unoxfer_get  (unoxfer_client *c, const char *rpath, int vol, const char *lpath, unoxfer_prog *p);
int  unoxfer_put  (unoxfer_client *c, int vol, const char *lpath, const char *rpath, unoxfer_prog *p);
int  unoxfer_mkdir(unoxfer_client *c, const char *path);
int  unoxfer_del  (unoxfer_client *c, const char *path);
void unoxfer_close(unoxfer_client *c);
```

Seven calls, and every backend implements the same seven through a vtable
(`unoxfer_backend` in `unoxfer.h`). Nothing above the seam switches on the
protocol; `unoxfer_open` picks the backend from `site->proto` and that is the
only place the enum is read. This is `ITransferClient` and
`DefaultTransferClientFactory` with the C# filed off.

A backend that cannot do an operation returns `UNOXFER_EUNSUP` rather than
faking it. TFTP has no listing and no delete; SCP cannot resume. The engine
above asks `unoxfer_caps()` before it plans, so an unsupported operation is a
plan that was never made rather than a transfer that fails halfway.

## Backends in v1

| `unoxfer_proto` | Rides on | Notes |
|---|---|---|
| `UNOXFER_LOCAL` | `pc64_fs.h` / `fat.h` | volume-to-volume copies; the left pane |
| `UNOXFER_SCP` | `unossh` `ssh_exec` | the SCP wire protocol (`scp -f` / `scp -t`), recursive with `-r` |
| `UNOXFER_SFTP` | `unossh` `ssh_subsystem` | **weak-linked**, see "Waiting on other lanes" |
| `UNOXFER_HTTP` / `UNOXFER_HTTPS` | `pc64_http.h` | fetch by URL; no listing (`EUNSUP`) |
| `UNOXFER_WEBDAV` / `UNOXFER_WEBDAVS` | `pc64_http.h` | PROPFIND for listing, PUT/GET, MKCOL |
| `UNOXFER_TFTP` | `netsock.h` UDP | RFC 1350; get/put by exact name, nothing else |

FTP/FTPS, S3, Azure and SMB are **not** in v1. They are not blocked by
anything, they are just more work than the seam needs to be proved; each is
one new `unoxfer_backend` and one row in `open_for()`. S3 and Azure are mostly
HMAC-SHA256 request signing, which BearSSL already provides; SMB is a protocol
subsystem in its own right and should be filed as one before anybody starts it.

## The engine (recursive transfers)

`unoxfer_job.c` is what turns "seven calls" into "pull that directory". It
walks the remote tree breadth-first, creates each local directory as it goes,
and streams each file. It is a **plan then run** design, taken from Portage's
`SyncEngine`: the walk produces a count and a byte total before a single byte
moves, so the caller (and the URC client) gets a real percentage instead of a
spinner, and a job that is going to fail on a missing directory fails during
planning.

Three properties worth stating because they are the ones that get lost in a
rewrite:

- **Partial then rename.** A file lands under a work name and is renamed onto
  its real name (`uno_fat_rename`) only after the last byte verifies. A killed
  transfer therefore leaves a visibly incomplete file, never a
  truncated-looking real one. The rename is the commit point.

  The work name is `<base>.$$$`, **not** `<name>.PART`, and that is not a style
  choice: `HTTP1.TXT.PART` is two dots and a four-character extension, which
  FAT will not accept, so every partial write failed and the error surfaced
  against the *final* path - which is not where the bad name was. `$$$` is the
  DOS convention for a work file and is legal 8.3.
- **Resume is from the partial's size.** Reconnecting to a half-done job asks
  the backend to start at that size. Backends that cannot seek (`SCP`, `TFTP`)
  report so through `unoxfer_caps()` and restart the file; they do not silently
  append to a partial and hand back a corrupt file. HTTP and WebDAV advertise
  resume only once streaming is live, because without an append a resumed body
  would be staged alone and committed as the WHOLE file.
- **Nothing blocks the desktop.** The engine is a step machine:
  `unoxfer_job_step()` does a bounded slice of work and returns. The app calls
  it once a frame; the URC verb calls it from the remote tick. A stalled server
  costs a frame, exactly as `ssh_poll` does in the SSH app.

### The single-file size cap, and why it is temporary

`pc64_fs.h` / `fat.h` can write a **whole file from a buffer** and cannot
append to one. There is no `uno_fat_write_at`. So a streamed download has
nowhere to put byte 16,777,217 except a bigger buffer.

v1 therefore stages one file at a time in a heap buffer sized by
`unoxfer_stage_cap()` (default 8 MB, settable through `xfer stage`), and a
**job** - a whole recursive directory - is unbounded, because the buffer is
reused per file. A four-gigabyte source tree of ordinary files pulls fine; a
single four-gigabyte ISO does not, yet.

The buffer is allocated on first use, **halved on failure** down to 64 KB
rather than refusing, and freed when the last holder lets go: the heap is 32 MB
and shared with Studio's compile arena, the browser and the Python VM, so a box
under memory pressure should transfer slowly in small pieces rather than take
the desktop down with it. There is exactly ONE buffer for the whole machine,
which is also what serialises two concurrent jobs' file writes.

The fix is one call in unofs's lane, requested on 2026-08-22 (see
`UNOAUTOMATE-REQUESTS.md`). unoxfer already calls it through a **weak symbol**:

```c
__attribute__((weak)) int uno_fat_append(int vol, const char *path,
                                         const unsigned char *buf, long len);
```

When unofs lands the strong symbol the engine streams in fixed windows and the
per-file cap disappears with no change here - the `r8169_dbg_cmd` pattern,
applied to a filesystem call. `unoxfer_streaming()` reports which mode is live
so the app and the verb can say so rather than surprising someone at 8 MB.

## The `xfer` URC verb

A pass-through, exactly like `ssh` / `iwl` / `eth`: unoautomate lands one weak
stub and one dispatch clause, and the sub-verb grammar and output format are
ours (`unoxfer_cmd()` in `unoxfer_cmd.c`). Full grammar: `xfer help`.

```
xfer site <name> <proto> <host> <port> <user> [key]   save a site
xfer sites                                            list them
xfer siterm <name>                                    forget one
xfer ls <site|url> [path]                             one listing
xfer pull <site|url> <rpath> <vol> <lpath> [-r]       START a job (returns at once)
xfer push <site|url> <vol> <lpath> <rpath> [-r]       the other direction
xfer status [id]                                      progress of a running job
xfer cancel <id>                                      stop one
xfer log <id>                                         the per-file result list
```

`pull` and `push` are **asynchronous by construction**. They validate, plan,
and return an id; the job then runs on the shell tick. This is not a
convenience - a URC command that blocked for the length of a multi-gigabyte
transfer would stall the dispatcher, miss the guard's deadline, and get the
box hard-reset by its own dead-man's switch. `status` is how you watch it.

```
> xfer pull nas /srv/media/ep01.wav 1 \MEDIA -r
ok id=3 planned files=12 bytes=418334720
> xfer status 3
ok id=3 state=running files=4/12 bytes=139460608/418334720 33% cur=ep04.wav
> xfer status 3
ok id=3 state=done files=12/12 bytes=418334720/418334720 100% errors=0
```

### Privilege: `xfer` is SYSTEM

Its `GATE[]` row is `UNOAUTO_P_SYSTEM`, landed in the same commit as the verb
because the table is fail-closed. The reasoning is `ssh`'s, only more so: it
**writes arbitrary files anywhere on any writable volume** (that is `put`'s
tier on its own) and it **authenticates to other machines with this box's
stored credentials** (that is `ssh`'s tier). Either one alone puts it in
SYSTEM. It is emphatically not DRIVE: nothing a person at the keyboard can do
includes "write 40 GB to the boot volume from a host I chose".

Sites are stored through unossh's session store where the protocol is
SSH-based, so an SCP/SFTP site needs no second credential store and no second
place to leak a key. Non-SSH sites (HTTP, WebDAV, TFTP) hold no secret in v1:
a URL may carry one, and one that does is refused by `xfer site` rather than
written to disk.

## The terminal

`unoterm.c` is a **real VT emulator** - parser, screen buffer, scrollback -
ported from Portage's `TerminalScreen.cs`. It is a separate file from anything
that draws, because the emulator is pure computation and therefore
host-testable: `tools/unoterm_test.c` builds it on the dev PC and feeds it
capture files.

This is the piece the SSH app was missing. `sshapp_ui.c` appends channel bytes
to a flat 4 KB scroll buffer, which is fine for `echo hello` and wrong for
anything that moves the cursor: `top`, `vim`, a progress bar, even `ls` with
colour, all render as escape-sequence soup. `unoterm` takes the same bytes and
maintains a cell grid, so the app draws a screen instead of a log.

Supported: CSI cursor motion, ED/EL erase, SGR (16 + 256 colour, bold,
inverse, underline), scroll regions, DECAWM, alternate screen buffer, DECSC/
DECRC, tabs, OSC 0/2 title, and the C0 set. Not supported and deliberately so:
double-width lines, sixels, mouse reporting, and anything that needs a font
this OS does not have.

## Waiting on other lanes

Two things are requested and weak-linked, so this subsystem builds and works
without either, and improves the moment they land. Both are in
`UNOAUTOMATE-REQUESTS.md` under 2026-08-22.

1. **unofs: `uno_fat_append()`** - removes the per-file staging cap (above).
2. **unossh: `ssh_subsystem(handle, "sftp")`** - five lines beside `ssh_exec`;
   `channel_request()` already has exactly the right shape. Until it lands,
   `UNOXFER_SFTP` reports `EUNSUP` at `unoxfer_open` and the app offers SCP,
   which does the same job over the same connection.

And one **finding filed against unossh** rather than a request, because it is
a correctness bug and not a feature: `ua_data_in()` appends channel data to a
`SSH_RBCAP` (16 KB) ring and **drops the overflow silently**, while
`open_session()` advertises a 32 KB maximum packet and `ssh_poll()` will
dispatch up to 64 packets before the caller reads any. Interactive use never
notices; a bulk transfer loses bytes in the middle of a file. unoxfer's SCP
backend drains after every single poll to keep the ring shallow, which makes
the loss unlikely rather than impossible - the fix belongs in unossh.

## What was left out (from Portage, on purpose)

- **yt-dlp, the link grabber, the web scraper, the media-format picker.**
  Requested out of scope. With them go `ILinkAnalyzer`, `ProposedDownload`,
  `MediaFormat` and `ToolManager` - the entire "paste a URL and pick a
  quality" half of Portage. `UNOXFER_HTTP` is a plain fetch of a URL that is
  already a file.
- **The plugin host.** Portage loads download and tool engines out-of-tree
  through an `AssemblyLoadContext`. UnoDOS has `.UNO` modules for that, and
  the transfer engine has to be in the kernel image anyway because the URC
  verb dispatches before any app is loaded.
- **Workspaces, tab groups, multi-window.** A shell-native app has one window,
  as the SSH app and the Browser do.
- **The Windows Credential Manager.** unossh's encrypted store already holds
  the keys and sessions this needs, and adding a second secret store to hold
  the same secrets is how one of them ends up stale.

## Files

| File | What |
|---|---|
| `unoxfer.h` | the seam: types, the vtable, the public API |
| `unoxfer.c` | backend registry, site store, URL parse, path helpers |
| `unoxfer_job.c` | the plan/run recursive engine and its step machine |
| `unoxfer_local.c` | the local-volume backend |
| `unoxfer_scp.c` | SCP over `ssh_exec`, and the SFTP weak-link |
| `unoxfer_http.c` | HTTP/HTTPS and WebDAV/WebDAVS over `pc64_http` |
| `unoxfer_tftp.c` | RFC 1350 over a UDP netsock |
| `unoxfer_cmd.c` | the `xfer` URC verb's grammar and report format |
| `unoterm.h` / `unoterm.c` | the VT emulator |
| `xferapp_ui.c` | the windowed app: dual pane, queue, terminal tab |
| `tools/unoterm_test.c` | host build of the emulator + its fixtures |
| `tools/xfer_qemu.py` | the HTTP-family gate, and the WebDAV server it tests against |
| `tools/xfer_scp_qemu.py` | the SCP gate, and the throwaway sshd it tests against |

## Verification

Three layers, each testing what it can test honestly.

- **`tools/unoterm_test.c`** - host-side, no device. Builds `unoterm.c` itself
  (it includes nothing, so there is nothing to shim) and asserts the resulting
  cell grid: CR-overwrite progress bars, scroll regions with the rows outside
  them untouched, alt-screen enter-and-leave with the primary intact, UTF-8
  split across two feeds, a stray continuation byte resyncing rather than
  poisoning the rest, and an undersized init block refused. 42 assertions.
- **SPECTEST `network` / `xfer:local` + `xfer:url`** - on the device, in a
  debug build. The capability bits must agree with what the volume can actually
  do (a read-only volume must not advertise `put`), and a URL carrying a
  password must be REFUSED - a security property, not a convenience.
- **`tools/xfer_scp_qemu.py`** - end to end over SSH, against a **throwaway
  sshd** started under a scratch directory on a chosen high port with its own
  host key and its own `authorized_keys`, killed at the end. It touches nothing
  in `~/.ssh`: a test that edits the developer's own `authorized_keys` to prove
  a client works has traded a real risk for a convenience. The key is generated
  ON THE DEVICE (`ssh keygen`) and its public half read back over URC, which
  exercises the one credential path a headless box actually has. It covers
  `ls -l` parsing, a recursive pull verified byte for byte, a push read back on
  the HOST, and - by restarting the server with a fresh host key - that a
  CHANGED host key is refused.
- **`tools/xfer_qemu.py`** - end to end over the HTTP family, and the one that
  proves the headline claim. The host runs a small WebDAV server; the gate drives
  `xfer pull … -r` over URC, polls `status` to completion, and checks every
  file with a **position-weighted** byte sum on the device (the guest has no
  `hashlib`, and a plain sum would miss a file whose bytes arrived rearranged).
  It also asserts that no work file survived the commit, that a finished job is
  still queryable, that a recursive TFTP pull is refused up front with a
  reason, and that the collection does not appear inside its own listing.

  The WebDAV server is written IN the gate rather than pulled from a library,
  so the PROPFIND parser is tested against XML the parser's author did not also
  write: a non-`D` namespace prefix, percent-encoded hrefs, and the self-first
  ordering a real server sends - the three things a hand-rolled WebDAV client
  gets wrong.

### Six bugs the gates found that review had not

Worth recording, because each compiles, and most survive a smoke test:

1. **`pull` and `push` both have `'u'` at index 1.** `sub[1] == 'u'` made every
   pull a push, which then failed as "this protocol cannot upload" and pointed
   at the backend.
2. **A tight `net_poll()` loop wedges TCP.** net.c derives part of its notion of
   time from how often `net_poll()` is called (`g_ticks * 5u`, and the DHCP and
   retransmit stages are commented "~5 ms"), so spinning on it runs the stack's
   timers hundreds of times too fast. The symptom was a socket stuck in
   SYN_SENT against an address that `nst` connected to in two seconds. Every
   wait loop now pumps through `ux_pump(idle)` and yields when nothing moved.
3. **A negative `net_send` is back-pressure, not failure.** netsock keeps one
   segment in flight and returns -1 while it is outstanding. Treating that as
   fatal made large request bodies fail intermittently, which reads as a flaky
   server and is not.
4. **`ls -l` column off-by-one.** The name is column 7 with `--time-style=+%s`
   and column 9 without; the walk looked for 8 and 9. The effect is not a wrong
   size - it is NO ROWS AT ALL, because the name column is never reached and
   every row is rejected. A working connection, a working exec, and an empty
   directory. (The epoch form is now *proved* by checking that column 6 is a
   long run of digits, because otherwise a default listing "parses" with the
   day of the month as the filename.)
5. **The staging allocator refused anything below its own floor.** The halving
   loop stops at 64 KB, so a request *smaller* than that fell out of it having
   allocated nothing - and the caller was told the buffer was busy. Every small
   file failed and every large one worked, which is the opposite of the shape
   you go looking for.
6. **Trust-on-first-use that never records the first use is just trust.**
   `ssh_verify_host()` only ASKS the store; it does not write to it. Accepting
   UNKNOWN and moving on meant the key was never written down, so every later
   connection was UNKNOWN too and a CHANGED host key could never be detected.
   Indistinguishable from working until the day it matters. The gate caught it
   by restarting the server with a fresh host key and watching the box connect
   happily.
