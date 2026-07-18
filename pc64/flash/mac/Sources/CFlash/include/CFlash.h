#ifndef CFLASH_H
#define CFLASH_H

#include <stdio.h>

/* Run `tool arg1 arg2` as root via Authorization Services, prompting the user
 * for an administrator password (the macOS analogue of a Windows UAC prompt).
 * On success returns 0 (errAuthorizationSuccess) and sets *outpipe to a FILE*
 * connected to the child's stdout (read the writer's progress lines from it).
 * Returns the OSStatus otherwise (e.g. errAuthorizationCanceled == -60006 when
 * the user dismisses the password dialog). */
int uno_authorized_exec(const char *tool, const char *arg1, const char *arg2,
                        FILE **outpipe);

#endif /* CFLASH_H */
