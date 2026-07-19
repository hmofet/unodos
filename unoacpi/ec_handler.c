/* ec_handler.c - see ec_handler.h.  ACPI Embedded Controller region handler. */
#include "ec_handler.h"

#include <uacpi/opregion.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>
#include <uacpi/kernel_api.h>

/* ---- port I/O (self-contained; this file is shared, no host headers) -------- */
static inline uint8_t ec_inb(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void    ec_outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }

/* EC status register bits (read from the command/status port). */
#define EC_OBF  0x01u   /* output buffer full - data ready to read     */
#define EC_IBF  0x02u   /* input buffer full  - EC has not consumed yet */

/* EC commands. */
#define RD_EC   0x80u
#define WR_EC   0x81u

/* Per-wait timeout.  Generous enough for a slow-but-live EC, short enough that a
 * dead/hostile EC fails a whole _BST (~a dozen reads) in well under a second. */
#define EC_WAIT_NS  (5ull * 1000ull * 1000ull)   /* 5 ms */

static uint16_t g_cmd  = 0x66;    /* command/status port */
static uint16_t g_data = 0x62;    /* data port           */
static int      g_from_ecdt;
static int      g_reads;          /* successful byte reads (diag)  */
static int      g_timeouts;       /* bounded-wait timeouts (diag)  */

/* Pull EC ports from the ECDT if present; otherwise keep the 0x66/0x62 default. */
int wu_ec_init(void)
{
    uacpi_table tbl;
    if (uacpi_table_find_by_signature(ACPI_ECDT_SIGNATURE, &tbl) == UACPI_STATUS_OK && tbl.ptr) {
        struct acpi_ecdt *e = (struct acpi_ecdt *)tbl.ptr;
        if (e->ec_control.address) g_cmd  = (uint16_t)e->ec_control.address;
        if (e->ec_data.address)    g_data = (uint16_t)e->ec_data.address;
        g_from_ecdt = 1;
    }
    /* Liveness hint: an open bus reads 0xFF on the status port. */
    uint8_t st = ec_inb(g_cmd);
    return st != 0xFFu;
}

/* Spin until (status & mask) == want, or the deadline passes.  Returns 1 on the
 * wanted condition, 0 on timeout (counted).  Fast-bails on an open-bus status. */
static int ec_wait(uint8_t mask, uint8_t want)
{
    uint64_t deadline = uacpi_kernel_get_nanoseconds_since_boot() + EC_WAIT_NS;
    for (;;) {
        uint8_t st = ec_inb(g_cmd);
        if (st == 0xFFu) { g_timeouts++; return 0; }     /* no EC on the bus */
        if ((st & mask) == want) return 1;
        if (uacpi_kernel_get_nanoseconds_since_boot() >= deadline) { g_timeouts++; return 0; }
    }
}

/* RD_EC: read one byte at EC address 'addr'.  Returns 1 + *out on success. */
static int ec_read_byte(uint8_t addr, uint8_t *out)
{
    if (!ec_wait(EC_IBF, 0)) return 0;
    ec_outb(g_cmd, RD_EC);
    if (!ec_wait(EC_IBF, 0)) return 0;
    ec_outb(g_data, addr);
    if (!ec_wait(EC_OBF, EC_OBF)) return 0;
    *out = ec_inb(g_data);
    g_reads++;
    return 1;
}

/* WR_EC: write one byte 'val' at EC address 'addr'. */
static int ec_write_byte(uint8_t addr, uint8_t val)
{
    if (!ec_wait(EC_IBF, 0)) return 0;
    ec_outb(g_cmd, WR_EC);
    if (!ec_wait(EC_IBF, 0)) return 0;
    ec_outb(g_data, addr);
    if (!ec_wait(EC_IBF, 0)) return 0;
    ec_outb(g_data, val);
    if (!ec_wait(EC_IBF, 0)) return 0;
    return 1;
}

uacpi_status wu_ec_region_handler(uacpi_region_op op, uacpi_handle op_data)
{
    switch (op) {
    case UACPI_REGION_OP_ATTACH:
    case UACPI_REGION_OP_DETACH:
        return UACPI_STATUS_OK;                    /* EC is stateless for us */

    case UACPI_REGION_OP_READ: {
        uacpi_region_rw_data *d = op_data;
        uint64_t off = d->offset;
        uint8_t  w   = d->byte_width ? d->byte_width : 1;
        uint64_t val = 0;
        for (uint8_t i = 0; i < w; i++) {
            if (off + i > 0xFF) break;             /* EC space is 256 bytes */
            uint8_t b;
            if (!ec_read_byte((uint8_t)(off + i), &b))
                return UACPI_STATUS_HARDWARE_TIMEOUT;
            val |= (uint64_t)b << (8u * i);
        }
        d->value = val;
        return UACPI_STATUS_OK;
    }

    case UACPI_REGION_OP_WRITE: {
        uacpi_region_rw_data *d = op_data;
        uint64_t off = d->offset;
        uint8_t  w   = d->byte_width ? d->byte_width : 1;
        for (uint8_t i = 0; i < w; i++) {
            if (off + i > 0xFF) break;
            if (!ec_write_byte((uint8_t)(off + i), (uint8_t)(d->value >> (8u * i))))
                return UACPI_STATUS_HARDWARE_TIMEOUT;
        }
        return UACPI_STATUS_OK;
    }

    default:
        return UACPI_STATUS_UNIMPLEMENTED;
    }
}

void wu_ec_info(uint16_t *cmd_port, uint16_t *data_port, int *from_ecdt,
                int *reads, int *timeouts)
{
    if (cmd_port)  *cmd_port  = g_cmd;
    if (data_port) *data_port = g_data;
    if (from_ecdt) *from_ecdt = g_from_ecdt;
    if (reads)     *reads     = g_reads;
    if (timeouts)  *timeouts  = g_timeouts;
}
