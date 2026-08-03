/* ===========================================================================
 * uofile - the Office 97 Open / Save As dialog.            [EXPERIMENTAL]
 * (OFFICE97-PLAN §5 phase 6d; SPEC S-OFF-05.)
 *
 * Office 97 shipped its OWN file dialogs rather than the raw common dialog -
 * a Look-in combo, the file list, a name field, a file-type combo and the
 * view buttons - and every app in the suite got the same one.  So does this.
 *
 * It is a uodlg dialog like any other; what it adds is that its list is not
 * static data.  The contents come through a FILESYSTEM SEAM: pc64 installs
 * one wrapping uno_fs_*, the host gate installs a fake, and uofile.c never
 * learns which.  That is what makes the whole thing testable without booting
 * the OS - the same trick unodoc's ud_src plays.
 * ======================================================================== */
#ifndef UOFILE_H
#define UOFILE_H

#include "uodlg.h"

typedef struct {
    int         (*volumes)(void);
    const char *(*volume_name)(int vol);
    int         (*list_begin)(int vol);                  /* how many entries */
    int         (*list_get)(int vol, int i, char *name, int cap);
    int         (*is_dir)(int vol, const char *name);
} uof_fs;

/* Install the filesystem the dialog browses.  Idempotent; with none
 * installed the dialog opens and honestly shows nothing. */
void uof_set_fs(const uof_fs *fs);

/* Open the dialog.  `save` picks the Save As wording and button; `types` is
 * the "Files of type" list (e.g. "Word Document (*.doc)"). */
void uof_open(uod_ui *s, int save, const char *const *types, int ntypes,
              int sw, int sh);

/* Call after every uod_handle while a uofile dialog is up: it repopulates the
 * list when the volume changes and mirrors a picked row into the name field.
 * Keeping this OUT of uod_handle is deliberate - the dialog engine stays a
 * pure control layer that knows nothing about files. */
void uof_sync(uod_ui *s);

/* Once uod_result() is UOD_ID_OK: what was chosen. */
int         uof_volume(void);
const char *uof_name(void);
int         uof_type(void);

#endif /* UOFILE_H */
