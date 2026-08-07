# pc64 app registry: apps that install themselves

**Status: IMPLEMENTED, all five phases, 2026-08-07.** Written the same day in
answer to the request + correction in `pc64/UNOAUTOMATE-REQUESTS.md` ("a desktop
slot for APPS\VMGR.UNO", and the correction that a slot is not discoverability
but the only way to run a unoui-class module at all).

This document is kept as the RATIONALE - why the format is shaped the way it is,
and which constraints are load-bearing. The contract an app author needs is
`pc64/MODULES.md` and `pc64/uno_appdesc.h`. §12 records where the plan turned
out to be wrong.

Owner of the largest piece: the **toolkits lane** (`pc64_uui.c`). Additive
pieces landed in `pc64_modload.c` (choke-point), `tools/mkuno.py` and
`pc64/build.sh`. Two small capabilities were needed from other lanes and are
listed in §9; both were built here rather than filed, since neither is more than
a listing call.

---

## 1. What is wrong today

An app's existence is a compile-time fact in `pc64_uui.c`. Adding one
unoui-class `.UNO` costs, in a file the adding lane usually does not own:

| Edit | Where |
|---|---|
| `NEXTRA` +1, a new `EX_<APP>` index define | `pc64_uui.c:88-99` |
| `g_<app>`, `g_<app>_tried`, `g_<app>_present`, `<app>_ensure()` | `pc64_uui.c:246-315` |
| a row in the `app_name` ternary chain | `pc64_uui.c:323` |
| a row in the `app_short` ternary chain | `pc64_uui.c:334` |
| a row in `app_hidden` | `pc64_uui.c:348` |
| a `PCI_<APP>` id, its art, and a row in `app_icon` | `pc64_icons.h`, `pc64_icons.c`, `pc64_uui.c:372` |
| `->build`, `->opened`, `->closed`, `->canvas_index`, `->key`, `->action`, `->frame` | seven separate sites, `pc64_uui.c:2826-6153` |
| the boot presence probe | `pc64_uui.c:5931` |
| a ~10-line compile/thunk/link/convert block | `pc64/build.sh` |

Three consequences, all of them already observed rather than predicted:

- **A shipped app that cannot be run.** `APPS\VMGR.UNO` is built by
  `build.sh:559`, installs onto the ESP, and has no slot. `pc64_shell_run_user()`
  handles a PYAPP and a classic `.UNO` and has no third branch, so Files reports
  `Could not launch that .UNO.` The URC `launch <n>` verb indexes shell slots.
  There is no path to the app at all.
- **The seven-site trap.** Phase 8 of unoffice wired three of the seven hooks
  for UnoWord. The window appeared, drew correctly, and ignored every keystroke
  (2026-08-03 entry). The advice that came out of it, "grep for `g_photos->` and
  match ALL of them", is a workaround for a shape that should not need one.
- **Lane friction.** LOGVIEW's slot was filed as a request, superseded, then
  made by the unolog lane itself under explicit authorisation, with a note
  asking the file's owner to rework it if the shape was wrong. That is eleven
  edits of process for one icon.

And a fourth that has not bitten yet but is loaded and pointed at the floor:
**everything durable is keyed by slot index**. `SHELL.CFG` writes `geom14=`,
`snap14=`, `min14=`, `desk14=`, `grp14=` and `open=0,2,14`; `g_icon_pos[32]` is
indexed by app; the manual's scenes and parts of the harness launch by
Start-menu row. Insert one app in the middle of the roster and every one of
those silently means a different app. The `pc64_icons.h` header already carries
the note that got the icons out of this trap: *"An icon is a property OF AN APP,
not of its position in the launcher... Apps loaded from storage at runtime could
never have worked that way at all."* The same argument applies to the other
five.

## 2. What "extensible" has to mean here

