# Foreign packages in UnoDOS: Android first, Firefox first

> **Status: 2026-08-23. P1 part-done, P2 DONE and proven, P3 onward is plan.**
> Branch `unopkg`. It sits on Track A (`unovirt`), the appliance payload
> (`pc64/guest/appliance/`), the app registry (`docs/APP-REGISTRY-PLAN.md`,
> `uno_appdesc.h`) and Track B (`unoguest`). §7 says how this lane and the
> GIMP lane stay out of each other's way.
>
> **What works today.** Double-click a real Firefox 154.0 x86_64 APK in Files,
> press again, and "Firefox" is on the desktop beside every built-in app; open
> it and a native UnoDOS window opens carrying its name. It survives a reboot.
> The runtime behind it is a stub that reports, accurately, why nothing is
> running yet. Contract and evidence: `pc64/UNOPKG.md`.
>
> **Appliance-side, P1:** the guest kernel now carries binder, binderfs, the
> namespace and cgroup set, veth/bridge/NAT and virtio-snd; built and verified
> on quill. The Waydroid image build (`build_android.sh`) is the remaining
> half of P1.

## 0. What is being asked, and the verdict

**Goal.** A user double-clicks an `.APK` (later a `.deb`, an `.rpm`, a
`.msi`) in Files, UnoDOS installs it, and from then on it is an application
like any other: a desktop icon, a Start-menu row, its own native window, a
taskbar entry, Alt-Tab, snap, close. **The guest is never shown.** No
launcher, no Android status bar, no "Appliances" window with a foreign desktop
in it. First package: **Firefox for Android** (Fenix, x86_64, MPL-2.0, no
Google Play Services).

**Verdict: feasible, in two halves that are separable and should be built in
that order.**

