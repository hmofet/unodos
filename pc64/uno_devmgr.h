/* unodevices - PCI/USB device tree + driver registry.  See DEVICES.md.
 *
 * Phase 1: full PCI enumeration + a text dump for introspection (answers
 * "what hardware is here, and what has no driver?").  Owns discovery; consumes
 * pc64_pci.c's config accessors (does not edit that shared file). */
#ifndef UNO_DEVMGR_H
#define UNO_DEVMGR_H

/* Enumerate every PCI function on the machine and write a human-readable list
 * into buf (always NUL-terminated).  One line per device:
 *
 *   "bb:dd.f VVVV:DDDD cc/ss <class-name>"
 *
 * (bus:dev.fn, vendor:device, class/subclass hex, short class name).
 * Returns the string length written (excluding the NUL), truncated to fit cap. */
int devmgr_list_str(char *buf, int cap);

#endif /* UNO_DEVMGR_H */
