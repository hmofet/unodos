/* ===========================================================================
 * UnoDOS/PS2 native module loader  -  the EE implementation of the platform
 * hook uno_load_module(proc).  App modules are SEPARATE artifacts stored on the
 * PS2 memory card at  mc0:/UnoDOS/Apps/appNN.uno  and loaded at runtime.
 *
 * The .uno images are the app modules' RELOCATABLE ELF objects (ET_REL, the
 * mips64r5900el-ps2-elf-gcc -c output, packaged by `make package`).  This file
 * is a REAL EE overlay loader: it reads the image off mc0:, lays the allocated
 * sections (.text/.data/.rodata/.bss) into EE RAM, applies the MIPS
 * relocations (R_MIPS_32 / R_MIPS_26 / R_MIPS_HI16 / R_MIPS_LO16), resolves the
 * module's undefined symbols against the kernel's exported services (the
 * Mac-compat Toolbox + the handful of libc helpers the apps use), flushes the
 * cache, and returns the relocated `uno_app_main_<name>` entry to dispatch.
 *
 * STATUS (honest):
 *   - UNO_EE_OVERLAY = 1 (default): the entry returned by uno_load_module() is
 *     the one RELOCATED from the mc0: image - the app's code genuinely executes
 *     from the copy read off the memory card, not from the registry.  If any
 *     step fails (image missing, unknown reloc/symbol, alloc failure) the
 *     loader logs why and falls back to the linked-in registry entry so the
 *     desktop still runs - i.e. the storage read is load-bearing when it works
 *     and degrades gracefully when it can't.
 *   - UNO_EE_OVERLAY = 0: pure linked-in registry (the original conservative
 *     behaviour) - the storage path is exercised (image is read) but the entry
 *     comes from the linked-in symbol.
 *
 * The .uno objects depend only on the KernelApi table (passed at uno_app_main)
 * plus a small fixed set of Toolbox/libc symbols; gKernelSyms below is that
 * resolver, taken by &address so it survives relinking (no hardcoded values).
 * ===========================================================================
 */
#include "uno_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UNO_EE_OVERLAY
#  define UNO_EE_OVERLAY 1
#endif

/* module images live here on the card (packaged by build.sh / make package) */
#define UNO_APPS_MC "mc0:/UnoDOS/Apps"

/* the linked-in entries (fallback + UNO_EE_OVERLAY=0 path) */
extern const AppInterface *uno_app_main_sysinfo(const KernelApi *);
extern const AppInterface *uno_app_main_clock  (const KernelApi *);
extern const AppInterface *uno_app_main_files  (const KernelApi *);
extern const AppInterface *uno_app_main_notepad(const KernelApi *);
extern const AppInterface *uno_app_main_music  (const KernelApi *);
extern const AppInterface *uno_app_main_dostris(const KernelApi *);
extern const AppInterface *uno_app_main_outlast(const KernelApi *);
extern const AppInterface *uno_app_main_pacman (const KernelApi *);
extern const AppInterface *uno_app_main_tracker(const KernelApi *);
extern const AppInterface *uno_app_main_paint  (const KernelApi *);
extern const AppInterface *uno_app_main_theme  (const KernelApi *);

static const UnoAppEntry kRegistry[APP_NAPPS] = {
    uno_app_main_sysinfo, uno_app_main_clock, uno_app_main_files,
    uno_app_main_notepad, uno_app_main_music, uno_app_main_dostris,
    uno_app_main_outlast, uno_app_main_pacman, uno_app_main_tracker,
    uno_app_main_paint, uno_app_main_theme,
};

/* the entry symbol each module exports (matches its -DUNO_APP_SYM=<name>) */
static const char *kEntryName[APP_NAPPS] = {
    "uno_app_main_sysinfo", "uno_app_main_clock",  "uno_app_main_files",
    "uno_app_main_notepad", "uno_app_main_music",  "uno_app_main_dostris",
    "uno_app_main_outlast", "uno_app_main_pacman", "uno_app_main_tracker",
    "uno_app_main_paint",   "uno_app_main_theme",
};