1. **The package half** is new and small: a *foreign package installer* in
   the shell and a *shim `.UNO`* per installed app. The app registry already
   promises that dropping a `.UNO` into `APPS\` is the whole install, and its
   descriptor format ignores keys it does not know, which is exactly the
   extension point a shim needs (`kind: foreign`, `host: android`, `launch:
   org.mozilla.firefox`). Nothing in `pc64_uui.c` changes.
2. **The window half** is Track B's L1, already planned as B0/B1 in
   `UNOVIRT-PLAN.md` §4 and not yet built. Until it exists, the only way to
   see a foreign app is L0, the whole guest framebuffer in one window, which
   is precisely the look this programme rejects. **B0 and B1 are therefore on
   the critical path**, not a later refinement; this plan consumes them and
   adds the Android-specific agent behaviour they need.

**The Android runtime itself** is Waydroid inside the existing Alpine
appliance, not a second guest. It reuses every piece M3..M5 paid for (kernel,
cage on simpledrm, i8042 input, bridged net, read-only virtio-blk), draws with
SwiftShader, and, decisively for L1, **already exposes each Android activity
as its own Wayland surface** in multi-window mode, so the B1 agent sees
Firefox the same way it will see GIMP: as a toplevel with a title and damage.

| Vehicle | Display on simpledrm, no GPU | virtio-mmio only, no PCI | Per-app surfaces for L1 | Verdict |
|---|---|---|---|---|
| **Waydroid in the Alpine appliance** | cage works; SwiftShader into a Wayland surface | nothing new | yes, multi-window mode | **do this** |
| Android-x86 / Bliss as the guest | its hwcomposer on a bare framebuffer is the fight M4 lost | kernel and init expect PCI, ACPI, a writable disk | no: one framebuffer, one desktop | no |
| Cuttlefish / ranchu | virtio-gpu or goldfish over PCI | crosvm-shaped, 2 vCPU, 2 GB | no | no |

**What this does not buy.** GPU acceleration, hardware video decode, ARM-only
apps (no `libhoudini`/`libndk`: proprietary, out of policy), or pixel-identical
fonts between Android and pc64's TTF engine.

---

## 1. What is already there, and what each piece forces

Read from the tree on 2026-08-23:

| Fact | Where | What it forces |
|---|---|---|
| One vCPU, `nolapic`, jiffies clocksource | `hv_phases.c` `L_CMDLINE_TEXT` | Android boots single-core; SwiftShader and the app share the core |
| Guest gets 4 ms of each 16 ms frame, 10 ms when its display is focused | `unovirt.c` `SLICE_BUDGET_US`, `SLICE_FOCUS_US`, `uno_vmm_focus_display` | ~60% of a core. **With L1 there is no "display window" to focus**; the focus rule becomes "any foreign window is focused", a one-line policy change in the same place |
| Carve: 768 / **1536** / 2048 MB by RAM tier | `uno_vmm_carve_mb` | Android + Firefox is ~1 GB on top of Alpine; **1536 MB is the floor**, the 768 tier refuses by name |
| Framebuffer 800x600, simpledrm | `L_FB_W/H` | Irrelevant to L1 (surfaces go through the channel, not the fb); still the fast loop's display and the L0 fallback |
| i8042 keyboard + relative mouse | `unovdev_pc.c` | L1 injects input per window through the channel (`INPUT_*` commands), which gives ABSOLUTE pointer positions for free and retires the drift problem for foreign windows |
| virtio-net bridged, guest owns its lease | `unovdev_net.c` | Waydroid NATs `waydroid0` behind the guest's interface; kernel needs netfilter/NAT/bridge/veth it lacks today |
| virtio-blk **read-only**, whole-file writes only below it | `UNOVDEV.md` | Android `/data` is tmpfs. **Installed state cannot live in the guest**; §3 puts it on the ESP instead |
| No audio device in the guest | `unovdev.c` | Firefox plays video silently until a virtio-snd model exists (§4, P6); not on Firefox's acceptance path |
| Kernel 6.12, everything `=y` | `unodos-guest.config` | binder/binderfs `=y`, no module dance; memfd replaces ashmem (Waydroid >= 1.2.1, kernel >= 5.18) |
| `.UNO` descriptor: `id/name/short/icon/cat/flags`, unknown keys ignored, icon by emblem name, `APPS\` scanned at boot | `uno_appdesc.h`, `MODULES.md` | The shim needs no registry change. A per-app icon (Firefox's, not a generic emblem) needs the descriptor to carry or point at QOI pixels: one additive key (§3) |
| `pc64_shell_run_user()` runs PYAPP and classic/unoui `.UNO` | `pc64_uui.c` | The shim is a real (tiny) unoui module, so it needs no third branch; its `uno_app_main` is what talks to the appliance |
| B0: channel + stub agent + L1 plumbing; B1: real wlroots agent | `UNOVIRT-PLAN.md` §7 | Unbuilt. This plan's P3/P4 are B0/B1 with Android as the first real client |
| Hypervisor loop ~25 min; fast loop (plain QEMU, same payload) ~2 min | `guest/appliance/README.md` | Everything guest-side is found in the fast loop; B0 is provable in QEMU/TCG with no hypervisor at all (§4.5 of the plan) |

## 2. What the user sees

1. Files shows `firefox-x86_64.apk`. Double-click. A native dialog: the
   package's label, icon, version, size, "Install". (The label and icon are
   read by the host from the APK's `resources.arsc` / `AndroidManifest.xml`,
   which is a zip plus a binary XML: a few hundred lines in the installer,
   no guest needed to show the dialog.)
2. Progress while the appliance starts (if it was not running) and installs.
3. A "Firefox" icon appears on the desktop and in Start, indistinguishable
   from UnoWord's.
4. Launch: a native UnoDOS window titled "Firefox" with Firefox inside it.
   Move, resize, snap, minimise, Alt-Tab, close: all the shell's own
   behaviours, on a `unoui_window` holding one `UI_CANVAS` fed by the channel.
5. Reboot: the icon is still there; launching it works. (Firefox's own
   profile, bookmarks and tabs are **not** preserved until P7; the plan says
   so in the install dialog until then.)

Nothing in steps 1..5 names Android, Linux or an appliance. The only place
the guest exists for the user is a Settings row: "Foreign apps: Android
runtime running, 1.1 GB" with Stop/Restart.

## 3. The package half: installer, shim, ESP as the source of truth

### 3.1 Where installed state lives

The guest cannot keep anything across boots, and UnoDOS storage writes whole
files. So **the ESP is the source of truth and the guest's `/data` is a cache
rebuilt at appliance boot**:

```
EFI\UNODOS\APPS\FIREFOX.UNO            the shim: descriptor + icon + ~2 KB of code
EFI\UNODOS\APPS\PKG\FIREFOX.APK        the package, as installed
EFI\UNODOS\APPS\PKG\FIREFOX.PKG        {host: android, pkg: org.mozilla.firefox,
                                        activity: .App, sha256, installed: date}
