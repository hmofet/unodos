# Round 2 - hard-crash robustness sweep (2026-07-20)

Cross-cutting pattern: the code ships correct bounded path-join helpers (`join_path`,
`ph_join`) but also hand-rolled UNBOUNDED copy loops (`fcat`, raw
`while(*s)*q++=*s++`) sitting right next to them. Three are live overflows. The top
two are hit by ordinary Files/Photos folder navigation and are the best match for the
user's "several hard crashes I didn't record."

## Tier 1 - CONFIRMED, a normal user reaches these

### 1. `pc64_files.c:262-266` - `head[64]` stack overflow from the pane path (CONFIRMED)
Verified first-hand. `draw_pane` builds `VOLUME\PATH` into `char head[64]` via the
unbounded `fcat` (`:59` `while(*s)*p++=*s++;`). `fm_pane.path` is `char[120]` (`:41`)
and legitimately fills as the user descends directories. Volume label + `"\"` + a path
past ~52 chars overruns `head[64]`, smashing `draw_pane`'s return frame -> triple
fault (no memory protection).
- Trigger: open Files, descend into nested subfolders on a FAT volume (or open a
  pre-existing deep tree). Fires on the next automatic repaint.
- Fix: size `head` for the worst case (`P->path` 119 + label ~11 + sep, use `[160]`)
  and replace `fcat` with a bounds-checked copy like `join_path` already does.

### 2. `apps/photos.c:580-585` - `np[120]`/`ph_path[120]` overflow on descent (CONFIRMED)
Verified first-hand. `ph_activate` copies `ph_path` (up to 119) + `'\'` +
`ph_e[idx].name` into `char np[120]` with a raw unbounded loop, then
`strcpy(ph_path, np)`. ~9 levels of FAT 8.3 folders overruns `np` then `ph_path`.
- Trigger: open Photos, navigate its pane into deeply nested directories.
- Fix: bound the build like the file's own `ph_join` (`:173-181`); refuse to descend
  if it will not fit. (`ph_open_entry` already uses safe `ph_join`; only this entry
  path is unguarded.)

### 3. `pc64_games.c:588-589` - `pc64_game_name(GAME_RUNNER)` returns NULL -> NULL-deref (CONFIRMED)
Verified first-hand. `static const char *n[PC64_NGAMES] = {"Dostris","Pac-Man",
"OutLast"};` but `PC64_NGAMES == 4`, so `n[3]` (GAME_RUNNER, a valid index) is
implicitly NULL and the `game < PC64_NGAMES` guard passes it through. Any shell path
rendering the Runner's name via `fb_text`/`strcpy` (`while(*s)`) derefs NULL.
- Trigger: the shell labelling/opening the Runner game (taskbar chip, launcher).
- Fix: add the 4th string (one line).

## Tier 2 - CONFIRMED mechanism, pathological or gated trigger

### 4. `apps/ucc.c` parser - unbounded recursive descent -> kernel-stack overflow (CONFIRMED)
No depth counter anywhere: `primary`(1290)/`unary`(1354)/`assign`(1510)/
`cond_expr`(1489)/`stmt`<->`block`(1915/2028)/`declarator`(1131, twice per level) all
recurse. ~200-400 B/level on the ring0 shell stack (incl. `char b[64]` in `primary`).
A few hundred nested `((((...))))` / `{{{...}}}` / `a=b=c=...` overflows and triple-
faults instead of erroring. Codegen (`ucc_x64.c:391/552`) overflows at the same depth.
- Trigger: compile pathologically nested source (generated / DoS), not normal code.
- Fix: add `int depth` to `Cc`, bump on entry to those functions, `ucc_fatal` past
  ~200 - the same guard `js.c:206` and `pyrt.c:166` already have; ucc is the one
  language runtime missing it.

