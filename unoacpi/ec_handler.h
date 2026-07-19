/* ec_handler.h - EmbeddedController OperationRegion handler for uACPI.
 *
 * Shared verbatim between Writer's Unlock and UnoDOS.  The EC is the one region
 * type uACPI does not handle itself, and it is where battery/lid telemetry lives
 * on most laptops (a _BST method reads EC field units).  This installs over the
 * standard ACPI EC protocol (RD_EC 0x80 / WR_EC 0x81 on the command/data ports).
 *
 * SAFETY: every IBF/OBF wait is bounded via uacpi_kernel_get_nanoseconds_since_boot
 * and the probe fast-bails on an open-bus (0xFF) status, so a hostile/absent EC
 * degrades to a failed read - never a hang.  (The Dell-Latitude lesson: a single
 * bounded RD_EC on that EC times out cleanly; only a per-frame storm ever froze it.)
 *
 * Only built when WU_ACPI is defined.
 */
#pragma once
#include <stdint.h>
#include <uacpi/types.h>

/* Discover the EC command/data ports (from the ECDT table, else the 0x66/0x62
 * default) so the handler is ready before it is installed.  Returns 1 if an EC
 * looks present, 0 otherwise.  Safe to call once, before uacpi_namespace_initialize. */
int wu_ec_init(void);

/* The uACPI region handler.  Pass to uacpi_install_address_space_handler for
 * UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER. */
uacpi_status wu_ec_region_handler(uacpi_region_op op, uacpi_handle op_data);

/* Override the EC command/status + data ports from the DSDT PNP0C09 _CRS (for
 * machines with no ECDT, e.g. the Dell).  Call after namespace_initialize. */
void wu_ec_set_ports(uint16_t cmd_port, uint16_t data_port);

/* Diagnostics for the overlay: the resolved ports, their source (0 = default,
 * 1 = ECDT, 2 = _CRS), and running read/timeout counters. */
void wu_ec_info(uint16_t *cmd_port, uint16_t *data_port, int *port_source,
                int *reads, int *timeouts);
