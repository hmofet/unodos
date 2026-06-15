/* ===========================================================================
 * UnoDOS/Dreamcast - File Manager (M2) + Sound Manager (M3) shim.
 *
 * UnoDOS's Files + Notepad + Tracker + Paint persist through the classic
 * FSOpen/FSRead/FSWrite/Create/FSDelete calls and browse via PBGetCatInfo.
 * Two backends, selected at compile time:
 *
 *   HOST build (UNO_HOST): a POSIX directory ("uno_disk/") via stdio - byte
 *     identical to the PS2 port's host backend, so the PC inner loop renders
 *     and round-trips files exactly the same way.
 *   DC build:  the Dreamcast **VMU** ("/vmu/a1/") via KallistiOS's POSIX VFS.
 *     The VMU is a 128 KB flash card in the controller; KOS surfaces each save
 *     file under /vmu/<port><slot> and supports fopen/opendir over it. UnoDOS
 *     only ever Creates then writes a whole file in one FSWrite, and opens then
 *     reads a whole file in one FSRead (no mid-file seeks - the only seeking
 *     path is the .Sony Mac floppy, which we fail below), so a flush-on-close
 *     RAM buffer per handle is the VMU-safe shape: writes accumulate in RAM and
 *     hit the card once, on FSClose; reads slurp the file at FSOpen. This
 *     sidesteps the VMU VFS's lack of an update ("r+b") mode and its 512-byte
 *     block granularity. So Files/Notepad/Tracker/Paint persist on real
 *     hardware and across power cycles.
 *
 * Mac names are Pascal strings (length byte + chars); we convert at the edge.
 *
 * Sound Manager: a square-wave channel model. Silent on host; on DC M3 it will
 * drive the AICA via KOS (dc_main.c). Either way Music/Tracker link and run.
 * ===========================================================================
 */
#include "mac_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Pascal <-> C name helpers (shared) -------------------------------- */
static void p2c(const void *pstr, char *out, int cap)
{
    const unsigned char *p = (const unsigned char *)pstr;
    int n = p[0], i;
    if (n > cap - 1) n = cap - 1;
    for (i = 0; i < n; i++) out[i] = (char)p[i + 1];
    out[n] = 0;
}
static void c2p(const char *c, unsigned char *pout, int cap)
{
    int n = (int)strlen(c), i;
    if (n > 255) n = 255;
    if (n > cap - 1) n = cap - 1;
    pout[0] = (unsigned char)n;
    for (i = 0; i < n; i++) pout[i + 1] = (unsigned char)c[i];
}

#if defined(UNO_HOST)
/* =========================================================================
 * HOST backend - stdio over uno_disk/ (identical to ps2/mac_io.c)
 * ======================================================================== */
#include <dirent.h>
#include <sys/stat.h>

#ifndef UNO_DISK_DIR
#define UNO_DISK_DIR "uno_disk"
#endif
static void disk_path(const char *name, char *out, int cap)
{ snprintf(out, cap, "%s/%s", UNO_DISK_DIR, name); }

#define MAXF 8
static FILE *gF[MAXF];
static int   gFused[MAXF];
static int alloc_ref(void) { int i; for (i = 0; i < MAXF; i++) if (!gFused[i]) return i; return -1; }

OSErr FSOpen(const void *fileName, short vRefNum, short *refNum)
{
    char name[64], path[128]; int r; FILE *f;
    (void)vRefNum;
    p2c(fileName, name, sizeof name); disk_path(name, path, sizeof path);
    f = fopen(path, "r+b");
    if (!f) return fnfErr;
    r = alloc_ref(); if (r < 0) { fclose(f); return -42; }
    gF[r] = f; gFused[r] = 1; *refNum = (short)r;
    return noErr;
}
OSErr FSClose(short refNum)
{
    if (refNum < 0 || refNum >= MAXF || !gFused[refNum]) return rfNumErr;
    fclose(gF[refNum]); gFused[refNum] = 0; gF[refNum] = NULL;
    return noErr;
}
OSErr FSRead(short refNum, long *count, void *buf)
{
    size_t got;
    if (refNum < 0 || refNum >= MAXF || !gFused[refNum]) return rfNumErr;
    got = fread(buf, 1, (size_t)*count, gF[refNum]); *count = (long)got;
    return got > 0 ? noErr : eofErr;
}
OSErr FSWrite(short refNum, long *count, const void *buf)
{
    size_t put;
    if (refNum < 0 || refNum >= MAXF || !gFused[refNum]) return rfNumErr;
    put = fwrite(buf, 1, (size_t)*count, gF[refNum]); *count = (long)put;
    return noErr;
}
OSErr Create(const void *fileName, short vRefNum, OSType creator, OSType type)
{
    char name[64], path[128]; FILE *f;
    (void)vRefNum; (void)creator; (void)type;
    p2c(fileName, name, sizeof name); disk_path(name, path, sizeof path);
    mkdir(UNO_DISK_DIR, 0777);
    f = fopen(path, "rb"); if (f) { fclose(f); return dupFNErr; }
    f = fopen(path, "wb"); if (!f) return dskFulErr; fclose(f);
    return noErr;
}
OSErr FSDelete(const void *fileName, short vRefNum)
{
    char name[64], path[128]; (void)vRefNum;
    p2c(fileName, name, sizeof name); disk_path(name, path, sizeof path);
    return remove(path) == 0 ? noErr : fnfErr;
}

