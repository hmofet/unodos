# pc64 dynamic apps — the `.UNO` module format

Since the decoupling milestone M2, **no app code is linked into the pc64
kernel image**. Every app ships as a `.UNO` file and is loaded from storage
on first launch — the modern-PC analogue of the C64 port loading `.PRG` apps
through its JMP table. Two build-time asserts enforce it: the kernel image
must contain no `uno_app_main_*` symbol, and every module import must appear
in the kernel export table.

## Where modules live

| Layout | Path |
|---|---|
| dev / USB stick | `APPS\<NAME>.UNO` (volume root) |
| installed system | `EFI\UNODOS\APPS\<NAME>.UNO` |

The loader (`pc64_modload.c`) scans every mounted volume for the first path,
then volumes 1.. for the second — the same convention the font loader uses.
The ESP installer copies the `APPS` directory; the whole-disk installer
clones it implicitly. A missing module degrades gracefully: the window shows
"module not found: APPS\\<NAME>.UNO".

The 6 module apps: Dostris, Pac-Man, OutLast, Tracker, Paint, Network.
(**Music left this list**: it is now a native unoui app, `pc64_music.c`. The
legacy module drew itself with the Mac-Toolbox primitives against a fixed
four-colour palette — that is where its hardcoded blue background came from,
and why no theme change ever affected it. See AUDIO.md.) (Runner3D and the Browser are native shell canvases — they drive
uno3d / the HTML engine directly and have no AppInterface counterpart.
The classic games' native canvases in `pc64_games.c` are no longer routed;
the `.UNO` bridge versions run instead, so ALL apps load from storage.)

## Container format (`tools/mkuno.py`)

A `.UNO` is a flattened PE32+ DLL: a 48-byte header, the section image laid
out at its RVAs (trailing zeros trimmed; bss stays virtual), then the two
tables the in-kernel loader needs:

- **relocations** — u32 RVAs of u64 cells to rebase by `base - pref_base`
  (extracted from the PE `.reloc` DIR64 entries),
- **imports** — the `.unoimp` section: 32-byte `{char name[24]; u64 slot;}`
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

Adding an export: add a `KX(name)` line in `pc64_modload.c` — build.sh greps
that table for the import check, and the kernel link fails on a typo.

## Verification

- `python3 harness.py unoapps` — boots the shell in QEMU and opens all 7
  module apps through the Start menu, one screenshot each.
- `python3 tools/install_test.py` — end-to-end: install to a disk (both
  modes), reboot from the installed disk alone, verify the module files on
  the installed ESP offline (mtools) and open a `.UNO` app on the installed
  system.
- Negative: hide `APPS/` and any app window shows the missing-module notice.
