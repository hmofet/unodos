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

/* F4: I2C-HID devices declared in the ACPI namespace (PNP0C50/ACPI0C50).
 * slave = I2C address from _CRS I2cSerialBus; desc_reg = HID descriptor
 * register from _DSM (0x0001 fallback); ctrl_dev/ctrl_fn = the controller's
 * PCI location from its _ADR (-1 = unresolved); ctrl_mmio = the controller's
 * MMIO base from its own _CRS memory resource (0 = none) - the ONLY way to
 * reach an LPSS I2C controller the firmware presents in ACPI mode, hidden from
 * PCI (the Yoga's ctrls=0); src = controller ACPI path. Returns the number of
 * hits. Requires uno_acpi_start() to have succeeded. */
typedef struct {
    unsigned short slave, desc_reg;
    int ctrl_dev, ctrl_fn;
    uint64_t ctrl_mmio;
    char src[40];
} uno_acpi_i2chid;
int uno_acpi_i2c_hid_enum(uno_acpi_i2chid *out, int max);

/* Tail of the interpreter's log ring (always a valid C string). */
const char *uno_acpi_log_tail(void);

/* ACPI S5 soft-off. Returns only on FAILURE (success powers the machine off).
 * The reliable poweroff where EFI_RESET_SHUTDOWN is a no-op (the Surface). */
int uno_acpi_poweroff(void);

#endif
