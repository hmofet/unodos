# unovirt: foreign applications in UnoDOS, and UnoUI around them

> **Status: [plan].** Nothing below is built. This is the architecture and the
> implementation programme for running Linux and Windows software inside pc64,
> written 2026-08-06. It is the x86-64 counterpart of Glide's `docs/HYPERVISOR.md`
> (EL2, stage-2, virtio, V2..V3c.8, confirmed on a real A64 2026-08-05) and it
> borrows that programme's shape deliberately: a foothold you can prove with one
> round trip, then translation, then legible exits, then supply. Contract
> references: `/AGENTS.md` (process and lanes), `pc64/SPEC.md` (contract
> numbering), `pc64/DETACH.md` (the eligibility-gate pattern this reuses),
> `pc64/DEBUG.md` (report families), `docs/UNOUI.md` (the toolkit this is all in
> aid of). When a phase lands, flip its marker and add its `pc64/SPEC.md`
> contracts.

## 0. What is being asked for, and the verdict

**Goal.** Run Linux and Windows applications in UnoDOS, and where possible have
them appear as UnoDOS applications: a real `unoui_window` each, native title
bar, native close box, the modern WM (snap, Alt-Tab, taskbar, virtual desktops,
tiling) applying to them, and the active theme reaching inside their content.

**Verdict on the two obvious shortcuts, both rejected.**

*Reimplementing the ABIs natively for the general case* (a Linux personality:
ELF loader plus syscall emulation) fails on the same arithmetic that killed the
native Blink port in Glide's `docs/WEBVIEW-PLAN.md` §0, and fails harder here
because pc64 is further from POSIX than Glide is. pc64 runs everything in ring 0
in one address space on the firmware's identity map, has no per-process paging,
no `fork`, no `mmap`, no signals, no threads, and a filesystem facade over FAT
8.3 names. A Linux binary that does nothing but `printf` needs `mmap`,
`brk`, TLS, `writev`, and a dynamic linker before it prints. Every piece of that
is guest-shaped anyway, minus the isolation.

*Emulating an x86 CPU in software* discards the one advantage this port has over
Glide: the guest's instruction set is the host's instruction set. Interpreting
it would be slower than the PS2 port's 3D rasteriser for no benefit.

**So the architecture is a hypervisor plus a window protocol, in that order:**

- **Track A, `unovirt`:** a small VMX/SVM hypervisor inside pc64, guest memory
  through EPT/NPT, virtio-mmio device models, and one Linux appliance. This is
  the machinery. It is a direct translation of Glide's V2..V3c ladder into x86
  terms, and the ladder's ordering is the part worth copying, not just the ideas.
