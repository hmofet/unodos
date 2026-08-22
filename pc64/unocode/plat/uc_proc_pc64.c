/*
 * VENDORED FILE - DO NOT EDIT HERE.
 *
 * UnoCode is developed at https://github.com/hmofet/unocode-desktop, in its core/ directory.
 * An edit made here is lost at the next sync, and until then it silently
 * forks the editor away from the tree the desktop builds are cut from.
 *
 * Change it there; bring it back with pc64/tools/sync_unocode.py.
 * See pc64/UNOCODE-UPSTREAM.md.
 */
/* ===========================================================================
 * uc_proc_pc64.c - uc_proc.h answered by UnoDOS (UCD-14).
 *
 * The answer is no, and that is the whole file.
 *
 * pc64 has no process model: an app is a module loaded into the one address
 * space, and there is nothing to fork, no second stdout to read and nothing to
 * send a signal to.  So uc_proc_available() says 0 and the terminal keeps its
 * builtins rather than offering `git status` and then explaining itself.
 *
 * That is not a placeholder waiting to be filled in.  A real answer here would
 * be a process model for the OS, which is an enormous piece of work with its
 * own reasons for and against - not a detail of the editor's terminal.  What
 * the editor gets instead is a capability question it already knows how to
 * ask, and pc64_shell_run_user() for the one thing this machine CAN do:
 * launch a UnoDOS app.
 *
 * Lives in core/plat/ rather than beside the uc_*.c files because the desktop
 * build globs core/uc_*.c - see uc_net_pc64.c for the same reasoning.
 * ======================================================================== */
#include "uc_proc.h"

int uc_proc_available(void) { return 0; }

const char *uc_proc_shell_name(void) { return ""; }

const char *uc_proc_error(void)
{
    return "UnoDOS has no process model - the terminal's own commands are "
           "what runs here";
}

uc_proc *uc_proc_spawn(const char *cmdline, const char *cwd)
{
    (void)cmdline; (void)cwd;
    return 0;
}

uc_proc *uc_proc_spawn_pipes(const char *cmdline, const char *cwd)
{
    (void)cmdline; (void)cwd;
    return 0;
}

int uc_proc_read_err(uc_proc *p, char *buf, int cap)
{ (void)p; (void)buf; (void)cap; return -1; }

int uc_proc_workdir(int vol, const char *dir, char *out, int cap)
{
    (void)vol; (void)dir;
    if (cap > 0) out[0] = 0;
    return 0;                    /* nothing to start, so nowhere to start it */
}

int uc_proc_read(uc_proc *p, char *buf, int cap)
{ (void)p; (void)buf; (void)cap; return -1; }

int uc_proc_write(uc_proc *p, const char *s, int n)
{ (void)p; (void)s; (void)n; return 0; }

void uc_proc_resize(uc_proc *p, int cols, int rows)
{ (void)p; (void)cols; (void)rows; }

void uc_proc_interrupt(uc_proc *p) { (void)p; }

int uc_proc_alive(uc_proc *p) { (void)p; return 0; }

int uc_proc_exit_code(uc_proc *p) { (void)p; return -1; }

void uc_proc_free(uc_proc *p) { (void)p; }