The ask is that dropping `FOO.UNO` into `APPS\` gives you a desktop icon, a
Start-menu row and a taskbar entry, with no kernel edit and no rebuild. That
implies four things that the current design cannot do, in order of difficulty:

1. **The shell must find the file.** It has no directory scan; it has a fixed
   roster of names.
2. **The shell must learn the app's name, icon and category without running
   it.** Loading a module means an arena allocation, a rebase, an import
   resolve and a call into `uno_app_main`. Doing that for every file in `APPS\`
   at boot would cost 4 MB of arena that never frees (`MOD_ARENA_PAGES` is
   1024 pages and `mod_free` only unwinds the most recent allocation), plus
   ~1.1 s of single-sector I/O per 300 KB module.
3. **One hosting path, not one per app**, or "extensible" just moves the twelve
   edits somewhere else.
4. **Identity that survives the roster changing**, or every new app corrupts
   the saved session of the ones already installed.

## 3. Principles

1. **The module is the source of truth about itself.** Name, icon, category
   and hosting tier travel inside the `.UNO`, not in a table in the kernel.
2. **Metadata is readable without executing anything.** Probing an app costs
   two small reads and allocates nothing.
3. **One registry, one dispatch path**, covering native, bridged-legacy,
   classic `.UNO`, unoui-class `.UNO` and Python containers.
4. **Identity is a string.** Slot indices remain, as this boot's ordering, and
   nothing durable is ever keyed by one.
5. **Additive and fail-soft in both directions.** An old kernel loads a new
   module; a new kernel loads an old module; a missing or corrupt descriptor
   yields defaults derived from the filename rather than a refusal.
6. **A registration seam, not a central switch** (AGENTS.md §2). Nothing about
   adding an app should require editing a file its lane does not own.

## 4. Part A: the app descriptor (module side)

### 4.1 Where it lives

`UnoModHdr` ends with `unsigned int crc, rsv;`. `rsv` is written as 0 by
`mkuno.py` (`tools/mkuno.py:161`) and never read by the loader. Rename it
**`desc_rva`**: the RVA, inside the module image, of a descriptor block.

This is the compatible choice, and the alternative is not:

- Appending the descriptor after the reloc table fails, because
  `mod_instantiate` requires `sizeof(hdr) + file_size + 4*nreloc == n` exactly
  (`pc64_modload.c:470`). A trailing block would make every new module
  unloadable on every older kernel.
- A `desc_rva` inside the image changes no size arithmetic. Old kernel + new
  module: `rsv` ignored, loads as before. New kernel + old module:
  `desc_rva == 0`, defaults apply.

`mkuno.py convert` must assert `desc_rva + desc_len <= file_size`, so the block
is in the trimmed file image and not in bss where a probe cannot read it.

### 4.2 Format

At file offset `48 + desc_rva`:

```
u32  magic  'UAPP'  (0x50504155)
u16  ver    1
u16  len    total bytes of the block including this 8-byte prologue, <= 1024
char body[len - 8]     LF-terminated "key: value" lines, ASCII
```

Body keys, all optional, unknown keys ignored (that is the extension point):

```
id:     vmgr             stable identity: [a-z0-9._-], <= 15 chars.
                         Default: the filename stem, lowercased.
name:   Appliances       launcher / taskbar / window-title label
short:  Appliances       desktop-icon label (default: name)
icon:   sys              named emblem; unknown name -> PCI_GENERIC
cat:    system           Start-menu section: system | tools | media | games | net | other
rank:   50               sort key within the section (default 100)
flags:  singleton,game   comma list, unknown values ignored
min:    480x320          preferred window size
needs:  net,fs.sys       advisory only; the enforced grant is the signed .MFT
```

Defined `flags` values for v1: `singleton` (a second launch focuses the first),
`hidden` (registered and launchable by id, no icon or menu row: services and
helpers), `game` (fullscreen-preferred, matching `unoapp_is_game`),
`nosession` (never restored by `SHELL.CFG`; the current `app_restorable` rule).

Everything about this is deliberately the same shape as `<APP>.MFT`
(`pc64/UNOSECURE.md` SPEC 5): line-oriented ASCII, additive keys, a version on
line one. One parser idiom, already proven in this codebase, and an author who
has written a manifest already knows the syntax.

### 4.3 Authoring

One macro in the app's source, next to its `UnoUuiApp`:

```c
UNO_APP_DESC("id: vmgr\n"
             "name: Appliances\n"
             "icon: sys\n"
             "cat: system\n"
             "min: 560x380\n");
```

It expands to a `const` struct in section `.unodesc` with the magic, version
and a `sizeof`-computed length, marked `used` so `--exclude-all-symbols` and
`-ffunction-sections` cannot drop it. `mkuno.py convert` locates the section in
the PE headers, validates the prologue and the key syntax, and writes its RVA
into `desc_rva`. **Validation at build time, not at boot**: a typo in a
category name is a build failure, in the same spirit as the `kExports` import
check that "earns its keep" (2026-08-03 entry).

Apps with no macro keep working, with `id` and `name` derived from the
filename.

## 5. Part B: cheap probing (loader side)

New in `pc64_modload.c`, additive, one `KX()` append each:

```c
typedef struct {
    char  id[16], name[32], shortnm[16], icon[16];
    unsigned char  cat, rank, tier;      /* tier: from UnoModHdr.flags */
    unsigned short flags;
    short pref_w, pref_h;
} UnoAppDesc;