/* File Manager surface (mac_io.c, EE = libmc-backed mc0:) used to read images */
OSErr FSOpen(const void *fileName, short vRefNum, short *refNum);
OSErr FSClose(short refNum);
OSErr FSRead(short refNum, long *count, void *buffPtr);

/* ---- first-boot card population ---------------------------------------- *
 * The 11 module images are embedded (uno_mod_images.h / build/mod_images.c).
 * On first boot we WRITE them to mc0:/UnoDOS/Apps/appNN.uno via libmc, so the
 * overlay loader can then read them BACK off the card and relocate - a real
 * storage round-trip with no external card prep.  Subsequent boots skip the
 * write (the files already exist), so the persistent card is the source. */
#if UNO_EE_OVERLAY
#include <libmc.h>
#include "uno_mod_images.h"

#define UNO_APPS_DIR "/UnoDOS/Apps"    /* libmc path form (no device prefix) */

static int gCardPopulated = 0;

static void mc_populate_card(void)
{
    int i, r = 0;
    if (gCardPopulated) return;
    gCardPopulated = 1;

    /* ensure the Apps subdir exists (ee_platform.c already made /UnoDOS) */
    mcMkDir(0, 0, UNO_APPS_DIR);
    r = -99; mcSync(0, NULL, &r);
    printf("[modload] mkdir %s -> %d\n", UNO_APPS_DIR, r);

    for (i = 0; i < APP_NAPPS; i++) {
        char path[64];
        int fd, off = 0;
        snprintf(path, sizeof(path), UNO_APPS_DIR "/app%02d.uno", i);

        /* present already? (readable open succeeds) -> keep the card's copy */
        mcOpen(0, 0, path, sceMcFileAttrReadable);
        fd = -1; mcSync(0, NULL, &fd);
        if (fd >= 0) { mcClose(fd); mcSync(0, NULL, &r); continue; }

        /* create + write the embedded image */
        mcOpen(0, 0, path, sceMcFileCreateFile | sceMcFileAttrReadable |
                           sceMcFileAttrWriteable);
        fd = -1; mcSync(0, NULL, &fd);
        if (fd < 0) { printf("[modload] create %s -> fd %d\n", path, fd); continue; }
        while (off < gUnoModImages[i].len) {
            int chunk = (int)(gUnoModImages[i].len - off);
            int put;
            mcWrite(fd, (void *)(gUnoModImages[i].data + off), chunk);
            put = -1; mcSync(0, NULL, &put);
            if (put <= 0) break;
            off += put;
        }
        mcClose(fd); mcSync(0, NULL, &r);
        printf("[modload] wrote %s (%ld bytes)\n", path, (long)off);
    }
}
#endif

#if UNO_EE_OVERLAY
/* ------------------------------------------------------------------------ *
 *  Genuine EE overlay loader: read the ET_REL image, lay out + relocate it. *
 * ------------------------------------------------------------------------ */
#include <kernel.h>          /* FlushCache */
#include <malloc.h>          /* memalign */

/* --- the kernel symbols a module may import (resolved by &address) ------- *
 * Captured across all 11 apps: nm app*.uno | grep ' U '.  Toolbox calls come
 * from mac_compat.c, the libc helpers from newlib; taking their address here
 * makes the linker fill in the real runtime address, so this survives any
 * relink (no baked-in constants). */
typedef struct { const char *name; void *addr; } KSym;

extern void *memmove(void *, const void *, size_t);
extern char *stpcpy(char *, const char *);

static const KSym gKernelSyms[] = {
    { "DisposePtr",   (void *)DisposePtr   },
    { "DrawText",     (void *)DrawText     },
    { "FrameOval",    (void *)FrameOval    },
    { "FrameRect",    (void *)FrameRect    },
    { "GetMouse",     (void *)GetMouse     },
    { "InsetRect",    (void *)InsetRect    },
    { "LineTo",       (void *)LineTo       },
    { "MoveTo",       (void *)MoveTo       },
    { "NewPtr",       (void *)NewPtr       },
    { "OffsetRect",   (void *)OffsetRect   },
    { "PaintOval",    (void *)PaintOval    },
    { "PaintRect",    (void *)PaintRect    },
    { "PenMode",      (void *)PenMode      },
    { "PenNormal",    (void *)PenNormal    },
    { "PtInRect",     (void *)PtInRect     },
    { "RGBForeColor", (void *)RGBForeColor },
    { "Random",       (void *)Random       },
    { "SetRect",      (void *)SetRect      },
    { "StillDown",    (void *)StillDown    },
    { "TextMode",     (void *)TextMode     },
    { "TextWidth",    (void *)TextWidth    },
    { "TickCount",    (void *)TickCount    },
    { "memmove",      (void *)memmove      },
    { "memset",       (void *)memset       },
    { "stpcpy",       (void *)stpcpy       },
    { "strcat",       (void *)strcat       },
    { "strcpy",       (void *)strcpy       },
    { "strlen",       (void *)strlen       },
};
#define NKSYM ((int)(sizeof(gKernelSyms)/sizeof(gKernelSyms[0])))

