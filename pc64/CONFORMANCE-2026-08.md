# Pre-launch conformance on real hardware, 2026-08-20

A record of how UnoDOS pc64 was conformance-tested on metal before launch: the
method, the instrumentation, what it found, and the things that cost time. It
is written to be enough, on its own, to reconstruct the exercise later - for a
repeat run, or for an article about it.

The premise is worth stating plainly because it shapes everything below: **an
AI agent drove the entire test campaign against an operating system that was
itself written by AI agents.** Nobody typed on the machines under test except
to power them on and move a USB stick. Every app launch, click, keystroke,
screenshot and diagnosis came over a wire.

---

## 1. What was under test

Master at `3bd2b372`, built `UNO_DEBUG=1 UNO_DETACH=1`. Two machines, chosen
because they fail differently:

| box | hardware | why it is in the fleet |
|---|---|---|
| **ZimaBlade** | Apollo Lake Celeron N3350, r8169 wired, eMMC, 960x540 desktop | always-on, headless, no keyboard - everything must work remotely |
| **X13 Yoga** | i5-10210U, 7.5 GB RAM, AX201 WiFi, NVMe, USB ethernet dongle | a real laptop: battery, trackpad, WiFi, and a Windows disk to not destroy |

Three features had **never been tested on hardware at all**: the Studio AI
assistant, OOXML in the office suite, and UnoCode. Those were the point of the
exercise.

## 2. The instrumentation chain

Nothing here is new tooling; the run used what the project already had, which
is itself a finding - the harness was good enough to diagnose faults it was
never designed for.

```
agent -> ssh devbuntu -> urc_bridge.py -> TCP -> the box's own network stack
                      \-> zgrab.py (screen grab / click / type, via the bridge files)
                      \-> syslog_recv.py :5514  <- the box's live kernel log
                      \-> the USB stick, read directly when the box is dead
```

- **URC** (the unoautomate remote channel) is the spine. The box **dials out**
  to devbuntu, so nothing listens on the box. Verbs are appended one per line
  to `~/urc-multi/<box>/cmd.txt`; replies land in `session.log`.
- **`zgrab.py`** turns that file interface into `shot()`, `click()`,
  `type_str()`. Screenshots come back as QOI and are written as PNG - and the
  agent can *read those PNGs directly*, which is what made blind UI driving
  possible. Every UI verdict in this document was reached by looking at a
  picture.
- **`conf_run.py`** (written during the run) is the batch runner: launch every
  app by id, screenshot each, assert liveness between steps, log to a TSV.
- **`\LOGS\LOG.CFG`** arms remote syslog **from boot** (`level`,
  `remote_level`, `remote=host:port`). This matters enormously on a box that
  dies 90 seconds after boot: arming by hand is a race you lose.
