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

/* Did the BOOT device itself come back on native drivers?
 *
 * Not "is some mass-storage device bound" - the boot one, matched by the USB
 * id usbboot latched before ExitBootServices. On a USB-booted machine this is
 * the only trustworthy answer to "is the system volume still ours", because
 * the generic test (a volume carrying \EFI\BOOT\BOOTX64.EFI on a natively
 * reachable controller) is satisfied by ANY other operating system's ESP -
 * a Windows Boot Manager has an MZ header like everything else. */
int uno_usbmsc_boot_bound(void);

#endif