static void *ksym_lookup(const char *name)
{
    int i;
    for (i = 0; i < NKSYM; i++)
        if (strcmp(name, gKernelSyms[i].name) == 0) return gKernelSyms[i].addr;
    return NULL;
}

/* --- minimal ELF32 little-endian structures (PS2SDK ships no <elf.h>) ---- */
typedef unsigned int   Elf32_Word;
typedef int            Elf32_Sword;
typedef unsigned short Elf32_Half;
typedef unsigned int   Elf32_Addr;
typedef unsigned int   Elf32_Off;

typedef struct {
    unsigned char e_ident[16];
    Elf32_Half  e_type, e_machine;
    Elf32_Word  e_version;
    Elf32_Addr  e_entry;
    Elf32_Off   e_phoff, e_shoff;
    Elf32_Word  e_flags;
    Elf32_Half  e_ehsize, e_phentsize, e_phnum;
    Elf32_Half  e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word  sh_name, sh_type, sh_flags;
    Elf32_Addr  sh_addr;
    Elf32_Off   sh_offset;
    Elf32_Word  sh_size, sh_link, sh_info, sh_addralign, sh_entsize;
} Elf32_Shdr;

typedef struct {
    Elf32_Word    st_name;
    Elf32_Addr    st_value;
    Elf32_Word    st_size;
    unsigned char st_info, st_other;
    Elf32_Half    st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr  r_offset;
    Elf32_Word  r_info;
    Elf32_Sword r_addend;
} Elf32_Rela;

#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHF_ALLOC    0x2
#define SHN_UNDEF    0
#define SHN_COMMON   0xFFF2
#define SHN_ABS      0xFFF1

#define ELF32_R_SYM(i)  ((i) >> 8)
#define ELF32_R_TYPE(i) ((i) & 0xFF)
#define ELF32_ST_TYPE(i) ((i) & 0xF)

#define R_MIPS_32    2
#define R_MIPS_26    4
#define R_MIPS_HI16  5
#define R_MIPS_LO16  6

/* --- HI16/LO16 pairing queue -------------------------------------------- *
 * R_MIPS_HI16 (lui) carries no low half; its rounding depends on the matching
 * R_MIPS_LO16's sign-extended low 16 bits.  One or more HI16s may precede a
 * single LO16, so queue each HI16 (loc + final value) and patch them all when
 * the LO16 arrives. */
#define HI_QUEUE 32
static unsigned int *gHiLoc[HI_QUEUE];
static Elf32_Word    gHiVal[HI_QUEUE];
static int           gHiN = 0;

static void hi_push(unsigned int *loc, Elf32_Word val)
{
    if (gHiN < HI_QUEUE) { gHiLoc[gHiN] = loc; gHiVal[gHiN] = val; gHiN++; }
}
/* A LO16 completes only the queued HI16s that share its target value (i.e. the
 * same symbol+addend).  HI16s for a DIFFERENT symbol stay queued for their own
 * LO16 - so interleaved (HI16 a)(HI16 b)(LO16 b)(LO16 a) sequences pair up
 * correctly.  hi = ((value - (s16)lo) >> 16), the standard MIPS rounding. */