- **The stick itself** is the last resort and the best one. Pulled into
  devbuntu it gives the full `CRASH\<MACHINE>\` tree with no transfer cap and
  no race.

## 3. The method

1. **Gate first, on quill.** Full merge gate (73 golden checks, host tests for
   csslib/quickjs/unodoc/unocode, TLS gates, SPECTEST in QEMU) before anything
   was written to a stick. A trip to a box is never spent discovering a build
   problem.
2. **Prove the image in QEMU**, including booting the *physical stick* with
   `snapshot=on` so the stick is byte-identical afterwards.
3. **Flash by serial, never by device letter**, with the write-blocker dance
   and a `trap` that restores it on any exit path.
4. **Identify the box from its own hardware before driving it** (see §5).
5. **Observation before mutation**: `vols`/`disks`/`devices`, then app
   launches, then anything that writes, then anything that reboots.
6. **Anything risky runs behind the box's own watchdog** (`guard <s> reboot`),
   because a wedge otherwise needs a human.
7. **Screenshot everything.** A figure you would refuse to publish is a bug
   report.

## 4. Results

86 recorded results across the two machines; 57 passed. Published (behind the
portal's Access gate) at `home.arinbakht.ca/unodos-conformance-results.html`.

**Passed on metal:** 23/23 apps launch; the security store and session file
land on the boot volume; Duum interactive; the whole office suite; UnoCode
including its integrated terminal (`js 6*7` -> 42 on the live unojs engine)
and a sample extension that ran and read the active editor; SSH keygen plus
`ssh run` to a real host with exit 0; the browser loading a real HTTPS site;
JPEG/WebP/QOI decoding, and malformed fuzz-corpus images rejected *gracefully
with a specific reason*; Control Panel across all six tabs; a `.wsz` Winamp
skin decoded and applied; a resolution change that **auto-reverted when left
unconfirmed**; Runner3D switching the real GOP mode and restoring it on exit;
two hours of continuous uptime at 60 fps and 96% idle.

**Accounts and the sign-in gate pass end to end** - admin creation through the
Control Panel (injection reaches the security sheet only because `ui-unlock`
is set), the gate appearing on the next boot, remote authentication, and
deletion. The load-bearing part: **the machine still reaches its remote
channel while gated**, so a headless box is never stranded at a password
prompt.

**First-ever hardware tests, all three passed their core function:**
- **Studio AI assistant** - a live round trip to Anthropic over real TLS,
  which in one test proves DNS, TLS with certificate validation against the
  bundled trust store, the HTTPS POST and the JSON reply parse.
- **OOXML** - `.xlsx`, `.docx` and `.pptx` all open with correct content, and
  `.xlsx` survives a full round trip back through our own writer.
- **UnoCode** - file tree, minimap, syntax highlighting, terminal, at 60 fps.

**Four defects found and fixed during the run** (all gated and on master):
1. `unossh_store.c` and `iwlwifi.c` still carried copies of the pre-fix
   lowest-index volume picker, so the SSH key store and saved WiFi credentials
   went to whatever disk enumerated first - on the ZimaBlade, an eMMC that
   accepts a create and never completes the write, which hangs the machine.
2. **The guest was silent with >=4 GB of RAM.** The AC'97 BDL and ring lived in
   `.bss`, which lands above the 4 GB line on a big-memory machine, and the
   32-bit `PO_BDBAR`/BDL casts truncate. Measured: 3 GB peak 6259, 4 GB peak 0;
   after the fix both play.
3. **`arena_init_lowmem()` called boot services after ExitBootServices**, on
   the strength of a comment asserting WiFi always brings up first. See §6.
4. The same pattern, inherited by copy, in the AC'97 fix written earlier the
   same day - fixed in the same commit rather than left as the next crash.

### Still open at the end of the run (not fixed - operator's call)

- **Start menu > Power > Restart never dispatches.** Established by controlled
  comparison: the *identical* long-hold click opened Clock from the same menu
  seconds earlier (confirmed by `probe`), while Restart highlights on hover and
  then does nothing; Enter on the highlighted item is equally inert; uptime
  keeps climbing and no re-dial happens. The reset mechanism is fine - the URC
  `reboot` verb reboots this box normally - so it is the menu action that does
  not fire, and **a user cannot restart from the interface**. `noshutdown` in
  DEBUG.CFG is not the cause; it only gates the stress driver's auto-poweroff.
- **Session restore reopens the wrong window.** `SHELL.CFG` correctly records
  `restore=1`, `open=files,clock,unoamp` and per-app geometry, and "Restore
  last session" is ticked - yet after reboot exactly one window returns and it
  is **Control Panel**, which was not in the list. Reproduced twice. Control is
  index 0 in the native app table, which is the shape of the bug.
- **A Python app whose `draw()` raises reports nothing** (§7).
- **WiFi is WPA2-only** (§8) - addressed after the run on branch `wpa3-sae`.
- **The Yoga's network path collapses** (§6).

Minor and deferred: the Audio tab draws its value over its label; the office
Open dialog ignores typed input entirely; Studio opens binary `.UNO` files in
the text editor as garbage; `nst` reports failure on a machine whose
networking demonstrably works; and menu items need a **~1.2 s press hold** to
activate under injection - 0.35 s registers as hover only, even at 60 fps.

### Not covered, stated plainly

Audible sound and "what is on the frozen screen" both need a person in the
room. Appliances could not run: the guest kernel is 17 MB against an 8 MiB
push cap, and the images were on the other machine's stick. Shut Down was not
exercised (Restart already showed the Power items do not dispatch, and a
successful power-off needs a human to switch the box back on), and the
production-image boot is deliberately last because it removes the channel.
The Yoga matrix is roughly a fifth covered.

## 5. Traps, in the order they cost time

**Identify the machine from its own hardware, never from a label.** The two
sticks went into the opposite boxes. The bridge cheerfully labelled the
ZimaBlade "zima" from a stale IP map while it was running the *Yoga's*
overlay and dialing the Yoga's lane. `devices`/`disks` settle it in seconds
(Apollo Lake + r8169 + eMMC is one box; an NVMe is the other), and the USB
disk's sector count names which stick is in it.

**Never guess telemetry filenames - list the directory.** A remote probe for
`HG001/HG002/CR001/CR002` returned "not found" for all four, and the honest
conclusion written at the time was "no telemetry whatsoever". The box had
**nine** reports; this machine's numbering starts at 005. Pulling the stick
and running `ls` found them instantly and root-caused a day's mystery.

**Debounce liveness.** URC verbs intermittently exceed the bridge's 15 s
timeout under load. Every single "the box is dead" scare during this run was
a slow verb; the box was always alive on a re-check. Break on three
consecutive failures, never one.

**A comment asserting an invariant is not evidence.** The crash in §6 was
guarded by a comment saying the code could only run before ExitBootServices.

**An interrupted URC push writes nothing.** `put` stages chunks in an 8 MiB
`.bss` buffer and only touches the disk at the `done` step. Worth knowing
before panicking about a truncated kernel, as happened here.

**`nonet` is blunter than it reads.** It is documented as skipping the network
*test*, but `net_init()` - which binds the stack to a NIC - lives inside that
test, so setting it leaves the machine with no networking at all and no remote
channel. The screen says "no link (no NIC bound)".

**A `pkill` pattern matches the shell running it.** Killing the bridge from an
inline ssh command killed the ssh session first, leaving the bridge dead
rather than restarted. Put it in a script file.

**The office Open dialog is mouse-only.** Its File name field ignores injected
keys entirely. Push the fixture to the RAM disk so its row is directly
clickable - which is exactly what `tools/uoffice_ooxml_urc.py` documents, for
exactly this reason.

## 6. The Yoga crash loop: a worked example of the method

Symptom: a laptop that died every few minutes, sometimes pinging while
unreachable, sometimes gone entirely. Diagnosis, in order:

1. **Ask what the screen says.** "Frozen on the desktop" ruled out a modal
   dialog walling off the channel, and meant the shell loop was dead while
   something low-level still answered ICMP.
2. **Pull the stick and read it.** Nine reports: `CR005` plus eight
   hang/reset pairs. `BOOTS.TXT` proved the loop began at the *first* boot,
   before any remote contact - which exonerated the `devices` verb that had
   been blamed.
3. **Read the crash.** `vector 6 #UD, UBSAN TRAP at arena_init_lowmem+0xb5`,
   checkpoint `net:wifi-bringup`, **`detached: 1`**. The WiFi driver
   dereferenced `ST->BootServices` after the firmware was gone. Fixed by
   asking `uno_pc64_detached()` first, as every other driver already does.
