/* smbus_handler.h - ACPI SMBus / GenericSerialBus OperationRegion handler.
 *
 * Some laptops expose the battery as a standard control-method battery whose
 * _STA/_BST read through an ACPI **SMBus** region (e.g. Apple: BAT0 -> SMB0.SBRW)
 * rather than the EC.  uACPI dispatches those as SERIAL_READ/WRITE ops but does
 * not drive any bus itself, so this supplies the transaction over the Intel PCH
 * SMBus host controller (the same silicon WU's smbus_read_word already uses).
 *
 * Shared, x86/Intel-PCH-specific (like ec_handler is x86-specific).  All waits are
 * bounded via uacpi_kernel_get_nanoseconds_since_boot so a contended/absent bus
 * fails the transaction (status byte != 0) rather than hanging.  NOTE: on some
 * machines (notably Macs) the SMC owns the host SMBus and every transaction will
 * report busy - that is surfaced, not hidden.
 *
 * Only built when WU_ACPI is defined.
 */
#pragma once
#include <stdint.h>
#include <uacpi/types.h>

/* Provide the Intel PCH SMBus I/O base (PCI BAR4 of the SMBus controller).  Call
 * once before uacpi_namespace_initialize.  base 0 disables the handler. */
void wu_smbus_init(uint16_t pch_io_base);

/* The uACPI region handler for UACPI_ADDRESS_SPACE_SMBUS and
 * UACPI_ADDRESS_SPACE_GENERIC_SERIAL_BUS. */
uacpi_status wu_smbus_region_handler(uacpi_region_op op, uacpi_handle op_data);

/* Diagnostics for the overlay: whether a base was set, how many transactions ran,
 * and the parameters + result of the most recent one. */
void wu_smbus_info(uint16_t *base, int *calls, uint8_t *last_addr, uint8_t *last_cmd,
                   uint8_t *last_proto, uint8_t *last_status, uint32_t *last_data);
