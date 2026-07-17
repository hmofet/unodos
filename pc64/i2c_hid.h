/* ===========================================================================
 * UnoDOS/pc64 - native I2C-HID trackpad driver (foundation).
 *
 * Modern laptop trackpads (2015+, incl. the X1 Carbon Gen 8) are I2C-HID
 * devices behind an Intel LPSS DesignWare I2C controller, described in ACPI.
 * A native driver replaces the firmware's (jerky) Absolute Pointer with direct
 * multi-touch control. This file has the two REUSABLE, spec-defined layers -
 * the DesignWare I2C master and HID-over-I2C - plus a first-pass report parser.
 *
 * STATUS: unverifiable in QEMU (no emulated I2C touchpad), so it is gated
 * behind -DUNO_I2C_TRACKPAD and OFF by default (the shipped build is
 * untouched). Bring-up on real hardware needs two device facts that normally
 * come from ACPI/AML (which we don't parse yet); supply them from Linux on the
 * same machine (see the CONFIG block in i2c_hid.c) and iterate the parser on
 * real report dumps (uno_i2c_hid_dump()).
 * ======================================================================== */
#ifndef PC64_I2C_HID_H
#define PC64_I2C_HID_H

/* try to bring the trackpad up; returns 1 if a device answered. No-op (0)
   when UNO_I2C_TRACKPAD is not defined. Safe to call on any machine - bounded,
   never hangs, inert if no LPSS I2C controller / device is found. */
int  uno_i2c_hid_init(void);

/* poll one input report; on a movement/button report, writes absolute
   position (0..32767 range) + button mask and returns 1. */
int  uno_i2c_hid_poll(int *absx, int *absy, int *buttons);

/* debug: copy the last raw input report (for nailing the parser). Returns
   its length. */
int  uno_i2c_hid_dump(unsigned char *out, int max);

int  uno_i2c_hid_present(void);

/* diagnostics: how many candidate BARs were seen, how many were confirmed
   DesignWare I2C controllers, whether a HID device answered, its slave addr,
   and whether its report descriptor parsed. Any pointer may be NULL. Lets the
   System app surface WHY the trackpad did / didn't come up. */
void uno_i2c_hid_status(int *nbars, int *nctrl, int *present, int *addr, int *parsed);
/* extra probe diagnostics: saw_ack = a transfer returned bytes (bus alive + a
 * device answered); abrt = last DW TX_ABRT_SOURCE (bit7 = address NAK). */
void uno_i2c_hid_diag(int *saw_ack, unsigned *abrt);

#endif
