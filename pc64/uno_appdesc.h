/* ===========================================================================
 * UnoDOS/pc64 - the app descriptor: what a `.UNO` says about itself.
 *
 * A module used to be invisible to the shell until somebody hand-wrote a slot
 * for it in `pc64_uui.c` - a name, a short name, an icon, a presence probe and
 * seven hook dispatch sites, about a dozen edits in a file the adding lane
 * usually does not own.  `APPS\VMGR.UNO` shipped for a day with no slot and no
 * way to run it at all, which is what settled the design (see
 * `docs/APP-REGISTRY-PLAN.md`).
 *
 * So a module now CARRIES its launcher metadata.  The block lives inside the
 * module image, in its own `.unodesc` section, and `UnoModHdr.desc_rva` points
 * at it.  Two properties matter and both are deliberate:
 *
 *   - Reading it costs TWO SECTOR READS and executes nothing.  The shell can
 *     enumerate every app on the disk without loading one: the module arena is
 *     4 MB and never frees, and a 300 KB module is ~1.1 s of single-sector I/O.
 *   - It is compatible in BOTH directions.  `desc_rva` was the header's unused
 *     `rsv` word, so an older kernel ignores it and loads a new module exactly
 *     as before, and a newer kernel meeting an old module sees `desc_rva == 0`
 *     and falls back to defaults derived from the filename.  (Appending the
 *     block after the reloc table was the obvious alternative and is wrong:
 *     `mod_instantiate` requires `48 + file_size + 4*nreloc == n` EXACTLY, so a
 *     trailing block would make every new module unloadable on every older
 *     kernel.)
 *
 * On-disk block at file offset `48 + desc_rva`:
 *
 *     u32 magic 'UAPP'   u16 ver (1)   u16 len (whole block, <= 1024)
 *     char body[len - 8]     LF-terminated "key: value" lines, ASCII
 *
 * Body keys, every one optional, unknown keys IGNORED - that is the extension
 * point, and the same shape as the `<APP>.MFT` capability manifest so an author
 * who has written one already knows the syntax:
 *
 *     id:     vmgr           stable identity, [a-z0-9._-], <= 15 chars.
 *                            THE key: everything durable is keyed by it.
 *     name:   Appliances     launcher / taskbar / window-title label
 *     short:  Appliances     desktop-icon label (defaults to name)
 *     icon:   sys            named emblem; an unknown name gets PCI_GENERIC
 *     cat:    system         Start-menu section (see UAC_* below)
 *     rank:   50             sort key within the section, default 100
 *     flags:  singleton      comma list; unknown values ignored
 *     min:    560x380        preferred window size
 *     needs:  net,fs.sys     ADVISORY only - the enforced grant is the signed
 *                            .MFT manifest, never this.
 * ======================================================================== */
#ifndef UNO_APPDESC_H
#define UNO_APPDESC_H

#define UNO_APPDESC_MAGIC 0x50504155u      /* 'UAPP', little-endian */
#define UNO_APPDESC_VER   1
#define UNO_APPDESC_MAX   1024             /* cap on the whole block */

/* Start-menu sections.  Ordering here IS the menu's section order. */
enum { UAC_SYSTEM = 0, UAC_NET, UAC_TOOLS, UAC_MEDIA, UAC_GAMES, UAC_OTHER,
       UAC_NCAT };

/* `flags:` values */
#define UAF_SINGLETON 0x0001   /* a second launch focuses the first window   */
#define UAF_HIDDEN    0x0002   /* registered + launchable by id, no icon/row */
#define UAF_GAME      0x0004   /* fullscreen-preferred (cf. unoapp_is_game)  */
#define UAF_NOSESSION 0x0008   /* never restored by SHELL.CFG                */
/* Not a `flags:` value a module may declare: pinning is the USER's decision,
 * set in APPS.CFG (`pin.<id>=1`).  An app that could pin itself to the taskbar
 * by shipping a line in its own descriptor would, and then the bar would be
 * whatever was installed last rather than what its owner chose. */
#define UAF_PINNED    0x0100

/* the parsed form the shell and the loader pass around */
typedef struct UnoAppDesc {
    char id[16];               /* always non-empty: filename stem if unstated */
    char name[32];
    char shortnm[16];
    char icon[16];             /* emblem NAME, never an index (pc64_icons.h)  */
    unsigned char  cat;        /* UAC_*                                       */
    unsigned char  rank;
    unsigned short flags;      /* UAF_*                                       */
    unsigned short tier;       /* UnoModHdr.flags, so the reader learns the   */
                               /* hosting tier from the same probe            */
    unsigned char has_desc;    /* 1 = the module carried a block; 0 = every   */
                               /* field above was derived from the filename   */
    short pref_w, pref_h;      /* 0 = the shell's default size                */
} UnoAppDesc;

/* the on-disk prologue */
typedef struct UnoAppDescHdr {
    unsigned int   magic;
    unsigned short ver, len;
} UnoAppDescHdr;

/* ---- module side: declare your app -----------------------------------------
 * One of these next to your `UnoUuiApp`, e.g.
 *
 *     UNO_APP_DESC("id: vmgr\n"
 *                  "name: Appliances\n"
 *                  "icon: sys\n"
 *                  "cat: system\n");
 *
 * `used` keeps it past the compiler even though nothing references it, and
 * `mkuno.py convert` fails the BUILD on a malformed body or an unknown
 * category - the same bargain as build.sh's kExports import check, which earns
 * its keep by turning a module that loads and jumps into nothing into a
 * compile error. */
/* The attribute goes on the DECLARATOR, not after the struct's closing brace:
 * there it would attach to the anonymous TYPE, `section` would be quietly
 * dropped, and the block would vanish into .rdata with nothing to find it by.
 * That is exactly what the first version of this macro did. */
#define UNO_APP_DESC(body)                                                  \
    static const struct {                                                   \
        UnoAppDescHdr h;                                                    \
        char          b[sizeof(body)];                                      \
    } uno_app_desc_block                                                    \
    __attribute__((section(".unodesc"), used, aligned(4))) = {              \
        { UNO_APPDESC_MAGIC, UNO_APPDESC_VER,                               \
          (unsigned short)(sizeof(UnoAppDescHdr) + sizeof(body)) },         \
        body                                                                \
    }

#endif /* UNO_APPDESC_H */