### 5. `apps/photos.c:313-338` - partial-OOM leaves `ph_map` NULL but marks dims cached -> NULL write (CONFIRMED, OOM-gated)
`ph_cw/ph_ch` set to `vw/vh` before the null check on `ph_cache`/`ph_map`. If one
malloc succeeds and the other fails, the next repaint sees `ph_cw==vw`, skips realloc,
and writes `ph_map[vx*2]=x0` (`:338`) through NULL. Fix: assign `ph_cw/ph_ch` only
after both allocations succeed; on failure free the survivor and null both.

## Tier 3 - SUSPECTED / latent (bounded in practice, worth hardening)

- **6. `pc64_uui.c:545-566` `g_nat[80]` .bss overflow** in System diagnostics:
  `build_natstat` appends with unbounded `ap_str`/`ap_int`; worst case ~86 bytes into
  an 80-byte static (detached, 3+ long-labelled FAT vols). Each label capped, running
  total not. Fix: end pointer or `g_nat[160]`.
- **7. `apps/ucc.c:826-833` compile-time `INT64_MIN / -1` #DE** - folding guards /0 but
  not signed overflow. Fix: special-case `b==-1 && a==INT64_MIN`.
- **8. `apps/tracker.c:54-55,157`** - corrupt `SONG.TRK` feeds out-of-range note to the
  synth: `tk_load` doesn't validate `gTkPat`; edits clamp `cell[0]` to 1..24 but a
  loaded file holds 0..255 -> `music_note_on(59+cell[0])` up to 314. Fix: clamp after
  load.
- **9. `pc64_math.c:41-42` `sinf`/`cosf` range-reduction infinite loop** on Inf/huge
  input: `while (x > PI_F) x -= TWOPI;` never terminates for +Inf. Reachable via
  `math.sin()` huge arg or a bad uno3d value -> hang. Fix: clamp/early-return for
  non-finite / large magnitude (floorf/ceilf already guard this).
- **10. `apps/photos.c:238-240,289`** - 32-bit `malloc(w*h*4)` overflow on >1-gigapixel
  dims; div0 in `ph_scale16` on a 0 dimension. Fix: 64-bit byte count + cap;
  `if (w<1||h<1) fail` after `um_image_open`.
- **11. `pc64_http.c:46-51` `num[8]`** TLS-error decimal buffer, no bound (codes small,
  not normally reachable). Fix: bound the loop or `num[12]`.

## Verified clean (bounds false positives)
- `pc64_libc.c` allocator: `free` rejects out-of-arena + double-free, `calloc` checks
  multiply overflow, `realloc` copies safely. Residual risk = callers must null-check
  malloc (surfaces as #5).
- `pc64_native.c`, `pc64_io.c`, `pc64_uui_apps.c`, `pc64_fs.c`, `settings.c`,
  `theme.c`, `clock.c`, `sysinfo.c`: all indices range-checked.
- `pc64_uui.c` lifecycle (remove_win/close_focused/menu/cycle/taskbar) bounded against
  NAPPS/nwin; widgets capped at UNOUI_MAX_WIDGETS; windows capped at 24.
- `music.c`, `paint.c`, `dostris.c`, `pacman.c`, `outlast.c`: robust; the hypothesized
  "piece rotates to board[-1]" bug does NOT exist (all moves funnel through
  bounds-checked `dt_fits`/`pm_walkable`); Paint flood-fill is null/bounds-guarded.
- `pc64_browser.c`, `pc64_http.c`: no clickable-link array / history / redirect
  following; url/host/path/tag-stack all bounded. `pc64_write.c`: insert clamps to
  WR_MAX-1; all buffers bounded.

## Recommended fix order
1. `pc64_files.c:262` and 2. `photos.c:580` - the two path-join overflows ordinary
   navigation triggers; best match for the unrecorded crashes.
3. `pc64_games.c:588` - one-line NULL-name fix.
4. `ucc.c` parser depth guard.
Then Tier 3. Also: sweep the whole tree for the unbounded-copy idiom (`fcat`,
`while(*s)*p++=*s++` into a fixed local) - it recurs precisely where a safe helper
already exists.
