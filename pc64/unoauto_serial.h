/* ===========================================================================
 * UnoDOS/pc64 - UNOAUTOMATE 16550 UART transport backend for the remote channel.
 * A NIC-independent URC carrier: the same URC line protocol as the TCP link
 * (see REMOTE.md) over a PC serial port, so a box whose only network is the NIC
 * being debugged can still be driven live.  UNO_DEBUG-only, like the rest of
 * unoautomate.  See UNOAUTOMATE-REQUESTS.md (2026-07-22 r8169 Request 2).
 * ======================================================================== */
#ifndef UNOAUTO_SERIAL_H
#define UNOAUTO_SERIAL_H

#ifdef UNO_DEBUG

/* Bring up a polled 16550 at I/O `base` (0 = COM1 0x3F8), `baud` (0 = 115200),
 * 8 data bits, no parity, 1 stop, FIFOs on, interrupts off. */
void uart_init(unsigned base, unsigned baud);

/* Push up to `n` bytes into the TX FIFO; returns the count actually written
 * (< n when the FIFO fills - never spins, so a stalled peer can't hang us). */
int  uart_write(const char *buf, int n);

/* Pull whatever the RX FIFO holds, up to `cap` bytes; returns the count. */
int  uart_read(unsigned char *buf, int cap);

#endif /* UNO_DEBUG */
#endif /* UNOAUTO_SERIAL_H */
