/* compat/uno_appdesc.h - the app registry's descriptor header, for the
 * desktop build.
 *
 * UNO_APP_DESC places the launcher block in a ".unodesc" section.  That is a
 * valid name for PE and ELF, but Mach-O section names are "segment,section"
 * and clang rejects the bare form outright - so on a Mac the unmodified app
 * would not compile.  The header belongs to the app-registry lane, so rather
 * than editing it this directory sits AHEAD of pc64/ on the include path,
 * pulls the real header in, and re-spells the one macro for Mach-O.  The
 * desktop shell never reads the block; it only has to link. */
#include "../../../uno_appdesc.h"

#if defined(__APPLE__) && defined(UNO_APP_DESC)
#undef UNO_APP_DESC
#define UNO_APP_DESC(body)                                                  \
    static const struct {                                                   \
        UnoAppDescHdr h;                                                    \
        char          b[sizeof(body)];                                      \
    } uno_app_desc_block                                                    \
    __attribute__((section("__DATA,__unodesc"), used, aligned(4))) = {      \
        { UNO_APPDESC_MAGIC, UNO_APPDESC_VER,                               \
          (unsigned short)(sizeof(UnoAppDescHdr) + sizeof(body)) },         \
        body                                                                \
    }
#endif
