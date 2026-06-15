# uno3d — a portable 3D graphics library for UnoDOS

uno3d lets a 3D application be **written once** and run on any UnoDOS target,
using real 3D hardware where it exists and a software rasteriser where it
doesn't. The same application code (`uno3d_demo.c`) runs unchanged on:

| Backend | Hardware | File | Status |
|---|---|---|---|
| `soft` | none — CPU rasteriser into the framebuffer | `uno3d_soft.c` | ✅ verified on host + PC/DOS |
| `ps2-gs` | PlayStation 2 Graphics Synthesizer (gsKit) | `uno3d_ps2.c` | ✅ verified in PCSX2 @ 60 fps |
| `dc-pvr` | Dreamcast PowerVR2 (KallistiOS) | `uno3d_dc.c` | ✅ verified in Flycast (PVR hardware) |

## The game: UnoDOS Runner

`uno3d_game.c` is a complete little 3D game — an obstacle dodger. You pilot a
ship down a corridor; walls of blocks rush toward you, each with one gap; steer
to line up and pass through, clip a block and you crash. The corridor speeds up;
gap-to-gap jumps are bounded so it stays fair. An attract-mode autopilot plays on
its own until you take the controls (so the game self-demos on every machine).

It is written ONCE against the uno3d API plus an abstract `game_input`; each
platform's ~40-line glue maps real input (DualShock 2 / maple) and presents.
**The same `uno3d_game.c` runs on the hardware-3D consoles + the host:**

| Target | Renderer | 3D hardware? | Verified |
|---|---|---|---|
| Host PC | `soft` | software | ✅ `build/game.png` (autopilot 2000 frames, score 150) |
| **Sony PS2** | `ps2-gs` | GS | ✅ PCSX2 @ 60 fps (`build/runner_ps2_pcsx2.png`) |
| **Sega Dreamcast** | `dc-pvr` | PowerVR2 | ✅ Flycast (`build/runner_dc_flycast.png`) |

```sh
./build.sh host-game   # PC software -> build/game.png
./build.sh ps2-game    # PS2 GS -> build/uno3d-runner-ps2.elf
./build.sh dc-game     # Dreamcast PVR -> build/uno3d-runner-dc.elf
```

### The PC 386+ version is a native UnoDOS app, not a DOS program

UnoDOS is its own bare-metal OS, and a 386 has no 3D hardware, so the PC version
of the game is **[`apps/runner3d.asm`](../apps/runner3d.asm)** — a native UnoDOS
application that talks only to the UnoDOS kernel via `INT 0x80` (set VGA mode,
draw, read events). It uses the same game design and the same perspective math,
hand-written in 8086 assembly (UnoDOS apps are flat NASM `.BIN`s, not C, so the C
`uno3d` library can't be reused there — and its 256 KB buffers wouldn't fit a
64 KB app segment anyway). Because the camera never rotates, each wall block
projects to an axis-aligned rectangle, so the whole solid 3D corridor is drawn
with the kernel's filled-rect API in painter's order — no FPU, no framebuffer
poking. It assembles for the 8088 (runs on every UnoDOS machine; snappy on a
386+). Verified booting the UnoDOS floppy and launching the app from the desktop
in QEMU (`build/run3d_qemu.png`). Build: it ships in the OS image — `make
build/unodos-144.img` from the repo root puts `RUN3D.BIN` on the floppy and the
launcher auto-discovers it.

> **Yes, the PS2 does hardware-accelerated 3D.** The UnoDOS PS2 port normally
> uses the GS only as a blitter; this library drives it as the hardware triangle
> rasteriser it is — hardware z-buffer, hardware gouraud — at 60 fps.

## Architecture

uno3d is split into a **portable front-end** and a **per-platform backend**, so
adding a machine never touches the core or any application.

```
  application  (uno3d_demo.c)            <- write once, names no platform
        |  u3d_* API (uno3d.h)
  front-end    (uno3d.c)                 <- math, transform, projection,
        |                                   back-face cull, near clip  (portable)
        |  u3d_backend vtable (uno3d_backend.h)
  backend      (uno3d_soft/ps2/dc .c)    <- clear / rasterise tri / flush / present
```

The front-end transforms model-space geometry into **screen-space triangles**
(`u3d_stri`: pixel x/y, depth 0..1, gouraud colour) and hands them to the active
backend. A backend therefore only ever does four things — clear, draw one
triangle, flush, present — which map cleanly onto every triangle pipeline (a CPU
rasteriser, gsKit prims, PVR vertex lists, GX display lists, a D3D/GL draw).

