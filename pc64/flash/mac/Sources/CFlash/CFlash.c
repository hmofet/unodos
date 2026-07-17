#include "CFlash.h"
#include <Security/Security.h>

/* AuthorizationExecuteWithPrivileges has been deprecated since 10.7 but remains
 * the only one-shot "run this as root after an admin prompt" API that doesn't
 * require a full SMJobBless/SMAppService privileged-helper install with a
 * Developer ID. For a locally-signed, self-distributed personal tool (same
 * footing as the Slate Mac app) it's the pragmatic, faithful match to the
 * Windows flasher's UAC elevation. */
int uno_authorized_exec(const char *tool, const char *arg1, const char *arg2,
                        FILE **outpipe)
{
    AuthorizationRef auth = NULL;
    OSStatus st = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
                                      kAuthorizationFlagDefaults, &auth);
    if (st != errAuthorizationSuccess)
        return st;

    char *args[] = { (char *)arg1, (char *)arg2, NULL };

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    st = AuthorizationExecuteWithPrivileges(auth, tool,
                                            kAuthorizationFlagDefaults,
                                            args, outpipe);
#pragma clang diagnostic pop

    AuthorizationFree(auth, kAuthorizationFlagDefaults);
    return st;
}
