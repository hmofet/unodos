# Vendored uACPI — pin record

This directory is a **verbatim, untouched** drop-in of the uACPI interpreter
(https://github.com/uACPI/uACPI, MIT). It is shared byte-identical between
Writer's Unlock and UnoDOS. **Do not hand-edit anything under `source/` or
`include/`** — all host adaptation lives outside this tree (see below). To change
the interpreter, re-vendor a new pinned release rather than patching in place.

| | |
|---|---|
| **Pinned tag** | `6.0.0` |
| **Commit SHA** | `9c9b26d6291a1cdd9014cc5bb6b03e596697cbfd` |
| **Date** | 2026-06-28 |
| **License** | MIT (`LICENSE`) |
| **Vendored** | 2026-07-18, Phase 1 |
| **Contents** | `source/` (19 `.c`), `include/uacpi/` (main + `platform/` + `internal/`) |

## What is NOT vendored (host-side, lives outside this tree)

- `shared/acpi_arena.{c,h}` — fixed-arena free-list heap backing
  `uacpi_kernel_alloc`/`free` (shared with UnoDOS).
- `<os>/acpi_host.c` — the ~25 `uacpi_kernel_*` glue functions (per OS; WU's is
  `uefi/acpi_host.c`).
- `<os>/acpi_entry.{c,h}` — the OS-facing API and the `WU_ACPI`-off stub.

## Build configuration (passed via compiler `-D`, config.h left stock)

- **Not** `UACPI_BAREBONES_MODE` (it strips `uacpi_eval*`, which is the whole point).
- `UACPI_SIZED_FREES` **undefined** → `uacpi_kernel_free(void*)` single-arg; the
  arena recovers each block's size from its own boundary tag.
- `UACPI_NATIVE_MMIO` / `UACPI_NATIVE_ALLOC_ZEROED` **undefined** → uACPI's built-in
  volatile MMIO and alloc+memset zeroed-alloc are used, so the host need not supply them.
- `UACPI_USE_BUILTIN_STRING` **undefined** → `uacpi_memcpy`/`uacpi_memset` map to the
  host `memcpy`/`memset` (Writer's Unlock provides them in `kernel/string.c`).
  uACPI's own `snprintf`/`strlen`/`strcmp` live in `source/stdlib.c` under
  `uacpi_`-prefixed names — no symbol clash with the host libc-ish names.
- Atomics: clang (`__clang__` defined) takes uACPI's `__atomic_*` builtin branch —
  no libatomic needed. On the 32-bit BIOS build add `UACPI_PHYS_ADDR_IS_32BITS`.

## Re-vendoring (bump the pin)

```
git clone --depth 1 --branch <tag> https://github.com/uACPI/uACPI /tmp/uacpi
rm -rf vendored/uacpi/source vendored/uacpi/include
cp -r /tmp/uacpi/source vendored/uacpi/include vendored/uacpi/LICENSE vendored/uacpi/
# then update this file's tag/SHA/date and re-run the WU_ACPI=1 build
```

Run `cloc source include` on the pinned tree for exact size figures when updating
`docs/AML-INTERPRETER-DESIGN.md`.
