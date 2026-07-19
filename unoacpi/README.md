# unoacpi — the shared AML/ACPI power stack

A portable, host-abstracted ACPI battery / lid / device-power stack built on a
vendored [uACPI](https://github.com/uACPI/uACPI) interpreter. **Shared verbatim
from the canonical repo `hmofet/acpipower`** (extracted from writers-unlock
2026-07-19) — do not hand-edit anything here except to re-sync:

    <acpipower>/tools/sync-to-consumer.sh --flat <unodos>/unoacpi

Per-OS code belongs in the host layer (`pc64/acpi_host.c`).

Integration contract: `CONTRACT.md` in the acpipower repo (Writer's Unlock
remains the hardware-reference host). uACPI pin/re-vendor instructions:
`uacpi/UACPI-PIN.md` (tag 6.0.0).

| Layer | Files | Ownership |
|---|---|---|
| consumer API | `acpi_power.{c,h}` — bringup + `acpi_battery_percent()` / `acpi_lid_state()` / `acpi_device_power_on()` | shared, verbatim |
| op-region handlers | `ec_handler.{c,h}` (EmbeddedController), `smbus_handler.{c,h}` (SMBus/GenericSerialBus over an Intel PCH host) | shared, verbatim |
| heap | `acpi_arena.{c,h}` — boundary-tag free-list over a host buffer | shared, verbatim |
| interpreter | `uacpi/` — pinned, never hand-edited | vendored |
| host callbacks | `pc64/acpi_host.c` — the ~25 `uacpi_kernel_*` fns | **per-OS** |

Key invariants (hard-won on real hardware — see the WU repo history):

- The loaded namespace **lives in the arena** and must persist; never reset the
  arena after bring-up. 8 MiB arena (1 MiB truncates real laptops' SSDTs).
- `UACPI_FLAG_NO_ACPI_MODE`: never write ACPI_ENABLE/SMI_CMD — read-only client.
- Every EC/SMBus wait is bounded via `uacpi_kernel_get_nanoseconds_since_boot()`
  (must actually advance), so hostile ECs time out instead of hanging.
- The FACS hardware Global Lock is neutralised after `namespace_load` (no SCI
  service here), or `Lock`-qualified EC fields abort before reaching the handler.