```

At appliance boot, `uno-init` asks the host (over the channel, §3.3) for the
package list and installs each APK that is not already present in `/data`
(`pm install` is idempotent; Firefox takes ~15 s under SwiftShader in the
fast loop, to be measured). A package is "installed" from the user's point of
view the moment the shim exists, whether or not the runtime is up.

This matches how everything else on the ESP works, costs no unofs change, and
makes uninstall "delete three files". The cost is a slow first launch after
each boot and no profile persistence, which P7 removes once unofs gains
write-at-offset and `/data` can sit on a writable virtio-blk.

### 3.2 The shim `.UNO`

One tiny unoui-class module per foreign app, generated by the installer from
a template that ships in the tree (`pc64/apps/foreign_shim.c`, built once;
the installer patches only the descriptor block and the icon). Its
descriptor:

```
id:      firefox
name:    Firefox
short:   Firefox
icon:    @          # "@" = pixels follow the block (additive, see below)
cat:     internet
kind:    foreign    # ignored by today's reader, read by the shim itself
host:    android
launch:  org.mozilla.firefox/.App
```

**A real icon needs NO change to the app registry**, contrary to what this
plan said when it was written. `app_icon_resolve` (`pc64_uui.c:379`) already
reads a QOI that ships beside a module: `icon: file:NAME.QOI` resolves against
the module's own directory and volume, into a 12-slot 32x32 custom slab, and
`mkuno.py` validates the form at build time. VMGR.UNO has used it since the
registry landed - the very case the first draft claimed was unsupported. The
request was filed from the descriptor's *prose*, which lists `icon:` as "a
named emblem", without reading the *resolver*; it is withdrawn in
`pc64/UNOAUTOMATE-REQUESTS.md`.

So the remaining work is entirely in this lane: decode the APK's icon (a PNG,
and `um_png` is already in the kernel) and write a 32x32 QOI beside the shim.
P2 ships `icon: generic`, which is the only part of an installed app that
still looks borrowed.

The shim's `uno_app_main`: ensure the runtime (start the appliance if
stopped), send `LAUNCH(pkg/activity)`, and then **own nothing**: the windows
that appear belong to the L1 plumbing, which maps `WIN_CREATE` events to
`unoui_window`s tagged with the shim's id so the taskbar groups them under
"Firefox" and closing the last one ends the app. Two foreign apps from the
same runtime are two shims and one appliance.

### 3.3 The installer and the channel

`pc64_pkg.c` (new, small; registered as a Files association for `.APK`):
parse the APK header for label/icon/version, show the dialog, copy the APK
to `APPS\PKG\`, write the `.PKG` and the shim, then if the runtime is up push
the install over the channel.

The channel is B0's (`UNOVIRT-PLAN.md` §4.1): a control page with two rings
in the carve. This plan adds to its command set, additively:

```
PKG_LIST   -> agent         (boot-time catch-up: which packages the ESP holds)
PKG_PUT    -> agent         (chunked bulk in a 4 MB slot in the carve; the APK)
PKG_INSTALL(host, path)     agent runs `waydroid app install` / `pm install`
PKG_REMOVE(host, pkg)
LAUNCH(host, target)        already in the plan; target = pkg/activity
PKG_RESULT <- agent         ok / error text
```

### 3.4 The same shape for `.deb` and `.rpm`

Nothing above is Android-specific except the `host:` value and the agent's
install command. A `.deb` is `host: linux`, installed into a writable
overlay of the Alpine appliance (`apk add` for Alpine packages, `dpkg -x`
for a `.deb` into `/opt` with its `.desktop` file as the launch target), and
its window comes through the same B1 agent. The installer grows one parser
per format (ar+tar for `.deb`, cpio for `.rpm`) for the dialog; the ESP
layout, shim and channel are identical. This is why the package half is
designed first and Android is its first client rather than its shape.

## 3.5 What the appliance's init must provide, measured

**Android boots on the UnoDOS appliance kernel** (2026-08-23, `android_probe.sh`
under plain QEMU): `Session: RUNNING`, `Container: RUNNING`, LXC PID 192,
`zygote64`, `zygote`, `surfaceflinger`, `servicemanager`, `hwservicemanager`
and `vndservicemanager` in the process list, and `waydroid app list` answering
out of Android's own package manager with Settings and the AOSP keyboard.

**Measured: the container alone is 820 MiB to 1000 MiB across runs** (about 32
MiB of it kernel memory) with nothing running in it but the framework. Alpine,
the compositor and a browser sit on top of that, which is what makes the 768 MB
carve tier impossible and 1536 MB tight rather than comfortable - and the
variance is itself the point: a figure that moves by 180 MiB between two
identical boots is not one to size a carve against by taking the smaller
reading. That number gets re-measured with Firefox actually running, and it is
what P4 reports back.

Getting there took **seven environment faults, and every one of them would
have presented through the appliance as a black window.** They are listed
because `apps/android.app` has to get all seven right and none of them is
discoverable from Waydroid's documentation:

| What was missing | How it presented |
|---|---|
| A writable **mount point** for the runtime image (`/usr/share/waydroid-extra/images`), created at BUILD time - the root is read-only | `mkdir` failed at boot, so the image had nowhere to mount, so Waydroid found no preinstalled images, so it tried to DOWNLOAD 1 GB, and reported a network error. Three layers of wrong answer from one missing directory |
| **`sse4_2` in `/proc/cpuinfo`** | `x86_64 CPU does not support SSE4.2, falling back to x86...` - an INFO line, after which it looks for x86 images the appliance does not carry. QEMU's default CPU model has no sse4_2; `-cpu host` fixes the probe, and UnoDOS passes CPUID through, but a guest that masked leaf 1 ECX would break this silently |
| **CA roots** | `CERTIFICATE_VERIFY_FAILED` - the same fault M3 hit and for the same reason: an Alpine rootfs ships no roots unless asked |
| **DNS**, and `/etc/resolv.conf` on a writable path | `waydroid init` CANNOT tolerate being offline: `helpers/http.py`'s `retrieve()` catches only `ValueError` and `HTTPError`, so the `URLError` a DNS failure raises escapes and kills init - even with the images already preinstalled and it one line from deciding it does not need them |
| A **system dbus** bus, with `/var/lib/dbus` and a machine-id | the container service never registers |
| A **session dbus** bus (a different bus) | `Unable to autolaunch a dbus-daemon without a $DISPLAY for X11` - an X11 error message, on a Wayland-only appliance, for a missing environment variable |
| A **writable `$HOME`** | `[Errno 30] Read-only file system: '//.local'` - a doubled slash that looks like a Waydroid bug and is an unset variable |

Two structural facts worth keeping beside them:

- **`waydroid init` needs binder**, so it cannot be run at build time on an
  ordinary Linux box - `probeBinderDriver` runs before anything else and fails
  on a kernel without it. The config is therefore generated on the guest, at
  first boot, which is why the DNS row above matters. Pre-seeding
  `waydroid.cfg` into ANDROID.IMG would remove the boot-time network
  dependency entirely and is the obvious next hardening step.
- **The container starts on a SESSION, not on `container start`.**
  `waydroid container start` runs a service; the Android container itself
  stays `STOPPED` until a session attaches to a Wayland display. A probe with
  no compositor therefore looks like a total failure while everything under it
  is working, which is exactly what happened.

## 4. Phases

Each is one worktree off `origin/master`, lands small, and has an exit
criterion a screenshot or a log line can show. P1..P3 need no hypervisor.

| # | Phase | Exit criterion | Sessions |
|---|---|---|---|
| **P1** (half done) | **Android runs in the fast loop.** Kernel config gains binder/binderfs, namespaces, cgroup2, veth/bridge, netfilter + NAT, `CONFIG_SND_VIRTIO`. `build_android.sh` produces ANDROID.IMG (LineageOS 20 x86_64 vanilla, `waydroid_base.prop` with `ro.hardware.gralloc=default`, `ro.hardware.egl=swiftshader`, `persist.waydroid.multi_windows=true`). `uno-init` mounts it from `/dev/vdb` when present and starts the container headless (no launcher shown: `waydroid show-full-ui` is NOT run). Firefox x86_64 APK installed from the ttyS0 shell; `waydroid app launch org.mozilla.firefox` puts a Firefox toplevel on cage. | `shots/android_fastloop_firefox.png`: Firefox, and only Firefox, on the appliance's framebuffer; `waydroid status` RUNNING; peak memory from `/proc/meminfo` recorded | 2 |
| ~~**P2**~~ | **DONE 2026-08-23.** `pc64_pkg.c` (a local zip reader, a binary-XML manifest reader, install/remove, the runtime probe), `apps/foreign_shim.c` -> `PKG\FSHIM.UNO`, the `.APK` branch in Files behind an arm-twice confirm, two `kExports` rows, and two gates. **Nothing in `pc64_uui.c` changed** - an installed foreign app reaches the desktop through the ordinary app registry, which is the first outside proof of what `docs/APP-REGISTRY-PLAN.md` promised. | **MET.** Firefox 154.0 x86_64 installs from Files, appears on the desktop as "Firefox", opens a native window, and is still there after a reboot (`shots/pkg_*.png`). Two findings paid for: **a `.UNO` is CRC-sealed**, so an in-place rewrite must re-seal or the loader refuses an app that looks perfect everywhere else; and the arm-twice flag must be cleared by MOVING, never by the key that arms it. | 2 |
| **P3** | **B0: the channel and L1 plumbing, with the stub agent.** As `UNOVIRT-PLAN.md` B0, plus the `PKG_*` commands. Three stub foreign windows are real `unoui_window`s; the shim's `LAUNCH` makes the stub emit a `WIN_CREATE` tagged to the shim. | In QEMU/TCG: launching the Firefox shim opens a native window titled "Firefox" (stub content); snap, Alt-Tab, taskbar grouping and close all work; screenshots in the gate. **Either this is the B0 slice of the unoguest lane, or it is claimed here in that lane's name; it is not built twice** | 2..3 |
| **P4** | **B1 for Android: the real agent.** A headless wlroots compositor in the appliance (cage is replaced, or run with a custom backend: decide in P4a by trying wlroots' headless backend + a damage exporter first) that exports every toplevel's damage into a surface slot and forwards injected input. Waydroid's per-activity surfaces arrive here with their titles. `uno-init` runs the boot-time `PKG_LIST` catch-up. | On devbuntu under unovirt: double-click Firefox's APK, install, launch, and **Firefox is a native UnoDOS window**; type a URL through it and a page renders (`shots/firefox_native_window.png`). Boot-to-usable time and memory recorded | 4..6 |
| **P5** | **Focus policy and harness.** `uno_vmm_focus_display` becomes "any foreign window has focus"; `tools/vm_pkg_urc.py` scene: install, launch, navigate, screenshot, uninstall. Conformance row. | Scene passes three times in a row; P4's screenshot reproduces from cold | 1..2 |
| P6 | **Sound.** `unovdev_snd.c` (virtio-snd, one S16LE 48 kHz stereo playback stream into the host `snd_*` seam); Waydroid's audio HAL talks to the appliance's ALSA. Verify `-device virtio-sound-device` in the fast loop BEFORE writing the model (virtio-input's transport corner, RA7). | A YouTube page in Firefox plays with sound; host-side mixer level captured | 2..3 |
| P7 | **Persistence.** `/data` on a writable virtio-blk when unofs gains write-at-offset; the boot-time reinstall becomes a consistency check. | Firefox's tabs survive a reboot | depends on unofs |
| P8 | **Second format.** `.deb` via the Linux appliance through the identical shim/channel path; GIMP is the natural first `.deb` if that lane's appliance is in by then. | a `.deb` double-clicked in Files becomes a native window | 2..3 |

To Firefox in a native window: P1..P4, **10..13 sessions**, of which 6..9 are
Track B work that every other foreign app (GIMP included) needs anyway. The
Android-specific share is about 4.

### What A9 (SMP) changes

A whole core instead of a 60% slice: SwiftShader keeps up with a scrolling
page, video goes from 360p to 720p. Not a prerequisite for P4; P4's numbers
are the measurement that justifies scheduling it.

## 5. Firefox specifically

- **Package:** Fenix release APK, `x86_64` split, from Mozilla's release
  archive (MPL-2.0, redistributable, ~100 MB). No Play Services, no
  certification, no account. The build script and the installer both refuse an
  APK whose `lib/` has no `x86_64/` rather than installing one that dies at its
  first JNI call.
- **Why it is the right first app:** it exercises networking (TLS, DNS through
  Waydroid's NAT), a real rendering workload (Gecko + SwiftShader), keyboard
  and pointer input with a URL bar to type into, and multiple windows
  (a second tab opened in a new window is a second `WIN_CREATE`), and it fails
  for exactly one reason at a time. It has no dependency on sound, GMS, or
  writable storage.
- **Density:** `ro.sf.lcd_density=120` so Firefox's phone layout fits a
  desktop-sized window; Firefox honours Android's freeform-window resizing,
  which is what L1 sends as `SET_RECT`.
- **Later, YouTube:** the official app needs Google Play Services and a sounded
  guest; after P6 it is a GApps-image build (`--gapps`, user-supplied,
  proprietary, never default) and the signed-out playback test from the
  first draft of this plan. YouTube in Firefox is the P6 acceptance test and
  needs none of that.

## 6. Licensing and what enters the tree

Same structure as the browser appliance: **the tree carries build scripts,
configs, the installer, the shim template, the agent and the channel;
everything they build or install is a file on the ESP.**

| Component | Licence | Where |
|---|---|---|
| Waydroid | GPL-3.0 | ROOTFS.IMG, Alpine package; never in the tree |
| LineageOS x86_64 images, SwiftShader | Apache-2.0 (+ GPL kernel bits) | ANDROID.IMG, fetched by `build_android.sh` |
| Firefox for Android | MPL-2.0 | user's file, copied to `APPS\PKG\` by the installer; a pinned URL in the harness for the gate, not a vendored binary |
| GApps, YouTube APK | proprietary | user-supplied, `--gapps` / user's file only, never default |
| `libhoudini` / `libndk` | proprietary | not used |
| `pc64_pkg.c`, `foreign_shim.c`, `unoguest*`, the guest agent, `unovdev_snd.c`, `build_android.sh`, kernel config lines, harness | ours | the tree |

## 7. Ownership, claims, seams

- **New subsystem row** in `/AGENTS.md`, same commit as P2's first file:
  `unopkg` (foreign package installer + shim template + `APPS\PKG\` layout),
  contract `pc64/UNOPKG.md`, files `pc64_pkg.*`, `pc64/apps/foreign_shim.c`.
- **unoguest** (`unoguest*`, `guest/`): P3/P4 ARE this lane's B0/B1. Claim it
  explicitly before P3, or hand P3/P4 to whoever holds it and build P1/P2
  against the stub meanwhile: P2 needs nothing from the channel.
- **Appliance payload** (shared with the GIMP lane): **that lane has since
  generalised it, and it removes this plan's only real collision.** An
  appliance is now one file, `apps/<name>.app` - packages, compositor, client
  and self-test - with `rootfs_inner.sh` holding everything that does not care
  which. So Android becomes `apps/android.app` plus `build_android.sh`, and
  `rootfs_inner.sh` is not touched at all. `unodos-guest.config` and
  `build_kernel.sh` already carry this lane's appended `# android` block
  (landed). Rebase daily; both lanes append.
