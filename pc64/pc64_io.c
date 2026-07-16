/* ===========================================================================
 * UnoDOS/pc64 - File Manager (M2) + Sound Manager (M3) shim.
 *
 * Storage backend: a RAM disk. The port boots from a UEFI ESP but has no
 * native block driver yet (NVMe/AHCI are the documented driver tail), so the
 * File Manager surface the core uses - FSOpen/FSRead/FSWrite/Create/FSDelete
 * + PBGetCatInfo enumeration - runs over an in-memory file table, seeded with
 * a README. Files/Notepad save + reload round-trip within a session; media
 * persistence arrives with the block driver.
 *
 * The handle model is the Dreamcast VMU backend's flush-on-close shape
 * (dreamcast/mac_io.c): a read handle snapshots the file at FSOpen, a write
 * handle accumulates FSWrite bytes and commits the whole buffer at FSClose -
 * exactly UnoDOS's whole-file save/load pattern.
 *
 * Sound Manager: square-wave channels on the PC SPEAKER - PIT channel 2 via
 * port I/O from long mode (uefi_main.c provides uno_pc64_snd_*). The one
 * square-wave voice mirrors the x86 reference kernel's API 41/42 audio;
 * Music/Tracker/Dostris link and run, voicing the leftmost channel.
 * ===========================================================================
 */
#include "mac_compat.h"
#include <stdlib.h>
#include <string.h>

/* ---- Pascal <-> C name helpers ------------------------------------------ */
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

/* =========================================================================
 * RAM-disk store: a flat volume of named files
 * ======================================================================== */
#define MAXFILES  24
#define NAME_MAX  32
#define FILE_MAX  (256 * 1024)      /* per-file ceiling (Paint images fit) */

typedef struct {
    int           used;
    char          name[NAME_MAX];
    unsigned char *buf;
    long          len;
} RamFile;
static RamFile gDisk[MAXFILES];
static int gSeeded = 0;

static void seed_disk(void)
{
    static const char kReadme[] =
        "Welcome to UnoDOS on a modern PC.\r"
        "\r"
        "This desktop is the same portable core the\r"
        "Mac, PS2 and Dreamcast ports run - here it\r"
        "boots bare-metal over UEFI: the firmware\r"
        "hands the kernel a GOP framebuffer and\r"
        "keyboard, and UnoDOS does the rest.\r"
        "\r"
        "This file lives on the RAM disk. Save from\r"
        "Notepad and it shows up in Files.\r";
    if (gSeeded) return;
    gSeeded = 1;
    gDisk[0].used = 1;
    strcpy(gDisk[0].name, "README.TXT");
    gDisk[0].len = (long)(sizeof kReadme - 1);
    gDisk[0].buf = (unsigned char *)malloc(sizeof kReadme - 1);
    if (gDisk[0].buf) memcpy(gDisk[0].buf, kReadme, sizeof kReadme - 1);
    else gDisk[0].used = 0;
}

static RamFile *disk_find(const char *name)
{
    int i;
    seed_disk();
    for (i = 0; i < MAXFILES; i++)
        if (gDisk[i].used && strcmp(gDisk[i].name, name) == 0) return &gDisk[i];
    return 0;
}

static RamFile *disk_slot(void)
{
    int i;
    seed_disk();
    for (i = 0; i < MAXFILES; i++) if (!gDisk[i].used) return &gDisk[i];
    return 0;
}

/* =========================================================================
 * File Manager handles - flush-on-close RAM buffers (the VMU backend shape)
 * ======================================================================== */
#define MAXF 8

typedef struct {
    int           used;
    int           writing;          /* 1 = buffer commits to the store on close */
    char          name[NAME_MAX];
    unsigned char *buf;
    long          len;              /* valid bytes in buf */
    long          pos;              /* read cursor */
    long          cap;              /* allocation size */
} FHandle;
static FHandle gH[MAXF];