/* PBGetCatInfo: enumerate uno_disk/ by 1-based ioFDirIndex. */
OSErr PBGetCatInfoSync(CInfoPBPtr pb)
{
    static DIR *dir = NULL;
    static int  lastIdx = 0;
    struct dirent *de;
    int want = pb->dirInfo.ioFDirIndex;
    if (want <= 0) return paramErr;
    if (want == 1 || want <= lastIdx) {
        if (dir) closedir(dir);
        dir = opendir(UNO_DISK_DIR); lastIdx = 0;
    }
    if (!dir) return fnfErr;
    for (;;) {
        de = readdir(dir);
        if (!de) { closedir(dir); dir = NULL; lastIdx = 0; return fnfErr; }
        if (de->d_name[0] == '.') continue;
        lastIdx++;
        if (lastIdx == want) break;
    }
    if (pb->dirInfo.ioNamePtr) c2p(de->d_name, pb->dirInfo.ioNamePtr, 64);
    {
        char path[128]; struct stat st;
        disk_path(de->d_name, path, sizeof path);
        pb->dirInfo.ioFlAttrib = 0; pb->dirInfo.ioFlLgLen = 0;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) pb->dirInfo.ioFlAttrib = 0x10;
            else pb->dirInfo.ioFlLgLen = (long)st.st_size;
        }
    }
    pb->dirInfo.ioDirID = pb->dirInfo.ioDrDirID = (long)(2 + want);
    pb->dirInfo.ioDrParID = fsRtDirID;
    pb->dirInfo.ioResult = noErr;
    return noErr;
}

#else
/* =========================================================================
 * DC backend - Dreamcast VMU (/vmu/a1) via KallistiOS, flush-on-close buffers
 *
 * Each open handle owns a RAM buffer. A read handle slurps the whole VMU file
 * at FSOpen; FSRead serves bytes from RAM. A write handle (Create marks the
 * name "to be written") accumulates FSWrite bytes in a growing RAM buffer and
 * commits them to the card in one fwrite at FSClose. This matches UnoDOS's
 * whole-file save/load model and the VMU's block-oriented flash, and avoids
 * relying on an "r+b" update mode the VMU VFS does not provide.
 *
 * KOS mounts the VMU in port A slot 1 at /vmu/a1; opendir/readdir enumerate it
 * for the Files listing. Port/slot is fixed at a1 (the standard first VMU) -
 * a config knob can generalise it later.
 * ======================================================================== */
#include <dirent.h>

#ifndef UNO_VMU_DIR
#define UNO_VMU_DIR "/vmu/a1"
#endif
static void disk_path(const char *name, char *out, int cap)
{ snprintf(out, cap, "%s/%s", UNO_VMU_DIR, name); }

#define MAXF 8
#define VMU_MAX (128 * 1024)        /* a VMU is ~128 KB total */

typedef struct {
    int          used;
    int          writing;           /* 1 = buffer commits to the card on close */
    char         path[160];
    unsigned char *buf;
    long         len;               /* valid bytes in buf */
    long         pos;               /* read cursor */
    long         cap;               /* allocation size */
} VmuFile;
static VmuFile gV[MAXF];

static int alloc_ref(void) { int i; for (i = 0; i < MAXF; i++) if (!gV[i].used) return i; return -1; }