/* read a module's descriptor without loading it: two uno_fs_read_at calls,
 * no arena, no relocation, no code executed. 0 = ok, -1 = not a module,
 * 1 = module with no descriptor (out is filled from `path`). */
int uno_mod_desc_read(int vol, const char *path, UnoAppDesc *out);

/* enumerate APPS\ across every volume, newest-wins by id */
int uno_mod_scan(UnoAppDesc *out, char (*file)[16], signed char *vol, int maxn);
```

`uno_mod_desc_read` reads 48 bytes, checks magic/abi, and if `desc_rva` is
non-zero reads at most 1 KB at `48 + desc_rva`. Two sector reads per app. For
comparison, `PYRT.UNO` is 318 KB, which the 2026-07-30 measurement clocked at
~1.1 s of single-sector I/O; the point of the descriptor is that enumeration
never pays that.

`uno_mod_scan` walks volumes in the order `uno_mod_present` already uses
(`APPS\` on every volume, then `EFI\UNODOS\APPS\` on volumes 1..), and
de-duplicates by `id` so an installed copy and a stick copy are one entry. **It
reports over-cap rather than truncating**: `uno_fat_list` caps at 64 entries and
`uno_fs_list_begin` caches 64, and a silently short list would present as "my
app did not install" (the browser size-cap lesson: silent truncation looks like
a broken feature).

The one missing primitive is listing a **subdirectory** through `uno_fs_*`.
`uno_fs_list_begin(vol)` lists the volume root only (`pc64_fs.c:118` passes
`dir = 0`). Both backends can already do it: `uno_fat_list_ex(vol, dir, ...)`
takes a directory, and `uno_efifs_snapshot` (`uefi_main.c:2526`) only needs to
`Open` the subdirectory before iterating instead of using `fs_root` directly.
Filed as a request in §9.

## 6. Part C: the shell registry

Replace the `EX_*` arithmetic with a table built at boot.

```c
#define APPS_MAX 48          /* every [NAPPS] array becomes [APPS_MAX] */

typedef enum { AK_NATIVE, AK_BRIDGE, AK_UUIMOD, AK_CLASSICMOD, AK_PYAPP } app_kind;

typedef struct {
    char   id[16];
    char   name[32], shortnm[16];
    unsigned char kind, icon, cat, rank;
    unsigned short flags;
    char   file[16];                 /* modules: "VMGR.UNO" */
    signed char vol;                 /* modules: where it was found */
    short  ordinal;                  /* AK_NATIVE / AK_BRIDGE: index into the
                                        existing native or bridge tables */
    const UnoUuiApp *iface;          /* AK_UUIMOD: resolved on first open */
    unsigned char tried;             /* load attempted (success or not) */
} app_slot;