- **Track B, `unoguest`:** the protocol and the guest-side agents that turn
  foreign top-level windows into real UnoDOS windows, and then push the UnoDOS
  theme down into their content. This is the part the user actually sees, and it
  is designed so it is testable against a host-side stub agent before any VM
  exists (the same trick as `webviewd`'s stub renderer).
- **Track C, `unowin32`:** a native Win32 personality with no VM at all, for the
  subset of Windows software that a personality genuinely can host. This exists
  because pc64 is unusually well placed for it (see §6) and because it is the
  only tier where a foreign app gets *actual* unoui widgets rather than a very
  good imitation of them.

Tracks A and B are the spine. Track C is parallel and independent, and can be
cancelled without touching the others.

---

## 1. The five integration levels

"Where possible" is the load-bearing phrase in the request, so the levels are
defined up front, worst to best, with what each one costs and what it can never
give. Every foreign application lands on exactly one of them, and the level is a
property of the application's toolkit, not of UnoDOS.

| Level | What the user sees | Mechanism | Applies to |
|---|---|---|---|
| **L0 Appliance** | one UnoDOS window containing a whole foreign desktop | guest framebuffer blitted into a `UI_CANVAS` | anything, including a Windows desktop |
| **L1 Seamless** | one UnoDOS window per foreign window, native chrome, native WM | guest agent streams per-window damage into shared surfaces; unoui events go back as injected input | anything with a window manager we can replace |
| **L2 Themed** | as L1, and the app's *controls* follow the active UnoDOS theme | unoui's painters compiled inside the guest as a toolkit style backend, live palette and metrics over the channel | Qt directly; GTK by generated CSS; Win32 by generated theme |
| **L3 Native chrome** | menus, message boxes and file dialogs are real unoui widgets over real UnoDOS storage | structured menu and dialog descriptions forwarded instead of pixels, through the toolkit's own platform-integration extension points | Qt (QPA), GTK (D-Bus global menu), Win32 (hooks, best effort) |
| **L4 Personality** | the app is a UnoDOS process; `CreateWindowEx("BUTTON")` **is** a unoui button | `unowin32`: PE32+ loader plus a Win32 subset over unoui/unofs/fb | small, well-behaved Win32 programs only |

L1 is the deliverable that makes the feature real. L2 is what makes it not look
borrowed. L3 is where a foreign file dialog stops offering the guest's
filesystem and starts offering yours. L4 is a different thing wearing the same
coat, and it is honest to keep it visibly separate.

**What no level gives**, stated now so nobody debugs it later: pixel-identical
font rendering between a guest toolkit and pc64's TTF engine, GPU acceleration
inside the guest, or a foreign app that keeps running when the appliance is
stopped.

---

## 2. What pc64 already has, and what each thing forces

The design is mostly determined by seven existing facts. They are listed with
the consequence rather than as background, because each one closes a door.

1. **Ring 0, one address space, firmware page tables.** There is no user mode to
   demote a guest into and no address-space switch to hide it behind. The
   isolation therefore has to come from the hardware virtualization extensions;
   EPT/NPT is not a performance choice here, it is the only boundary available.
   It also means guest memory is directly addressable by the device models with
   no mapping step, which makes the virtio backends far simpler than Glide's
   (whose EL2 runs with its MMU off and cannot touch cached kernel memory).

2. **Single core, cooperative frame loop, no locks** (`pc64_uui.c`). A guest
   cannot simply be scheduled. Phase V3 runs the guest as a *budgeted slice of
   the frame loop* using the VMX preemption timer, which preserves the no-locking
   invariant exactly. SMP (V11) then follows Glide's rule literally: application
   processors run VMM code only, never touch kernel state, and talk to the BSP
   through lock-free rings and an IPI.

3. **The watchdog fires on a 20 s stale heartbeat, and >100 ms is a logged
   hitch** (`pc64/DEBUG.md`, S-DBG-09/19). A guest slice sits inside the
   heartbeat interval, so the budget is a correctness constraint and not a
   tuning knob. `uno_dbg_heartbeat()` around each slice, the same way
   `uno_dbg_net_trace` feeds it during a WiFi join.

4. **`unonet`'s TCP is single-connection by contract** (S-NET-12/13). The guest
   cannot borrow it and must not try. The bridge is at the Ethernet layer through
   the `uno_nic_t` seam, guest owns TCP/TLS/DNS entirely. This is the same
   conclusion Glide reached for the opposite reason (no TCP at all), and it is
   the right one anyway: a browser in the guest wants hundreds of sockets.

5. **`uno_modload_reserve()` already carves executable pages before
   ExitBootServices**, with a BIOS-path fallback through `uno_bios_find_ram()`.
   `uno_vmm_reserve()` is the same function with a different size, and inherits
   the same constraint: on the BIOS path only the low 4 GiB is mapped by the
   loader's page tables, so the guest carve lives below 4 GiB until pc64 builds
   page tables of its own.

6. **`detachgate.c` is an eligibility gate with attributed blockers**
   (`pc64/DETACH.md`, S-BOOT-07). Virtualization has more ways to be unavailable
   than detach does, and the same shape answers them: `uno_vmm_eligible()`
   returns a reason, never a hang, and the Settings row shows it.

7. **`UI_CANVAS` is an app-drawn rect with its own `draw` and `event` hooks, and
   the shell can add and remove windows at runtime** (`pc64_shell_add_window`,
   `unoui.h`). A foreign window therefore needs no new widget type and no
   compositor change: it is a one-widget unoui window whose canvas blits a
   surface and whose events are forwarded. L1 costs the WM nothing.

The one thing pc64 lacks that Glide had for free is a **single** virtualization
architecture. ARM has EL2; x86 has Intel VMX and AMD SVM, which agree on
concepts and disagree on every register. That is handled the way this repo
handles every such split, with a backend vtable (`uno3d`'s rasterisers,
`unoui`'s themes, `unobus`' drivers): `uno_hv_t` with `vmx`, `svm` and `none`.

---

## 3. Track A: `unovirt`

### 3.1 The backend seam

```c
typedef struct uno_hv {
    const char *name;                       /* "vmx" | "svm"                  */
    int  (*probe)(uno_hv_caps *out);        /* 0 = unavailable, with a reason */
    int  (*enable)(void);                   /* VMXON / EFER.SVME              */
    int  (*vcpu_create)(uno_vcpu *, const uno_vm_cfg *);
    int  (*vcpu_run)(uno_vcpu *, unsigned budget_us, uno_vmexit *out);
    int  (*map)(uno_vm *, u64 gpa, u64 hpa, u64 len, unsigned prot, unsigned memtype);
    void (*inject)(uno_vcpu *, unsigned vector, unsigned err, int has_err);
} uno_hv_t;
```

`vcpu_run` returning an *architecture-neutral* `uno_vmexit` is what keeps the
device models, the scheduler slice and the debug reporting free of vendor
detail. Exactly one file per vendor decodes exit reasons into that struct; every
other file in the subsystem compiles once.

### 3.2 Eligibility, and the ways this is unavailable

`uno_vmm_eligible()` refuses, with attribution, when any of:

- no `CPUID.1:ECX.VMX[5]` and no `CPUID.80000001:ECX.SVM[2]`;
- **`IA32_FEATURE_CONTROL` is locked with VMXON-outside-SMX clear.** This is the
  one that bites in the field: the machine supports VMX, the firmware disabled it
  in setup, and the lock bit means the OS cannot turn it on. It must be a named
  blocker with the words "enable virtualization in firmware setup", not a fault;
- no EPT with 4-level paging, or no NPT (`CPUID.8000000A:EDX.NP`);
- **not detached.** While firmware-attached, boot services own timers, the
  memory map and SMM entry conditions, and `pc64/DETACH.md` already establishes
  that reprogramming under a live firmware corrupts it (S-BLK-03 is the same
  lesson for storage). The appliance requires `gDetached`; this also means the
  OS owns its own IDT and LAPIC, which is what an exit path needs;
- total usable RAM below the appliance floor (§3.4).

Nothing about that list is a runtime surprise. It is computed once at boot and
printed, like the detach gate's blockers.

### 3.3 The foothold, and how it is proved

Glide's V2 is worth copying verbatim in spirit: the banner does not say "we own
EL2", it says "we made a hypercall and control came back". The pc64 analogue:

```
HV        : vmx, ept 4-level, unrestricted guest, apicv no, ept-wb yes
            guest cpuid round trip -> 0x534f444f4e55 OK, exit reason 10
```

`cpuid` is the right instruction for this because it exits unconditionally on
both vendors, needs no memory, no interrupt controller and no timer, and cannot
be faked by a buffer that happens to hold the right bytes: the marker comes back
in the vCPU's saved registers after a VM exit that names its own reason.

Then, immediately and before anything else is built on it, the **crasher
guest**, as in Glide V3b and for the same reason: a guest that faults on purpose
must produce a report and leave the desktop running. Without that, every later
mistake presents identically, as a machine that stops. pc64 has somewhere to put
the report already: a new family `GF###.TXT` alongside `CR/HG/RS/PN/PF` under
`CRASH\<TAG>\`, carrying exit reason, guest RIP, exit qualification, and the
guest-physical address for EPT violations. `pc64/DEBUG.md` §"report families"
gains one row.

### 3.4 Guest memory

`uno_vmm_reserve()` runs beside `uno_modload_reserve()` in `try_detach`, before
ExitBootServices, and takes one contiguous carve:

| Host RAM | Carve | Note |
|---|---|---|
| < 2 GiB | none | eligibility refuses; L4 personality still works |
| 2..4 GiB | 768 MiB | one appliance, one or two apps |
| 4..8 GiB | 1.5 GiB | comfortable Linux appliance |
| > 8 GiB | 2 GiB | plus a second carve if a Windows appliance is configured |

Those are installed sizes; the gate compares against **free conventional
memory**, which is always less (a 4 GiB machine reports about 3.9 GiB under
OVMF and less under a vendor firmware). `uno_vmm_carve_mb()` therefore steps at
1800/3500/7000 MiB rather than at the round numbers, which is the difference
between a 4 GiB laptop getting its 1.5 GiB carve and silently dropping a step.

Three constraints, each of which has already cost somebody a day somewhere:

- **The carve must be write-back.** pc64 has `pc64_mtrr.c` and the debug builds
  can force write-combining; a guest running out of UC memory is slower than
  emulation and the symptom is "virtualization is useless", not "the MTRR is
  wrong".
- **The carve must be host-mapped**, because the device models read and write
  guest memory directly with ordinary loads and stores. Below 4 GiB on the BIOS
  path, unconditionally, until pc64 owns its page tables.
- **Nothing zeroes the carve and nothing should** (Glide V3c.7's finding). What
  gets zeroed is the handful of structures a device writes and the guest reads
  back, and they are zeroed *first*, before the kernel image and its parameters
  are placed. Doing it last wipes the guest and it then executes whatever zero
  decodes to.

### 3.5 The slice, or how a guest runs without a scheduler

```
shell frame N:  poll input -> handle events -> render -> present
                             \-> uno_vmm_tick():  vcpu_run(budget 4..8 ms)
                                                  service exits
                                                  uno_dbg_heartbeat()
```

Entry sets the VMX preemption timer (or, on SVM, an LAPIC one-shot the host
takes as an intercept) so an uncooperative guest cannot hold the core. The exit
is not an interruption of the guest, it is the *end of its slice*, and the vCPU
context is resumable by construction, which is the same property Glide needed
before trap-and-emulate could work.

The honest number: a guest getting 6 ms of every 16 ms frame has about 35% of
one core. Web browsing and a text editor are fine; compiling is not. V11 (SMP)
is what changes that, and it is deliberately last because it is the phase that
can destabilise the kernel's central invariant.

**A guest slice must never run while a modal security dialog is up.** The URC
gate already learned that synthetic input and security dialogs interact badly
(`unodos-urc-production-gate`); a guest holding the core during an RBAC prompt
is the same failure with worse timing.

### 3.6 Devices

virtio-mmio transport, not PCI, for the Linux appliance. Linux discovers
mmio transports from the kernel command line
(`virtio_mmio.device=4K@0xd0000000:5`), so a whole PCI host bridge and its
config space are avoidable, which is exactly the trade Glide made with a device
tree. The device set is small on purpose:

| Device | Backend | Notes |
|---|---|---|
| console | ring to `CRASH\GUEST.TXT` and the debug console | guest dmesg visible from the first boot, before anything else works |
| blk | an image **file** on unofs (`EFI\UNODOS\VM\ROOTFS.IMG`), through `uno_fs_read_at`/`uno_fs_write` | no new partition scheme, no installer change, and the appliance is a file the user can delete |
| net | raw Ethernet bridged at the `uno_nic_t` seam | guest owns TCP/TLS/DNS; see R3 for the one-MAC problem |
| input | not virtio: unoui events go to the agent over the control channel | keeps the guest input path trivial and keeps the shell the sole toucher |
| frames | a custom shared-surface device, not virtio-gpu | we need BGRA rects with damage, not a display server. virtio-gpu is weeks of work for nothing L1 needs |

`fb.h` is `0xAABBGGRR` (S-FB-06) and the guest agent produces BGRA; the surface
format is fixed at that and converted nowhere, because a per-pixel conversion in
the blit path at 1920x1200 is the whole frame budget.

### 3.7 Booting Linux without a BIOS

**The x86 equivalent of Glide's device tree is `boot_params`.** The appliance
loads a `bzImage` and an initramfs into the carve, fills the zero page (e820 map
describing the carve, command line pointer, initrd address and size, the
`ext_loader_type` fields), and enters at the 64-bit entry point with the register
contract the boot protocol specifies. No BIOS, no UEFI, no real mode in the
guest, which means "unrestricted guest" is a nice-to-have for later appliances
rather than a requirement for this one.

That decision has a corollary worth stating: the Windows appliance (V12) cannot
work this way. Windows needs firmware, ACPI tables, a PCI host bridge and
virtio-**pci** drivers, which is a machine model several times the size of this
one. It is therefore a separate, later phase and not a variant of V5.

---

## 4. Track B: `unoguest`, the part that is the point

### 4.1 The channel

One shared-memory control region per appliance plus one surface slot per foreign
window, in the guest carve, addressed directly by both sides.

```
control page:  magic 'UGST' | version | caps | theme_seq | win_count
               cmd  ring 64 x 256 B     UnoDOS -> agent
               evt  ring 64 x 256 B     agent  -> UnoDOS
theme page:    the live unoui_palette + unoui_metrics + font metrics, versioned
surface slot:  {win_id, w, h, stride, format, damage_seq, damage rects[16]} + pixels
```

Single-producer single-consumer rings with sequence counters, polled from the
shell frame loop, which every UnoDOS frame already is. No new IPC primitive, no
interrupt on the hot path.

**Events (agent to UnoDOS):** `WIN_CREATE(id, w, h, title, flags, parent)`,
`WIN_TITLE`, `WIN_RESIZE`, `WIN_CLOSE`, `WIN_DAMAGE(id, seq)`, `CURSOR(shape)`,
`MENU(id, structured menu tree)`, `DIALOG(kind, structured description)`,
`APP_LIST(icons and names for the Start menu)`.

**Commands (UnoDOS to agent):** `INPUT_MOUSE`, `INPUT_KEY`, `INPUT_CHAR`,
`INPUT_WHEEL`, `SET_RECT`, `SET_FOCUS`, `CLOSE`, `MINIMISE`, `THEME(seq)`,
`LAUNCH(app)`, `MENU_PICK`, `DIALOG_RESULT`.

The event set is deliberately close to `unoui_event` and `unoui_action` in shape,
because the conversion at both ends should be a switch statement and not a
translation layer.

### 4.2 L1, seamless windows

For each `WIN_CREATE`, the shell builds a genuine `unoui_window` holding exactly
one `UI_CANVAS`:

- `canvas->draw` blits the surface slot's damaged rects. Undamaged frames cost
  nothing, which matters because `uno_pc64_present` already diffs rows and a
  fully clean frame does zero VRAM writes (S-BOOT-04).
- `canvas->event` forwards mouse and key events as `INPUT_*`, translated to
  window-local coordinates. The shell remains the only thing that touches the
  pointer, unchanged.
- Everything else in the window is the OS: the title bar, the close box, snap,
  Alt-Tab, minimise, the taskbar button, virtual desktops, the switcher
  animation. None of it needs to know the content is foreign.

That last point is the whole return on this design. The modern WM landed
2026-08-03 and every one of its behaviours applies to foreign windows for free,
because a foreign window is not a special case of anything.

**The agent, per guest:**

- **Linux:** the agent *is the compositor*. A headless Wayland compositor
  (wlroots-based) with no output of its own, handing each surface's buffer to a
  slot, plus Xwayland for X11 clients. This is the only design where per-window
  buffers arrive without screen-scraping, and it makes the guest's window manager
  a thing we wrote rather than a thing we fight.
- **Windows (V12):** Windows Graphics Capture per window plus a window-event hook
  for create/destroy/move/title. The Windows analogue of "replace the compositor"
  does not exist, so this tier is capture-based and honest about it.

### 4.3 L2, the theme goes inside

This is the piece that makes foreign applications look like UnoDOS applications
rather than like a screenshot of another computer, and it exists because of one
property of this codebase: **`unoui` is portable C over `fb.h` with a swappable
theme vtable and zero platform code in its input layer** (`docs/UNOUI.md` §1,
§7). It compiles in the guest as easily as it compiles for the PS2.

- **Qt: a real `QStyle` plugin.** `QStyle` is a painter vtable, `unoui_draw` is a
  painter vtable, and the mapping between them is mechanical for the primitives
  that matter (`PE_PanelButtonCommand` to `ui_bevel`, `PE_FrameLineEdit` to the
  field painter, `CC_ScrollBar` to the scrollbar painter, and so on). The style
  paints into the widget's `QPainter` through a small `fb.h` shim over a
  `QImage`. Qt applications then get UnoDOS's *actual* bevels, palette roles and
  bit-depth handling, and a theme switch in the UnoDOS Control Panel changes them
  live through `THEME(seq)`.
- **GTK: a generated CSS theme.** GTK's theming is CSS, not a painter vtable, so
  there is no honest way to run unoui's painters inside it. What is honest is a
  transpiler from `unoui_palette` plus `unoui_metrics` to a GTK CSS theme,
  regenerated whenever `theme_seq` changes. Colours, metrics, corner radii and
  focus rings track; bevel *character* does not. Say so in the docs rather than
  claiming parity.
- **Win32 (V12):** palette through the system colour APIs plus a generated visual
  style for the common controls. Best effort, and applications that owner-draw
  will simply not follow.

The tiering here is not a compromise, it is the answer to "where possible". Qt is
possible. GTK is approximable. Electron is not, and no amount of work changes
that.

### 4.4 L3, menus and dialogs stop being pixels

A foreign menu bar rendered as pixels inside a canvas is the moment the illusion
breaks, because it is the one part of an application the OS normally owns. Both
major toolkits already have the extension point, because macOS's global menu bar
and Unity's app menu forced them to:

- **Qt:** `QPlatformMenu` / `QPlatformMenuBar` / `QPlatformDialogHelper` in the
  QPA plugin. The plugin sends the menu *structure* as a `MENU` event; UnoDOS
  renders it with `unoui_add_menubar` and real popups; the pick comes back as
  `MENU_PICK`. Same for `QFileDialog` and `QMessageBox`, which means a Qt app's
  file dialog becomes the UnoDOS file dialog over `unofs`, showing UnoDOS
  volumes.
- **GTK:** the `org.gtk.Menus` / `com.canonical.AppMenu` D-Bus protocols that
  already carry menu models out of GTK applications for desktop shells. The agent
  subscribes and forwards.
- **Windows:** hooks around the menu APIs, best effort, marked as such.

The file dialog case is where this earns its keep: it turns "the foreign app can
only see the guest's disk" into "the foreign app opens files from your machine",
without a shared filesystem, because the *dialog* is native and only the chosen
bytes cross the boundary.

### 4.5 Testing this before any hypervisor exists

`unoguest`'s protocol, the L1 window plumbing, the theme channel and the menu
forwarding are all testable with a **host-side stub agent**: a process on
amanuensis or devbuntu that speaks the same rings over a file-backed region and
serves canned windows. That is the same trick as `webviewd`'s stub renderer in
Glide's plan, and here it means Track B lands and is provable in QEMU under TCG
while Track A is still on the metal boxes. It also stays useful forever as the
thing that paints "appliance starting" and the error window.

---

## 5. Track C: `unowin32`, no VM at all

pc64 is closer to hosting Win32 than any other target in this repo, for a reason
that is an accident of the module format: **a `.UNO` is already a flattened
PE32+ DLL with relocations and imports resolved by an in-kernel loader**
(`pc64/MODULES.md`), built with a mingw toolchain. The loader that loads a real
PE is a superset of one that already exists and is already hardened (S-MOD-01
through S-MOD-14 are exactly the checks a PE loader needs).

So `unowin32` is: a real PE32+ image loader, a `kernel32`/`user32`/`gdi32`/
`comctl32` subset implemented over `unofs`, `unoui`, `fb.h` and the pc64 heap,
and a message pump that is the shell's frame loop. `CreateWindowEx` with class
`BUTTON` produces `unoui_add_button`. `MessageBox` produces a unoui dialog.
`GetOpenFileName` produces the UnoDOS file picker.

**What it can host:** single-threaded, non-DirectX, non-COM Win32 programs that
draw with GDI and the common controls. Notepad-shaped things, small tools,
period software. **What it cannot host:** anything that expects the NT kernel,
threads, DirectX, .NET, or a modern C runtime. That is not a phase-ordering
problem, it is the boundary of the technique, and the appliance exists for
everything past it.

It is worth building anyway because it is the only tier where a foreign
application's controls *are* unoui widgets, with real hit-testing, real theming
and real accessibility to the toolkit's own event stream, at a fraction of the
appliance's cost. It also has no dependency on Track A whatsoever.

---

## 6. Ownership, seams, and process

New rows for `/AGENTS.md` §1, added in each phase's first commit:

| Subsystem | Contract / spec | Root files |
|---|---|---|
| unovirt (VMX/SVM, EPT/NPT, vCPU, exit decode, guest carve) | `docs/UNOVIRT-PLAN.md`, `pc64/UNOVIRT.md` | `unovirt*`, `hv_vmx*`, `hv_svm*` |
| unovdev (virtio-mmio transport and device models) | `pc64/UNOVDEV.md` | `unovdev*` |
| unoguest (channel, agents, appliance images, seamless windows) | `pc64/UNOGUEST.md` | `unoguest*`, `guest/` |
| unowin32 (native PE32+ personality) | `pc64/UNOWIN32.md` | `unowin32*` |

Shared choke-points touched, all append-only per `/AGENTS.md` §2: the `build.sh`
file list; `kExports` `KX()` lines for anything a future module calls; one boot
call in `uefi_main.c` (`uno_vmm_reserve`) and one tick call in `pc64_uui.c`
(`uno_vmm_tick`); `pc64/REMOTE.md`'s verb table plus **a `GATE[]` row in the same
commit** for each new URC verb (`vm status`, `vm start`, `vm stop`, `vm shot`),
since the table is fail-closed; `pc64/DEBUG.md`'s report families for `GF###`.

New `pc64/SPEC.md` areas: **S-HV** (eligibility, foothold, carve, slice
budget, containment), **S-VDEV** (virtqueue bounds and descriptor-chain
termination), **S-GUEST** (ring discipline, window lifecycle, theme sequence),
**S-W32** (loader validation, reusing the S-MOD numbering discipline).

**The bounds-checking contract is the security boundary and gets written down
before the first device model.** Glide's `guest_pa()` is the model: every address
in a virtqueue came from the guest, including the ones inside descriptors;
address and length are checked *together* because checking them separately is how
an overflow slips through; and the descriptor chain walk is bounded by the queue
size because a guest can point a descriptor's `next` at itself. On pc64 this is
sharper than on Glide, because a bad guest address is not a fault, it is a
successful write into the kernel's own address space.

### Licensing, which constrains what can be in the tree

The standing rule from 2026-08-05 is **no GPL in the repository, MIT is fine**.
That rule is not violated by this programme, but it does dictate the shape:

- The Linux kernel and a Buildroot rootfs are GPL. They are **never** in the
  tree and never linked into anything. The appliance is an artefact built on
  devbuntu and staged on the NAS beside the flasher, fetched or installed as a
  file (`EFI\UNODOS\VM\ROOTFS.IMG`). Mere aggregation on a filesystem, the same
  relationship a UnoDOS machine has with a `.txt` file.
- The guest-side agent and the Qt style plugin *are* ours and *do* link Qt and
  wlroots inside the guest. They live in `guest/`, ship in the appliance image,
  and are covered by the appliance's own licence notice, not by
  `DOCS\LICENSES.MD` in the OS image. Keeping them out of the kernel's link is
  what makes that clean.
- Windows and its virtio drivers are the user's, supplied by the user, referenced
  by configuration. UnoDOS ships no Microsoft bytes.

`unomedia`'s licensing precedent (notices in About and `DOCS\LICENSES.MD`)
applies to anything that does end up linked.

---

## 7. Phases, exit criteria, sizing

Worktree per slice off `origin/master`, per `/AGENTS.md` §3. Track B's V0 and
Track C are parallel-safe from day one; Track A is a serial spine.

| Phase | Contents | Exit criterion (provable) | Est. sessions |
|---|---|---|---|
| **B0** | `unoguest` channel + host-side stub agent + L1 window plumbing + `vm` URC verbs and gate rows | in QEMU: three stub "foreign" windows are real unoui windows; snap, Alt-Tab, taskbar and close all work on them; screenshots in the gate | 2..3 |
| ~~**A0**~~ | ~~capability gate~~ | **DONE 2026-08-06** (`pc64/UNOVIRT.md`): the probe, the attributed blockers, the `HV:` line in every boot log, and `tools/hv_test.py` across five QEMU CPU models plus one KVM boot on real capability MSRs. Contracts S-HV-01..11. Outstanding: the **Intel arm has never run** - both metal boxes are Intel and TCG drops `+vmx`, so the first hardware boot is also the first execution of half the file. | 1..2 (spent: 1) |
| **A1** | SVME/VMXON, a vCPU context, the `cpuid` round trip, and the crasher guest | **MET on VMX 2026-08-06**: `guest round trip -> 534f444f4e55 OK, crasher contained (exit 2)`, and the boot continues. The `GF###` report family is still to come (A3), since nothing yet runs a guest outside the selftest. | 2..3 (spent: 2) |
| ↳ A1a | **WRITTEN, NOT PROVED 2026-08-06:** the `uno_hv_t` seam, the SVM backend, the full lower-register vCPU context (Glide's V3c.1 lesson taken up front), the marker guest and the crasher. `EFER.SVME` sets and the host save area is accepted; **the first VMRUN does not return**, reproducibly, on the only machine available - which is three levels of nesting deep (Hyper-V → WSL2 → KVM → UnoDOS). Not known whether that is this code or that environment. The selftest is therefore opt-in (DEBUG.CFG `vm-selftest`), because a VMRUN that does not return leaves GIF clear and no interrupt, including the watchdog, can reach that core again. | 1 |
| ↳ A1b | **DONE 2026-08-06:** the VMX backend, and the first run at **one** level of nesting (devbuntu, bare metal, Intel, `kvm_intel nested=Y`). VMXON, a guest, a CPUID intercept answered, the marker read back out of GUEST memory, the guest resumed at RIP+2 to its `hlt`, then a guest that triple-faults on purpose and a boot that carries on past it into the shell's frame loop. `tools/hv_remote.py` reruns it. It also gives the Intel arm of A0 its first real numbers: `ept wb 2m 1g unrestricted vpid preempt`. | 2..3 (spent: 1) |
| ↳ A1c | The SVM row stays unproved: no AMD machine with KVM at L0 is on the LAN. Not blocking - A2 can be built and gated on VMX, with the AMD backend following the same seam. | 1 |
| ~~**A2**~~ | ~~EPT/NPT, the carve, WB memory type~~ | **DONE 2026-08-06 on VMX**: `ept OK gpa 0x100000 -> pa 1bf00000 wrote 534f444f4e56 (memtype 6, 1536 MB at 1be00000)`. The exit criterion word for word, plus the `uno_vmm_gpa()` bounds seam the device models will be built on. Trap: a 2 MiB second-stage leaf must be 2 MiB ALIGNED and `AllocatePages` promises 4 KiB, which presents as EPT misconfiguration (exit 49). NPT on the SVM backend waits on A1c. | 2 (spent: 1) |
| ~~**A3**~~ | ~~the frame-loop slice~~ | **DONE 2026-08-06 on VMX**: `120 slices x 4000 us budget: max 4811 us, mean 4025 us, 0 exits that were not the clock`, with `shots/hv_slice.png` showing the desktop painting mid-run (HUD: 0.5 ms render, 93% idle, no crash reports). Found and fixed a defect in A1/A2: a VM exit loads host RFLAGS with IF clear, so the host had been running with interrupts disabled since its first guest - invisible, because only the watchdog uses them. **Still owed: the 10-minute soak and a real window drag, both metal.** | 2 (spent: 1) |
| ~~**A4**~~ | ~~guest clock, interrupt, MSR space~~ | **DONE 2026-08-06 on VMX**: `clock ticks (+59145778 over 140 exits), irq taken once (1, not redelivered), msr answered`. The criterion word for word. The clock is sampled ACROSS slices rather than within one, and the interrupt count is re-checked after forty more entries, because delivered-forever and never-delivered look the same for the first millisecond. **Carried forward:** an MSR bitmap (today every MSR access exits, fine for eighteen instructions, unworkable for Linux) and a virtual LAPIC, both of which belong with A5's device work rather than here. | 2..3 (spent: 1) |
| ~~**A5**~~ | ~~virtio-mmio + console~~ | **DONE 2026-08-06 on VMX**: `virtio OK magic 74726976, used.idx 1, 29 bytes in 1 notify, cycle refused`. All three parts of the criterion. **The structural finding:** an EPT violation gives the address and a direction bit and NOTHING about the register or operand size, so x86 needs an instruction decoder where ARM needs a switch on a syndrome register - this is the biggest single divergence from Glide's design. Scope keeps it small: driver MMIO is `mov`, so four opcodes are decoded and everything else is refused loudly. | 2..3 (spent: 1) |
| **A6** | Linux boot protocol: bzImage, `boot_params`, initramfs | Linux boots to a shell on virtio-console under UnoDOS, on metal | 3..4 (spent: 1) |
| ↳ A6a | **LINUX BOOTS AND PRINTS 2026-08-06**, not yet to a shell. A 16.8 MB Ubuntu kernel loads into the carve, takes a `boot_params` zero page, and runs: it reads the command line we placed, echoes back the **e820 map we invented** (our reserved run included), enables a console on our 8250, and is still running when the three-second bound stops it. Four faults on the way, each further in: a triple fault (no IDT yet); `#GP(0)` because **VMX forces CR4.VMXE and Linux clears it - a kernel cannot boot without CR shadow registers**; a `#PF` that was OUR fault for stealing an exception the decompressor takes on purpose and handles itself; then silence, because **port I/O does not exit unless asked** and the guest had been driving the host's real ports. | 1 |
| ↳ A6b | **IN PROGRESS 2026-08-06: the kernel completes its boot.** 333 lines, ending `Kernel panic - not syncing: VFS: Unable to mount root fs` - the honest panic for a machine with no root filesystem. Runs from `uno_vmm_tick` at 4 ms a frame. Six findings, and they share a shape: **one VMCS means one guest**; **Linux calibrates its clock against the 8254** (spun on port 0x42); **INVPCID #UDs in a guest unless enabled** while CPUID says otherwise; **XSETBV always exits** because VMX carries XCR0 in neither direction; **XSAVE had to be masked out of CPUID entirely** (the kernel died in `fpstate_reset` with a null pointer, which is a zero xstate size seen from the far end); and **0xFF is a dangerous default for a status register** - the CMOS read as permanently update-in-progress and the kernel waited forever. The default is never "works as on real hardware". | 1 |
| ↳ A6c | **USERSPACE 2026-08-06.** A 1.1 MB busybox initramfs loads at GPA `0x20000000` via `ramdisk_image`/`ramdisk_size`; the kernel unpacks it, registers `ttyS0` as a real console, and execs the shell. Needed **the SYSCALL MSRs written through to the real machine** (LSTAR/STAR/SFMASK/KERNEL_GS_BASE in a variable works until userspace runs its first `syscall`, which reads LSTAR out of the CPU) and **a `/dev/console` node inside the cpio** (the kernel opens it before init runs, so an init that creates it is too late and cannot report the failure either). **The shell then exits with status 0, which IS the diagnosis: stdin at EOF.** The 8250 answers LSR with data-ready always clear, so nothing ever arrives. | 1 |
| ↳ A6d | **The receive path is in 2026-08-06**: an RX FIFO, LSR data-ready following it, a queued seed handed over on the first poll that finds it empty, and the MCR loopback the driver's autoconfig tests. **The shell still exits with status 0**, so the missing thing is not the bytes, it is the wake-up: the 8250 driver is interrupt-driven and **this guest has no interrupt controller at all**. | 1 |
| ↳ A6e | **An 8259 and two interrupts**, which is the last piece: ports 0x20/0x21 and 0xA0/0xA1 so the guest can unmask and acknowledge (`nolapic` means Linux uses the legacy PIC), IRQ4 injected when the RX FIFO gains a byte, IRQ0 from the PIT for a periodic tick, both through `VM_ENTRY_INTR_INFO` - the path **A4 already proved** with vector 0x20 and which has been idle since. Everything under it exists: the queue, the status bit, the injection mechanism, the frame loop. | 1..2 |
| **A7** | virtio-blk over a unofs image file, virtio-net bridged at `uno_nic_t` | on metal: guest mounts its rootfs, gets a lease, fetches 10 MB over HTTPS twice with no wedge | 4..5 |
| **A8** | Shared-surface frames device + **L0**: whole guest framebuffer in one unoui window | a foreign desktop visible and clickable inside a UnoDOS window | 2 |
| **B1** | The real Linux agent: headless wlroots compositor + Xwayland, replacing the stub behind the same channel; **L1 on real applications** | three real Linux applications as three native UnoDOS windows, WM behaviours intact, in a screenshot set | 4..6 |
| **B2** | **L2:** unoui compiled in the guest as a Qt style + QPA platform plugin; GTK CSS transpiler; live `THEME(seq)` | a Qt application repainted by UnoDOS's own painters; switching theme in Control Panel re-skins it live, side-by-side shots under three themes | 4..5 |
| **B3** | **L3:** QPA menu and dialog helpers; GTK D-Bus menu forwarding; native file dialog over unofs | a Qt application's File menu is a unoui menu, and its open dialog lists UnoDOS volumes | 3..4 |
| **A9** | SMP: AP-hosted vCPU, lock-free rings, IPI doorbells, kernel stays single-core | guest gets a full core; the EL1-equivalent invariant test (kernel state unchanged across guest runs) passes; no lock anywhere in kernel code | 4..6 |
| **A10** | Lifecycle: appliance off at boot, cold start on first launch, freeze when idle, watchdog restart, Settings row, `pull-the-rug` tests | kill the agent, kill the guest, stop mid-load: the native side never blinks | 2..3 |
| **A11** | **Windows appliance:** PCI host bridge, ACPI tables, virtio-pci, user-supplied media, Windows agent for L1, generated visual style for L2 | a Windows application in a native UnoDOS window on metal | 8..12 |
| **C0..C3** | `unowin32`: PE32+ loader, kernel32/gdi32 subset, user32 over unoui, common controls | a real period Win32 binary running with genuine unoui widgets, no VM | 6..8 |

Total: roughly **50..70 sessions** for everything, of which **A0..A8 plus B0..B1
(about 25) reach the actual headline**: Linux applications as native UnoDOS
windows. B2 and B3 are what make them look and behave like they belong. A11 is
its own programme and should be scheduled as one.

---

## 8. Risks, with fallbacks

| # | Risk | Odds | Mitigation / fallback |
|---|---|---|---|
| R1 | `IA32_FEATURE_CONTROL` locked with VMX disabled by firmware | **medium, and normal** | Not a bug and not fixable from software. Named blocker, Settings row, documented "enable virtualization in firmware setup". The X1 and the ZimaBlade both need checking early, in A0, before anything is built on top |
| R2 | No nested virtualization on the usual test path | high | QEMU on Windows (amanuensis) cannot give nested VMX. Track A gates on **devbuntu with nested KVM** for logic and on the **ZimaBlade** for metal; Track B and Track C stay fully testable in QEMU/TCG. Same split as `nettest`: host-testable logic, silicon for the seams |
| R3 | Two IP addresses behind one MAC (guest plus native) | medium, real | Identical to Glide's R3, and the same two plans: guest DHCPs with a distinct client-id and the bridge demuxes inbound by destination IP; fallback is L3 NAT in the bridge. Decide against the real router, not on paper |
| R4 | SMM steals time unpredictably and lands inside a guest slice | low/medium | Budget is a *deadline*, not an assumption; the slice measures elapsed TSC and reports overruns to the hitch counter rather than trusting the preemption timer alone |
| R5 | The carve mapped UC instead of WB destroys performance | medium | Explicit MTRR/EPT memory-type check in A2's exit criterion, printed in the banner, not discovered later as "virtualization is slow" |
| R6 | Guest memory is directly addressable by the host, so a bounds bug is a kernel write | **high impact** | The single `uno_vm_gpa()` seam, written before the first device model, checking address and length together; the descriptor walk bounded by queue size; S-VDEV contracts for both, with SPECTEST feeding crafted rings |
| R7 | The frame loop stutters visibly once a guest runs | medium | Budget tuning plus the existing hitch counter and perf HUD as the measurement, not opinion. If a single core cannot do both, A9 (SMP) becomes a prerequisite for shipping rather than an improvement |
| R8 | Qt style mapping turns out shallower than hoped | medium | L2 degrades to the GTK approach (generated palette) for Qt as well; L1 is unaffected, which is why L1 is the phase that defines success |
| R9 | The Windows appliance's device surface is much larger than the Linux one | **certain** | Already priced in: it is A11, 8..12 sessions, after everything else works. It never blocks the Linux path |
| R10 | GPL contamination through the appliance | low | Structural, not procedural: the kernel and rootfs are a file on a filesystem, built elsewhere, never linked, never in the tree (§6) |
| R11 | Scope creep back into the kernel's single-core invariant | design risk | The Glide rule, adopted verbatim: application processors run VMM code only and never touch kernel state; cross-core is lock-free rings plus IPI; any slice needing a kernel lock is out of contract and goes back to design |
| R12 | The only always-on test box is three levels of nesting deep | **hit, 2026-08-06** | amanuensis is Hyper-V → WSL2 → KVM, so a guest under UnoDOS is a fourth level and nobody supports that. It was good enough to prove A0 against real capability MSRs and it is NOT good enough for A1. The one-level-deep machine is devbuntu (bare metal, Intel, nested KVM), which is why the VMX backend moved earlier in the order than planned |
| R13 | A wedged VMRUN is unrecoverable and silent | **structural** | GIF is clear from VMRUN until the STGI after it, and with GIF clear the core takes no interrupt at all - not the LAPIC watchdog whose whole job is this. So the first VMRUN on any new machine is operator-present and opt-in (DEBUG.CFG `vm-selftest`), the `mtrr-wc` precedent. This does not relax after A1 proves out; it relaxes when a machine has run it once |

---

## 9. Roads considered and not taken

- **A native Linux personality (ELF loader plus syscall emulation).** §0. pc64
  has no per-process address space, so the first thing it would need is the
  thing it most lacks.
- **Software CPU emulation.** Throws away the only structural advantage this
  port has, which is that the guest ISA is the host ISA.
- **Running the guest under firmware, before detach.** Boot services own the
  timers and the memory map, and the storage stack already learned what
  reprogramming a live firmware's controllers does (S-BLK-03).
- **virtio-gpu and a display server in the guest.** L1 needs BGRA rectangles
  with damage, not a display server. Revisit only if GPU passthrough ever
  happens.
- **virtio-pci for the Linux appliance.** A whole PCI host bridge for a guest
  that will happily take its devices from the command line.
- **Screen-scraping the guest's X server for L1.** A compositor we wrote gets
  per-window buffers directly and gets window lifecycle events for free.
  Scraping gets neither, and fights the guest's WM forever.
- **Shadow paging.** EPT/NPT is an eligibility requirement instead. A shadow
  paging path is a second complete MMU implementation for machines that should
  simply be told they cannot run appliances.
