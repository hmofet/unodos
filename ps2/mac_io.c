/* ===========================================================================
 * UnoDOS/PS2 - File Manager (M2) + Sound Manager (M3) shim.
 *
 * UnoDOS's Files + Notepad + Tracker + Paint persist through the classic
 * FSOpen/FSRead/FSWrite/Create/FSDelete calls and browse via PBGetCatInfo.
 * Two backends, selected at compile time:
 *
 *   HOST build (UNO_HOST): a POSIX directory ("uno_disk/") via stdio, so
 *     save/load + the Files listing work on the PC inner loop.
 *   EE build:  the PS2 **memory card** ("mc0:/UnoDOS/") via PS2SDK - low-level
 *     open/read/write/close route to mc0: once MCMAN/MCSERV are loaded and
 *     mcInit has run (done in ee_platform.c), and mcGetDir enumerates the
 *     directory for the Files listing. So Files/Notepad persist on real
 *     hardware and across boots.
 *
 * Mac names are Pascal strings (length byte + chars); we convert at the edge.
 *
 * Sound Manager: a square-wave channel model. Silent on host; on EE M3 it will
 * drive audsrv. Either way Music/Tracker link and run.
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
 * HOST backend - stdio over uno_disk/
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
 * EE backend - PS2 memory card (/UnoDOS/) via libmc
 *
 * The PS2 memory card is NOT a POSIX filesystem - newlib open(O_CREAT) makes a
 * directory-like entry that doesn't round-trip. The correct API is libmc:
 * mcOpen with sceMcFileCreateFile makes a real save-file, mcRead/mcWrite/mcClose
 * transfer bytes, mcGetDir enumerates, mcDelete removes. Each call is async -
 * mcSync(0,...) blocks for the result. refNum is the libmc fd. SIO2MAN/MCMAN/
 * MCSERV + mcInit + the /UnoDOS dir are brought up in ee_platform.c.
 * ======================================================================== */
#include <libmc.h>

#define UNO_MC_DIR "/UnoDOS"                 /* libmc path form (no device prefix) */
static void disk_path(const char *name, char *out, int cap)
{ snprintf(out, cap, "%s/%s", UNO_MC_DIR, name); }

static int mc_wait(void) { int r = -1; mcSync(0, NULL, &r); return r; }

OSErr FSOpen(const void *fileName, short vRefNum, short *refNum)
{
    char name[64], path[160]; int fd; (void)vRefNum;
    p2c(fileName, name, sizeof name); disk_path(name, path, sizeof path);
    mcOpen(0, 0, path, sceMcFileAttrReadable | sceMcFileAttrWriteable);
    fd = mc_wait();
    if (fd < 0) return fnfErr;
    *refNum = (short)fd; return noErr;
}
OSErr FSClose(short refNum) { mcClose(refNum); return mc_wait() == 0 ? noErr : rfNumErr; }
OSErr FSRead(short refNum, long *count, void *buf)
{
    int got; mcRead(refNum, buf, (int)*count); got = mc_wait();
    *count = (got < 0) ? 0 : got;
    return got > 0 ? noErr : eofErr;
}
OSErr FSWrite(short refNum, long *count, const void *buf)
{
    int put; mcWrite(refNum, (void *)buf, (int)*count); put = mc_wait();
    *count = (put < 0) ? 0 : put;
    return put >= 0 ? noErr : -36;           /* ioErr */
}
OSErr Create(const void *fileName, short vRefNum, OSType creator, OSType type)
{
    char name[64], path[160]; int fd; (void)vRefNum; (void)creator; (void)type;
    p2c(fileName, name, sizeof name); disk_path(name, path, sizeof path);
    mcOpen(0, 0, path, sceMcFileCreateFile | sceMcFileAttrReadable | sceMcFileAttrWriteable);
    fd = mc_wait();
    if (fd < 0) return dskFulErr;
    mcClose(fd); mc_wait();
    return noErr;
}
OSErr FSDelete(const void *fileName, short vRefNum)
{
    char name[64], path[160]; (void)vRefNum;
    p2c(fileName, name, sizeof name); disk_path(name, path, sizeof path);
    mcDelete(0, 0, path);
    return mc_wait() == 0 ? noErr : fnfErr;
}

