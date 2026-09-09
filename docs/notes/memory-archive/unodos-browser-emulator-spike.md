# pc64 runs in a browser under QEMU-wasm (Outcome 2). PUBLIC at unodos.arinbakht.com/try/ and private at unodos.arinbakht.ca; qemu-wasm ships NO graphical display, so a canvas backend had to be written; Pages caps assets at 25 MiB

Archived verbatim on 2026-09-08 from the Claude Code memory file `unodos-browser-emulator-spike.md`. This is dated session history kept for reference. Later sections supersede earlier ones, so read bottom-up for the current state. The durable facts now live in the memory file itself and in the repo docs.

---


Phase 0 of the browser-emulator plan, run 2026-08-18. Full write-up and the
reusable artefacts are committed at `hmofet/unodos-launch` `SPIKE-RESULTS.md`
+ `spike/` (`ui-wasm.c`, `harness.html`, `serve.py`, screenshots). Read that
first; this note only carries what a future session would otherwise re-derive.

**Verdict: Outcome 2.** Desktop usable (boot 14-20 s, window drag median 24 fps
Chrome / 13 fps Firefox, 23 MB transfer), Duum unplayable (0.13 fps after a
4-6 minute first frame). Build Phase 1; Duum goes to Phase 2-alt.
Wasm-TCG is about **5x slower than native TCG** on the same hardware.

**The upstream question is settled and will need re-checking, not re-deriving:**
as of QEMU **v11.1.0** the TCG wasm backend has NOT landed. `meson.build` says
`error('WebAssembly host requires --enable-tcg-interpreter')`, i.e. upstream
wasm = TCI = slow. Use **`ktock/qemu-wasm` master** (`tcg/wasm32`, QEMU 8.2.0
base). Verify you got the JIT: `config-host.h` must have
`#undef CONFIG_TCG_INTERPRETER`.

**qemu-wasm has no graphical display at all** - every example is a serial
console on xterm-pty. `spike/ui-wasm.c` is the canvas + input backend written
for this. Its three non-obvious pieces: the blit must be **async**
(`MAIN_THREAD_ASYNC_EM_ASM`) because a sync proxied call stops the *guest* dead
when the tab is hidden; input arrives via a lock-free ring in shared memory;
and the frame counter must be incremented **on the QEMU thread**, because a
JS-side counter only ticks when the browser lets the main thread run.

**Build traps** (all cost time): the Dockerfile's zlib URL 404s (use
`zlib.net/fossils/...tar.gz`); it needs BuildKit, so `apt install docker-buildx`
on quill; mount the source **rw** not `:ro` (meson fetches the `dtc` subproject
into the tree); and build `-g0` not `-g` - DWARF makes the wasm 41 MB instead
of 11.7 MB. Guard the new file with `cc.get_id() == 'emscripten'`:
`targetos` is **`bogus`** in this fork, so the obvious guard compiles nothing.

**Measurement traps.** Firefox reports `visibilityState: hidden` for an
occluded window on an RDP desktop and throttles the tab to ~0 fps - set
`widget.windows.window_occlusion_tracking.enabled=false` (Chrome needs
`--disable-backgrounding-occluded-windows --disable-renderer-backgrounding
--disable-background-timer-throttling`). A one-way window drag runs into the
snap zone and reads low; oscillate inside the screen instead.

**Two OS facts worth keeping.** The shipped image renders **512x384
pixel-doubled** into a 1024x768 VBE mode, which is why the demo film's 1.3 fps
anchor did not materialise. And the pc64 idle loop spins (`rdtsc`/`pause`) but
that costs **host CPU, not frame rate** - 175% of a core to show a static
desktop. HLT-when-idle is NOT a small change: pc64 installs no IDT for IRQs
outside the debug crash handler, so it needs the whole deferred timer-IRQ item.
See [[unodos-pc64-review-2026-08]].

The v3.33.0 release image ships **Freedoom Phase 1** (BSD-3-Clause) as
`DOOM1.WAD`; the local dev tree has id's shareware WAD instead. Duum has no
desktop icon and no Start-menu row (it is a PYAPP with no registry descriptor):
launch it from Files, and with the **Open** button, since Enter opens
directories only. Related: [[unodos-launch-strategy]], [[unodos-duum-complete]],
[[unodos-pc64-build-on-quill]].

## PUBLISHED 2026-08-18: `unodos.arinbakht.ca`

Live on the homelab portal behind Cloudflare Access (app `3c396eee`, `arin
only`, 730 h), nginx on **devbuntu loopback :8792** through the existing
`homelab` tunnel - no new tunnel needed. Runbook, vhost and page source:
`hmofet/homelab` `unodos/`. See [[homelab-portal]].

