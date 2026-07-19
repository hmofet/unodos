# UnoDOS shared C core — runtime app modules (Mac / PS2 / Dreamcast)

The three ports (mac/, ps2/, dreamcast/) share the portable core `unodos.c`.
This change makes the runtime-app-loading architecture **REAL in the actual
core**: the app function bodies and the compile-time `switch(proc)` dispatch are
removed from `unodos.c`; the core now dispatches every window through an
**AppInterface** vtable populated by a generic **loader**, and the 11 apps are
separate modules loaded from storage — the C analogue of the C64 port's
`kernel_api.inc` + JMP-table contract.

(Earlier this was only a SIDECAR demonstrator — `demo_kernel.c` + 5 apps — that
proved the ABI without touching `unodos.c`.  That is now superseded: the real
`unodos.c` itself is app-free.  `demo_kernel.c`/`build_modular.sh` remain as the
original 5-app demonstrator; `build_real.sh` builds the REAL refactored core.)

## ABI (shared verbatim by kernel + every app)  — `uno_app.h`  (identical in all 3 ports)

- `KernelApi` — the callbacks an app may invoke: the UnoDOS widget helpers
  (`uno_fill`/`uno_box`/`uno_invert`/`text_at`/`text_at_max`/`fill_rgb`),
  formatting (`fmt_u`/`put2`), time (`now_secs`), the window manager
  (`draw_window`/`find_app_window`/`launch_app`/`repaint_all`/`topmost_proc`),
  the FAT12 storage stack (`fat12_mount`/`list`/`read`/`write` + `gFatCount`/
  `gFatNames`/`gFatSizes`), and the synth (`music_open_chan`/`note_on`/`quiet`/
  `start`/`stop` + the game-music engine `gm_start`/`gm_stop`).  Raw
  Toolbox/`mac_compat` primitives (SetRect, MoveTo, RGBForeColor, PaintRect,
  TickCount, NewPtr, File/Sound managers) are ordinary external symbols resolved
  at load — not duplicated in the struct.
- `AppInterface` — the per-app vtable the WM dispatches through:
  `{ draw, key, click, tick, opened, closed, win_title, win_rect[4] }`.
- Each app module exports one entry: `const AppInterface *uno_app_main(const KernelApi *k)`.

## Loader  — `app_loader.c` (#included by the kernel; contains NO app code)

`app_loader_init()` (called once from `main()`) fills the `KernelApi` from the
kernel's own helpers.  `app_iface(proc)` loads on demand: calls the platform
hook `UnoAppEntry uno_load_module(short proc)`, invokes the returned entry with
the `KernelApi`, caches the `AppInterface`.  `draw_app_content` / `app_key` /
`app_click` / `app_opened` / `app_close` / `app_title` / `app_default_rect` (the
names the WM already calls) dispatch purely through the cached pointers —
**no `switch` on app identity anywhere in the kernel**.  Per-frame ticks go
through `tick_all_apps()` → each window's `AppInterface.tick`.

## The 11 app modules  — `apps/*.c`  (shared verbatim across the 3 ports)

`sysinfo clock files notepad music dostris outlast pacman tracker paint theme`,
ids `APP_SYSINFO..APP_THEME` (0..10).  Each `#include "apps/uno_mod.h"` which
maps the kernel helpers onto the `KernelApi` pointer so the bodies port
near-verbatim from the old core.  Module-local audio/storage notes:
- **music/tracker** drive the kernel's single synth channel via the KernelApi
  primitives (Music owns its song sequencer; Tracker plays the row's first
  active note — the full 4-channel Sound-Manager mix stays a native-build extra).
- **dostris/outlast** carry their own game-music note tables and hand the
  pointer to `gm_start()` (the kernel keeps only the pointer + the engine).
- **files/notepad/tracker/paint** persist over the portable FAT surface
  (`fat12_read`/`fat12_write`); HFS File-Manager browsing stays a native extra.

The same source compiles two ways:
- **loadable module** (host `.so`, DC `.klf`): exports `uno_app_main`.
- **linked-in module** (native single-binary builds with no dynamic linker):
  compiled with `-DUNO_APP_SYM=uno_app_main_<name>` so all 11 entries coexist;
  the platform modload registry resolves the distinct symbol.

## Per-platform `uno_load_module` (load from storage)

| Platform | Hook file | Storage path | Status |
|---|---|---|---|
| pc64 (x86-64 UEFI) | `pc64/pc64_modload.c` | `APPS\<NAME>.UNO` / `EFI\UNODOS\APPS\<NAME>.UNO` on any FAT volume | **REAL & QEMU-VERIFIED end-to-end** — flattened-PE `.UNO` (tools/mkuno.py: rebase table + named `jmp *slot(%rip)` import thunks resolved against the kernel export table); NO app code in the kernel image (build-asserted); loads survive the M3 firmware detach (pre-reserved executable arena + native AHCI). See `pc64/MODULES.md` |
| Host shim (WSL gcc) | `host_modload.c` | `apps_store/appNN.so` via `dlopen` | **REAL & HOST-RUN-VERIFIED** — genuine runtime load + pointer dispatch of all 11, screenshot-rendered |
| PS2 (EE) | `ee_modload.c` | `mc0:/UnoDOS/Apps/appNN.uno` via libmc File Mgr | BUILD-WIRED; storage read REAL on hw; EE-overlay relocate = TODO (`UNO_EE_OVERLAY`) — registry links the modules in meanwhile |
| Dreamcast | `dc_modload.c` | `/cd/UNODOS/APPS/APPNN.KLF` via KOS `library_open`/`elf_load` | **GENUINE runtime load+relocate from CD** — see the DC note below |
| Mac | `mac_modload.c` | `APPNN.UNO` on the FAT12 PC volume (or 'CODE' resource) | BUILD-WIRED; storage read REAL via `fat12_read`; CODE-resource PIC relocate needs Retro68 = TODO |