- **app registry**: **nothing.** The `icon: @` request is withdrawn -
  `icon: file:NAME.QOI` already does it (see §3.2).
- **unovdev**: the third blk slot (P1/P4, probably a table entry) and P6's
  `unovdev_snd.c`, as requests if not owned here.
- **unovirt**: the focus-policy line (P5), `uno.appliance=` token.
- **unofs**: the write-at-offset request already on file is P7's gate.
- **unoautomate**: `vm_stage.py` path for ANDROID.IMG; `vm_pkg_urc.py`; the
  `pkg` URC verbs and their fail-closed `GATE[]` rows in the same commit.

## 8. Risks

| # | Risk | Likelihood | Fallback |
|---|---|---|---|
| RA1 | 1536 MB too small for Alpine + Android + Firefox | medium | zram (on); Chromium not started on the Android path; require the 2048 tier by name |
| RA2 | B1's damage exporter on Waydroid surfaces is harder than on ordinary Wayland clients (Waydroid's own compositor shim sits between) | medium | Waydroid renders each activity as a plain `xdg_toplevel`; if not, fall back to exporting Waydroid's single surface and let it run full-screen inside one native window (L0 for the app, still no launcher visible) |
| RA3 | SwiftShader on a 60% slice makes Firefox sluggish | medium | density 120, small window; A9 |
| RA4 | Boot-time reinstall is slow enough to make "launch after reboot" feel broken | medium | show progress in the shim's window frame; P7 |
| RA5 | Waydroid's NAT kernel features change the browser appliance's behaviour | low | netfilter is inert without rules |
| RA6 | A third blk slot needs more than a table entry | low | single rootfs with an `--android` build profile |
| RA7 | virtio-snd over virtio-mmio hits a transport corner | low/medium | verify in the fast loop first |
| RA8 | APK parse (binary XML + resources.arsc) grows past "a few hundred lines" | low | label from `AndroidManifest.xml` only, icon from the guest after install via `PKG_RESULT` carrying the icon pixels |

## 9. Not doing

- **Showing the Android launcher, ever**, including as a stopgap: the L0
  Display view stays a debug tool behind the Appliances app, not a user path.
- **An in-guest app store (F-Droid) as the install route**: installation is
  the OS's job; the guest is a runtime.
- **Android-x86 / Bliss**, **ARM translation**, **virtio-gpu**: as before.
- **YouTube as the first app**: it is P6's test, after sound and GApps exist.