**Two things decide whether it works at all.** COOP/COEP on the vhost, or the
Boot button refuses (the page says why, and its Diagnostics panel prints
`isolated true/false`). And `gzip_static on; gzip off;` with `foo.data.gz`
beside `foo.data` - gzipping a 96 MiB image per request costs more CPU than
the emulator does.

**Access blocks curl, so verify the origin by forwarding it instead**:
`ssh -f -N -L 8792:127.0.0.1:8792 devbuntu`, then open
`http://localhost:8792/try.html` - localhost is a secure context so
SharedArrayBuffer works. That tests the real files and the real vhost; only
Cloudflare's header pass-through needs a signed-in browser.

**Tuned config, after setting Duum aside**: `TOTAL_MEMORY=640MB`, guest
`-m 192M`, `tb-size=96`. Transfer **12.2 MB**, desktop in ~15 s, Chrome peak
1288 MB. 640 MB with the old 256/128 split aborts `Aborted(OOM)`. Deleting the
WAD needs a **zero pass over the freed clusters** (mcopy a big zero file then
mdel) - a FAT delete only unlinks, so the bytes stay in the download otherwise.

## Measured on `mini` (bare metal Ryzen 7 5700X3D), 2026-08-18

The spike's hardware caveat is closed. Same bundle, same config: **desktop in
6.1 s (Chrome) / 5.6 s (Firefox)**, window drag **median 41.6 / 39.4 fps**, vs
14-20 s and 24 / 13 fps on amanuensis. Roughly **2.4x on boot, 1.7x on fps** -
most of the original slowness was the hypervisor, not the emulator.

**Firefox has to run `-headless` on mini.** A GUI Firefox launched over SSH
lands in **session 0** and never opens a window, so the run hangs with no
firefox.exe at all; Chrome tolerates session 0 and composites fine. Also: run
the whole measurement inside ONE ssh invocation, because a `Start-Process`
server started by an earlier invocation dies with it.

## PUBLIC 2026-08-18: `unodos.arinbakht.com/try/` (Phase 1)

Live on the real site, `hmofet/unodos-site`: `src/try/index.html`,
`src/_headers` scoping COOP/COEP to **`/try/*` only**, `functions/api/hit.js`
counting boots once per session on a **closed key set** (an endpoint that
writes any key it is handed lets anyone fill the shared `hits` table). Front
page gets one ghost button and fetches nothing. Measured live: **15.2 s to
desktop, 12.07 MB over the wire**.

**Cloudflare Pages rejects any single asset over 25 MiB.** The disk image is
96 MiB, so it ships as `load-disk.data.gz` and is expanded in the browser with
`DecompressionStream` (check the gzip magic first: an edge that decoded the
Content-Encoding itself would otherwise hand you plain bytes and a crash).
`build.py` fails on that limit and on the 40 MB per-boot budget.

**The guest surface is ALWAYS 1024x768** and no page-side scaling changes that:
the pc64 BIOS loader sets one VBE mode before long mode (`PREF_W`/`PREF_H` in
`boot/bios_stage2.asm`) and cannot renegotiate it. Raising Resolution in the
Control Panel changes how many logical pixels the OS draws, then fill-scales
them into the same surface. So the screen must be sized to a WHOLE MULTIPLE:
stretching 1024 across ~880 px with `image-rendering: pixelated` drops every
seventh row and column and eats one-pixel borders. Non-integer scales must
render smooth instead.

**Watch out when testing:** reusing a Chrome profile across runs can leave the
guest in a state an earlier script drove it to, which reads exactly like a bad
shipped image. Confirm with a fresh `--user-data-dir` before believing it.

## 2026-08-19: 1280x1024, and the pointer rework

**The surface is whatever VBE mode the pc64 loader sets, full stop.** Raised via
`UNO_BIOS_PREF=WxH` (branch `browser-display`, PUSHED UNMERGED) which overrides
`PREF_W/PREF_H` at assembly time. Default stays 1024x768 on purpose: a mode a
real panel cannot sync is a black screen with no way back. Cost on mini,
1024x768 vs 1280x1024: **38 vs 36.5 fps**, ~4% for 1.67x the pixels, because the
drag path repaints the moved window not the screen. **amanuensis cannot measure
this** - two runs there gave 24.0 and 17.7 for the same build.