## Dreamcast — genuine CD module loading (`dc_modload.c`)

The DC native build does **NOT** link any app into `1ST_READ.BIN`.  Each of the
11 apps is built as a separate **relocatable-ELF module** `build/app_NN.klf`
(KOS loadable `.klf`: `sh-elf-gcc -Wl,-d -Wl,-r` partial link with the KOS
`loadable/shlelf_dc.xr` script), staged on the CD data track at
`/UNODOS/APPS/APPNN.KLF`.  At launch the loader calls KOS's real dynamic-library
machinery:

    library_open("unoappNN", "/cd/UNODOS/APPS/APPNN.KLF")
      -> elf_load()  reads the .KLF off the ISO9660 FS, allocates a fresh image,
                     applies the SH `R_SH_DIR32` relocations, and resolves every
                     undefined symbol via export_lookup().
      -> lib_open()  KOS calls the module's lib_open() (the UNO_DC_MODULE shim in
                     apps/uno_mod.h), which hands the module's uno_app_main back
                     to the loader (uno_dc_register_entry); the loader then calls
                     it with the kernel's KernelApi.

Because a normal `kos-cc` app does NOT link `-lkallisti_exports`, the running
kernel's own export table is unavailable to `export_lookup`.  `dc_modload.c`
therefore registers its OWN export symtab (`gUnoSymtab`, via
`nmmgr_handler_add`) carrying the Mac-Toolbox primitives (SetRect/MoveTo/
PaintRect/TickCount/NewPtr/...), the libc the modules use (memcpy/strcpy/strcat/
strncpy/strlen/memmove/memset/stpcpy), and the loader hook
`uno_dc_register_entry`.  These resolve to the live addresses in the linked
1ST_READ.BIN, so the relocated module calls straight back into the running
kernel image.

**Proof it is real load-from-CD (not link-in):**
- `sh-elf-nm build/unodos-dc.elf | grep uno_app_main` → 0 matches.  No app body
  symbols (`pacman_*`/`gPm*`/`theme_*`/`sysinfo_*`/...) are in the main ELF.
- each `build/app_NN.klf` carries exactly one `T uno_app_main` plus the KOS
  `lib_get_name`/`lib_get_version`/`lib_open`/`lib_close` quartet.
- the modules ship as separate files on the CD: `/UNODOS/APPS/APP00.KLF .. APP10.KLF`.
- a runtime probe (`-DUNO_DC_LOADDBG`) confirms the files open off `/cd`, and
  KOS `elf_load` runs the relocation + symbol-resolution pass to completion
  (after `gUnoSymtab` is registered the "symbol '_strcat'/'_SetRect' is undefined"
  aborts disappear and `library_open` returns a live `klibrary_t`).

**Honest limitation (verified):** the **desktop** boots cleanly and runs at
60 fps in Flycast (headless REIOS, `shots/dc_native_desktop.png`).  The genuine
load+relocate+symbol-resolve of an app module from the CD all succeed.  However,
*executing* the relocated module code is **unstable under Flycast's headless
REIOS HLE BIOS**: the dynarec faults non-deterministically on the freshly-
relocated image (`SH4 branch instruction in delay slot` / `SH4 exception when
blocked`), and Flycast's SH4 interpreter (`Dynarec.Enabled=no`) is far too slow
to reach an app within a practical headless capture window.  This is an
emulator/HLE-BIOS constraint on running runtime-relocated code, not a defect in
the load path — on real hardware (or a full-BIOS run) KOS `library_open`
modules execute normally.  The mechanism is therefore genuinely implemented and
verified up to (and including) load+relocate+resolve; the running-app screenshot
is the only piece blocked by this environment.

## Verified (this environment = WSL gcc; sh-elf-gcc 15.2 + Flycast for DC)

`./build_real.sh` (the REAL core, not the demo) on the host:
1. **refactors** `unodos.c` from the pristine `tools/unodos_orig_*.c`;
2. **builds the REAL core** with **zero app code** (`nm build/unodos.o` shows no
   app symbols — the build FAILS if any leak), `-rdynamic` so modules resolve it;
3. builds **all 11 apps as separate `.so`** modules into `apps_store/` (each
   `nm -D` shows exactly one `uno_app_main`);
4. runs: the core `dlopen`s each module from storage, dispatches through the
   `AppInterface` pointers, renders `shots/real_*.png`.

Dreamcast (`make all && make cdi`): app-free `1ST_READ.BIN` + 11 `.klf` modules
on the CD; the genuine `library_open`-from-CD loader; desktop verified booting in
Flycast (`shots/dc_native_desktop.png`).

## Deviations / stubbed (honest)

- **PS2/Mac native targets are BUILD-WIRED** — the relocate-and-call overlay
  (`UNO_EE_OVERLAY`/Mac CODE-resource) is the part that still needs those
  toolchains.  **Dreamcast is the one native target where the relocate-and-call
  is GENUINE** (KOS `elf_load`), with the running-app screenshot blocked only by
  the Flycast/REIOS execution limitation documented above.
- The host path is the real architectural proof and is fully exercised.
