/* smbus_handler.c - see smbus_handler.h.  Intel PCH SMBus host controller. */
#include "smbus_handler.h"

#include <uacpi/opregion.h>
#include <uacpi/status.h>
#include <uacpi/kernel_api.h>

/* ---- port I/O (self-contained; shared file, no host headers) --------------- */
static inline uint8_t s_inb(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void    s_outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }

/* Intel PCH SMBus (ICH/PCH) register offsets from the SMBus IO base (BAR4). */
#define R_STS   0x00   /* HST_STS  */
#define R_CNT   0x02   /* HST_CNT  */
#define R_CMD   0x03   /* HST_CMD  */
#define R_SLVA  0x04   /* XMIT_SLVA */
#define R_D0    0x05   /* HST_D0   */
#define R_D1    0x06   /* HST_D1   */
#define R_BLKDAT 0x07  /* HOST_BLOCK_DB */

/* HST_STS bits. */
#define STS_BUSY  0x01
#define STS_INTR  0x02
#define STS_DEVERR 0x04
#define STS_BUSERR 0x08
#define STS_FAILED 0x10

/* HST_CNT protocol codes (bits 4:2), OR'd with START (0x40). */
#define P_QUICK  0x0
#define P_BYTE   0x1   /* send/receive byte           */
#define P_BYTED  0x2   /* read/write byte data        */
#define P_WORDD  0x3   /* read/write word data        */
#define P_BLOCK  0x5

/* ACPI SMBus access attributes (uacpi_access_attribute). */
#define A_QUICK        0x02
#define A_SEND_RECEIVE 0x04
#define A_BYTE         0x06
#define A_WORD         0x08
#define A_BLOCK        0x0A

#define SMB_WAIT_NS (50ull * 1000 * 1000)   /* 50 ms budget per transaction */

static uint16_t g_base;
/* diagnostics */
static int      g_calls;
static uint8_t  g_last_addr, g_last_cmd, g_last_proto, g_last_status;
static uint32_t g_last_data;

void wu_smbus_init(uint16_t pch_io_base) { g_base = pch_io_base; }

/* Wait for HST_STS to signal completion (INTR) or an error.  Returns the ACPI
 * SMBus status byte: 0 = ok, 0x01 = generic failure, 0x1A = bus busy/timeout. */
static uint8_t smb_wait(void)
{
    uint64_t dl = uacpi_kernel_get_nanoseconds_since_boot() + SMB_WAIT_NS;
    for (;;) {
        uint8_t s = s_inb((uint16_t)(g_base + R_STS));
        if (s & (STS_DEVERR | STS_BUSERR | STS_FAILED)) { s_outb((uint16_t)(g_base + R_STS), 0xFF); return 0x1F; }
        if (s & STS_INTR) return 0x00;
        if (uacpi_kernel_get_nanoseconds_since_boot() >= dl) {
            s_outb((uint16_t)(g_base + R_CNT), 0x02);   /* KILL */
            s_outb((uint16_t)(g_base + R_STS), 0xFF);
            return 0x1A;                                /* SMBus "bus busy" */
        }
    }
}

/* Issue one PCH SMBus transaction.  proto is a P_* code; for reads, results land
 * in *w (word/byte) or blk[]/*blklen (block).  Returns the ACPI status byte. */
static uint8_t smb_xact(uint8_t addr, uint8_t cmd, uint8_t proto, int write,
                        uint32_t wval, uint8_t *blk, uint8_t *blklen, uint32_t *out)
{
    if (!g_base) return 0x07;                            /* no controller */

    /* Wait for idle (do not gate on INUSE - reading it sets it; clearing STS frees it). */
    uint64_t dl = uacpi_kernel_get_nanoseconds_since_boot() + SMB_WAIT_NS;
    while (s_inb((uint16_t)(g_base + R_STS)) & STS_BUSY)
        if (uacpi_kernel_get_nanoseconds_since_boot() >= dl) return 0x1A;
    s_outb((uint16_t)(g_base + R_STS), 0xFF);

    s_outb((uint16_t)(g_base + R_SLVA), (uint8_t)((addr << 1) | (write ? 0 : 1)));
    s_outb((uint16_t)(g_base + R_CMD), cmd);
    if (write) {
        if (proto == P_BYTED) s_outb((uint16_t)(g_base + R_D0), (uint8_t)wval);
        else if (proto == P_WORDD) { s_outb((uint16_t)(g_base + R_D0), (uint8_t)wval);
                                     s_outb((uint16_t)(g_base + R_D1), (uint8_t)(wval >> 8)); }
    }
    s_outb((uint16_t)(g_base + R_CNT), (uint8_t)(0x40 | (proto << 2)));   /* START */

    uint8_t st = smb_wait();
    if (st) return st;

    if (!write && out) {
        if (proto == P_BYTED || proto == P_BYTE)
            *out = s_inb((uint16_t)(g_base + R_D0));
        else if (proto == P_WORDD)
            *out = (uint32_t)s_inb((uint16_t)(g_base + R_D0)) |
                   ((uint32_t)s_inb((uint16_t)(g_base + R_D1)) << 8);
    }
    if (!write && proto == P_BLOCK && blk && blklen) {
        uint8_t n = s_inb((uint16_t)(g_base + R_D0));
        if (n > 32) n = 32;
        for (uint8_t i = 0; i < n; i++) blk[i] = s_inb((uint16_t)(g_base + R_BLKDAT));
        *blklen = n;
    }
    return 0x00;
}

