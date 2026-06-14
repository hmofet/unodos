/* ===========================================================================
 * UnoDOS/PS2 - File Manager (M2) + Sound Manager (M3) shim.
 *
 * File Manager: UnoDOS's Files + Notepad + Tracker + Paint persist through the
 * classic FSOpen/FSRead/FSWrite/Create/FSDelete calls and browse via
 * PBGetCatInfo. This backs them with a real directory tree:
 *   - HOST build  (UNO_HOST): a POSIX directory ("uno_disk/"), so save/load and
 *     the Files listing work on the PC inner loop.
 *   - EE build:    PS2 memory card (mc0:) via the same stdio calls once fileXio
 *     is wired; until then the same POSIX path is used if present, else errors
 *     are returned gracefully (Files shows an empty volume, Notepad starts blank).
 *
 * Mac names are Pascal strings (length byte + chars); we convert at the edge.
 *
 * Sound Manager: a square-wave channel model. On host it records the last note
 * (silent); on EE M3 it will drive audsrv. Either way the Music/Tracker apps
 * link and run.
 * ===========================================================================
 */
#include "mac_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(UNO_HOST)
#include <dirent.h>
#include <sys/stat.h>
#endif

#ifndef UNO_DISK_DIR
#define UNO_DISK_DIR "uno_disk"
#endif

/* ---- Pascal <-> C name helpers ----------------------------------------- */
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
static void disk_path(const char *name, char *out, int cap)
{ snprintf(out, cap, "%s/%s", UNO_DISK_DIR, name); }

/* ---- open file table --------------------------------------------------- */
#define MAXF 8
static FILE *gF[MAXF];
static int   gFused[MAXF];

static int alloc_ref(void) { int i; for (i = 0; i < MAXF; i++) if (!gFused[i]) return i; return -1; }

OSErr FSOpen(const void *fileName, short vRefNum, short *refNum)
{
    char name[64], path[128]; int r; FILE *f;
    (void)vRefNum;
    p2c(fileName, name, sizeof name);
    disk_path(name, path, sizeof path);
    f = fopen(path, "r+b");
    if (!f) return -43;                    /* fnfErr */
    r = alloc_ref();
    if (r < 0) { fclose(f); return -42; }  /* tmfoErr */
    gF[r] = f; gFused[r] = 1; *refNum = (short)r;
    return noErr;
}
OSErr FSClose(short refNum)
{
    if (refNum < 0 || refNum >= MAXF || !gFused[refNum]) return -51;
    fclose(gF[refNum]); gFused[refNum] = 0; gF[refNum] = NULL;
    return noErr;
}
OSErr FSRead(short refNum, long *count, void *buf)
{
    size_t got;
    if (refNum < 0 || refNum >= MAXF || !gFused[refNum]) return -51;
    got = fread(buf, 1, (size_t)*count, gF[refNum]);
    *count = (long)got;
    return got > 0 ? noErr : -39;          /* eofErr when nothing read */
}
OSErr FSWrite(short refNum, long *count, const void *buf)
{
    size_t put;
    if (refNum < 0 || refNum >= MAXF || !gFused[refNum]) return -51;
    put = fwrite(buf, 1, (size_t)*count, gF[refNum]);
    *count = (long)put;
    return noErr;
}
OSErr Create(const void *fileName, short vRefNum, OSType creator, OSType type)
{
    char name[64], path[128]; FILE *f;
    (void)vRefNum; (void)creator; (void)type;
    p2c(fileName, name, sizeof name);
    disk_path(name, path, sizeof path);
#if defined(UNO_HOST)
    mkdir(UNO_DISK_DIR, 0777);
#endif
    f = fopen(path, "rb");                 /* already exists? */
    if (f) { fclose(f); return -48; }      /* dupFNErr */
    f = fopen(path, "wb");
    if (!f) return -34;                    /* dskFulErr-ish */
    fclose(f);
    return noErr;
}
OSErr FSDelete(const void *fileName, short vRefNum)
{
    char name[64], path[128];
    (void)vRefNum;
    p2c(fileName, name, sizeof name);
    disk_path(name, path, sizeof path);
    return remove(path) == 0 ? noErr : -43;
}
OSErr FlushVol(const void *volName, short vRefNum) { (void)volName; (void)vRefNum; return noErr; }

