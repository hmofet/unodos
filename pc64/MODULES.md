# pc64 dynamic apps, the `.UNO` module format

Since the decoupling milestone M2, **no app code is linked into the pc64
kernel image**. Every app ships as a `.UNO` file and is loaded from storage
on first launch, the modern-PC analogue of the C64 port loading `.PRG` apps
through its JMP table. Two build-time asserts enforce it: the kernel image
must contain no `uno_app_main_*` symbol, and every module import must appear
in the kernel export table.

## Where modules live

| Layout | Path |
|---|---|
| dev / USB stick | `APPS\<NAME>.UNO` (volume root) |
| installed system | `EFI\UNODOS\APPS\<NAME>.UNO` |

The loader (`pc64_modload.c`) scans every mounted volume for the first path,
then volumes 1.. for the second, the same convention the font loader uses.
The ESP installer copies the `APPS` directory; the whole-disk installer
clones it implicitly. A missing module degrades gracefully: the window shows
"module not found: APPS\\<NAME>.UNO".

The 6 classic-tier module apps: Dostris, Pac-Man, OutLast, Tracker, Paint,
Network. Two more ship in the unoui-class tier (flags bit 0, full desktop
citizens): **Studio** (the IDE) and **Photos** (the image viewer, carrying
the whole unomedia decoder library inside the module - see IMAGES.md).
(**Music left this list**: it is now a native unoui app, `pc64_music.c`. The
legacy module drew itself with the Mac-Toolbox primitives against a fixed
four-colour palette, that is where its hardcoded blue background came from,
and why no theme change ever affected it. See AUDIO.md.) (Runner3D and the Browser are native shell canvases, they drive
uno3d / the HTML engine directly and have no AppInterface counterpart.
The classic games' native canvases in `pc64_games.c` are no longer routed;
the `.UNO` bridge versions run instead, so ALL apps load from storage.)

## The app descriptor: what a module says about itself

A module carries the metadata the launcher needs, so **dropping a `.UNO` into
`APPS\` is the whole install**: it gets a desktop icon, a Start-menu row, a
taskbar chip and a window with no kernel edit and no rebuild. One macro, beside
your `UnoUuiApp`:

```c
#include "uno_appdesc.h"

UNO_APP_DESC("id: myapp\n"          /* stable identity - EVERYTHING durable   */
             "name: My App\n"       /*   is keyed by it, never by slot index  */
             "short: MyApp\n"       /* desktop-icon label (defaults to name)  */
             "icon: tools\n"        /* a named emblem, or file:MYAPP.QOI      */
             "cat: tools\n"         /* system|net|tools|media|games|other     */
             "rank: 50\n"           /* sort within the section                */
             "min: 480x320\n");     /* preferred window size                  */