uacpi_status wu_smbus_region_handler(uacpi_region_op op, uacpi_handle op_data)
{
    switch (op) {
    case UACPI_REGION_OP_ATTACH: {
        uacpi_region_attach_data *d = op_data;
        d->out_region_context = (void *)(uintptr_t)d->generic_info.base;  /* slave address */
        return UACPI_STATUS_OK;
    }
    case UACPI_REGION_OP_DETACH:
        return UACPI_STATUS_OK;

    case UACPI_REGION_OP_SERIAL_READ:
    case UACPI_REGION_OP_SERIAL_WRITE: {
        uacpi_region_serial_rw_data *d = op_data;
        int write = (op == UACPI_REGION_OP_SERIAL_WRITE);

        /* slave address from ATTACH; SMBus command = abs_offset - base */
        uint64_t base = (uint64_t)(uintptr_t)d->region_context;
        uint8_t  addr = (uint8_t)base;
        uint8_t  cmd  = (uint8_t)(d->command - base);

        uint8_t proto;
        switch (d->access_attribute) {
        case A_BYTE:         proto = P_BYTED; break;
        case A_WORD:         proto = P_WORDD; break;
        case A_BLOCK:        proto = P_BLOCK; break;
        case A_SEND_RECEIVE: proto = P_BYTE;  break;
        case A_QUICK:        proto = P_QUICK; break;
        default:             proto = P_WORDD; break;   /* most battery fields are Word */
        }

        /* ACPI SMBus data buffer: [0]=status [1]=length [2..]=data. */
        uint8_t *buf = d->in_out_buffer.bytes;
        uint64_t blen = d->in_out_buffer.length;

        uint32_t val = 0;
        uint8_t  n = 0, status;
        if (write) {
            uint32_t w = 0;
            if (blen > 2) w = buf[2];
            if (blen > 3) w |= (uint32_t)buf[3] << 8;
            status = smb_xact(addr, cmd, proto, 1, w, UACPI_NULL, UACPI_NULL, UACPI_NULL);
        } else {
            status = smb_xact(addr, cmd, proto, 0, 0,
                              (proto == P_BLOCK && blen > 2) ? &buf[2] : UACPI_NULL,
                              &n, &val);
            if (buf && blen >= 1) buf[0] = status;
            if (buf && blen >= 2) buf[1] = (proto == P_BLOCK) ? n : 0;
            if (status == 0 && buf) {
                if (proto == P_WORDD) { if (blen > 2) buf[2] = (uint8_t)val;
                                        if (blen > 3) buf[3] = (uint8_t)(val >> 8); }
                else if (proto == P_BYTED || proto == P_BYTE) { if (blen > 2) buf[2] = (uint8_t)val; }
            }
        }
        if (write && buf && blen >= 1) buf[0] = status;

        g_calls++;
        g_last_addr = addr; g_last_cmd = cmd; g_last_proto = proto;
        g_last_status = status; g_last_data = val;
        return UACPI_STATUS_OK;   /* transport ran; SMBus error is reported in buf[0] */
    }
    default:
        return UACPI_STATUS_UNIMPLEMENTED;
    }
}

void wu_smbus_info(uint16_t *base, int *calls, uint8_t *last_addr, uint8_t *last_cmd,
                   uint8_t *last_proto, uint8_t *last_status, uint32_t *last_data)
{
    if (base)        *base        = g_base;
    if (calls)       *calls       = g_calls;
    if (last_addr)   *last_addr   = g_last_addr;
    if (last_cmd)    *last_cmd    = g_last_cmd;
    if (last_proto)  *last_proto  = g_last_proto;
    if (last_status) *last_status = g_last_status;
    if (last_data)   *last_data   = g_last_data;
}