/* PBGetCatInfo: enumerate the disk directory by 1-based ioFDirIndex. The Files
   app calls this with ioFDirIndex = 1,2,3,... until an error, building its
   listing. ioNamePtr receives a Pascal name; ioFlAttrib bit 4 flags dirs. */
OSErr PBGetCatInfoSync(CInfoPBPtr pb)
{
#if defined(UNO_HOST)
    static DIR *dir = NULL;
    static int  lastIdx = 0;
    struct dirent *de;
    int want = pb->dirInfo.ioFDirIndex, n;
    if (want <= 0) return -50;             /* we only support index mode */
    if (want == 1 || want <= lastIdx) {    /* (re)start enumeration */
        if (dir) closedir(dir);
        dir = opendir(UNO_DISK_DIR);
        lastIdx = 0;
    }
    if (!dir) return -43;
    for (;;) {
        de = readdir(dir);
        if (!de) { closedir(dir); dir = NULL; lastIdx = 0; return -43; } /* end */
        if (de->d_name[0] == '.') continue;     /* skip . .. hidden */
        lastIdx++;
        if (lastIdx == want) break;
    }
    if (pb->dirInfo.ioNamePtr) c2p(de->d_name, pb->dirInfo.ioNamePtr, 64);
    {
        char path[128]; struct stat st;
        disk_path(de->d_name, path, sizeof path);
        pb->dirInfo.ioFlAttrib = 0;
        pb->dirInfo.ioFlLgLen = 0;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) pb->dirInfo.ioFlAttrib = 0x10;
            else pb->dirInfo.ioFlLgLen = (long)st.st_size;
        }
    }
    pb->dirInfo.ioDirID = pb->dirInfo.ioDrDirID = (long)(2 + want);
    pb->dirInfo.ioDrParID = 2;             /* parent = root (fsRtDirID) */
    pb->dirInfo.ioResult = noErr;
    n = want; (void)n;
    return noErr;
#else
    (void)pb; return -43;                  /* EE: no MC backend yet (M2 follow-up) */
#endif
}
OSErr PBHSetVolSync(void *pb) { (void)pb; return noErr; }

/* Raw block I/O. The core only uses this for the .Sony Mac floppy (ioRefNum
   -5), which has no PS2 equivalent - fail it so fat_dev_sony() reports "no
   floppy" and the RAM FAT12 volume (fat_dev_ram) is the working path. A real
   positive refNum still routes to the file backend. */
OSErr PBReadSync(ParmBlkPtr pb)
{
    long c;
    if (pb->ioParam.ioRefNum < 0) { pb->ioParam.ioResult = -19; return -19; } /* readErr */
    c = pb->ioParam.ioReqCount;
    { OSErr e = FSRead(pb->ioParam.ioRefNum, &c, pb->ioParam.ioBuffer);
      pb->ioParam.ioActCount = c; pb->ioParam.ioResult = e; return e; }
}
OSErr PBWriteSync(ParmBlkPtr pb)
{
    long c;
    if (pb->ioParam.ioRefNum < 0) { pb->ioParam.ioResult = -20; return -20; } /* writErr */
    c = pb->ioParam.ioReqCount;
    { OSErr e = FSWrite(pb->ioParam.ioRefNum, &c, pb->ioParam.ioBuffer);
      pb->ioParam.ioActCount = c; pb->ioParam.ioResult = e; return e; }
}

/* ===========================================================================
 * Sound Manager (M3 stub - links + runs; audio is silent on host)
 * ======================================================================== */
static SndChannel gChans[8];
static int gChanUsed[8];

OSErr SndNewChannel(SndChannelPtr *chan, short synth, long init, void *proc)
{
    int i; (void)synth; (void)init; (void)proc;
    for (i = 0; i < 8; i++) if (!gChanUsed[i]) { gChanUsed[i] = 1; gChans[i].id = i; *chan = &gChans[i]; return noErr; }
    *chan = NULL; return -204;             /* notEnoughHardwareErr */
}
OSErr SndDisposeChannel(SndChannelPtr chan, Boolean quiet)
{
    (void)quiet;
    if (chan && chan->id >= 0 && chan->id < 8) gChanUsed[chan->id] = 0;
    return noErr;
}
OSErr SndDoImmediate(SndChannelPtr chan, const SndCommand *cmd)
{
    (void)chan; (void)cmd;                 /* host: silent. EE M3: drive audsrv. */
    return noErr;
}
