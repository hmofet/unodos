/* detachgate - can this machine leave the firmware, and if not, WHICH device
 * is stopping it?  See pc64/DETACH.md.
 *
 * Every question here is asked while the firmware is still alive and answered
 * about a world that does not exist yet, which is the whole difficulty: past
 * ExitBootServices there is no way back, so a service that turns out not to
 * survive cannot be recovered, only reported.  The gate therefore predicts,
 * from the firmware's own descriptors, what our native drivers will be able to
 * claim - the same method usbboot.c uses for the boot volume.
 *
 * This file owns the INPUT half of that prediction (keyboard + pointer) and the
 * blocker attribution used by the System window.  Storage keeps its own
 * answers in usbboot.c / fat.c; try_detach() composes the three.
 */
#ifndef UNO_DETACHGATE_H
#define UNO_DETACHGATE_H

/* Bumped on any breaking change to the surface below, with a dated DETACH.md
 * changelog entry (AGENTS.md §6).  1 = the input gate + blocker attribution. */
#define UNO_DETACHGATE_API 1

/* Which transport backs a firmware input device, read from its device path.
 * The distinction is the point: a firmware pointer on the i8042 aux port is
 * one uno_ps2_init() takes over at detach, and a firmware pointer on an LPSS
 * I2C controller we failed to bind is one that dies with the firmware. */
enum {
    UNO_DGT_ABSENT = 0,  /* the firmware publishes no such device           */
    UNO_DGT_PS2,         /* ACPI PNP03xx / PNP0Fxx node: the i8042          */
    UNO_DGT_USB,         /* a USB() messaging node                          */
    UNO_DGT_OTHER,       /* a readable path that is neither (I2C, vendor)   */
    UNO_DGT_UNKNOWN      /* no readable device path (a firmware aggregate)  */
};

/* What backs the firmware's pointer / keyboard right now (UNO_DGT_*).  Latched
 * on first call while boot services live; a reader after detach gets the
 * latched value. */
int uno_dg_fw_ptr_transport(void);
int uno_dg_fw_kbd_transport(void);

/* Will a POINTER / KEYBOARD exist on our own drivers after ExitBootServices?
 *
 * Both count a device that is already bound (I2C-HID) AND one we can prove we
 * will bind at detach: a USB HID boot device the native stack can reach, or an
 * i8042 aux mouse the firmware is currently driving.  That last arm is what
 * clears the ThinkPad X1 Carbon, whose TrackPoint is a PS/2 Elan reached
 * through firmware SimplePointer: the aux port cannot be interrogated while
 * the firmware owns the controller, but its device path names it exactly. */
int uno_dg_ptr_survives(void);
int uno_dg_kbd_survives(void);

/* Would detaching leave this machine with NO pointer at all?  False when the
 * firmware has no pointer either (nothing to lose) or when one survives. */
int uno_dg_would_strand_pointer(void);

/* A registry device rendered short: "00:15.0 8086:34e8 i2c", or "" when the
 * device manager is not in this build or has no such device.  cls/sub are the
 * PCI class triple's first two bytes. */
const char *uno_dg_dev_str(unsigned char cls, unsigned char sub);

/* The device holding this machine attached, in the same short form, or "".
 * Set by whichever gate refused; read by the System window and the env block
 * so a glance at any machine explains itself (Phase D). */
const char *uno_dg_blocker(void);
void uno_dg_set_blocker(const char *dev);

/* One line of diagnostics for the boot env block / the `devices` verb:
 *   "fw ptr=ps2 kbd=usb  survives ptr=1 kbd=1  blocker=00:15.0 8086:34e8 i2c"
 * Returns the length written, excluding the NUL. */
int uno_dg_status_str(char *buf, int cap);

/* Post-detach honesty check: did the pointer we PREDICTED actually turn up?
 * Returns 0 when a pointer was promised and none arrived, and names it in
 * *why.  A stranded pointer is not fatal the way a stranded boot volume is
 * (the shell is keyboard-driven), but a machine that says so beats one that
 * silently lost its mouse. */
int uno_dg_ptr_arrived(const char **why);

#endif /* UNO_DETACHGATE_H */
