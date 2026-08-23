/* unopkg - foreign packages that install as ordinary UnoDOS apps.
 *
 * Contract: pc64/UNOPKG.md.  Plan: docs/ANDROID-APPLIANCE-PLAN.md.
 *
 * THE WHOLE IDEA IN ONE SENTENCE: double-click a foreign package in Files and
 * it becomes an app - a desktop icon, a Start-menu row, a window - with no
 * launcher, no guest desktop and no second operating system visible anywhere.
 *
 * The mechanism is deliberately boring, because the app registry already did
 * the hard part: `docs/APP-REGISTRY-PLAN.md` promises that dropping a `.UNO`
 * into `APPS\` IS the whole install, and `uno_appdesc.h` says unknown
 * descriptor keys are ignored forever.  So installing a foreign package means
 * writing a `.UNO`: a tiny shim, copied from a template, whose descriptor
 * carries the app's name and icon and whose data carries the target to launch.
 * Nothing in the shell learns what an APK is.
 *
 * WHAT IS DELIBERATELY NOT HERE.  The runtime.  This file decides what an app
 * IS and where it lives; making it RUN is `uno_pkg_launch`, which hands a
 * target to whichever appliance hosts that `host:` and does nothing else.  In
 * this phase the only backend is a stub that reports "not present", which is
 * the honest answer on a machine with no appliance and is also exactly what
 * the QEMU gate can test without a hypervisor.
 */
#ifndef PC64_PKG_H
#define PC64_PKG_H

#define UNO_PKG_API 1

/* Package kinds.  The value is stable and goes in the `.PKG` sidecar. */
enum {
    UNO_PKG_NONE = 0,
    UNO_PKG_APK  = 1        /* Android, hosted by the appliance's container  */
    /* UNO_PKG_DEB / UNO_PKG_RPM follow the identical path; see UNOPKG.md */
};

/* Where the shim template lives, searched the same two ways every module is
 * (volume root first, then the installed ESP layout) - see uno_mod_find.
 * It is deliberately NOT in APPS\, because a template is not an app and a
 * scanner that found it there would give it a desktop icon. */
#define UNO_PKG_TPL_FILE "FSHIM.UNO"
#define UNO_PKG_DIR_ROOT "PKG"
#define UNO_PKG_DIR_ESP  "EFI\\UNODOS\\PKG"

/* What a probe learned about a package file, before anything is installed. */
typedef struct {
    int  kind;                  /* UNO_PKG_*                                 */
    char id[16];                /* app-registry identity, [a-z0-9._-]        */
    char name[32];              /* what the desktop icon will say            */
    char target[128];           /* what uno_pkg_launch is given later        */
    char version[24];           /* "" when the package does not say          */
    long size;                  /* bytes on disk                             */
    int  arch_ok;               /* 1 = this machine's ISA is in the package  */
    char arch[32];              /* what it DOES carry, for the refusal text  */
} uno_pkg_info;

/* Read `path` on `vol` and fill `out`.
 *  1 = understood and installable
 *  0 = understood and REFUSED (out->arch/name explain why; today: no x86_64)
 * -1 = not a package this build knows
 * Reads at most a few hundred KB and allocates only through um_inflate. */
int uno_pkg_probe(int vol, const char *path, uno_pkg_info *out);

/* Install what `uno_pkg_probe` described.  Copies the package beside its
 * sidecar, writes the shim, and rescans so the icon appears without a reboot.
 * `progress` may be NULL; pct is 0..100, msg is short and user-facing.
 * 1 on success; 0 on failure with `err` (may be NULL) filled. */
int uno_pkg_install(int vol, const char *path, const uno_pkg_info *info,
                    void (*progress)(int pct, const char *msg),
                    char *err, int errmax);

/* Remove an installed foreign app: the shim, the sidecar and the package.
 * 1 = removed, 0 = nothing of that id was installed. */
int uno_pkg_remove(const char *id);

/* 1 if `id` names a foreign app this machine has installed. */
int uno_pkg_installed(const char *id);

/* ---- what the shim calls (exported through pc64_modload.c's kExports) ---- */

/* Ask the host to run `target` (the string the shim carries).  Returns 1 when
 * the runtime accepted it, 0 when it could not, and fills `msg` either way
 * with one sentence fit to put in a window.  NEVER blocks: a runtime that has
 * to be started reports "starting" and the shim asks again next frame. */
int uno_pkg_launch(const char *target, char *msg, int msgmax);

/* One line describing the runtime that would host `target`, for the shim's
 * window and for Settings.  Always writes something. */
void uno_pkg_runtime_str(const char *target, char *buf, int max);

#endif /* PC64_PKG_H */