4. **Read the hangs.** Twelve of the thirteen failures are not that crash:
   they are `last_checkpoint: net:dhcp` (or `net:dns`), main loop silent past
   20 s, LAPIC watchdog reset.
5. **Arm the live log and catch it in the act.** Gateway pings of **267 ms,
   1020 ms, 3806 ms** on a LAN - degrading roughly fourfold per round. Later a
   URC transfer measured **1.1 KB/s on a gigabit link**, still decelerating.
   One bug, not three: the whole network path collapses.

**Still open.** The degradation itself. Note the limitation this exposed:
**remote syslog cannot see this class of fault**, because the code that ships
log records stops when the main loop does. Catching it needs instrumentation
outside the main loop.

## 7. The AI assistant, tested as a user would use it

The assistant was asked, on the machine, to write a complete UnoDOS Python
app. Its code was taken verbatim, put on the box, and run through Studio's
normal Ctrl+R path.

It got the *shape* right: subclassing `uno.App`, a module-global `app`,
correct `fill_rect` and `uno.rgb` signatures. Studio packaged and launched it.
**It rendered nothing.** Isolated with a three-way A/B:

| version | change | result |
|---|---|---|
| the AI's code | `cv.clear(0)`, animation in `draw()` | blank |
| control 1 | `build()` instead of `__init__` | still blank |
| control 2 | `uno.rgb(0,0,60)` + animation in `tick()` | **renders** |

