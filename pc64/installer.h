/* installer.h - install UnoDOS/pc64 to a local disk (see installer.c).
 *
 * Two target kinds, chosen by what the user selects:
 *   ESP volume  - NON-DESTRUCTIVE: copy the running system into \EFI\UNODOS\
 *                 on an existing FAT/ESP volume (e.g. next to Windows) and add
 *                 a "UnoDOS" UEFI boot entry.  Nothing is deleted.
 *   whole disk  - DESTRUCTIVE: clone the boot USB's GPT + ESP onto the disk
 *                 (everything on it is lost), relocate the backup GPT to the
 *                 disk's real end, and add the boot entry.
 */
#ifndef PC64_INSTALLER_H
#define PC64_INSTALLER_H

#define UNO_INST_ESP  0
#define UNO_INST_DISK 1

/* Enumerate install targets (rescans firmware handles).  Returns the count
 * (<= 12).  The boot USB itself is never listed. */
int uno_inst_scan(void);

/* Display line / kind for target i (valid until the next scan). */
const char *uno_inst_desc(int i);
int         uno_inst_kind(int i);
int         uno_inst_usable(int i);        /* 0 = listed but refused (why in desc) */

/* Install to target i.  make_default: put UnoDOS first in BootOrder (else
 * append).  progress may be NULL; pct is 0..100.  Returns 1 on success; on
 * failure uno_inst_error() has a short reason. */
int uno_inst_install(int i, int make_default,
                     void (*progress)(int pct, const char *msg));

const char *uno_inst_error(void);

#endif
