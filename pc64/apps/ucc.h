/* ===========================================================================
 * ucc - the UnoC compiler, public API.
 *
 * UnoC is the on-device application language of UnoDOS pc64: a C subset
 * (see docs_esp/LANG.MD) compiled straight to x86-64 machine code in a
 * .UNO module container - no assembler, no linker, no host toolchain.
 * The same sources build freestanding (inside STUDIO.UNO) and hosted
 * (tools/ucc_host.c, tools/ucc_test.c) so the compiler is unit-testable
 * on the development machine.
 *
 * The compiler owns no memory: the caller hands it a work buffer (holds
 * tokens, AST, symbol tables, section images) and an output buffer that
 * receives the finished .UNO file (header + image + relocs, exactly the
 * tools/mkuno.py layout that pc64_modload.c validates).
 * ======================================================================== */
#ifndef UCC_H
#define UCC_H

typedef struct {
    int  line, col;             /* 1-based; 0 = not location-specific     */
    char file[16];              /* source/include name (8.3), NUL-padded  */
    char msg[96];
} UccDiag;

/* Include reader: resolve `#include "NAME.H"` - return the byte count
 * copied into buf, or -1 if not found.  ctx is passed through verbatim. */
typedef long (*UccIncRead)(void *ctx, const char *name, char *buf, long max);

/* Compile UnoC source into a complete .UNO module image.
 *
 *   src/srclen   the main translation unit (need not be NUL-terminated)
 *   srcname      name recorded in diagnostics for the main unit
 *   inc/incctx   include reader (may be 0: any #include then errors)
 *   work/worklen scratch arena; 3 MB is comfortable for IDE-sized sources
 *   out/outmax   receives the .UNO file bytes
 *   diag/maxdiag receives up to maxdiag diagnostics
 *   ndiag        out: number of diagnostics recorded
 *
 * Returns the .UNO byte count on success (*ndiag == 0), else -1. */
long ucc_compile(const char *src, long srclen, const char *srcname,
                 UccIncRead inc, void *incctx,
                 void *work, long worklen,
                 unsigned char *out, long outmax,
                 UccDiag *diag, int maxdiag, int *ndiag);

/* One-line build summary of the last successful compile ("entry=... code=...
 * data=... imports=..."), valid until the next ucc_compile on this thread. */
const char *ucc_summary(void);

#endif /* UCC_H */