static int alloc_ref(void)
{ int i; for (i = 0; i < MAXF; i++) if (!gH[i].used) return i; return -1; }

OSErr FSOpen(const void *fileName, short vRefNum, short *refNum)
{
    char name[64]; int r; RamFile *f;
    (void)vRefNum;
    p2c(fileName, name, sizeof name);

    r = alloc_ref(); if (r < 0) return -42;      /* tmfoErr */

    f = disk_find(name);
    if (f) {                                     /* read path: snapshot */
        gH[r].buf = (unsigned char *)malloc(f->len > 0 ? (size_t)f->len : 1);
        if (!gH[r].buf) return -108;             /* memFullErr */
        memcpy(gH[r].buf, f->buf, (size_t)f->len);
        gH[r].len = f->len;
        gH[r].writing = 0;
        gH[r].cap = f->len;
    } else {                                     /* write path: fresh buffer */
        gH[r].cap = 4096;
        gH[r].buf = (unsigned char *)malloc((size_t)gH[r].cap);
        if (!gH[r].buf) return -108;
        gH[r].len = 0;
        gH[r].writing = 1;
    }
    strncpy(gH[r].name, name, NAME_MAX - 1);
    gH[r].name[NAME_MAX - 1] = 0;
    gH[r].pos = 0; gH[r].used = 1;
    *refNum = (short)r;
    return noErr;
}

OSErr FSClose(short refNum)
{
    FHandle *h;
    if (refNum < 0 || refNum >= MAXF || !gH[refNum].used) return rfNumErr;
    h = &gH[refNum];
    if (h->writing && h->len > 0) {              /* one whole-file commit */
        RamFile *f = disk_find(h->name);
        if (!f) {
            f = disk_slot();
            if (f) { f->used = 1; strcpy(f->name, h->name); f->buf = 0; f->len = 0; }
        }
        if (f) {
            unsigned char *nb = (unsigned char *)malloc((size_t)h->len);
            if (nb) {
                memcpy(nb, h->buf, (size_t)h->len);
                free(f->buf);
                f->buf = nb; f->len = h->len;
            }
        }
    }
    free(h->buf);
    memset(h, 0, sizeof *h);
    return noErr;
}

OSErr FSRead(short refNum, long *count, void *buf)
{
    FHandle *h; long n;
    if (refNum < 0 || refNum >= MAXF || !gH[refNum].used) return rfNumErr;
    h = &gH[refNum];
    n = *count;
    if (n > h->len - h->pos) n = h->len - h->pos;
    if (n < 0) n = 0;
    memcpy(buf, h->buf + h->pos, (size_t)n);
    h->pos += n; *count = n;
    return n > 0 ? noErr : eofErr;
}

OSErr FSWrite(short refNum, long *count, const void *buf)
{
    FHandle *h; long need;
    if (refNum < 0 || refNum >= MAXF || !gH[refNum].used) return rfNumErr;
    h = &gH[refNum];
    need = h->len + *count;
    if (need > FILE_MAX) { *count = 0; return dskFulErr; }
    if (need > h->cap) {
        long nc = h->cap ? h->cap : 4096;
        unsigned char *nb;
        while (nc < need) nc *= 2;
        nb = (unsigned char *)realloc(h->buf, (size_t)nc);
        if (!nb) { *count = 0; return dskFulErr; }
        h->buf = nb; h->cap = nc;
    }
    memcpy(h->buf + h->len, buf, (size_t)*count);
    h->len += *count;
    h->writing = 1;
    return noErr;
}

OSErr Create(const void *fileName, short vRefNum, OSType creator, OSType type)
{
    char name[64];
    (void)vRefNum; (void)creator; (void)type;
    p2c(fileName, name, sizeof name);
    if (disk_find(name)) return dupFNErr;
    /* bytes land at FSClose of the write handle; Create just reports "ok" */
    return noErr;
}