OSErr FSOpen(const void *fileName, short vRefNum, short *refNum)
{
    char name[64], path[160]; int r; FILE *f; long sz;
    (void)vRefNum;
    p2c(fileName, name, sizeof name); disk_path(name, path, sizeof path);

    r = alloc_ref(); if (r < 0) return -42;

    /* Try to slurp an existing file (read path). If it does not exist, open a
       fresh write buffer (the Create->FSOpen->FSWrite save path). */
    f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz < 0) sz = 0; if (sz > VMU_MAX) sz = VMU_MAX;
        gV[r].buf = (unsigned char *)malloc(sz > 0 ? (size_t)sz : 1);
        if (!gV[r].buf) { fclose(f); return -108; }       /* memFullErr */
        gV[r].len = (long)fread(gV[r].buf, 1, (size_t)sz, f);
        fclose(f);
        gV[r].writing = 0;
    } else {
        gV[r].cap = 4096;
        gV[r].buf = (unsigned char *)malloc((size_t)gV[r].cap);
        if (!gV[r].buf) return -108;
        gV[r].len = 0;
        gV[r].writing = 1;
    }
    strncpy(gV[r].path, path, sizeof gV[r].path - 1);
    gV[r].path[sizeof gV[r].path - 1] = 0;
    gV[r].pos = 0; gV[r].used = 1;
    *refNum = (short)r;
    return noErr;
}

OSErr FSClose(short refNum)
{
    VmuFile *v;
    if (refNum < 0 || refNum >= MAXF || !gV[refNum].used) return rfNumErr;
    v = &gV[refNum];
    if (v->writing && v->len > 0) {
        FILE *f = fopen(v->path, "wb");                   /* one whole-file commit */
        if (f) { fwrite(v->buf, 1, (size_t)v->len, f); fclose(f); }
    }
    free(v->buf);
    memset(v, 0, sizeof *v);
    return noErr;
}

OSErr FSRead(short refNum, long *count, void *buf)
{
    VmuFile *v; long n;
    if (refNum < 0 || refNum >= MAXF || !gV[refNum].used) return rfNumErr;
    v = &gV[refNum];
    n = *count;
    if (n > v->len - v->pos) n = v->len - v->pos;
    if (n < 0) n = 0;
    memcpy(buf, v->buf + v->pos, (size_t)n);
    v->pos += n; *count = n;
    return n > 0 ? noErr : eofErr;
}

OSErr FSWrite(short refNum, long *count, const void *buf)
{
    VmuFile *v; long need;
    if (refNum < 0 || refNum >= MAXF || !gV[refNum].used) return rfNumErr;
    v = &gV[refNum];
    need = v->len + *count;
    if (need > VMU_MAX) { *count = 0; return dskFulErr; }
    if (need > v->cap) {
        long nc = v->cap ? v->cap : 4096;
        unsigned char *nb;
        while (nc < need) nc *= 2;
        nb = (unsigned char *)realloc(v->buf, (size_t)nc);
        if (!nb) { *count = 0; return dskFulErr; }
        v->buf = nb; v->cap = nc;
    }
    memcpy(v->buf + v->len, buf, (size_t)*count);
    v->len += *count;
    v->writing = 1;
    return noErr;
}

OSErr Create(const void *fileName, short vRefNum, OSType creator, OSType type)
{
    char name[64], path[160]; FILE *f;
    (void)vRefNum; (void)creator; (void)type;
    p2c(fileName, name, sizeof name); disk_path(name, path, sizeof path);
    f = fopen(path, "rb"); if (f) { fclose(f); return dupFNErr; }
    /* The actual bytes are written at FSClose of the write handle; Create just
       reports "new file ok" so the core proceeds to FSOpen+FSWrite. */
    return noErr;
}

OSErr FSDelete(const void *fileName, short vRefNum)
{
    char name[64], path[160]; (void)vRefNum;
    p2c(fileName, name, sizeof name); disk_path(name, path, sizeof path);
    return remove(path) == 0 ? noErr : fnfErr;
}