static void hi_flush(Elf32_Word loval)
{
    short lo = (short)(loval & 0xFFFF);            /* sign-extended low half */
    int i, w = 0;
    for (i = 0; i < gHiN; i++) {
        if (gHiVal[i] == loval) {                  /* same symbol+addend */
            Elf32_Word hi = ((gHiVal[i] - (Elf32_Word)(Elf32_Sword)lo) >> 16) & 0xFFFF;
            *gHiLoc[i] = (*gHiLoc[i] & 0xFFFF0000) | hi;
        } else {
            gHiLoc[w] = gHiLoc[i]; gHiVal[w] = gHiVal[i]; w++;  /* keep queued */
        }
    }
    gHiN = w;
}

/* --- read the whole module image OFF THE CARD into a malloc'd buffer ----- *
 * Uses libmc directly with the real path mc0:/UnoDOS/Apps/appNN.uno (the
 * generic FSOpen prepends /UnoDOS/ to flat names, so it can't reach the Apps
 * subdir).  The bytes returned are the relocatable ELF that overlay_load()
 * lays out + relocates - so the executing code genuinely comes off the card. */
static unsigned char *mc_read_module(short proc, long *out_len)
{
    char path[64];
    int fd, r = -1;
    long cap = 64 * 1024, total = 0;
    unsigned char *buf;

    snprintf(path, sizeof(path), UNO_APPS_DIR "/app%02d.uno", (int)proc);
    mcOpen(0, 0, path, sceMcFileAttrReadable);
    fd = -1; mcSync(0, NULL, &fd);
    if (fd < 0) return NULL;

    buf = (unsigned char *)malloc(cap);
    if (!buf) { mcClose(fd); mcSync(0, NULL, &r); return NULL; }

    for (;;) {
        long want = cap - total;
        int got;
        if (want <= 0) {
            unsigned char *nb = (unsigned char *)realloc(buf, cap * 2);
            if (!nb) { free(buf); mcClose(fd); mcSync(0, NULL, &r); return NULL; }
            buf = nb; cap *= 2; want = cap - total;
        }
        mcRead(fd, buf + total, (int)want);
        got = -1; mcSync(0, NULL, &got);
        if (got <= 0) break;                   /* EOF / error */
        total += got;
        if (got < want) break;                 /* short read = EOF */
    }
    mcClose(fd); mcSync(0, NULL, &r);
    if (total < (long)sizeof(Elf32_Ehdr)) { free(buf); return NULL; }
    *out_len = total;
    return buf;
}

/* per-allocated-section runtime base (parallel to the section header array) */
#define MAX_SHDR 48

/* resolve a symbol-table index to its final runtime address (or NULL) */
static void *resolve_sym(const Elf32_Sym *syms, const char *strtab,
                         Elf32_Word *seg_base, int idx, long nsyms)
{
    const Elf32_Sym *s;
    if (idx < 0 || idx >= nsyms) return NULL;  /* S1: bound the symbol index */
    s = &syms[idx];
    Elf32_Half shndx = s->st_shndx;
    const char *nm = strtab + s->st_name;

    if (shndx == SHN_UNDEF) {                 /* import from the kernel */
        void *a = ksym_lookup(nm);
        if (!a) printf("[modload] unresolved symbol '%s'\n", nm);
        return a;
    }
    if (shndx == SHN_ABS)    return (void *)(uintptr_t)s->st_value;
    if (shndx >= MAX_SHDR)   return NULL;
    if (!seg_base[shndx])    return NULL;     /* defined in a non-loaded sect */
    return (void *)(uintptr_t)(seg_base[shndx] + s->st_value);
}

