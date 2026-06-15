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
- **loadable module** (host `.so`, native `.uno`): exports `uno_app_main`.
- **linked-in module** (native single-binary builds with no dynamic linker):
  compiled with `-DUNO_APP_SYM=uno_app_main_<name>` so all 11 entries coexist;
  the platform modload registry resolves the distinct symbol.

## Per-platform `uno_load_module` (load from storage)

| Platform | Hook file | Storage path | Status |
|---|---|---|---|
| Host shim (WSL gcc) | `host_modload.c` | `apps_store/appNN.so` via `dlopen` | **REAL & HOST-RUN-VERIFIED** — genuine runtime load + pointer dispatch of all 11, screenshot-rendered |
| PS2 (EE) | `ee_modload.c` | `mc0:/UnoDOS/Apps/appNN.uno` via libmc File Mgr | BUILD-WIRED; storage read REAL on hw; EE-overlay relocate = TODO (`UNO_EE_OVERLAY`) — registry links the modules in meanwhile |
| Dreamcast | `dc_modload.c` | `/cd/UNODOS/APPS/APPNN.UNO` via fs_iso9660 | BUILD-WIRED; storage read REAL; `elf_load` relocate feasible in KOS = TODO (`UNO_DC_ELF`) |
| Mac | `mac_modload.c` | `APPNN.UNO` on the FAT12 PC volume (or 'CODE' resource) | BUILD-WIRED; storage read REAL via `fat12_read`; CODE-resource PIC relocate needs Retro68 = TODO |

## Verified (this environment = WSL gcc 13; no cross toolchains / emulators)

`./build_real.sh` in `ps2/` and `dreamcast/` (the REAL core, not the demo):
1. **refactors** `unodos.c` from the pristine `tools/unodos_orig_*.c` via
   `tools/refactor_core.py` (reproducible — regenerated every build);
2. **builds the REAL core** with **zero app code** (`nm build/unodos.o` shows no
   `pacman_*`/`dostris_*`/`theme_*`/`sysinfo_*`/`files_*`/… symbols — the build
   FAILS if any leak), `-rdynamic` so modules resolve it;
3. builds **all 11 apps as separate `.so`** modules into `apps_store/` (each
   `nm -D` shows exactly one `uno_app_main`);
4. runs: the core `dlopen`s each module from storage, dispatches through the
   `AppInterface` pointers, renders `shots/real_*.png` (desktop + every app;
   PS2 640×448, DC 640×480).  Games/animations are driven through the module's
   `key`/`tick` pointers so the shots show played states.

## Deviations / stubbed (honest)

- **Native targets are BUILD-WIRED, NOT COMPILED here** — no Retro68/Executor
  (Mac), ee-gcc/PCSX2 (PS2), sh-elf/Flycast (DC) in this environment.  The
  Makefiles/CMakeLists compile the app-free core + the platform modload + the 11
  modules and package the `.uno` images to each platform's real storage; the
  modload registries link the modules in.  The relocate-and-call overlay
  (`UNO_EE_OVERLAY`/`UNO_DC_ELF`/Mac CODE-resource) is the only piece behind a
  flag, and is the part that genuinely needs those toolchains.
- The host path is the real architectural proof and is fully exercised.