OSErr FSDelete(const void *fileName, short vRefNum)
{
    char name[64]; RamFile *f;
    (void)vRefNum;
    p2c(fileName, name, sizeof name);
    f = disk_find(name);
    if (!f) return fnfErr;
    free(f->buf);
    memset(f, 0, sizeof *f);
    return noErr;
}

/* PBGetCatInfo: enumerate the RAM disk by 1-based ioFDirIndex. */
OSErr PBGetCatInfoSync(CInfoPBPtr pb)
{
    int want = pb->dirInfo.ioFDirIndex, i, seen = 0;
    RamFile *hit = 0;
    if (want <= 0) return paramErr;
    seed_disk();
    for (i = 0; i < MAXFILES; i++) {
        if (!gDisk[i].used) continue;
        if (++seen == want) { hit = &gDisk[i]; break; }
    }
    if (!hit) return fnfErr;
    if (pb->dirInfo.ioNamePtr) c2p(hit->name, pb->dirInfo.ioNamePtr, 32);
    pb->dirInfo.ioFlAttrib = 0;                  /* flat volume: no subdirs */
    pb->dirInfo.ioFlLgLen  = hit->len;
    pb->dirInfo.ioDirID = pb->dirInfo.ioDrDirID = (long)(2 + want);
    pb->dirInfo.ioDrParID = fsRtDirID;
    pb->dirInfo.ioResult = noErr;
    return noErr;
}

/* ---- shared: FlushVol + raw block I/O ----------------------------------- */
OSErr FlushVol(const void *volName, short vRefNum) { (void)volName; (void)vRefNum; return noErr; }
OSErr PBHSetVolSync(void *pb) { (void)pb; return noErr; }

/* The core uses PBRead/PBWrite only for the .Sony Mac floppy (ioRefNum -5),
   which has no pc64 equivalent yet - fail it so fat_dev_sony() reports "no
   floppy", the same shape as the Dreamcast port. The real block path here is
   the NVMe/AHCI driver tail. */
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
 * Sound Manager (M3): square-wave channels on the PC speaker.
 *
 * One physical voice (PIT channel 2 gates the speaker), the same constraint
 * as the x86 reference kernel: the most recent noteCmd wins, quietCmd stops
 * it. Music / Tracker / Dostris drive this path unchanged.
 * ======================================================================== */
static SndChannel gChans[8];
static int gChanUsed[8];
static int gSpkOwner = -1;          /* which channel currently gates the speaker */

void uno_pc64_snd_note(int midi);   /* uefi_main.c: PIT ch2 + speaker gate on */
void uno_pc64_snd_quiet(void);      /* uefi_main.c: speaker gate off */

OSErr SndNewChannel(SndChannelPtr *chan, short synth, long init, void *proc)
{
    int i; (void)synth; (void)init; (void)proc;
    for (i = 0; i < 8; i++)
        if (!gChanUsed[i]) { gChanUsed[i] = 1; gChans[i].id = i; *chan = &gChans[i]; return noErr; }
    *chan = NULL; return -204;
}
OSErr SndDisposeChannel(SndChannelPtr chan, Boolean quiet)
{
    (void)quiet;
    if (chan && chan->id >= 0 && chan->id < 8) {
        if (gSpkOwner == chan->id) { uno_pc64_snd_quiet(); gSpkOwner = -1; }
        gChanUsed[chan->id] = 0;
    }
    return noErr;
}
OSErr SndDoImmediate(SndChannelPtr chan, const SndCommand *cmd)
{
    if (chan && cmd) {
        if (cmd->cmd == noteCmd || cmd->cmd == freqCmd) {
            uno_pc64_snd_note((int)cmd->param2);        /* param2 = MIDI note */
            gSpkOwner = chan->id;
        } else if (cmd->cmd == quietCmd || cmd->cmd == flushCmd) {
            if (gSpkOwner == chan->id || gSpkOwner < 0) {
                uno_pc64_snd_quiet();
                gSpkOwner = -1;
            }
        }
    }
    return noErr;
}
