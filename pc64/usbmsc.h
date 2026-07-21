/* USB mass storage (Bulk-Only Transport) over xHCI - see usbmsc.c.
 * P4: lets a USB boot volume survive detach (F8). Registers with blkdev. */
#ifndef PC64_USBMSC_H
#define PC64_USBMSC_H

int uno_usbmsc_supported(void);   /* 1 = the xHCI stack is compiled in       */
int uno_usbmsc_init(void);        /* post-detach: claim BOT device, register */

#endif