/* PBGetCatInfo: enumerate /vmu/a1 by 1-based ioFDirIndex via opendir/readdir. */
OSErr PBGetCatInfoSync(CInfoPBPtr pb)
{
    static DIR *dir = NULL;
    static int  lastIdx = 0;
    struct dirent *de;
    int want = pb->dirInfo.ioFDirIndex;
    if (want <= 0) return paramErr;
    if (want == 1 || want <= lastIdx) {
        if (dir) closedir(dir);
        dir = opendir(UNO_VMU_DIR); lastIdx = 0;
    }
    if (!dir) return fnfErr;
    for (;;) {
        de = readdir(dir);
        if (!de) { closedir(dir); dir = NULL; lastIdx = 0; return fnfErr; }
        if (de->d_name[0] == '.') continue;
        lastIdx++;
        if (lastIdx == want) break;
    }
    if (pb->dirInfo.ioNamePtr) c2p(de->d_name, pb->dirInfo.ioNamePtr, 64);
    /* VMU files are flat (no subdirs); size is not always reported by readdir,
       so default to a file with unknown length (0). The Files app shows the
       name, which is what matters for open/load. */
    pb->dirInfo.ioFlAttrib = 0;
    pb->dirInfo.ioFlLgLen  = 0;
    pb->dirInfo.ioDirID = pb->dirInfo.ioDrDirID = (long)(2 + want);
    pb->dirInfo.ioDrParID = fsRtDirID;
    pb->dirInfo.ioResult = noErr;
    return noErr;
}

#endif /* UNO_HOST vs DC */

/* ---- shared: FlushVol + raw block I/O ---------------------------------- */
OSErr FlushVol(const void *volName, short vRefNum) { (void)volName; (void)vRefNum; return noErr; }
OSErr PBHSetVolSync(void *pb) { (void)pb; return noErr; }

/* The core uses PBRead/PBWrite only for the .Sony Mac floppy (ioRefNum -5),
   which has no DC equivalent - fail it so fat_dev_sony() reports "no floppy"
   and the RAM FAT12 volume is the working path. */
OSErr PBReadSync(ParmBlkPtr pb)
{
    long c;
    if (pb->ioParam.ioRefNum < 0) { pb->ioParam.ioResult = -19; return -19; }
    c = pb->ioParam.ioReqCount;
    { OSErr e = FSRead(pb->ioParam.ioRefNum, &c, pb->ioParam.ioBuffer);
      pb->ioParam.ioActCount = c; pb->ioParam.ioResult = e; return e; }
}
OSErr PBWriteSync(ParmBlkPtr pb)
{
    long c;
    if (pb->ioParam.ioRefNum < 0) { pb->ioParam.ioResult = -20; return -20; }
    c = pb->ioParam.ioReqCount;
    { OSErr e = FSWrite(pb->ioParam.ioRefNum, &c, pb->ioParam.ioBuffer);
      pb->ioParam.ioActCount = c; pb->ioParam.ioResult = e; return e; }
}

/* ===========================================================================
 * Sound Manager (M3): square-wave channels.
 *   HOST build: silent (links + runs).
 *   DC build:   drives the AICA via the uno_dc_snd_* hooks in dc_main.c - the
 *               core's noteCmd (param2 = MIDI note) becomes a looping
 *               square-wave voice on the matching AICA channel; quietCmd stops
 *               it. Music / Tracker (4 voices) / Dostris all use this path.
 * ======================================================================== */
static SndChannel gChans[8];
static int gChanUsed[8];

#ifdef UNO_DC
void uno_dc_snd_note(int chan, int midi, int amp);  /* dc_main.c */
void uno_dc_snd_quiet(int chan);
#endif

OSErr SndNewChannel(SndChannelPtr *chan, short synth, long init, void *proc)
{
    int i; (void)synth; (void)init; (void)proc;
    for (i = 0; i < 8; i++) if (!gChanUsed[i]) { gChanUsed[i] = 1; gChans[i].id = i; *chan = &gChans[i]; return noErr; }
    *chan = NULL; return -204;
}
OSErr SndDisposeChannel(SndChannelPtr chan, Boolean quiet)
{
    (void)quiet;
    if (chan && chan->id >= 0 && chan->id < 8) {
#ifdef UNO_DC
        uno_dc_snd_quiet(chan->id);
#endif
        gChanUsed[chan->id] = 0;
    }
    return noErr;
}
OSErr SndDoImmediate(SndChannelPtr chan, const SndCommand *cmd)
{
#ifdef UNO_DC
    if (chan && cmd) {
        if (cmd->cmd == noteCmd || cmd->cmd == freqCmd)
            uno_dc_snd_note(chan->id, (int)cmd->param2, 200);   /* param2 = MIDI note */
        else if (cmd->cmd == quietCmd || cmd->cmd == flushCmd)
            uno_dc_snd_quiet(chan->id);
    }
#else
    (void)chan; (void)cmd;                   /* host: silent */
#endif
    return noErr;
}