```

`mkuno.py convert` validates this at BUILD time - an unknown category, an
unknown flag, a bad id, a malformed `min`, a repeated key or a second block in
one module are all build failures. An unknown KEY is deliberately accepted and
ignored at runtime; that is the format's extension point.

**The attribute goes on the declarator.** `__attribute__((section(...)))`
written after a struct's closing brace attaches to the anonymous TYPE, is
silently dropped, and the block vanishes into `.rdata` with nothing to find it
by. The macro gets this right; a hand-rolled one may not.

### How the shell reads it, and why the format is shaped this way

`UnoModHdr.desc_rva` (the header word formerly called `rsv`) points at the block
inside the module image. Reading it is **two `uno_fs_read` calls and executes
nothing**: 48 bytes for the header, then at most 1 KB at `48 + desc_rva`. That
constraint drives the whole design - the module arena is 4 MB and `mod_free`
only unwinds the most recent allocation, so enumerating apps by loading them is
not available at any price, and a 300 KB module is ~1.1 s of single-sector I/O
besides.

Compatible in both directions: an older kernel ignores `desc_rva` and loads a
new module exactly as before; a newer kernel meeting an old module sees 0 and
derives an id and name from the filename. (Appending the block after the reloc
table was the obvious alternative and is wrong: `mod_instantiate` requires
`48 + file_size + 4*nreloc == n` EXACTLY.)

### Shipping your own icon

`icon: file:MYAPP.QOI` names a QOI file beside the module. QOI because the shell
draws an app's icon *before* it would load a byte of that app's code, so the
decoder is in the kernel (`pc64_qoi.c`) - and the OS already encodes QOI for
remote desktop, so this is the other half of a format it speaks. 32x32 RGBA,
alpha as a 1-bit key. Author one with `tools/mkicon.py`. Twelve custom emblems
fit; past that an app keeps its named emblem, so a bad or absent icon costs a
plainer icon and nothing else.

### Which tiers get a desktop slot

**unoui-class** modules (flags bit 0) are full desktop citizens. **Classic**
KernelApi modules get a row too if they carry a descriptor, but they run in the
shared user slot, so only one can be resident at a time - a limit of that tier,
and the reason to write new apps to the unoui one. PYRT, `.PY` containers and
`\DRIVERS\` modules deliberately get no row: an icon for any of them would be an
icon that opens nothing.

### The user's last word

`APPS.CFG`, beside `SHELL.CFG`, overrides what a module declared:
`name.<id>=`, `short.<id>=`, `cat.<id>=`, `rank.<id>=`, `hide.<id>=1`,
`pin.<id>=1`. Renaming or hiding an app never means editing somebody's `.UNO`.
Pinning is not a flag a module may declare about itself.

## Container format (`tools/mkuno.py`)

A `.UNO` is a flattened PE32+ DLL: a 48-byte header, the section image laid
out at its RVAs (trailing zeros trimmed; bss stays virtual), then the two
tables the in-kernel loader needs:

- **relocations**: u32 RVAs of u64 cells to rebase by `base - pref_base`
  (extracted from the PE `.reloc` DIR64 entries),
- **imports**: the `.unoimp` section: 32-byte `{char name[24]; u64 slot;}`
  records the loader resolves against the kernel export table `kExports[]`
  in `pc64_modload.c`.

Imports are **functions only**. Each undefined symbol in the app object
becomes a one-instruction thunk `jmp *slot(%rip)` in a generated assembly
file, so there are no import libraries at build time and no PE machinery at
runtime. Loading = read + CRC32 check + `AllocatePages(EfiLoaderCode)`
(executable under firmware NX policies) + copy + rebase + resolve. After the
M3 firmware detach, `AllocatePages` is gone: loads come from a 1.5 MB
executable arena the loader reserves (same memory type) right before
`ExitBootServices`.

## Build pipeline (in `build.sh`, per app)

```
cc app.c -> app.o
nm -u app.o -> app.syms                    # imports; checked against kExports
mkuno.py thunks app.syms -> thunks.s       # jmp-thunks + .unoimp records
cc -shared -nostdlib -e uno_app_main_<app> app.o thunks.o -> app.dll
mkuno.py convert app.dll -> APPS/<NAME>.UNO
```

Adding an export: add a `KX(name)` line in `pc64_modload.c`: build.sh greps
that table for the import check, and the kernel link fails on a typo.

## Verification

- `python3 tools/appdesc_test.py`: mkuno's validator rejects what it promises
  to, and every shipped `.UNO`'s descriptor parses the same way an independent
  re-implementation of the kernel's reader parses it.
- `python3 tools/qoi_test.py`: compiles the kernel's QOI decoder with the host
  gcc and round-trips it against `mkicon.py`'s encoder, plus six malformed
  inputs it must refuse.
- `UNO_DEBUG=1 ./build.sh && python3 tools/appreg_urc.py`: a module with no
  compiled-in slot (`APPS\VMGR.UNO`) gets a row and opens. Also
  `appreg_id_urc.py` (launch by id, `SHELL.CFG` v3), `appreg_v2_urc.py` (a v2
  session file still restores and migrates), `appreg_p5_urc.py` (`APPS.CFG`
  overrides, pinning, a shipped icon).
- `UNO_DEBUG=1 ./build.sh && python3 harness.py unoapps`: reads the roster with
  `apps list` and opens **every** row by its own id, one screenshot each, named
  `shots/uno_<id>.png`. It asserts the window that appeared is that app's before
  keeping the shot, so a wrong shot fails rather than being filed under the
  wrong name. Rewritten 2026-08-07: it used to count `down` presses from the
  Start menu and had been off by one since UnoAmp joined the natives, filing
  UnoAmp as `dostris` and Runner3D as `network` without ever failing.
- `python3 tools/install_test.py`: end-to-end: install to a disk (both
  modes), reboot from the installed disk alone, verify the module files on
  the installed ESP offline (mtools) and open a `.UNO` app on the installed
  system.
- Negative: hide `APPS/` and any app window shows the missing-module notice.