/* PBGetCatInfo: mcGetDir("/UnoDOS/*") cached on ioFDirIndex==1, then the
   want-th non-dot entry. AttrFile's subdir bit -> ioFlAttrib 0x10. */
static sceMcTblGetDir gMcDir[64] __attribute__((aligned(64)));
static int gMcN = 0;
OSErr PBGetCatInfoSync(CInfoPBPtr pb)
{
    int want = pb->dirInfo.ioFDirIndex, i, idx, ret;
    if (want <= 0) return paramErr;
    if (want == 1) {
        mcGetDir(0, 0, "/UnoDOS/*", 0, 64, gMcDir);
        mcSync(0, NULL, &ret);
        gMcN = (ret < 0) ? 0 : ret;
    }
    idx = 0;
    for (i = 0; i < gMcN; i++) {
        const unsigned char *e = gMcDir[i].EntryName;
        char nm[34]; int j;
        if (e[0] == '.') continue;           /* skip . and .. */
        if (++idx != want) continue;
        for (j = 0; j < 32 && e[j]; j++) nm[j] = (char)e[j];
        nm[j] = 0;
        if (pb->dirInfo.ioNamePtr) c2p(nm, pb->dirInfo.ioNamePtr, 64);
        /* PCSX2/mcman tags our flat save-files with the directory attribute
           (0x8427) even though open() created writable files - so a non-zero
           file size is the reliable "this is a file" signal here. */
        pb->dirInfo.ioFlLgLen  = (long)gMcDir[i].FileSizeByte;
        pb->dirInfo.ioFlAttrib =
            ((gMcDir[i].AttrFile & MC_ATTR_SUBDIR) && gMcDir[i].FileSizeByte == 0)
            ? 0x10 : 0;
        pb->dirInfo.ioDirID = pb->dirInfo.ioDrDirID = (long)(2 + want);
        pb->dirInfo.ioDrParID = fsRtDirID;
        pb->dirInfo.ioResult = noErr;
        return noErr;
    }
    return fnfErr;                           /* past the last entry */
}

#endif /* UNO_HOST vs EE */

/* ---- shared: FlushVol + raw block I/O ---------------------------------- */
OSErr FlushVol(const void *volName, short vRefNum) { (void)volName; (void)vRefNum; return noErr; }
OSErr PBHSetVolSync(void *pb) { (void)pb; return noErr; }

/* The core uses PBRead/PBWrite only for the .Sony Mac floppy (ioRefNum -5),
   which has no PS2 equivalent - fail it so fat_dev_sony() reports "no floppy"
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
 * Sound Manager (M3 stub - links + runs; audio is silent on host)
 * ======================================================================== */
static SndChannel gChans[8];
static int gChanUsed[8];

OSErr SndNewChannel(SndChannelPtr *chan, short synth, long init, void *proc)
{
    int i; (void)synth; (void)init; (void)proc;
    for (i = 0; i < 8; i++) if (!gChanUsed[i]) { gChanUsed[i] = 1; gChans[i].id = i; *chan = &gChans[i]; return noErr; }
    *chan = NULL; return -204;
}
OSErr SndDisposeChannel(SndChannelPtr chan, Boolean quiet)
{
    (void)quiet;
    if (chan && chan->id >= 0 && chan->id < 8) gChanUsed[chan->id] = 0;
    return noErr;
}
OSErr SndDoImmediate(SndChannelPtr chan, const SndCommand *cmd)
{
    (void)chan; (void)cmd;                   /* host: silent. EE M3: audsrv. */
    return noErr;
}
