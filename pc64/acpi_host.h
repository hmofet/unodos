/* acpi_host.h - UnoDOS/pc64 host entry for the unoacpi stack (acpi_host.c).
 *
 * Only compiled with -DUNO_ACPI.  Consumers read battery/lid via the portable
 * API in unoacpi/acpi_power.h; this header is just the pc64 bring-up + diag.
 */
#ifndef PC64_ACPI_HOST_H
#define PC64_ACPI_HOST_H
#include <stdint.h>

/* Bind EFI services (pass the EFI_SYSTEM_TABLE *), locate the RSDP, allocate
 * the 8 MiB arena, seed the SMBus handler, and run acpi_power_bringup().
 * Returns 1 when the interpreter is up (namespace built), 0 otherwise.
 * Idempotent - only the first call does the work. */
int uno_acpi_start(void *efi_system_table);

/* The RSDP physical address (0 = none found / not started). */
uint64_t uno_acpi_rsdp(void);

/* Tail of the interpreter's log ring (always a valid C string). */
const char *uno_acpi_log_tail(void);

#endif