## Adding a platform (PS3, PC, GameCube, Xbox, …)

The design is built for this. To bring uno3d to a new machine:

1. **Write `uno3d_<plat>.c`** implementing the six `u3d_backend` functions
   (`init/shutdown/clear/tri/flush/present`) over that machine's 3D API, and
   define `const u3d_backend u3d_backend_<plat> = { … }`.
2. **Add one `extern`** for it in `uno3d_backend.h`.
3. **Link that file** into the port's build and have the port's glue call
   `u3d_use_backend(&u3d_backend_<plat>)` before `u3d_init()`.

No change to `uno3d.c` or to any application. Sketches for the planned targets:

- **PS3** (`u3d_backend_ps3`): RSX via libgcm/librsx — compile a vertex/fragment
  shader pair that just passes through position + colour; `tri` appends to a
  vertex buffer, `flush` kicks the command buffer, `present` flips.
- **PC, Pentium+** (`u3d_backend_pc`): a Direct3D/OpenGL backend for machines
  with a GPU; on a bare Pentium with no 3D card, simply select `u3d_backend_soft`
  at runtime instead (backends are chosen by pointer, so detection is trivial).
- **GameCube** (`u3d_backend_gc`): GX display lists via libogc/devkitPPC.
- **Xbox (original)** (`u3d_backend_xbox`): Direct3D8 / nxdk.

Backends can coexist in one build and be picked at runtime — e.g. a PC build
links both the GPU backend and `soft`, probes for hardware, and falls back.

## API (uno3d.h)

```c
void u3d_use_backend(const u3d_backend *be);   /* pick a backend (glue) */
void u3d_init(int w, int h);
void u3d_perspective(float fov_deg, float aspect, float znear, float zfar);
void u3d_load_identity(void);
void u3d_translate(float x, float y, float z);
void u3d_rotate_x/y/z(float deg);
void u3d_begin(unsigned char r, unsigned char g, unsigned char b);  /* clear */
void u3d_triangles(const u3d_vert *verts, int tri_count);           /* draw */
void u3d_end(void);                                                 /* flush */
void u3d_present(void);                                             /* to screen */
```

Right-handed, +Y up, -Z into the screen; column-major matrices (OpenGL
conventions). Geometry is gouraud-shaded and depth-tested; back faces culled.

## Build & verify

```sh
./build.sh host        # software backend on the PC -> build/cube.png   (verifiable here)
./build.sh ps2         # PS2 GS (PS2SDK+gsKit)       -> build/uno3d-cube-ps2.elf
./build.sh dc          # Dreamcast PVR (KallistiOS)  -> build/uno3d-cube-dc.elf
```

`uno3d.c`, `uno3d_demo.c` and `uno3d.h` are byte-identical across all three —
only the backend file and a ~30-line platform `main` differ. That is the proof
the abstraction works: one application, three rasterisers, two of them real 3D
hardware.

## Files

| File | Role |
|---|---|
| `uno3d.h` | public application API |
| `uno3d_backend.h` | the `u3d_backend` vtable + the known-backend externs (the extension point) |
| `uno3d_int.h` | internal screen-space vertex types shared by core + backends |
| `uno3d.c` | portable pipeline: math, transform, projection, cull, clip, dispatch |
| `uno3d_soft.c` | software rasteriser backend (universal fallback) |
| `uno3d_ps2.c` | PlayStation 2 GS backend (gsKit) |
| `uno3d_dc.c` | Dreamcast PowerVR2 backend (KallistiOS) |
| `uno3d_demo.c` | the spinning-cube demo (write-once application #1) |
| `uno3d_game.c` / `uno3d_game.h` | UnoDOS Runner, the 3D game (write-once application #2) |
| `host_demo.c` / `host_game.c` | host glue (software backend → PPM/PNG) |
| `uno3d_ps2_main.c` / `uno3d_ps2_game.c` | PS2 glue (GS + DualShock 2) |
| `uno3d_dc_main.c` / `uno3d_dc_game.c` | Dreamcast glue (PVR + maple) |
| `uno3d_pc_game.c` / `pc/fb.h` | PC 386+ DOS glue (software + VGA mode 13h + arrow keys) |
| `Makefile.ps2` / `Makefile.dc` / `Makefile.dos` / `build.sh` | builds |
| `tools/dc_run.sh` / `tools/dos_run.sh` | headless Flycast / DOSBox capture rigs |
