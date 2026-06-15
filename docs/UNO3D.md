# Uno3D — the UnoDOS portable 3D graphics library

Uno3D is a tiny 3D graphics library that lets a 3D application be **written once**
and run on every UnoDOS target — using real 3D hardware where it exists and a
software rasteriser where it doesn't. It lives in [`uno3d/`](../uno3d/).

The same application code drives:

| Backend | Hardware | Where |
|---|---|---|
| `soft` | none — CPU rasteriser into a framebuffer | every port (universal fallback) + host PC |
| `ps2-gs` | PlayStation 2 Graphics Synthesizer (gsKit) | hardware-accelerated 3D |
| `dc-pvr` | Dreamcast PowerVR2 (KallistiOS) | hardware-accelerated 3D |

> **The native UnoDOS-x86 case is special** — see [UnoDOS x86 (a native app, not
> the C library)](#unodos-x86-a-native-app-not-the-c-library) at the end. The
> bare-metal UnoDOS OS runs 16-bit assembly apps, not C, so its 3D game is a
> hand-written native app that uses the same *design* over the kernel's own
> graphics API rather than this C library.

---

## 1. The idea: one pipeline, swappable rasteriser

Uno3D splits a 3D program into two halves:

```
  your application  (e.g. uno3d_game.c)      <- written once, names no platform
        |  u3d_* API  (uno3d.h)
  portable front-end  (uno3d.c)              <- matrix math, transform, projection,
        |                                       back-face cull, near-plane clip
        |  u3d_backend vtable  (uno3d_backend.h)
  per-platform backend (uno3d_soft/ps2/dc.c) <- clear · rasterise triangle · flush · present
```

The **front-end** is identical everywhere. It transforms your model-space
triangles through the current model-view and projection matrices, divides by w,
maps to the screen, culls back faces, drops triangles behind the near plane, and
hands the survivors — now **screen-space triangles** — to the active backend.

A **backend** only ever does four things: clear the frame, rasterise one
screen-space triangle, flush, present. That maps cleanly onto a CPU rasteriser,
gsKit primitives, PVR vertex lists, or (future) a GPU draw call. Picking a
backend is a single call, by pointer, so it can even be chosen at runtime.

---

## 2. Quick start

A complete spinning-cube program ([`uno3d_demo.c`](../uno3d/uno3d_demo.c) is the
real one):

```c
#include "uno3d.h"
#include "uno3d_backend.h"

static const u3d_vert tri[3] = {        /* one triangle, model space + RGB */
    { -1, -1, 0,  255,0,0 },
    {  1, -1, 0,  0,255,0 },
    {  0,  1, 0,  0,0,255 },
};

void render_one_frame(float angle, int w, int h)
{
    u3d_begin(0, 0, 40);                          /* clear to dark blue */
    u3d_perspective(60.0f, (float)w/h, 0.1f, 100.0f);
    u3d_load_identity();
    u3d_translate(0, 0, -4);                       /* push it in front of the camera */
    u3d_rotate_y(angle);
    u3d_triangles(tri, 1);                         /* 1 triangle */
    u3d_end();
}
```

The platform glue (a tiny `main`) picks a backend, sets up the viewport, and
presents each frame:

```c
u3d_use_backend(&u3d_backend_soft);   /* or _ps2 / _dc */
u3d_init(W, H);
for (;;) {
    render_one_frame(angle, W, H);
    u3d_present();
    angle += 1.0f;
}
```

---

## 3. API reference (`uno3d.h`)

### Types

```c
typedef struct { float x, y, z; } u3d_vec3;
typedef struct { float m[16]; }   u3d_mat4;   /* column-major */

typedef struct {                  /* an application vertex */
    float x, y, z;                /* model-space position */
    unsigned char r, g, b;        /* per-vertex colour (gouraud) */
} u3d_vert;
```

### Backend selection (platform glue)

| Call | Meaning |
|---|---|
| `void u3d_use_backend(const u3d_backend *be)` | choose the rasteriser; call **before** `u3d_init`. Backends: `u3d_backend_soft`, `u3d_backend_ps2`, `u3d_backend_dc` (declared in `uno3d_backend.h`). |
| `const char *u3d_backend_name(void)` | active backend name (`"soft"`, `"ps2-gs"`, `"dc-pvr"`). |
| `int u3d_backend_caps(void)` | capability bits: `U3D_CAP_HW`, `U3D_CAP_ZBUFFER`, `U3D_CAP_GOURAUD`, `U3D_CAP_TEXTURE`. |

### Lifecycle

| Call | Meaning |
|---|---|
| `void u3d_init(int w, int h)` | bring the renderer/hardware up for a `w`×`h` viewport. |
| `void u3d_shutdown(void)` | tear it down. |
| `void u3d_begin(unsigned char r,g,b)` | start a frame; clear colour + depth. |
| `void u3d_end(void)` | finish geometry (flush to the rasteriser/hardware). |
| `void u3d_present(void)` | put the finished frame on screen (GS flip / PVR scene / your blit). |

### Transform (a one-deep model-view + a projection)

| Call | Meaning |
|---|---|
| `void u3d_perspective(float fov_deg, float aspect, float znear, float zfar)` | set the projection. |
| `void u3d_load_identity(void)` | reset the model-view to identity. |
| `void u3d_translate(float x, float y, float z)` | post-multiply a translation. |
| `void u3d_scale(float x, float y, float z)` | post-multiply a scale. |
| `void u3d_rotate_x/y/z(float deg)` | post-multiply a rotation about that axis. |

Transforms compose like OpenGL: the **last** call you make is applied **first**
to the vertex. So `translate; rotate_y; u3d_triangles(...)` rotates the model
about its own origin, *then* moves it.

### Geometry

| Call | Meaning |
|---|---|
| `void u3d_triangles(const u3d_vert *verts, int tri_count)` | draw `tri_count` triangles (3 vertices each) through the current transform; back faces culled, near-plane-crossing triangles dropped, the rest gouraud-shaded + depth-tested. |
| `int u3d_last_tris(void)` | triangles actually rasterised last frame (HUD/diagnostics). |

### Conventions

- Right-handed: **+X** right, **+Y** up, **−Z** into the screen.
- Column-major matrices (OpenGL layout).
- Front faces wind **counter-clockwise** in model space; back faces are culled.
- Colours are per-vertex `unsigned char` 0–255, interpolated (gouraud).

---

## 4. Writing a game with Uno3D

A Uno3D game is **portable logic + thin per-platform glue**. Keep all game state,
rules, and drawing in platform-independent C; let each platform's `main` do only
input and present. This is exactly how [`uno3d_game.c`](../uno3d/uno3d_game.c)
("UnoDOS Runner") is built.

### 4.1 Structure your game as three calls

Put the game behind an interface the glue can drive, and an **abstract input**
struct so no platform names leak into the logic
([`uno3d_game.h`](../uno3d/uno3d_game.h)):

```c
typedef struct { int left, right, up, down, fire, start; } game_input;

void game_init(int w, int h);          /* viewport aspect + reset state */
void game_update(const game_input *in);/* advance one frame of logic     */
void game_render(void);                /* u3d_begin … u3d_triangles … u3d_end */
```

### 4.2 Draw with helpers, not raw vertex dumps

Build reusable model helpers. For example, a coloured box from one unit-cube
template (positions constant, colours filled per call), transformed into place:

```c
static void draw_box(float cx,float cy,float cz, float sx,float sy,float sz,
                     int r,int g,int b)
{
    fill_cube_colors(g_box, r, g, b);     /* set the 36 verts' colours */
    u3d_load_identity();
    u3d_translate(cx, cy, cz);
    u3d_scale(sx, sy, sz);
    u3d_triangles(g_box, 12);             /* 12 tris = 1 cube */
}
```

`game_render()` then becomes a readable scene description:

```c
void game_render(void)
{
    u3d_begin(0, 0, 35);                  /* clear */
    u3d_perspective(65.0f, g_aspect, 0.5f, 80.0f);
    for (i = 0; i < NWALLS; i++) draw_wall(&wall[i]);   /* obstacles */
    draw_box(player_x, -1.6f, PLAYER_Z, 1.7f,0.7f,2.0f, 0,240,220);  /* ship */
    u3d_end();
}
```

### 4.3 The platform glue (one tiny `main` per target)

Each glue file maps real input to `game_input`, runs `update`/`render`, and
presents. The whole game *logic* is shared; only this differs:

```c
/* PS2 (uno3d_ps2_game.c) — DualShock 2 */
u3d_use_backend(&u3d_backend_ps2);
game_init(640, 448);  u3d_init(640, 448);
for (;;) {
    game_input in = {0};
    pad_read(&btn);
    in.left  = btn.left  || stick_left;
    in.right = btn.right || stick_right;
    in.start = btn.start;
    game_update(&in);
    game_render();
    u3d_present();
}
```

The Dreamcast glue is the same shape with maple-controller reads; the host
harness drives it on autopilot for testing. Three files, ~40 lines each,
zero changes to `uno3d_game.c`.

### 4.4 Tips that make a game feel right

- **Attract mode.** Have the game auto-play (a simple AI steering toward the
  goal) until the player first touches a control. It self-demos on any machine —
  invaluable when an emulator can't inject input. Runner exposes
  `game_ai_target()` for exactly this.
- **Keep it fair and finite.** Bound per-step difficulty (Runner limits how far
  an obstacle gap can move between walls, and caps the speed ramp) so the game —
  and the autopilot — can always survive. A frozen "game over" frame is usually
  an *unfair-difficulty* bug, not a renderer bug.
- **Avoid the z-buffer when you can.** A single convex object (a cube) needs only
  back-face culling. A scene of separate objects with a known depth order (a
  corridor of walls) can be drawn **far-to-near** (painter's order) and look
  correct without any depth buffer — cheaper on every backend.
- **Determinism helps testing.** Seed your RNG from a fixed value (or the tick
  counter) so a headless run is reproducible for screenshots.

### 4.5 Build & run

```sh
cd uno3d
./build.sh host-game     # software backend on the PC  -> build/game.png
./build.sh ps2-game      # PS2 GS hardware             -> build/uno3d-runner-ps2.elf
./build.sh dc-game       # Dreamcast PVR hardware       -> build/uno3d-runner-dc.elf
```

(`host` / `ps2` / `dc` build the spinning-cube demo instead of the game.)

---

## 5. Backends in detail

| File | Backend | How it rasterises |
|---|---|---|
| `uno3d_soft.c` | `soft` | half-space (edge-function) triangle fill into `fb` (the port's `fb.h`), gouraud + a `float` z-buffer. Needs nothing but a 32-bit framebuffer → runs on every port and is the reference the hardware backends are checked against. |
| `uno3d_ps2.c` | `ps2-gs` | gsKit: `gsKit_prim_triangle_gouraud_3d` with a hardware z-buffer (`GS_PSMZ_32`, `GS_ZTEST_ON`); depth mapped so nearer = larger z. |
| `uno3d_dc.c` | `dc-pvr` | KallistiOS PVR: one compiled gouraud poly context, each triangle a 3-vertex strip. **Two gotchas:** disable PVR culling (`PVR_CULLING_NONE`) since the front-end already culls on the CPU, and submit `z = 2 − depth` so depth stays positive (the PVR wants z > 0, larger = nearer). |

---

## 6. Adding a new platform

The design is built for this. To bring Uno3D to a new machine — **PS3 (RSX),
GPU-equipped PC (Direct3D/OpenGL), GameCube (GX), original Xbox (D3D8), …**:

1. Write `uno3d_<plat>.c` implementing the six `u3d_backend` functions
   (`init / shutdown / clear / tri / flush / present`) over that machine's 3D
   API, and define `const u3d_backend u3d_backend_<plat> = { … }`.
2. Add one `extern` for it in `uno3d_backend.h`.
3. Link that file into the port's build and have the glue call
   `u3d_use_backend(&u3d_backend_<plat>)` before `u3d_init()`.

**No change to `uno3d.c` or to any application.** A backend receives geometry
already transformed to screen space (`u3d_stri`: pixel x/y, depth 0..1, gouraud
colour), so it only maps "draw this triangle" onto the hardware. Backends can
coexist in one build and be selected at runtime — e.g. a PC links both a Direct3D
backend and `soft`, probes for a GPU, and falls back.

A machine with **no** 3D hardware (an old PC, an Amiga, a Mac) needs no new
backend at all: it just selects `u3d_backend_soft`, which renders into the
framebuffer it already has.

---

## 7. Limitations

- Flat/gouraud-shaded triangles only; **no textures yet** (`U3D_CAP_TEXTURE` is
  reserved). The software backend interpolates colour affinely (no
  perspective-correct divide per pixel) — fine for the solid look UnoDOS 3D apps
  use.
- One model-view "slot" (no matrix stack); set it fresh per object.
- The software backend's z-buffer is sized to the framebuffer at compile time.

---

## 8. UnoDOS x86 — a native app, not the C library

UnoDOS itself (the bare-metal x86 OS) is **not** a C target: its applications are
flat 16-bit NASM `.BIN` files that call the kernel via `INT 0x80`
([APP_DEVELOPMENT.md](APP_DEVELOPMENT.md)). So the C `uno3d` library above does
**not** run there — and couldn't anyway (its 256 KB framebuffer/z-buffer wouldn't
fit a 64 KB app segment).

Instead the UnoDOS-x86 version of the game is a **native application**,
[`apps/runner3d.asm`](../apps/runner3d.asm) (`RUN3D.BIN`), which implements the
*same game design and the same perspective math* by hand, using only the UnoDOS
graphics API. Key points:

- Talks only to the kernel: `INT 0x80` to set VGA mode 13h, draw filled rects,
  read key events, and restore on exit. No DOS, no direct hardware assumptions
  beyond what the kernel exposes.
- Because the camera never rotates and walls face the camera, each wall block
  projects to an **axis-aligned rectangle**, so the whole solid 3D corridor is
  drawn with the kernel's `gfx_draw_filled_rect_color` in painter's order
  (far→near) — 16-bit fixed point, no FPU, no framebuffer poking.
- Assembles for the 8088, so it runs on **every** UnoDOS machine (and is snappy
  on a 386+). It ships in the OS image (`make build/unodos-144.img`) and the
  desktop launcher auto-discovers it from its icon header.

This is the deliberate division of labour: the portable **C `uno3d` library**
serves the hosted/console ports (PS2, Dreamcast, host, future GPUs); the **native
asm app** serves the bare-metal OS. Both render the same game.

---

## See also

- [`uno3d/README.md`](../uno3d/README.md) — the library's own quick reference + file list.
- [APP_DEVELOPMENT.md](APP_DEVELOPMENT.md) — writing native UnoDOS apps (the x86 path).
- [API_REFERENCE.md](API_REFERENCE.md) — the UnoDOS `INT 0x80` kernel API the native app uses.
- [PORTS-PLAN.md](PORTS-PLAN.md) — the multi-platform porting program.