/* Load + relocate the ET_REL image; return the address of `want_entry`. */
static UnoAppEntry overlay_load(unsigned char *img, long len, const char *want_entry)
{
    Elf32_Ehdr *eh = (Elf32_Ehdr *)img;
    Elf32_Shdr *sh;
    Elf32_Word seg_base[MAX_SHDR];
    unsigned char *blob = NULL;
    long blob_sz = 0, off, nsyms = 0;
    int i, shnum, symtab_i = -1, strtab_i = -1;
    const Elf32_Sym *syms = NULL;
    const char *strtab = NULL, *shstr;
    void *entry = NULL;

    if (!(img[0]==0x7f && img[1]=='E' && img[2]=='L' && img[3]=='F')) return NULL;
    if (eh->e_type != 1 /*ET_REL*/) return NULL;
    shnum = eh->e_shnum;
    if (shnum <= 0 || shnum > MAX_SHDR) return NULL;
    /* S1: bound the section-header table and shstrndx against the image so a
     * crafted e_shoff/e_shstrndx can't read past the malloc'd module. */
    if ((long)eh->e_shoff <= 0 ||
        (long)eh->e_shoff + (long)shnum * (long)sizeof(Elf32_Shdr) > len) return NULL;
    if (eh->e_shstrndx >= (Elf32_Half)shnum) return NULL;
    sh = (Elf32_Shdr *)(img + eh->e_shoff);
    shstr = (const char *)(img + sh[eh->e_shstrndx].sh_offset);
    (void)shstr;
    memset(seg_base, 0, sizeof(seg_base));

    /* pass 1: size the runtime blob from the ALLOC sections, find symtab */
    for (i = 0; i < shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) { symtab_i = i; strtab_i = sh[i].sh_link; }
        if ((sh[i].sh_flags & SHF_ALLOC) &&
            (sh[i].sh_type == SHT_PROGBITS || sh[i].sh_type == SHT_NOBITS)) {
            Elf32_Word al = sh[i].sh_addralign ? sh[i].sh_addralign : 4;
            blob_sz = (blob_sz + al - 1) & ~(long)(al - 1);
            blob_sz += sh[i].sh_size;
        }
    }
    if (symtab_i < 0 || strtab_i < 0 || strtab_i >= shnum || blob_sz == 0) return NULL;

    /* one 64-byte-aligned executable blob holds every loaded section */
    blob = (unsigned char *)memalign(64, blob_sz + 64);
    if (!blob) return NULL;

    /* pass 2: copy PROGBITS / zero NOBITS, record each section's runtime base */
    off = 0;
    for (i = 0; i < shnum; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        if (sh[i].sh_type != SHT_PROGBITS && sh[i].sh_type != SHT_NOBITS) continue;
        {
            Elf32_Word al = sh[i].sh_addralign ? sh[i].sh_addralign : 4;
            off = (off + al - 1) & ~(long)(al - 1);
        }
        seg_base[i] = (Elf32_Word)(uintptr_t)(blob + off);
        if (sh[i].sh_type == SHT_NOBITS) memset(blob + off, 0, sh[i].sh_size);
        else {
            /* S1: the section's source bytes must lie within the image */
            if ((long)sh[i].sh_offset + (long)sh[i].sh_size > len) { free(blob); return NULL; }
            memcpy(blob + off, img + sh[i].sh_offset, sh[i].sh_size);
        }
        off += sh[i].sh_size;
    }

    /* S1: bound the symbol + string table offsets against the image */
    if ((long)sh[symtab_i].sh_offset + (long)sh[symtab_i].sh_size > len) { free(blob); return NULL; }
    if ((long)sh[strtab_i].sh_offset >= len) { free(blob); return NULL; }
    syms   = (const Elf32_Sym *)(img + sh[symtab_i].sh_offset);
    strtab = (const char *)(img + sh[strtab_i].sh_offset);
    nsyms  = (long)sh[symtab_i].sh_size / (long)sizeof(Elf32_Sym);

    /* pass 3: apply RELA relocations for each loaded section */
    for (i = 0; i < shnum; i++) {
        Elf32_Shdr *rs = &sh[i];
        Elf32_Word tgt_sec, nrel, j;
        const Elf32_Rela *rel;
        unsigned char *base;
        if (rs->sh_type != SHT_RELA) continue;
        tgt_sec = rs->sh_info;
        if (tgt_sec >= (Elf32_Word)shnum || !seg_base[tgt_sec]) continue;
        base = (unsigned char *)(uintptr_t)seg_base[tgt_sec];
        rel  = (const Elf32_Rela *)(img + rs->sh_offset);
        nrel = rs->sh_size / sizeof(Elf32_Rela);
        /* S1: the write target base+r_offset must stay inside the target
         * section, else a crafted r_offset is an arbitrary 32-bit write into
         * EE RAM (no MMU). sh_size bounds the region copied to seg_base. */
        {
            Elf32_Word tgt_size = sh[tgt_sec].sh_size;
            for (j = 0; j < nrel; j++) {
                Elf32_Word ro = rel[j].r_offset;
                if (ro > tgt_size || tgt_size - ro < 4) { free(blob); return NULL; }
            }
        }
        gHiN = 0;                              /* reset HI16 queue per section */
        for (j = 0; j < nrel; j++) {
            Elf32_Word type = ELF32_R_TYPE(rel[j].r_info);
            Elf32_Word symx = ELF32_R_SYM(rel[j].r_info);
            unsigned int *loc = (unsigned int *)(base + rel[j].r_offset);
            void *S = resolve_sym(syms, strtab, seg_base, symx, nsyms);
            Elf32_Word A = rel[j].r_addend;
            Elf32_Word value;
            if (!S && ELF32_R_SYM(rel[j].r_info) != 0) {
                /* unresolved import -> bail, caller falls back to registry */
                free(blob); return NULL;
            }
            value = (Elf32_Word)(uintptr_t)S + A;
            switch (type) {
            case R_MIPS_32:
                *loc += value;                     /* word add per the ABI */
                break;
            case R_MIPS_26: {
                Elf32_Word insn = *loc & 0xFC000000;
                Elf32_Word t = (value >> 2) & 0x03FFFFFF;
                *loc = insn | t;
                break;
            }
            case R_MIPS_HI16:
                /* defer: the lui's high half rounds on the matching LO16 */
                hi_push(loc, value);
                break;
            case R_MIPS_LO16:
                hi_flush(value);               /* patch all pending HI16s */
                *loc = (*loc & 0xFFFF0000) | (value & 0xFFFF);
                break;
            default:
                printf("[modload] unhandled reloc type %u\n", (unsigned)type);
                free(blob); return NULL;
            }
        }
    }

    /* pass 4: find the requested entry symbol's runtime address */
    {
        Elf32_Word nsym = sh[symtab_i].sh_size / sizeof(Elf32_Sym);
        Elf32_Word k;
        for (k = 0; k < nsym; k++) {
            const char *nm = strtab + syms[k].st_name;
            if (syms[k].st_shndx != SHN_UNDEF &&
                strcmp(nm, want_entry) == 0) {
                entry = resolve_sym(syms, strtab, seg_base, k, nsyms);
                break;
            }
        }
    }
    if (!entry) { free(blob); return NULL; }

    /* make the freshly written code visible to the I-cache */
    FlushCache(0);

    return (UnoAppEntry)entry;
}
#endif /* UNO_EE_OVERLAY */