So `cv.clear(0)` - a raw int where a colour is required - is fatal. An earlier
answer also called `self.cv.width()`, a member the runtime never sets.

**The more important finding is ours, not the model's:** when that app's
`draw()` raised, the system reported *nothing*. Blank window, no error in
Studio's output pane, no dialog, no log line, while the status bar said
"Running". A one-character API mistake becomes an unsolvable mystery. This
affects all Python app development, not only AI-assisted work.

## 8. WiFi: WPA2 only, and what that costs

The standing "associates then never gets a lease" defect is **resolved** - the
Yoga joined an Eero guest SSID through the Control Panel and got a DHCP lease.

But it **cannot join the main SSID of an Eero Pro 6E**. `wpa_build_rsn_ie()`
advertises exactly one AKM, `00-0F-AC-02` (PSK), accepts only PSK/PSK-SHA256
with CCMP, and sets RSN capabilities to `0x0000`: **no SAE, so no WPA3, and no
PMF/802.11w**. Modern Wi-Fi 6E/7 routers default to WPA3 or WPA2/WPA3
transition with PMF, and 6 GHz *mandates* WPA3. The guest network worked
precisely because guest SSIDs are commonly WPA2-only. As shipped, a large and
growing share of home networks are unjoinable.

**Addressed after this run** (branch `wpa3-sae`, see
[WIFI-SECURITY.md](WIFI-SECURITY.md)): WPA3-SAE over group 19, an AKM-aware
4-way handshake, and PMF. One correction to the diagnosis above, which had the
cause right and the mechanism half right: the missing AKM was not the whole
story. The driver did not parse a beacon's RSN element **at all** -
`wpa_rsn_ie_ok()` existed and had zero callers - so a WPA3 BSS was not
declined, it was attempted with the wrong AKM, and the deauthentication that
came back was indistinguishable from a wrong password. That is why the failure
carried no diagnostic content. `iwl sec` now prints what each scanned BSS
offers and what we would negotiate with it.

Verified on the host against an independent model of the 802.11 text; **not yet
against a real access point**. Re-testing SKYNET on the Yoga's AX201 is the
open item.

## 9. For the article

The threads worth pulling later:

- An AI agent tested an AI-written OS on real hardware and found four real
  bugs, three of which were *inherited by copy-paste between drivers* - the
  same volume-picking heuristic and the same boot-services assumption,
  propagated because each new driver was modelled on an existing one. The
  fourth was introduced during this very session by the same mechanism, and
  caught for the same reason.
- The most valuable diagnostic moves were the least sophisticated: read the
  screen, list the directory, pull the disk.
- Screenshots as the agent's primary sense. Every UI verdict here came from
  looking at a PNG, including catching that a click had silently run the wrong
  file.
- The failure modes that are invisible without a human: sound, and a frozen
  picture. Both needed someone in the room.
- Honest negative results matter: the assistant wrote confident, well-shaped,
  non-working code, and the OS gave the user no way to find out why.

---

## 10. What happened to the open items (2026-08-21)

Written the day after, as an addendum rather than an edit: §4's "still open"
list is what the run itself found, and rewriting it would erase the finding.
Full detail, including the before/after measurements, is in
`UNOAUTOMATE-REQUESTS.md` under the 2026-08-21 heading.

| §4 item | now |
|---|---|
| Start > Power > Restart never dispatches | **fixed** - the right pane's canvas had a null event handler, so Tile/Cascade/Minimize all/Restart/Shut Down were all dead. Restart takes uptime 66,866 ms -> 18,851 ms |
| A Python app whose `draw()` raises reports nothing | **fixed** - PYRT paints the traceback into the app's own window and logs it at `LOG_ERR` |
| Session restore reopens the wrong window | **fixed, root-caused on the box** - the dead eMMC carries a ZERO-BYTE `SHELL.CFG` and the reader stopped on the first read that was not *negative*. See below |
| WiFi is WPA2-only | **held** - another lane |
| The Yoga's network path collapses | **held** - Yoga-specific, deferred |
| Audio tab draws its value over its label | **fixed** |
| Office Open dialog ignores typed input | **fixed** - `uof_sync` overwrote the field on every sync |
| Studio opens binary `.UNO` files as garbage | **fixed** - sniffed and refused |
| `nst` reports failure on a working machine | **fixed** - it dials the QEMU slirp gateway; it now says so, and takes a real host |
| Menu items need a ~1.2 s press hold | **not a real threshold** - it was the dead right pane. 0.18 s activates Restart first time |

