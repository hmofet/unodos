# Duum lives in its own repository now

**Canonical home: https://github.com/hmofet/duum** (MPL-2.0, same licence as
UnoDOS).

Duum started here, as `pc64/apps/DUUM.PY` — the acceptance test that Python is
a first-class app language on this OS. It is now a standalone cross-platform
Doom engine that runs anywhere CPython does, and UnoDOS is one of its ports
rather than its home.

## The rule

**Do not develop Duum in this repository.** `pc64/apps/DUUM.PY` is a
**vendored, generated file**; its header says so. An edit made here is lost
the next time anyone syncs, and worse, it silently forks the engine away from
the upstream that every other platform builds on.

A change to the engine — a rendering fix, an optimisation, a gameplay
behaviour — goes to the duum repo, and comes back here as a sync.

What still belongs to UnoDOS, and is edited here normally:

| File | Why it is ours |
|---|---|
| `pc64/upy_port/mod_uno.c` | the C canvas (`cv_wall_col`, `cv_flat_col`, …). This is the span-writer contract Duum draws through, and it is a pc64 implementation detail. |
| `pc64/tools/duum_host.py` | the host **mirror of that C canvas**, so the gates below test the pixels the *device* would produce, not upstream's reference rasteriser. |
| `pc64/tools/duum_golden.py`, `duum_verify.py` | the gates, run against the mirror above. Upstream carries its own copies pointed at its own rasteriser; both are wanted, and they check different implementations of the same contract. |
| `pc64/tools/demo/duum_demo.py`, `duum_ab.py`, `duum_sound.py`, `duum_vs_doom.py` | device recording, on-device A/B, audio and oracle tooling. UnoDOS-specific. |
| `pc64/build.sh` step 3e, `pc64/docs_esp/DUUM.MD`, `pc64/wads/` | packaging `DUUM.UNO`, the shipped user doc, and where a WAD goes. |

## Syncing a new version

```bash
# from a local checkout of the duum repo
python pc64/tools/sync_duum.py --from ../duum

# or straight from a published tag
python pc64/tools/sync_duum.py --ref v0.1.0
```

It drops upstream's `dist/unodos/DUUM.PY` — the engine only; the desktop
rasteriser and tkinter frontend are not in that build — onto
`pc64/apps/DUUM.PY`, and prints the version it landed.

Then prove it, because a sync is a code drop from another repository:

```bash
cd pc64
python tools/duum_verify.py --wad wads/DOOM1.WAD      # 0 failing views
python tools/duum_golden.py check --wad wads/DOOM1.WAD # 54/54 identical
```

`duum_golden` compares against a saved baseline; if a sync is *meant* to
change pixels, look at the diff first and then re-`save`. `duum_verify` is an
independent oracle and must be 0 failing views regardless.

## Why the engine can be shared at all

Duum only ever asked its platform for six things — `size`, `read_at`, `beep`,
`quiet`, and optionally `ticks` and `keys_down` — and only ever drew through
a canvas with a span-writer contract. That is the whole porting surface, and
it is why the same file runs on a desktop and on pc64 unchanged: upstream
supplies a pure-Python rasteriser, we supply a C one, and the engine cannot
tell the difference.

The engine itself is pure Python on both. There is a C per-column rasteriser
in `mod_uno.c` (`cv_seg_cols`) from an earlier experiment; it is **no longer
called** — upstream's engine does that loop in Python — and survives only as a
reference transcription.

## Not in THIRD-PARTY.md, on purpose

`THIRD-PARTY.md` is the manifest of code belonging to **another entity**, and
its keys are checked against the notices UnoDOS ships in
`pc64/docs_esp/LICENSES.MD`. Duum is the same author under the same licence
(MPL-2.0) as UnoDOS, so it carries no third-party notice obligation and adding
a key would only add a shipped notice that says nothing. It is a separate
*repository*, not a separate *entity*.