**THE SURFACE WAS ONLY HALF THE RESOLUTION QUESTION, and the other half was
worth 4x.** pc64 starts its desktop at `gModeW/2 x gModeH/2` and fill-scales it
up (`set_geometry` in `uefi_main.c`), so the 1280x1024 surface shipped a
**640x512** desktop doubled - **0 of 327,680 2x2 blocks non-uniform** in the
frame, i.e. three quarters of the pixels the page fetched, expanded, blitted
and scaled carried nothing. The Control Panel had been reporting "640x512" on a
1280x1024 screen the whole time. **`UNO_DESKTOP=native` (2026-08-19) starts 1:1**
and /try/ is built with it; the default stays half because a 13" panel at 1:1 is
a UI you lean into and a panel cannot be zoomed by its viewer, while a browser
tab can. Cost, 3 runs each, boot to a painted taskbar, same host and TCG:
**3.22 s half vs 3.75 s native**, +/- 0.01 - ~16% for 4x the pixels, because
present's scaled-output half writes the same 1280x1024 either way.

**Both knobs or neither**: a rebuild with only `UNO_BIOS_PREF` boots, looks
right at a glance, and is quietly back to a quarter of the resolution.

**A 2x2-block uniformity count is the cheap test for this class of bug** - an
upscale that carries no detail is invisible to the eye at a glance and obvious
in one loop over the pixels.

**THE WIDGET HAS GUEST NETWORKING AS OF 2026-08-19, AND IT CANNOT REACH A
NETWORK.** `-nic none` is gone: the guest has an e1000 on hub 0, takes a DHCP
lease, resolves names and loads pages. The far end of the wire is the tab.
**A page cannot open a TCP or UDP socket** - not "cannot easily", the capability
does not exist - so `-netdev user` has no host sockets to NAT onto and
`-netdev socket` is mapped by emscripten to a WebSocket needing a server. Both
would relay a visitor's traffic through somebody's infrastructure.

- **`net/wasm.c`** (launch repo `spike/net-wasm.c` + `qemu-wasm-net-hooks.patch`)
  is the network sibling of `ui/wasm.c`: raw Ethernet frames over two
  shared-memory rings, joining hub 0 at machine-init-done so no netdev type had
  to be invented. **`romfile=` is mandatory** - the e1000's PXE ROM is not in
  the packaged ROM set and QEMU treats a missing one as FATAL.
- **`vlan.js`, inlined in `src/try/index.html`**: ARP, IPv4, ICMP, UDP, DHCP,
  DNS, and enough TCP to serve one page. Every name resolves to it.
- **The absence of an egress path IS the security property.** Verified end to
  end: the guest's own browser resolved a name, opened a connection and rendered
  the page (`arp 2, dhcp 2, dns 1, tcp 1, http 1`).

