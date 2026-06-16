# unofs — the storage subsystem worked example (CONTRACT-ARCH §12, Phase 3)

`unofs` is the Layer-2 storage **policy** extracted as portable C, sitting over the
Layer-1 **`block`** service. It is the worked proof of the contract-driven pattern:
the policy is written once and driven entirely by the generated Contract
(`unodef/gen/c/unodef.h`) — FAT12 geometry, the 32-byte `dirent`, and the
`file_handle` layout (owner @ byte 24) — so it contains **no magic numbers**. A port
supplies only a `block` backend.

## Files

| File | Layer | Role |
|---|---|---|
| `unofs.h` | — | `block` service interface + the `unofs_*` API |
| `unofs_core.c` | L2 policy | mount, readdir, open/read/close, create/write/delete, **owner-based reaping**, cluster-chain walk, 12-bit FAT parse, **consecutive-cluster batching** |
| `block_file.c` | L1 mechanism | host file-backed `block` (a `.img` is a host file) — the §15 host reference |
| `unofs_test.c` | — | host-first verification harness |

## Host-first verification

```
make                 # builds build/unodos-144.img (the real shipping floppy)
sh unofs/build.sh    # compiles unofs + runs the test against that image
```

The test (CONTRACT-ARCH §12 "Host-first") mounts the **real shipping x86 floppy**
and proves the contract-driven reader correct *before any emulator*:

- **readdir** lists the volume.
- **extract & byte-diff** — `CLOCK.BIN` (2 clusters), `SYSINFO.BIN` (3), `TEXT.BIN`
  (13), `PAINT.BIN` (62 clusters — exercises the chain walk + consecutive-cluster
  batching) are each read out and compared **byte-identical** to the `build/*.bin`
  outputs they were packed from.
- **owner-based reaping** (PORT-SPEC §6.7) — a dying task's handles are freed, others
  survive.
- **write / re-read / delete** on a scratch copy — `create` → `write` → re-mount →
  read back the exact bytes → `delete` → confirm gone.

All checks pass (`exit 0`). Because the geometry and layouts come from the Contract,
the same `unofs_core` compiles for any C-world port (PS2/DC/Mac/…) against that
port's own `block` backend; the asm ports consume the generated equates instead.
