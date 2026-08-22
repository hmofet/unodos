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
 * uc_proc.h - running a child process, and the whole of the editor's opinion
 * about it (UCD-14).
 *
 * THE SEAM EXISTS BECAUSE ONE PLATFORM HAS NO PROCESSES AT ALL.  UnoDOS/pc64
 * has no process model to host a shell in; the desktop has two different ones
 * (ConPTY and a POSIX pty).  So the editor asks uc_proc_available() first and
 * offers what the answer allows - the same rule the run tool follows in the
 * assistant, and the same reason pc64_shell_can_run() exists.  A terminal that
 * offered to run `git status` and then explained it could not would be worse
 * than one that never offered.
 *
 * IT IS A PTY WHERE IT CAN BE, NOT A PIPE.  A child on a pipe sees a
 * non-terminal stdout and behaves differently for it: no colour, no progress
 * line, block buffering that makes a build look hung for a minute and then
 * finish instantly.  The point of this task is a terminal that behaves like
 * one, so the child gets a terminal.
 *
 * EVERY CALL IS NON-BLOCKING, for the reason every seam here is: the editor
 * has a frame to draw and a `make` to survive.  read() returns what has
 * arrived, which is usually nothing, and that is not an error.
 * ======================================================================== */
#ifndef UC_PROC_H
#define UC_PROC_H

typedef struct uc_proc uc_proc;

/* 1 when this platform can actually start a child.  pc64 answers 0 and the
 * terminal keeps its builtins; the desktop answers 1. */
int uc_proc_available(void);

/* The name of the shell a bare `sh` would start here, for the UI to say.
 * Never NULL; "" when nothing can be started. */
const char *uc_proc_shell_name(void);

/* Start `cmdline` through the platform's shell, with `cwd` as its working
 * directory (0 = inherit).  NULL if it could not start, with the reason in
 * uc_proc_error().  A NULL or empty cmdline starts an INTERACTIVE shell. */
uc_proc *uc_proc_spawn(const char *cmdline, const char *cwd);

/* Turn a (volume, directory) - which is how the editor addresses everything -
 * into a working directory a child can be started in.  1 if it filled `out`.
 *
 * It is in THIS seam rather than the filesystem one because "where does a
 * child start" is a process question, and because the answer only exists on a
 * platform that has children: pc64 answers 0.  Without it the shell inherits
 * the EDITOR's directory, so a build task ran `gcc main.c` somewhere that had
 * no main.c and reported a missing file rather than a compile error. */
int uc_proc_workdir(int vol, const char *dir, char *out, int cap);
const char *uc_proc_error(void);

/* Take whatever output has arrived.  Returns bytes copied, 0 if none is
 * waiting - which is the ordinary answer between frames, not an error - or a
 * negative value when the child has ended AND its output is drained.  Read
 * until it goes negative before asking for the exit code. */
int uc_proc_read(uc_proc *p, char *buf, int cap);

/* Send input (keystrokes) to the child.  Returns bytes accepted. */
int uc_proc_write(uc_proc *p, const char *s, int n);

/* Tell the child its window is this many columns and rows.  A pty child that
 * is never told wraps at 80 for ever. */
void uc_proc_resize(uc_proc *p, int cols, int rows);

/* Ctrl+C: ask the child to stop, the way a terminal does - the whole
 * foreground group, not just the shell, or `make` keeps its compiler. */
void uc_proc_interrupt(uc_proc *p);

int uc_proc_alive(uc_proc *p);
int uc_proc_exit_code(uc_proc *p);       /* valid once read() went negative */

/* Kill the child if it is still running, then release the handle.  Closing
 * the panel calls this: a terminal that left its build running after the
 * window went away would be a leak nobody could see to fix. */
void uc_proc_free(uc_proc *p);

#endif