**THE POINTER DRIFTED, AND RAISING THE DESKTOP EXPOSED IT (fixed 2026-08-19).**
Symptom: the guest pointer parks in the top-left and only tracks if you enter
the canvas from exactly there. Cause: **a PS/2 packet carries at most +-127 per
axis**, so a traversal costs `width/120` packets - six at 640x512, fourteen at
1600x900 - and the page advanced its BELIEVED position per packet SENT, not per
packet the guest drained. A 16 ms timer spending a 3-packet credit allowance let
three go out between two frames; every dropped one became permanent error, so
the belief drifted monotonically and the cursor lagged further the further it
travelled. The old comment already prescribed the fix ("one packet per presented
frame") - the code just never did it. Now: **step inside the blit callback**,
timer demoted to an idle deadlock-breaker, homing over-travel **doubled rather
than a flat +4** (the flat margin had decayed from 1.9x to 1.35x), and a
**re-home on `mouseenter`** carrying a `pending` target so the walk resumes at
the entry point. STEP cannot be raised past 127 - that is the protocol.

**THE SECOND POINTER BUG, and it was the same root cause: `measureGeom()`
reads the geometry out of the PICTURE**, scanning in from each edge for a lit
pixel. That works when the desktop is scaled up and centred inside a black
letterbox. **At 1:1 there is no letterbox to find**, so anything dark touching a
screen edge - a maximised dark window, Duum, a dark web page - shrinks the rect,
reads as a mode change, and re-homes: the cursor slammed into the corner every
1.5 s, under the user's hand. Committing a bogus geometry is the WORSE half: it
rescales every coordinate the page sends. Fixed by (1) **a re-home may never
happen while the pointer is over the canvas** - flag it and settle it on
`mouseleave`, walking back to where the pointer left - and (2) **a geometry
reading must be seen TWICE before it is believed**. Note the first attempt at
this re-homed on `mouseenter`, which was worse than the bug: the cursor bolted
away exactly as someone reached for something.

**To verify a pointer fix, find the cursor in the framebuffer**: the guest draws
its own cursor, so a small dark-pixel box at the target reads ~38 when it
arrives and 0 where it left. Two full traversals landing exactly on the belief
is the proof; no screenshot needed.

**Two traps that cost hours when driving the guest from the page:**
1. **The i8042 queue holds a handful of bytes and SILENTLY DROPS the rest.** A
   burst of 138 key events was consumed by the ring (head==tail) and landed as
   *nothing*. Pace keys on presented frames, exactly like the pointer credits.
2. **Mean colour cannot see a window over a desktop** - a whole browser window
   moved the regional mean by ~2/255 and read as "bare desktop". **Count dark
   pixels** in a region instead; that jumped 95 -> 5913 when the page rendered.

Also: a hidden tab clamps `setTimeout` to >=1 s AND applies a timer budget, so
setTimeout-chained automation crawls. The Browser pane cannot be displayed here,
so `computer` screenshots fail outright - drive and screendump under **native
QEMU on quill** whenever the thing being tested is guest-side.

**Duum + Freedoom ship in the /try/ image as of 2026-08-19** (current engine,
122 KB; `DOOM1.WAD` 28,795,076 bytes at the volume root; `APPS/DUUM.UNO`).
Payload went 9.1 MB -> 19.4 MB, ~23 MB per boot, still inside both the 25 MiB
per-asset cap and the 40 MB budget. **It is worth seeing, not playing**: 0.13 fps
with minutes to the first frame. Launch it from Files, and note the volume
selector starts on **RAM** - you must switch to UNODOS. `pane_enter()` now
launches a `.UNO` on **Enter** (the older "Enter opens directories only" note is
out of date).

**nasm `-dPREF_W=1280` defines a MACRO**, so `PREF_W equ PREF_W` expands to
`1280 equ 1280` and will not assemble. Use `%define`, not `equ`.

**Pointer lock had to go**: the browser takes Esc before the page sees it, so a
locked pointer can NEVER give Esc to the guest, and Esc is how pc64 closes
menus. Replaced with absolute tracking of a relative device:
- guest framebuffer geometry **measured from the picture** (content rect from
  the letterbox border; integer zoom from the fact that under a whole-number
  upscale every adjacent-pixel difference falls on one phase)
- home into a corner once for a known origin
- **pace packets by CREDITS, not by frames.** Frame-pacing deadlocks: an idle
  guest draws nothing -> nothing sent -> pointer never moves -> nothing to draw.

**pc64 bug found:** `apply_desktop` CLAMPED the pointer into the new fb instead
of scaling it, so a resolution change teleported it to an edge where a 2x
fill-scale left one column visible. Fixed on the same branch.

**devbuntu's sudo now needs a password** (it did not earlier the same day), so
the homelab vhost can be read but not redeployed.

## THE PATTERN IS WRITTEN DOWN: `unodos-site/BROWSER-WIDGET.md`

Authoritative doc for building a second widget (Duum is next, another agent).
Lives next to the code so it cannot drift. Covers what exists and where, what
one boot does, the seven constraints that fail as something else, the QEMU
build (fork + `ui-wasm.c` + hooks patch, all archived in
`unodos-launch/spike/`), the site wiring and the verification list.

**Its last section is the one that matters for Duum:** page shell, delivery and
build wiring all transfer; the PAYLOAD does not. Duum measured **0.13 fps**
inside the emulated OS, first frame 4-6 min, so **Phase 2-alt** (compile Duum's
inner loop to wasm, shim the `uno` API) was the live plan.
`EMULATOR-IMPLEMENTATION.md` Phase 2 now points here.

**Phase 2-alt is DONE (2026-08-18) and BROWSER-WIDGET.md carries the outcome**
in a new "What Duum actually did" section. See [[duum-web-port]]. It was far
cheaper than the doc expected, and four of the seven traps turn out NOT to
apply to a payload that is an app rather than a machine - including COOP/COEP,
which a single-threaded build does not need at all.

## THE THIRD POINTER BUG (2026-08-20): re-homing on every canvas exit

Symptom reported: "the pointer is locked to the top left of the canvas and
cannot be moved away". Cause: `mouseleave` re-homed **unconditionally**, and a
home was 25 blind packets paced by the 100 ms deadlock timer = **2.96 s** on an
idle guest, with `homed` false and every mouse move discarded into `pending`.
Crossing the canvas edge is something a hand does constantly, so each exit
restarted a slam that outlasted the gap before the next one. Reproduced exactly:
five leave-and-return cycles 1.5 s apart, mouse moving throughout, cursor at
(0,0) for all five.

Fixed on `unodos-site` master (`96a57c3`), and BROWSER-WIDGET.md now carries the
whole rule set:

- **A departure must LAST to count**: 400 ms dwell off the canvas before the
  re-home is armed, cancelled if the pointer returns.
- **Homing is not paced like walking.** A dropped HOME packet costs nothing (the
  destination is a hard clamp, the 2x over-travel absorbs losses); a dropped WALK
  packet is permanent error. So homing gets its own 16 ms timer: **0.45 s vs
  2.96 s** for the same 28 packets. The walk keeps one-per-presented-frame.
- **Believe the FIRST geometry reading.** The confirm-twice rule (added for mode
  changes) was also discarding the measurement taken the instant the desktop
  appears, so the 1280x1024 default sized the first home and clamped every
  coordinate on a 1600x900 screen until a re-measure 1.5 s later fired a SECOND
  home.

**Measured facts worth not re-deriving:**

- The page's own event ring is NOT where packets die: cap 1024, max depth 1 under
  frantic sweeping, zero would-be overflows. Losses are downstream in QEMU's PS/2
  queue and are invisible from the page - one dropped walk packet showed up as a
  flat **61,60** offset that survived every later traversal. Only a re-home fixes
  it, which is why some exit-triggered resync has to stay.
- The guest absorbs bursts far faster than one packet per presented frame: 28
  packets at 8 ms with **zero** frames presented still reached the corner.

**Harness (reusable):** the CC Browser pane reports `innerWidth/innerHeight` 0 on
this box, so it cannot drive or verify the widget. Use **Edge over CDP** instead:
`msedge.exe --remote-debugging-port=9333 --remote-allow-origins=*
--user-data-dir=<fresh>`, python `websocket-client` with `suppress_origin=True`,
`Input.dispatchMouseEvent` for the mouse, `Runtime.evaluate` for state. Serve
`dist/` with COOP/COEP from `http.server.ThreadingHTTPServer` (localhost is a
secure context, so SharedArrayBuffer works). **Verify by finding the cursor in
the framebuffer**, never by assuming: reference frame + flood-fill clusters of
the changed pixels; the pc64 arrow is a **9x15, ~71-pixel** cluster, and a
cluster of 150-300 px is a repaint, not the cursor. Two traps in that harness:
the canvas is taller than the viewport at `auto` size, so half the test points
land off-window unless you `scrollIntoView` first; and the top-right clock
repaints on its own (mask it).

**THE HARNESS TRAP THAT SILENTLY PASSES A TEST: send `Page.bringToFront` first.**
Without it, a background Edge window never fires `mouseenter`/`mouseleave` for
CDP-synthesised moves - `inside` still flips (the page computes it from
geometry), so a leave-and-return test runs to completion and reports PASS while
never having left the canvas at all. With the tab fronted, the same script
fires `leave` in all three directions and the dwell timer arms. Also: Edge
launched from a Bash background task gets reaped mid-run (the run dies with
`Connection to remote host was lost`); launch it detached with PowerShell
`Start-Process -PassThru` and stop it later by PID - never `taskkill /IM
msedge.exe`, which takes the user's own browser with it.

**Repeat boots OOM.** Booting a second time in a warm tab (or right after a
reload) aborts with `Aborted(OOM)` from the wasm heap perhaps half the time,
with 20 GB free on the host. First boot in a fresh browser is reliable. Not
investigated; retry is the workaround.

**THE BROWSER DEMO WAS SILENT** (SOLVED 2026-08-20, see [[unodos-browser-audio]]: a WebAudio backend was written for qemu-wasm. The note below is why no flag alone could have done it) (found 2026-08-20,
chasing "the games have no music"). `unodos-site/dist/try/index.html`'s
`Module.arguments` has no `-audiodev` and no audio device - and the packaged
`qemu-system-x86_64.wasm` was built with **zero audio backends compiled in**:
searching it for the driver names QEMU registers (`none`, `wav`, `sdl`, `alsa`,
`pa`, `dsound`, `pipewire`, ...) finds none of them, only the generic
`-audiodev` plumbing and "no default audio driver available". So adding
`-device intel-hda -audiodev ...` to the command line cannot work; giving the
demo sound meant rebuilding qemu-wasm with a WebAudio backend, which is what [[unodos-browser-audio]] did. Everything on
the guest side is fine - the same images play in desktop QEMU with
`-device intel-hda`, verified by capturing the DAC to a wav
(`pc64/tools/game_audio_test.py`, and see [[unodos-pc64-game-audio]]).


## Index detail (moved from MEMORY.md 2026-09-08)

/try/; **CC browser pane is 0x0, use Edge over CDP**. [Audio](unodos-browser-audio.md) not deployed