/* ------------------------------------------------------------------------ */
UnoAppEntry uno_load_module(short proc)
{
    if (proc < 0 || proc >= APP_NAPPS) return NULL;

#if UNO_EE_OVERLAY
    {
        long len = 0;
        unsigned char *img;
        mc_populate_card();                /* first call writes images to card */
        img = mc_read_module(proc, &len);  /* then read them BACK off the card */
        if (img) {
            UnoAppEntry e = overlay_load(img, len, kEntryName[proc]);
            free(img);
            if (e) {
                printf("[modload] app%02d: RELOCATED from %s/app%02d.uno (genuine)\n",
                       (int)proc, UNO_APPS_MC, (int)proc);
                return e;                      /* code runs from the mc image */
            }
            printf("[modload] app%02d: overlay load failed -> linked-in fallback\n",
                   (int)proc);
        } else {
            printf("[modload] app%02d: mc image missing -> linked-in fallback\n",
                   (int)proc);
        }
    }
#else
    /* UNO_EE_OVERLAY=0: still exercise the storage read, dispatch linked-in */
    {
        unsigned char pname[40]; char nm[40]; short ref; long n; int i, len;
        static unsigned char scratch[256];
        snprintf(nm, sizeof(nm), UNO_APPS_MC "/app%02d.uno", (int)proc);
        len = (int)strlen(nm); if (len > 38) len = 38;
        pname[0] = (unsigned char)len;
        for (i = 0; i < len; i++) pname[i + 1] = (unsigned char)nm[i];
        if (FSOpen(pname, 0, &ref) == noErr) {
            n = sizeof(scratch); FSRead(ref, &n, scratch); FSClose(ref);
        }
    }
#endif
    return kRegistry[proc];                    /* linked-in fallback / default */
}
