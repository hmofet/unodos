/* ===========================================================================
 * UNOAUTOMATE - 16550 UART transport backend for the remote channel.
 *
 * A NIC-independent URC transport: the same newline-delimited URC line protocol
 * (see REMOTE.md) carried over a PC 16550-compatible serial port instead of TCP.
 * This lets a box whose ONLY network is the NIC being debugged (e.g. a ZimaBlade
 * whose onboard Realtek is down) still be driven live over URC - register pokes,
 * `rerun`, `probe` - through a USB-serial cable, with no reflash per change.
 * Answers Request 2 of the 2026-07-22 r8169 entry in UNOAUTOMATE-REQUESTS.md.
 *
 * Polled and non-blocking: uart_write drains the software queue into the TX
 * FIFO only as far as it fits (it never spins on a full FIFO, so a detached host
 * cannot hang the shell frame); uart_read pulls whatever the RX FIFO holds. The
 * URC pump in unoauto_remote.c calls both once per shell frame. The key
 * correctness point vs a naive port: uart_write checks THRE before EVERY byte,
 * so it can never overrun the holding register and drop bytes - a dropped byte
 * would desync the line framing and strand a `RSP ... end`, hanging the host.
 *
 * NOTE: intended for interactive/low-rate control (the `eth`/`iwl`/`probe`/drive
 * verbs).  A 16550 has only a 16-byte RX FIFO, so a sustained inbound flood (a
 * multi-MB `put`) can overrun it - do A/B kernel pushes over the TCP transport.
 * Register debug and drive verbs are short and safe.  UNO_DEBUG-only.
 * ======================================================================== */
#include "unoauto_serial.h"

#ifdef UNO_DEBUG

/* 16550 register offsets from the port base. */
enum {
    R_RBR = 0,   /* DLAB=0: receive buffer (read) / THR transmit (write) */
    R_THR = 0,
    R_IER = 1,   /* DLAB=0: interrupt enable                             */
    R_DLL = 0,   /* DLAB=1: divisor latch low                           */
    R_DLM = 1,   /* DLAB=1: divisor latch high                          */
    R_FCR = 2,   /* FIFO control (write)                                */
    R_LCR = 3,   /* line control                                        */
    R_MCR = 4,   /* modem control                                       */
    R_LSR = 5    /* line status                                         */
};
#define LSR_DR   0x01   /* data ready: the RX FIFO holds at least one byte */
#define LSR_THRE 0x20   /* transmit holding register empty: room to write  */

static inline void io_out8(unsigned port, unsigned char v)
{ __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"((unsigned short)port)); }
static inline unsigned char io_in8(unsigned port)
{ unsigned char v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"((unsigned short)port)); return v; }

static unsigned g_base = 0x3F8;    /* COM1 by default; set by uart_init */

void uart_init(unsigned base, unsigned baud)
{
    unsigned div;
    if (!base) base = 0x3F8;
    if (!baud) baud = 115200;
    div = 115200u / baud; if (!div) div = 1;
    g_base = base;
    io_out8(base + R_IER, 0x00);      /* polled - no UART interrupts            */
    io_out8(base + R_LCR, 0x80);      /* DLAB=1: divisor latch accessible       */
    io_out8(base + R_DLL, (unsigned char)(div & 0xFF));
    io_out8(base + R_DLM, (unsigned char)((div >> 8) & 0xFF));
    io_out8(base + R_LCR, 0x03);      /* DLAB=0, 8 data bits, no parity, 1 stop */
    io_out8(base + R_FCR, 0xC7);      /* FIFO on, clear RX+TX, 14-byte trigger  */
    io_out8(base + R_MCR, 0x03);      /* DTR + RTS asserted                     */
}

int uart_write(const char *buf, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!(io_in8(g_base + R_LSR) & LSR_THRE)) break;  /* FIFO full - retry later */
        io_out8(g_base + R_THR, (unsigned char)buf[i]);
    }
    return i;
}

int uart_read(unsigned char *buf, int cap)
{
    int i = 0;
    while (i < cap && (io_in8(g_base + R_LSR) & LSR_DR))
        buf[i++] = io_in8(g_base + R_RBR);
    return i;
}

#endif /* UNO_DEBUG */
