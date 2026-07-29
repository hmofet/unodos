/* USB mass storage (Bulk-Only Transport) over xHCI - see usbmsc.c.
 * P4: lets a USB boot volume survive detach (F8). Registers with blkdev. */
#ifndef PC64_USBMSC_H
#define PC64_USBMSC_H

int uno_usbmsc_supported(void);   /* 1 = the xHCI stack is compiled in       */
int uno_usbmsc_init(void);        /* post-detach: claim BOT device, register */
/* How the last bring-up went, in words. Post-detach there is no way back, so
 * when the boot stick does not come home this string is the only account of
 * why - and it has to exist in production, where the debug log does not. */
const char *uno_usbmsc_why(void);

#endif