Two things were found while fixing the above that the run did not catch, both
worth recording because of *why* it missed them:

- **Leaving a fullscreen app did not restore the resolution.** Runner3D renders
  at a quarter of the panel in each axis and only its CLOSE path undid that,
  while eight separate places drop a window out of fullscreen. §4 records
  "Runner3D switching the real GOP mode and restoring it on exit" as a PASS -
  and it was, because the run closed it with the title-bar close box, the one
  route that happened to work. **Esc**, which is what the game's own HUD tells
  you to press, left the desktop at 320x200 with the game still running.
  A test that exercises one exit from a state does not test the state.
- **`power_down()` never asked the firmware to reset.** The comment above it
  says ResetSystem is the last resort for a reset; the code only called it for
  power-off. Invisible on any machine where CF9 works, which is all of ours.

The pattern in both is the same as the run's own headline finding: an invariant
asserted in a comment rather than expressed as a check.

### 10a. Re-verified on the ZimaBlade, 2026-08-21 - and one root cause that only the box could give

The fixed build was pushed to the running ZimaBlade over URC (`push <vol> <path>
<localfile>` through the bridge: the kernel plus PYRT, STUDIO, UOWORD, UOCALC
and UOSHOW, 5.7 MB, every transfer byte-verified at ~45-145 KB/s) and the box
rebooted onto it. Nobody touched the machine.

| item | before | after |
|---|---|---|
| Start > Restart | menu stays open, uptime climbs | uptime 267,130 ms -> 50,562 ms |
| Start > Tile | highlights, does nothing | two windows tiled, menu closed |
| session restore | one window: Control Panel | all four saved windows return |
| Runner3D + Esc | 480x270, game still running | 960x540, app closed |
| Control Panel > Audio | `Output devic` overdrawn | `Output device: HD Audio` |
| Open dialog typing | `ABC` -> `README.TXT` | `ABC` |
| Studio opens a `.UNO` | mojibake, saveable back | refused, document untouched |
| Python `draw()` raises | blank window, silence | traceback on screen AND in unolog |
| `nst` | `connA=0 connB=0 accepted=-1` | `connA=2 connB=2 accepted=4 peer=192.168.2.100:60364` |

**THE SESSION-RESTORE ROOT CAUSE WAS NOT WHAT §4 GUESSED, AND ONE COMMAND ON THE
BOX SETTLED IT.** §4 wrote "Control is index 0 in the native app table, which is
the shape of the bug". It is not an index bug at all:

```
py uno.size(1,'SHELL.CFG')  ->  0        the dead eMMC
py uno.size(2,'SHELL.CFG')  ->  178      the boot stick, restore=1 and every pref
```

`prefs_read` scanned from volume 0 and stopped on the first read that was not
NEGATIVE. **Zero is not negative.** Every boot parsed an empty buffer, so
`restore=` and `open=` were both absent and the fallback opened Control Panel.
The same empty read was discarding every Control Panel PREFERENCE on that
machine - theme, resolution, scale, volume, wallpaper, pointer speed - every
boot, for as long as that zero-byte file has existed. Nobody noticed, because a
default looks like a choice.

The lesson generalises past this bug: **the eMMC's failure mode is not "writes
fail", it is "writes leave a plausible-looking artefact"**. A create that never
completes leaves a real directory entry of length zero, and every "is it there?"
test in the tree answers yes.

**Two harness facts for the next run:**

- The bridge's `push` verb is the whole A/B update path and it works: a headless
  box took a new kernel over its own link three times in one session with no
  physical access. `push_file` verifies, and `put` stages in an 8 MiB `.bss`
  buffer and only touches the disk at `done`, so an interrupted push writes
  nothing.
- **There is no delete verb in URC and the Files pane does not scroll**, so a
  file pushed into `APPS\` cannot be removed remotely. Truncating it to zero
  with `uno.write(vol, path, b"")` deregisters it (no 48-byte header, so
  `uno_mod_desc_read` skips it) and is the only remote cleanup available. Worth
  a `rm` verb if anyone is taking requests.