static app_slot g_app[APPS_MAX];
static int      g_napps;
```

**Population order is deterministic**: the eight natives, then the five bridged
legacy apps, then discovered modules sorted by `(cat, rank, name)`. Built-ins
therefore keep the positions they have today, and a newly dropped module
appends in a predictable place rather than wherever the FAT directory happened
to put it.

**The `EX_*` constants stop being arithmetic** and become ids looked up once at
registration, for the handful of places that genuinely mean a specific app:
`g_pyapp_slot = app_by_id("pyapp")`, `g_userapp_slot = app_by_id("userapp")`,
`g_browser_slot = app_by_id("browser")`. Everything else stops naming apps
individually.

The seven dispatch sites collapse to one shape each:

```c
static const UnoUuiApp *iface(int a)      /* lazy load, one place */
{
    app_slot *s = &g_app[a];
    if (s->kind != AK_UUIMOD || s->tried) return s->iface;
    s->tried = 1;
    { UnoUuiEntry e = uno_mod_load_uui_at(s->vol, s->file);
      if (e) s->iface = e(0);
      if (s->iface && s->iface->abi != UNO_UUIAPP_ABI) s->iface = 0; }
    return s->iface;
}
```

and then `build`/`opened`/`closed`/`canvas_index`/`key`/`action`/`frame` are
`{ const UnoUuiApp *m = iface(a); if (m && m->key) ... }`. Missing three of
seven becomes impossible, because there is one of each. `app_name`,
`app_short`, `app_icon` and `app_hidden` become field reads. The presence
probes disappear: a module is in the table because the scan found the file.

`pc64_shell_run_user()` gains its missing third branch as a side effect: a
`UNO_MODF_UUI` file that is already registered focuses or opens its own slot,
and one that is not (a module outside `APPS\`) is registered on the spot as a
transient slot. That closes the "cannot be run at all" hole immediately, even
before discovery lands.

## 7. Part D: identity everywhere

**`SHELL.CFG` v3.** Per-app keys carry the id instead of the index:

```
open=files,browser,vmgr
geom.vmgr=40,20,520,380
snap.vmgr=0
min.vmgr=1
desk.vmgr=1
grp.vmgr=2
icon.vmgr=320,140          <- new: icon positions finally persist
```

The reader accepts both. A frozen 24-entry table maps the v2 numeric slots
(`geom14=` and friends) onto ids, so an existing installation restores its
session exactly once and is then written back as v3. The writer only emits v3.
This is the same reasoning already applied to the theme and the display mode in
`session_save`, which are written by name precisely because "the row moved"
must not silently mean something else.

Note the bonus: `g_icon_pos[32]` is in-memory only today, so a dragged desktop
icon has never survived a reboot. Once positions are keyed by id there is
something worth persisting, and it costs one more line per placed icon.

**Automation and docs.** `pc64_shell_app_index_by_id(const char *id)` plus an
`id` field in the `app.msg info` reply. The URC `launch` verb takes an id or a
name as well as an index, and a new `apps` verb lists `id name cat present
open`. `tools/urcui.py` grows `launch_id()` beside `launch_named()`. This is
the fix for two known traps at once: the manual's scenes launch by Start-menu
index and drift when an app is added, and a test that launches
`app_count() - 1` does not fail when an app is appended, it just tests the
wrong app.

**`APPS.CFG`**, beside `SHELL.CFG`, holds user overrides that must never
require touching a module:

```
hide.vmgr=1
name.vmgr=VMs
cat.vmgr=tools
pin.vmgr=1
```

## 8. Part E: what this makes possible later, cheaply

- **Start-menu sections.** `cat:` already exists in the descriptor; the menu
  builder groups by it. The right pane already demonstrates headers that the
  keyboard steps over.
- **Third-party apps.** `FOO.UNO` plus optional `FOO.MFT` in `APPS\`, and it
  appears. No rebuild, no kernel edit, no lane negotiation.
- **Custom icon art.** `icon: file:VMGR.QOI` in the descriptor. The kernel has
  a QOI *encoder* already (`unoauto_screen.c`), so the decoder is the natural
  ~60-line completion of a format the OS already speaks, with no new
  dependency and no PNG. Deliberately **not** v1: the named-emblem path ships
  first, and the syntax leaves room.
- **The classic tier.** `kModFile[APP_NAPPS]` in `pc64_modload.c` is the same
  static roster one tier down. `AK_CLASSICMOD` absorbs it, with the static
  table kept as the fallback when a scan yields nothing (a firmware-SFS boot
  before the subdirectory listing exists, say).
- **Rescan on demand.** `pc64_shell_apps_rescan()` exported, called after a URC
  `push` into `APPS\`, after an install, and from a Control Panel button, with
  an `apps.changed` unoauto hook fired. Installing an app without rebooting is
  then free.

## 9. Requests to other lanes

- **unofs:** `int uno_fs_list_dir(int vol, const char *dir, char (*names)[13],
  int maxn)`, listing a subdirectory on all three backends. `uno_fat_list_ex`
  already takes a directory; `uno_efifs_snapshot` needs to `Open` the subdir
  rather than iterate `fs_root`. Stopgap meanwhile: `uno_fs_fat_index()` plus
  `uno_fat_list_ex` directly, which covers native FAT (an installed system) and
  leaves the pre-detach firmware boot on the static roster.
- **unoautomate:** `launch <id|name>` alongside `launch <n>`, and an `apps`
  verb. Both are additive rows in `GATE[]` and `REMOTE.md`, in the same commit
  as the verb, per AGENTS.md §2.

## 10. Phasing, each phase shippable on its own

| Phase | Content | Gate | Landed |
|---|---|---|---|
| 1 | Descriptor format, `UNO_APP_DESC`, `mkuno.py` support, `uno_mod_desc_read`. Descriptors on all seven uui modules. No shell change. | `tools/appdesc_test.py`; both builds | `c68a2256` |
| 2 | The registry inside `pc64_uui.c`, replacing `EX_*` for the modules and the natives/bridge. Behaviour identical. | `harness.py unoapps` run against master AND the branch, all 8 scenes pixel-compared | `516d3b20` |
| 3 | Discovery: scan `APPS\`, register unknown modules. VMGR appears with zero code written for it. | `tools/appreg_urc.py` | `7d67ae2f` |
| 4 | `SHELL.CFG` v3 + `APPS.CFG` + id-based automation. | `tools/appreg_id_urc.py`, `tools/appreg_v2_urc.py` | `81b2c289` |
| 5 | Categories in the menu, pinning, QOI icons, classic tier. | `tools/qoi_test.py`, `tools/appreg_p5_urc.py` | `0b58b973` |

Phase 3 is the one that was asked for. Phases 1 and 2 are what make it not a
pile of special cases, and phase 4 is what stops it corrupting saved sessions
the first time somebody installs an app.

## 11. Traps this design is deliberately shaped around

- **Never load a module to enumerate it.** The arena is 4 MB and never frees.
- **Never key durable state by slot index.** §7 exists because of this alone.
- **Report a truncated scan.** 64-entry listing caps are real on both backends.
- **One dispatch site per hook.** The three-of-seven bug is a shape problem,
  not an attention problem.
- **Verify the ESP with mtools, not with a QEMU vvfat read-write mount**, which
  is known to corrupt writes on this project.
- **Do not run `gate.sh | tail`**: the pipe throws the exit status away and a
  failed gate reads as a pass.

## 12. Where the plan was wrong, and what implementing it taught

Kept because the corrections are more useful than the parts that were right.

**The descriptor block needed its own SECTION, and the attribute goes on the
declarator.** The plan said "a `const` struct in section `.unodesc`" and left it
there. `__attribute__((section(...)))` written after a struct's closing brace
attaches to the anonymous TYPE, where `section` is silently dropped: the block
lands in `.rdata` with nothing to find it by, and every module reports NO
DESCRIPTOR. Cost one build cycle to find, and it is in the macro's comment now
so nobody rediscovers it.

**A separate section costs a page per module.** `.unodesc` lands before `.bss`
(verified on all seven), so the file image grows by one section alignment page
rather than by the bss gap - about 4 KB for a 67-byte descriptor. Accepted for
determinism: locating the block by section NAME cannot false-positive, whereas
scanning the image for a magic number could.

**The built-in slots did NOT need renumbering, and not renumbering them was the
right call.** The plan described sorting every app by `(cat, rank, name)`. Doing
that would have moved every existing index in one commit, before the id-keyed
persistence of phase 4 existed to absorb it. Built-ins keep their indices;
discovered rows append. The registry got all of its value without the churn.

**`app_restorable` was left exactly as it was** (natives plus the Browser).
Widening it to "anything that is not a host slot" would have been a behaviour
change smuggled into a refactor, and the flag that would justify it
(`UAF_NOSESSION`) has no consumer asking for it yet.

**The hand-written dispatch lists were still wrong when phase 2 landed**, which
is a stronger argument for the registry than the plan made. LOGVIEW was missing
from both the `action` list and the `key` list, and its `canvas_index` case said
`return m->canvas_index();` inside a void function - the same bug the comment
three lines above it described as already fixed for the other three. gcc had
been reporting it as `'return' with a value, in function returning void` the
whole time.

**Sections in the Start menu had to be opt-in.** The plan treated grouping as a
free win once `cat:` existed. It is not free: it REORDERS the menu, and every
harness scene and manual figure that reaches an app by counting `down` presses
depends on the flat order. Default off, with the toggle in the Control Panel,
until those scenes use `launch <id>`.

**QOI arrived in phase 5 rather than "later, cheaply".** It was worth doing
immediately, because a named emblem still requires a case in the kernel and so
is not actually available to an app that arrives on a stick - which is the whole
population this design exists for. Eighty lines, no tables, no allocation, and
the encoder was already in the tree.

**Pinning is not a flag a module may declare.** The plan listed `pin` beside
`hide` and `name` in `APPS.CFG` without saying why it could not also be a
descriptor key. It cannot: an app that could pin itself to the taskbar would,
and the bar would be whatever was installed last rather than what its owner
chose.

**One bug the gates found that reading did not:** an `APPS.CFG` rename renamed
the app in the launcher, on the desktop and on the taskbar, and not in its title
bar, because the module titles its own window from its `UnoUuiApp.name`. The
registry is the authority for what an app is called; the module stays the
authority for everything inside the frame.
